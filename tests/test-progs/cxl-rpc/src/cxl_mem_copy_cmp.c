/*
 * CXL copy publish microbenchmark:
 * Compare memcpy publish (copy + clflushopt + sfence) against
 * async CopyEngine submit cost (no completion wait).
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "cxl_rpc.h"
#include "cxl_timing.h"

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

#define CXL_BASE_ADDR      0x100000000ULL
#define CXL_MAP_SIZE       0x10000000ULL   /* 256MB */

#define MEMCPY_DST_OFFSET  0x02000000ULL   /* 32MB */
#define CE_DST_OFFSET      0x0A000000ULL   /* 160MB */
#define DST_REGION_BYTES   (64ULL * 1024ULL * 1024ULL)

#define CACHELINE_SIZE     64ULL

/* CopyEngine channel-0 registers */
#define CE_CHAN_BASE       0x80u
#define CE_CHAN_STATUS     0x04u
#define CE_CHAINADDR       0x0Cu
#define CE_COMMAND         0x14u
#define CE_CMD_START_DMA   0x01u
#define CE_CMD_APPEND_DMA  0x02u
#define CE_DEFAULT_CHUNK_BYTES 4096u
/* Smallest BAR window covering all MMIO offsets used by this benchmark. */
#define CE_MMIO_REQUIRED_BYTES \
    (CE_CHAN_BASE + CE_COMMAND + sizeof(uint8_t))

#define CE_PCI_VENDOR_ID   0x8086u
#define CE_PCI_DEVICE_ID   0x1A38u
#define PCI_CFG_ADDR_PORT  0xCF8u
#define PCI_CFG_DATA_PORT  0xCFCu

typedef struct ce_desc {
    uint32_t len;
    uint32_t command;
    uint64_t src;
    uint64_t dest;
    uint64_t next;
    uint64_t reserved1;
    uint64_t reserved2;
    uint64_t user1;
    uint64_t user2;
} ce_desc_t;

_Static_assert(sizeof(ce_desc_t) == 64u, "CopyEngine descriptor size mismatch");

typedef struct bench_case {
    size_t bytes;
    int iters;
} bench_case_t;

static const bench_case_t g_cases[] = {
    { 64u, 1 },
    { 256u, 1 },
    { 1024u, 1 },
    { 4096u, 1 },
    { 16384u, 1 },
    { 65536u, 1 },
    { 262144u, 1 },
    { 1048576u, 1 },
};

typedef struct ce_state {
    int fd;
    volatile uint8_t *bar0;
    void *bar_map_base;
    size_t bar_map_len;
    ce_desc_t *desc_pool;
    uint64_t *desc_phys;
    size_t desc_count;
    size_t next_desc;
    ce_desc_t *last_desc;
    int chain_started;
    uint8_t *src_buf;
    uint64_t src_phys;
} ce_state_t;

static inline uint64_t
global_ns(void)
{
    return m5_rpns();
}

static inline uint64_t
roundup64(uint64_t x)
{
    return (x + 63ULL) & ~63ULL;
}

static inline void
store_sfence(void)
{
    __asm__ __volatile__("sfence" ::: "memory");
}

static inline void
flushopt_line(const volatile void *addr)
{
    /*
     * Use CLFLUSH (not CLFLUSHOPT) for benchmark stability in timing-mode
     * full-system runs where aggressive overlapping clean requests can trip
     * crossbar request-tracking assertions.
     */
    __asm__ __volatile__("clflush (%0)" :: "r"(addr) : "memory");
}

static inline void
flush_range(const volatile void *addr, size_t len)
{
    uintptr_t start = ((uintptr_t)addr) & ~((uintptr_t)63);
    uintptr_t end = (((uintptr_t)addr + len) + 63u) & ~((uintptr_t)63);
    for (uintptr_t p = start; p < end; p += 64u)
        flushopt_line((const volatile void *)p);
}

static inline void
ce_mmio_write8(volatile uint8_t *bar0, uint32_t off, uint8_t val)
{
    *(volatile uint8_t *)(bar0 + off) = val;
}

