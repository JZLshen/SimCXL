/*
 * CXL RPC Core Library
 *
 * Provides user-space API for CXL shared memory RPC:
 *   - Context: KM NUMA-backed allocation (no /dev/cxl_mem0 or /dev/mem)
 *   - Connection: allocated via global allocator, managed regions
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

/* Doorbell method types (from pipeline.md) */
#define CXL_METHOD_REQUEST      0
#define CXL_METHOD_RESPONSE     1
#define CXL_METHOD_HEAD_UPDATE  2
#define CXL_METHOD_CONTROL      3

/*
 * Internal control namespace used by controller/client bootstrap.
 * REGISTER_TRANSLATION carries the client logical doorbell address in
 * data[63:0], allowing the controller to infer the runtime doorbell-page GPA.
 *
 * REGISTER_METADATA_PAGE_TRANSLATION keeps the api-numa backend and teaches
 * the controller the observed GPA of each metadata page explicitly.
 * request_id[15:0] + payload_len[19:0] encode the metadata page index relative
 * to metadata_queue_addr, while data[63:0] carries the observed page base GPA.
 */
#define CXL_CONTROL_SERVICE_INTERNAL                      0x07A1u
#define CXL_CONTROL_METHOD_REGISTER_TRANSLATION           0x0001u
#define CXL_CONTROL_METHOD_REGISTER_METADATA_PAGE_TRANSLATION 0x0002u
#define CXL_CONTROL_RESET_STATE 1

/*
 * Response entry header layout (8 bytes):
 *   bytes 0-3:  payload_len (uint32 LE)
 *   bytes 4-5:  request_id  (uint16 LE)
 *   bytes 6-7:  response_id (uint16 LE)
 */

/*
 * Request entry format (non-inline request_data entries):
 *
 * - byte 0.. : payload bytes only (no request header).
 *
 * Storage slot is cacheline-rounded (64B aligned start, 64B-multiple span).
 * Bytes after the published payload region in the slot are reserved.
 *
 * Request payload length is carried in metadata header bits [23:4] (20 bits)
 * for both inline and non-inline requests.
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
 * allocators and `cxl_rpc_phys_to_virt()`.
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
 * Create a connection: allocate all regions via global allocator
 * and initialize local allocators + adaptive sync.
 *
 * @param ctx        RPC context
 * @param mq_entries Metadata queue entries (0 for default 1024)
 * @return           Connection handle, or NULL on failure
 */
cxl_connection_t *cxl_connection_create(cxl_context_t *ctx,
                                         uint32_t mq_entries);

/**
 * Create a connection with pre-assigned addresses. This is the owner path
 * for a fixed layout. It initializes local runtime state and registers
 * address translation with the RPC engine.
 */
cxl_connection_t *cxl_connection_create_fixed_owner(cxl_context_t *ctx,
                                                     const cxl_connection_addrs_t *addrs,
                                                     uint32_t mq_entries);

/**
 * Create a connection with pre-assigned addresses without modifying shared
 * request-side state. This attach-only path still performs a non-destructive
 * address-translation registration so request doorbells can be learned even
 * when the peer wins the startup race.
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
 * Destroy connection: free regions back to global allocator.
 */
void cxl_connection_destroy(cxl_connection_t *conn);

/**
 * Get connection addresses (for exchanging with peer).
 */
const cxl_connection_addrs_t *cxl_connection_get_addrs(
    const cxl_connection_t *conn);

/**
 * Select this connection's client-tag value inside the fixed request_id
 * layout used by the protocol.
 *
 * The 16-bit request_id always uses the high bits as a client tag and the
 * low bits as a cyclic per-client sequence:
 *   request_id = (client_tag << (16 - client_tag_bits)) | seq_low_bits
 *
 * seq_low_bits is allocated per-connection and remains reserved until the
 * corresponding response payload has been consumed into local memory by the
 * client. For a single-client deployment, the fixed layout degenerates to
 * client_tag=0 and client_tag_bits=0.
 *
 * @param conn             Connection handle
 * @param client_tag       Client identifier carried in request_id high bits
 * @param client_tag_bits  Number of request_id high bits reserved for tag
 * @return                 0 on success, -1 on invalid config
 */
