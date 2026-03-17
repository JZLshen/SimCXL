#ifndef CXL_RPC_INTERNAL_H
#define CXL_RPC_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "cxl_rpc.h"

#define CXL_RPC_ID_MASK            0x00007FFFu
#define CXL_RPC_ID_SPACE           0x00008000u
#define CXL_RPC_ID_BITMAP_WORDS    (CXL_RPC_ID_SPACE / 64u)
#define CXL_NODE_ID_MASK           0x00003FFFu
#define CXL_REQ_PAYLOAD_SOFT_MAX   (256u * 1024u)
#define CXL_DOORBELL_ENTRY_LEN     16u
#define CXL_DOORBELL_PUBLISH_LEN   16u
#define CXL_FLAG_PUBLISH_LEN       2u
#define CXL_FLAG_CACHELINE_BYTES   64u
#define CXL_RESP_HEADER_LEN        8u
#define CXL_RESP_SLOT_BYTES        4096u
#define CXL_RESP_PAYLOAD_SOFT_MAX  (CXL_RESP_SLOT_BYTES - CXL_RESP_HEADER_LEN)
#define CXL_METADATA_TRANSLATION_PAGE_BYTES 4096u

typedef struct cxl_ce_desc cxl_ce_desc_t;

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

    cxl_connection_addrs_t addrs;

    volatile uint8_t *doorbell;
    volatile uint8_t *metadata_queue;
    volatile uint8_t *request_data;
    volatile uint8_t *response_data;
    volatile uint8_t *flag;

    uint32_t mq_entries;
    uint32_t mq_head;
    uint32_t mq_phase;
    uint32_t mq_prefetch_start_line;
    uint32_t mq_prefetch_nr_lines;
    uint8_t mq_prefetch_window_valid;

    uint16_t rpc_id_seq_mask;
    uint16_t rpc_id_next;
    uint32_t rpc_id_inflight_count;
    uint32_t rpc_id_capacity;
    uint64_t *rpc_id_inflight_bitmap;
    uint32_t *req_entry_offsets;
    uint32_t *req_entry_sizes;
    uint16_t *req_entry_next;
    uint8_t *req_entry_complete;
    uint16_t req_ring_head_id;
    uint16_t req_ring_tail_id;
    size_t req_reclaim_offset;
    size_t req_ring_used_bytes;
    size_t resp_read_offset;
    size_t req_write_offset;

    cxl_adaptive_sync_t *sync;

    uint64_t peer_response_data_addr;
    uint64_t peer_flag_addr;
    size_t peer_response_data_size;
    volatile uint8_t *peer_response_data;
    volatile uint8_t *peer_flag;
    size_t peer_resp_write_offset;
    int resp_tx_ready;
    uint8_t request_local_buf[CXL_REQ_PAYLOAD_SOFT_MAX + 1];

    int ce_lane_bind_valid;
    size_t ce_bind_engine_index;
    uint32_t ce_bind_channel_id;

    int ce_init_attempted;
    int ce_ready;
    int ce_warned;
    int ce_chain_started;
    int ce_lane_assigned;
    int ce_bar_fd;
    volatile uint8_t *ce_bar0;
    void *ce_bar_map_base;
    size_t ce_bar_map_len;
    size_t ce_engine_index;
    size_t ce_channel_index;
    uint32_t ce_hw_channel_id;
    size_t ce_slots;
    size_t ce_next_slot;
    uint8_t *ce_resp_src_pool;
    uint8_t *ce_flag_src_pool;
    cxl_ce_desc_t *ce_desc_pool;
    cxl_ce_desc_t *ce_last_desc;

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
int cxl_copyengine_ensure_response_slots(cxl_connection_t *conn);
int cxl_copyengine_update_peer_response_mapping(cxl_connection_t *conn);
int cxl_copyengine_update_peer_flag_mapping(cxl_connection_t *conn);
int cxl_copyengine_validate_submit_invariants(cxl_connection_t *conn);
int cxl_copyengine_submit_response_async(cxl_connection_t *conn,
                                         uint16_t rpc_id,
                                         const void *data,
                                         size_t len,
                                         size_t dst_resp_offset,
                                         size_t resp_transfer_size);

#endif
