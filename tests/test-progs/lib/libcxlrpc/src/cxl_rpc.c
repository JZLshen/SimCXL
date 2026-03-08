/*
 * CXL RPC Core Library - Implementation
 *
 * Provides context management (KM NUMA-backed allocation),
 * connection lifecycle, client send/poll, and server poll/send.
 */

#include "cxl_rpc_internal.h"

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

static inline size_t
cxl_response_entry_size(size_t payload_len)
{
    const size_t header_size = 12;
    const size_t total_size = header_size + payload_len;
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

/* ================================================================
 * Low-level Utilities
 * ================================================================ */

static void cxl_build_doorbell(uint8_t *buf, uint8_t method, uint8_t is_inline,
                               uint16_t service_id, uint16_t method_id,
                               uint16_t request_id, uint32_t payload_len,
                               uint64_t data)
{
    if (!buf)
        return;

    /* Fill all 16 bytes directly: [0..7]=header, [8..15]=data (LE). */
    uint64_t header_lo = 0;
    header_lo |= ((uint64_t)(method & 0x03u));
    header_lo |= ((uint64_t)(is_inline & 0x01u) << 2);
    header_lo |= ((uint64_t)(payload_len & CXL_REQ_META_LEN_MAX) << 4);
    header_lo |= ((uint64_t)(service_id & 0x0FFFu) << 24);
    header_lo |= ((uint64_t)(method_id & 0x0FFFu) << 36);
    header_lo |= ((uint64_t)(request_id & 0xFFFFu) << 48);

    memcpy(buf, &header_lo, sizeof(header_lo));
    memcpy(buf + 8, &data, sizeof(data));
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

    /* Initialize global allocator for this CXL region */
    ctx->allocator = cxl_global_alloc_init(cxl_base, cxl_size);
    if (!ctx->allocator) {
        if (ctx->base && ctx->shm_fd >= 0) {
            munmap((void *)ctx->base, cxl_size);
            close(ctx->shm_fd);
        }
        free(ctx);
        return NULL;
    }

    return ctx;
}

void cxl_rpc_destroy(cxl_context_t *ctx)
{
    if (!ctx)
        return;

    if (ctx->allocator)
        cxl_global_alloc_destroy(ctx->allocator);

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

static inline uint32_t
cxl_req_id_prng_next(cxl_connection_t *conn)
{
    uint32_t x = conn->req_id_prng_state;
    if (x == 0)
        x = 0xA341316Cu;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    conn->req_id_prng_state = x;
    return x;
}

static void
cxl_req_id_allocator_init(cxl_connection_t *conn)
{
    if (!conn)
        return;

    uint64_t mix = 0x9E3779B97F4A7C15ULL ^
                   conn->addrs.doorbell_addr ^
                   conn->addrs.metadata_queue_addr ^
                   conn->addrs.request_data_addr ^
                   (((uint64_t)(uintptr_t)conn) >> 4);
    uint32_t seed = (uint32_t)(mix ^ (mix >> 32));
    if (seed == 0)
        seed = 0xA5A5A5A5u;
    conn->req_id_prng_state = seed;
}

static inline uint16_t
cxl_generate_request_id(cxl_connection_t *conn)
{
    uint8_t prefix_bits = conn->req_id_prefix_bits;
    uint16_t seq_mask = CXL_REQ_ID_MASK;
    uint16_t prefix_base = 0;
    if (prefix_bits > 0) {
        seq_mask = (uint16_t)((1u << (16u - prefix_bits)) - 1u);
        prefix_base = (uint16_t)(conn->req_id_prefix << (16u - prefix_bits));
    }
    uint16_t seq = (uint16_t)(cxl_req_id_prng_next(conn) & seq_mask);
    if (seq == 0)
        seq = 1;
    return (uint16_t)(prefix_base | seq);
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

/*
 * Touch the first byte with a read so page-table setup happens before hot-path
 * accesses, without creating dirty cache lines on DMA destination regions.
 */
static inline void
cxl_prefault_first_byte_ro(volatile uint8_t *ptr, size_t size)
{
    if (!ptr || size == 0)
        return;

    volatile uint8_t v = ptr[0];
    (void)v;
}

static void
cxl_prefault_each_page_ro(volatile uint8_t *ptr, size_t size)
{
    if (!ptr || size == 0)
        return;

    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) {
        cxl_prefault_first_byte_ro(ptr, size);
        return;
    }

    size_t page_size = (size_t)ps;
    for (size_t off = 0; off < size; off += page_size) {
        volatile uint8_t v = ptr[off];
        (void)v;
    }

    volatile uint8_t tail = ptr[size - 1];
    (void)tail;
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
connection_register_remote_translation(cxl_connection_t *conn)
{
    if (!conn || !conn->doorbell)
        return -1;

    uint8_t doorbell_buf[16] __attribute__((aligned(16)));
    cxl_build_doorbell(doorbell_buf, CXL_METHOD_CONTROL, 1,
                       CXL_CONTROL_SERVICE_INTERNAL,
                       CXL_CONTROL_METHOD_REGISTER_TRANSLATION,
                       0, 0, conn->addrs.doorbell_addr);
    /* Publish order: store payload, flush touched cacheline(s), then sfence. */
    memcpy((void *)conn->doorbell, doorbell_buf, CXL_DOORBELL_PUBLISH_LEN);
    uintptr_t line_start = ((uintptr_t)conn->doorbell) & ~((uintptr_t)63);
    uintptr_t line_end =
        (((uintptr_t)conn->doorbell + CXL_DOORBELL_PUBLISH_LEN) + 63u) &
        ~((uintptr_t)63);
    for (uintptr_t line = line_start; line < line_end; line += 64u) {
        __asm__ __volatile__(
            "clflushopt (%0)"
            :
            : "r"((void *)line)
            : "memory");
    }
    __asm__ __volatile__("sfence" ::: "memory");
    return 0;
}

static int
cxl_virt_to_phys_local(const void *vaddr, uint64_t *phys_out)
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
        if (pagemap_fd < 0) {
            cxl_lib_errorf("virt_to_phys: open(/proc/self/pagemap) failed: %s",
                           strerror(errno));
            return -1;
        }
    }
    if (pagemap_fd < 0)
        return -1;

    volatile const uint8_t *page_ptr =
        (const volatile uint8_t *)(uintptr_t)(virt & ~(page_size - 1u));
    volatile uint8_t page_touch = *page_ptr;
    (void)page_touch;

    off_t off = (off_t)((virt / page_size) * sizeof(uint64_t));
    ssize_t n = pread(pagemap_fd, &entry, sizeof(entry), off);
    if (n != (ssize_t)sizeof(entry)) {
        cxl_lib_errorf("virt_to_phys: pread(pagemap) failed vaddr=%p n=%zd err=%s",
                       vaddr, n, strerror(errno));
        return -1;
    }
    if ((entry & (1ULL << 63)) == 0) {
        cxl_lib_errorf("virt_to_phys: page not present vaddr=%p entry=%#llx",
                       vaddr, (unsigned long long)entry);
        return -1;
    }

    uint64_t pfn = entry & ((1ULL << 55) - 1);
    if (pfn == 0) {
        cxl_lib_errorf("virt_to_phys: PFN resolved to zero vaddr=%p entry=%#llx",
                       vaddr, (unsigned long long)entry);
        return -1;
    }

    *phys_out = pfn * page_size + (virt % page_size);
    return 0;
}

static int
connection_register_metadata_page_translation(cxl_connection_t *conn,
                                              uint64_t page_index,
                                              uint64_t observed_page_base)
{
    if (!conn || !conn->doorbell)
        return -1;

    uint8_t doorbell_buf[16] __attribute__((aligned(16)));
    cxl_build_doorbell(doorbell_buf, CXL_METHOD_CONTROL, 1,
                       CXL_CONTROL_SERVICE_INTERNAL,
                       CXL_CONTROL_METHOD_REGISTER_METADATA_PAGE_TRANSLATION,
                       (uint16_t)(page_index & 0xFFFFu),
                       (uint32_t)((page_index >> 16) & CXL_REQ_META_LEN_MAX),
                       observed_page_base);

    memcpy((void *)conn->doorbell, doorbell_buf, CXL_DOORBELL_PUBLISH_LEN);
    uintptr_t line_start = ((uintptr_t)conn->doorbell) & ~((uintptr_t)63);
    uintptr_t line_end =
        (((uintptr_t)conn->doorbell + CXL_DOORBELL_PUBLISH_LEN) + 63u) &
        ~((uintptr_t)63);
    for (uintptr_t line = line_start; line < line_end; line += 64u) {
        __asm__ __volatile__(
            "clflushopt (%0)"
            :
            : "r"((void *)line)
            : "memory");
    }
    __asm__ __volatile__("sfence" ::: "memory");
    return 0;
}

static int
connection_register_metadata_translation_pages(cxl_connection_t *conn,
                                               size_t metadata_size)
{
    if (!conn || !conn->ctx || !conn->metadata_queue || metadata_size == 0) {
        cxl_lib_errorf("metadata_translation_pages: invalid args conn=%p ctx=%p metadata=%p size=%zu",
                       (void *)conn,
                       conn ? (void *)conn->ctx : NULL,
                       conn ? (void *)conn->metadata_queue : NULL,
                       metadata_size);
        return -1;
    }

    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) {
        cxl_lib_errorf("metadata_translation_pages: sysconf(_SC_PAGESIZE) failed");
        return -1;
    }
    const uint64_t page_size = (uint64_t)ps;
    const uint64_t page_mask = ~(page_size - 1u);
    const uint64_t logical_start = conn->addrs.metadata_queue_addr;
    const uint64_t logical_end = logical_start + metadata_size;
    const uint64_t page_start = logical_start & page_mask;
    const uint64_t page_end = (logical_end + page_size - 1u) & page_mask;

    for (uint64_t logical_page = page_start;
         logical_page < page_end;
         logical_page += page_size) {
        size_t page_off = (size_t)(logical_page - logical_start);
        volatile uint8_t *virt_page = conn->metadata_queue + page_off;
        uint64_t observed_page = 0;
        if (cxl_virt_to_phys_local((const void *)virt_page, &observed_page) != 0) {
            cxl_lib_errorf("metadata_translation_pages: virt_to_phys failed logical_page=%#llx page_off=%zu virt=%p",
                           (unsigned long long)logical_page,
                           page_off,
                           (const void *)virt_page);
            return -1;
        }
        observed_page &= page_mask;

        const uint64_t page_index64 = (logical_page - page_start) / page_size;
        if (page_index64 > (((uint64_t)CXL_REQ_META_LEN_MAX << 16) | 0xFFFFu)) {
            cxl_lib_errorf("metadata_translation_pages: page index overflow index=%#llx logical_page=%#llx",
                           (unsigned long long)page_index64,
                           (unsigned long long)logical_page);
            return -1;
        }

        if (connection_register_metadata_page_translation(
                conn, page_index64, observed_page) != 0) {
            cxl_lib_errorf("metadata_translation_pages: register failed page_index=%#llx observed_page=%#llx",
                           (unsigned long long)page_index64,
                           (unsigned long long)observed_page);
            return -1;
        }
    }

    return 0;
}

