/* Wirepas Oy licensed under Apache License, Version 2.0
 *
 * See file LICENSE for full license details.
 *
 */
#define LOG_MODULE_NAME "dsap"
#define MAX_LOG_LEVEL WARNING_LOG_LEVEL 
/*#define MAX_LOG_LEVEL INFO_LOG_LEVEL */
#include "logger.h"
#include "wpc_types.h"
#include "wpc_internal.h"
#include "util.h"
#include "platform.h"
#include "reassembly.h"

#include "string.h"

#ifdef REGISTER_DATA_PER_ENDPOINT
// Table to store the registered client callbacks for Rx data
static onDataReceived_cb_f data_cb_table[MAX_NUMBER_EP];
#else
static onDataReceived_cb_f m_data_cb;
#endif

typedef struct
{
    onDataSent_cb_f cb;
    uint16_t pdu_id;
    bool busy;
} packet_with_indication_t;

// Minimum period between two consecutive garbage collects of
// uncomplete fragments
// GC is anyway synchronous with received fragment so period between 2 GC
// could be much bigger if not fragments are received for a while
#define MIN_GARBAGE_COLLECT_PERIOD_S   5

// Max timeout in seconds for uncomplete fragmented packet to be discarded
// from rx queue.
static uint32_t m_fragment_max_duration_s = 0;

// Static buffer used to reassemble messages. Allocated statically
// to not have it allocated on stack dynamically. Could also be allocated
// dynamically with platform malloc, but as there is only one needed, static
// allocation is probably best option.
static uint8_t reassembly_buffer[1500];

// Table to store the data sent callbacks status for Tx data
static packet_with_indication_t indication_sent_cb_table[MAX_SENT_PACKET_WITH_INDICATION];

static bool set_indication_cb(onDataSent_cb_f cb, uint16_t pdu_id)
{
    int i;

    for (i = 0; i < MAX_SENT_PACKET_WITH_INDICATION; i++)
    {
        if (!indication_sent_cb_table[i].busy)
        {
            indication_sent_cb_table[i].busy = true;
            indication_sent_cb_table[i].cb = cb;
            indication_sent_cb_table[i].pdu_id = pdu_id;
            break;
        }
    }

    return i < MAX_SENT_PACKET_WITH_INDICATION;
}

static onDataSent_cb_f get_indication_cb(uint16_t pdu_id)
{
    onDataSent_cb_f cb = NULL;

    for (int i = 0; i < MAX_SENT_PACKET_WITH_INDICATION; i++)
    {
        if (indication_sent_cb_table[i].busy && indication_sent_cb_table[i].pdu_id == pdu_id)
        {
            // Release entry
            indication_sent_cb_table[i].busy = false;
            cb = indication_sent_cb_table[i].cb;
        }
    }

    return cb;
}

static void fill_tx_tt_request(wpc_frame_t * request,
                               const uint8_t * buffer,
                               size_t len,
                               uint16_t pdu_id,
                               uint32_t dest_add,
                               uint8_t qos,
                               uint8_t src_ep,
                               uint8_t dest_ep,
                               uint8_t tx_options,
                               uint32_t buffering_delay)
{
    dsap_data_tx_tt_req_pl_t * payload = &request->payload.dsap_data_tx_tt_request_payload;

    payload->pdu_id = pdu_id;
    payload->src_endpoint = src_ep;
    payload->dest_add = dest_add;
    payload->dest_endpoint = dest_ep;
    payload->qos = qos;
    payload->tx_options = tx_options;
    payload->apdu_length = len;
    payload->buffering_delay = buffering_delay;

    // copy the APDU
    memcpy(&payload->apdu, buffer, len);
}

/*static void fill_tx_frag_request(wpc_frame_t * request,
                               const uint8_t * buffer,
                               size_t len,
                               uint16_t pdu_id,
                               uint32_t dest_add,
                               uint8_t qos,
                               uint8_t src_ep,
                               uint8_t dest_ep,
                               uint8_t tx_options,
                               uint32_t buffering_delay,
			       uint16_t packet_id,
			       uint16_t frag_offset,
			       bool last_frag)
{
    dsap_data_tx_tt_req_pl_t * payload = &request->payload.dsap_data_tx_tt_request_payload;

    payload->pdu_id = pdu_id;
    payload->src_endpoint = src_ep;
    payload->dest_add = dest_add;
    payload->dest_endpoint = dest_ep;
    payload->qos = qos;
    payload->tx_options = tx_options;
    payload->apdu_length = len + FRAGMENT_HEADER_SIZE;
    payload->buffering_delay = buffering_delay;
*/
    /****************************************/
    /*  packet id    |   MF|Frag offset     */
    /****************************************/
    /* MSB first and LSB second */
