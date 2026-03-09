#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cxl_rpc.h"
#include "cxl_timing.h"

#define CXL_BASE_ADDR_DEFAULT      0x100000000ULL
#define CXL_MAP_SIZE_DEFAULT       0x40000000ULL
#define CXL_DST_OFFSET_DEFAULT     0x02000000ULL
#define CXL_DST_STRIDE_AUTO        0ULL
#define COPYENGINE_DEFAULT_TIMEOUT_NS 10000000000ULL

#define CXL_CE_GEN_CHANCOUNT       0x00u
#define CXL_CE_GEN_XFERCAP         0x01u
#define CXL_CE_CHAN_BASE           0x80u
#define CXL_CE_CHAN_STRIDE         0x80u
#define CXL_CE_CHAN_STATUS         0x04u
#define CXL_CE_CHAN_CHAINADDR      0x0Cu
#define CXL_CE_CHAN_COMMAND        0x14u
#define CXL_CE_CHAN_ERROR          0x28u
#define CXL_CE_CMD_START_DMA       0x01u
#define CXL_CE_PCI_CMD_OFF         0x04u
#define CXL_CE_PCI_CMD_MEM         0x0002u
#define CXL_CE_PCI_CMD_BM          0x0004u
#define CXL_CE_PCI_VENDOR_ID       0x8086u
#define CXL_CE_PCI_DEVICE_ID       0x1A38u

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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

typedef struct {
    int fd;
    volatile uint8_t *bar0;
    void *bar_map_base;
    size_t bar_map_len;
    size_t page_size;
    uint32_t channel;
    uint32_t chan_count;
    uint32_t chan_mmio_base;
    size_t max_xfer_bytes;
    char resource0_path[PATH_MAX];
    ce_desc_t *desc_pool;
    uint64_t *desc_phys;
    size_t desc_count;
    uint64_t first_desc_phys;
    uint8_t *src_buf;
    size_t src_buf_bytes;
    size_t lane_index;
    size_t engine_index;
} ce_state_t;

typedef struct {
    uint64_t min_ns;
    uint64_t avg_ns;
    uint64_t p50_ns;
    uint64_t p90_ns;
    uint64_t p95_ns;
    uint64_t p99_ns;
    uint64_t max_ns;
} bench_stats_t;

static inline uint8_t
ce_mmio_read8(volatile uint8_t *bar0, uint32_t off)
{
    return *(volatile uint8_t *)(bar0 + off);
}

static inline uint32_t
ce_mmio_read32(volatile uint8_t *bar0, uint32_t off)
{
    return *(volatile uint32_t *)(bar0 + off);
}

static inline uint64_t
ce_mmio_read64(volatile uint8_t *bar0, uint32_t off)
{
    return *(volatile uint64_t *)(bar0 + off);
}

static inline void
ce_mmio_write8(volatile uint8_t *bar0, uint32_t off, uint8_t val)
{
    *(volatile uint8_t *)(bar0 + off) = val;
}

static inline void
ce_mmio_write32(volatile uint8_t *bar0, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(bar0 + off) = val;
}

static inline void
ce_mmio_write64(volatile uint8_t *bar0, uint32_t off, uint64_t val)
{
    *(volatile uint64_t *)(bar0 + off) = val;
}

static inline void
store_sfence(void)
{
    __asm__ __volatile__("sfence" ::: "memory");
}

static inline void
pause_loop(void)
{
    __asm__ __volatile__("pause" ::: "memory");
}

static inline void
flush_line(const volatile void *addr)
{
    __asm__ __volatile__("clflush (%0)" :: "r"(addr) : "memory");
}

static inline uint64_t
align_up_u64(uint64_t value, uint64_t align)
{
    if (align == 0)
        return value;
    return ((value + align - 1u) / align) * align;
}

static void
flush_range(const volatile void *addr, size_t len)
{
    uintptr_t start = ((uintptr_t)addr) & ~((uintptr_t)63);
    uintptr_t end = (((uintptr_t)addr + len) + 63u) & ~((uintptr_t)63);
    for (uintptr_t p = start; p < end; p += 64u)
        flush_line((const volatile void *)p);
}

static int
cmp_str_ptrs(const void *a, const void *b)
{
    const char *const *lhs = (const char *const *)a;
    const char *const *rhs = (const char *const *)b;
    return strcmp(*lhs, *rhs);
}

static int
parse_int_arg(int argc, char **argv, const char *flag,
              int default_v, int min_v, int max_v)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], flag) == 0) {
            char *end = NULL;
            long value = strtol(argv[i + 1], &end, 0);
            if (!end || *end != '\0' || value < min_v || value > max_v)
                return default_v;
            return (int)value;
        }
    }
    return default_v;
}

static uint64_t
parse_u64_arg(int argc, char **argv, const char *flag, uint64_t default_v)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], flag) == 0) {
            char *end = NULL;
            unsigned long long value = strtoull(argv[i + 1], &end, 0);
            if (!end || *end != '\0')
                return default_v;
            return (uint64_t)value;
        }
    }
    return default_v;
}

static const char *
parse_str_arg(int argc, char **argv, const char *flag, const char *default_v)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], flag) == 0)
            return argv[i + 1];
    }
    return default_v;
}

static int
read_hex_u32_file(const char *path, uint32_t *val_out)
{
    if (!path || !val_out) {
        errno = EINVAL;
        return -1;
    }

    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;

    char buf[64];
    if (!fgets(buf, sizeof(buf), fp)) {
        int saved = errno;
        fclose(fp);
        errno = saved ? saved : EIO;
        return -1;
    }
    fclose(fp);

    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(buf, &end, 0);
    if (errno != 0 || !end || end == buf) {
        errno = EINVAL;
        return -1;
    }

    *val_out = (uint32_t)value;
    return 0;
}

static void
free_copyengine_paths(char **paths, size_t count)
{
    if (!paths)
        return;
    for (size_t i = 0; i < count; i++)
        free(paths[i]);
    free(paths);
}

