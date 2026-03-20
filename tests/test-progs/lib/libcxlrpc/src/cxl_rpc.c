/*
 * CXL RPC Core Library - Implementation
 *
 * Provides context management (KM NUMA-backed allocation),
 * connection lifecycle, client send/poll, and server poll/send.
 */

#include "cxl_rpc_internal.h"

#include <assert.h>
#include <numa.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>

static inline int
cxl_env_flag_enabled(const char *name)
{
    const char *env = getenv(name);
    return (env && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
}

static int
cxl_lib_debug_enabled(void)
{
    static int initialized = 0;
    static int enabled = 0;
    if (!initialized) {
        const char *lib_debug = getenv("CXL_RPC_LIB_DEBUG");
        if (lib_debug) {
            enabled = cxl_env_flag_enabled("CXL_RPC_LIB_DEBUG");
        } else {
            enabled = cxl_env_flag_enabled("CXL_RPC_PROGRESS_DEBUG");
        }
        initialized = 1;
    }
    return enabled;
}

static int
cxl_lib_trace_polls_enabled(void)
{
    static int initialized = 0;
    static int enabled = 0;
    if (!initialized) {
        enabled = cxl_env_flag_enabled("CXL_RPC_LIB_TRACE_POLLS");
        initialized = 1;
    }
    return enabled;
}

static void
cxl_lib_debugf(const char *fmt, ...)
{
    if (!cxl_lib_debug_enabled())
        return;

    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[libcxlrpc][DBG] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void
cxl_lib_errorf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[libcxlrpc][ERR] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* MQ policy is fixed: keep prefetch + invalidate policies always on. */
#define CXL_MQ_PREFETCH_LINES 16u
#define CXL_MQ_INVALIDATE_CONSUMED 1
#define CXL_MQ_INVALIDATE_PREFETCHED 1
#define CXL_BOOTSTRAP_LEN_MAGIC 0xFFFF0000u
#define CXL_BOOTSTRAP_OP_REGISTER_DOORBELL 0x0001u
#define CXL_BOOTSTRAP_OP_REGISTER_METADATA_PAGE_BASE 0x0100u

static inline size_t
cxl_response_entry_size(size_t payload_len)
{
    const size_t total_size = CXL_RESP_HEADER_LEN + payload_len;
    return ((total_size + 63) / 64) * 64;
}

static inline size_t
cxl_request_entry_size(size_t payload_len)
{
    return ((payload_len + 63) / 64) * 64;
}

static inline void
cxl_invalidate_cacheline(volatile uint8_t *line_addr)
{
    if (!line_addr)
        return;
    __asm__ __volatile__(
        "clflushopt (%0)"
        :
        : "r"((void *)line_addr)
        : "memory");
}

static inline void
cxl_invalidate_load_barrier(void)
{
    /*
     * Read-side invalidate paths must not let the subsequent load observe a
     * stale cacheline before the invalidate has completed. These call sites
     * only need a load barrier after clflushopt, not a full read/write fence.
     */
    __asm__ __volatile__("lfence" ::: "memory");
}

static void
cxl_flush_range(volatile uint8_t *ptr, size_t size)
{
    if (!ptr || size == 0)
        return;

    uintptr_t line_start = ((uintptr_t)ptr) & ~((uintptr_t)63);
    uintptr_t line_end = (((uintptr_t)ptr + size) + 63u) & ~((uintptr_t)63);
    for (uintptr_t line = line_start; line < line_end; line += 64u) {
        __asm__ __volatile__(
            "clflushopt (%0)"
            :
            : "r"((void *)line)
            : "memory");
    }
    __asm__ __volatile__("sfence" ::: "memory");
}

static void
cxl_zero_and_flush_range(volatile uint8_t *ptr, size_t size)
{
    if (!ptr || size == 0)
        return;

    memset((void *)ptr, 0, size);
    cxl_flush_range(ptr, size);
}

static inline void
cxl_mq_invalidate_lines(volatile uint8_t *metadata_queue,
                        uint32_t start_line,
                        uint32_t nr_lines,
                        uint32_t total_lines)
{
    if (!metadata_queue || nr_lines == 0 || total_lines == 0)
        return;

    if (nr_lines > total_lines)
        nr_lines = total_lines;

    for (uint32_t i = 0; i < nr_lines; i++) {
        uint32_t line = (start_line + i) % total_lines;
        cxl_invalidate_cacheline(metadata_queue + ((size_t)line * 64u));
    }
    __asm__ __volatile__("sfence" ::: "memory");
}

static int
cxl_rpc_virt_to_phys_local(const void *vaddr, uint64_t *phys_out)
{
    static int pagemap_fd = -2;
    static uint64_t page_size = 0;
    uint64_t entry = 0;
    uint64_t virt = (uint64_t)(uintptr_t)vaddr;

    if (!phys_out)
        return -1;

    if (page_size == 0) {
        long ps = sysconf(_SC_PAGESIZE);
        if (ps <= 0)
            return -1;
        page_size = (uint64_t)ps;
    }

    if (pagemap_fd == -2) {
        pagemap_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        if (pagemap_fd < 0)
            return -1;
    }
    if (pagemap_fd < 0)
        return -1;

    off_t off = (off_t)((virt / page_size) * sizeof(uint64_t));
    ssize_t n = pread(pagemap_fd, &entry, sizeof(entry), off);
    if (n != (ssize_t)sizeof(entry))
        return -1;

    if ((entry & (1ULL << 63)) == 0)
        return -1;

    uint64_t pfn = entry & ((1ULL << 55) - 1);
    *phys_out = pfn * page_size + (virt % page_size);
    return 0;
}

static void
cxl_publish_doorbell_raw(volatile uint8_t *doorbell, const uint8_t *doorbell_buf)
{
    if (!doorbell || !doorbell_buf)
        return;

    memcpy((void *)doorbell, doorbell_buf, CXL_DOORBELL_PUBLISH_LEN);
    cxl_flush_range(doorbell, CXL_DOORBELL_PUBLISH_LEN);
}

/* ================================================================
 * Low-level Utilities
 * ================================================================ */

static void cxl_build_doorbell(uint8_t *buf, uint8_t method, uint8_t is_inline,
                               uint32_t length, uint16_t node_id,
                               uint16_t rpc_id, uint64_t data)
{
    if (!buf)
        return;

    /* Fill all 16 bytes directly: [0..7]=header, [8..15]=data (LE). */
    uint64_t header_lo = 0;
    header_lo |= ((uint64_t)(method & 0x01u));
    header_lo |= ((uint64_t)(is_inline & 0x01u) << 1);
    header_lo |= ((uint64_t)length << 3);
    header_lo |= ((uint64_t)(node_id & CXL_NODE_ID_MASK) << 35);
    header_lo |= ((uint64_t)(rpc_id & CXL_RPC_ID_MASK) << 49);

    memcpy(buf, &header_lo, sizeof(header_lo));
    memcpy(buf + 8, &data, sizeof(data));
}

static int
cxl_send_controller_bootstrap_doorbell(cxl_connection_t *conn,
                                       uint32_t bootstrap_len,
                                       uint64_t data)
{
    uint8_t doorbell_buf[16] __attribute__((aligned(16)));
    uint64_t header_lo = 0;

    if (!conn || !conn->doorbell)
        return -1;

    cxl_build_doorbell(doorbell_buf, CXL_METHOD_REQUEST, 1,
                       bootstrap_len, conn->addrs.node_id, 0, data);
    memcpy(&header_lo, doorbell_buf, sizeof(header_lo));
    cxl_lib_debugf("bootstrap publish doorbell=%p node_id=%u len=%#x "
                   "header_lo=%#llx data=%#llx",
                   (void *)conn->doorbell,
                   (unsigned)conn->addrs.node_id,
                   (unsigned)bootstrap_len,
                   (unsigned long long)header_lo,
                   (unsigned long long)data);
    cxl_publish_doorbell_raw(conn->doorbell, doorbell_buf);
    return 0;
}

static int
cxl_bootstrap_controller_translation(cxl_connection_t *conn,
                                     size_t metadata_clear_size)
{
    uint64_t observed_doorbell = 0;

    if (!conn || !conn->doorbell || !conn->metadata_queue)
        return -1;

    if (cxl_rpc_virt_to_phys_local((const void *)conn->doorbell,
                                   &observed_doorbell) == 0) {
        cxl_lib_debugf("bootstrap observed doorbell gpa=%#llx logical=%#llx "
                       "metadata=%#llx node_id=%u",
                       (unsigned long long)observed_doorbell,
                       (unsigned long long)conn->addrs.doorbell_addr,
                       (unsigned long long)conn->addrs.metadata_queue_addr,
                       (unsigned)conn->addrs.node_id);
    } else {
        cxl_lib_debugf("bootstrap doorbell gpa unavailable before first "
                       "publish logical=%#llx node_id=%u",
                       (unsigned long long)conn->addrs.doorbell_addr,
                       (unsigned)conn->addrs.node_id);
    }

    if (cxl_send_controller_bootstrap_doorbell(
            conn,
            CXL_BOOTSTRAP_LEN_MAGIC | CXL_BOOTSTRAP_OP_REGISTER_DOORBELL,
            0) != 0) {
        return -1;
    }

    size_t page_count =
        (metadata_clear_size + CXL_METADATA_TRANSLATION_PAGE_BYTES - 1u) /
        CXL_METADATA_TRANSLATION_PAGE_BYTES;
    for (size_t page_index = 0; page_index < page_count; ++page_index) {
        volatile uint8_t *page_ptr =
            conn->metadata_queue +
            page_index * CXL_METADATA_TRANSLATION_PAGE_BYTES;
        uint64_t observed_page = 0;
        if (cxl_rpc_virt_to_phys_local((const void *)page_ptr,
                                       &observed_page) != 0) {
            cxl_lib_errorf("bootstrap failed to resolve metadata page gpa "
                           "page=%zu logical=%#llx",
                           page_index,
                           (unsigned long long)(conn->addrs.metadata_queue_addr +
                               page_index * CXL_METADATA_TRANSLATION_PAGE_BYTES));
            return -1;
        }
        observed_page &= ~((uint64_t)CXL_METADATA_TRANSLATION_PAGE_BYTES - 1u);
        cxl_lib_debugf("bootstrap metadata page=%zu logical=%#llx "
                       "observed=%#llx",
                       page_index,
                       (unsigned long long)(conn->addrs.metadata_queue_addr +
                           page_index * CXL_METADATA_TRANSLATION_PAGE_BYTES),
                       (unsigned long long)observed_page);

        if (cxl_send_controller_bootstrap_doorbell(
                conn,
                CXL_BOOTSTRAP_LEN_MAGIC |
                    (CXL_BOOTSTRAP_OP_REGISTER_METADATA_PAGE_BASE |
                     (uint32_t)page_index),
                observed_page) != 0) {
            return -1;
        }
    }

    cxl_lib_debugf("controller bootstrap complete doorbell=%#llx metadata=%#llx "
                   "pages=%zu node_id=%u",
                   (unsigned long long)conn->addrs.doorbell_addr,
                   (unsigned long long)conn->addrs.metadata_queue_addr,
                   page_count,
                   (unsigned)conn->addrs.node_id);
    return 0;
}

/* ================================================================
 * Context Management
 * ================================================================ */

static int
cxl_parse_env_int(const char *name, int default_val, int min_val, int max_val)
{
    const char *value = getenv(name);
    if (!value || value[0] == '\0')
        return default_val;

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (!end || *end != '\0' || parsed < min_val || parsed > max_val) {
        fprintf(stderr, "cxl_rpc_init: invalid %s='%s'\n", name, value);
        return -1;
    }

    return (int)parsed;
}

static int
cxl_map_via_shared_numa(cxl_context_t *ctx, size_t cxl_size)
{
    const char *name_env = getenv("CXL_RPC_SHM_NAME");
    const char *shm_name = (name_env && name_env[0] != '\0')
        ? name_env : "/cxl_rpc_region";

    int fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        fprintf(stderr, "cxl_rpc_init: shm_open(%s) failed: %s\n",
                shm_name, strerror(errno));
        return -1;
    }

    if (ftruncate(fd, (off_t)cxl_size) != 0) {
        fprintf(stderr, "cxl_rpc_init: ftruncate(%s, %zu) failed: %s\n",
                shm_name, cxl_size, strerror(errno));
        close(fd);
        return -1;
    }

    void *mapped = mmap(NULL, cxl_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        fprintf(stderr, "cxl_rpc_init: mmap(shared) failed: %s\n",
                strerror(errno));
        close(fd);
        return -1;
    }

    ctx->base = (volatile uint8_t *)mapped;
    ctx->shm_fd = fd;
    strncpy(ctx->shm_name, shm_name, sizeof(ctx->shm_name) - 1);
    ctx->shm_name[sizeof(ctx->shm_name) - 1] = '\0';

    /*
     * Keep NUMA binding semantics: prefer CXL_RPC_NUMA_NODE pages for this
     * shared region. If binding fails, keep running and report once.
     */
    errno = 0;
    numa_tonode_memory((void *)ctx->base, cxl_size, ctx->numa_node);
    if (errno != 0) {
        fprintf(stderr,
                "cxl_rpc_init: warning: numa_tonode_memory(node=%d) errno=%d (%s)\n",
                ctx->numa_node, errno, strerror(errno));
    }

    const char *clear_on_init = getenv("CXL_RPC_CLEAR_ON_INIT");
    if (clear_on_init && clear_on_init[0] != '\0' &&
        strcmp(clear_on_init, "0") != 0) {
        memset((void *)ctx->base, 0, cxl_size);
    }

    return 0;
}

static int
cxl_map_via_km_numa(cxl_context_t *ctx, size_t cxl_size)
{
    int numa_node = cxl_parse_env_int("CXL_RPC_NUMA_NODE", 1, 0, 63);
    if (numa_node < 0)
        return -1;
    ctx->numa_node = numa_node;

    if (numa_available() < 0) {
        fprintf(stderr, "cxl_rpc_init: NUMA API unavailable\n");
        return -1;
    }

    return cxl_map_via_shared_numa(ctx, cxl_size);
}

cxl_context_t *cxl_rpc_init(uint64_t cxl_base, size_t cxl_size)
{
    if (cxl_base == 0) cxl_base = CXL_BASE_DEFAULT;
    if (cxl_size == 0) cxl_size = CXL_SIZE_DEFAULT;

    cxl_context_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->shm_fd = -1;

    if (cxl_map_via_km_numa(ctx, cxl_size) != 0) {
        free(ctx);
        return NULL;
    }

    ctx->phys_base = cxl_base;
    ctx->map_size = cxl_size;

    return ctx;
}

void cxl_rpc_destroy(cxl_context_t *ctx)
{
    if (!ctx)
        return;

    if (ctx->base && ctx->shm_fd >= 0) {
        munmap((void *)ctx->base, ctx->map_size);
        close(ctx->shm_fd);
        const char *unlink_on_destroy = getenv("CXL_RPC_SHM_UNLINK_ON_DESTROY");
        if (unlink_on_destroy && unlink_on_destroy[0] != '\0' &&
            strcmp(unlink_on_destroy, "0") != 0 &&
            ctx->shm_name[0] != '\0') {
            shm_unlink(ctx->shm_name);
        }
    }

    free(ctx);
}

volatile void *cxl_rpc_get_base(const cxl_context_t *ctx)
{
    return ctx ? ctx->base : NULL;
}

volatile void *cxl_rpc_phys_to_virt(const cxl_context_t *ctx,
                                     uint64_t phys_addr)
{
    if (!ctx || phys_addr < ctx->phys_base)
        return NULL;

    uint64_t offset = phys_addr - ctx->phys_base;
    if (offset >= ctx->map_size)
        return NULL;

    return ctx->base + offset;
}

static int cxl_rpc_phys_range_valid(const cxl_context_t *ctx,
                                     uint64_t phys_addr,
                                     size_t size)
{
    if (!ctx || size == 0 || phys_addr < ctx->phys_base)
        return 0;

    uint64_t offset = phys_addr - ctx->phys_base;
    uint64_t map_size = (uint64_t)ctx->map_size;
    uint64_t size64 = (uint64_t)size;

    if (offset > map_size)
        return 0;
    if (size64 > map_size - offset)
        return 0;

    return 1;
}

/* ================================================================
 * Connection Management
 * ================================================================ */

static inline int
cxl_req_id_inflight_test(const cxl_connection_t *conn, uint16_t req_id)
{
    return ((conn->rpc_id_inflight_bitmap[req_id >> 6] >>
             (req_id & 0x3Fu)) & 0x1ULL) != 0;
}

static inline void
cxl_req_id_inflight_set(cxl_connection_t *conn, uint16_t req_id)
{
    conn->rpc_id_inflight_bitmap[req_id >> 6] |= (1ULL << (req_id & 0x3Fu));
}

static inline void
cxl_req_id_inflight_clear(cxl_connection_t *conn, uint16_t req_id)
{
    conn->rpc_id_inflight_bitmap[req_id >> 6] &= ~(1ULL << (req_id & 0x3Fu));
}

static void cxl_req_id_allocator_release(cxl_connection_t *conn,
                                         uint16_t req_id);

static int
cxl_connection_state_arrays_init(cxl_connection_t *conn)
{
    if (!conn)
        return -1;

    if (!conn->rpc_id_inflight_bitmap) {
        conn->rpc_id_inflight_bitmap =
            (uint64_t *)calloc((size_t)CXL_RPC_ID_BITMAP_WORDS,
                               sizeof(uint64_t));
    }
    if (!conn->req_entry_offsets) {
        conn->req_entry_offsets =
            (uint32_t *)calloc((size_t)CXL_RPC_ID_SPACE, sizeof(uint32_t));
    }
    if (!conn->req_entry_sizes) {
        conn->req_entry_sizes =
            (uint32_t *)calloc((size_t)CXL_RPC_ID_SPACE, sizeof(uint32_t));
    }
    if (!conn->req_entry_next) {
        conn->req_entry_next =
            (uint16_t *)calloc((size_t)CXL_RPC_ID_SPACE, sizeof(uint16_t));
    }
    if (!conn->req_entry_complete) {
        conn->req_entry_complete =
            (uint8_t *)calloc((size_t)CXL_RPC_ID_SPACE, sizeof(uint8_t));
    }

    if (!conn->rpc_id_inflight_bitmap || !conn->req_entry_offsets ||
        !conn->req_entry_sizes || !conn->req_entry_next ||
        !conn->req_entry_complete) {
        return -1;
    }

    return 0;
}

static void
cxl_request_ring_state_reset(cxl_connection_t *conn)
{
    if (!conn)
        return;

    if (conn->req_entry_offsets) {
        memset(conn->req_entry_offsets, 0,
               (size_t)CXL_RPC_ID_SPACE * sizeof(uint32_t));
    }
    if (conn->req_entry_sizes) {
        memset(conn->req_entry_sizes, 0,
               (size_t)CXL_RPC_ID_SPACE * sizeof(uint32_t));
    }
    if (conn->req_entry_next) {
        memset(conn->req_entry_next, 0,
               (size_t)CXL_RPC_ID_SPACE * sizeof(uint16_t));
    }
    if (conn->req_entry_complete) {
        memset(conn->req_entry_complete, 0,
               (size_t)CXL_RPC_ID_SPACE * sizeof(uint8_t));
    }

    conn->req_ring_head_id = 0;
    conn->req_ring_tail_id = 0;
    conn->req_reclaim_offset = 0;
    conn->req_ring_used_bytes = 0;
    conn->req_write_offset = 0;
}

static int
cxl_req_id_allocator_init(cxl_connection_t *conn)
{
    uint32_t response_slots = 0;

    if (!conn)
        return -1;

    if (cxl_connection_state_arrays_init(conn) != 0)
        return -1;

    memset(conn->rpc_id_inflight_bitmap, 0,
           (size_t)CXL_RPC_ID_BITMAP_WORDS * sizeof(uint64_t));
    cxl_request_ring_state_reset(conn);

    conn->rpc_id_seq_mask = (uint16_t)CXL_RPC_ID_MASK;
    conn->rpc_id_next = 1u;
    conn->rpc_id_inflight_count = 0;

    response_slots =
        (uint32_t)(conn->addrs.response_data_size / CXL_RESP_SLOT_BYTES);
    if (response_slots == 0)
        response_slots = (uint32_t)CXL_RPC_ID_MASK;
    conn->rpc_id_capacity =
        ((uint32_t)CXL_RPC_ID_MASK < response_slots) ?
        (uint32_t)CXL_RPC_ID_MASK : response_slots;

    return (conn->rpc_id_capacity > 0) ? 0 : -1;
}

static int
cxl_request_ring_reserve(cxl_connection_t *conn,
                         uint16_t req_id,
                         size_t entry_size,
                         size_t *out_offset)
{
    size_t offset = 0;
    size_t region_size = 0;

    if (!conn || !out_offset || req_id == 0 || entry_size == 0)
        return -1;

    region_size = conn->addrs.request_data_size;
    if (region_size == 0 || entry_size > region_size)
        return -1;
    if ((region_size - conn->req_ring_used_bytes) < entry_size)
        return -1;

    if (conn->req_ring_used_bytes == 0) {
        offset = 0;
    } else {
        offset = conn->req_write_offset;
        if (offset >= conn->req_reclaim_offset) {
            size_t tail_free = region_size - offset;
            if (entry_size > tail_free) {
                if (entry_size > conn->req_reclaim_offset)
                    return -1;
                offset = 0;
            }
        } else if (entry_size > (conn->req_reclaim_offset - offset)) {
            return -1;
        }
    }

    conn->req_entry_offsets[req_id] = (uint32_t)offset;
    conn->req_entry_sizes[req_id] = (uint32_t)entry_size;
    conn->req_entry_next[req_id] = 0;
    conn->req_entry_complete[req_id] = 0;

    if (conn->req_ring_tail_id != 0) {
        conn->req_entry_next[conn->req_ring_tail_id] = req_id;
    } else {
        conn->req_ring_head_id = req_id;
        conn->req_reclaim_offset = offset;
    }
    conn->req_ring_tail_id = req_id;
    conn->req_ring_used_bytes += entry_size;
    conn->req_write_offset = offset + entry_size;
    if (conn->req_write_offset >= region_size)
        conn->req_write_offset = 0;

    *out_offset = offset;
    return 0;
}

static void
cxl_request_ring_mark_complete(cxl_connection_t *conn, uint16_t req_id)
{
    if (req_id == 0 || conn->req_entry_sizes[req_id] == 0)
        return;

    conn->req_entry_complete[req_id] = 1;

    while (conn->req_ring_head_id != 0) {
        uint16_t head_id = conn->req_ring_head_id;
        uint16_t next_id = conn->req_entry_next[head_id];
        size_t entry_size = conn->req_entry_sizes[head_id];
        size_t entry_offset = conn->req_entry_offsets[head_id];

        if (!conn->req_entry_complete[head_id])
            break;

        conn->req_ring_used_bytes -= entry_size;

        conn->req_reclaim_offset = entry_offset + entry_size;
        if (conn->req_reclaim_offset >= conn->addrs.request_data_size)
            conn->req_reclaim_offset = 0;

        conn->req_entry_offsets[head_id] = 0;
        conn->req_entry_sizes[head_id] = 0;
        conn->req_entry_next[head_id] = 0;
        conn->req_entry_complete[head_id] = 0;
        conn->req_ring_head_id = next_id;
        if (next_id == 0)
            conn->req_ring_tail_id = 0;
    }

    if (conn->req_ring_head_id == 0 && conn->req_ring_used_bytes == 0) {
        conn->req_reclaim_offset = 0;
        conn->req_write_offset = 0;
    }
}

static int
cxl_req_id_allocator_alloc(cxl_connection_t *conn, uint16_t *out_req_id)
{
    if (!conn || !out_req_id || !conn->rpc_id_inflight_bitmap)
        return -1;

    if (conn->rpc_id_inflight_count >= conn->rpc_id_capacity)
        return -1;

    const uint16_t seq_mask = conn->rpc_id_seq_mask;
    uint16_t seq = (uint16_t)(conn->rpc_id_next & seq_mask);
    const uint32_t scan_span = (uint32_t)seq_mask + 1u;

    for (uint32_t i = 0; i < scan_span; i++) {
        const uint16_t req_id = seq;
        seq = (uint16_t)((seq + 1u) & seq_mask);
        if (req_id == 0)
            continue;
        if (cxl_req_id_inflight_test(conn, req_id))
            continue;

        cxl_req_id_inflight_set(conn, req_id);
        conn->rpc_id_inflight_count++;
        conn->rpc_id_next = (seq == 0) ? 1u : seq;
        *out_req_id = req_id;
        return 0;
    }

    return -1;
}

static void
cxl_req_id_allocator_release(cxl_connection_t *conn, uint16_t req_id)
{
    if (!conn || req_id == 0)
        return;

    if (!cxl_req_id_inflight_test(conn, req_id))
        return;

    cxl_req_id_inflight_clear(conn, req_id);
    conn->rpc_id_inflight_count--;
}

static void connection_init_virt_ptrs(cxl_connection_t *conn)
{
    conn->doorbell = cxl_rpc_phys_to_virt(conn->ctx, conn->addrs.doorbell_addr);
    conn->metadata_queue = cxl_rpc_phys_to_virt(conn->ctx,
                                                  conn->addrs.metadata_queue_addr);
    conn->request_data = cxl_rpc_phys_to_virt(conn->ctx,
                                                conn->addrs.request_data_addr);
    conn->response_data = cxl_rpc_phys_to_virt(conn->ctx,
                                                 conn->addrs.response_data_addr);
    conn->flag = cxl_rpc_phys_to_virt(conn->ctx, conn->addrs.flag_addr);
}

static void
cxl_prefault_each_page_ro_and_flush_touched(volatile uint8_t *ptr, size_t size)
{
    if (!ptr || size == 0)
        return;

    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) {
        volatile uint8_t v = ptr[0];
        (void)v;
        cxl_invalidate_cacheline((volatile uint8_t *)
                                 (((uintptr_t)ptr) & ~((uintptr_t)63)));
        __asm__ __volatile__("sfence" ::: "memory");
        return;
    }

    size_t page_size = (size_t)ps;
    uintptr_t last_flushed_line = ~(uintptr_t)0;
    for (size_t off = 0; off < size; off += page_size) {
        volatile uint8_t v = ptr[off];
        (void)v;
        uintptr_t line = (((uintptr_t)(ptr + off)) & ~((uintptr_t)63));
        if (line != last_flushed_line) {
            cxl_invalidate_cacheline((volatile uint8_t *)line);
            last_flushed_line = line;
        }
    }

    volatile uint8_t tail = ptr[size - 1];
    (void)tail;
    uintptr_t tail_line = (((uintptr_t)(ptr + size - 1)) & ~((uintptr_t)63));
    if (tail_line != last_flushed_line)
        cxl_invalidate_cacheline((volatile uint8_t *)tail_line);
    __asm__ __volatile__("sfence" ::: "memory");
}

