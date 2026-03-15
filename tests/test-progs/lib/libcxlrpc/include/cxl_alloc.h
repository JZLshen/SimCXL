/*
 * CXL RPC fixed shared-memory layout definitions.
 *
 * The current public RPC flow uses caller-provided fixed address ranges for
 * doorbell, metadata queue, request data, response data, and flag regions.
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
 * Flag publishes a 15-bit logical rpc_id carried in a uint16_t slot, but the
 * storage region is still a full 64B cacheline because the CopyEngine writes
 * the whole line atomically.
 */
#define CXL_DEFAULT_FLAG_SIZE           64

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

typedef struct {
    uint64_t doorbell_addr;
    uint64_t metadata_queue_addr;
    uint64_t request_data_addr;
    uint64_t response_data_addr;
    uint64_t flag_addr;
    size_t   metadata_queue_size;
    size_t   request_data_size;
    size_t   response_data_size;
    /*
     * Source node identity published into request doorbells.
     * Server-owner connections that only poll the shared metadata queue can
     * leave this as 0.
     */
    uint16_t node_id;
} cxl_connection_addrs_t;

#ifdef __cplusplus
}
#endif

#endif /* CXL_ALLOC_H */