static int
find_copyengine_resource0_paths(char ***paths_out, size_t *count_out)
{
    if (!paths_out || !count_out) {
        errno = EINVAL;
        return -1;
    }

    *paths_out = NULL;
    *count_out = 0;

    DIR *dir = opendir("/sys/bus/pci/devices");
    if (!dir)
        return -1;

    char **paths = NULL;
    size_t count = 0;
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        char dev_dir[PATH_MAX];
        char vendor_path[PATH_MAX];
        char device_path[PATH_MAX];
        char resource0_path[PATH_MAX];

        if ((size_t)snprintf(dev_dir, sizeof(dev_dir), "/sys/bus/pci/devices/%s",
                             ent->d_name) >= sizeof(dev_dir))
            continue;
        if ((size_t)snprintf(vendor_path, sizeof(vendor_path), "%s/vendor",
                             dev_dir) >= sizeof(vendor_path))
            continue;
        if ((size_t)snprintf(device_path, sizeof(device_path), "%s/device",
                             dev_dir) >= sizeof(device_path))
            continue;
        if ((size_t)snprintf(resource0_path, sizeof(resource0_path), "%s/resource0",
                             dev_dir) >= sizeof(resource0_path))
            continue;

        uint32_t vendor = 0;
        uint32_t device = 0;
        if (read_hex_u32_file(vendor_path, &vendor) != 0)
            continue;
        if (read_hex_u32_file(device_path, &device) != 0)
            continue;
        if (vendor != CXL_CE_PCI_VENDOR_ID || device != CXL_CE_PCI_DEVICE_ID)
            continue;
        if (access(resource0_path, R_OK | W_OK) != 0)
            continue;

        char *copy = strdup(resource0_path);
        if (!copy) {
            int saved = errno ? errno : ENOMEM;
            closedir(dir);
            free_copyengine_paths(paths, count);
            errno = saved;
            return -1;
        }

        char **new_paths = (char **)realloc(paths, (count + 1) * sizeof(*new_paths));
        if (!new_paths) {
            int saved = errno ? errno : ENOMEM;
            free(copy);
            closedir(dir);
            free_copyengine_paths(paths, count);
            errno = saved;
            return -1;
        }

        paths = new_paths;
        paths[count++] = copy;
    }

    int saved = errno;
    closedir(dir);
    if (count == 0) {
        errno = ENOENT;
        return -1;
    }

    qsort(paths, count, sizeof(*paths), cmp_str_ptrs);
    *paths_out = paths;
    *count_out = count;
    errno = saved;
    return 0;
}