static void
cxl_advance_mq_head(cxl_connection_t *conn)
{
    if (!conn || conn->mq_entries == 0)
        return;

    conn->mq_head++;
    if (conn->mq_head >= conn->mq_entries) {
        conn->mq_head = 0;
        conn->mq_phase ^= 1;  /* Toggle expected phase */
    }

    if (conn->sync)
        cxl_sync_entry_processed(conn->sync, conn->mq_head);
}

static void
cxl_advance_mq_head_with_policy(cxl_connection_t *conn,
                                uint32_t consumed_head_idx,
                                uint32_t consumed_line_idx,
                                uint32_t mq_entries_per_line,
                                uint32_t mq_total_lines,
                                int invalidate_consumed)
{
    if (!conn)
        return;

    if (invalidate_consumed &&
        mq_entries_per_line > 0 &&
        mq_total_lines > 0 &&
        conn->mq_entries > 0) {
        uint32_t next_head = consumed_head_idx + 1;
        if (next_head >= conn->mq_entries)
            next_head = 0;
        uint32_t next_line_idx = next_head / mq_entries_per_line;
        if (next_line_idx != consumed_line_idx) {
            /*
             * Invalidate consumed metadata cacheline once all entries in this
             * line are drained, so future prefetch can fetch producer updates.
             */
            cxl_mq_invalidate_lines(conn->metadata_queue,
                                    consumed_line_idx,
                                    1,
                                    mq_total_lines);
        }
    }

    cxl_advance_mq_head(conn);
}

