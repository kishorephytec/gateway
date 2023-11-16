/* Wirepas Oy licensed under Apache License, Version 2.0
 *
 * See file LICENSE for full license details.
 *
 */
#define LOG_MODULE_NAME "reassembly"
#define MAX_LOG_LEVEL INFO_LOG_LEVEL
#define PRINT_BUFFERS
#include "logger.h"

#include "reassembly.h"
#include "uthash.h"
#include "utlist.h"

#include "platform.h"

/* undefine the defaults */
#undef uthash_malloc
#undef uthash_free

/* re-define, specifying alternate functions */
#define uthash_malloc(sz) Platform_malloc(sz)
#define uthash_free(ptr, sz) Platform_free(ptr, sz)

// Packed struct as used as a key
typedef struct __attribute__((__packed__))
{
    uint32_t src_add;
    uint16_t packet_id;
} packet_key_t;

typedef struct internal_fragment_t
{
    uint8_t * bytes;
    size_t size;
    size_t offset;
    struct internal_fragment_t * next;
} internal_fragment_t;

typedef struct
{
    // Key used for the hashing
    packet_key_t key;
    // Only known when last fragment is received
    size_t full_size;
    // Set when all fragment are received
    bool is_full;
    // List of already received frag
    internal_fragment_t *head;
    // Timestamp of first fragment received
    unsigned long long timestamp_ms_epoch_first;
    // Timestamp of last rx fragment received
    unsigned long long timestamp_ms_epoch_last;
    UT_hash_handle hh;
} full_packet_t;

/**
 * Hash containing all the fragmented packet under construction
 */
static full_packet_t * m_packets = NULL;

static full_packet_t * get_packet_from_hash(uint32_t src_add, uint16_t packet_id)
{
    full_packet_t * p;
    packet_key_t key;
    key.src_add = src_add;
    key.packet_id = packet_id;

    HASH_FIND(hh, m_packets, &key, sizeof(packet_key_t), p);  /* id already in the hash? */

    return p;
}

static full_packet_t * create_packet_in_hash(uint32_t src_add, uint16_t packet_id)
{
    full_packet_t * p;
    p = (full_packet_t *) Platform_malloc(sizeof(full_packet_t));
    if (p == NULL)
    {
        return NULL;
    }

    // Fill the info
    *p = (full_packet_t) {
        .key.src_add = src_add,
        .key.packet_id = packet_id,
    };

    // Add it to hash
    HASH_ADD(hh, m_packets, key, sizeof(packet_key_t), p);

    return p;
}

static bool add_fragment_to_full_packet(full_packet_t * full_packet_p, reassembly_fragment_t * frag_p)
{
    internal_fragment_t * f;
    uint8_t * data;

    // Check if it is not a duplicate
    // iteration is O(n) and should be fast as usually limited to
    // max 15 fragments. It could be optimized with checking only at the end

    LL_SEARCH_SCALAR(full_packet_p->head, f, offset, frag_p->offset);
    if (f != NULL)
    {
        LOGE("Already a fragment at this offset! Duplicated packet received?\n");
        return false;
    }

    // Alocate memory for the rx fragment header
    f = Platform_malloc(sizeof(internal_fragment_t));
    data = Platform_malloc(frag_p->size);

    if (f == NULL || data == NULL)
    {
        // Fine to call free on NULL
        Platform_free(f, sizeof(internal_fragment_t));
        Platform_free(data, frag_p->size);
        LOGE("Cannot alocate memory for the fragment\n");
        return false;
    }

    memcpy(data, frag_p->bytes, frag_p->size);
    *f = (internal_fragment_t) {
        .bytes = data,
        .size = frag_p->size,
        .offset = frag_p->offset
    };

    LL_PREPEND(full_packet_p->head, f);

    // Update full size if we have the info
    if (frag_p->last_fragment)
    {
        full_packet_p->full_size = frag_p->offset + frag_p->size;
        LOGD("Full size known = %u\n", full_packet_p->full_size);
    }

    // Update timestamp of latest fragment
    full_packet_p->timestamp_ms_epoch_last = frag_p->timestamp;

    return true;
}