/*    payload->apdu[1] = packet_id & 0xFF;
    payload->apdu[0] = (packet_id >> 8) & 0xFF;
   //  Frag_offset calculation in multiples of 8 
    payload->apdu[3] = (frag_offset/8) & 0xFF;
    payload->apdu[2] = ((frag_offset/8) >> 8) & 0xFF;
 
    if (last_frag)
	payload->apdu[2] |= 1 << 14;
*/
/*    payload->apdu[0] = 0;
    payload->apdu[1] = 0;
    payload->apdu[2] = 0;
    payload->apdu[3] = 0;
    

    // copy the APDU
    memcpy(&payload->apdu+FRAGMENT_HEADER_SIZE, buffer, len);
}
*/

/*static void fill_tx_frag_request(wpc_frame_t * request,
                                 const uint8_t * buffer,
                                 size_t len,
                                 uint16_t pdu_id,
                                 uint32_t dest_add,
                                 uint8_t qos,
                                 uint8_t src_ep,
                                 uint8_t dest_ep,
                                 uint8_t tx_options,
                                 uint32_t buffering_delay,
                                 uint16_t full_packet_id,
                                 uint16_t frag_offset,
                                 bool last_frag)
{
    dsap_data_tx_frag_req_pl_t * payload = &request->payload.dsap_data_tx_frag_request_payload;

    uint16_t frag_flag = 0;
    if (last_frag)
    {
        frag_flag = DSAP_FRAG_LAST_FLAG_MASK;
    }
    else
    {
        // Never track intermediate fragment so clear first bit
        tx_options &= 0xFE;
    }

    *payload = (dsap_data_tx_frag_req_pl_t) {
        .pdu_id = pdu_id,
        .src_endpoint = src_ep,
        .dest_add = dest_add,
        .dest_endpoint = dest_ep,
        .qos = qos,
        .tx_options = tx_options,
        .buffering_delay = buffering_delay,
        .full_packet_id = full_packet_id,
        .apdu_length  = len,
        .fragment_offset_flag = (frag_offset & DSAP_FRAG_LENGTH_MASK) | frag_flag,
    };

    // copy the APDU
    memcpy(&payload->apdu, buffer, len);
}
*/

