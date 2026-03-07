/*
 * CXL RPC Core Library
 *
 * Provides user-space API for CXL shared memory RPC:
 *   - Context: KM NUMA-backed allocation (no /dev/cxl_mem0 or /dev/mem)
 *   - Connection: allocated via global allocator, managed regions
 *   - Client: send_request (shared store publish path) + poll_response (flag)
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

/* CXL memory default base address (16GB boundary) */
#define CXL_BASE_DEFAULT        0x400000000ULL
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
 * Response entry header layout (12 bytes):
 *   bytes 0-3:  payload_len (uint32 LE)
 *   bytes 4-7:  request_id  (uint32 LE)
 *   bytes 8-11: response_id (uint32 LE)
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
 * Use for direct memory access (offset from physical base).
 */
volatile void *cxl_rpc_get_base(const cxl_context_t *ctx);

/**
 * Convert physical address to virtual pointer.
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
 * Configure request_id prefix bits for this connection.
 *
 * request_id is carried as 16 bits in metadata. With `prefix_bits > 0`,
 * send_request() encodes:
 *   request_id = (prefix << (16 - prefix_bits)) | random_low_bits
 * where random_low_bits is non-zero.
 *
 * This allows N-client:1-server routing (server derives client_id from
 * request_id) and works for both inline/non-inline requests.
 *
 * @param conn         Connection handle
 * @param prefix       Prefix value (typically client_id)
 * @param prefix_bits  Number of high bits used as prefix (0..15)
 * @return             0 on success, -1 on invalid config
 */
int cxl_connection_set_request_id_prefix(cxl_connection_t *conn,
                                          uint16_t prefix,
                                          uint8_t prefix_bits);

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
 * @return           request_id (>= 0) or -1 on error
 */
int32_t cxl_send_request(cxl_connection_t *conn,
                          uint16_t service_id,
                          uint16_t method_id,
                          const void *data,
                          size_t len);

/**
 * Poll for response (non-blocking).
 *
 * Drains response ring in producer order from the current read offset.
 * Returns ready only when flag reports exactly the requested request_id;
 * otherwise returns pending (0) without loading response_data.
 *
 * @param conn       Connection handle
 * @param request_id Requested response ID (must be > 0)
 * @param out_data   Buffer for response payload
 * @param out_len    In: buffer size, Out: exact response payload size
 * @return           1 if response ready, 0 if pending, -1 on error
 */
int cxl_poll_response(cxl_connection_t *conn,
                       int32_t request_id,
                       void *out_data,
                       size_t *out_len);

/**
 * Poll for response (non-blocking, zero-copy view).
 *
 * On success, out_data_view points directly to payload bytes in shared memory
 * (response_data entry body after 12B header), avoiding an extra local memcpy.
 * Drains response ring in producer order from the current read offset.
 * Returns ready only when flag reports exactly the requested request_id;
 * otherwise returns pending (0) without loading response_data.
 *
 * @param conn          Connection handle
 * @param request_id    Requested response ID (must be > 0)
 * @param out_data_view Out: payload view pointer
 * @param out_len       Out: exact payload size
 * @param out_response_request_id Out: response header request_id (optional)
 * @return              1 if response ready, 0 if pending, -1 on error
 */
int cxl_poll_response_view(cxl_connection_t *conn,
                            int32_t request_id,
                            const void **out_data_view,
                            size_t *out_len,
                            uint32_t *out_response_request_id);

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
                      uint32_t *request_id,
                      const void **out_data_view,
                      size_t *out_len);

/**
 * Send an RPC response (server-side).
 *
 * Writes response entry to peer's response_data region, then updates
 * peer's flag with request_id using the standard store+flush+fence publish
 * sequence.
 *
 * @param conn       Server-side connection handle (with peer addresses set)
 * @param request_id Request ID to respond to
 * @param data       Response payload
 * @param len        Payload length in bytes
 * @return           0 on success, -1 on error
 */
int cxl_send_response(cxl_connection_t *conn,
                      uint32_t request_id,
                      const void *data,
                      size_t len);

/**
 * Set peer addresses for server sending responses.
 *
 * Must be called after connection creation, before send_response.
 * Maps peer response_data into the server virtual address space.
 * Peer flag is configured separately via cxl_connection_set_peer_flag_addr().
 *
 * @param conn                   Connection handle
 * @param peer_response_data_addr Physical address of peer's response_data
 * @param peer_response_data_size Size of peer's response_data region
 * @return                       0 on success, -1 on error
 */
int cxl_connection_set_peer_addrs(cxl_connection_t *conn,
                                   uint64_t peer_response_data_addr,
                                   size_t peer_response_data_size);

/**
 * Set peer flag address for server response completion publish.
 *
 * The server writes request_id to peer flag only after response_data publish
 * has completed, then flushes + fences the flag store.
 *
 * @param conn            Connection handle
 * @param peer_flag_addr  Physical address of peer's flag region
 * @return                0 on success, -1 on error
 */
int cxl_connection_set_peer_flag_addr(cxl_connection_t *conn,
                                       uint64_t peer_flag_addr);

/**
 * Set peer request_data address range for strict server-side request parsing.
 *
 * `cxl_poll_request()` accepts non-inline request pointers only if they fall in
 * either:
 *   1) this connection's own request_data range, or
 *   2) this configured peer request_data range.
 *
 * @param conn                    Connection handle
 * @param peer_request_data_addr  Physical address of peer's request_data
 * @param peer_request_data_size  Size of peer's request_data region
 * @return                        0 on success, -1 on error
 */
int cxl_connection_set_peer_request_data(cxl_connection_t *conn,
                                          uint64_t peer_request_data_addr,
                                          size_t peer_request_data_size);

#ifdef __cplusplus
}
#endif

#endif /* CXL_RPC_H */