static int
connection_finalize_setup(cxl_connection_t *conn,
                          size_t metadata_clear_size,
                          int bootstrap_owner)
{
    if (!conn)
        return -1;

    connection_init_virt_ptrs(conn);
    cxl_prefault_each_page_ro_and_flush_touched(conn->metadata_queue,
                                                metadata_clear_size);
    cxl_prefault_each_page_ro_and_flush_touched(conn->response_data,
                                                conn->addrs.response_data_size);
    cxl_prefault_each_page_ro_and_flush_touched(conn->flag,
                                                CXL_DEFAULT_FLAG_SIZE);

    const int do_destructive_bootstrap = bootstrap_owner;

    /*
     * Owner/bootstrap paths reset the completion flag. Attach paths must not
     * destroy already-published completions on a live shared connection.
     */
    if (do_destructive_bootstrap)
        cxl_zero_and_flush_range(conn->flag, CXL_DEFAULT_FLAG_SIZE);

    if (do_destructive_bootstrap && conn->metadata_queue && metadata_clear_size > 0) {
        cxl_zero_and_flush_range(conn->metadata_queue, metadata_clear_size);
    }


    if (conn->doorbell) {
        cxl_sync_config_t sync_cfg = {
            .queue_size_n = conn->mq_entries,
	    };
        conn->sync = cxl_sync_init(&sync_cfg, (volatile void *)conn->doorbell);
    }

    if (do_destructive_bootstrap &&
        cxl_bootstrap_controller_translation(conn, metadata_clear_size) != 0) {
        cxl_lib_errorf("connection bootstrap controller translation failed "
                       "doorbell=%#llx metadata=%#llx",
                       (unsigned long long)conn->addrs.doorbell_addr,
                       (unsigned long long)conn->addrs.metadata_queue_addr);
        return -1;
    }

    return 0;
}

