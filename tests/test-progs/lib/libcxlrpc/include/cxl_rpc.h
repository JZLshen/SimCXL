/*
 * CXL RPC Core Library
 *
 * Provides user-space API for CXL shared memory RPC:
 *   - Context: KM NUMA-backed shared mapping (no /dev/cxl_mem0 or /dev/mem)
 *   - Connection: caller-provided fixed shared-memory layout
 *   - Client: send_request + producer-cursor probe + ordered response drain
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
 *   bytes 6-7:  reserved (written as zero, ignored by consumer)
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
 * Create a server-side metadata-queue polling connection for a fixed layout.
 *
 * This role owns the destructive bootstrap path for the shared request queue.
 * It initializes only the local state required to poll requests and publish
 * periodic HEAD_UPDATE notifications.
 */
cxl_connection_t *cxl_connection_create_server_poll_owner(
    cxl_context_t *ctx,
    const cxl_connection_addrs_t *addrs,
    uint32_t mq_entries);

/**
 * Create a client-side fixed-layout endpoint.
 *
 * This role initializes only the local state required to send requests and
 * drain responses from the caller's own response-data / flag region.
 */
cxl_connection_t *cxl_connection_create_client_attach(
    cxl_context_t *ctx,
    const cxl_connection_addrs_t *addrs);

/**
 * Create a server-side response transmit endpoint.
 *
 * This role carries only server-side response-transmit state. It does not map
 * or initialize any local fixed-layout doorbell / metadata / response / flag
 * regions. Peer response-data / flag addresses are configured later via
 * cxl_connection_set_peer_response_data() and
 * cxl_connection_set_peer_response_flag_addr().
 */
cxl_connection_t *cxl_connection_create_response_tx(cxl_context_t *ctx);

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
 * Bind one dedicated CopyEngine lane for large-response DMA publish.
 *
 * A lane is one fixed `(engine_index, channel_index)` pair. Small responses
 * are still published by CPU store+flush, but `cxl_send_response()` requires
 * one bound lane for payloads at or above the library's large-response DMA
 * threshold. Public configs keep `channel_index = 0` and derive
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

/**
 * Bind one dedicated CopyEngine lane by global lane index.
 *
 * Lane indices follow the library's discovered global lane order:
 * engines are sorted by PCI resource path, and channels are enumerated in
 * ascending channel ID inside each engine. This keeps server-side client to
 * lane binding stable even when public configs use multiple channels per
 * engine.
 *
 * @param conn        Connection handle
 * @param lane_index  Zero-based global lane index
 * @return            0 on success, -1 on error
 */
int cxl_connection_bind_copyengine_lane_index(cxl_connection_t *conn,
                                              size_t lane_index);

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
 * The current public request_data path is append-only for the lifetime of one
 * connection. It does not reclaim or wrap request_data at runtime.
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
 * Read response producer cursor from peer flag.
 *
 * The returned cursor is the producer's committed byte cursor for the shared
 * response ring. It is monotonically increasing and counts wrap padding, so
 * the client can distinguish real data from tail gaps without any extra
 * sentinel metadata in response headers.
 *
 * The client has unread responses iff the returned cursor differs from the
 * local consumer cursor.
 *
 * Physical storage is one 64-bit value inside the 64B shared flag cacheline.
 *
 * @param conn        Connection handle
 * @param out_cursor  Out: committed producer byte cursor
 * @return            1 if unread responses exist, 0 if consumer is caught up,
 *                    -1 on error
 */
int cxl_peek_response_producer_cursor(cxl_connection_t *conn,
                                      uint64_t *out_cursor);

/**
 * Peek the next response payload as an in-place view (non-blocking).
 *
 * This is the zero-copy counterpart to `cxl_consume_next_response()`.
 * It returns a direct payload view into the shared response-data region at the
 * current consumer head, but does not advance that head yet.
 *
 * The returned view remains valid only until the caller advances the response
 * head or another agent overwrites the same shared slot.
 *
 * @param conn            Connection handle
 * @param out_data_view   Out: direct payload view inside shared response_data
 * @param out_len         Out: exact payload size
 * @param out_rpc_id      Out: response rpc_id at current consumer head
 * @return                1 if one response is available, 0 if pending, -1 on error
 */