/*
int dsap_data_tx_request(const uint8_t * buffer,
                         size_t len,
                         uint16_t pdu_id,
                         uint32_t dest_add,
                         uint8_t qos,
                         uint8_t src_ep,
                         uint8_t dest_ep,
                         onDataSent_cb_f on_data_sent_cb,
                         uint32_t buffering_delay,
                         bool is_unack_csma_ca,
                         uint8_t hop_limit)
{
    wpc_frame_t request, confirm;
    int res;
    uint8_t confirm_res;
    uint8_t tx_options = 0;
    size_t fragments = 0;
    size_t last_fragment_size = 0;
    // Packet id used for fragmented packet
    static uint16_t packet_id = 0;

    if (len > MAX_FULL_PACKET_SIZE)
    {
        // Not very clean, but reuse dualmcu 6 error code for sending data
        // to generate a INVALID_PARAM at gateway level instead of INTERNAL_ERROR
        return 6;
    }

    if (len > MAX_DATA_PDU_SIZE)
    {
        // Packet must be fragmented
        fragments = (len + MAX_DATA_PDU_SIZE - 1)  / MAX_DATA_PDU_SIZE;
        last_fragment_size = len % MAX_DATA_PDU_SIZE;
        if (last_fragment_size == 0)
        {
            last_fragment_size = MAX_DATA_PDU_SIZE;
        }
        LOGI("Packet of size %d must be splitted in %d fragments (last is %d bytes)\n", len, fragments, last_fragment_size);
    }

    // Fill the tx options
    if (on_data_sent_cb != NULL)
    {
        tx_options |= 0x1;
    }

    // Is it a unack_csma_ca transmission
    if (is_unack_csma_ca)
    {
        tx_options |= 0x2;
    }

    // Add hop limit (on 4 bits)
    tx_options |= (hop_limit & 0xf) << 2;

    // Create the frame
    if (fragments == 0)
    {
        // Full packet in a single frame
        // Even with buffering_delay of 0, TX_TT_REQUEST can be used
        request.primitive_id = DSAP_DATA_TX_TT_REQUEST;
        fill_tx_tt_request(&request, buffer, len, pdu_id, dest_add, qos, src_ep, dest_ep, tx_options, buffering_delay);
        request.payload_length =
            sizeof(dsap_data_tx_tt_req_pl_t) - (MAX_DATA_PDU_SIZE - len);
	LOGI("strucrure length : %d payload length : %d\n", sizeof(dsap_data_tx_tt_req_pl_t), request.payload_length);

        // Do the sending
        res = WPC_Int_send_request(&request, &confirm);
    }
    else
    {
        // id is on 12 bits
        uint16_t p_id = packet_id++ & 0xfff;
        // Packet must be fragmented
        request.primitive_id = DSAP_DATA_TX_FRAG_REQUEST;
        // Send all fragment except last
        for (size_t i = 0; i < fragments; i++)
        {
            uint16_t offset = i * MAX_DATA_PDU_SIZE;
            bool last = false;
            size_t frag_len = MAX_DATA_PDU_SIZE;

            if (i == (fragments -1))
            {
                last = true;
                frag_len= last_fragment_size;
            }
            LOGI("Sending frag %d/%d for id = %d\n", i, fragments, p_id);

            fill_tx_frag_request(&request,
                                 buffer + offset,
                                 frag_len,
                                 pdu_id,
                                 dest_add,
                                 qos,
                                 src_ep,
                                 dest_ep,
                                 tx_options,
                                 buffering_delay,
                                 p_id,
                                 offset,
                                 last);

            request.payload_length =
            sizeof(dsap_data_tx_frag_req_pl_t) - (MAX_DATA_PDU_SIZE - frag_len);

            // Do the sending
            res = WPC_Int_send_request(&request, &confirm);
            if (res < 0)
            {
                // No way to recall previous fragment, they will be sent
                LOGE("Cannot send frag %d/%d for dst=%d id=%d size=%d\n", i, fragments, dest_add, p_id, frag_len);
                return res;
            }

            // Check stack return code
            confirm_res = confirm.payload.dsap_data_tx_confirm_payload.result;

            if (confirm_res != 0)
            {
                // No way to recall previous fragment, they will be sent
                LOGE("Stack refused (res=%d) intermediate frag %d/%d for dst=%d id=%d size=%d\n", confirm_res, i, fragments, dest_add, p_id, frag_len);
                return res;
            }
        }
    }

    if (res < 0)
        return res;

    confirm_res = confirm.payload.dsap_data_tx_confirm_payload.result;

    // If success, register the callback
    if (confirm_res == 0 && on_data_sent_cb != NULL)
    {
        set_indication_cb(on_data_sent_cb, pdu_id);
    }

    LOGI("Send data result = 0x%02x capacity = %d \n",
         confirm_res,
         confirm.payload.dsap_data_tx_confirm_payload.capacity);

    return confirm_res;
}

*/