static inline void
ce_mmio_write64(volatile uint8_t *bar0, uint32_t off, uint64_t val)
{
    *(volatile uint64_t *)(bar0 + off) = val;
}

static inline uint64_t
ce_mmio_read64(volatile uint8_t *bar0, uint32_t off)
{
    return *(volatile uint64_t *)(bar0 + off);
}

static int
ce_wait_idle(ce_state_t *ce)
{
    const uint64_t max_spins = 100000000ULL;
    for (uint64_t i = 0; i < max_spins; i++) {
        const uint64_t st = ce_mmio_read64(ce->bar0, CE_CHAN_BASE + CE_CHAN_STATUS);
        if (st & 0x1ULL)
            return 0;
        __asm__ __volatile__("pause" ::: "memory");
    }
    return -1;
}

static int
parse_int_arg(int argc, char **argv, const char *flag, int default_v,
              int min_v, int max_v)
{
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0) {
            char *end = NULL;
            long v = strtol(argv[i + 1], &end, 0);
            if (end && *end == '\0' && v >= min_v && v <= max_v)
                return (int)v;
            return default_v;
        }
    }
    return default_v;
}

static const char *
parse_str_arg(int argc, char **argv, const char *flag, const char *default_v)
{
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0)
            return argv[i + 1];
    }
    return default_v;
}

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    return 0;
}

static void
compute_stats(uint64_t *samples, int n,
              uint64_t *min, uint64_t *avg, uint64_t *p50,
              uint64_t *p99, uint64_t *max)
{
    uint64_t sum = 0;
    qsort(samples, (size_t)n, sizeof(uint64_t), cmp_u64);
    for (int i = 0; i < n; i++)
        sum += samples[i];
    *min = samples[0];
    *avg = sum / (uint64_t)n;
    *p50 = samples[(size_t)n * 50 / 100];
    *p99 = samples[(size_t)n * 99 / 100];
    *max = samples[n - 1];
}

static int
virt_to_phys(const void *vaddr, uint64_t *phys_out)
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

    *phys_out = (entry & ((1ULL << 55) - 1)) * page_size + (virt % page_size);
    return 0;
}

static int
pci_cfg_io_enable(void)
{
    static int enabled = 0;
    if (enabled)
        return 0;
    if (ioperm(PCI_CFG_ADDR_PORT, 8, 1) != 0)
        return -1;
    enabled = 1;
    return 0;
}

static uint32_t
pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint32_t cfg_addr = 0x80000000u |
                        ((uint32_t)bus << 16) |
                        ((uint32_t)dev << 11) |
                        ((uint32_t)func << 8) |
                        (off & 0xFCu);
    outl(cfg_addr, PCI_CFG_ADDR_PORT);
    return inl(PCI_CFG_DATA_PORT);
}

static void
pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val)
{
    uint32_t cfg_addr = 0x80000000u |
                        ((uint32_t)bus << 16) |
                        ((uint32_t)dev << 11) |
                        ((uint32_t)func << 8) |
                        (off & 0xFCu);
    outl(cfg_addr, PCI_CFG_ADDR_PORT);
    outl(val, PCI_CFG_DATA_PORT);
}