static int connection_reset_remote_state(cxl_connection_t *conn)
{
    if (!conn || !conn->doorbell)
        return -1;

    uint8_t doorbell_buf[16] __attribute__((aligned(16)));
    cxl_build_doorbell(doorbell_buf, CXL_METHOD_CONTROL, 1,
                        0, 0, 0, 0, CXL_CONTROL_RESET_STATE);
    /* Publish order: store payload, flush touched cacheline(s), then sfence. */
    memcpy((void *)conn->doorbell, doorbell_buf, CXL_DOORBELL_PUBLISH_LEN);
    uintptr_t line_start = ((uintptr_t)conn->doorbell) & ~((uintptr_t)63);
    uintptr_t line_end =
        (((uintptr_t)conn->doorbell + CXL_DOORBELL_PUBLISH_LEN) + 63u) &
        ~((uintptr_t)63);
    for (uintptr_t line = line_start; line < line_end; line += 64u) {
        __asm__ __volatile__(
            "clflushopt (%0)"
            :
            : "r"((void *)line)
            : "memory");
    }
    __asm__ __volatile__("sfence" ::: "memory");
    return 0;
}

static int
connection_finalize_setup(cxl_connection_t *conn,
                          size_t metadata_clear_size,
                          int bootstrap_owner)
{
    if (!conn)
        return -1;

    connection_init_virt_ptrs(conn);
    cxl_prefault_each_page_ro(conn->metadata_queue, metadata_clear_size);
    cxl_prefault_first_byte_ro(conn->request_data, conn->addrs.request_data_size);
    cxl_prefault_first_byte_ro(conn->response_data, conn->addrs.response_data_size);
    cxl_prefault_first_byte_ro(conn->flag, CXL_DEFAULT_FLAG_SIZE);

    /*
     * The completion flag must start empty on every connection setup. We do
     * not eagerly zero the full response ring here because fixed-layout rings
     * are 10MiB and whole-range zero+flush is prohibitively expensive under
     * gem5 ATOMIC. Response publish uses CopyEngine source buffers that fully
     * overwrite the destination entry.
     */

    cxl_zero_and_flush_range(conn->flag, CXL_DEFAULT_FLAG_SIZE);

    const int do_destructive_bootstrap =
        conn->owns_addrs || bootstrap_owner;

    if (do_destructive_bootstrap && conn->metadata_queue && metadata_clear_size > 0) {
        cxl_zero_and_flush_range(conn->metadata_queue, metadata_clear_size);
    }


    if (conn->doorbell) {
        cxl_sync_config_t sync_cfg = {
            .queue_size_n = conn->mq_entries,
        };
        conn->sync = cxl_sync_init(&sync_cfg, (volatile void *)conn->doorbell);
    }

    if (connection_register_remote_translation(conn) != 0) {
        cxl_lib_errorf("connection_finalize_setup: remote translation register failed doorbell=%#llx",
                       (unsigned long long)conn->addrs.doorbell_addr);
        return -1;
    }
    if (connection_register_metadata_translation_pages(conn,
                                                       metadata_clear_size) != 0) {
        cxl_lib_errorf("connection_finalize_setup: metadata translation register failed metadata=%#llx size=%zu",
                       (unsigned long long)conn->addrs.metadata_queue_addr,
                       metadata_clear_size);
        return -1;
    }
    if (do_destructive_bootstrap) {
        if (connection_reset_remote_state(conn) != 0) {
            cxl_lib_errorf("connection_finalize_setup: reset remote state failed doorbell=%#llx",
                           (unsigned long long)conn->addrs.doorbell_addr);
            return -1;
        }
    }

    return 0;
}