static cxl_connection_t *
cxl_connection_create_fixed_internal(cxl_context_t *ctx,
                                     const cxl_connection_addrs_t *addrs,
                                     uint32_t mq_entries,
                                     int bootstrap_owner)
{
    if (!ctx || !addrs)
        return NULL;

    if (mq_entries == 0) mq_entries = 1024;
    size_t required_mq_size = (size_t)mq_entries * CXL_METADATA_ENTRY_SIZE;

    if (addrs->doorbell_addr == 0 ||
        addrs->metadata_queue_addr == 0 ||
        addrs->flag_addr == 0 ||
        addrs->node_id > CXL_NODE_ID_MASK ||
        addrs->metadata_queue_size < required_mq_size) {
        cxl_lib_errorf("connection_create_fixed(%s): invalid addrs doorbell=%#llx metadata=%#llx flag=%#llx node_id=%u metadata_size=%zu required=%zu",
                       bootstrap_owner ? "owner" : "attach",
                       (unsigned long long)addrs->doorbell_addr,
                       (unsigned long long)addrs->metadata_queue_addr,
                       (unsigned long long)addrs->flag_addr,
                       (unsigned)addrs->node_id,
                       addrs->metadata_queue_size,
                       required_mq_size);
        return NULL;
    }

    if (!cxl_rpc_phys_range_valid(ctx, addrs->doorbell_addr,
                                  CXL_DEFAULT_DOORBELL_SIZE) ||
        !cxl_rpc_phys_range_valid(ctx, addrs->metadata_queue_addr,
                                  addrs->metadata_queue_size) ||
        !cxl_rpc_phys_range_valid(ctx, addrs->flag_addr,
                                  CXL_DEFAULT_FLAG_SIZE)) {
        cxl_lib_errorf("connection_create_fixed(%s): invalid phys range doorbell=%#llx metadata=%#llx size=%zu flag=%#llx phys_base=%#llx map_size=%zu",
                       bootstrap_owner ? "owner" : "attach",
                       (unsigned long long)addrs->doorbell_addr,
                       (unsigned long long)addrs->metadata_queue_addr,
                       addrs->metadata_queue_size,
                       (unsigned long long)addrs->flag_addr,
                       ctx ? (unsigned long long)ctx->phys_base : 0ULL,
                       ctx ? ctx->map_size : 0u);
        return NULL;
    }

    if (addrs->request_data_size > 0 &&
        (addrs->request_data_addr == 0 ||
         !cxl_rpc_phys_range_valid(ctx, addrs->request_data_addr,
                                   addrs->request_data_size))) {
        cxl_lib_errorf("connection_create_fixed(%s): invalid request range addr=%#llx size=%zu",
                       bootstrap_owner ? "owner" : "attach",
                       (unsigned long long)addrs->request_data_addr,
                       addrs->request_data_size);
        return NULL;
    }

    if (addrs->response_data_size > 0 &&
        (addrs->response_data_addr == 0 ||
         !cxl_rpc_phys_range_valid(ctx, addrs->response_data_addr,
                                   addrs->response_data_size))) {
        cxl_lib_errorf("connection_create_fixed(%s): invalid response range addr=%#llx size=%zu",
                       bootstrap_owner ? "owner" : "attach",
                       (unsigned long long)addrs->response_data_addr,
                       addrs->response_data_size);
        return NULL;
    }

    cxl_connection_t *conn = calloc(1, sizeof(*conn));
    if (!conn)
        return NULL;
    cxl_connection_init_runtime_defaults(conn);

    cxl_lib_debugf("connection_create_fixed(%s) mq_entries=%u doorbell=%#llx metadata=%#llx request=%#llx response=%#llx flag=%#llx node_id=%u",
                   bootstrap_owner ? "owner" : "attach",
                   mq_entries,
                   (unsigned long long)addrs->doorbell_addr,
                   (unsigned long long)addrs->metadata_queue_addr,
                   (unsigned long long)addrs->request_data_addr,
                   (unsigned long long)addrs->response_data_addr,
                   (unsigned long long)addrs->flag_addr,
                   (unsigned)addrs->node_id);

    conn->ctx = ctx;
    conn->addrs = *addrs;
    conn->mq_entries = mq_entries;
    conn->mq_phase = 1;  /* Must match RPC engine's initial phase (true=1) */
    if (cxl_req_id_allocator_init(conn) != 0) {
        cxl_connection_destroy(conn);
        return NULL;
    }

    if (connection_finalize_setup(conn, required_mq_size, bootstrap_owner) != 0) {
        cxl_lib_errorf("connection_create_fixed(%s): finalize_setup failed doorbell=%#llx metadata=%#llx request=%#llx response=%#llx flag=%#llx",
                       bootstrap_owner ? "owner" : "attach",
                       (unsigned long long)conn->addrs.doorbell_addr,
                       (unsigned long long)conn->addrs.metadata_queue_addr,
                       (unsigned long long)conn->addrs.request_data_addr,
                       (unsigned long long)conn->addrs.response_data_addr,
                       (unsigned long long)conn->addrs.flag_addr);
        cxl_connection_destroy(conn);
        return NULL;
    }

    cxl_lib_debugf("connection_create_fixed(%s) done metadata_ptr=%p request_ptr=%p response_ptr=%p flag_ptr=%p",
                   bootstrap_owner ? "owner" : "attach",
                   (void *)conn->metadata_queue,
                   (void *)conn->request_data,
                   (void *)conn->response_data,
                   (void *)conn->flag);

    return conn;
}

