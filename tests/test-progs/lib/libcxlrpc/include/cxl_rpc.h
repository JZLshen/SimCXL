/*
 * CXL RPC Core Library
 *
 * Provides user-space API for CXL shared memory RPC:
 *   - Context: KM NUMA-backed shared mapping (no /dev/cxl_mem0 or /dev/mem)
 *   - Connection: caller-provided fixed shared-memory layout
 *   - Client: send_request + completion-flag probe + ordered response drain
 *   - Server: poll_request (metadata queue) + adaptive head sync
 */

#ifndef CXL_RPC_H
#define CXL_RPC_H

#include <stdint.h>
#include <stddef.h>

#include "cxl_alloc.h"
#include "cxl_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CXL memory default base address (4GB boundary) */
#define CXL_BASE_DEFAULT        0x100000000ULL
#define CXL_SIZE_DEFAULT        0x200000000ULL  /* 8GB */

/* Doorbell method types */
#define CXL_METHOD_REQUEST      0
#define CXL_METHOD_HEAD_UPDATE  1

/*
 * Response entry header layout (8 bytes):
 *   bytes 0-3:  payload_len (uint32 LE)
 *   bytes 4-5:  rpc_id      (uint16 LE, low 15 bits used)
 *   bytes 6-7:  response_id (uint16 LE, low 15 bits used)
 */

/*
 * Request entry format (non-inline request_data entries):
 *
 * - byte 0.. : payload bytes only (no request header).
 *
 * Storage slot is cacheline-rounded (64B aligned start, 64B-multiple span).
 * Bytes after the published payload region in the slot are reserved.
 *
 * Request payload length is carried in the 32-bit doorbell length field for
 * both inline and non-inline requests.
 *
 */
/* Metadata queue entry size: 16B (4 entries per 64B cacheline). */
#define CXL_METADATA_ENTRY_SIZE 16

/* ================================================================
 * Context Management
 * ================================================================ */

typedef struct cxl_context cxl_context_t;

/**
 * Initialize CXL RPC context.
 *
 * The context uses KM NUMA policy with a process-shared mapping by default.
 * Relevant environment variables:
 *   - CXL_RPC_NUMA_NODE (default: 1)
 *   - CXL_RPC_SHM_NAME (default: /cxl_rpc_region)
 *   - CXL_RPC_SHM_UNLINK_ON_DESTROY (default: 0). If non-zero, unlink
 *     the shared object in cxl_rpc_destroy().
 *   - CXL_RPC_CLEAR_ON_INIT (default: 0). When set to a non-zero value,
 *     the whole mapped region is zeroed during init.
 *
 * The API base (`cxl_base`) stays as a logical/protocol base used by
 * fixed-layout address helpers and `cxl_rpc_phys_to_virt()`.
 *
 * @param cxl_base  Logical/protocol base address (0 for CXL_BASE_DEFAULT)
 * @param cxl_size  Map size (0 for default 8GB)
 * @return          Context handle, or NULL on failure
 */
cxl_context_t *cxl_rpc_init(uint64_t cxl_base, size_t cxl_size);

/**
 * Destroy context and release NUMA-backed allocation.
 */
void cxl_rpc_destroy(cxl_context_t *ctx);

/**
 * Get virtual base pointer for CXL memory.
 * Use for direct memory access (offset from logical/protocol base).
 */
volatile void *cxl_rpc_get_base(const cxl_context_t *ctx);

/**
 * Convert logical/protocol address to virtual pointer inside the shared map.
 */
volatile void *cxl_rpc_phys_to_virt(const cxl_context_t *ctx,
                                     uint64_t phys_addr);

/* ================================================================
 * Connection Management
 * ================================================================ */

typedef struct cxl_connection cxl_connection_t;

/**
 * Create a connection with pre-assigned addresses. This is the owner path
 * for a fixed layout. It initializes local runtime state only; controller-side
 * registration is assumed to be configured out-of-band with final observed
 * addresses.
 */
cxl_connection_t *cxl_connection_create_fixed_owner(cxl_context_t *ctx,
                                                     const cxl_connection_addrs_t *addrs,
                                                     uint32_t mq_entries);

/**
 * Create a connection with pre-assigned addresses without modifying shared
 * request-side state.
 */
cxl_connection_t *cxl_connection_create_fixed_attach(cxl_context_t *ctx,
                                                      const cxl_connection_addrs_t *addrs,
                                                      uint32_t mq_entries);

/**
 * Convenience alias of cxl_connection_create_fixed_owner().
 */
cxl_connection_t *cxl_connection_create_fixed(cxl_context_t *ctx,
                                               const cxl_connection_addrs_t *addrs,
                                               uint32_t mq_entries);

/**
 * Destroy connection and release local runtime state.
 */
void cxl_connection_destroy(cxl_connection_t *conn);

/**
 * Get connection addresses (for exchanging with peer).
 */
const cxl_connection_addrs_t *cxl_connection_get_addrs(
    const cxl_connection_t *conn);

/**
 * Bind one dedicated CopyEngine lane for server-side response DMA.
 *
 * A lane is one fixed `(engine_index, channel_index)` pair. The current
 * response path requires explicit binding before peer response-data / flag
 * setup. Public configs keep `channel_index = 0` and derive
 * `engine_index = node_id`, i.e. `1 node : 1 engine : 1 channel`.
 *
 * @param conn          Connection handle
 * @param engine_index  CopyEngine device index
 * @param channel_index Channel index inside that engine
 * @return              0 on success, -1 on error
 */