int dsap_data_tx_request(const uint8_t * buffer,
                         size_t len,
                         uint16_t pdu_id,
                         uint32_t dest_add,
                         uint8_t qos,
                         uint8_t src_ep,
                         uint8_t dest_ep,
                         onDataSent_cb_f on_data_sent_cb,
                         uint32_t buffering_delay,
                         bool is_unack_csma_ca,
                         uint8_t hop_limit)
{
    wpc_frame_t request, confirm;
    int res;
    uint8_t confirm_res;
    uint8_t tx_options = 0;
    size_t wirepas_fragments = 0;
    size_t last_fragment_size = 0;
    // Packet id used for packet identification
    static uint8_t packet_id = 0;
    uint8_t wirepas_payload[MAX_DATA_PDU_SIZE] = {0};
  //  uint8_t wirepas_payload_len = 0;

    if (len > MAX_FULL_PACKET_SIZE)
    {
        // Not very clean, but reuse dualmcu 6 error code for sending data
        // to generate a INVALID_PARAM at gateway level instead of INTERNAL_ERROR
        return 6;
    }

    if (len + FRAGMENT_HEADER_SIZE > MAX_DATA_PDU_SIZE)
    {
        // Packet must be fragmented
        wirepas_fragments = (len + FRAGMENT_MAX_SIZE - 1)  / FRAGMENT_MAX_SIZE;
	uint16_t frag_header_bytes = wirepas_fragments * FRAGMENT_HEADER_SIZE;
        last_fragment_size = (len + frag_header_bytes) % MAX_DATA_PDU_SIZE;
        if (last_fragment_size == 0)
        {
            last_fragment_size = MAX_DATA_PDU_SIZE;
        }
        LOGI("Packet of size.... %d must be splitted in %d fragments (last is %d bytes)\n", len, wirepas_fragments, last_fragment_size);
    }

    // Fill the tx options
    if (on_data_sent_cb != NULL)
    {
        tx_options |= 0x1;
    }

    // Is it a unack_csma_ca transmission
    if (is_unack_csma_ca)
    {
        tx_options |= 0x2;
    }

    // Add hop limit (on 4 bits)
    tx_options |= (hop_limit & 0xf) << 2;

    // Rolling packet_id to zero 
    if(packet_id >= 0XFE)
        packet_id = 1;
    else
        packet_id++;

    // Full packet in a single frame
    if (wirepas_fragments == 0)
    {
        /*-------------------------------------------------------
        | fragment header = packet_id(1byte) + fragment no(1byte)|  
        | fragmnet no = 00 ---> NON fragmented packet            |
        |               01 ---> First fragment                   |
        |               02 ---> Second fragment                  |
        |                     |                                  |
        |                     |                                  |
        |               FF ---> End fragment                     |
        --------------------------------------------------------*/

        // Even with buffering_delay of 0, TX_TT_REQUEST can be used
	wirepas_payload[0] = packet_id;
	wirepas_payload[1] = 0x00;
	memcpy(wirepas_payload + FRAGMENT_HEADER_SIZE, buffer, len);

        request.primitive_id = DSAP_DATA_TX_TT_REQUEST;
        fill_tx_tt_request(&request, wirepas_payload, len + FRAGMENT_HEADER_SIZE, 
			pdu_id, dest_add, qos, src_ep, dest_ep, tx_options, buffering_delay);
        request.payload_length =
            sizeof(dsap_data_tx_tt_req_pl_t) - (MAX_DATA_PDU_SIZE - (len + FRAGMENT_HEADER_SIZE));
	LOGI("strucrure length : %d payload length : %d\n", sizeof(dsap_data_tx_tt_req_pl_t), request.payload_length);	

        // Do the sending
        res = WPC_Int_send_request(&request, &confirm);
    }

    // Packet must be fragmented
    else
    {
	request.primitive_id = DSAP_DATA_TX_TT_REQUEST;
        // Send all fragment except last
        for (size_t i = 0; i < wirepas_fragments; i++)
        {
            uint16_t offset = i * FRAGMENT_MAX_SIZE;
           // bool last = false;
            size_t frag_len = FRAGMENT_MAX_SIZE;

	    wirepas_payload[0] = packet_id;
            wirepas_payload[1] = i+1;
            // last fragment
            if (i == (wirepas_fragments -1))
            {
             //   last = true;
                frag_len= last_fragment_size - FRAGMENT_HEADER_SIZE ;
		wirepas_payload[1] = 0xFF;
            }
            LOGI("Sending frag %d/%d for id = %d\n", i, wirepas_fragments, packet_id);
	    memcpy(wirepas_payload+FRAGMENT_HEADER_SIZE, buffer+offset, frag_len);
	 
	    // Fill request 	
	    fill_tx_tt_request(&request, wirepas_payload, frag_len + FRAGMENT_HEADER_SIZE,
                        pdu_id, dest_add, qos, src_ep, dest_ep, tx_options, buffering_delay);	
            request.payload_length =
            sizeof(dsap_data_tx_tt_req_pl_t) - (MAX_DATA_PDU_SIZE - (frag_len + FRAGMENT_HEADER_SIZE));

	    LOGI("structure length : %d payload length : %d frag len : %d\n", sizeof(dsap_data_tx_tt_req_pl_t), request.payload_length, frag_len);

            // Do the sending
            res = WPC_Int_send_request(&request, &confirm);
            if (res < 0)
            {
                // No way to recall previous fragment, they will be sent
                LOGE("Cannot send frag %d/%d for dst=%d id=%d size=%d\n", i, wirepas_fragments, dest_add, packet_id, frag_len);
                return res;
            }
            memset(wirepas_payload, 0, MAX_DATA_PDU_SIZE);

            // Check stack return code
            confirm_res = confirm.payload.dsap_data_tx_confirm_payload.result;

            if (confirm_res != 0)
            {
                // No way to recall previous fragment, they will be sent
                LOGE("Stack refused (res=%d) intermediate frag %d/%d for dst=%d id=%d size=%d\n", 
					         confirm_res, i, wirepas_fragments, dest_add, packet_id, frag_len);
                return res;
            }
        }
    }
    memset(wirepas_payload, 0, MAX_DATA_PDU_SIZE);

    if (res < 0)
        return res;

    confirm_res = confirm.payload.dsap_data_tx_confirm_payload.result;

    // If success, register the callback
    if (confirm_res == 0 && on_data_sent_cb != NULL)
    {
        set_indication_cb(on_data_sent_cb, pdu_id);
    }

    LOGI("Send data result = 0x%02x capacity = %d \n",
         confirm_res,
         confirm.payload.dsap_data_tx_confirm_payload.capacity);

    return confirm_res;
}