cxl_connection_t *cxl_connection_create_fixed_owner(cxl_context_t *ctx,
                                                     const cxl_connection_addrs_t *addrs,
                                                     uint32_t mq_entries)
{
    return cxl_connection_create_fixed_internal(ctx, addrs, mq_entries, 1);
}

cxl_connection_t *cxl_connection_create_fixed_attach(cxl_context_t *ctx,
                                                      const cxl_connection_addrs_t *addrs,
                                                      uint32_t mq_entries)
{
    return cxl_connection_create_fixed_internal(ctx, addrs, mq_entries, 0);
}

cxl_connection_t *cxl_connection_create_fixed(cxl_context_t *ctx,
                                               const cxl_connection_addrs_t *addrs,
                                               uint32_t mq_entries)
{
    return cxl_connection_create_fixed_owner(ctx, addrs, mq_entries);
}

void cxl_connection_destroy(cxl_connection_t *conn)
{
    if (!conn)
        return;

    cxl_copyengine_disable(conn);

    if (conn->sync)
        cxl_sync_destroy(conn->sync);

    free(conn->rpc_id_inflight_bitmap);
    free(conn->req_entry_offsets);
    free(conn->req_entry_sizes);
    free(conn->req_entry_next);
    free(conn->req_entry_complete);
    free(conn);
}

const cxl_connection_addrs_t *cxl_connection_get_addrs(
    const cxl_connection_t *conn)
{
    return conn ? &conn->addrs : NULL;
}

int cxl_connection_bind_copyengine_lane(cxl_connection_t *conn,
                                        size_t engine_index,
                                        uint32_t channel_index)
{
    if (!conn)
        return -1;
    if (conn->ce_lane_assigned) {
        if (conn->ce_engine_index != engine_index ||
            conn->ce_hw_channel_id != channel_index) {
            return -1;
        }
        return 0;
    }

    conn->ce_lane_bind_valid = 1;
    conn->ce_bind_lane_index_valid = 0;
    conn->ce_bind_engine_index = engine_index;
    conn->ce_bind_lane_index = 0;
    conn->ce_bind_channel_id = channel_index;
    return 0;
}