static int
ce_find_and_enable_bar0_sysfs(uint64_t *bar_phys_out)
{
    const char *cfg_path = "/sys/bus/pci/devices/0000:00:07.0/config";
    uint32_t id = 0;
    uint16_t cmd = 0;
    uint32_t bar0 = 0;
    int fd = -1;

    if (!bar_phys_out)
        return -1;

    fd = open(cfg_path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;

    if (pread(fd, &id, sizeof(id), 0x00) != (ssize_t)sizeof(id))
        goto fail;
    if ((uint16_t)(id & 0xFFFFu) != CE_PCI_VENDOR_ID ||
        (uint16_t)((id >> 16) & 0xFFFFu) != CE_PCI_DEVICE_ID)
        goto fail;

    if (pread(fd, &cmd, sizeof(cmd), 0x04) != (ssize_t)sizeof(cmd))
        goto fail;
    if ((cmd & 0x2u) == 0) {
        cmd |= 0x2u;
        if (pwrite(fd, &cmd, sizeof(cmd), 0x04) != (ssize_t)sizeof(cmd))
            goto fail;
    }

    if (pread(fd, &bar0, sizeof(bar0), 0x10) != (ssize_t)sizeof(bar0))
        goto fail;
    if (bar0 == 0 || bar0 == 0xFFFFFFFFu)
        goto fail;

    *bar_phys_out = (uint64_t)(bar0 & ~0xFu);
    close(fd);
    return 0;

fail:
    close(fd);
    return -1;
}

static int
ce_find_and_enable_bar0(uint64_t *bar_phys_out)
{
    uint8_t found_bus = 0xFF;
    uint8_t found_dev = 0xFF;
    uint8_t found_func = 0xFF;

    if (!bar_phys_out)
        return -1;

    if (ce_find_and_enable_bar0_sysfs(bar_phys_out) == 0)
        return 0;

    if (pci_cfg_io_enable() != 0)
        return -1;

    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id = pci_cfg_read32(0, dev, 0, 0x00);
        uint16_t ven = (uint16_t)(id & 0xFFFFu);
        uint16_t did = (uint16_t)((id >> 16) & 0xFFFFu);
        if (ven == CE_PCI_VENDOR_ID && did == CE_PCI_DEVICE_ID) {
            found_bus = 0;
            found_dev = dev;
            found_func = 0;
            break;
        }
    }

    if (found_dev == 0xFF)
        return -1;

    uint32_t cmd = pci_cfg_read32(found_bus, found_dev, found_func, 0x04);
    if ((cmd & 0x2u) == 0) {
        cmd |= 0x2u; /* Memory space enable */
        pci_cfg_write32(found_bus, found_dev, found_func, 0x04, cmd);
    }

    uint32_t bar0 = pci_cfg_read32(found_bus, found_dev, found_func, 0x10);
    if (bar0 == 0 || bar0 == 0xFFFFFFFFu)
        return -1;

    *bar_phys_out = (uint64_t)(bar0 & ~0xFu);
    return 0;
}

static void
ce_destroy(ce_state_t *ce)
{
    if (!ce)
        return;
    if (ce->bar_map_base && ce->bar_map_len > 0) {
        munmap(ce->bar_map_base, ce->bar_map_len);
        ce->bar_map_base = NULL;
        ce->bar_map_len = 0;
        ce->bar0 = NULL;
    }
    if (ce->fd >= 0) {
        close(ce->fd);
        ce->fd = -1;
    }
    free(ce->desc_pool);
    free(ce->desc_phys);
    free(ce->src_buf);
    ce->desc_pool = NULL;
    ce->desc_phys = NULL;
    ce->src_buf = NULL;
}