int cxl_connection_bind_copyengine_lane(cxl_connection_t *conn,
                                        size_t engine_index,
                                        uint32_t channel_index);

/* ================================================================
 * Client API
 * ================================================================ */

/**
 * Send an RPC request.
 *
 * If len <= 8, uses inline mode (data in doorbell).
 * Otherwise, writes request_data payload + absolute logical address in the
 * doorbell data field.
 *
 * Doorbell length is a full 32-bit field. Current software limit remains
 * 256 KiB. The request source `node_id` is taken from `conn->addrs.node_id`.
 *
 * @param conn       Connection handle
 * @param data       Request payload
 * @param len        Payload length in bytes
 * @return           rpc_id in [1, 32767], or -1
 */
int cxl_send_request(cxl_connection_t *conn,
                     const void *data,
                     size_t len);

/**
 * Read completion flag (latest completed rpc_id) from peer.
 *
 * This is a non-blocking flag probe. The returned rpc_id is the latest
 * completed response id visible in the single-flag protocol.
 *
 * Physical storage remains `uint16_t`, but only the low 15 bits are used.
 *
 * @param conn        Connection handle
 * @param out_rpc_id  Out: latest rpc_id from flag
 * @return            1 if flag is non-zero, 0 if no completion yet, -1 on error
 */
int cxl_peek_latest_completed_rpc_id(cxl_connection_t *conn,
                                     uint16_t *out_rpc_id);

/**
 * Consume the next response entry in producer order (non-blocking).
 *
 * Copies exactly one response payload from the current response-data head into
 * caller-owned local memory, then advances the consumer offset.
 *
 * This API is intended for single-flag multi-outstanding workflows where the
 * client first calls `cxl_peek_latest_completed_rpc_id()`, checks the
 * returned rpc_id against its pending set, and then drains response_data
 * in producer order until that rpc_id is consumed.
 *
 * The response is considered consumed only after this local copy completes.
 *
 * If `*out_len` is smaller than the payload size, this function returns -1,
 * updates `*out_len` with the exact required size, and does not consume the
 * response entry.
 *
 * @param conn            Connection handle
 * @param out_data        Caller-owned local buffer for payload copy
 * @param out_len         In: buffer size, Out: exact payload size
 * @param out_rpc_id      Out: consumed response rpc_id
 * @return                1 if one response consumed, 0 if pending, -1 on error
 */
int cxl_consume_next_response(cxl_connection_t *conn,
                              void *out_data,
                              size_t *out_len,
                              uint16_t *out_rpc_id);

/* ================================================================
 * Server API
 * ================================================================ */

/**
 * Poll metadata queue for incoming request (non-blocking).
 *
 * Checks phase bit at current head position. If new entry found,
 * advances head and calls adaptive sync internally.
 *
 * @param conn       Connection handle
 * @param node_id    Out: request source node ID
 * @param rpc_id     Out: request rpc_id
 * Payload view semantics:
 *   - Inline request: out_data_view points to metadata entry bytes [8..15].
 *   - Non-inline request: bytes [8..15] carry an absolute logical payload
 *     address inside the shared CXL aperture. The payload is first copied
 *     into a connection-local staging buffer, then out_data_view points to
 *     that local buffer.
 *   Lifetime is valid until this connection performs the next request poll.
 *
 * @param out_data_view Out: payload view pointer (inline or request_data)
 * @param out_len       Out: exact payload size from metadata
 * @return           1 if request available, 0 if empty, -1 on error
 */
int cxl_poll_request(cxl_connection_t *conn,
                     uint16_t *node_id,
                     uint16_t *rpc_id,
                     const void **out_data_view,
                     size_t *out_len);

/**
 * Send an RPC response (server-side).
 *
 * Sends one response entry to peer `response_data` and then publishes the
 * corresponding `rpc_id` to peer `flag` through CopyEngine.
 *
 * Physical storage remains `uint16_t`, but only the low 15 bits are used.
 *
 * @param conn    Server-side connection handle (with peer addresses set)
 * @param rpc_id  RPC ID to respond to (must be 1..32767)
 * @param data    Response payload
 * @param len     Payload length in bytes (current backend max: 4088)
 * @return        0 on success, -1 on error
 */
int cxl_send_response(cxl_connection_t *conn,
                      uint16_t rpc_id,
                      const void *data,
                      size_t len);

/**
 * Set peer response-data region for server sending responses.
 *
 * Must be called after connection creation, before send_response.
 * Resolves peer response_data into a local observed DMA page map once during
 * setup so the response send hot path only works with logical offsets.
 * Peer flag is configured separately via
 * cxl_connection_set_peer_response_flag_addr().
 *
 * @param conn                   Connection handle
 * @param peer_response_data_addr Logical/protocol address of peer response-data region
 * @param peer_response_data_size Size of peer response-data region
 * @return                       0 on success, -1 on error
 */
int cxl_connection_set_peer_response_data(cxl_connection_t *conn,
                                          uint64_t peer_response_data_addr,
                                          size_t peer_response_data_size);

/**
 * Set peer response-flag address for server response completion publish.
 *
 * The response path becomes ready only after both peer `response_data` and
 * peer `flag` are configured. Responses are then sent through CopyEngine.
 *
 * @param conn            Connection handle
 * @param peer_flag_addr  Logical/protocol address of peer's flag region
 * @return                0 on success, -1 on error
 */
int cxl_connection_set_peer_response_flag_addr(cxl_connection_t *conn,
                                               uint64_t peer_flag_addr);

#ifdef __cplusplus
}
#endif

#endif /* CXL_RPC_H */