int cxl_connection_set_client_tag(cxl_connection_t *conn,
                                  uint16_t client_tag,
                                  uint8_t client_tag_bits);

/**
 * Bind one dedicated CopyEngine lane for server-side response DMA.
 *
 * A lane is one fixed `(engine_index, channel_index)` pair. The current
 * response path requires explicit binding before peer response-data / flag
 * setup. Public configs keep `channel_index = 0` and derive
 * `engine_index = client_id`, i.e. `1 client : 1 engine : 1 channel`.
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
 * Otherwise, writes request_data payload + doorbell reference.
 *
 * Payload length is encoded in metadata header bits [23:4] (20 bits).
 * Current software limit is 256 KiB.
 * service_id and method_id are encoded as 12-bit fields.
 *
 * @param conn       Connection handle
 * @param service_id Target service
 * @param method_id  Target method
 * @param data       Request payload
 * @param len        Payload length in bytes
 * @return           request_id in [1, 65535], or -1
 */
int cxl_send_request(cxl_connection_t *conn,
                     uint16_t service_id,
                     uint16_t method_id,
                     const void *data,
                     size_t len);

/**
 * Read completion flag (latest completed request_id) from peer.
 *
 * This is a non-blocking flag probe. The returned request_id is the latest
 * completed response id visible in the single-flag protocol.
 *
 * @param conn            Connection handle
 * @param out_request_id  Out: latest request_id from flag
 * @return                1 if flag is non-zero, 0 if no completion yet, -1 on error
 */
int cxl_peek_latest_completed_request_id(cxl_connection_t *conn,
                                         uint16_t *out_request_id);

/**
 * Consume the next response entry in producer order (non-blocking).
 *
 * Copies exactly one response payload from the current response-data head into
 * caller-owned local memory, then advances the consumer offset.
 *
 * This API is intended for single-flag multi-outstanding workflows where the
 * client first calls `cxl_peek_latest_completed_request_id()`, checks the
 * returned request_id against its pending set, and then drains response_data
 * in producer order until that request_id is consumed.
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
 * @param out_request_id  Out: consumed response request_id
 * @return                1 if one response consumed, 0 if pending, -1 on error
 */
int cxl_consume_next_response(cxl_connection_t *conn,
                              void *out_data,
                              size_t *out_len,
                              uint16_t *out_request_id);

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
 * @param service_id Out: service ID from request
 * @param method_id  Out: method ID from request
 * @param request_id Out: request ID
 * Payload view semantics:
 *   - Inline request: out_data_view points to metadata entry bytes [8..15].
 *   - Non-inline request: payload is first copied into a connection-local
 *     staging buffer, then out_data_view points to that local buffer.
 *   Lifetime is valid until this connection performs the next request poll.
 *
 * @param out_data_view Out: payload view pointer (inline or request_data)
 * @param out_len       Out: exact payload size from metadata
 * @return           1 if request available, 0 if empty, -1 on error
 */
int cxl_poll_request(cxl_connection_t *conn,
                     uint16_t *service_id,
                     uint16_t *method_id,
                     uint16_t *request_id,
                     const void **out_data_view,
                     size_t *out_len);

/**
 * Send an RPC response (server-side).
 *
 * Sends one response entry to peer `response_data` and then publishes the
 * corresponding `request_id` to peer `flag` through CopyEngine.
 *
 * @param conn       Server-side connection handle (with peer addresses set)
 * @param request_id Request ID to respond to (must be 1..65535)
 * @param data       Response payload
 * @param len        Payload length in bytes (current backend max: 4088)
 * @return           0 on success, -1 on error
 */
int cxl_send_response(cxl_connection_t *conn,
                      uint16_t request_id,
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