static int
ce_init(ce_state_t *ce, size_t desc_count, size_t max_copy_size,
        const char *resource)
{
    uint64_t tmp_bar = 0;
    size_t map_len = 0;
    struct stat st;

    memset(ce, 0, sizeof(*ce));
    ce->fd = -1;
    ce->desc_count = desc_count;

    if (posix_memalign((void **)&ce->desc_pool, 64, desc_count * sizeof(ce_desc_t)) != 0)
        return -1;
    if (posix_memalign((void **)&ce->src_buf, 64, roundup64(max_copy_size)) != 0)
        return -1;
    ce->desc_phys = (uint64_t *)calloc(desc_count, sizeof(uint64_t));
    if (!ce->desc_phys)
        return -1;

    memset(ce->desc_pool, 0, desc_count * sizeof(ce_desc_t));
    memset(ce->src_buf, 0xA5, roundup64(max_copy_size));

    /* Best effort only */
    (void)mlock(ce->desc_pool, desc_count * sizeof(ce_desc_t));
    (void)mlock(ce->src_buf, roundup64(max_copy_size));

    if (virt_to_phys(ce->src_buf, &ce->src_phys) != 0)
        return -1;
    for (size_t i = 0; i < desc_count; i++) {
        if (virt_to_phys(ce->desc_pool + i, &ce->desc_phys[i]) != 0)
            return -1;
    }

    /*
     * Always try to enable PCI memory decode first, even when mapping
     * resource0 directly. Otherwise the BAR range may never be advertised
     * to the IO bus in timing mode.
     */
    if (ce_find_and_enable_bar0(&tmp_bar) != 0) {
        return -1;
    }

    long page_size_l = sysconf(_SC_PAGESIZE);
    if (page_size_l <= 0)
        return -1;
    size_t page_size = (size_t)page_size_l;
    size_t bar_page_off = (size_t)(tmp_bar & (uint64_t)(page_size - 1u));

    /*
     * resource0 mmap base can be page-aligned below BAR0 when BAR0 is not
     * page-aligned (e.g., BAR0=...1800). Adjust bar0 pointer by page offset.
     */
    map_len = bar_page_off + CE_MMIO_REQUIRED_BYTES;
    map_len = ((map_len + page_size - 1u) / page_size) * page_size;

    ce->fd = open(resource, O_RDWR | O_SYNC | O_CLOEXEC);
    if (ce->fd < 0)
        return -1;

    if (fstat(ce->fd, &st) == 0 && st.st_size > 0 &&
        (uint64_t)st.st_size < (uint64_t)CE_MMIO_REQUIRED_BYTES) {
        close(ce->fd);
        ce->fd = -1;
        return -1;
    }

    void *map_base = mmap(NULL, map_len,
                          PROT_READ | PROT_WRITE, MAP_SHARED,
                          ce->fd, 0);
    if (map_base == MAP_FAILED) {
        ce->bar0 = NULL;
        close(ce->fd);
        ce->fd = -1;
        return -1;
    }

    ce->bar_map_base = map_base;
    ce->bar_map_len = map_len;
    ce->bar0 = (volatile uint8_t *)map_base + bar_page_off;
    return 0;
}

static int
ce_submit_one_async(ce_state_t *ce, uint64_t src_phys, uint64_t dst_phys,
                    size_t bytes)
{
    if (!ce || !ce->bar0 || ce->next_desc >= ce->desc_count)
        return -1;

    ce_desc_t *d = ce->desc_pool + ce->next_desc;
    uint64_t d_phys = ce->desc_phys[ce->next_desc];
    ce->next_desc++;

    memset(d, 0, sizeof(*d));
    d->len = (uint32_t)bytes;
    d->command = 0;
    d->src = src_phys;
    d->dest = dst_phys;
    d->next = 0;

    __sync_synchronize();

    if (!ce->chain_started) {
        ce_mmio_write64(ce->bar0, CE_CHAN_BASE + CE_CHAINADDR, d_phys);
        __sync_synchronize();
        ce_mmio_write8(ce->bar0, CE_CHAN_BASE + CE_COMMAND, CE_CMD_START_DMA);
        ce->chain_started = 1;
    } else {
        ce->last_desc->next = d_phys;
        __sync_synchronize();
        ce_mmio_write8(ce->bar0, CE_CHAN_BASE + CE_COMMAND, CE_CMD_APPEND_DMA);
    }

    ce->last_desc = d;
    return 0;
}

static int
ce_submit_copy_windowed(ce_state_t *ce, uint64_t dst_phys, size_t bytes,
                        size_t chunk_bytes, int inflight_limit,
                        int *inflight_out)
{
    size_t off = 0;
    int inflight = inflight_out ? *inflight_out : 0;

    if (chunk_bytes == 0 || inflight_limit <= 0)
        return -1;

    while (off < bytes) {
        size_t chunk = bytes - off;
        if (chunk > chunk_bytes)
            chunk = chunk_bytes;

        if (inflight >= inflight_limit) {
            if (ce_wait_idle(ce) != 0)
                return -1;
            inflight = 0;
        }

        if (ce_submit_one_async(ce,
                                ce->src_phys + (uint64_t)off,
                                dst_phys + (uint64_t)off,
                                chunk) != 0) {
            return -1;
        }
        inflight++;
        off += chunk;
    }

    if (inflight_out)
        *inflight_out = inflight;
    return 0;
}