void dsap_data_tx_indication_handler(dsap_data_tx_ind_pl_t * payload)
{
    onDataSent_cb_f cb = get_indication_cb(payload->pdu_id);

    LOGD("Tx indication received: indication_status = %d, buffering_delay = "
         "%d\n",
         payload->indication_status,
         payload->buffering_delay);
    if (cb != NULL)
    {
        LOGD("App cb set, call it...\n");
        cb(payload->pdu_id,
           internal_time_to_ms(payload->buffering_delay),
           payload->result);
    }
}

void dsap_data_rx_frag_indication_handler(dsap_data_rx_frag_ind_pl_t * payload,
                                          unsigned long long timestamp_ms_epoch)
{
    static unsigned long long last_gc_ts_ms = 0;
    reassembly_fragment_t frag;
    size_t full_size;
    app_qos_e qos;
    uint8_t hop_count;
    uint32_t internal_travel_time = uint32_decode_le((uint8_t *) &(payload->travel_time));
    LOGI("Fragmented Data received: indication_status = %d, src_add = %d, lenght=%u, "
         "travel time = %d, dst_ep = %d, ts=%llu, id=%u at=%u\n",
                payload->indication_status,
                payload->src_add,
                payload->apdu_length,
                internal_travel_time,
                payload->dest_endpoint,
                timestamp_ms_epoch,
                payload->full_packet_id,
                payload->fragment_offset_flag & DSAP_FRAG_LENGTH_MASK);

    frag = (reassembly_fragment_t) {
        .src_add = payload->src_add,
        .packet_id = payload->full_packet_id,
        .size = payload->apdu_length,
        .offset = payload->fragment_offset_flag & DSAP_FRAG_LENGTH_MASK,
        .last_fragment = (payload->fragment_offset_flag & DSAP_FRAG_LAST_FLAG_MASK) == DSAP_FRAG_LAST_FLAG_MASK,
        .bytes = payload->apdu,
        .timestamp = timestamp_ms_epoch,
    };

    if (reassembly_add_fragment(&frag, &full_size) && full_size != 0 )
    {
        size_t full_size = sizeof(reassembly_buffer);

        if (!reassembly_get_full_message(payload->src_add, payload->full_packet_id, reassembly_buffer, &full_size))
        {
            LOGE("Cannot get full packet that was supposed to be full\n");
        }

        // Full packet received
        LOGD("Full size is %d\n", full_size);

        if (m_data_cb == NULL)
        {
            // No cb registered
            return;
        }

        // Create the qos (taken from last rx fragment)
        qos = APP_QOS_NORMAL;
        if (payload->qos_hop_count & 0x01)
        {
            qos = APP_QOS_HIGH;
        }

        // Get the number of hops (taken from last rx fragment)
        hop_count = payload->qos_hop_count >> 2;

        // Call the registered callback
        m_data_cb(reassembly_buffer,
            full_size,
            payload->src_add,
            payload->dest_add,
            qos,
            payload->src_endpoint,
            payload->dest_endpoint,
            internal_time_to_ms(internal_travel_time), // Travel time is the one from last fragment received
            hop_count,
            timestamp_ms_epoch); // TS is also the one from last fragment received
    }

    // Do GC synchronously to avoid races as all fragment related actions happens on same thread
    // and no need for an another scheduling method to add in Platform
    if (m_fragment_max_duration_s > 0 &&
        Platform_get_timestamp_ms_epoch() - last_gc_ts_ms > (MIN_GARBAGE_COLLECT_PERIOD_S * 1000))
    {
        // Time for a new GC
        reassembly_garbage_collect(m_fragment_max_duration_s);
        last_gc_ts_ms = Platform_get_timestamp_ms_epoch();
    }
}