static bool is_packet_full(full_packet_t * full_packet_p)
{
    internal_fragment_t * f;
    size_t rx_size = 0;

    if (full_packet_p->is_full)
    {
        // packet was already considered full
        return true;
    }

    if (full_packet_p->full_size == 0)
    {
        // Last fragment not received so cannot be full
        return false;
    }

    // Make the sum of all received fragment.
    // Duplicate were already detected when adding fragments
    LL_FOREACH(full_packet_p->head, f) {
        rx_size += f->size;
    }

    if (rx_size == full_packet_p->full_size)
    {
        // Cached the full status
        full_packet_p->is_full = true;
        LOGD("Packet is full\n");
        return true;
    }
    else
    {
        LOGW("Packet not full yet but last fragment received. Out of order?\n");
        return false;
    }
}

static bool reassemble_full_packet(full_packet_t * full_packet_p, uint8_t * buffer_p, size_t * size)
{
    internal_fragment_t *f;
    internal_fragment_t *tmp;
    if (!full_packet_p->is_full)
    {
        // No need to call is_packet_full as is_full will not change without
        // adding a new fragment
        *size = 0;
        return false;
    }

    if (*size < full_packet_p->full_size)
    {
        // Buffer is too small
        return false;
    }

    // Packet is full, copy each frag in final buffer and release
    // alocated fragment
    LL_FOREACH_SAFE(full_packet_p->head, f, tmp) {
        memcpy(buffer_p + f->offset, f->bytes, f->size);
        Platform_free(f->bytes, f->size);
        LL_DELETE(full_packet_p->head, f);
        Platform_free(f, sizeof(internal_fragment_t));
    }

    // update written size
    *size = full_packet_p->full_size;

    // release also full packet struct form hash
    HASH_DEL(m_packets, full_packet_p);
    Platform_free(full_packet_p, sizeof(full_packet_t));
    return true;
}

void reassembly_init()
{
    // Nothing to do at the moment
}

bool reassembly_add_fragment(reassembly_fragment_t * frag, size_t * full_size_p)
{
    full_packet_t *full_packet_p;

    *full_size_p = 0;

    // Get packet or create it
    full_packet_p = get_packet_from_hash(frag->src_add, frag->packet_id);
    if (full_packet_p == NULL)
    {
        full_packet_p = create_packet_in_hash(frag->src_add, frag->packet_id);
        if (full_packet_p == NULL)
        {
            LOGE("Cannot allocate packet from hash\n");
            return false;
        }

        // Set timestamp for first fragment
        full_packet_p->timestamp_ms_epoch_first = frag->timestamp;
    }

    // Now we have our full packet holder in full_packet_p
    if (!add_fragment_to_full_packet(full_packet_p, frag))
    {
        LOGE("Cannot add fragment to full packet\n");
        //TODO Remove the full packet as it will never be full
        return false;
    }

    // Check if packet is full as we just added a fragment
    if (is_packet_full(full_packet_p))
    {
        *full_size_p = full_packet_p->full_size;
    }

    return true;
}

bool reassembly_get_full_message(uint32_t src_add, uint16_t packet_id, uint8_t * buffer_p, size_t * size)
{
    full_packet_t *full_packet_p;

    full_packet_p = get_packet_from_hash(src_add, packet_id);
    if (full_packet_p == NULL)
    {
        LOGE("Cannot find the packet from %u with id %u\n", src_add, packet_id);
        return false;
    }

    return reassemble_full_packet(full_packet_p, buffer_p, size);

}

void reassembly_garbage_collect(uint32_t timeout_s)
{
    full_packet_t *fp, *tmp;
    uint32_t messages_removed = 0;
    HASH_ITER(hh, m_packets, fp, tmp) {
        uint32_t last_activity =
            (Platform_get_timestamp_ms_epoch() - fp->timestamp_ms_epoch_last) / 1000;

        /* Check if message is not getting too old */
        if (last_activity > timeout_s)
        {
            LOGW("Fragmented message from src %u with id %u has no activity for more than %u s => delete it\n",
                fp->key.src_add, fp->key.packet_id, timeout_s);

            internal_fragment_t *f, *tmp;
            LL_FOREACH_SAFE(fp->head, f, tmp) {
                Platform_free(f->bytes, f->size);
                LL_DELETE(fp->head, f);
                Platform_free(f, sizeof(internal_fragment_t));
            }

            // release also full packet struct from hash
            HASH_DEL(m_packets, fp);
            Platform_free(fp, sizeof(full_packet_t));
            messages_removed ++;
        }
    }
    LOGD("GC: %d message removed\n", messages_removed);
}
