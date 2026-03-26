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

#if defined(__has_include)
#if __has_include(<gem5/m5ops.h>)
#include <gem5/m5ops.h>
#elif __has_include("gem5/m5ops.h")
#include "gem5/m5ops.h"
#elif __has_include(<m5ops.h>)
#include <m5ops.h>
#else
#error "m5ops.h is not found"
#endif
#else
#include <gem5/m5ops.h>
#endif

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
        enabled = cxl_env_flag_enabled("CXL_RPC_LIB_DEBUG");
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

static inline uint64_t
cxl_trace_tick(void)
{
    return m5_rpns();
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
#define CXL_MQ_ENTRIES_PER_LINE (64u / CXL_METADATA_ENTRY_SIZE)
#define CXL_BOOTSTRAP_LEN_MAGIC 0xFFFF0000u
#define CXL_BOOTSTRAP_OP_REGISTER_DOORBELL 0x0001u
#define CXL_BOOTSTRAP_OP_REGISTER_DOORBELL_PAGE_BASE 0x0200u
#define CXL_BOOTSTRAP_OP_REGISTER_METADATA_PAGE_BASE 0x0100u
#define CXL_RESPONSE_DMA_THRESHOLD 4096u

static inline size_t
cxl_response_publish_bytes(size_t payload_len)
{
    return CXL_RESP_HEADER_LEN + payload_len;
}

static inline size_t
cxl_response_entry_size(size_t payload_len)
{
    const size_t total_size = cxl_response_publish_bytes(payload_len);
    return ((total_size + 63) / 64) * 64;
}

static inline int
cxl_response_payload_len_valid(size_t payload_len)
{
    if (payload_len > (size_t)UINT32_MAX)
        return 0;
    if (payload_len > SIZE_MAX - CXL_RESP_HEADER_LEN)
        return 0;
    if ((payload_len + CXL_RESP_HEADER_LEN) > SIZE_MAX - 63u)
        return 0;
    return 1;
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
cxl_mq_invalidate_lines_nofence(volatile uint8_t *metadata_queue,
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
}

static inline void
cxl_mq_invalidate_lines(volatile uint8_t *metadata_queue,
                        uint32_t start_line,
                        uint32_t nr_lines,
                        uint32_t total_lines)
{
    cxl_mq_invalidate_lines_nofence(metadata_queue,
                                    start_line,
                                    nr_lines,
                                    total_lines);
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

static void
cxl_publish_request_payload(volatile uint8_t *dst,
                            const void *src,
                            size_t len)
{
    memcpy((void *)dst, src, len);
    cxl_flush_range(dst, len);
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

    if (!conn || !conn->doorbell)
        return -1;

    cxl_build_doorbell(doorbell_buf, CXL_METHOD_REQUEST, 1,
                       bootstrap_len, conn->addrs.node_id, 0, data);
    cxl_publish_doorbell_raw(conn->doorbell, doorbell_buf);
    return 0;
}

static int
cxl_bootstrap_controller_translation(cxl_connection_t *conn,
                                     size_t metadata_clear_size)
{
    size_t doorbell_page_count = 0;

    if (!conn || !conn->doorbell || !conn->metadata_queue)
        return -1;

    if (cxl_send_controller_bootstrap_doorbell(
            conn,
            CXL_BOOTSTRAP_LEN_MAGIC | CXL_BOOTSTRAP_OP_REGISTER_DOORBELL,
            0) != 0) {
        return -1;
    }

    if (conn->addrs.metadata_queue_addr > conn->addrs.doorbell_addr) {
        const uint64_t doorbell_bytes =
            conn->addrs.metadata_queue_addr - conn->addrs.doorbell_addr;
        doorbell_page_count =
            (size_t)((doorbell_bytes + CXL_METADATA_TRANSLATION_PAGE_BYTES - 1u) /
                     CXL_METADATA_TRANSLATION_PAGE_BYTES);
    }
    for (size_t page_index = 0; page_index < doorbell_page_count; ++page_index) {
        volatile uint8_t *page_ptr =
            conn->doorbell +
            page_index * CXL_METADATA_TRANSLATION_PAGE_BYTES;
        uint64_t observed_page = 0;
        if (cxl_rpc_virt_to_phys_local((const void *)page_ptr,
                                       &observed_page) != 0) {
            cxl_lib_errorf("bootstrap failed to resolve doorbell page gpa "
                           "page=%zu logical=%#llx",
                           page_index,
                           (unsigned long long)(conn->addrs.doorbell_addr +
                               page_index * CXL_METADATA_TRANSLATION_PAGE_BYTES));
            return -1;
        }
        observed_page &= ~((uint64_t)CXL_METADATA_TRANSLATION_PAGE_BYTES - 1u);

        if (cxl_send_controller_bootstrap_doorbell(
                conn,
                CXL_BOOTSTRAP_LEN_MAGIC |
                    (CXL_BOOTSTRAP_OP_REGISTER_DOORBELL_PAGE_BASE |
                     (uint32_t)page_index),
                observed_page) != 0) {
            return -1;
        }
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

        if (cxl_send_controller_bootstrap_doorbell(
                conn,
                CXL_BOOTSTRAP_LEN_MAGIC |
                    (CXL_BOOTSTRAP_OP_REGISTER_METADATA_PAGE_BASE |
                     (uint32_t)page_index),
                observed_page) != 0) {
            return -1;
        }
    }
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
                "cxl_rpc_init: numa_tonode_memory(node=%d) failed: errno=%d (%s)\n",
                ctx->numa_node, errno, strerror(errno));
        munmap((void *)ctx->base, cxl_size);
        close(fd);
        ctx->base = NULL;
        ctx->shm_fd = -1;
        ctx->shm_name[0] = '\0';
        return -1;
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

static inline volatile uint8_t *
cxl_rpc_phys_to_virt_trusted(const cxl_context_t *ctx,
                             uint64_t phys_addr)
{
    return ctx->base + (size_t)(phys_addr - ctx->phys_base);
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

static inline void
cxl_invalidate_range_for_load(volatile uint8_t *ptr, size_t size)
{
    if (!ptr || size == 0)
        return;

    uintptr_t line_start = ((uintptr_t)ptr) & ~((uintptr_t)63);
    uintptr_t line_end = (((uintptr_t)ptr + size) + 63u) & ~((uintptr_t)63);
    for (uintptr_t line = line_start; line < line_end; line += 64u) {
        cxl_invalidate_cacheline((volatile uint8_t *)line);
    }
    cxl_invalidate_load_barrier();
}

static inline void
cxl_prefetch_range_ro(const volatile uint8_t *ptr, size_t size)
{
    if (!ptr || size == 0)
        return;

    uintptr_t line_start = ((uintptr_t)ptr) & ~((uintptr_t)63);
    uintptr_t line_end = (((uintptr_t)ptr + size) + 63u) & ~((uintptr_t)63);
    for (uintptr_t line = line_start; line < line_end; line += 64u) {
        __builtin_prefetch((const void *)line, 0, 3);
    }
}

static inline void
cxl_prepare_payload_for_direct_read(volatile uint8_t *ptr, size_t size)
{
    cxl_invalidate_range_for_load(ptr, size);
    cxl_prefetch_range_ro(ptr, size);
}

static inline volatile uint8_t *
cxl_request_payload_ptr_trusted(const cxl_connection_t *conn,
                                uint64_t phys_addr)
{
    return cxl_rpc_phys_to_virt_trusted(conn->ctx, phys_addr);
}

static inline uint32_t
cxl_mq_line_slot_count(const cxl_connection_t *conn,
                       uint32_t line_idx)
{
    uint32_t line_base = 0;
    uint32_t remaining = 0;

    if (!conn || conn->mq_entries == 0)
        return 0;

    line_base = line_idx * CXL_MQ_ENTRIES_PER_LINE;
    if (line_base >= conn->mq_entries)
        return 0;

    remaining = conn->mq_entries - line_base;
    if (remaining > CXL_MQ_ENTRIES_PER_LINE)
        remaining = CXL_MQ_ENTRIES_PER_LINE;
    return remaining;
}

static inline void
cxl_mq_payload_prepare_reset_for_head(cxl_connection_t *conn)
{
    uint32_t slot_in_line = 0;
    uint32_t line_idx = 0;
    uint32_t line_slot_count = 0;
    uint32_t next_probe_slot = 0;

    if (!conn || conn->mq_entries == 0) {
        if (conn) {
            conn->mq_payload_prepared_mask = 0;
            conn->mq_payload_next_probe_slot = 0;
        }
        return;
    }

    slot_in_line = conn->mq_head % CXL_MQ_ENTRIES_PER_LINE;
    line_idx = conn->mq_head / CXL_MQ_ENTRIES_PER_LINE;
    line_slot_count = cxl_mq_line_slot_count(conn, line_idx);
    next_probe_slot = slot_in_line + 1u;
    if (next_probe_slot > line_slot_count)
        next_probe_slot = line_slot_count;

    conn->mq_payload_prepared_mask = 0;
    conn->mq_payload_next_probe_slot = (uint8_t)next_probe_slot;
}

static inline void
cxl_mq_payload_prepare_after_advance(cxl_connection_t *conn,
                                     uint32_t consumed_head_idx)
{
    uint32_t consumed_slot = 0;
    uint32_t consumed_line_idx = 0;
    uint32_t next_line_idx = 0;

    if (!conn || conn->mq_entries == 0)
        return;

    consumed_slot = consumed_head_idx % CXL_MQ_ENTRIES_PER_LINE;
    conn->mq_payload_prepared_mask &=
        (uint8_t)~(uint8_t)(1u << consumed_slot);

    consumed_line_idx = consumed_head_idx / CXL_MQ_ENTRIES_PER_LINE;
    next_line_idx = conn->mq_head / CXL_MQ_ENTRIES_PER_LINE;
    if (conn->mq_head <= consumed_head_idx ||
        next_line_idx != consumed_line_idx) {
        cxl_mq_payload_prepare_reset_for_head(conn);
    }
}

static void
cxl_prepare_future_payloads_same_line(cxl_connection_t *conn,
                                      uint32_t head_idx,
                                      uint32_t head_line_idx,
                                      uint8_t expected_phase)
{
    volatile uint8_t *entry_line = NULL;
    uint32_t line_slot_count = 0;
    uint32_t current_slot = 0;
    uint32_t start_slot = 0;

    if (!conn || !conn->metadata_queue) {
        return;
    }

    line_slot_count = cxl_mq_line_slot_count(conn, head_line_idx);
    if (line_slot_count == 0)
        return;

    current_slot = head_idx % CXL_MQ_ENTRIES_PER_LINE;
    start_slot = conn->mq_payload_next_probe_slot;
    if (start_slot < current_slot + 1u)
        start_slot = current_slot + 1u;
    if (start_slot >= line_slot_count) {
        conn->mq_payload_next_probe_slot = (uint8_t)line_slot_count;
        return;
    }

    entry_line = conn->metadata_queue + ((size_t)head_line_idx * 64u);
    for (uint32_t slot = start_slot; slot < line_slot_count; ++slot) {
        volatile uint8_t *future_entry =
            entry_line + ((size_t)slot * CXL_METADATA_ENTRY_SIZE);
        uint64_t future_lo = *(volatile uint64_t *)future_entry;
        uint8_t future_phase = (uint8_t)((future_lo >> 2) & 0x01u);

        if (future_phase != expected_phase) {
            conn->mq_payload_next_probe_slot = (uint8_t)slot;
            return;
        }

        conn->mq_payload_next_probe_slot = (uint8_t)(slot + 1u);

        uint8_t future_method = (uint8_t)(future_lo & 0x01u);
        uint8_t future_inline = (uint8_t)((future_lo >> 1) & 0x01u);
        uint32_t future_len = (uint32_t)((future_lo >> 3) & 0xFFFFFFFFu);
        uint16_t future_rid =
            (uint16_t)((future_lo >> 49) & CXL_RPC_ID_MASK);

        if (future_method != CXL_METHOD_REQUEST || future_rid == 0 ||
            future_inline) {
            continue;
        }

        if (future_len == 0 || future_len > CXL_REQ_PAYLOAD_SOFT_MAX) {
            continue;
        }

        uint64_t future_addr = *(volatile uint64_t *)(future_entry + 8);
        volatile uint8_t *payload_ptr =
            cxl_request_payload_ptr_trusted(conn, future_addr);
        cxl_prepare_payload_for_direct_read(payload_ptr, (size_t)future_len);
        conn->mq_payload_prepared_mask |= (uint8_t)(1u << slot);
    }
}

/* ================================================================
 * Connection Management
 * ================================================================ */

static inline int
cxl_connection_has_cap(const cxl_connection_t *conn, uint32_t cap)
{
    return (conn && ((conn->caps & cap) != 0u)) ? 1 : 0;
}

static int
cxl_connection_require_cap(const cxl_connection_t *conn,
                           uint32_t cap,
                           const char *api)
{
    if (cxl_connection_has_cap(conn, cap))
        return 1;

    cxl_lib_errorf("%s: unsupported connection role caps=%#x required=%#x",
                   api ? api : "connection_api",
                   conn ? conn->caps : 0u, cap);
    return 0;
}

static inline void
cxl_request_issue_state_reset(cxl_connection_t *conn)
{
    if (!conn)
        return;

    conn->rpc_id_next = 1u;
    conn->req_write_offset = 0;
}

static void
cxl_response_rx_state_reset(cxl_connection_t *conn)
{
    if (!conn)
        return;

    conn->resp_read_cursor = 0;
    conn->resp_known_producer_cursor = 0;
    conn->resp_peek_cursor = 0;
    conn->resp_peek_len = 0;
    conn->resp_peek_rpc_id = 0;
    conn->resp_peek_valid = 0;
    conn->resp_peek_payload_loaded = 0;
}

static int
cxl_req_id_allocator_init(cxl_connection_t *conn)
{
    if (!conn)
        return -1;
    if ((conn->caps & (CXL_CONN_CAP_REQUEST_TX | CXL_CONN_CAP_RESPONSE_RX)) == 0)
        return 0;

    cxl_request_issue_state_reset(conn);
    cxl_response_rx_state_reset(conn);
    return 0;
}

static int
cxl_request_issue_prepare(cxl_connection_t *conn,
                          uint32_t entry_size,
                          size_t *out_offset,
                          uint16_t *out_req_id)
{
    uint16_t req_id = 0;

    if (out_offset)
        *out_offset = 0;

    if (!conn || !out_req_id)
        return -1;

    if (entry_size > 0) {
        if (!out_offset)
            return -1;
        *out_offset = conn->req_write_offset;
        conn->req_write_offset += (size_t)entry_size;
    }

    req_id = conn->rpc_id_next;
    if (req_id == 0 || req_id > (uint16_t)CXL_RPC_ID_MASK)
        req_id = 1u;

    conn->rpc_id_next =
        (req_id >= (uint16_t)CXL_RPC_ID_MASK) ? 1u : (uint16_t)(req_id + 1u);
    *out_req_id = req_id;
    return 0;
}

static void connection_init_virt_ptrs(cxl_connection_t *conn)
{
    if (!conn || !conn->ctx)
        return;

    if (conn->caps & (CXL_CONN_CAP_REQUEST_RX |
                      CXL_CONN_CAP_REQUEST_TX |
                      CXL_CONN_CAP_HEAD_SYNC |
                      CXL_CONN_CAP_BOOTSTRAP)) {
        conn->doorbell = cxl_rpc_phys_to_virt(conn->ctx,
                                              conn->addrs.doorbell_addr);
    }
    if (conn->caps & CXL_CONN_CAP_REQUEST_RX) {
        conn->metadata_queue = cxl_rpc_phys_to_virt(
            conn->ctx, conn->addrs.metadata_queue_addr);
    }
    if (conn->caps & CXL_CONN_CAP_REQUEST_TX) {
        conn->request_data = cxl_rpc_phys_to_virt(conn->ctx,
                                                  conn->addrs.request_data_addr);
    }
    if (conn->caps & CXL_CONN_CAP_RESPONSE_RX) {
        conn->response_data = cxl_rpc_phys_to_virt(
            conn->ctx, conn->addrs.response_data_addr);
        conn->flag = cxl_rpc_phys_to_virt(conn->ctx, conn->addrs.flag_addr);
    }
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
                                int invalidate_consumed)
{
    const uint32_t mq_total_lines = conn ? conn->mq_total_lines : 0;

    if (!conn)
        return;

    if (invalidate_consumed &&
        mq_total_lines > 0 &&
        conn->mq_entries > 0) {
        uint32_t next_head = consumed_head_idx + 1;
        if (next_head >= conn->mq_entries)
            next_head = 0;
        uint32_t next_line_idx = next_head / CXL_MQ_ENTRIES_PER_LINE;
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
connection_validate_fixed_addrs(cxl_context_t *ctx,
                                const cxl_connection_addrs_t *addrs,
                                uint32_t mq_entries,
                                uint32_t caps,
                                const char *label,
                                size_t *metadata_clear_size_out)
{
    size_t metadata_clear_size = 0;

    if (metadata_clear_size_out)
        *metadata_clear_size_out = 0;
    if (!ctx || !addrs)
        return -1;

    if ((caps & (CXL_CONN_CAP_REQUEST_TX |
                 CXL_CONN_CAP_REQUEST_RX |
                 CXL_CONN_CAP_RESPONSE_RX)) == 0) {
        return 0;
    }

    if (addrs->node_id > CXL_NODE_ID_MASK) {
        cxl_lib_errorf("%s: invalid node_id=%u",
                       label ? label : "connection_create",
                       (unsigned)addrs->node_id);
        return -1;
    }

    if (caps & (CXL_CONN_CAP_REQUEST_RX |
                CXL_CONN_CAP_REQUEST_TX |
                CXL_CONN_CAP_HEAD_SYNC |
                CXL_CONN_CAP_BOOTSTRAP)) {
        if (addrs->doorbell_addr == 0 ||
            !cxl_rpc_phys_range_valid(ctx, addrs->doorbell_addr,
                                      CXL_DEFAULT_DOORBELL_SIZE)) {
            cxl_lib_errorf("%s: invalid doorbell addr=%#llx",
                           label ? label : "connection_create",
                           (unsigned long long)addrs->doorbell_addr);
            return -1;
        }
    }

    if (caps & CXL_CONN_CAP_REQUEST_RX) {
        if (mq_entries == 0)
            mq_entries = 1024u;
        metadata_clear_size = (size_t)mq_entries * CXL_METADATA_ENTRY_SIZE;
        if (addrs->metadata_queue_addr == 0 ||
            addrs->metadata_queue_size < metadata_clear_size ||
            !cxl_rpc_phys_range_valid(ctx, addrs->metadata_queue_addr,
                                      addrs->metadata_queue_size)) {
            cxl_lib_errorf("%s: invalid metadata addr=%#llx size=%zu required=%zu",
                           label ? label : "connection_create",
                           (unsigned long long)addrs->metadata_queue_addr,
                           addrs->metadata_queue_size, metadata_clear_size);
            return -1;
        }
    }

    if (caps & CXL_CONN_CAP_REQUEST_TX) {
        if (addrs->request_data_addr == 0 || addrs->request_data_size == 0 ||
            !cxl_rpc_phys_range_valid(ctx, addrs->request_data_addr,
                                      addrs->request_data_size)) {
            cxl_lib_errorf("%s: invalid request range addr=%#llx size=%zu",
                           label ? label : "connection_create",
                           (unsigned long long)addrs->request_data_addr,
                           addrs->request_data_size);
            return -1;
        }
    }

    if (caps & CXL_CONN_CAP_RESPONSE_RX) {
        if (addrs->response_data_addr == 0 || addrs->response_data_size == 0 ||
            addrs->flag_addr == 0 ||
            !cxl_rpc_phys_range_valid(ctx, addrs->response_data_addr,
                                      addrs->response_data_size) ||
            !cxl_rpc_phys_range_valid(ctx, addrs->flag_addr,
                                      CXL_DEFAULT_FLAG_SIZE)) {
            cxl_lib_errorf("%s: invalid response-rx ranges response=%#llx size=%zu flag=%#llx",
                           label ? label : "connection_create",
                           (unsigned long long)addrs->response_data_addr,
                           addrs->response_data_size,
                           (unsigned long long)addrs->flag_addr);
            return -1;
        }
    }

    if (metadata_clear_size_out)
        *metadata_clear_size_out = metadata_clear_size;
    return 0;
}

static int
connection_finalize_setup(cxl_connection_t *conn,
                          size_t metadata_clear_size)
{
    size_t doorbell_span = 0;
    const int has_req_rx = cxl_connection_has_cap(conn, CXL_CONN_CAP_REQUEST_RX);
    const int has_resp_rx = cxl_connection_has_cap(conn, CXL_CONN_CAP_RESPONSE_RX);
    const int has_bootstrap = cxl_connection_has_cap(conn, CXL_CONN_CAP_BOOTSTRAP);
    const int has_head_sync = cxl_connection_has_cap(conn, CXL_CONN_CAP_HEAD_SYNC);

    if (!conn)
        return -1;

    connection_init_virt_ptrs(conn);
    if (has_req_rx &&
        conn->addrs.metadata_queue_addr > conn->addrs.doorbell_addr) {
        doorbell_span = (size_t)(conn->addrs.metadata_queue_addr -
                                 conn->addrs.doorbell_addr);
    }
    if ((has_req_rx || has_head_sync || has_bootstrap) &&
        doorbell_span > 0 &&
        conn->doorbell) {
        cxl_prefault_each_page_ro_and_flush_touched(conn->doorbell,
                                                    doorbell_span);
    }
    if (has_req_rx) {
        cxl_prefault_each_page_ro_and_flush_touched(conn->metadata_queue,
                                                    metadata_clear_size);
    }
    if (has_resp_rx) {
        cxl_prefault_each_page_ro_and_flush_touched(conn->flag,
                                                    CXL_DEFAULT_FLAG_SIZE);
    }

    /*
     * Owner/bootstrap paths reset the completion flag. Attach paths must not
     * destroy already-published completions on a live shared connection.
     */
    if (has_bootstrap && has_resp_rx)
        cxl_zero_and_flush_range(conn->flag, CXL_DEFAULT_FLAG_SIZE);

    if (has_bootstrap && has_req_rx &&
        conn->metadata_queue && metadata_clear_size > 0) {
        cxl_zero_and_flush_range(conn->metadata_queue, metadata_clear_size);
    }

    if (has_head_sync && conn->doorbell && conn->mq_entries > 0) {
        cxl_sync_config_t sync_cfg = {
            .queue_size_n = conn->mq_entries,
        };
        conn->sync = cxl_sync_init(&sync_cfg, (volatile void *)conn->doorbell);
    }

    if (has_bootstrap &&
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
cxl_connection_create_internal(cxl_context_t *ctx,
                               const cxl_connection_addrs_t *addrs,
                               uint32_t mq_entries,
                               uint32_t caps,
                               const char *label)
{
    cxl_connection_t *conn = NULL;
    size_t metadata_clear_size = 0;

    if (!ctx)
        return NULL;
    if ((caps & (CXL_CONN_CAP_REQUEST_RX |
                 CXL_CONN_CAP_REQUEST_TX |
                 CXL_CONN_CAP_RESPONSE_RX)) != 0 &&
        connection_validate_fixed_addrs(ctx, addrs, mq_entries, caps, label,
                                        &metadata_clear_size) != 0) {
        return NULL;
    }
    if ((caps & CXL_CONN_CAP_REQUEST_RX) && mq_entries == 0)
        mq_entries = 1024u;

    conn = calloc(1, sizeof(*conn));
    if (!conn)
        return NULL;
    cxl_connection_init_runtime_defaults(conn);

    conn->ctx = ctx;
    conn->caps = caps;
    if (addrs)
        conn->addrs = *addrs;
    if (caps & CXL_CONN_CAP_REQUEST_RX) {
        conn->mq_entries = mq_entries;
        conn->mq_total_lines =
            (mq_entries + CXL_MQ_ENTRIES_PER_LINE - 1u) /
            CXL_MQ_ENTRIES_PER_LINE;
        conn->mq_phase = 1;  /* Must match RPC engine's initial phase (true=1) */
        cxl_mq_payload_prepare_reset_for_head(conn);
    }

    cxl_lib_debugf("%s caps=%#x mq_entries=%u doorbell=%#llx metadata=%#llx request=%#llx response=%#llx flag=%#llx node_id=%u",
                   label ? label : "connection_create",
                   caps, conn->mq_entries,
                   (unsigned long long)conn->addrs.doorbell_addr,
                   (unsigned long long)conn->addrs.metadata_queue_addr,
                   (unsigned long long)conn->addrs.request_data_addr,
                   (unsigned long long)conn->addrs.response_data_addr,
                   (unsigned long long)conn->addrs.flag_addr,
                   (unsigned)conn->addrs.node_id);

    if (cxl_req_id_allocator_init(conn) != 0) {
        cxl_connection_destroy(conn);
        return NULL;
    }

    if (connection_finalize_setup(conn, metadata_clear_size) != 0) {
        cxl_lib_errorf("%s: finalize_setup failed doorbell=%#llx metadata=%#llx request=%#llx response=%#llx flag=%#llx",
                       label ? label : "connection_create",
                       (unsigned long long)conn->addrs.doorbell_addr,
                       (unsigned long long)conn->addrs.metadata_queue_addr,
                       (unsigned long long)conn->addrs.request_data_addr,
                       (unsigned long long)conn->addrs.response_data_addr,
                       (unsigned long long)conn->addrs.flag_addr);
        cxl_connection_destroy(conn);
        return NULL;
    }

    cxl_lib_debugf("%s done metadata_ptr=%p request_ptr=%p response_ptr=%p flag_ptr=%p",
                   label ? label : "connection_create",
                   (void *)conn->metadata_queue,
                   (void *)conn->request_data,
                   (void *)conn->response_data,
                   (void *)conn->flag);

    return conn;
}

cxl_connection_t *cxl_connection_create_server_poll_owner(
    cxl_context_t *ctx,
    const cxl_connection_addrs_t *addrs,
    uint32_t mq_entries)
{
    return cxl_connection_create_internal(
        ctx, addrs, mq_entries,
        CXL_CONN_CAP_REQUEST_RX |
            CXL_CONN_CAP_BOOTSTRAP |
            CXL_CONN_CAP_HEAD_SYNC,
        "connection_create_server_poll_owner");
}

cxl_connection_t *cxl_connection_create_client_attach(
    cxl_context_t *ctx,
    const cxl_connection_addrs_t *addrs)
{
    return cxl_connection_create_internal(
        ctx, addrs, 0,
        CXL_CONN_CAP_REQUEST_TX |
            CXL_CONN_CAP_RESPONSE_RX,
        "connection_create_client_attach");
}

cxl_connection_t *cxl_connection_create_response_tx(cxl_context_t *ctx)
{
    return cxl_connection_create_internal(
        ctx, NULL, 0,
        CXL_CONN_CAP_RESPONSE_TX,
        "connection_create_response_tx");
}

void cxl_connection_destroy(cxl_connection_t *conn)
{
    if (!conn)
        return;

    cxl_copyengine_disable(conn);

    if (conn->sync)
        cxl_sync_destroy(conn->sync);

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
    if (!cxl_connection_require_cap(conn, CXL_CONN_CAP_RESPONSE_TX,
                                    "cxl_connection_bind_copyengine_lane"))
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
    if (!cxl_connection_require_cap(conn, CXL_CONN_CAP_RESPONSE_TX,
                                    "cxl_connection_bind_copyengine_lane_index"))
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
    if (!cxl_connection_require_cap(conn, CXL_CONN_CAP_REQUEST_TX,
                                    "cxl_send_request"))
        return -1;

    if (len > 0 && !data)
        return -1;
    if (len > CXL_REQ_PAYLOAD_SOFT_MAX)
        return -1;

    uint8_t doorbell_buf[16] __attribute__((aligned(16)));
    uint16_t req_id = 0;

    if (len <= 8) {
        if (cxl_request_issue_prepare(conn, 0, NULL, &req_id) != 0)
            return -1;

        /* Inline mode: data fits in doorbell */
        uint64_t inline_data = 0;
        if (data)
            memcpy(&inline_data, data, len);

        cxl_build_doorbell(doorbell_buf, CXL_METHOD_REQUEST, 1,
                           (uint32_t)len, conn->addrs.node_id,
                           req_id, inline_data);
    } else {
        /* Non-inline: write request_data entry, then doorbell */
        /*
         * request_data publish uses 64B cacheline slots.
         * Doorbell header carries full payload length (32 bits), while
         * request_data stores payload bytes only (no in-band length header).
         */
        size_t entry_size = cxl_request_entry_size(len);
        size_t offset = 0;
        if (cxl_request_issue_prepare(conn, (uint32_t)entry_size,
                                      &offset, &req_id) != 0)
            return -1;
        volatile uint8_t *entry_ptr = conn->request_data + offset;
        cxl_publish_request_payload(entry_ptr, data, len);

        /* Compute logical/protocol address for doorbell data field. */
        uint64_t entry_phys = conn->addrs.request_data_addr + (uint64_t)offset;

        cxl_build_doorbell(doorbell_buf, CXL_METHOD_REQUEST, 0,
                           (uint32_t)len, conn->addrs.node_id,
                           req_id, entry_phys);
    }

    /* Publish doorbell via the same store+flush semantic path as payload. */
    cxl_publish_doorbell_raw(conn->doorbell, doorbell_buf);

    return (int)req_id;
}

/* forward declarations for client response drain helpers */
int cxl_peek_response_producer_cursor(cxl_connection_t *conn,
                                      uint64_t *out_cursor);

int cxl_consume_next_response(cxl_connection_t *conn,
                              void *out_data,
                              size_t *out_len,
                              uint16_t *out_rpc_id);

static void
cxl_response_peek_reset(cxl_connection_t *conn)
{
    if (!conn)
        return;

    conn->resp_peek_cursor = 0;
    conn->resp_peek_len = 0;
    conn->resp_peek_rpc_id = 0;
    conn->resp_peek_valid = 0;
    conn->resp_peek_payload_loaded = 0;
}

static inline const volatile uint8_t *
cxl_response_peek_payload_ptr(const cxl_connection_t *conn)
{
    size_t resp_offset = 0;

    if (!conn || conn->addrs.response_data_size == 0)
        return NULL;

    resp_offset =
        (size_t)(conn->resp_peek_cursor % conn->addrs.response_data_size);
    return conn->response_data + resp_offset + CXL_RESP_HEADER_LEN;
}

static void
cxl_prepare_response_payload_lines(volatile uint8_t *resp,
                                   size_t entry_size)
{
    if (!resp || entry_size <= 64u)
        return;

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
    cxl_prefetch_range_ro((const volatile uint8_t *)(entry_line_start + 64u),
                          entry_size - 64u);
}

static int
cxl_try_cached_response_entry(cxl_connection_t *conn,
                              int load_payload,
                              const volatile uint8_t **out_payload,
                              size_t *out_len,
                              uint16_t *out_rpc_id)
{
    if (!conn || !conn->resp_peek_valid ||
        conn->resp_peek_cursor != conn->resp_read_cursor) {
        return 0;
    }

    if (load_payload && !conn->resp_peek_payload_loaded) {
        size_t resp_offset =
            (size_t)(conn->resp_peek_cursor % conn->addrs.response_data_size);
        volatile uint8_t *resp = conn->response_data + resp_offset;
        size_t entry_size = cxl_response_entry_size(conn->resp_peek_len);
        cxl_prepare_response_payload_lines(resp, entry_size);
        conn->resp_peek_payload_loaded = 1;
    }

    if (out_payload)
        *out_payload = cxl_response_peek_payload_ptr(conn);
    if (out_len)
        *out_len = conn->resp_peek_len;
    if (out_rpc_id)
        *out_rpc_id = conn->resp_peek_rpc_id;
    return 1;
}

static int
cxl_parse_next_response_entry(cxl_connection_t *conn,
                              int load_payload,
                              const volatile uint8_t **out_payload,
                              size_t *out_len,
                              uint16_t *out_rpc_id)
{
    size_t resp_offset = 0;
    size_t msg_len = 0;
    size_t entry_size = 0;

    if (out_payload)
        *out_payload = NULL;
    if (out_len)
        *out_len = 0;
    if (out_rpc_id)
        *out_rpc_id = 0;

    resp_offset =
        (size_t)(conn->resp_read_cursor % conn->addrs.response_data_size);
    volatile uint8_t *resp = conn->response_data + resp_offset;
    __asm__ __volatile__("clflushopt (%0)" :: "r"((void *)resp) : "memory");
    cxl_invalidate_load_barrier();
    uint64_t header = *(volatile uint64_t *)resp;
    uint32_t payload_len_u32 = (uint32_t)(header & 0xFFFFFFFFu);
    uint16_t resp_req_id = (uint16_t)((header >> 32) & 0xFFFFu);

    if (resp_req_id == 0) {
        cxl_response_peek_reset(conn);
        return 0;
    }

    msg_len = (size_t)payload_len_u32;
    entry_size = cxl_response_entry_size(msg_len);
    if (entry_size > conn->addrs.response_data_size)
        return -1;
    if (resp_offset + entry_size > conn->addrs.response_data_size)
        return -1;
    if (resp_req_id > (uint16_t)CXL_RPC_ID_MASK)
        return -1;

    if (load_payload)
        cxl_prepare_response_payload_lines(resp, entry_size);

    conn->resp_peek_cursor = conn->resp_read_cursor;
    conn->resp_peek_len = msg_len;
    conn->resp_peek_rpc_id = resp_req_id;
    conn->resp_peek_valid = 1;
    conn->resp_peek_payload_loaded = load_payload ? 1u : 0u;

    if (out_payload)
        *out_payload = cxl_response_peek_payload_ptr(conn);
    if (out_len)
        *out_len = msg_len;
    if (out_rpc_id)
        *out_rpc_id = resp_req_id;
    return 1;
}

static int
cxl_read_response_producer_cursor(cxl_connection_t *conn,
                                  uint64_t *out_cursor)
{
    uint64_t cursor = 0;

    if (out_cursor)
        *out_cursor = 0;

    if (!conn || !out_cursor)
        return -1;

    volatile uint8_t *flag = conn->flag;
    __asm__ __volatile__("clflushopt (%0)" :: "r"((void *)flag) : "memory");
    cxl_invalidate_load_barrier();
    memcpy(&cursor, (const void *)flag, sizeof(cursor));

    if (cursor < conn->resp_read_cursor)
        return -1;

    conn->resp_known_producer_cursor = cursor;
    *out_cursor = cursor;
    return 0;
}

static int
cxl_skip_response_wrap_gap(cxl_connection_t *conn,
                           uint64_t producer_cursor)
{
    size_t region_size = 0;
    size_t resp_offset = 0;
    uint64_t wrap_cursor = 0;

    if (!conn || conn->addrs.response_data_size == 0)
        return -1;
    if (producer_cursor <= conn->resp_read_cursor)
        return -1;

    region_size = conn->addrs.response_data_size;
    resp_offset = (size_t)(conn->resp_read_cursor % region_size);
    if (resp_offset == 0)
        return -1;

    wrap_cursor = conn->resp_read_cursor + (uint64_t)(region_size - resp_offset);
    if (wrap_cursor > producer_cursor)
        return -1;

    conn->resp_read_cursor = wrap_cursor;
    cxl_response_peek_reset(conn);
    return 1;
}

static inline int
cxl_commit_response_head(cxl_connection_t *conn,
                         size_t msg_len)
{
    if (!conn)
        return -1;

    cxl_response_peek_reset(conn);
    conn->resp_read_cursor += (uint64_t)cxl_response_entry_size(msg_len);
    return 0;
}

static int
cxl_prepare_next_response_entry(cxl_connection_t *conn,
                                int load_payload,
                                const volatile uint8_t **out_payload,
                                size_t *out_len,
                                uint16_t *out_rpc_id)
{
    uint64_t producer_cursor = 0;

    if (out_payload)
        *out_payload = NULL;
    if (out_len)
        *out_len = 0;
    if (out_rpc_id)
        *out_rpc_id = 0;

    if (!conn || !out_len || !out_rpc_id)
        return -1;

    if (cxl_try_cached_response_entry(conn, load_payload,
                                      out_payload, out_len,
                                      out_rpc_id) == 1) {
        return 1;
    }

    producer_cursor = conn->resp_known_producer_cursor;
    if (producer_cursor <= conn->resp_read_cursor) {
        if (cxl_read_response_producer_cursor(conn, &producer_cursor) != 0)
            return -1;
    }
    if (producer_cursor == conn->resp_read_cursor)
        return 0;

    for (;;) {
        int peek_rc = cxl_parse_next_response_entry(conn, load_payload,
                                                    out_payload, out_len,
                                                    out_rpc_id);
        if (peek_rc == 1)
            return 1;
        if (peek_rc < 0)
            return -1;
        if (cxl_skip_response_wrap_gap(conn, producer_cursor) != 1)
            return -1;
    }
}

int cxl_peek_response_producer_cursor(cxl_connection_t *conn,
                                      uint64_t *out_cursor)
{
    uint64_t producer_cursor = 0;

    if (out_cursor)
        *out_cursor = 0;

    if (!cxl_connection_require_cap(conn, CXL_CONN_CAP_RESPONSE_RX,
                                    "cxl_peek_response_producer_cursor"))
        return -1;
    if (!out_cursor)
        return -1;

    if (cxl_read_response_producer_cursor(conn, &producer_cursor) != 0)
        return -1;

    *out_cursor = producer_cursor;
    return (producer_cursor != conn->resp_read_cursor) ? 1 : 0;
}

int cxl_peek_next_response_view(cxl_connection_t *conn,
                                const void **out_data_view,
                                size_t *out_len,
                                uint16_t *out_rpc_id)
{
    const volatile uint8_t *payload = NULL;

    if (out_data_view)
        *out_data_view = NULL;
    if (out_rpc_id)
        *out_rpc_id = 0;
    if (out_len)
        *out_len = 0;

    if (!cxl_connection_require_cap(conn, CXL_CONN_CAP_RESPONSE_RX,
                                    "cxl_peek_next_response_view"))
        return -1;
    if (!out_data_view || !out_len || !out_rpc_id)
        return -1;

    int peek_rc = cxl_prepare_next_response_entry(conn, 1,
                                                  &payload,
                                                  out_len,
                                                  out_rpc_id);
    if (peek_rc != 1)
        return peek_rc;

    *out_data_view = (const void *)payload;
    return 1;
}

int cxl_advance_response_head(cxl_connection_t *conn,
                              uint16_t rpc_id,
                              size_t len)
{
    size_t peek_len = 0;
    uint16_t peek_rpc_id = 0;

    if (!cxl_connection_require_cap(conn, CXL_CONN_CAP_RESPONSE_RX,
                                    "cxl_advance_response_head"))
        return -1;
    if (rpc_id == 0)
        return -1;

    int peek_rc =
        cxl_prepare_next_response_entry(conn, 0, NULL, &peek_len, &peek_rpc_id);
    if (peek_rc != 1)
        return peek_rc;
    if (peek_rpc_id != rpc_id || peek_len != len)
        return -1;

    if (cxl_commit_response_head(conn, len) != 0)
        return -1;
    return 1;
}

int cxl_consume_next_response_header(cxl_connection_t *conn,
                                     size_t *out_len,
                                     uint16_t *out_rpc_id)
{
    size_t msg_len = 0;
    uint16_t rpc_id = 0;

    if (out_len)
        *out_len = 0;
    if (out_rpc_id)
        *out_rpc_id = 0;

    if (!cxl_connection_require_cap(conn, CXL_CONN_CAP_RESPONSE_RX,
                                    "cxl_consume_next_response_header"))
        return -1;
    if (!out_len || !out_rpc_id)
        return -1;

    int peek_rc =
        cxl_prepare_next_response_entry(conn, 0, NULL, &msg_len, &rpc_id);
    if (peek_rc != 1)
        return peek_rc;

    if (cxl_commit_response_head(conn, msg_len) != 0)
        return -1;

    *out_len = msg_len;
    *out_rpc_id = rpc_id;
    return 1;
}

int cxl_consume_next_response(cxl_connection_t *conn,
                              void *out_data,
                              size_t *out_len,
                              uint16_t *out_rpc_id)
{
    if (out_rpc_id)
        *out_rpc_id = 0;

    if (!cxl_connection_require_cap(conn, CXL_CONN_CAP_RESPONSE_RX,
                                    "cxl_consume_next_response"))
        return -1;
    if (!out_data || !out_len || !out_rpc_id)
        return -1;

    size_t capacity = *out_len;
    const volatile uint8_t *payload = NULL;
    int peek_rc = cxl_prepare_next_response_entry(conn, 1,
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

    if (cxl_commit_response_head(conn, *out_len) != 0)
        return -1;
    return 1;
}

/* ================================================================
 * Server API
 * ================================================================ */

int cxl_poll_request_timed(cxl_connection_t *conn,
                           uint16_t *node_id,
                           uint16_t *rpc_id,
                           const void **out_data_view,
                           size_t *out_len,
                           cxl_request_poll_timing_t *out_timing)
{
    int ret = -1;
    const int mq_invalidate_consumed = CXL_MQ_INVALIDATE_CONSUMED;
    const int mq_invalidate_prefetched = CXL_MQ_INVALIDATE_PREFETCHED;
    const uint32_t mq_prefetch_lines = CXL_MQ_PREFETCH_LINES;
    const uint32_t mq_total_lines = conn ? conn->mq_total_lines : 0;
    uint64_t notify_ready_tick = 0;
    uint64_t req_data_ready_tick = 0;
    uint64_t poll_done_tick = 0;

    if (out_timing) {
        out_timing->notify_ready_tick = 0;
        out_timing->req_data_ready_tick = 0;
        out_timing->poll_done_tick = 0;
    }

    if (out_data_view)
        *out_data_view = NULL;
    if (out_len)
        *out_len = 0;
    if (!cxl_connection_require_cap(conn, CXL_CONN_CAP_REQUEST_RX,
                                    "cxl_poll_request") ||
        !conn->metadata_queue || !out_data_view || !out_len) {
        goto out;
    }

    /* Read metadata queue entry at head position */
    uint32_t head_idx = conn->mq_head;
    volatile uint8_t *entry = conn->metadata_queue +
                               (head_idx * CXL_METADATA_ENTRY_SIZE);
    uint32_t head_line_idx = head_idx / CXL_MQ_ENTRIES_PER_LINE;

    uint64_t meta_lo = *(volatile uint64_t *)entry;
    uint64_t meta_hi = 0;
    uint8_t entry_phase = (uint8_t)((meta_lo >> 2) & 0x01u);
    uint8_t expected_phase = conn->mq_phase & 0x01;

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
                cxl_mq_invalidate_lines_nofence(conn->metadata_queue,
                                                head_line_idx,
                                                1,
                                                mq_total_lines);
                cxl_mq_invalidate_lines_nofence(conn->metadata_queue,
                                                prefetched_start,
                                                prefetched_nr,
                                                mq_total_lines);
                __asm__ __volatile__("sfence" ::: "memory");
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
    uint32_t slot_in_line = head_idx % CXL_MQ_ENTRIES_PER_LINE;

    if (node_id) *node_id = nid;
    if (rpc_id)  *rpc_id = rid;

    /* Extract payload */
    if (mq_method != CXL_METHOD_REQUEST || rid == 0) {
        cxl_lib_errorf("poll_request: unexpected metadata entry head=%u "
                       "method=%u inline=%u phase=%u len=%u node=%u rpc_id=%u "
                       "meta_lo=%#llx meta_hi=%#llx",
                       head_idx,
                       (unsigned)mq_method,
                       (unsigned)mq_inline,
                       (unsigned)entry_phase,
                       (unsigned)mq_len,
                       (unsigned)nid,
                       (unsigned)rid,
                       (unsigned long long)meta_lo,
                       (unsigned long long)meta_hi);
        cxl_advance_mq_head_with_policy(conn, head_idx, head_line_idx,
                                        mq_invalidate_consumed);
        cxl_mq_payload_prepare_after_advance(conn, head_idx);
        ret = -1;
        goto out;
    }

    notify_ready_tick = cxl_trace_tick();

    if (mq_inline) {
        /* Inline: payload view is in bytes 8-15. */
        size_t msg_len = mq_len <= 8 ? (size_t)mq_len : 8;
        *out_data_view = (const void *)(entry + 8);
        *out_len = msg_len;
        req_data_ready_tick = notify_ready_tick;
    } else {
        /* Non-inline: bytes 8-15 contain an absolute logical payload address. */
        uint64_t data_addr = meta_hi;
        size_t msg_len = (size_t)mq_len;
        const uint8_t prepared_bit = (uint8_t)(1u << slot_in_line);
        if (msg_len == 0 || msg_len > CXL_REQ_PAYLOAD_SOFT_MAX) {
            cxl_advance_mq_head_with_policy(conn, head_idx, head_line_idx,
                                            mq_invalidate_consumed);
            cxl_mq_payload_prepare_after_advance(conn, head_idx);
            ret = -1;
            goto out;
        }

        volatile uint8_t *data_ptr =
            cxl_request_payload_ptr_trusted(conn, data_addr);
        if ((conn->mq_payload_prepared_mask & prepared_bit) == 0)
            cxl_prepare_payload_for_direct_read(data_ptr, msg_len);
        *out_data_view = (const void *)data_ptr;
        *out_len = msg_len;
        req_data_ready_tick =
            ((conn->mq_payload_prepared_mask & prepared_bit) != 0) ?
            notify_ready_tick :
            cxl_trace_tick();
    }

    cxl_prepare_future_payloads_same_line(conn, head_idx, head_line_idx,
                                          expected_phase);

    /* Advance head */
    cxl_advance_mq_head_with_policy(conn, head_idx, head_line_idx,
                                    mq_invalidate_consumed);
    cxl_mq_payload_prepare_after_advance(conn, head_idx);
    poll_done_tick = cxl_trace_tick();

    if (out_timing) {
        out_timing->notify_ready_tick = notify_ready_tick;
        out_timing->req_data_ready_tick = req_data_ready_tick;
        out_timing->poll_done_tick = poll_done_tick;
    }

    ret = 1;
out:
    return ret;
}

int cxl_poll_request(cxl_connection_t *conn,
                     uint16_t *node_id,
                     uint16_t *rpc_id,
                     const void **out_data_view,
                     size_t *out_len)
{
    return cxl_poll_request_timed(conn,
                                  node_id,
                                  rpc_id,
                                  out_data_view,
                                  out_len,
                                  NULL);
}

/* ================================================================
 * Server Response API
 * ================================================================ */

static int
cxl_write_response_entry_cpu(cxl_connection_t *conn,
                             uint16_t rpc_id,
                             const void *data,
                             size_t len,
                             size_t dst_resp_offset)
{
    volatile uint8_t *dst_resp = NULL;
    const uint64_t header = cxl_pack_response_header((uint32_t)len, rpc_id);

    if (!conn || !conn->peer_response_data)
        return -1;

    dst_resp = conn->peer_response_data + dst_resp_offset;
    memcpy((void *)dst_resp, &header, sizeof(header));
    if (len > 0)
        memcpy((void *)(dst_resp + sizeof(header)), data, len);
    cxl_flush_range(dst_resp, sizeof(header) + len);
    return 0;
}

static int
cxl_publish_response_cursor_cpu(cxl_connection_t *conn,
                                uint64_t producer_cursor)
{
    if (!conn || !conn->peer_flag)
        return -1;

    memcpy((void *)conn->peer_flag, &producer_cursor, CXL_FLAG_PUBLISH_LEN);
    cxl_flush_range(conn->peer_flag, CXL_FLAG_PUBLISH_LEN);
    return 0;
}

static int
cxl_response_publish_needs_dma_serialize(cxl_connection_t *conn)
{
    if (!conn)
        return -1;
    if (!conn->ce_lane_assigned)
        return 0;
    return cxl_copyengine_response_publish_pending(conn);
}

static int
cxl_prepare_response_tx_path(cxl_connection_t *conn)
{
    if (!cxl_connection_require_cap(conn, CXL_CONN_CAP_RESPONSE_TX,
                                    "cxl_prepare_response_tx_path"))
        return -1;

    conn->resp_tx_ready = 0;
    if (!conn->peer_response_data || conn->peer_response_data_size == 0 ||
        !conn->peer_flag || conn->peer_flag_addr == 0) {
        return 0;
    }

    conn->resp_tx_ready = 1;
    return 0;
}

static int
cxl_prepare_response_tx_copyengine_flag(cxl_connection_t *conn)
{
    if (!conn || !conn->ce_lane_bind_valid)
        return -1;
    if (cxl_copyengine_prepare(conn) != 0)
        return -1;
    if (!conn->ce_peer_flag_phys_valid) {
        if (cxl_copyengine_update_peer_flag_mapping(conn) != 0)
            return -1;
    }
    if (!conn->ce_peer_flag_phys_valid || conn->ce_peer_flag_phys == 0)
        return -1;
    return 0;
}

static int
cxl_prepare_response_tx_copyengine_response(cxl_connection_t *conn)
{
    if (cxl_prepare_response_tx_copyengine_flag(conn) != 0)
        return -1;
    if (!conn->ce_peer_resp_page_phys || conn->ce_peer_resp_page_count == 0) {
        if (cxl_copyengine_update_peer_response_mapping(conn) != 0)
            return -1;
    }
    if (cxl_copyengine_validate_submit_invariants(conn) != 0)
        return -1;
    return 0;
}

static int
cxl_publish_response_wrap_sentinel(cxl_connection_t *conn,
                                   size_t gap_offset,
                                   size_t gap_size)
{
    uint64_t zero_header = 0;

    if (!conn || !conn->peer_response_data || gap_size == 0)
        return 0;
    if (gap_size < CXL_RESP_HEADER_LEN)
        return -1;

    memcpy((void *)(conn->peer_response_data + gap_offset),
           &zero_header, CXL_RESP_HEADER_LEN);
    cxl_flush_range(conn->peer_response_data + gap_offset,
                    CXL_RESP_HEADER_LEN);
    return 0;
}

static int
cxl_reserve_peer_response_window(const cxl_connection_t *conn,
                                 size_t entry_size,
                                 size_t *out_offset,
                                 uint64_t *out_next_cursor,
                                 size_t *out_gap_offset,
                                 size_t *out_gap_size)
{
    uint64_t write_cursor = 0;
    size_t offset = 0;
    size_t gap_size = 0;

    if (out_offset)
        *out_offset = 0;
    if (out_next_cursor)
        *out_next_cursor = 0;
    if (out_gap_offset)
        *out_gap_offset = 0;
    if (out_gap_size)
        *out_gap_size = 0;

    if (!conn || !out_offset || !out_next_cursor || entry_size == 0)
        return -1;
    if (entry_size > conn->peer_response_data_size)
        return -1;

    write_cursor = conn->peer_resp_write_cursor;
    offset = (size_t)(write_cursor % (uint64_t)conn->peer_response_data_size);
    if (offset + entry_size > conn->peer_response_data_size) {
        gap_size = conn->peer_response_data_size - offset;
        if (gap_size < CXL_RESP_HEADER_LEN)
            return -1;
        if (write_cursor > UINT64_MAX - (uint64_t)gap_size)
            return -1;
        if (out_gap_offset)
            *out_gap_offset = offset;
        if (out_gap_size)
            *out_gap_size = gap_size;
        write_cursor += (uint64_t)gap_size;
        offset = 0;
    }
    if (write_cursor > UINT64_MAX - (uint64_t)entry_size)
        return -1;

    *out_offset = offset;
    *out_next_cursor = write_cursor + (uint64_t)entry_size;
    return 0;
}

int cxl_connection_set_peer_response_data(cxl_connection_t *conn,
                                          uint64_t peer_response_data_addr,
                                          size_t peer_response_data_size)
{
    if (!cxl_connection_require_cap(conn, CXL_CONN_CAP_RESPONSE_TX,
                                    "cxl_connection_set_peer_response_data"))
        return -1;
    if (!conn->ctx)
        return -1;

    if (peer_response_data_addr == 0 || peer_response_data_size == 0)
        return -1;

    if (!cxl_rpc_phys_range_valid(conn->ctx,
                                  peer_response_data_addr,
                                  peer_response_data_size)) {
        return -1;
    }

    /* Map peer logical region locally once for direct response publish. */
    volatile void *response_ptr = cxl_rpc_phys_to_virt(conn->ctx,
                                                        peer_response_data_addr);

    if (!response_ptr)
        return -1;

    /* All checks passed, commit the configuration */
    free(conn->ce_peer_resp_page_phys);
    conn->ce_peer_resp_page_phys = NULL;
    conn->ce_peer_resp_logical_page_base = 0;
    conn->ce_peer_resp_page_size = 0;
    conn->ce_peer_resp_page_count = 0;
    conn->ce_peer_resp_virt_page_base = 0;
    conn->peer_response_data_addr = peer_response_data_addr;
    conn->peer_response_data_size = peer_response_data_size;
    conn->peer_response_data = (volatile uint8_t *)response_ptr;
    /*
     * Response payload windows stay fully on-demand: CPU publish flushes only
     * the written entry, response RX invalidates only the consumed entry, and
     * large-response DMA resolves destination pages lazily as they are touched.
     */
    conn->peer_resp_write_cursor = 0;
    if (cxl_prepare_response_tx_path(conn) != 0)
        return -1;

    return 0;
}

int cxl_connection_set_peer_response_flag_addr(cxl_connection_t *conn,
                                               uint64_t peer_flag_addr)
{
    if (!cxl_connection_require_cap(conn, CXL_CONN_CAP_RESPONSE_TX,
                                    "cxl_connection_set_peer_response_flag_addr"))
        return -1;
    if (!conn->ctx)
        return -1;

    if (peer_flag_addr == 0)
        return -1;

    if (!cxl_rpc_phys_range_valid(conn->ctx, peer_flag_addr,
                                  CXL_DEFAULT_FLAG_SIZE))
        return -1;

    /* Map peer logical flag locally once for direct producer-cursor publish. */
    volatile void *flag_ptr = cxl_rpc_phys_to_virt(conn->ctx, peer_flag_addr);
    if (!flag_ptr)
        return -1;

    conn->ce_peer_flag_phys = 0;
    conn->ce_peer_flag_phys_valid = 0;
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
    int publish_via_dma = 0;
    size_t entry_size = 0;
    size_t publish_bytes = 0;
    size_t offset = 0;
    size_t gap_offset = 0;
    size_t gap_size = 0;
    uint64_t next_cursor = 0;

    if (!cxl_connection_require_cap(conn, CXL_CONN_CAP_RESPONSE_TX,
                                    "cxl_send_response"))
        return -1;
    if (!conn->resp_tx_ready)
        return -1;
    if (len > 0 && !data)
        return -1;
    if (rpc_id == 0 || rpc_id > CXL_RPC_ID_MASK)
        return -1;
    if (!cxl_response_payload_len_valid(len))
        return -1;

    publish_bytes = cxl_response_publish_bytes(len);

    if (publish_bytes <= (size_t)CXL_RESPONSE_DMA_THRESHOLD) {
        publish_via_dma = cxl_response_publish_needs_dma_serialize(conn);
        if (publish_via_dma < 0)
            return -1;
        if (publish_via_dma == 1 &&
            cxl_prepare_response_tx_copyengine_flag(conn) != 0) {
            return -1;
        }
    }

    entry_size = cxl_response_entry_size(len);
    if (cxl_reserve_peer_response_window(conn, entry_size,
                                         &offset, &next_cursor,
                                         &gap_offset, &gap_size) != 0)
        return -1;
    if (cxl_publish_response_wrap_sentinel(conn, gap_offset, gap_size) != 0)
        return -1;

    if (publish_bytes > (size_t)CXL_RESPONSE_DMA_THRESHOLD) {
        if (cxl_prepare_response_tx_copyengine_response(conn) != 0)
            return -1;
        if (cxl_copyengine_submit_response_async(conn, rpc_id, data, len,
                                                 next_cursor, offset) != 0)
            return -1;
    } else {
        if (cxl_write_response_entry_cpu(conn, rpc_id, data, len,
                                         offset) != 0) {
            return -1;
        }
        if (publish_via_dma == 1) {
            if (cxl_copyengine_submit_flag_async(conn, next_cursor) != 0)
                return -1;
        } else if (cxl_publish_response_cursor_cpu(conn, next_cursor) != 0) {
            return -1;
        }
    }

    conn->peer_resp_write_cursor = next_cursor;

    return 0;
}