void dsap_data_rx_indication_handler(dsap_data_rx_ind_pl_t * payload,
                                     unsigned long long timestamp_ms_epoch)
{
    uint32_t internal_travel_time = uint32_decode_le((uint8_t *) &(payload->travel_time));
    app_qos_e qos;
    uint8_t hop_count;
    onDataReceived_cb_f cb;

    LOGI("Data received: indication_status = %d, src_add = %d, lenght=%u, "
         "travel time = %d, dst_ep = %d, ts=%llu\n",
         payload->indication_status,
         payload->src_add,
         payload->apdu_length,
         internal_travel_time,
         payload->dest_endpoint,
         timestamp_ms_epoch);

#ifdef REGISTER_DATA_PER_ENDPOINT
    cb = data_cb_table[payload->dest_endpoint];
#else
    cb = m_data_cb;
#endif
    if (cb == NULL)
    {
        // No cb registered
        return;
    }

    // Create the qos
    qos = APP_QOS_NORMAL;
    if (payload->qos_hop_count & 0x01)
    {
        qos = APP_QOS_HIGH;
    }

    // Get the number of hops
    hop_count = payload->qos_hop_count >> 2;

    // Call the registered callback
    cb(payload->apdu,
       payload->apdu_length,
       payload->src_add,
       payload->dest_add,
       qos,
       payload->src_endpoint,
       payload->dest_endpoint,
       internal_time_to_ms(internal_travel_time),
       hop_count,
       timestamp_ms_epoch);
}

#ifdef REGISTER_DATA_PER_ENDPOINT
bool dsap_register_for_data(uint8_t dst_ep, onDataReceived_cb_f onDataReceived)
{
    bool ret = false;

    Platform_lock_request();
    if (dst_ep < MAX_NUMBER_EP && data_cb_table[dst_ep] == NULL)
    {
        data_cb_table[dst_ep] = onDataReceived;
        ret = true;
    }
    Platform_unlock_request();

    return ret;
}

bool dsap_unregister_for_data(uint8_t dst_ep)
{
    bool ret = false;

    Platform_lock_request();
    if (dst_ep < MAX_NUMBER_EP && data_cb_table[dst_ep] != NULL)
    {
        data_cb_table[dst_ep] = NULL;
        ret = true;
    }
    Platform_unlock_request();

    return ret;
}
#else
bool dsap_register_for_data(onDataReceived_cb_f onDataReceived)
{
    m_data_cb = onDataReceived;
    return true;
}

bool dsap_unregister_for_data()
{
    m_data_cb = NULL;
    return true;
}
#endif

bool dsap_set_max_fragment_duration(unsigned int fragment_max_duration_s)
{
    m_fragment_max_duration_s = fragment_max_duration_s;
    return true;
}

void dsap_init()
{
    // Initialize internal structures
#ifdef REGISTER_DATA_PER_ENDPOINT
    memset(data_cb_table, 0, sizeof(data_cb_table));
#else
    m_data_cb = NULL;
#endif
    memset(indication_sent_cb_table, 0, sizeof(indication_sent_cb_table));
}