int cxl_connection_bind_copyengine_lane_index(cxl_connection_t *conn,
                                              size_t lane_index)
{
    if (!conn)
        return -1;
    if (conn->ce_lane_assigned) {
        if (conn->ce_channel_index != lane_index)
            return -1;
        return 0;
    }

    conn->ce_lane_bind_valid = 1;
    conn->ce_bind_lane_index_valid = 1;
    conn->ce_bind_engine_index = 0;
    conn->ce_bind_lane_index = lane_index;
    conn->ce_bind_channel_id = 0;
    return 0;
}

/* ================================================================
 * Client API
 * ================================================================ */

int cxl_send_request(cxl_connection_t *conn,
                     const void *data,
                     size_t len)
{
    /* Parameter validation */
    if (!conn || !conn->doorbell)
        return -1;

    /* Check data pointer when length is non-zero */
    if (len > 0 && !data)
        return -1;

    if (len > CXL_REQ_PAYLOAD_SOFT_MAX)
        return -1;

    uint16_t req_id = 0;
    if (cxl_req_id_allocator_alloc(conn, &req_id) != 0)
        return -1;

    uint8_t doorbell_buf[16] __attribute__((aligned(16)));

    if (len <= 8) {
        /* Inline mode: data fits in doorbell */
        uint64_t inline_data = 0;
        if (data)
            memcpy(&inline_data, data, len);

        cxl_build_doorbell(doorbell_buf, CXL_METHOD_REQUEST, 1,
                           (uint32_t)len, conn->addrs.node_id,
                           req_id, inline_data);
    } else {
        /* Non-inline: write request_data entry, then doorbell */
        if (!conn->request_data || conn->addrs.request_data_size == 0)
            goto fail_release_req_id;

        /*
         * request_data publish uses 64B cacheline slots.
         * Doorbell header carries full payload length (32 bits), while
         * request_data stores payload bytes only (no in-band length header).
         */
        size_t entry_size = cxl_request_entry_size(len);
        size_t offset = 0;
        if (cxl_request_ring_reserve(conn, req_id, entry_size, &offset) != 0)
            goto fail_release_req_id;
        uint8_t *entry_ptr = (uint8_t *)conn->request_data + offset;
        if (len > 0) {
            memcpy((void *)entry_ptr, data, len);
        }

        /*
         * Publish request_data with exactly the same primitive sequence as
         * doorbell publish: store -> clflushopt(range) -> sfence.
         */
        uintptr_t req_line_start = ((uintptr_t)entry_ptr) & ~((uintptr_t)63);
        uintptr_t req_line_end =
            (((uintptr_t)entry_ptr + len) + 63u) & ~((uintptr_t)63);

        /* Publish order: store payload, flush touched cacheline(s), then sfence. */
        for (uintptr_t line = req_line_start; line < req_line_end;
             line += 64u) {
            __asm__ __volatile__(
                "clflushopt (%0)"
                :
                : "r"((void *)line)
                : "memory");
        }
        __asm__ __volatile__("sfence" ::: "memory");

        /* Compute logical/protocol address for doorbell data field. */
        uint64_t entry_phys = conn->addrs.request_data_addr +
                              ((uint8_t *)entry_ptr - (uint8_t *)conn->request_data);

        cxl_build_doorbell(doorbell_buf, CXL_METHOD_REQUEST, 0,
                           (uint32_t)len, conn->addrs.node_id,
                           req_id, entry_phys);
    }

    /* Publish doorbell via the same store+flush semantic path as payload. */
    cxl_publish_doorbell_raw(conn->doorbell, doorbell_buf);

    return (int)req_id;

fail_release_req_id:
    cxl_req_id_allocator_release(conn, req_id);
    return -1;
}

/* forward declarations for client response drain helpers */
int cxl_peek_latest_completed_rpc_id(cxl_connection_t *conn,
                                     uint16_t *out_rpc_id);

int cxl_consume_next_response(cxl_connection_t *conn,
                              void *out_data,
                              size_t *out_len,
                              uint16_t *out_rpc_id);

static int
cxl_peek_next_response_entry(cxl_connection_t *conn,
                             const volatile uint8_t **out_payload,
                             size_t *out_len,
                             uint16_t *out_rpc_id)
{
    if (out_payload)
        *out_payload = NULL;
    if (out_len)
        *out_len = 0;
    if (out_rpc_id)
        *out_rpc_id = 0;

    if (!conn || !conn->response_data ||
        !out_payload || !out_len || !out_rpc_id)
        return -1;
    if (conn->addrs.response_data_size == 0)
        return -1;

    if (conn->resp_read_offset >= conn->addrs.response_data_size) {
        conn->resp_read_offset = 0;
    }

    volatile uint8_t *resp = conn->response_data + conn->resp_read_offset;
    __asm__ __volatile__("clflushopt (%0)" :: "r"((void *)resp) : "memory");
    cxl_invalidate_load_barrier();
    uint64_t header = *(volatile uint64_t *)resp;
    uint32_t payload_len_u32 = (uint32_t)(header & 0xFFFFFFFFu);
    uint16_t resp_req_id = (uint16_t)((header >> 32) & 0xFFFFu);
    uint16_t resp_rsp_id = (uint16_t)((header >> 48) & 0xFFFFu);

    if (resp_req_id == 0 || resp_rsp_id == 0) {
        return 0;
    }

    size_t msg_len = (size_t)payload_len_u32;
    if (msg_len > CXL_RESP_PAYLOAD_SOFT_MAX)
        return -1;
    size_t entry_size = cxl_response_entry_size(msg_len);
    if (entry_size > conn->addrs.response_data_size)
        return -1;

    if (conn->resp_read_offset + entry_size > conn->addrs.response_data_size) {
        return -1;
    }
    if (resp_rsp_id != resp_req_id)
        return -1;
    if (!cxl_req_id_inflight_test(conn, resp_req_id))
        return -1;

    if (entry_size > 64u) {
        uintptr_t entry_line_start = ((uintptr_t)resp) & ~((uintptr_t)63);
        uintptr_t entry_line_end =
            (((uintptr_t)resp + entry_size) + 63u) & ~((uintptr_t)63);
        for (uintptr_t line = entry_line_start + 64u;
             line < entry_line_end; line += 64u) {
            __asm__ __volatile__(
                "clflushopt (%0)"
                :
                : "r"((void *)line)
                : "memory");
        }
        cxl_invalidate_load_barrier();
    }

    *out_payload = resp + CXL_RESP_HEADER_LEN;
    *out_len = msg_len;
    *out_rpc_id = resp_req_id;
    return 1;
}

static inline void
cxl_commit_response_head(cxl_connection_t *conn,
                         uint16_t rpc_id,
                         size_t msg_len)
{
    size_t entry_size = cxl_response_entry_size(msg_len);
    conn->resp_read_offset += entry_size;
    if (conn->addrs.response_data_size > 0 &&
        conn->resp_read_offset >= conn->addrs.response_data_size) {
        conn->resp_read_offset = 0;
    }
    cxl_request_ring_mark_complete(conn, rpc_id);
    cxl_req_id_allocator_release(conn, rpc_id);
}

int cxl_peek_latest_completed_rpc_id(cxl_connection_t *conn,
                                     uint16_t *out_rpc_id)
{
    if (out_rpc_id)
        *out_rpc_id = 0;

    if (!conn || !conn->flag || !out_rpc_id)
        return -1;

    volatile uint8_t *flag = conn->flag;
    __asm__ __volatile__("clflushopt (%0)" :: "r"((void *)flag) : "memory");
    cxl_invalidate_load_barrier();
    *out_rpc_id = *(volatile uint16_t *)flag;
    return (*out_rpc_id != 0u) ? 1 : 0;
}