int
main(int argc, char **argv)
{
    int cpu = parse_int_arg(argc, argv, "--cpu", 1, 0, 4095);
    int ce_inflight_limit = parse_int_arg(argc, argv, "--ce-inflight", 4, 1, 4096);
    int iters_override = parse_int_arg(argc, argv, "--iters", 0, 0, 1000000);
    int ce_chunk_bytes = parse_int_arg(
        argc, argv, "--ce-chunk-bytes", (int)CE_DEFAULT_CHUNK_BYTES, 64, 1048576);
    const char *mode = parse_str_arg(argc, argv, "--mode", "both");
    const int run_memcpy = (strcmp(mode, "copyengine") != 0);
    const int run_copyengine = (strcmp(mode, "memcpy") != 0);
    const char *resource = parse_str_arg(
        argc, argv, "--resource",
        "/sys/bus/pci/devices/0000:00:07.0/resource0");

    size_t max_size = g_cases[0].bytes;
    size_t total_desc = 0;
    for (size_t i = 0; i < sizeof(g_cases) / sizeof(g_cases[0]); i++) {
        size_t case_iters = (iters_override > 0) ?
            (size_t)iters_override : (size_t)g_cases[i].iters;
        if (g_cases[i].bytes > max_size)
            max_size = g_cases[i].bytes;
        if (run_copyengine) {
            size_t per_req = (g_cases[i].bytes + (size_t)ce_chunk_bytes - 1u) /
                             (size_t)ce_chunk_bytes;
            total_desc += per_req * case_iters;
        } else {
            total_desc += case_iters;
        }
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
        fprintf(stderr, "warn: sched_setaffinity cpu=%d failed: %s\n",
                cpu, strerror(errno));
    }

    cxl_context_t *ctx = cxl_rpc_init(CXL_BASE_ADDR, CXL_MAP_SIZE);
    if (!ctx) {
        fprintf(stderr, "ERROR: cxl_rpc_init failed\n");
        return 1;
    }

    volatile uint8_t *cxl_base = (volatile uint8_t *)cxl_rpc_get_base(ctx);
    if (!cxl_base) {
        fprintf(stderr, "ERROR: cxl_rpc_get_base failed\n");
        cxl_rpc_destroy(ctx);
        return 1;
    }

    volatile uint8_t *dst_memcpy_base = cxl_base + MEMCPY_DST_OFFSET;

    ce_state_t ce;
    if (run_copyengine) {
        if (ce_init(&ce, total_desc + 8u, max_size, resource) != 0) {
            fprintf(stderr, "ERROR: CopyEngine init failed (resource=%s)\n", resource);
            cxl_rpc_destroy(ctx);
            return 1;
        }
    }

    uint8_t *src_memcpy = NULL;
    if (posix_memalign((void **)&src_memcpy, 64, roundup64(max_size)) != 0 ||
        !src_memcpy) {
        fprintf(stderr, "ERROR: src buffer alloc failed\n");
        ce_destroy(&ce);
        cxl_rpc_destroy(ctx);
        return 1;
    }
    memset(src_memcpy, 0x3C, roundup64(max_size));

    /*
     * Touch and flush source once so first measured sample is not dominated
     * by one-time page faults.
     */
    flush_range(src_memcpy, max_size);
    store_sfence();

    printf("mode_semantics,memcpy=copy+flush+sfence,copyengine_complete=submit_plus_completion\n");
    printf("size_bytes,mode,iters,min_ns,avg_ns,p50_ns,p99_ns,max_ns\n");

    uint64_t memcpy_cursor = 0;
    uint64_t ce_cursor = 0;

    for (size_t c = 0; c < sizeof(g_cases) / sizeof(g_cases[0]); c++) {
        const size_t sz = g_cases[c].bytes;
        const int iters = (iters_override > 0) ? iters_override : g_cases[c].iters;
        const uint64_t stride = roundup64(sz);

        uint64_t *memcpy_ns = run_memcpy ?
            (uint64_t *)malloc((size_t)iters * sizeof(uint64_t)) : NULL;
        uint64_t *ce_submit_ns = run_copyengine ?
            (uint64_t *)malloc((size_t)iters * sizeof(uint64_t)) : NULL;
        if ((run_memcpy && !memcpy_ns) || (run_copyengine && !ce_submit_ns)) {
            fprintf(stderr, "ERROR: sample alloc failed\n");
            free(memcpy_ns);
            free(ce_submit_ns);
            free(src_memcpy);
            if (run_copyengine)
                ce_destroy(&ce);
            cxl_rpc_destroy(ctx);
            return 1;
        }

        if (run_memcpy) {
            for (int i = 0; i < iters; i++) {
                if (memcpy_cursor + stride > DST_REGION_BYTES)
                    memcpy_cursor = 0;
                volatile uint8_t *dst = dst_memcpy_base + memcpy_cursor;
                memcpy_cursor += stride;

                uint64_t t0 = global_ns();
                memcpy((void *)dst, src_memcpy, sz);
                flush_range(dst, sz);
                store_sfence();
                uint64_t t1 = global_ns();
                memcpy_ns[i] = t1 - t0;
            }
        }

        if (run_copyengine) {
            for (int i = 0; i < iters; i++) {
                int ce_inflight = 0;
                if (ce_cursor + stride > DST_REGION_BYTES)
                    ce_cursor = 0;
                uint64_t dst_phys = CXL_BASE_ADDR + CE_DST_OFFSET + ce_cursor;
                ce_cursor += stride;

                uint64_t t0 = global_ns();
                if (ce_submit_copy_windowed(&ce, dst_phys, sz,
                                            (size_t)ce_chunk_bytes,
                                            ce_inflight_limit,
                                            &ce_inflight) != 0) {
                    fprintf(stderr, "ERROR: copyengine submit failed at size=%zu iter=%d\n",
                            sz, i);
                    free(memcpy_ns);
                    free(ce_submit_ns);
                    free(src_memcpy);
                    if (run_copyengine)
                        ce_destroy(&ce);
                    cxl_rpc_destroy(ctx);
                    return 1;
                }
                if (ce_inflight > 0) {
                    if (ce_wait_idle(&ce) != 0) {
                        fprintf(stderr, "ERROR: copyengine wait idle timeout\n");
                        free(memcpy_ns);
                        free(ce_submit_ns);
                        free(src_memcpy);
                        if (run_copyengine)
                            ce_destroy(&ce);
                        cxl_rpc_destroy(ctx);
                        return 1;
                    }
                }
                uint64_t t1 = global_ns();
                ce_submit_ns[i] = t1 - t0;
            }
        }

        uint64_t min_v = 0, avg_v = 0, p50_v = 0, p99_v = 0, max_v = 0;
        if (run_memcpy) {
            compute_stats(memcpy_ns, iters, &min_v, &avg_v, &p50_v, &p99_v, &max_v);
            printf("%zu,memcpy,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                   sz, iters, min_v, avg_v, p50_v, p99_v, max_v);
        }

        if (run_copyengine) {
            compute_stats(ce_submit_ns, iters, &min_v, &avg_v, &p50_v, &p99_v, &max_v);
            printf("%zu,copyengine_complete,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                   sz, iters, min_v, avg_v, p50_v, p99_v, max_v);
        }

        free(memcpy_ns);
        free(ce_submit_ns);
    }

    free(src_memcpy);
    if (run_copyengine)
        ce_destroy(&ce);
    cxl_rpc_destroy(ctx);
    return 0;
}