static int
enable_copyengine_pci_command(const char *resource0_path)
{
    if (!resource0_path) {
        errno = EINVAL;
        return -1;
    }

    static const char suffix[] = "/resource0";
    size_t path_len = strlen(resource0_path);
    size_t suffix_len = sizeof(suffix) - 1u;
    if (path_len <= suffix_len ||
        strcmp(resource0_path + path_len - suffix_len, suffix) != 0) {
        errno = EINVAL;
        return -1;
    }

    char config_path[PATH_MAX];
    if ((size_t)snprintf(config_path, sizeof(config_path), "%.*s/config",
                         (int)(path_len - suffix_len), resource0_path) >=
        sizeof(config_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = open(config_path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;

    uint16_t cmd = 0;
    ssize_t n = pread(fd, &cmd, sizeof(cmd), (off_t)CXL_CE_PCI_CMD_OFF);
    if (n != (ssize_t)sizeof(cmd)) {
        int saved = errno ? errno : EIO;
        close(fd);
        errno = saved;
        return -1;
    }

    uint16_t wanted = (uint16_t)(cmd | CXL_CE_PCI_CMD_MEM | CXL_CE_PCI_CMD_BM);
    if (wanted != cmd) {
        n = pwrite(fd, &wanted, sizeof(wanted), (off_t)CXL_CE_PCI_CMD_OFF);
        if (n != (ssize_t)sizeof(wanted)) {
            int saved = errno ? errno : EIO;
            close(fd);
            errno = saved;
            return -1;
        }
    }

    close(fd);
    return 0;
}

static int
read_copyengine_resource0_page_off(const char *resource0_path,
                                   size_t page_size,
                                   size_t *page_off_out)
{
    if (!resource0_path || !page_off_out || page_size == 0) {
        errno = EINVAL;
        return -1;
    }

    static const char suffix[] = "/resource0";
    size_t path_len = strlen(resource0_path);
    size_t suffix_len = sizeof(suffix) - 1u;
    if (path_len <= suffix_len ||
        strcmp(resource0_path + path_len - suffix_len, suffix) != 0) {
        errno = EINVAL;
        return -1;
    }

    char resource_table_path[PATH_MAX];
    if ((size_t)snprintf(resource_table_path, sizeof(resource_table_path),
                         "%.*s/resource",
                         (int)(path_len - suffix_len), resource0_path) >=
        sizeof(resource_table_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    FILE *fp = fopen(resource_table_path, "r");
    if (!fp)
        return -1;

    char line[256];
    if (!fgets(line, sizeof(line), fp)) {
        int saved = errno ? errno : EIO;
        fclose(fp);
        errno = saved;
        return -1;
    }
    fclose(fp);

    unsigned long long start = 0;
    unsigned long long end = 0;
    unsigned long long flags = 0;
    if (sscanf(line, "%llx %llx %llx", &start, &end, &flags) != 3) {
        errno = EINVAL;
        return -1;
    }

    *page_off_out = (size_t)(start % (unsigned long long)page_size);
    return 0;
}

static int
virt_to_phys(const void *vaddr, uint64_t *phys_out)
{
    static int pagemap_fd = -2;
    static uint64_t page_size = 0;
    uint64_t entry = 0;
    uint64_t virt = (uint64_t)(uintptr_t)vaddr;

    if (!phys_out) {
        errno = EINVAL;
        return -1;
    }

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

static void
ce_destroy(ce_state_t *ce)
{
    if (!ce)
        return;

    if (ce->bar_map_base && ce->bar_map_len > 0) {
        munmap(ce->bar_map_base, ce->bar_map_len);
        ce->bar_map_base = NULL;
        ce->bar_map_len = 0;
    }
    if (ce->fd >= 0) {
        close(ce->fd);
        ce->fd = -1;
    }

    free(ce->desc_pool);
    free(ce->desc_phys);
    free(ce->src_buf);

    ce->bar0 = NULL;
    ce->desc_pool = NULL;
    ce->desc_phys = NULL;
    ce->src_buf = NULL;
    ce->desc_count = 0;
    ce->src_buf_bytes = 0;
}

static int
ce_init_mmio(ce_state_t *ce, const char *resource0_path, uint32_t channel)
{
    if (!ce || !resource0_path) {
        errno = EINVAL;
        return -1;
    }

    memset(ce, 0, sizeof(*ce));
    ce->fd = -1;
    ce->channel = channel;

    long page_size_l = sysconf(_SC_PAGESIZE);
    if (page_size_l <= 0)
        return -1;
    ce->page_size = (size_t)page_size_l;

    if ((size_t)snprintf(ce->resource0_path, sizeof(ce->resource0_path), "%s",
                         resource0_path) >= sizeof(ce->resource0_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (enable_copyengine_pci_command(resource0_path) != 0)
        return -1;

    size_t bar_page_off = 0;
    if (read_copyengine_resource0_page_off(resource0_path, ce->page_size,
                                           &bar_page_off) != 0)
        return -1;

    ce->fd = open(resource0_path, O_RDWR | O_SYNC | O_CLOEXEC);
    if (ce->fd < 0)
        return -1;

    struct stat st;
    if (fstat(ce->fd, &st) != 0)
        return -1;

    size_t resource_len = (st.st_size > 0) ? (size_t)st.st_size : 0;
    size_t required_bytes = CXL_CE_CHAN_BASE +
        (size_t)channel * CXL_CE_CHAN_STRIDE +
        CXL_CE_CHAN_ERROR + sizeof(uint32_t);
    if (resource_len > 0 && resource_len < required_bytes) {
        errno = EINVAL;
        return -1;
    }
    if (resource_len == 0)
        resource_len = required_bytes;

    size_t map_len = resource_len + bar_page_off;
    map_len = ((map_len + ce->page_size - 1u) / ce->page_size) * ce->page_size;

    ce->bar_map_base = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
                            MAP_SHARED, ce->fd, 0);
    if (ce->bar_map_base == MAP_FAILED) {
        ce->bar_map_base = NULL;
        return -1;
    }

    ce->bar_map_len = map_len;
    ce->bar0 = (volatile uint8_t *)ce->bar_map_base + bar_page_off;
    ce->chan_count = (uint32_t)ce_mmio_read8(ce->bar0, CXL_CE_GEN_CHANCOUNT);
    if (ce->chan_count == 0 || channel >= ce->chan_count) {
        errno = EINVAL;
        return -1;
    }

    uint8_t xfercap_log2 = ce_mmio_read8(ce->bar0, CXL_CE_GEN_XFERCAP);
    if (xfercap_log2 >= 63) {
        errno = EINVAL;
        return -1;
    }
    ce->max_xfer_bytes = (size_t)(1ULL << xfercap_log2);
    ce->chan_mmio_base = CXL_CE_CHAN_BASE + channel * CXL_CE_CHAN_STRIDE;
    return 0;
}

static int
ce_alloc_chain_storage(ce_state_t *ce, size_t desc_count, size_t total_bytes)
{
    if (!ce || desc_count == 0 || total_bytes == 0) {
        errno = EINVAL;
        return -1;
    }

    if (posix_memalign((void **)&ce->desc_pool, 64,
                       desc_count * sizeof(ce_desc_t)) != 0) {
        errno = ENOMEM;
        return -1;
    }
    ce->desc_phys = (uint64_t *)calloc(desc_count, sizeof(uint64_t));
    if (!ce->desc_phys)
        return -1;

    if (posix_memalign((void **)&ce->src_buf, ce->page_size, total_bytes) != 0) {
        errno = ENOMEM;
        return -1;
    }
    ce->src_buf_bytes = total_bytes;
    ce->desc_count = desc_count;

    memset(ce->desc_pool, 0, desc_count * sizeof(ce_desc_t));
    memset(ce->src_buf, 0xA5, total_bytes);

    (void)mlock(ce->desc_pool, desc_count * sizeof(ce_desc_t));
    (void)mlock(ce->src_buf, total_bytes);

    for (size_t i = 0; i < desc_count; i++) {
        if (virt_to_phys(ce->desc_pool + i, &ce->desc_phys[i]) != 0)
            return -1;
    }

    return 0;
}

static int
ce_build_chain(ce_state_t *ce, uint64_t dst_phys_base,
               size_t total_bytes, size_t chunk_bytes)
{
    if (!ce || !ce->desc_pool || !ce->desc_phys || !ce->src_buf ||
        total_bytes == 0 || chunk_bytes == 0) {
        errno = EINVAL;
        return -1;
    }

    size_t off = 0;
    size_t desc_index = 0;
    while (off < total_bytes) {
        if (desc_index >= ce->desc_count) {
            errno = ENOSPC;
            return -1;
        }

        size_t chunk = total_bytes - off;
        if (chunk > chunk_bytes)
            chunk = chunk_bytes;

        uint64_t src_phys = 0;
        if (virt_to_phys(ce->src_buf + off, &src_phys) != 0)
            return -1;

        ce_desc_t *desc = &ce->desc_pool[desc_index];
        memset(desc, 0, sizeof(*desc));
        desc->len = (uint32_t)chunk;
        desc->command = 0;
        desc->src = src_phys;
        desc->dest = dst_phys_base + off;
        desc->next = 0;

        if (desc_index + 1 < ce->desc_count && off + chunk < total_bytes)
            desc->next = ce->desc_phys[desc_index + 1];

        off += chunk;
        desc_index++;
    }

    ce->first_desc_phys = ce->desc_phys[0];
    flush_range(ce->src_buf, ce->src_buf_bytes);
    flush_range(ce->desc_pool, ce->desc_count * sizeof(ce_desc_t));
    store_sfence();
    return 0;
}

static int
ce_wait_idle(ce_state_t *ce, uint64_t timeout_ns)
{
    if (!ce || !ce->bar0) {
        errno = EINVAL;
        return -1;
    }

    uint64_t timeout_cycles = ns_to_cycles(timeout_ns);
    uint64_t start_cycles = rdtsc();
    while (1) {
        uint32_t err = ce_mmio_read32(ce->bar0,
                                      ce->chan_mmio_base + CXL_CE_CHAN_ERROR);
        if (err != 0) {
            fprintf(stderr,
                    "copyengine_bw: channel error resource=%s channel=%u err=0x%08x\n",
                    ce->resource0_path, ce->channel, err);
            return -1;
        }

        uint64_t status = ce_mmio_read64(ce->bar0,
                                         ce->chan_mmio_base + CXL_CE_CHAN_STATUS);
        if ((status & 0x1ULL) != 0)
            return 0;

        if ((rdtsc() - start_cycles) > timeout_cycles) {
            fprintf(stderr,
                    "copyengine_bw: timeout waiting idle resource=%s channel=%u timeout_ns=%" PRIu64 "\n",
                    ce->resource0_path, ce->channel, timeout_ns);
            errno = ETIMEDOUT;
            return -1;
        }

        pause_loop();
    }
}

static int
ce_start_chain(ce_state_t *ce)
{
    if (!ce || !ce->bar0 || ce->first_desc_phys == 0) {
        errno = EINVAL;
        return -1;
    }

    ce_mmio_write32(ce->bar0, ce->chan_mmio_base + CXL_CE_CHAN_ERROR, 0xFFFFFFFFu);
    ce_mmio_write64(ce->bar0, ce->chan_mmio_base + CXL_CE_CHAN_CHAINADDR,
                    ce->first_desc_phys);
    store_sfence();
    ce_mmio_write8(ce->bar0, ce->chan_mmio_base + CXL_CE_CHAN_COMMAND,
                   CXL_CE_CMD_START_DMA);
    return 0;
}

static double
bytes_per_ns_to_gib_per_s(uint64_t bytes, uint64_t ns)
{
    if (ns == 0)
        return 0.0;
    return ((double)bytes * 1000000000.0) /
        ((double)ns * 1024.0 * 1024.0 * 1024.0);
}

static int
verify_copy_result(volatile uint8_t *dst, const uint8_t *src, size_t bytes)
{
    flush_range(dst, bytes);
    store_sfence();

    const uint8_t *dst_bytes = (const uint8_t *)(const void *)dst;
    if (memcmp(dst_bytes, src, bytes) == 0)
        return 0;

    for (size_t i = 0; i < bytes; i++) {
        if (dst_bytes[i] != src[i]) {
            fprintf(stderr,
                    "copyengine_bw: verify mismatch at offset=%zu dst=0x%02x src=0x%02x\n",
                    i, dst_bytes[i], src[i]);
            break;
        }
    }
    return -1;
}

static void
compute_bench_stats(uint64_t *samples_ns, size_t count, bench_stats_t *stats)
{
    if (!stats) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    if (!samples_ns || count == 0)
        return;

    stats->avg_ns = compute_avg_latency(samples_ns, count);
    compute_latency_stats(samples_ns, count,
                          &stats->min_ns, &stats->p50_ns, &stats->p90_ns,
                          &stats->p95_ns, &stats->p99_ns, &stats->max_ns);
}

static int
ce_wait_all_idle(ce_state_t *lanes, size_t lane_count,
                 uint64_t timeout_ns, uint64_t *done_cycles_out)
{
    if (!lanes || lane_count == 0) {
        errno = EINVAL;
        return -1;
    }

    int *done = (int *)calloc(lane_count, sizeof(int));
    if (!done)
        return -1;

    if (done_cycles_out) {
        memset(done_cycles_out, 0, lane_count * sizeof(*done_cycles_out));
    }

    uint64_t timeout_cycles = ns_to_cycles(timeout_ns);
    uint64_t start_cycles = rdtsc();
    size_t done_count = 0;

    while (done_count < lane_count) {
        uint64_t now = rdtsc();
        if ((now - start_cycles) > timeout_cycles) {
            fprintf(stderr,
                    "copyengine_bw: timeout waiting %zu lanes to go idle\n",
                    lane_count);
            free(done);
            errno = ETIMEDOUT;
            return -1;
        }

        for (size_t i = 0; i < lane_count; i++) {
            if (done[i])
                continue;

            uint32_t err = ce_mmio_read32(
                lanes[i].bar0,
                lanes[i].chan_mmio_base + CXL_CE_CHAN_ERROR);
            if (err != 0) {
                fprintf(stderr,
                        "copyengine_bw: channel error lane=%zu resource=%s channel=%u err=0x%08x\n",
                        lanes[i].lane_index, lanes[i].resource0_path,
                        lanes[i].channel, err);
                free(done);
                errno = EIO;
                return -1;
            }

            uint64_t status = ce_mmio_read64(
                lanes[i].bar0,
                lanes[i].chan_mmio_base + CXL_CE_CHAN_STATUS);
            if ((status & 0x1ULL) != 0) {
                done[i] = 1;
                done_count++;
                if (done_cycles_out)
                    done_cycles_out[i] = now - start_cycles;
            }
        }

        if (done_count < lane_count)
            pause_loop();
    }

    free(done);
    return 0;
}

int
main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    int ret = 1;
    int cpu = parse_int_arg(argc, argv, "--cpu", 0, 0, 4095);
    int channel_base = parse_int_arg(argc, argv, "--channel", 0, 0, 63);
    int engine_count_req = parse_int_arg(argc, argv, "--engines", 1, 1, 64);
    int channels_per_engine_req = parse_int_arg(
        argc, argv, "--channels-per-engine", 1, 1, 64);
    int warmup = parse_int_arg(argc, argv, "--warmup", 2, 0, 1000000);
    int loops = parse_int_arg(argc, argv, "--loops", 10, 1, 1000000);
    int verify = parse_int_arg(argc, argv, "--verify", 0, 0, 1);
    uint64_t cxl_base_addr = parse_u64_arg(argc, argv, "--cxl-base",
                                           CXL_BASE_ADDR_DEFAULT);
    uint64_t cxl_map_size = parse_u64_arg(argc, argv, "--cxl-map-size",
                                          CXL_MAP_SIZE_DEFAULT);
    uint64_t dst_offset_base = parse_u64_arg(argc, argv, "--dst-offset",
                                             CXL_DST_OFFSET_DEFAULT);
    uint64_t dst_stride = parse_u64_arg(argc, argv, "--dst-stride",
                                        CXL_DST_STRIDE_AUTO);
    uint64_t total_bytes = parse_u64_arg(argc, argv, "--total-bytes",
                                         64ULL * 1024ULL * 1024ULL);
    uint64_t chunk_bytes = parse_u64_arg(argc, argv, "--chunk-bytes", 0);
    uint64_t timeout_ns = parse_u64_arg(argc, argv, "--timeout-ns",
                                        COPYENGINE_DEFAULT_TIMEOUT_NS);
    const char *resource_arg = parse_str_arg(argc, argv, "--resource", "auto");
    const char *mode = parse_str_arg(argc, argv, "--mode", "both");
    const int run_single = (strcmp(mode, "parallel") != 0);
    const int run_parallel = (strcmp(mode, "single") != 0);
    cxl_context_t *ctx = NULL;
    volatile uint8_t *cxl_base = NULL;
    char **resource_paths = NULL;
    size_t resource_count = 0;
    size_t engine_count = 0;
    size_t lane_count = 0;
    ce_state_t *lanes = NULL;
    volatile uint8_t **dst_virt = NULL;
    uint64_t *dst_offsets = NULL;
    bench_stats_t *single_stats = NULL;
    uint64_t *parallel_ns = NULL;
    uint64_t *parallel_done_cycles = NULL;
    uint64_t *parallel_engine_done_cycles = NULL;
    uint64_t **parallel_lane_ns = NULL;
    uint64_t **parallel_engine_ns = NULL;
    size_t *engine_lane_counts = NULL;
    size_t page_size = 0;
    size_t desc_count = 0;

    if (strcmp(mode, "single") != 0 &&
        strcmp(mode, "parallel") != 0 &&
        strcmp(mode, "both") != 0) {
        fprintf(stderr,
                "copyengine_bw: --mode must be one of single|parallel|both\n");
        return 1;
    }
    if (cxl_map_size == 0 || total_bytes == 0 || timeout_ns == 0) {
        fprintf(stderr, "copyengine_bw: sizes and timeout must be non-zero\n");
        return 1;
    }
    if (channel_base + channels_per_engine_req > 64) {
        fprintf(stderr,
                "copyengine_bw: requested channels [%d, %d] exceed max channel id 63\n",
                channel_base, channel_base + channels_per_engine_req - 1);
        return 1;
    }
    if (engine_count_req > 1 && strcmp(resource_arg, "auto") != 0) {
        fprintf(stderr,
                "copyengine_bw: manual --resource is only supported with --engines 1\n");
        return 1;
    }

    fprintf(stderr,
            "copyengine_bw: start engines=%d channels_per_engine=%d channel_base=%d"
            " total_bytes=%" PRIu64 " loops=%d warmup=%d mode=%s resource=%s\n",
            engine_count_req, channels_per_engine_req, channel_base,
            total_bytes, loops, warmup, mode, resource_arg);

    long page_size_l = sysconf(_SC_PAGESIZE);
    if (page_size_l <= 0) {
        fprintf(stderr, "copyengine_bw: sysconf(_SC_PAGESIZE) failed\n");
        return 1;
    }
    page_size = (size_t)page_size_l;

    if (dst_stride == CXL_DST_STRIDE_AUTO)
        dst_stride = align_up_u64(total_bytes, (uint64_t)page_size);
    if (dst_stride < total_bytes) {
        fprintf(stderr,
                "copyengine_bw: dst_stride=%#" PRIx64 " must be >= total_bytes=%" PRIu64 "\n",
                dst_stride, total_bytes);
        return 1;
    }
    lane_count = (size_t)engine_count_req * (size_t)channels_per_engine_req;
    if (dst_offset_base + (uint64_t)(lane_count - 1) * dst_stride +
            total_bytes > cxl_map_size) {
        fprintf(stderr,
                "copyengine_bw: destination windows exceed mapped CXL region"
                " dst_offset=%#" PRIx64 " dst_stride=%#" PRIx64
                " total_bytes=%" PRIu64 " lanes=%zu cxl_map_size=%#" PRIx64 "\n",
                dst_offset_base, dst_stride, total_bytes,
                lane_count, cxl_map_size);
        return 1;
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
        fprintf(stderr, "copyengine_bw: warn: sched_setaffinity cpu=%d failed: %s\n",
                cpu, strerror(errno));
    }

    if (strcmp(resource_arg, "auto") == 0) {
        if (find_copyengine_resource0_paths(&resource_paths, &resource_count) != 0) {
            fprintf(stderr, "copyengine_bw: no CopyEngine resource0 found\n");
            goto cleanup;
        }
        if (resource_count < (size_t)engine_count_req) {
            fprintf(stderr,
                    "copyengine_bw: requested %d engines but found only %zu\n",
                    engine_count_req, resource_count);
            goto cleanup;
        }
        engine_count = (size_t)engine_count_req;
    } else {
        resource_paths = (char **)calloc(1, sizeof(*resource_paths));
        if (!resource_paths)
            goto cleanup;
        resource_paths[0] = strdup(resource_arg);
        if (!resource_paths[0])
            goto cleanup;
        resource_count = 1;
        engine_count = 1;
    }
    lane_count = engine_count * (size_t)channels_per_engine_req;

    ctx = cxl_rpc_init(cxl_base_addr, (size_t)cxl_map_size);
    if (!ctx) {
        fprintf(stderr, "copyengine_bw: cxl_rpc_init failed\n");
        goto cleanup;
    }

    cxl_base = (volatile uint8_t *)cxl_rpc_get_base(ctx);
    if (!cxl_base) {
        fprintf(stderr, "copyengine_bw: cxl_rpc_get_base failed\n");
        goto cleanup;
    }

    lanes = (ce_state_t *)calloc(lane_count, sizeof(*lanes));
    dst_virt = (volatile uint8_t **)calloc(lane_count, sizeof(*dst_virt));
    dst_offsets = (uint64_t *)calloc(lane_count, sizeof(*dst_offsets));
    single_stats = (bench_stats_t *)calloc(lane_count, sizeof(*single_stats));
    engine_lane_counts = (size_t *)calloc(engine_count, sizeof(*engine_lane_counts));
    if (!lanes || !dst_virt || !dst_offsets || !single_stats ||
        !engine_lane_counts)
        goto cleanup;

    for (size_t engine_idx = 0; engine_idx < engine_count; engine_idx++) {
        for (size_t channel_slot = 0;
             channel_slot < (size_t)channels_per_engine_req;
             channel_slot++) {
            size_t lane_idx =
                engine_idx * (size_t)channels_per_engine_req + channel_slot;
            uint32_t channel_id = (uint32_t)(channel_base + (int)channel_slot);

            if (ce_init_mmio(&lanes[lane_idx], resource_paths[engine_idx],
                             channel_id) != 0) {
                fprintf(stderr,
                        "copyengine_bw: ce_init_mmio failed lane=%zu resource=%s channel=%u err=%s\n",
                        lane_idx, resource_paths[engine_idx], channel_id,
                        strerror(errno));
                goto cleanup;
            }

            lanes[lane_idx].lane_index = lane_idx;
            lanes[lane_idx].engine_index = engine_idx;
            engine_lane_counts[engine_idx]++;

            if (chunk_bytes == 0)
                chunk_bytes = lanes[lane_idx].max_xfer_bytes;
            if (chunk_bytes > lanes[lane_idx].max_xfer_bytes) {
                fprintf(stderr,
                        "copyengine_bw: chunk_bytes=%" PRIu64
                        " exceeds device xfercap=%zu on lane=%zu engine=%zu channel=%u\n",
                        chunk_bytes, lanes[lane_idx].max_xfer_bytes, lane_idx,
                        engine_idx, lanes[lane_idx].channel);
                goto cleanup;
            }
            if ((lanes[lane_idx].page_size % chunk_bytes) != 0) {
                fprintf(stderr,
                        "copyengine_bw: chunk_bytes=%" PRIu64
                        " must divide page_size=%zu on lane=%zu engine=%zu channel=%u\n",
                        chunk_bytes, lanes[lane_idx].page_size, lane_idx,
                        engine_idx, lanes[lane_idx].channel);
                goto cleanup;
            }

            desc_count = (size_t)((total_bytes + chunk_bytes - 1u) / chunk_bytes);
            if (ce_alloc_chain_storage(&lanes[lane_idx], desc_count,
                                       (size_t)total_bytes) != 0) {
                fprintf(stderr,
                        "copyengine_bw: ce_alloc_chain_storage failed lane=%zu engine=%zu channel=%u err=%s\n",
                        lane_idx, engine_idx, lanes[lane_idx].channel,
                        strerror(errno));
                goto cleanup;
            }

            memset(lanes[lane_idx].src_buf,
                   (int)(0xA5u + (uint8_t)lane_idx), (size_t)total_bytes);
            dst_offsets[lane_idx] = dst_offset_base + (uint64_t)lane_idx * dst_stride;
            dst_virt[lane_idx] = cxl_base + dst_offsets[lane_idx];
            memset((void *)dst_virt[lane_idx], 0, (size_t)total_bytes);
            flush_range(dst_virt[lane_idx], (size_t)total_bytes);
            store_sfence();

            if (ce_build_chain(&lanes[lane_idx],
                               cxl_base_addr + dst_offsets[lane_idx],
                               (size_t)total_bytes, (size_t)chunk_bytes) != 0) {
                fprintf(stderr,
                        "copyengine_bw: ce_build_chain failed lane=%zu engine=%zu channel=%u err=%s\n",
                        lane_idx, engine_idx, lanes[lane_idx].channel,
                        strerror(errno));
                goto cleanup;
            }
        }
    }

    printf("copyengine_bw_config,engines=%zu,channels_per_engine=%d,lane_count=%zu,channel_base=%d,total_bytes_per_lane=%" PRIu64 ",chunk_bytes=%" PRIu64 ",warmup=%d,loops=%d,verify=%d,mode=%s,cxl_base=%#" PRIx64 ",cxl_map_size=%#" PRIx64 ",dst_offset=%#" PRIx64 ",dst_stride=%#" PRIx64 "\n",
           engine_count, channels_per_engine_req, lane_count, channel_base,
           total_bytes, chunk_bytes, warmup, loops, verify, mode,
           cxl_base_addr, cxl_map_size, dst_offset_base, dst_stride);
    printf("copyengine_bw_timer,rdtsc_based_ns,cpu_freq_mhz=%d\n",
           CXL_CPU_FREQ_MHZ);
    for (size_t lane_idx = 0; lane_idx < lane_count; lane_idx++) {
        printf("copyengine_bw_lane,lane=%zu,engine=%zu,resource=%s,channel=%u,chan_count=%u,xfercap_bytes=%zu,descriptors=%zu,dst_offset=%#" PRIx64 "\n",
               lane_idx, lanes[lane_idx].engine_index,
               lanes[lane_idx].resource0_path, lanes[lane_idx].channel,
               lanes[lane_idx].chan_count, lanes[lane_idx].max_xfer_bytes,
               lanes[lane_idx].desc_count, dst_offsets[lane_idx]);
        }

    if (run_single) {
        for (size_t lane_idx = 0; lane_idx < lane_count; lane_idx++) {
            for (int i = 0; i < warmup; i++) {
                if (ce_start_chain(&lanes[lane_idx]) != 0 ||
                    ce_wait_idle(&lanes[lane_idx], timeout_ns) != 0) {
                    fprintf(stderr,
                            "copyengine_bw: single warmup failed lane=%zu engine=%zu channel=%u iter=%d err=%s\n",
                            lane_idx, lanes[lane_idx].engine_index,
                            lanes[lane_idx].channel, i, strerror(errno));
                    goto cleanup;
                }
            }

            uint64_t *samples_ns = (uint64_t *)calloc((size_t)loops, sizeof(uint64_t));
            if (!samples_ns)
                goto cleanup;

            for (int i = 0; i < loops; i++) {
                uint64_t t0 = rdtsc();
                if (ce_start_chain(&lanes[lane_idx]) != 0 ||
                    ce_wait_idle(&lanes[lane_idx], timeout_ns) != 0) {
                    fprintf(stderr,
                            "copyengine_bw: single measured loop failed lane=%zu engine=%zu channel=%u iter=%d err=%s\n",
                            lane_idx, lanes[lane_idx].engine_index,
                            lanes[lane_idx].channel, i, strerror(errno));
                    free(samples_ns);
                    goto cleanup;
                }
                uint64_t t1 = rdtsc();
                samples_ns[i] = cycles_to_ns(t1 - t0);
                printf("copyengine_bw_single_loop,lane=%zu,engine=%zu,channel=%u,iter=%d,ns=%" PRIu64 ",gib_per_s=%.3f\n",
                       lane_idx, lanes[lane_idx].engine_index,
                       lanes[lane_idx].channel, i, samples_ns[i],
                       bytes_per_ns_to_gib_per_s(total_bytes, samples_ns[i]));
            }

            if (verify &&
                verify_copy_result(dst_virt[lane_idx], lanes[lane_idx].src_buf,
                                   (size_t)total_bytes) != 0) {
                free(samples_ns);
                goto cleanup;
            }

            compute_bench_stats(samples_ns, (size_t)loops, &single_stats[lane_idx]);
            printf("copyengine_bw_single_summary,lane=%zu,engine=%zu,channel=%u,total_bytes=%" PRIu64 ",loops=%d,min_ns=%" PRIu64 ",avg_ns=%" PRIu64 ",p50_ns=%" PRIu64 ",p90_ns=%" PRIu64 ",p95_ns=%" PRIu64 ",p99_ns=%" PRIu64 ",max_ns=%" PRIu64 ",best_gib_per_s=%.3f,avg_gib_per_s=%.3f,p50_gib_per_s=%.3f,p99_gib_per_s=%.3f,worst_gib_per_s=%.3f\n",
                   lane_idx, lanes[lane_idx].engine_index, lanes[lane_idx].channel,
                   total_bytes, loops,
                   single_stats[lane_idx].min_ns, single_stats[lane_idx].avg_ns,
                   single_stats[lane_idx].p50_ns, single_stats[lane_idx].p90_ns,
                   single_stats[lane_idx].p95_ns, single_stats[lane_idx].p99_ns,
                   single_stats[lane_idx].max_ns,
                   bytes_per_ns_to_gib_per_s(total_bytes, single_stats[lane_idx].min_ns),
                   bytes_per_ns_to_gib_per_s(total_bytes, single_stats[lane_idx].avg_ns),
                   bytes_per_ns_to_gib_per_s(total_bytes, single_stats[lane_idx].p50_ns),
                   bytes_per_ns_to_gib_per_s(total_bytes, single_stats[lane_idx].p99_ns),
                   bytes_per_ns_to_gib_per_s(total_bytes, single_stats[lane_idx].max_ns));
            free(samples_ns);
        }

        if (verify)
            printf("copyengine_bw_single_verify,PASS\n");
    }

    if (run_parallel) {
        parallel_ns = (uint64_t *)calloc((size_t)loops, sizeof(*parallel_ns));
        parallel_done_cycles =
            (uint64_t *)calloc(lane_count, sizeof(*parallel_done_cycles));
        parallel_engine_done_cycles =
            (uint64_t *)calloc(engine_count, sizeof(*parallel_engine_done_cycles));
        parallel_lane_ns =
            (uint64_t **)calloc(lane_count, sizeof(*parallel_lane_ns));
        parallel_engine_ns =
            (uint64_t **)calloc(engine_count, sizeof(*parallel_engine_ns));
        if (!parallel_ns || !parallel_done_cycles || !parallel_engine_done_cycles ||
            !parallel_lane_ns || !parallel_engine_ns)
            goto cleanup;
        for (size_t lane_idx = 0; lane_idx < lane_count; lane_idx++) {
            parallel_lane_ns[lane_idx] =
                (uint64_t *)calloc((size_t)loops, sizeof(**parallel_lane_ns));
            if (!parallel_lane_ns[lane_idx])
                goto cleanup;
        }
        for (size_t engine_idx = 0; engine_idx < engine_count; engine_idx++) {
            parallel_engine_ns[engine_idx] =
                (uint64_t *)calloc((size_t)loops, sizeof(**parallel_engine_ns));
            if (!parallel_engine_ns[engine_idx])
                goto cleanup;
        }

        for (int i = 0; i < warmup; i++) {
            for (size_t lane_idx = 0; lane_idx < lane_count; lane_idx++) {
                if (ce_start_chain(&lanes[lane_idx]) != 0) {
                    fprintf(stderr,
                            "copyengine_bw: parallel warmup start failed lane=%zu engine=%zu channel=%u iter=%d err=%s\n",
                            lane_idx, lanes[lane_idx].engine_index,
                            lanes[lane_idx].channel, i, strerror(errno));
                    goto cleanup;
                }
            }
            if (ce_wait_all_idle(lanes, lane_count, timeout_ns, NULL) != 0) {
                fprintf(stderr,
                        "copyengine_bw: parallel warmup wait failed iter=%d err=%s\n",
                        i, strerror(errno));
                goto cleanup;
            }
        }

        for (int i = 0; i < loops; i++) {
            uint64_t t0 = rdtsc();
            for (size_t lane_idx = 0; lane_idx < lane_count; lane_idx++) {
                if (ce_start_chain(&lanes[lane_idx]) != 0) {
                    fprintf(stderr,
                            "copyengine_bw: parallel start failed lane=%zu engine=%zu channel=%u iter=%d err=%s\n",
                            lane_idx, lanes[lane_idx].engine_index,
                            lanes[lane_idx].channel, i, strerror(errno));
                    goto cleanup;
                }
            }
            if (ce_wait_all_idle(lanes, lane_count, timeout_ns,
                                 parallel_done_cycles) != 0) {
                fprintf(stderr,
                        "copyengine_bw: parallel wait failed iter=%d err=%s\n",
                        i, strerror(errno));
                goto cleanup;
            }
            uint64_t t1 = rdtsc();
            parallel_ns[i] = cycles_to_ns(t1 - t0);
            memset(parallel_engine_done_cycles, 0,
                   engine_count * sizeof(*parallel_engine_done_cycles));
            for (size_t lane_idx = 0; lane_idx < lane_count; lane_idx++) {
                size_t engine_idx = lanes[lane_idx].engine_index;
                parallel_lane_ns[lane_idx][i] =
                    cycles_to_ns(parallel_done_cycles[lane_idx]);
                if (parallel_done_cycles[lane_idx] >
                    parallel_engine_done_cycles[engine_idx]) {
                    parallel_engine_done_cycles[engine_idx] =
                        parallel_done_cycles[lane_idx];
                }
            }
            for (size_t engine_idx = 0; engine_idx < engine_count; engine_idx++) {
                parallel_engine_ns[engine_idx][i] =
                    cycles_to_ns(parallel_engine_done_cycles[engine_idx]);
            }

            printf("copyengine_bw_parallel_loop,iter=%d,engines=%zu,channels_per_engine=%d,lanes=%zu,ns=%" PRIu64 ",total_gib_per_s=%.3f,per_lane_gib_per_s=%.3f\n",
                   i, engine_count, channels_per_engine_req, lane_count,
                   parallel_ns[i],
                   bytes_per_ns_to_gib_per_s(total_bytes * lane_count,
                                             parallel_ns[i]),
                   bytes_per_ns_to_gib_per_s(total_bytes, parallel_ns[i]));
        }

        if (verify) {
            for (size_t lane_idx = 0; lane_idx < lane_count; lane_idx++) {
                if (verify_copy_result(dst_virt[lane_idx], lanes[lane_idx].src_buf,
                                       (size_t)total_bytes) != 0) {
                    goto cleanup;
                }
            }
            printf("copyengine_bw_parallel_verify,PASS\n");
        }

        bench_stats_t parallel_stats;
        compute_bench_stats(parallel_ns, (size_t)loops, &parallel_stats);
        for (size_t lane_idx = 0; lane_idx < lane_count; lane_idx++) {
            bench_stats_t lane_parallel_stats;
            compute_bench_stats(parallel_lane_ns[lane_idx], (size_t)loops,
                                &lane_parallel_stats);
            printf("copyengine_bw_parallel_lane_summary,lane=%zu,engine=%zu,channel=%u,total_bytes=%" PRIu64 ",loops=%d,min_ns=%" PRIu64 ",avg_ns=%" PRIu64 ",p50_ns=%" PRIu64 ",p90_ns=%" PRIu64 ",p95_ns=%" PRIu64 ",p99_ns=%" PRIu64 ",max_ns=%" PRIu64 ",best_gib_per_s=%.3f,avg_gib_per_s=%.3f\n",
                   lane_idx, lanes[lane_idx].engine_index, lanes[lane_idx].channel,
                   total_bytes, loops,
                   lane_parallel_stats.min_ns, lane_parallel_stats.avg_ns,
                   lane_parallel_stats.p50_ns, lane_parallel_stats.p90_ns,
                   lane_parallel_stats.p95_ns, lane_parallel_stats.p99_ns,
                   lane_parallel_stats.max_ns,
                   bytes_per_ns_to_gib_per_s(total_bytes,
                                             lane_parallel_stats.min_ns),
                   bytes_per_ns_to_gib_per_s(total_bytes,
                                             lane_parallel_stats.avg_ns));
        }
        for (size_t engine_idx = 0; engine_idx < engine_count; engine_idx++) {
            bench_stats_t engine_parallel_stats;
            uint64_t engine_total_bytes =
                total_bytes * (uint64_t)engine_lane_counts[engine_idx];
            compute_bench_stats(parallel_engine_ns[engine_idx], (size_t)loops,
                                &engine_parallel_stats);
            printf("copyengine_bw_parallel_engine_summary,engine=%zu,channel_count=%zu,total_bytes=%" PRIu64 ",loops=%d,min_ns=%" PRIu64 ",avg_ns=%" PRIu64 ",p50_ns=%" PRIu64 ",p90_ns=%" PRIu64 ",p95_ns=%" PRIu64 ",p99_ns=%" PRIu64 ",max_ns=%" PRIu64 ",best_gib_per_s=%.3f,avg_gib_per_s=%.3f\n",
                   engine_idx, engine_lane_counts[engine_idx], engine_total_bytes,
                   loops,
                   engine_parallel_stats.min_ns, engine_parallel_stats.avg_ns,
                   engine_parallel_stats.p50_ns, engine_parallel_stats.p90_ns,
                   engine_parallel_stats.p95_ns, engine_parallel_stats.p99_ns,
                   engine_parallel_stats.max_ns,
                   bytes_per_ns_to_gib_per_s(engine_total_bytes,
                                             engine_parallel_stats.min_ns),
                   bytes_per_ns_to_gib_per_s(engine_total_bytes,
                                             engine_parallel_stats.avg_ns));
        }

        double single_sum_avg_gib = 0.0;
        if (run_single) {
            for (size_t lane_idx = 0; lane_idx < lane_count; lane_idx++) {
                single_sum_avg_gib +=
                    bytes_per_ns_to_gib_per_s(total_bytes,
                                              single_stats[lane_idx].avg_ns);
            }
        }

        printf("copyengine_bw_parallel_summary,engines=%zu,channels_per_engine=%d,lanes=%zu,total_bytes_per_lane=%" PRIu64 ",aggregate_bytes=%" PRIu64 ",loops=%d,min_ns=%" PRIu64 ",avg_ns=%" PRIu64 ",p50_ns=%" PRIu64 ",p90_ns=%" PRIu64 ",p95_ns=%" PRIu64 ",p99_ns=%" PRIu64 ",max_ns=%" PRIu64 ",best_total_gib_per_s=%.3f,avg_total_gib_per_s=%.3f,p50_total_gib_per_s=%.3f,p99_total_gib_per_s=%.3f,worst_total_gib_per_s=%.3f,avg_per_lane_gib_per_s=%.3f",
               engine_count, channels_per_engine_req, lane_count,
               total_bytes, total_bytes * lane_count, loops,
               parallel_stats.min_ns, parallel_stats.avg_ns,
               parallel_stats.p50_ns, parallel_stats.p90_ns,
               parallel_stats.p95_ns, parallel_stats.p99_ns,
               parallel_stats.max_ns,
               bytes_per_ns_to_gib_per_s(total_bytes * lane_count,
                                         parallel_stats.min_ns),
               bytes_per_ns_to_gib_per_s(total_bytes * lane_count,
                                         parallel_stats.avg_ns),
               bytes_per_ns_to_gib_per_s(total_bytes * lane_count,
                                         parallel_stats.p50_ns),
               bytes_per_ns_to_gib_per_s(total_bytes * lane_count,
                                         parallel_stats.p99_ns),
               bytes_per_ns_to_gib_per_s(total_bytes * lane_count,
                                         parallel_stats.max_ns),
               bytes_per_ns_to_gib_per_s(total_bytes * lane_count,
                                         parallel_stats.avg_ns) /
                   (double)lane_count);
        if (run_single && single_sum_avg_gib > 0.0) {
            double avg_total_gib =
                bytes_per_ns_to_gib_per_s(total_bytes * lane_count,
                                          parallel_stats.avg_ns);
            printf(",single_sum_avg_gib_per_s=%.3f,bus_pressure_ratio=%.3f",
                   single_sum_avg_gib, avg_total_gib / single_sum_avg_gib);
        }
        printf("\n");
    }

    ret = 0;

cleanup:
    if (parallel_lane_ns) {
        for (size_t lane_idx = 0; lane_idx < lane_count; lane_idx++)
            free(parallel_lane_ns[lane_idx]);
    }
    free(parallel_lane_ns);
    if (parallel_engine_ns) {
        for (size_t engine_idx = 0; engine_idx < engine_count; engine_idx++)
            free(parallel_engine_ns[engine_idx]);
    }
    free(parallel_engine_ns);
    free(parallel_engine_done_cycles);
    free(parallel_done_cycles);
    free(parallel_ns);
    free(single_stats);
    free(engine_lane_counts);
    free(dst_offsets);
    free(dst_virt);
    if (lanes) {
        for (size_t lane_idx = 0; lane_idx < lane_count; lane_idx++)
            ce_destroy(&lanes[lane_idx]);
    }
    free(lanes);
    if (ctx)
        cxl_rpc_destroy(ctx);
    free_copyengine_paths(resource_paths, resource_count);
    return ret;
}