int cxl_consume_next_response(cxl_connection_t *conn,
                              void *out_data,
                              size_t *out_len,
                              uint16_t *out_rpc_id)
{
    if (out_rpc_id)
        *out_rpc_id = 0;

    if (!conn || !conn->response_data || !out_data || !out_len ||
        !out_rpc_id)
        return -1;

    size_t capacity = *out_len;
    const volatile uint8_t *payload = NULL;
    int peek_rc = cxl_peek_next_response_entry(conn,
                                               &payload,
                                               out_len,
                                               out_rpc_id);
    if (peek_rc != 1)
        return peek_rc;

    if (capacity < *out_len)
        return -1;

    /*
     * A response is not considered complete on the client until the payload
     * has been demand-loaded into caller-owned local memory.
     */
    if (*out_len > 0)
        memcpy(out_data, (const void *)payload, *out_len);

    cxl_commit_response_head(conn, *out_rpc_id, *out_len);
    return 1;
}

/* ================================================================
 * Server API
 * ================================================================ */

int cxl_poll_request(cxl_connection_t *conn,
                     uint16_t *node_id,
                     uint16_t *rpc_id,
                     const void **out_data_view,
                     size_t *out_len)
{
    int ret = -1;
    const int mq_invalidate_consumed = CXL_MQ_INVALIDATE_CONSUMED;
    const int mq_invalidate_prefetched = CXL_MQ_INVALIDATE_PREFETCHED;
    const uint32_t mq_prefetch_lines = CXL_MQ_PREFETCH_LINES;
    const uint32_t mq_entries_per_line = 64u / CXL_METADATA_ENTRY_SIZE;

    if (out_data_view)
        *out_data_view = NULL;
    if (out_len)
        *out_len = 0;
    if (!conn || !conn->metadata_queue || !out_data_view || !out_len) {
        goto out;
    }

    if (cxl_lib_trace_polls_enabled()) {
        cxl_lib_debugf("poll_request begin mq_head=%u mq_phase=%u metadata=%#llx request_region=%#llx",
                       conn->mq_head, conn->mq_phase,
                       (unsigned long long)conn->addrs.metadata_queue_addr,
                       (unsigned long long)conn->addrs.request_data_addr);
    }

    /* Read metadata queue entry at head position */
    uint32_t head_idx = conn->mq_head;
    volatile uint8_t *entry = conn->metadata_queue +
                               (head_idx * CXL_METADATA_ENTRY_SIZE);
    uint32_t head_line_idx = (mq_entries_per_line > 0) ?
                             (head_idx / mq_entries_per_line) : 0;
    uint32_t mq_total_lines = (mq_entries_per_line > 0) ?
                              ((conn->mq_entries + mq_entries_per_line - 1) /
                               mq_entries_per_line) : 0;
    volatile uint8_t *entry_line = conn->metadata_queue +
                                   ((size_t)head_line_idx * 64u);

    uint64_t meta_lo = *(volatile uint64_t *)entry;
    uint64_t meta_hi = 0;
    uint8_t entry_phase = (uint8_t)((meta_lo >> 2) & 0x01u);
    uint8_t expected_phase = conn->mq_phase & 0x01;

    if (cxl_lib_trace_polls_enabled()) {
        cxl_lib_debugf("poll_request probe head=%u meta_lo=%#llx flags4=%#x entry_phase=%u expected_phase=%u",
                       head_idx,
                       (unsigned long long)meta_lo,
                       (unsigned)(meta_lo & 0x0Fu),
                       (unsigned)entry_phase,
                       (unsigned)expected_phase);
    }

    /* Check phase bit: new entry has phase matching our expected phase */
    if (entry_phase != expected_phase) {
        /*
         * Always invalidate the probed head line after an empty poll because
         * this poll just demand-loaded it into CPU caches. For the prefetched
         * lookahead window, only invalidate once per prefetch arm; repeated
         * empty polls on the same head should not keep re-flushing the same
         * stale tail window unless a later successful poll has prefetched it
         * again.
         */
        uint32_t invalidate_start_line = head_line_idx;
        uint32_t invalidate_nr_lines = 1;

        if (mq_invalidate_prefetched &&
            conn->mq_prefetch_window_valid &&
            conn->mq_prefetch_nr_lines > 0 &&
            mq_total_lines > 1) {
            uint32_t prefetched_start = conn->mq_prefetch_start_line;
            uint32_t prefetched_nr = conn->mq_prefetch_nr_lines;

            if (prefetched_nr >= mq_total_lines)
                prefetched_nr = mq_total_lines - 1;

            if (prefetched_start == head_line_idx) {
                invalidate_nr_lines += prefetched_nr - 1u;
            } else if (prefetched_start ==
                       ((head_line_idx + 1u) % mq_total_lines)) {
                invalidate_nr_lines += prefetched_nr;
            } else {
                cxl_invalidate_cacheline(entry_line);
                __asm__ __volatile__("sfence" ::: "memory");
                cxl_mq_invalidate_lines(conn->metadata_queue,
                                        prefetched_start,
                                        prefetched_nr,
                                        mq_total_lines);
                conn->mq_prefetch_window_valid = 0;
                conn->mq_prefetch_nr_lines = 0;
                ret = 0;
                goto out;
            }

            conn->mq_prefetch_window_valid = 0;
            conn->mq_prefetch_nr_lines = 0;
        }

        cxl_mq_invalidate_lines(conn->metadata_queue,
                                invalidate_start_line,
                                invalidate_nr_lines,
                                mq_total_lines);
        ret = 0;
        goto out;
    }

    meta_hi = *(volatile uint64_t *)(entry + 8);

    if (mq_prefetch_lines > 0 && mq_total_lines > 1) {
        uint32_t prefetched_nr_lines = mq_prefetch_lines;
        if (prefetched_nr_lines >= mq_total_lines)
            prefetched_nr_lines = mq_total_lines - 1u;

        for (uint32_t i = 1; i <= prefetched_nr_lines; i++) {
            uint32_t next_line = (head_line_idx + i) % mq_total_lines;
            __builtin_prefetch((const void *)(conn->metadata_queue +
                                              ((size_t)next_line * 64u)),
                               0, 3);
        }
        conn->mq_prefetch_start_line = (head_line_idx + 1u) % mq_total_lines;
        conn->mq_prefetch_nr_lines = prefetched_nr_lines;
        conn->mq_prefetch_window_valid = (prefetched_nr_lines > 0) ? 1u : 0u;
    } else {
        conn->mq_prefetch_nr_lines = 0;
        conn->mq_prefetch_window_valid = 0;
    }

    /* Parse entry fields */
    uint8_t mq_method  = (uint8_t)(meta_lo & 0x01u);
    uint8_t mq_inline  = (uint8_t)((meta_lo >> 1) & 0x01u);
    uint32_t mq_len = (uint32_t)((meta_lo >> 3) & 0xFFFFFFFFu);
    uint16_t nid = (uint16_t)((meta_lo >> 35) & CXL_NODE_ID_MASK);
    uint16_t rid = (uint16_t)((meta_lo >> 49) & CXL_RPC_ID_MASK);

    if (node_id) *node_id = nid;
    if (rpc_id)  *rpc_id = rid;

    /* Extract payload */
    if (mq_method != CXL_METHOD_REQUEST || rid == 0) {
        cxl_lib_errorf("poll_request: unexpected metadata entry method=%u rpc_id=%u head=%u",
                       (unsigned)mq_method, (unsigned)rid, head_idx);
        cxl_advance_mq_head_with_policy(conn, head_idx, head_line_idx,
                                        mq_entries_per_line,
                                        mq_total_lines,
                                        mq_invalidate_consumed);
        ret = -1;
        goto out;
    }

    if (mq_inline) {
        /* Inline: payload view is in bytes 8-15. */
        size_t msg_len = mq_len <= 8 ? (size_t)mq_len : 8;
        *out_data_view = (const void *)(entry + 8);
        *out_len = msg_len;
    } else {
        /* Non-inline: bytes 8-15 contain an absolute logical payload address. */
        uint64_t data_addr = meta_hi;
        size_t msg_len = (size_t)mq_len;
        if (msg_len == 0 ||
            msg_len > sizeof(conn->request_local_buf) ||
            !cxl_rpc_phys_range_valid(conn->ctx, data_addr, msg_len)) {
            cxl_advance_mq_head_with_policy(conn, head_idx, head_line_idx,
                                            mq_entries_per_line,
                                            mq_total_lines,
                                            mq_invalidate_consumed);
            ret = -1;
            goto out;
        }

        volatile uint8_t *data_ptr = cxl_rpc_phys_to_virt(conn->ctx,
                                                          data_addr);
        if (data_ptr) {
            const volatile uint8_t *payload_ptr = data_ptr;

            /* Invalidate payload cacheline(s) before demand-load. */
            uintptr_t line_start = ((uintptr_t)payload_ptr) & ~((uintptr_t)63);
            uintptr_t line_end =
                (((uintptr_t)payload_ptr + msg_len) + 63u) &
                ~((uintptr_t)63);
            for (uintptr_t line = line_start; line < line_end;
                 line += 64u) {
                __asm__ __volatile__(
                    "clflushopt (%0)"
                    :
                    : "r"((void *)line)
                    : "memory");
            }
            cxl_invalidate_load_barrier();
            if (msg_len > 0)
                memcpy(conn->request_local_buf,
                       (const void *)payload_ptr,
                       msg_len);
            *out_data_view = (const void *)conn->request_local_buf;
            *out_len = msg_len;
        } else {
            ret = -1;
            goto out;
        }
    }

    /* Advance head */
    cxl_advance_mq_head_with_policy(conn, head_idx, head_line_idx,
                                    mq_entries_per_line,
                                    mq_total_lines,
                                    mq_invalidate_consumed);

    ret = 1;
out:
    return ret;
}