cxl_connection_t *cxl_connection_create(cxl_context_t *ctx,
                                         uint32_t mq_entries)
{
    if (!ctx)
        return NULL;

    if (mq_entries == 0) mq_entries = 1024;

    cxl_connection_t *conn = calloc(1, sizeof(*conn));
    if (!conn)
        return NULL;
    cxl_connection_init_runtime_defaults(conn);

    cxl_lib_debugf("connection_create mq_entries=%u allocator=%p",
                   mq_entries, (void *)ctx->allocator);

    conn->ctx = ctx;
    conn->mq_entries = mq_entries;
    conn->owns_addrs = 1;
    conn->mq_phase = 1;  /* Must match RPC engine's initial phase (true=1) */
    conn->req_id_prefix = 0;
    conn->req_id_prefix_bits = 0;

    /* Allocate regions via global allocator */
    size_t mq_size = (size_t)mq_entries * CXL_METADATA_ENTRY_SIZE;
    int ret = cxl_global_alloc_connection(ctx->allocator,
                                           mq_size, 0, 0,
                                           &conn->addrs);
    if (ret < 0) {
        free(conn);
        return NULL;
    }
    cxl_req_id_allocator_init(conn);

    if (connection_finalize_setup(conn, mq_size, 1) != 0) {
        cxl_connection_destroy(conn);
        return NULL;
    }

    cxl_lib_debugf("connection_create done doorbell=%#llx metadata=%#llx request=%#llx response=%#llx flag=%#llx",
                   (unsigned long long)conn->addrs.doorbell_addr,
                   (unsigned long long)conn->addrs.metadata_queue_addr,
                   (unsigned long long)conn->addrs.request_data_addr,
                   (unsigned long long)conn->addrs.response_data_addr,
                   (unsigned long long)conn->addrs.flag_addr);

    return conn;
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
        addrs->metadata_queue_size < required_mq_size) {
        cxl_lib_errorf("connection_create_fixed(%s): invalid addrs doorbell=%#llx metadata=%#llx flag=%#llx metadata_size=%zu required=%zu",
                       bootstrap_owner ? "owner" : "attach",
                       (unsigned long long)addrs->doorbell_addr,
                       (unsigned long long)addrs->metadata_queue_addr,
                       (unsigned long long)addrs->flag_addr,
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

    cxl_lib_debugf("connection_create_fixed(%s) mq_entries=%u doorbell=%#llx metadata=%#llx request=%#llx response=%#llx flag=%#llx",
                   bootstrap_owner ? "owner" : "attach",
                   mq_entries,
                   (unsigned long long)addrs->doorbell_addr,
                   (unsigned long long)addrs->metadata_queue_addr,
                   (unsigned long long)addrs->request_data_addr,
                   (unsigned long long)addrs->response_data_addr,
                   (unsigned long long)addrs->flag_addr);

    conn->ctx = ctx;
    conn->addrs = *addrs;
    conn->owns_addrs = 0;
    conn->mq_entries = mq_entries;
    conn->mq_phase = 1;  /* Must match RPC engine's initial phase (true=1) */
    conn->req_id_prefix = 0;
    conn->req_id_prefix_bits = 0;
    cxl_req_id_allocator_init(conn);

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

    /* Free regions if we own them */
    if (conn->owns_addrs && conn->ctx && conn->ctx->allocator)
        cxl_global_free_connection(conn->ctx->allocator, &conn->addrs);

    free(conn);
}

const cxl_connection_addrs_t *cxl_connection_get_addrs(
    const cxl_connection_t *conn)
{
    return conn ? &conn->addrs : NULL;
}

int cxl_connection_set_request_id_prefix(cxl_connection_t *conn,
                                          uint16_t prefix,
                                          uint8_t prefix_bits)
{
    if (!conn)
        return -1;
    if (prefix_bits > 15)
        return -1;
    if (prefix_bits > 0 && prefix >= (1u << prefix_bits))
        return -1;

    conn->req_id_prefix = prefix;
    conn->req_id_prefix_bits = prefix_bits;
    cxl_req_id_allocator_init(conn);
    return 0;
}

/* ================================================================
 * Client API
 * ================================================================ */

int32_t cxl_send_request(cxl_connection_t *conn,
                          uint16_t service_id,
                          uint16_t method_id,
                          const void *data,
                          size_t len)
{
    /* Parameter validation */
    if (!conn || !conn->doorbell)
        return -1;

    /* Check data pointer when length is non-zero */
    if (len > 0 && !data)
        return -1;

    cxl_lib_debugf("send_request begin svc=%u method=%u len=%zu mq_head=%u mq_phase=%u doorbell=%#llx request_data=%#llx",
                   service_id, method_id, len, conn->mq_head, conn->mq_phase,
                   (unsigned long long)conn->addrs.doorbell_addr,
                   (unsigned long long)conn->addrs.request_data_addr);

    if (service_id > 0x0FFFu || method_id > 0x0FFFu)
        return -1;

    if (len > CXL_REQ_PAYLOAD_SOFT_MAX || len > (size_t)CXL_REQ_META_LEN_MAX)
        return -1;

    uint16_t req_id = cxl_generate_request_id(conn);

    uint8_t doorbell_buf[16] __attribute__((aligned(16)));

    if (len <= 8) {
        /* Inline mode: data fits in doorbell */
        uint64_t inline_data = 0;
        if (data)
            memcpy(&inline_data, data, len);

        cxl_build_doorbell(doorbell_buf, CXL_METHOD_REQUEST, 1,
                            service_id, method_id, req_id,
                            (uint32_t)len, inline_data);
    } else {
        /* Non-inline: write request_data entry, then doorbell */
        if (!conn->request_data || conn->addrs.request_data_size == 0)
            return -1;

        /*
         * request_data publish uses 64B cacheline slots.
         * Doorbell header carries full payload length (20 bits), while
         * request_data stores payload bytes only (no in-band length header).
         */
        size_t entry_size = cxl_request_entry_size(len);
        size_t offset = conn->req_write_offset;
        size_t region_size = conn->addrs.request_data_size;
        if (entry_size > region_size)
            return -1;
        if (offset + entry_size > region_size) {
            offset = 0;
        }
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

        /* Compute physical address for doorbell data field */
        uint64_t entry_phys = conn->addrs.request_data_addr +
                              ((uint8_t *)entry_ptr - (uint8_t *)conn->request_data);

        conn->req_write_offset = offset + entry_size;
        if (conn->req_write_offset >= region_size) {
            conn->req_write_offset = 0;
        }

        cxl_build_doorbell(doorbell_buf, CXL_METHOD_REQUEST, 0,
                            service_id, method_id, req_id,
                            (uint32_t)len, entry_phys);
    }

    /* Publish doorbell via the same store+flush semantic path as payload. */
    uintptr_t doorbell_line_start = ((uintptr_t)conn->doorbell) & ~((uintptr_t)63);
    uintptr_t doorbell_line_end =
        (((uintptr_t)conn->doorbell + CXL_DOORBELL_PUBLISH_LEN) + 63u) &
        ~((uintptr_t)63);
    /* Publish order: store payload, flush touched cacheline(s), then sfence. */
    memcpy((void *)conn->doorbell, doorbell_buf, CXL_DOORBELL_PUBLISH_LEN);
    for (uintptr_t line = doorbell_line_start; line < doorbell_line_end;
         line += 64u) {
        __asm__ __volatile__(
            "clflushopt (%0)"
            :
            : "r"((void *)line)
            : "memory");
    }
    __asm__ __volatile__("sfence" ::: "memory");

    cxl_lib_debugf("send_request done req_id=%u payload_len=%zu inline=%d doorbell_addr=%#llx",
                   req_id, len, len <= 8 ? 1 : 0,
                   (unsigned long long)conn->addrs.doorbell_addr);

    return (int32_t)((uint32_t)req_id);
}

/* forward declarations for client response polling helpers */
int cxl_peek_latest_response_request_id(cxl_connection_t *conn,
                                         uint32_t *out_request_id);

int cxl_poll_next_response_view(cxl_connection_t *conn,
                                 const void **out_data_view,
                                 size_t *out_len,
                                 uint32_t *out_response_request_id);

static int
cxl_poll_response_common(cxl_connection_t *conn,
                         int32_t request_id,
                         void *out_data,
                         const void **out_data_view,
                         size_t *out_len,
                         uint32_t *out_response_request_id)
{
    int ret = -1;
    const int use_view = (out_data_view != NULL);

    if (out_data_view)
        *out_data_view = NULL;
    if (out_response_request_id)
        *out_response_request_id = 0;

    if (!conn || !conn->response_data || !conn->flag ||
        request_id <= 0 || !out_len ||
        (!use_view && !out_data)) {
        goto out;
    }

    if (conn->resp_read_offset >= conn->addrs.response_data_size) {
        conn->resp_read_offset = 0;
        ret = -1;
        goto out;
    }

    volatile uint8_t *resp = conn->response_data + conn->resp_read_offset;
    uint32_t ready_req_id = 0;
    int ready_rc = cxl_peek_latest_response_request_id(conn, &ready_req_id);
    if (ready_rc < 0) {
        ret = -1;
        goto out;
    }

    /*
     * Single-flag protocol:
     * - flag carries "latest completed req_id"
     * - consumer drains response_data ring in-order from resp_read_offset
     * Exact-match rule:
     * - Only when flag equals requested request_id do we touch response_data.
     * - Any mismatch stays pending and avoids response_data load.
     */
    if (ready_rc == 0 || ready_req_id == 0) {
        ret = 0;
        goto out;
    }
    if (ready_req_id != (uint32_t)request_id) {
        ret = 0;
        goto out;
    }

    __asm__ __volatile__("clflushopt (%0)" :: "r"((void *)resp) : "memory");
    __asm__ __volatile__("sfence" ::: "memory");
    uint64_t header_lo = *(volatile uint64_t *)resp;
    uint32_t payload_len_u32 = (uint32_t)(header_lo & 0xFFFFFFFFu);
    uint32_t resp_req_id = (uint32_t)(header_lo >> 32);
    if (out_response_request_id)
        *out_response_request_id = resp_req_id;

    /*
     * Multi-client single-flag protocol can observe a non-zero flag before the
     * consumer's current response slot is populated. Treat zero header request
     * id as "not ready" and keep polling without advancing ring offset.
     */
    if (resp_req_id == 0) {
        ret = 0;
        goto out;
    }

    if (resp_req_id != (uint32_t)request_id) {
        cxl_lib_debugf("poll_response header mismatch want=%u flag=%u got=%u offset=%zu",
                       (uint32_t)request_id, ready_req_id, resp_req_id,
                       conn->resp_read_offset);
        ret = 0;
        goto out;
    }

    size_t msg_len = (size_t)payload_len_u32;
    size_t entry_size = cxl_response_entry_size(msg_len);

    if (entry_size > conn->addrs.response_data_size) {
        ret = -1;
        goto out;
    }

    if (conn->resp_read_offset + entry_size > conn->addrs.response_data_size) {
        conn->resp_read_offset = 0;
        ret = 0;
        goto out;
    }

    /*
     * First-line invalidate above guarantees header freshness. For payloads
     * spanning additional cachelines, invalidate the remaining lines before
     * copy/view handoff so readers never observe stale tail bytes.
     */
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
        __asm__ __volatile__("sfence" ::: "memory");
    }

    if (use_view) {
        *out_data_view = (const void *)(resp + 12);
        *out_len = msg_len;
    } else {
        size_t copy_len = msg_len < *out_len ? msg_len : *out_len;
        memcpy(out_data, (void *)(resp + 12), copy_len);
        *out_len = msg_len;
    }

    conn->resp_read_offset += entry_size;
    if (conn->addrs.response_data_size > 0 &&
        conn->resp_read_offset >= conn->addrs.response_data_size) {
        conn->resp_read_offset = 0;
    }

    ret = 1;

out:
    return ret;
}

