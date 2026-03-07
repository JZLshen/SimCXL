/*
 * CXL RPC Global Memory Allocator
 *
 * Manages the CXL memory space and allocates per-connection regions
 * (doorbell, metadata queue, request data, response data, flag).
 * Thread-safe: protected by internal pthread mutex.
 */

#ifndef CXL_ALLOC_H
#define CXL_ALLOC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimum allocation granularity: 64B cacheline */
#define CXL_ALLOC_MIN_SIZE      64
#define CXL_ALLOC_ALIGNMENT     64

/* Default region sizes */
#define CXL_DEFAULT_DOORBELL_SIZE       64                /* 64B  */
#define CXL_DEFAULT_METADATA_Q_SIZE     (16 * 1024)       /* 16KB (1024 entries x 16B) */
#define CXL_DEFAULT_REQUEST_DATA_SIZE   (10 * 1024 * 1024)/* 10MB */
#define CXL_DEFAULT_RESPONSE_DATA_SIZE  (10 * 1024 * 1024)/* 10MB */
/*
 * Flag carries a 32-bit request_id in the current protocol.
 * Global allocator still rounds every region up to cacheline granularity
 * internally.
 */
#define CXL_DEFAULT_FLAG_SIZE           4

/*
 * Default per-connection logical layout (relative to connection base).
 * For dynamic registration, controller/user-space agree on these offsets.
 */
#define CXL_CONN_OFF_DOORBELL       0x000000
#define CXL_CONN_OFF_METADATA_QUEUE 0x001000
#define CXL_CONN_OFF_REQUEST_DATA   0x005000
#define CXL_CONN_OFF_RESPONSE_DATA \
    (CXL_CONN_OFF_REQUEST_DATA + CXL_DEFAULT_REQUEST_DATA_SIZE)
#define CXL_CONN_OFF_FLAG \
    (CXL_CONN_OFF_RESPONSE_DATA + CXL_DEFAULT_RESPONSE_DATA_SIZE)

/* ================================================================
 * Tier 1: Global Allocator
 * ================================================================ */

typedef struct cxl_global_allocator cxl_global_alloc_t;

/**
 * Initialize global allocator for CXL memory space.
 *
 * @param base_addr  Physical base address of CXL memory (e.g., 0x100000000)
 * @param total_size Total size of CXL memory region
 * @return           Allocator handle, or NULL on failure
 */
cxl_global_alloc_t *cxl_global_alloc_init(uint64_t base_addr,
                                           size_t total_size);

/**
 * Destroy global allocator and free all internal state.
 */
void cxl_global_alloc_destroy(cxl_global_alloc_t *alloc);

/* Convenience: allocate all regions for one connection */
typedef struct {
    uint64_t doorbell_addr;
    uint64_t metadata_queue_addr;
    uint64_t request_data_addr;
    uint64_t response_data_addr;
    uint64_t flag_addr;
    size_t   metadata_queue_size;
    size_t   request_data_size;
    size_t   response_data_size;
    /* Underlying allocation block for dynamic layout. */
    uint64_t alloc_base_addr;
    size_t   alloc_size;
} cxl_connection_addrs_t;

/**
 * Allocate all regions for a connection in one call.
 *
 * @param alloc     Global allocator
 * @param mq_size   Metadata queue size (0 for default 16KB)
 * @param req_size  Request data size (0 for default 10MB)
 * @param resp_size Response data size (0 for default 10MB)
 * @param out       Output: allocated addresses
 * @return          0 on success, -1 on failure (all freed on error)
 */
int cxl_global_alloc_connection(cxl_global_alloc_t *alloc,
                                 size_t mq_size,
                                 size_t req_size,
                                 size_t resp_size,
                                 cxl_connection_addrs_t *out);

/**
 * Free all regions for a connection.
 */
void cxl_global_free_connection(cxl_global_alloc_t *alloc,
                                 const cxl_connection_addrs_t *addrs);

#ifdef __cplusplus
}
#endif

#endif /* CXL_ALLOC_H */