/* ================================================================
 * Server Response API
 * ================================================================ */

static int
cxl_prepare_response_tx_path(cxl_connection_t *conn)
{
    if (!conn)
        return -1;

    conn->resp_tx_ready = 0;
    if (!conn->peer_response_data || conn->peer_response_data_size == 0 ||
        !conn->peer_flag || conn->peer_flag_addr == 0) {
        return 0;
    }

    if (cxl_copyengine_update_peer_response_mapping(conn) != 0)
        return -1;
    if (cxl_copyengine_update_peer_flag_mapping(conn) != 0)
        return -1;
    if (cxl_copyengine_prepare(conn) != 0)
        return -1;
    if (cxl_copyengine_ensure_response_slots(conn) != 0)
        return -1;
    if (cxl_copyengine_validate_submit_invariants(conn) != 0)
        return -1;

    conn->resp_tx_ready = 1;
    return 0;
}

static int
cxl_pick_peer_response_offset(const cxl_connection_t *conn,
                              size_t entry_size,
                              size_t *out_offset)
{
    size_t offset = 0;

    if (!conn || !out_offset || entry_size == 0)
        return -1;
    if (entry_size > conn->peer_response_data_size)
        return -1;

    offset = conn->peer_resp_write_offset;
    if (offset >= conn->peer_response_data_size)
        offset = 0;
    if (offset + entry_size > conn->peer_response_data_size)
        offset = 0;

    *out_offset = offset;
    return 0;
}

int cxl_connection_set_peer_response_data(cxl_connection_t *conn,
                                          uint64_t peer_response_data_addr,
                                          size_t peer_response_data_size)
{
    if (!conn || !conn->ctx)
        return -1;

    if (peer_response_data_addr == 0 || peer_response_data_size == 0)
        return -1;

    if (!cxl_rpc_phys_range_valid(conn->ctx,
                                  peer_response_data_addr,
                                  peer_response_data_size)) {
        return -1;
    }

    /*
     * Map peer logical region locally once so setup can observe the runtime
     * DMA page map. Response submit no longer depends on this virtual pointer.
     */
    volatile void *response_ptr = cxl_rpc_phys_to_virt(conn->ctx,
                                                        peer_response_data_addr);

    if (!response_ptr)
        return -1;

    /* All checks passed, commit the configuration */
    conn->peer_response_data_addr = peer_response_data_addr;
    conn->peer_response_data_size = peer_response_data_size;
    conn->peer_response_data = (volatile uint8_t *)response_ptr;
    conn->peer_resp_write_offset = 0;
    /*
     * Keep setup constant-time. Destination page translation is resolved on
     * first use when a response segment actually targets that page.
     */
    if (cxl_prepare_response_tx_path(conn) != 0)
        return -1;

    return 0;
}

int cxl_connection_set_peer_response_flag_addr(cxl_connection_t *conn,
                                               uint64_t peer_flag_addr)
{
    if (!conn || !conn->ctx)
        return -1;

    if (peer_flag_addr == 0)
        return -1;

    if (!cxl_rpc_phys_range_valid(conn->ctx, peer_flag_addr,
                                  CXL_DEFAULT_FLAG_SIZE))
        return -1;

    /*
     * Map peer logical flag locally once so setup can observe the runtime
     * DMA destination cacheline.
     */
    volatile void *flag_ptr = cxl_rpc_phys_to_virt(conn->ctx, peer_flag_addr);
    if (!flag_ptr)
        return -1;

    conn->peer_flag_addr = peer_flag_addr;
    conn->peer_flag = (volatile uint8_t *)flag_ptr;
    cxl_prefault_each_page_ro_and_flush_touched(conn->peer_flag,
                                                CXL_DEFAULT_FLAG_SIZE);
    if (cxl_prepare_response_tx_path(conn) != 0)
        return -1;

    return 0;
}

int cxl_send_response(cxl_connection_t *conn,
                      uint16_t rpc_id,
                      const void *data,
                      size_t len)
{
    size_t entry_size = 0;
    size_t offset = 0;

    if (!conn || !conn->resp_tx_ready)
        return -1;
    if (len > 0 && !data)
        return -1;
    if (rpc_id == 0 || rpc_id > CXL_RPC_ID_MASK)
        return -1;
    if (len > CXL_RESP_PAYLOAD_SOFT_MAX)
        return -1;

    entry_size = cxl_response_entry_size(len);
    if (cxl_pick_peer_response_offset(conn, entry_size, &offset) != 0)
        return -1;

    if (cxl_lib_debug_enabled()) {
        size_t next_offset = offset + entry_size;
        if (next_offset >= conn->peer_response_data_size)
            next_offset = 0;
        cxl_lib_debugf("send_response rpc_id=%u len=%zu entry_size=%zu offset=%zu next_offset=%zu flag_addr=%#llx",
                       rpc_id,
                       len,
                       entry_size,
                       offset,
                       next_offset,
                       (unsigned long long)conn->peer_flag_addr);
    }

    /*
     * Preferred path: asynchronous CopyEngine offload.
     * Submit resp_data then flag in-order without waiting for completion.
     * Destination readiness is prepared once during peer setup, so submit only
     * translates the logical offset into one or two observed DMA segments,
     * builds descriptors, and rings the engine.
     */
    if (!cxl_copyengine_submit_response_async(conn, rpc_id, data, len,
                                              offset, entry_size)) {
        fprintf(stderr,
                "cxl_rpc: ERROR: CopyEngine submit path failed (rpc_id=%u)\n",
                rpc_id);
        return -1;
    }

    conn->peer_resp_write_offset = offset + entry_size;
    if (conn->peer_resp_write_offset >= conn->peer_response_data_size)
        conn->peer_resp_write_offset = 0;

    return 0;
}
