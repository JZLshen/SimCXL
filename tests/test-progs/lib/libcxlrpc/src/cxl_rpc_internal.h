#ifndef CXL_RPC_INTERNAL_H
#define CXL_RPC_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "cxl_rpc.h"

#define CXL_RPC_ID_MASK            0x00007FFFu
#define CXL_NODE_ID_MASK           0x00003FFFu
#define CXL_REQ_PAYLOAD_SOFT_MAX   (256u * 1024u)
#define CXL_DOORBELL_ENTRY_LEN     16u
#define CXL_DOORBELL_PUBLISH_LEN   16u
#define CXL_FLAG_PUBLISH_LEN       8u
#define CXL_FLAG_CACHELINE_BYTES   64u
#define CXL_RESP_HEADER_LEN        8u
#define CXL_METADATA_TRANSLATION_PAGE_BYTES 4096u

#define CXL_CONN_CAP_REQUEST_RX     (1u << 0)
#define CXL_CONN_CAP_REQUEST_TX     (1u << 1)
#define CXL_CONN_CAP_RESPONSE_RX    (1u << 2)
#define CXL_CONN_CAP_RESPONSE_TX    (1u << 3)
#define CXL_CONN_CAP_BOOTSTRAP      (1u << 4)
#define CXL_CONN_CAP_HEAD_SYNC      (1u << 5)

typedef struct cxl_ce_desc cxl_ce_desc_t;

static inline uint64_t
cxl_pack_response_header(uint32_t payload_len, uint16_t rpc_id)
{
    return (uint64_t)payload_len |
           ((uint64_t)rpc_id << 32);
}

struct cxl_context {
    volatile uint8_t *base;
    uint64_t phys_base;
    size_t map_size;
    int numa_node;
    int shm_fd;
    char shm_name[64];
};

struct cxl_connection {
    cxl_context_t *ctx;
    uint32_t caps;

    cxl_connection_addrs_t addrs;

    volatile uint8_t *doorbell;
    volatile uint8_t *metadata_queue;
    volatile uint8_t *request_data;
    volatile uint8_t *response_data;
    volatile uint8_t *flag;

    uint32_t mq_entries;
    uint32_t mq_total_lines;
    uint32_t mq_head;
    uint32_t mq_phase;
    uint32_t mq_prefetch_start_line;
    uint32_t mq_prefetch_nr_lines;
    uint8_t mq_prefetch_window_valid;
    uint8_t mq_payload_prepared_mask;
    uint8_t mq_payload_next_probe_slot;

    uint16_t rpc_id_next;
    uint64_t resp_read_cursor;
    uint64_t resp_known_producer_cursor;
    uint64_t resp_peek_cursor;
    size_t resp_peek_len;
    uint16_t resp_peek_rpc_id;
    uint8_t resp_peek_valid;
    uint8_t resp_peek_payload_loaded;
    size_t req_write_offset;

    cxl_adaptive_sync_t *sync;

    uint64_t peer_response_data_addr;
    uint64_t peer_flag_addr;
    size_t peer_response_data_size;
    volatile uint8_t *peer_response_data;
    volatile uint8_t *peer_flag;
    uint64_t peer_resp_write_cursor;
    int resp_tx_ready;

    int ce_lane_bind_valid;
    int ce_bind_lane_index_valid;
    size_t ce_bind_engine_index;
    size_t ce_bind_lane_index;
    uint32_t ce_bind_channel_id;

    int ce_lane_assigned;
    size_t ce_engine_index;
    size_t ce_channel_index;
    uint32_t ce_hw_channel_id;

    uint64_t *ce_peer_resp_page_phys;
    uint64_t ce_peer_resp_logical_page_base;
    size_t ce_peer_resp_page_size;
    size_t ce_peer_resp_page_count;
    uintptr_t ce_peer_resp_virt_page_base;
    uint64_t ce_peer_flag_phys;
    int ce_peer_flag_phys_valid;
};

void cxl_connection_init_runtime_defaults(cxl_connection_t *conn);
void cxl_copyengine_disable(cxl_connection_t *conn);
int cxl_copyengine_prepare(cxl_connection_t *conn);
int cxl_copyengine_update_peer_response_mapping(cxl_connection_t *conn);
int cxl_copyengine_update_peer_flag_mapping(cxl_connection_t *conn);
int cxl_copyengine_validate_submit_invariants(cxl_connection_t *conn);
int cxl_copyengine_response_publish_pending(cxl_connection_t *conn);
int cxl_copyengine_submit_flag_async(cxl_connection_t *conn,
                                     uint64_t producer_cursor);
int cxl_copyengine_submit_response_async(cxl_connection_t *conn,
                                         uint16_t rpc_id,
                                         const void *data,
                                         size_t len,
                                         uint64_t producer_cursor,
                                         size_t dst_resp_offset);

#endif