int cxl_peek_next_response_view(cxl_connection_t *conn,
                                const void **out_data_view,
                                size_t *out_len,
                                uint16_t *out_rpc_id);

/**
 * Advance the response consumer head after a successful zero-copy peek.
 *
 * The caller must pass the `(rpc_id, len)` pair previously returned by
 * `cxl_peek_next_response_view()` for the current response head.
 *
 * @param conn        Connection handle
 * @param rpc_id      Expected rpc_id at current consumer head
 * @param len         Expected payload length at current consumer head
 * @return            1 if advanced, 0 if no response pending, -1 on error/mismatch
 */
int cxl_advance_response_head(cxl_connection_t *conn,
                              uint16_t rpc_id,
                              size_t len);

/**
 * Consume the next response header in producer order (non-blocking).
 *
 * Reads only the response header at the current consumer head, returns the
 * `(rpc_id, len)` pair, and advances the consumer head without copying or
 * demand-loading the payload body into host memory.
 *
 * This is the lowest-overhead client receive path when the caller only needs
 * completion ordering/latency and does not inspect response payload bytes.
 *
 * @param conn        Connection handle
 * @param out_len     Out: exact payload size
 * @param out_rpc_id  Out: consumed response rpc_id
 * @return            1 if one response consumed, 0 if pending, -1 on error
 */
int cxl_consume_next_response_header(cxl_connection_t *conn,
                                     size_t *out_len,
                                     uint16_t *out_rpc_id);

/**
 * Consume the next response entry in producer order (non-blocking).
 *
 * Copies exactly one response payload from the current response-data head into
 * caller-owned local memory, then advances the consumer offset. This is a
 * compatibility wrapper around `cxl_peek_next_response_view()` plus
 * `cxl_advance_response_head()`.
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
 *     address inside the shared CXL aperture. out_data_view points directly
 *     to that shared payload region after the library has invalidated and
 *     prefetched it for read-side consumption.
 *   Callers should consume the returned view before performing the next
 *   request poll on the same connection.
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
 * Sends one variable-length response entry into peer `response_data` and then
 * publishes the committed response producer cursor to peer `flag`.
 *
 * The response ring is one large shared region. Entries are cacheline-aligned
 * but do not use any fixed per-response slot size.
 *
 * Response entries whose `(header + payload)` size is at most 4 KiB are copied
 * into peer `response_data` by CPU store plus `clflushopt`/`sfence`.
 *
 * When the same connection already has an older CopyEngine response publish
 * in flight, the small-response payload still uses CPU copy, but the final
 * producer-cursor flag write is serialized onto that same CopyEngine lane so
 * the shared cursor never overtakes an older DMA response.
 *
 * Response entries whose `(header + payload)` size exceeds 4 KiB are published
 * by one asynchronous CopyEngine descriptor chain:
 *   response entry -> producer cursor flag
 * This large-response path requires one dedicated CopyEngine lane to be bound
 * on the connection before send time.
 *
 * Response headers remain 8 bytes wide for aligned 64-bit load/store, but
 * their active semantics are only `(payload_len, rpc_id)`. The upper 16 bits
 * are reserved and written as zero. The flag only advances the shared producer
 * cursor for ordered response draining.
 *
 * @param conn    Server-side connection handle (with peer addresses set)
 * @param rpc_id  RPC ID to respond to (must be 1..32767)
 * @param data    Response payload
 * @param len     Payload length in bytes
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
 * Maps peer `response_data` into the local CXL shared region used by the
 * direct response-publish path.
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
 * Set peer response-flag address for server response producer-cursor publish.
 *
 * The response path becomes ready only after both peer `response_data` and
 * peer `flag` are configured. Responses are then published directly into the
 * shared response ring and committed by producer cursor.
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