int cxl_peek_latest_response_request_id(cxl_connection_t *conn,
                                         uint32_t *out_request_id)
{
    if (out_request_id)
        *out_request_id = 0;

    if (!conn || !conn->flag || !out_request_id)
        return -1;

    volatile uint8_t *flag = conn->flag;
    __asm__ __volatile__("clflushopt (%0)" :: "r"((void *)flag) : "memory");
    __asm__ __volatile__("sfence" ::: "memory");
    uint32_t ready_req_id = *(volatile uint32_t *)flag;
    *out_request_id = ready_req_id;
    return (ready_req_id != 0) ? 1 : 0;
}

int cxl_poll_next_response_view(cxl_connection_t *conn,
                                 const void **out_data_view,
                                 size_t *out_len,
                                 uint32_t *out_response_request_id)
{
    if (out_data_view)
        *out_data_view = NULL;
    if (out_len)
        *out_len = 0;
    if (out_response_request_id)
        *out_response_request_id = 0;

    if (!conn || !conn->response_data || !conn->flag ||
        !out_data_view || !out_len || !out_response_request_id)
        return -1;

    if (conn->resp_read_offset >= conn->addrs.response_data_size) {
        conn->resp_read_offset = 0;
        return -1;
    }

    uint32_t latest_ready_req_id = 0;
    int ready_rc = cxl_peek_latest_response_request_id(conn, &latest_ready_req_id);
    if (ready_rc < 0)
        return -1;
    if (ready_rc == 0 || latest_ready_req_id == 0)
        return 0;

    volatile uint8_t *resp = conn->response_data + conn->resp_read_offset;
    __asm__ __volatile__("clflushopt (%0)" :: "r"((void *)resp) : "memory");
    __asm__ __volatile__("sfence" ::: "memory");

    uint64_t header_lo = *(volatile uint64_t *)resp;
    uint32_t payload_len_u32 = (uint32_t)(header_lo & 0xFFFFFFFFu);
    uint32_t resp_req_id = (uint32_t)(header_lo >> 32);

    if (resp_req_id == 0)
        return 0;

    *out_response_request_id = resp_req_id;

    size_t msg_len = (size_t)payload_len_u32;
    size_t entry_size = cxl_response_entry_size(msg_len);
    if (entry_size > conn->addrs.response_data_size)
        return -1;

    if (conn->resp_read_offset + entry_size > conn->addrs.response_data_size) {
        conn->resp_read_offset = 0;
        return 0;
    }

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
        __asm__ __volatile__("sfence" ::: "memory");
    }

    *out_data_view = (const void *)(resp + 12);
    *out_len = msg_len;

    conn->resp_read_offset += entry_size;
    if (conn->addrs.response_data_size > 0 &&
        conn->resp_read_offset >= conn->addrs.response_data_size) {
        conn->resp_read_offset = 0;
    }

    return 1;
}

