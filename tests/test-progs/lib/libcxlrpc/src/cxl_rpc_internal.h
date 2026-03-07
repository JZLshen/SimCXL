#ifndef CXL_RPC_INTERNAL_H
#define CXL_RPC_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "cxl_rpc.h"

#define CXL_REQ_ID_MASK            0x0000FFFFu
#define CXL_REQ_META_LEN_BITS      20u
#define CXL_REQ_META_LEN_MAX       ((1u << CXL_REQ_META_LEN_BITS) - 1u)
#define CXL_REQ_PAYLOAD_SOFT_MAX   (256u * 1024u)
#define CXL_DOORBELL_ENTRY_LEN     16u
#define CXL_DOORBELL_PUBLISH_LEN   16u
#define CXL_FLAG_PUBLISH_LEN       4u

typedef struct cxl_ce_desc cxl_ce_desc_t;

struct cxl_context {
    volatile uint8_t *base;
    uint64_t phys_base;
    size_t map_size;
    cxl_global_alloc_t *allocator;
    int numa_node;
    int shm_fd;
    char shm_name[64];
};

struct cxl_connection {
    cxl_context_t *ctx;

    cxl_connection_addrs_t addrs;
    int owns_addrs;

    volatile uint8_t *doorbell;
    volatile uint8_t *metadata_queue;
    volatile uint8_t *request_data;
    volatile uint8_t *response_data;
    volatile uint8_t *flag;

    uint32_t mq_entries;
    uint32_t mq_head;
    uint32_t mq_phase;

    uint32_t req_id_prng_state;
    uint16_t req_id_prefix;
    uint8_t req_id_prefix_bits;
    size_t resp_read_offset;
    size_t req_write_offset;

    cxl_adaptive_sync_t *sync;

    uint64_t peer_response_data_addr;
    uint64_t peer_flag_addr;
    size_t peer_response_data_size;
    volatile uint8_t *peer_response_data;
    volatile uint8_t *peer_flag;
    size_t peer_resp_write_offset;

    uint64_t peer_request_data_addr;
    size_t peer_request_data_size;
    uint8_t request_local_buf[CXL_REQ_PAYLOAD_SOFT_MAX + 1];

    int ce_init_attempted;
    int ce_ready;
    int ce_warned;
    int ce_chain_started;
    int ce_bar_fd;
    volatile uint8_t *ce_bar0;
    void *ce_bar_map_base;
    size_t ce_bar_map_len;
    size_t ce_slots;
    size_t ce_next_slot;
    uint8_t *ce_resp_src_pool;
    uint8_t *ce_flag_src_pool;
    cxl_ce_desc_t *ce_desc_pool;
    cxl_ce_desc_t *ce_last_desc;

    uint64_t *ce_peer_resp_page_phys;
    uintptr_t ce_peer_resp_virt_page_base;
    size_t ce_peer_resp_page_size;
    size_t ce_peer_resp_page_count;
    uint64_t ce_peer_flag_phys;
    int ce_peer_flag_phys_valid;
};

void cxl_connection_init_runtime_defaults(cxl_connection_t *conn);
void cxl_copyengine_disable(cxl_connection_t *conn);
int cxl_copyengine_prepare(cxl_connection_t *conn);
int cxl_copyengine_update_peer_response_mapping(cxl_connection_t *conn);
int cxl_copyengine_update_peer_flag_mapping(cxl_connection_t *conn);
int cxl_copyengine_submit_response_async(cxl_connection_t *conn,
                                         uint32_t request_id,
                                         const void *data,
                                         size_t len,
                                         const volatile void *dst_resp,
                                         size_t resp_transfer_size);

#endif