int cxl_poll_response(cxl_connection_t *conn,
                       int32_t request_id,
                       void *out_data,
                       size_t *out_len)
{
    return cxl_poll_response_common(conn, request_id, out_data, NULL, out_len,
                                    NULL);
}

int cxl_poll_response_view(cxl_connection_t *conn,
                            int32_t request_id,
                            const void **out_data_view,
                            size_t *out_len,
                            uint32_t *out_response_request_id)
{
    return cxl_poll_response_common(conn, request_id, NULL, out_data_view,
                                    out_len, out_response_request_id);
}

/* ================================================================
 * Server API
 * ================================================================ */

int cxl_poll_request(cxl_connection_t *conn,
                      uint16_t *service_id,
                      uint16_t *method_id,
                      uint32_t *request_id,
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
    uint8_t entry_phase = (uint8_t)((meta_lo >> 3) & 0x01u);
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
         * Naive prefetch mode: invalidate current metadata cacheline after
         * an empty poll. Optional "invalidate prefetched" additionally
         * invalidates the previously prefetched lookahead window.
         */
        if (mq_invalidate_prefetched &&
            mq_prefetch_lines > 0 &&
            mq_total_lines > 0) {
            cxl_mq_invalidate_lines(conn->metadata_queue,
                                    head_line_idx,
                                    mq_prefetch_lines + 1,
                                    mq_total_lines);
        } else {
            cxl_invalidate_cacheline(entry_line);
            __asm__ __volatile__("sfence" ::: "memory");
        }
        ret = 0;
        goto out;
    }

    meta_hi = *(volatile uint64_t *)(entry + 8);

    if (mq_prefetch_lines > 0 && mq_total_lines > 0) {
        for (uint32_t i = 1; i <= mq_prefetch_lines; i++) {
            uint32_t next_line = (head_line_idx + i) % mq_total_lines;
            __builtin_prefetch((const void *)(conn->metadata_queue +
                                              ((size_t)next_line * 64u)),
                               0, 3);
        }
    }

    /* Parse entry fields */
    uint8_t mq_method  = (uint8_t)(meta_lo & 0x03u);
    uint8_t mq_inline  = (uint8_t)((meta_lo >> 2) & 0x01u);
    uint32_t mq_len = (uint32_t)((meta_lo >> 4) & CXL_REQ_META_LEN_MAX);
    uint16_t svc = (uint16_t)((meta_lo >> 24) & 0x0FFFu);
    uint16_t mid = (uint16_t)((meta_lo >> 36) & 0x0FFFu);
    uint32_t rid = (uint32_t)((meta_lo >> 48) & 0xFFFFu);

    if (service_id) *service_id = svc;
    if (method_id)  *method_id  = mid;
    if (request_id) *request_id = rid;

    /* Extract payload */
    int is_request = (mq_method == CXL_METHOD_REQUEST);
    int is_inline_req = (mq_inline != 0);
    if (is_request) {
        if (is_inline_req) {
            /* Inline: payload view is in bytes 8-15. */
            size_t msg_len = mq_len <= 8 ? (size_t)mq_len : 8;
            *out_data_view = (const void *)(entry + 8);
            *out_len = msg_len;
        } else {
            /* Non-inline: bytes 8-15 contain request_data physical address */
            uint64_t data_addr = meta_hi;

            uint64_t local_base = conn->addrs.request_data_addr;
            uint64_t local_size = (uint64_t)conn->addrs.request_data_size;
            uint64_t peer_base = conn->peer_request_data_addr;
            uint64_t peer_size = (uint64_t)conn->peer_request_data_size;

            /* Strict white-list:
             *  1) connection-local request_data region
             *  2) explicitly configured peer request_data region
             */
            uint64_t region_base = 0;
            uint64_t region_size = 0;
            if (local_size > 0 &&
                data_addr >= local_base &&
                (data_addr - local_base) < local_size) {
                region_base = local_base;
                region_size = local_size;
            } else if (peer_size > 0 &&
                       data_addr >= peer_base &&
                       (data_addr - peer_base) < peer_size) {
                region_base = peer_base;
                region_size = peer_size;
            } else {
                cxl_advance_mq_head_with_policy(conn, head_idx, head_line_idx,
                                                mq_entries_per_line,
                                                mq_total_lines,
                                                mq_invalidate_consumed);
                ret = -1;
                goto out;
            }

            uint64_t entry_offset = data_addr - region_base;
            if (entry_offset > region_size) {
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
                size_t msg_len = (size_t)mq_len;
                const volatile uint8_t *payload_ptr = data_ptr;

                if (msg_len == 0 ||
                    (uint64_t)msg_len > (region_size - entry_offset)) {
                    ret = -1;
                    goto out;
                }

                if (msg_len > sizeof(conn->request_local_buf)) {
                    ret = -1;
                    goto out;
                }

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
                __asm__ __volatile__("sfence" ::: "memory");
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
    }

    /* Advance head */
    cxl_advance_mq_head_with_policy(conn, head_idx, head_line_idx,
                                    mq_entries_per_line,
                                    mq_total_lines,
                                    mq_invalidate_consumed);

    ret = 1;
    cxl_lib_debugf("poll_request hit req_id=%u svc=%u method=%u len=%zu mq_head=%u meta_lo=%#llx meta_hi=%#llx next_head=%u",
                   rid, svc, mid, *out_len, head_idx,
                   (unsigned long long)meta_lo,
                   (unsigned long long)meta_hi,
                   conn->mq_head);

out:
    return ret;
}

/* ================================================================
 * Server Response API
 * ================================================================ */

int cxl_connection_set_peer_addrs(cxl_connection_t *conn,
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

    /* Map virtual pointer */
    volatile void *response_ptr = cxl_rpc_phys_to_virt(conn->ctx,
                                                        peer_response_data_addr);

    if (!response_ptr)
        return -1;

    /* All checks passed, commit the configuration */
    conn->peer_response_data_addr = peer_response_data_addr;
    conn->peer_response_data_size = peer_response_data_size;
    conn->peer_response_data = (volatile uint8_t *)response_ptr;
    conn->peer_flag_addr = 0;
    conn->peer_flag = NULL;
    conn->peer_resp_write_offset = 0;
    cxl_prefault_each_page_ro(conn->peer_response_data, peer_response_data_size);
    cxl_flush_range(conn->peer_response_data, peer_response_data_size);
    if (cxl_copyengine_update_peer_response_mapping(conn) != 0)
        return -1;
    if (cxl_copyengine_update_peer_flag_mapping(conn) != 0)
        return -1;

    cxl_lib_debugf("set_peer_response_data addr=%#llx size=%zu ptr=%p",
                   (unsigned long long)peer_response_data_addr,
                   peer_response_data_size,
                   (void *)conn->peer_response_data);

    return 0;
}

int cxl_connection_set_peer_flag_addr(cxl_connection_t *conn,
                                       uint64_t peer_flag_addr)
{
    if (!conn || !conn->ctx)
        return -1;

    if (peer_flag_addr == 0)
        return -1;

    if (!cxl_rpc_phys_range_valid(conn->ctx, peer_flag_addr, CXL_FLAG_PUBLISH_LEN))
        return -1;

    volatile void *flag_ptr = cxl_rpc_phys_to_virt(conn->ctx, peer_flag_addr);
    if (!flag_ptr)
        return -1;

    conn->peer_flag_addr = peer_flag_addr;
    conn->peer_flag = (volatile uint8_t *)flag_ptr;
    cxl_prefault_first_byte_ro(conn->peer_flag, CXL_FLAG_PUBLISH_LEN);
    cxl_flush_range(conn->peer_flag, CXL_FLAG_PUBLISH_LEN);
    if (cxl_copyengine_update_peer_flag_mapping(conn) != 0)
        return -1;

    cxl_lib_debugf("set_peer_flag addr=%#llx ptr=%p",
                   (unsigned long long)peer_flag_addr,
                   (void *)conn->peer_flag);

    /*
     * Response publication is CopyEngine-only. Prepare it during setup so a
     * missing or broken device is reported before the first response send.
     */
    if (cxl_copyengine_prepare(conn) != 0)
        return -1;

    cxl_lib_debugf("set_peer_flag copyengine ready=%d bar0=%p slots=%zu next_slot=%zu",
                   conn->ce_ready,
                   (void *)conn->ce_bar0,
                   conn->ce_slots,
                   conn->ce_next_slot);

    return 0;
}

int cxl_connection_set_peer_request_data(cxl_connection_t *conn,
                                          uint64_t peer_request_data_addr,
                                          size_t peer_request_data_size)
{
    if (!conn || !conn->ctx)
        return -1;

    if (peer_request_data_addr == 0 || peer_request_data_size == 0)
        return -1;

    if (!cxl_rpc_phys_range_valid(conn->ctx,
                                  peer_request_data_addr,
                                  peer_request_data_size)) {
        return -1;
    }

    volatile uint8_t *request_ptr =
        (volatile uint8_t *)cxl_rpc_phys_to_virt(conn->ctx,
                                                 peer_request_data_addr);
    if (!request_ptr)
        return -1;

    conn->peer_request_data_addr = peer_request_data_addr;
    conn->peer_request_data_size = peer_request_data_size;
    cxl_prefault_first_byte_ro(request_ptr, peer_request_data_size);
    cxl_lib_debugf("set_peer_request_data addr=%#llx size=%zu ptr=%p",
                   (unsigned long long)peer_request_data_addr,
                   peer_request_data_size,
                   (void *)request_ptr);
    return 0;
}

int cxl_send_response(cxl_connection_t *conn,
                      uint32_t request_id,
                      const void *data,
                      size_t len)
{
    /* Comprehensive parameter validation */
    if (!conn || !conn->peer_response_data || !conn->peer_flag)
        return -1;
    if (len > 0 && !data)
        return -1;

    size_t entry_size = cxl_response_entry_size(len);
    if (len > (size_t)UINT32_MAX)
        return -1;
    if (entry_size > conn->peer_response_data_size)
        return -1;

    /* Pick destination from peer response ring tail. */
    size_t offset = conn->peer_resp_write_offset;
    if (offset >= conn->peer_response_data_size)
        offset = 0;
    if (offset + entry_size > conn->peer_response_data_size)
        offset = 0;

    cxl_lib_debugf("send_response begin req_id=%u len=%zu entry_size=%zu offset=%zu dst_resp=%#llx dst_flag=%#llx",
                   request_id, len, entry_size, offset,
                   (unsigned long long)(conn->peer_response_data_addr + offset),
                   (unsigned long long)conn->peer_flag_addr);

    /*
     * Preferred path: asynchronous CopyEngine offload.
     * Submit resp_data then flag in-order without waiting for completion.
     * Destination readiness is prepared once during peer setup (prefault +
     * flush), so submit only builds descriptors and rings the engine.
     */
    const volatile uint8_t *dst_resp_ptr = conn->peer_response_data + offset;
    if (!cxl_copyengine_submit_response_async(conn, request_id, data, len,
                                              dst_resp_ptr, entry_size)) {
        fprintf(stderr,
                "cxl_rpc: ERROR: CopyEngine submit path failed (req_id=%u)\n",
                request_id);
        cxl_lib_debugf("send_response fail req_id=%u ce_ready=%d ce_slots=%zu ce_next_slot=%zu ce_chain_started=%d",
                       request_id, conn->ce_ready, conn->ce_slots,
                       conn->ce_next_slot, conn->ce_chain_started);
        return -1;
    }

    conn->peer_resp_write_offset = offset + entry_size;
    if (conn->peer_resp_write_offset >= conn->peer_response_data_size)
        conn->peer_resp_write_offset = 0;

    cxl_lib_debugf("send_response submitted req_id=%u next_offset=%zu",
                   request_id, conn->peer_resp_write_offset);

    return 0;
}
