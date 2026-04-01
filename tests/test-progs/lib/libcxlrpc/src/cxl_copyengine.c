#include "cxl_rpc_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* CopyEngine (IOAT-like) MMIO register constants. */
#define CXL_CE_GEN_CHANCOUNT  0x00u
#define CXL_CE_GEN_XFERCAP    0x01u
#define CXL_CE_CHAN_BASE      0x80u
#define CXL_CE_CHAN_STRIDE    0x80u
#define CXL_CE_CHAN_STATUS    0x04u
#define CXL_CE_CHAN_CHAINADDR 0x0Cu
#define CXL_CE_CHAN_COMMAND   0x14u
#define CXL_CE_CHAN_ERROR     0x28u
#define CXL_CE_CMD_START_DMA  0x01u
#define CXL_CE_CMD_APPEND_DMA 0x02u
#define CXL_CE_PCI_CMD_OFF    0x04u
#define CXL_CE_PCI_CMD_MEM    0x0002u
#define CXL_CE_PCI_CMD_BM     0x0004u
#define CXL_CE_PCI_VENDOR_ID  0x8086u
#define CXL_CE_PCI_DEVICE_ID  0x1A38u

/*
 * Dedicated response-DMA resources per lane:
 * - reusable response submissions with persistent local staging buffers that
 *   keep [response header][payload] contiguous for true asynchronous DMA
 * - one cacheline-sized local source for the 8B producer cursor flag per
 *   reusable submission
 * - one reusable descriptor pool per queued response submission
 *
 * The public response ring protocol stays unchanged:
 *   [8B response header][payload] in response_data, then producer cursor flag.
 * Only the local DMA source side is staged so cxl_send_response() can return
 * without waiting for CopyEngine to drain. Submission-local staging resources
 * are retained on the lane and recycled after the hardware chain goes idle.
 */
#define CXL_CE_FLAG_SRC_BYTES   64u
#define CXL_CE_FIXED_DRAIN_SPINS (1u << 22)
#define CXL_CE_EAGER_RESP_PREP_PAGES 120u
/* Smallest BAR window covering MMIO offsets we actually access. */
#define CXL_CE_MMIO_REQUIRED_BYTES \
    (CXL_CE_CHAN_BASE + CXL_CE_CHAN_COMMAND + sizeof(uint8_t))

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct cxl_ce_desc {
    uint32_t len;
    uint32_t command;
    uint64_t src;
    uint64_t dest;
    uint64_t next;
    uint64_t reserved1;
    uint64_t reserved2;
    uint64_t user1;
    uint64_t user2;
};

_Static_assert(sizeof(cxl_ce_desc_t) == 64u,
               "CopyEngine descriptor size must match model");

typedef struct {
    int init_attempted;
    int ready;
    int warned;
    size_t attached_conns;
    size_t engine_count;
    size_t channel_count;
    struct cxl_ce_engine_state *engines;
    struct cxl_ce_channel_state *channels;
} cxl_ce_shared_state_t;

typedef struct cxl_ce_engine_state {
    int bar_fd;
    volatile uint8_t *bar0;
    void *bar_map_base;
    size_t bar_map_len;
    uint32_t chan_count;
    size_t xfer_cap_bytes;
    char resource0_path[PATH_MAX];
} cxl_ce_engine_state_t;

typedef struct cxl_ce_submission_state {
    struct cxl_ce_submission_state *next;
    uint8_t *entry_src;
    uint8_t *flag_src;
    cxl_ce_desc_t *desc_pool;
    cxl_ce_desc_t *tail_desc;
    uint64_t *entry_page_phys;
    uint64_t *desc_phys;
    uint64_t first_desc_phys;
    uint64_t flag_src_phys;
    uint8_t reusable_flag_only;
    uint8_t reusable_response;
    size_t entry_capacity;
    uintptr_t entry_page_base;
    size_t entry_page_size;
    size_t entry_page_count;
    size_t desc_capacity;
} cxl_ce_submission_t;

typedef struct cxl_ce_channel_state {
    cxl_ce_engine_state_t *engine;
    size_t bound_conn_count;
    size_t engine_index;
    size_t channel_index;
    uint32_t chan_id;
    uint32_t chan_mmio_base;
    int chain_started;
    int poisoned;
    cxl_ce_submission_t *submit_head;
    cxl_ce_submission_t *submit_tail;
    cxl_ce_submission_t *flag_free_head;
    cxl_ce_submission_t *response_free_head;
    cxl_ce_desc_t *last_desc;
} cxl_ce_channel_state_t;

static cxl_ce_shared_state_t g_cxl_ce = {0};

static int
cxl_copyengine_error_log_permitted(void)
{
    if (g_cxl_ce.warned)
        return 0;

    g_cxl_ce.warned = 1;
    return 1;
}

static int
cxl_copyengine_debug_enabled(void)
{
    static int initialized = 0;
    static int enabled = 0;
    if (!initialized) {
        const char *env = getenv("CXL_RPC_DEBUG_COPYENGINE");
        enabled = (env && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
        initialized = 1;
    }
    return enabled;
}

static void
cxl_copyengine_debug_dump_bytes(const char *label,
                                const uint8_t *ptr,
                                size_t bytes)
{
    if (!label)
        label = "bytes";

    fprintf(stderr, "[libcxlrpc][CE] %s=", label);
    if (!ptr || bytes == 0) {
        fputc('\n', stderr);
        return;
    }

    for (size_t i = 0; i < bytes; ++i)
        fprintf(stderr, "%02x", (unsigned)ptr[i]);
    fputc('\n', stderr);
}

static int
cmp_str_ptrs(const void *a, const void *b)
{
    const char *const *lhs = (const char *const *)a;
    const char *const *rhs = (const char *const *)b;
    return strcmp(*lhs, *rhs);
}

static inline void
cxl_ce_mmio_write8(volatile uint8_t *bar0, uint32_t off, uint8_t val)
{
    *(volatile uint8_t *)(bar0 + off) = val;
}

static inline void
cxl_ce_mmio_write64(volatile uint8_t *bar0, uint32_t off, uint64_t val)
{
    *(volatile uint64_t *)(bar0 + off) = val;
}

static inline uint32_t
cxl_ce_mmio_read32(volatile uint8_t *bar0, uint32_t off)
{
    return *(volatile uint32_t *)(bar0 + off);
}

static inline uint8_t
cxl_ce_mmio_read8(volatile uint8_t *bar0, uint32_t off)
{
    return *(volatile uint8_t *)(bar0 + off);
}

static inline uint64_t
cxl_ce_mmio_read64(volatile uint8_t *bar0, uint32_t off)
{
    return *(volatile uint64_t *)(bar0 + off);
}

static inline void
cxl_copyengine_flush_cpu_range(volatile uint8_t *ptr, size_t size)
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

static int cxl_copyengine_channel_wait_idle(cxl_connection_t *conn,
                                            cxl_ce_channel_state_t *chan);

static cxl_ce_channel_state_t *
cxl_copyengine_get_assigned_channel(const cxl_connection_t *conn)
{
    if (!conn || !g_cxl_ce.ready || !conn->ce_lane_assigned)
        return NULL;
    if (conn->ce_channel_index >= g_cxl_ce.channel_count)
        return NULL;

    return &g_cxl_ce.channels[conn->ce_channel_index];
}

int
cxl_copyengine_validate_submit_invariants(cxl_connection_t *conn)
{
    cxl_ce_channel_state_t *chan = NULL;

    if (!conn)
        return -1;

    chan = cxl_copyengine_get_assigned_channel(conn);
    if (!chan || chan->poisoned || !chan->engine || !chan->engine->bar0)
        return -1;

    if (!conn->ce_peer_resp_page_phys || conn->ce_peer_resp_page_count == 0 ||
        conn->ce_peer_resp_page_size == 0 ||
        conn->ce_peer_resp_virt_page_base == 0) {
        if (cxl_copyengine_error_log_permitted()) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine peer response map is not ready\n");
        }
        return -1;
    }

    if (!conn->ce_peer_flag_phys_valid || conn->ce_peer_flag_phys == 0)
        return -1;

    return 0;
}

static void
cxl_copyengine_clear_peer_phys_cache(cxl_connection_t *conn)
{
    if (!conn)
        return;

    free(conn->ce_peer_resp_page_phys);
    conn->ce_peer_resp_page_phys = NULL;
    conn->ce_peer_resp_logical_page_base = 0;
    conn->ce_peer_resp_page_size = 0;
    conn->ce_peer_resp_page_count = 0;
    conn->ce_peer_resp_virt_page_base = 0;
    conn->ce_peer_flag_phys = 0;
    conn->ce_peer_flag_phys_valid = 0;
}

static void
cxl_copyengine_submission_free(cxl_ce_submission_t *sub)
{
    if (!sub)
        return;

    free(sub->entry_src);
    free(sub->flag_src);
    free(sub->desc_pool);
    free(sub->entry_page_phys);
    free(sub->desc_phys);
    free(sub);
}

static void
cxl_copyengine_channel_recycle_submission(cxl_ce_channel_state_t *chan,
                                          cxl_ce_submission_t *sub)
{
    if (!sub)
        return;

    if (chan && sub->reusable_flag_only) {
        sub->next = chan->flag_free_head;
        chan->flag_free_head = sub;
        return;
    }
    if (chan && sub->reusable_response) {
        sub->next = chan->response_free_head;
        chan->response_free_head = sub;
        return;
    }

    cxl_copyengine_submission_free(sub);
}

static void
cxl_copyengine_channel_release_submissions(cxl_ce_channel_state_t *chan)
{
    cxl_ce_submission_t *sub = NULL;
    cxl_ce_submission_t *next = NULL;

    if (!chan)
        return;

    sub = chan->submit_head;
    while (sub) {
        next = sub->next;
        cxl_copyengine_channel_recycle_submission(chan, sub);
        sub = next;
    }

    chan->submit_head = NULL;
    chan->submit_tail = NULL;
    chan->last_desc = NULL;
}

static void
cxl_copyengine_channel_release_free_submissions(cxl_ce_channel_state_t *chan)
{
    cxl_ce_submission_t *sub = NULL;
    cxl_ce_submission_t *next = NULL;

    if (!chan)
        return;

    sub = chan->flag_free_head;
    while (sub) {
        next = sub->next;
        cxl_copyengine_submission_free(sub);
        sub = next;
    }
    chan->flag_free_head = NULL;

    sub = chan->response_free_head;
    while (sub) {
        next = sub->next;
        cxl_copyengine_submission_free(sub);
        sub = next;
    }
    chan->response_free_head = NULL;
}

static void
cxl_copyengine_channel_release_buffers(cxl_ce_channel_state_t *chan)
{
    if (!chan)
        return;

    cxl_copyengine_channel_release_submissions(chan);
    cxl_copyengine_channel_release_free_submissions(chan);
    chan->chain_started = 0;
}

static void
cxl_copyengine_clear_conn_state(cxl_connection_t *conn)
{
    if (!conn)
        return;

    conn->ce_lane_assigned = 0;
    conn->resp_tx_ready = 0;
    conn->ce_engine_index = 0;
    conn->ce_channel_index = 0;
    conn->ce_hw_channel_id = 0;
    cxl_copyengine_clear_peer_phys_cache(conn);
}

static int
cxl_copyengine_conn_matches_channel(const cxl_connection_t *conn,
                                    const cxl_ce_channel_state_t *chan)
{
    if (!conn || !chan || !conn->ce_lane_assigned)
        return 0;

    return conn->ce_channel_index == chan->channel_index;
}

static void
cxl_copyengine_global_reset(void)
{
    for (size_t i = 0; i < g_cxl_ce.engine_count; i++) {
        cxl_ce_engine_state_t *engine = &g_cxl_ce.engines[i];
        if (engine->bar_map_base && engine->bar_map_len > 0) {
            munmap(engine->bar_map_base, engine->bar_map_len);
        }
        if (engine->bar_fd >= 0) {
            close(engine->bar_fd);
        }
    }

    for (size_t i = 0; i < g_cxl_ce.channel_count; i++) {
        cxl_ce_channel_state_t *chan = &g_cxl_ce.channels[i];
        cxl_copyengine_channel_release_buffers(chan);
    }

    free(g_cxl_ce.engines);
    free(g_cxl_ce.channels);

    memset(&g_cxl_ce, 0, sizeof(g_cxl_ce));
}

static int
cxl_read_hex_u32_file(const char *path, uint32_t *val_out)
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
    unsigned long v = strtoul(buf, &end, 0);
    if (errno != 0 || end == buf) {
        errno = EINVAL;
        return -1;
    }

    *val_out = (uint32_t)v;
    return 0;
}

static int
cxl_enable_copyengine_pci_command(const char *resource0_path)
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

    size_t dev_path_len = path_len - suffix_len;
    char config_path[PATH_MAX];
    if ((size_t)snprintf(config_path, sizeof(config_path), "%.*s/config",
                         (int)dev_path_len, resource0_path) >=
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

        uint16_t verify = 0;
        n = pread(fd, &verify, sizeof(verify), (off_t)CXL_CE_PCI_CMD_OFF);
        if (n != (ssize_t)sizeof(verify) ||
            (verify & (CXL_CE_PCI_CMD_MEM | CXL_CE_PCI_CMD_BM)) !=
                (CXL_CE_PCI_CMD_MEM | CXL_CE_PCI_CMD_BM)) {
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
cxl_read_copyengine_resource0_page_off(const char *resource0_path,
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

    size_t dev_path_len = path_len - suffix_len;
    char resource_table_path[PATH_MAX];
    if ((size_t)snprintf(resource_table_path, sizeof(resource_table_path),
                         "%.*s/resource", (int)dev_path_len, resource0_path) >=
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
    int parsed = sscanf(line, "%llx %llx %llx", &start, &end, &flags);
    (void)end;
    (void)flags;
    if (parsed != 3) {
        errno = EINVAL;
        return -1;
    }

    *page_off_out = (size_t)(start % (unsigned long long)page_size);
    return 0;
}

static void
cxl_free_copyengine_paths(char **paths, size_t count)
{
    if (!paths)
        return;
    for (size_t i = 0; i < count; i++)
        free(paths[i]);
    free(paths);
}

static int
cxl_find_copyengine_resource0_paths(char ***paths_out, size_t *count_out)
{
    if (!paths_out || !count_out) {
        errno = EINVAL;
        return -1;
    }

    *paths_out = NULL;
    *count_out = 0;

    const char *sysfs_root = "/sys/bus/pci/devices";
    DIR *dir = opendir(sysfs_root);
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

        if ((size_t)snprintf(dev_dir, sizeof(dev_dir), "%s/%s",
                             sysfs_root, ent->d_name) >= sizeof(dev_dir)) {
            continue;
        }
        if ((size_t)snprintf(vendor_path, sizeof(vendor_path), "%s/vendor",
                             dev_dir) >= sizeof(vendor_path)) {
            continue;
        }
        if ((size_t)snprintf(device_path, sizeof(device_path), "%s/device",
                             dev_dir) >= sizeof(device_path)) {
            continue;
        }
        if ((size_t)snprintf(resource0_path, sizeof(resource0_path),
                             "%s/resource0", dev_dir) >= sizeof(resource0_path)) {
            continue;
        }

        uint32_t vendor = 0;
        uint32_t device = 0;
        if (cxl_read_hex_u32_file(vendor_path, &vendor) != 0)
            continue;
        if (cxl_read_hex_u32_file(device_path, &device) != 0)
            continue;

        if (vendor != CXL_CE_PCI_VENDOR_ID || device != CXL_CE_PCI_DEVICE_ID)
            continue;

        if (access(resource0_path, R_OK | W_OK) != 0)
            continue;

        char *path_copy = strdup(resource0_path);
        if (!path_copy) {
            int saved = errno ? errno : ENOMEM;
            closedir(dir);
            cxl_free_copyengine_paths(paths, count);
            errno = saved;
            return -1;
        }

        char **new_paths = (char **)realloc(paths, (count + 1) * sizeof(*new_paths));
        if (!new_paths) {
            int saved = errno ? errno : ENOMEM;
            free(path_copy);
            closedir(dir);
            cxl_free_copyengine_paths(paths, count);
            errno = saved;
            return -1;
        }

        paths = new_paths;
        paths[count++] = path_copy;
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
cxl_virt_to_phys(const void *vaddr, uint64_t *phys_out)
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

static int
cxl_prepare_virt_page_phys_cache(const void *virt_base,
                                 size_t size,
                                 uintptr_t *virt_page_base_out,
                                 size_t *page_size_out,
                                 size_t *page_count_out,
                                 uint64_t **page_phys_out)
{
    if (!virt_base || size == 0 ||
        !virt_page_base_out || !page_size_out ||
        !page_count_out || !page_phys_out) {
        return -1;
    }

    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0)
        return -1;
    const size_t page_size = (size_t)ps;
    const uintptr_t page_mask = ~((uintptr_t)page_size - 1u);

    const uintptr_t virt_start = (uintptr_t)virt_base;
    const uintptr_t virt_end = virt_start + size;
    const uintptr_t page_start = virt_start & page_mask;
    const uintptr_t page_end =
        (virt_end + (uintptr_t)page_size - 1u) & page_mask;
    if (page_end <= page_start)
        return -1;

    const size_t page_count = (size_t)((page_end - page_start) / page_size);
    uint64_t *page_phys = (uint64_t *)malloc(page_count * sizeof(uint64_t));
    if (!page_phys)
        return -1;

    for (size_t i = 0; i < page_count; i++)
        page_phys[i] = UINT64_MAX;

    *virt_page_base_out = page_start;
    *page_size_out = page_size;
    *page_count_out = page_count;
    *page_phys_out = page_phys;
    return 0;
}

static int
cxl_resolve_virt_page_phys(uintptr_t page_virt,
                           size_t page_size,
                           int lock_page,
                           uint64_t *page_phys_out)
{
    if (!page_phys_out || page_size == 0)
        return -1;

    if (lock_page)
        (void)mlock((const void *)page_virt, page_size);

    if (lock_page) {
        volatile uint8_t page_touch =
            *(const volatile uint8_t *)(uintptr_t)page_virt;
        (void)page_touch;

        /*
         * First-touch instantiates the shared backing page so pagemap returns a
         * stable PFN for the DMA target. This CXL path is not DMA-coherent, so
         * immediately evict the instantiated page from CPU caches before the
         * hardware publish can target it.
         */
        cxl_copyengine_flush_cpu_range((volatile uint8_t *)page_virt,
                                       page_size);
    }

    uint64_t phys = 0;
    if (cxl_virt_to_phys((const void *)page_virt, &phys) != 0)
        return -1;
    *page_phys_out = phys & ~((uint64_t)page_size - 1u);
    return 0;
}

static int
cxl_copyengine_get_peer_response_page_phys(cxl_connection_t *conn,
                                           size_t page_index,
                                           uint64_t *page_phys_out)
{
    if (!conn || !page_phys_out || !conn->ce_peer_resp_page_phys ||
        page_index >= conn->ce_peer_resp_page_count ||
        conn->ce_peer_resp_page_size == 0 ||
        conn->ce_peer_resp_virt_page_base == 0) {
        return -1;
    }

    uint64_t page_phys = conn->ce_peer_resp_page_phys[page_index];
    if (page_phys == UINT64_MAX) {
        const uintptr_t page_virt =
            conn->ce_peer_resp_virt_page_base +
            page_index * (uintptr_t)conn->ce_peer_resp_page_size;
        if (cxl_resolve_virt_page_phys(page_virt,
                                       conn->ce_peer_resp_page_size,
                                       1,
                                       &page_phys) != 0) {
            return -1;
        }
        conn->ce_peer_resp_page_phys[page_index] = page_phys;
    }

    *page_phys_out = page_phys;
    return 0;
}

static int
cxl_copyengine_eager_prepare_peer_response_pages(uintptr_t virt_page_base,
                                                 size_t page_size,
                                                 size_t page_count,
                                                 uint64_t *page_phys,
                                                 size_t *prepared_pages_out)
{
    size_t prepared_pages = 0;

    if (!page_phys || page_size == 0) {
        return -1;
    }

    prepared_pages = page_count;
    if (prepared_pages > (size_t)CXL_CE_EAGER_RESP_PREP_PAGES)
        prepared_pages = (size_t)CXL_CE_EAGER_RESP_PREP_PAGES;

    for (size_t i = 0; i < prepared_pages; ++i) {
        const uintptr_t page_virt = virt_page_base + i * (uintptr_t)page_size;

        if (cxl_resolve_virt_page_phys(page_virt, page_size, 1,
                                       &page_phys[i]) != 0) {
            return -1;
        }
    }

    if (prepared_pages_out)
        *prepared_pages_out = prepared_pages;
    return 0;
}

int
cxl_copyengine_update_peer_response_mapping(cxl_connection_t *conn)
{
    size_t eager_page_count = 0;

    if (!conn || !conn->peer_response_data || conn->peer_response_data_size == 0)
        return -1;

    if (cxl_copyengine_debug_enabled()) {
        fprintf(stderr,
                "[libcxlrpc][CE] update_peer_response_mapping begin response_data=%p size=%zu logical_addr=%#llx\n",
                (const void *)conn->peer_response_data,
                conn->peer_response_data_size,
                (unsigned long long)conn->peer_response_data_addr);
    }

    uint64_t *page_phys = NULL;
    uintptr_t virt_page_base = 0;
    size_t page_size = 0;
    size_t page_count = 0;
    if (cxl_prepare_virt_page_phys_cache(
            (const void *)conn->peer_response_data,
            conn->peer_response_data_size,
            &virt_page_base, &page_size, &page_count, &page_phys) != 0) {
        return -1;
    }

    /*
     * Current RPC experiments consume only a bounded prefix of each peer
     * response window. Resolve that prefix up front so response DMA does not
     * pay per-page mlock/touch/flush/pagemap work on the measured path.
     */
    if (cxl_copyengine_eager_prepare_peer_response_pages(virt_page_base,
                                                         page_size,
                                                         page_count,
                                                         page_phys,
                                                         &eager_page_count) != 0) {
        free(page_phys);
        return -1;
    }

    free(conn->ce_peer_resp_page_phys);
    conn->ce_peer_resp_page_phys = page_phys;
    conn->ce_peer_resp_logical_page_base =
        conn->peer_response_data_addr -
        (uint64_t)((uintptr_t)conn->peer_response_data - virt_page_base);
    conn->ce_peer_resp_page_size = page_size;
    conn->ce_peer_resp_page_count = page_count;
    conn->ce_peer_resp_virt_page_base = virt_page_base;
    if (cxl_copyengine_debug_enabled()) {
        fprintf(stderr,
                "[libcxlrpc][CE] update_peer_response_mapping ready page_base=%#llx virt_page_base=%#llx page_size=%zu page_count=%zu eager_pages=%zu\n",
                (unsigned long long)conn->ce_peer_resp_logical_page_base,
                (unsigned long long)conn->ce_peer_resp_virt_page_base,
                conn->ce_peer_resp_page_size,
                conn->ce_peer_resp_page_count,
                eager_page_count);
    }
    return 0;
}

int
cxl_copyengine_update_peer_flag_mapping(cxl_connection_t *conn)
{
    if (!conn)
        return -1;

    conn->ce_peer_flag_phys = 0;
    conn->ce_peer_flag_phys_valid = 0;

    if (!conn->peer_flag)
        return 0;

    if (cxl_copyengine_debug_enabled()) {
        fprintf(stderr,
                "[libcxlrpc][CE] update_peer_flag_mapping begin peer_flag=%p logical_addr=%#llx\n",
                (const void *)conn->peer_flag,
                (unsigned long long)conn->peer_flag_addr);
    }

    long ps = sysconf(_SC_PAGESIZE);
    if (ps > 0) {
        const uintptr_t page_mask = ~((uintptr_t)ps - 1u);
        const uintptr_t flag_page = ((uintptr_t)conn->peer_flag) & page_mask;
        (void)mlock((const void *)flag_page, (size_t)ps);
    }

    uint64_t flag_phys = 0;
    if (cxl_virt_to_phys((const void *)conn->peer_flag, &flag_phys) != 0)
        return -1;

    conn->ce_peer_flag_phys = flag_phys;
    conn->ce_peer_flag_phys_valid = 1;
    if (cxl_copyengine_debug_enabled()) {
        fprintf(stderr,
                "[libcxlrpc][CE] update_peer_flag_mapping ready flag_phys=%#llx\n",
                (unsigned long long)conn->ce_peer_flag_phys);
    }
    return 0;
}

static int
cxl_copyengine_channel_init(cxl_ce_channel_state_t *chan)
{
    if (!chan)
        return -1;

    chan->bound_conn_count = 0;
    chan->poisoned = 0;
    chan->chain_started = 0;
    chan->submit_head = NULL;
    chan->submit_tail = NULL;
    chan->flag_free_head = NULL;
    chan->response_free_head = NULL;
    chan->last_desc = NULL;
    return 0;
}

void
cxl_connection_init_runtime_defaults(cxl_connection_t *conn)
{
    if (!conn)
        return;

    conn->caps = 0;
    conn->mq_total_lines = 0;
    conn->rpc_id_next = 1;
    conn->mq_prefetch_start_line = 0;
    conn->mq_prefetch_nr_lines = 0;
    conn->mq_prefetch_window_valid = 0;
    conn->mq_payload_prepared_mask = 0;
    conn->mq_payload_next_probe_slot = 1;
    conn->request_data_prefetch_enabled = 1;
    conn->notify_prefetch_enabled = 1;
    conn->resp_read_cursor = 0;
    conn->resp_known_producer_cursor = 0;
    conn->resp_peek_cursor = 0;
    conn->resp_peek_len = 0;
    conn->resp_peek_rpc_id = 0;
    conn->resp_peek_valid = 0;
    conn->resp_peek_payload_loaded = 0;
    conn->req_write_offset = 0;
    conn->response_dma_payload_threshold =
        (size_t)CXL_RESPONSE_DMA_PAYLOAD_THRESHOLD_DEFAULT;
    conn->ce_lane_bind_valid = 0;
    conn->ce_bind_lane_index_valid = 0;
    conn->ce_bind_engine_index = 0;
    conn->ce_bind_lane_index = 0;
    conn->ce_bind_channel_id = 0;

    cxl_copyengine_clear_conn_state(conn);
}

void
cxl_copyengine_disable(cxl_connection_t *conn)
{
    cxl_ce_channel_state_t *chan = NULL;
    int wait_rc = 0;
    int last_bound_conn = 0;

    if (!conn)
        return;

    chan = cxl_copyengine_get_assigned_channel(conn);
    if (chan) {
        if (cxl_copyengine_conn_matches_channel(conn, chan) &&
            chan->bound_conn_count > 0) {
            chan->bound_conn_count--;
            if (g_cxl_ce.attached_conns > 0)
                g_cxl_ce.attached_conns--;
        }
        last_bound_conn = (chan->bound_conn_count == 0);
        if (last_bound_conn) {
            wait_rc = cxl_copyengine_channel_wait_idle(conn, chan);
            if (wait_rc != 0)
                chan->poisoned = 1;
        }
    }
    cxl_copyengine_clear_conn_state(conn);

    if (wait_rc != 0)
        return;

    if (chan && last_bound_conn) {
        /*
         * Leave channel submission/free pools intact while the global
         * CopyEngine state remains live. They are released by the final global
         * reset after the last bound connection detaches.
         */
    }

    if (g_cxl_ce.attached_conns == 0)
        cxl_copyengine_global_reset();
}

static int
cxl_copyengine_attach_channel(cxl_connection_t *conn,
                              cxl_ce_channel_state_t *chan)
{
    if (!conn || !chan)
        return -1;

    if (conn->ce_lane_assigned) {
        if (!cxl_copyengine_conn_matches_channel(conn, chan))
            return -1;
    } else {
        chan->bound_conn_count++;
        g_cxl_ce.attached_conns++;
    }
    conn->ce_lane_assigned = 1;
    conn->ce_engine_index = chan->engine_index;
    conn->ce_channel_index = chan->channel_index;
    conn->ce_hw_channel_id = chan->chan_id;
    return 0;
}

static int
cxl_copyengine_init(cxl_connection_t *conn)
{
    size_t page_size = 0;
    size_t map_len = 0;
    size_t bar_page_off = 0;
    size_t total_channels = 0;
    int fail_errno = 0;
    const char *fail_stage = "unknown";
    const char *resource = NULL;
    char **resource_paths = NULL;
    size_t resource_count = 0;

    if (!conn)
        return 0;
    if (cxl_copyengine_get_assigned_channel(conn)) {
        return 1;
    }
    if (g_cxl_ce.ready) {
        return 1;
    }
    if (g_cxl_ce.init_attempted) {
        return 0;
    }

    g_cxl_ce.init_attempted = 1;

    if (cxl_copyengine_debug_enabled()) {
        fprintf(stderr,
                "[libcxlrpc][CE] init begin lane_bind_valid=%d lane_index_valid=%d bind_engine=%zu bind_channel=%u bind_lane=%zu\n",
                conn->ce_lane_bind_valid,
                conn->ce_bind_lane_index_valid,
                conn->ce_bind_engine_index,
                (unsigned)conn->ce_bind_channel_id,
                conn->ce_bind_lane_index);
    }

    if (cxl_find_copyengine_resource0_paths(&resource_paths,
                                            &resource_count) != 0) {
        fail_stage = "find_copyengine_resource0_paths";
        fail_errno = errno;
        goto fail;
    }

    if (cxl_copyengine_debug_enabled()) {
        fprintf(stderr,
                "[libcxlrpc][CE] init discovered_resources count=%zu\n",
                resource_count);
        for (size_t i = 0; i < resource_count; ++i) {
            fprintf(stderr,
                    "[libcxlrpc][CE] init resource[%zu]=%s\n",
                    i, resource_paths[i]);
        }
    }

    long page_size_l = sysconf(_SC_PAGESIZE);
    if (page_size_l <= 0) {
        fail_stage = "sysconf(_SC_PAGESIZE)";
        fail_errno = (errno != 0) ? errno : EINVAL;
        goto fail;
    }
    page_size = (size_t)page_size_l;

    g_cxl_ce.engines = (cxl_ce_engine_state_t *)calloc(
        resource_count, sizeof(*g_cxl_ce.engines));
    if (!g_cxl_ce.engines) {
        fail_stage = "calloc(engines)";
        fail_errno = errno ? errno : ENOMEM;
        goto fail;
    }
    g_cxl_ce.engine_count = resource_count;
    for (size_t i = 0; i < resource_count; i++)
        g_cxl_ce.engines[i].bar_fd = -1;

    for (size_t i = 0; i < resource_count; i++) {
        cxl_ce_engine_state_t *engine = &g_cxl_ce.engines[i];
        struct stat st;
        size_t resource_len = 0;
        uint32_t chan_count = 0;
        uint8_t xfer_cap_log2 = 0;
        resource = resource_paths[i];

        if (cxl_copyengine_debug_enabled()) {
            fprintf(stderr,
                    "[libcxlrpc][CE] init resource_begin index=%zu path=%s\n",
                    i, resource);
        }

        engine->bar_fd = -1;
        if ((size_t)snprintf(engine->resource0_path, sizeof(engine->resource0_path),
                             "%s", resource) >= sizeof(engine->resource0_path)) {
            fail_stage = "copy_resource0_path";
            fail_errno = ENAMETOOLONG;
            goto fail;
        }

        if (cxl_enable_copyengine_pci_command(resource) != 0) {
            fail_stage = "enable_copyengine_pci_command";
            fail_errno = errno;
            goto fail;
        }
        if (cxl_read_copyengine_resource0_page_off(resource, page_size,
                                                   &bar_page_off) != 0) {
            fail_stage = "read_resource0_page_off";
            fail_errno = errno;
            goto fail;
        }

        if (cxl_copyengine_debug_enabled()) {
            fprintf(stderr,
                    "[libcxlrpc][CE] init resource_page_off path=%s page_off=%zu\n",
                    resource, bar_page_off);
        }

        engine->bar_fd = open(resource, O_RDWR | O_SYNC | O_CLOEXEC);
        if (engine->bar_fd < 0) {
            fail_stage = "open(resource0)";
            fail_errno = errno;
            goto fail;
        }

        if (fstat(engine->bar_fd, &st) != 0) {
            fail_stage = "fstat(resource0)";
            fail_errno = errno;
            goto fail;
        }
        resource_len = (st.st_size > 0) ? (size_t)st.st_size :
            (size_t)CXL_CE_MMIO_REQUIRED_BYTES;
        if (resource_len < (size_t)CXL_CE_MMIO_REQUIRED_BYTES) {
            fail_stage = "resource0 size too small";
            fail_errno = EINVAL;
            goto fail;
        }

        map_len = resource_len + bar_page_off;
        map_len = ((map_len + page_size - 1u) / page_size) * page_size;
        engine->bar_map_base = mmap(NULL, map_len,
                                    PROT_READ | PROT_WRITE, MAP_SHARED,
                                    engine->bar_fd, 0);
        if (engine->bar_map_base == MAP_FAILED) {
            engine->bar_map_base = NULL;
            fail_stage = "mmap(resource0)";
            fail_errno = errno;
            goto fail;
        }

        engine->bar0 = (volatile uint8_t *)engine->bar_map_base + bar_page_off;
        engine->bar_map_len = map_len;
        chan_count = (uint32_t)cxl_ce_mmio_read8(engine->bar0,
                                                 CXL_CE_GEN_CHANCOUNT);
        xfer_cap_log2 = cxl_ce_mmio_read8(engine->bar0, CXL_CE_GEN_XFERCAP);

        if (cxl_copyengine_debug_enabled()) {
            fprintf(stderr,
                    "[libcxlrpc][CE] init resource_mmio_ready path=%s map_len=%zu chan_count=%u xfer_cap_log2=%u\n",
                    resource,
                    map_len,
                    chan_count,
                    (unsigned)xfer_cap_log2);
        }
        if (chan_count == 0) {
            fail_stage = "read_channel_count";
            fail_errno = EINVAL;
            goto fail;
        }
        if (xfer_cap_log2 >= 63u) {
            fail_stage = "read_xfer_cap";
            fail_errno = EINVAL;
            goto fail;
        }

        size_t max_channels = 0;
        if (resource_len > (size_t)CXL_CE_CHAN_BASE) {
            max_channels = (resource_len - (size_t)CXL_CE_CHAN_BASE) /
                (size_t)CXL_CE_CHAN_STRIDE;
        }
        if (max_channels == 0) {
            fail_stage = "resource0 no channel window";
            fail_errno = EINVAL;
            goto fail;
        }
        if ((size_t)chan_count > max_channels)
            chan_count = (uint32_t)max_channels;

        engine->chan_count = chan_count;
        engine->xfer_cap_bytes = (size_t)(1ULL << xfer_cap_log2);
        total_channels += chan_count;
    }

    if (total_channels == 0) {
        fail_stage = "discover_channels";
        fail_errno = ENOENT;
        goto fail;
    }

    g_cxl_ce.channels = (cxl_ce_channel_state_t *)calloc(
        total_channels, sizeof(*g_cxl_ce.channels));
    if (!g_cxl_ce.channels) {
        fail_stage = "calloc(channels)";
        fail_errno = errno ? errno : ENOMEM;
        goto fail;
    }
    g_cxl_ce.channel_count = total_channels;

    size_t channel_index = 0;
    for (size_t i = 0; i < g_cxl_ce.engine_count; i++) {
        cxl_ce_engine_state_t *engine = &g_cxl_ce.engines[i];
        for (uint32_t chan_id = 0; chan_id < engine->chan_count; chan_id++) {
            cxl_ce_channel_state_t *chan = &g_cxl_ce.channels[channel_index];
            chan->engine = engine;
            chan->engine_index = i;
            chan->channel_index = channel_index;
            chan->chan_id = chan_id;
            chan->chan_mmio_base =
                CXL_CE_CHAN_BASE + chan_id * CXL_CE_CHAN_STRIDE;
            if (cxl_copyengine_channel_init(chan) != 0) {
                fail_stage = "init_channel_pools";
                fail_errno = errno ? errno : ENOMEM;
                goto fail;
            }
            channel_index++;
        }
    }

    g_cxl_ce.ready = 1;
    g_cxl_ce.attached_conns = 0;
    if (cxl_copyengine_debug_enabled()) {
        fprintf(stderr,
                "[libcxlrpc][CE] init ready engines=%zu channels=%zu\n",
                g_cxl_ce.engine_count,
                g_cxl_ce.channel_count);
    }
    cxl_free_copyengine_paths(resource_paths, resource_count);
    return 1;

fail:
    if (cxl_copyengine_error_log_permitted()) {
        if (fail_errno != 0) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine init failed"
                    " stage=%s resource=%s map_len=%zu bar_page_off=%zu"
                    " engines=%zu channels=%zu errno=%d (%s)\n",
                    fail_stage,
                    resource ? resource : "(null)",
                    map_len, bar_page_off,
                    g_cxl_ce.engine_count, total_channels,
                    fail_errno, strerror(fail_errno));
        } else {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine init failed"
                    " stage=%s resource=%s map_len=%zu bar_page_off=%zu"
                    " engines=%zu channels=%zu\n",
                    fail_stage,
                    resource ? resource : "(null)",
                    map_len, bar_page_off,
                    g_cxl_ce.engine_count, total_channels);
        }
    }
    cxl_free_copyengine_paths(resource_paths, resource_count);
    cxl_copyengine_global_reset();
    return 0;
}

int
cxl_copyengine_prepare(cxl_connection_t *conn)
{
    cxl_ce_channel_state_t *chan = NULL;

    if (!conn)
        return -1;

    if (!cxl_copyengine_init(conn))
        return -1;

    chan = cxl_copyengine_get_assigned_channel(conn);
    if (chan) {
        return chan->poisoned ? -1 : 0;
    }

    if (!conn->ce_lane_bind_valid) {
        if (cxl_copyengine_error_log_permitted()) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine lane is not bound before response setup\n");
        }
        return -1;
    }

    if (conn->ce_bind_lane_index_valid) {
        if (conn->ce_bind_lane_index >= g_cxl_ce.channel_count) {
            if (cxl_copyengine_error_log_permitted()) {
                fprintf(stderr,
                        "cxl_rpc: ERROR: requested CopyEngine lane index does not exist"
                        " lane=%zu available_channels=%zu\n",
                        conn->ce_bind_lane_index,
                        g_cxl_ce.channel_count);
            }
            return -1;
        }

        chan = &g_cxl_ce.channels[conn->ce_bind_lane_index];
        if (chan->poisoned) {
            if (cxl_copyengine_error_log_permitted()) {
                fprintf(stderr,
                        "cxl_rpc: ERROR: requested CopyEngine lane index is poisoned"
                        " lane=%zu engine=%zu channel=%u\n",
                        conn->ce_bind_lane_index,
                        chan->engine_index,
                        chan->chan_id);
            }
            return -1;
        }

        return cxl_copyengine_attach_channel(conn, chan);
    }

    for (size_t i = 0; i < g_cxl_ce.channel_count; i++) {
        chan = &g_cxl_ce.channels[i];
        if (chan->engine_index != conn->ce_bind_engine_index ||
            chan->chan_id != conn->ce_bind_channel_id) {
            continue;
        }
        if (chan->poisoned) {
            if (cxl_copyengine_error_log_permitted()) {
                fprintf(stderr,
                        "cxl_rpc: ERROR: requested CopyEngine lane is poisoned"
                        " engine=%zu channel=%u\n",
                        conn->ce_bind_engine_index,
                        conn->ce_bind_channel_id);
            }
            return -1;
        }

        return cxl_copyengine_attach_channel(conn, chan);
    }

    if (cxl_copyengine_error_log_permitted()) {
        fprintf(stderr,
                "cxl_rpc: ERROR: requested CopyEngine lane does not exist"
                " engine=%zu channel=%u available_channels=%zu\n",
                conn->ce_bind_engine_index,
                conn->ce_bind_channel_id,
                g_cxl_ce.channel_count);
    }
    return -1;
}

static void
cxl_copyengine_channel_reset_submit_state(cxl_ce_channel_state_t *chan,
                                          uint64_t ce_status)
{
    if (!chan)
        return;
    (void)ce_status;

    cxl_copyengine_channel_release_submissions(chan);
    chan->chain_started = 0;
    chan->last_desc = NULL;
}

static size_t
cxl_copyengine_drain_spins(void)
{
    return CXL_CE_FIXED_DRAIN_SPINS;
}

static int
cxl_copyengine_channel_reap_if_idle(cxl_connection_t *conn,
                                    cxl_ce_channel_state_t *chan)
{
    uint32_t ce_err = 0;
    uint64_t ce_status = 0;

    if (!conn || !chan || !chan->engine || !g_cxl_ce.ready ||
        !chan->engine->bar0 || !chan->chain_started) {
        return 0;
    }

    ce_err = cxl_ce_mmio_read32(chan->engine->bar0,
                                chan->chan_mmio_base + CXL_CE_CHAN_ERROR);
    if (ce_err != 0) {
        if (cxl_copyengine_error_log_permitted()) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine engine=%s channel=%u error while polling=0x%08x\n",
                    chan->engine->resource0_path, chan->chan_id, ce_err);
        }
        chan->poisoned = 1;
        return -1;
    }

    ce_status = cxl_ce_mmio_read64(chan->engine->bar0,
                                   chan->chan_mmio_base + CXL_CE_CHAN_STATUS);
    if ((ce_status & 0x1ULL) == 0)
        return 0;

    cxl_copyengine_channel_reset_submit_state(chan, ce_status);
    return 1;
}

static int
cxl_copyengine_channel_wait_idle(cxl_connection_t *conn,
                                 cxl_ce_channel_state_t *chan)
{
    if (!conn || !chan || !chan->engine || !g_cxl_ce.ready ||
        !chan->engine->bar0 || !chan->chain_started)
        return 0;

    size_t spins = cxl_copyengine_drain_spins();
    for (size_t i = 0; i < spins; i++) {
        uint32_t ce_err = cxl_ce_mmio_read32(chan->engine->bar0,
                                             chan->chan_mmio_base +
                                             CXL_CE_CHAN_ERROR);
        if (ce_err != 0) {
            if (cxl_copyengine_error_log_permitted()) {
                fprintf(stderr,
                        "cxl_rpc: ERROR: CopyEngine engine=%s channel=%u error while draining=0x%08x\n",
                        chan->engine->resource0_path, chan->chan_id, ce_err);
            }
            chan->poisoned = 1;
            return -1;
        }

        uint64_t ce_status = cxl_ce_mmio_read64(chan->engine->bar0,
                                                chan->chan_mmio_base +
                                                CXL_CE_CHAN_STATUS);
        if ((ce_status & 0x1ULL) != 0) {
            cxl_copyengine_channel_reset_submit_state(chan, ce_status);
            return 0;
        }
        __asm__ __volatile__("pause" ::: "memory");
    }

    fprintf(stderr,
            "cxl_rpc: ERROR: timed out waiting for CopyEngine idle"
            " engine=%s channel=%u chain_started=%d\n",
            chan->engine->resource0_path, chan->chan_id,
            chan->chain_started);
    chan->poisoned = 1;
    return -1;
}

int
cxl_copyengine_response_publish_pending(cxl_connection_t *conn)
{
    cxl_ce_channel_state_t *chan = NULL;

    if (!conn)
        return -1;
    if (!conn->ce_lane_assigned)
        return 0;

    chan = cxl_copyengine_get_assigned_channel(conn);
    if (!chan || chan->poisoned)
        return -1;

    if (cxl_copyengine_channel_reap_if_idle(conn, chan) < 0)
        return -1;

    return chan->chain_started ? 1 : 0;
}

static int
cxl_copyengine_alloc_locked_aligned(void **ptr_out, size_t align, size_t bytes)
{
    int memalign_rc = 0;

    if (!ptr_out || align == 0 || bytes == 0) {
        errno = EINVAL;
        return -1;
    }

    *ptr_out = NULL;
    memalign_rc = posix_memalign(ptr_out, align, bytes);
    if (memalign_rc != 0) {
        errno = memalign_rc;
        return -1;
    }

    memset(*ptr_out, 0, bytes);
    (void)mlock(*ptr_out, bytes);
    return 0;
}

static int
cxl_copyengine_page_size(size_t *page_size_out)
{
    long ps = 0;

    if (!page_size_out)
        return -1;

    ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0)
        return -1;

    *page_size_out = (size_t)ps;
    return 0;
}

static int
cxl_copyengine_prepare_flag_source(cxl_ce_submission_t *sub)
{
    if (!sub)
        return -1;

    if (!sub->flag_src) {
        if (cxl_copyengine_alloc_locked_aligned((void **)&sub->flag_src,
                                                64u,
                                                (size_t)CXL_CE_FLAG_SRC_BYTES) != 0) {
            return -1;
        }
    }

    if (sub->flag_src_phys == 0 &&
        cxl_virt_to_phys(sub->flag_src, &sub->flag_src_phys) != 0) {
        return -1;
    }

    return 0;
}

static int
cxl_copyengine_prepare_entry_source(cxl_ce_submission_t *sub,
                                    size_t entry_bytes)
{
    uint8_t *entry_src = NULL;
    uint64_t *page_phys = NULL;
    uintptr_t page_base = 0;
    size_t page_size = 0;
    size_t page_count = 0;
    size_t alloc_bytes = 0;

    if (!sub || entry_bytes == 0)
        return -1;
    if (sub->entry_src && sub->entry_capacity >= entry_bytes)
        return 0;

    if (cxl_copyengine_page_size(&page_size) != 0)
        return -1;
    if (entry_bytes > SIZE_MAX - (page_size - 1u))
        return -1;
    alloc_bytes = ((entry_bytes + page_size - 1u) / page_size) * page_size;

    if (cxl_copyengine_alloc_locked_aligned((void **)&entry_src,
                                            page_size,
                                            alloc_bytes) != 0) {
        return -1;
    }

    if (cxl_prepare_virt_page_phys_cache(entry_src, alloc_bytes,
                                         &page_base, &page_size,
                                         &page_count, &page_phys) != 0) {
        free(entry_src);
        return -1;
    }

    for (size_t i = 0; i < page_count; ++i) {
        if (cxl_resolve_virt_page_phys(page_base + i * page_size,
                                       page_size,
                                       0,
                                       &page_phys[i]) != 0) {
            free(page_phys);
            free(entry_src);
            return -1;
        }
    }

    free(sub->entry_src);
    free(sub->entry_page_phys);
    sub->entry_src = entry_src;
    sub->entry_capacity = alloc_bytes;
    sub->entry_page_base = page_base;
    sub->entry_page_size = page_size;
    sub->entry_page_count = page_count;
    sub->entry_page_phys = page_phys;
    return 0;
}

static int
cxl_copyengine_prepare_desc_pool(cxl_ce_submission_t *sub,
                                 size_t desc_capacity)
{
    cxl_ce_desc_t *desc_pool = NULL;
    uint64_t *desc_phys = NULL;
    size_t page_size = 0;
    size_t alloc_bytes = 0;
    size_t alloc_desc_capacity = 0;

    if (!sub || desc_capacity == 0)
        return -1;
    if (sub->desc_pool && sub->desc_capacity >= desc_capacity)
        return 0;

    if (desc_capacity > SIZE_MAX / sizeof(cxl_ce_desc_t))
        return -1;
    if (cxl_copyengine_page_size(&page_size) != 0)
        return -1;

    alloc_bytes = desc_capacity * sizeof(cxl_ce_desc_t);
    if (alloc_bytes > SIZE_MAX - (page_size - 1u))
        return -1;
    alloc_bytes = ((alloc_bytes + page_size - 1u) / page_size) * page_size;
    alloc_desc_capacity = alloc_bytes / sizeof(cxl_ce_desc_t);

    if (cxl_copyengine_alloc_locked_aligned((void **)&desc_pool,
                                            page_size,
                                            alloc_bytes) != 0) {
        return -1;
    }

    desc_phys = (uint64_t *)malloc(alloc_desc_capacity * sizeof(*desc_phys));
    if (!desc_phys) {
        free(desc_pool);
        return -1;
    }

    for (size_t i = 0; i < alloc_desc_capacity; ++i) {
        if (cxl_virt_to_phys(desc_pool + i, &desc_phys[i]) != 0) {
            free(desc_phys);
            free(desc_pool);
            return -1;
        }
    }

    free(sub->desc_pool);
    free(sub->desc_phys);
    sub->desc_pool = desc_pool;
    sub->desc_phys = desc_phys;
    sub->desc_capacity = alloc_desc_capacity;
    sub->first_desc_phys = desc_phys[0];
    return 0;
}

static int
cxl_copyengine_alloc_submission(size_t entry_bytes,
                                size_t desc_count,
                                cxl_ce_submission_t **sub_out)
{
    cxl_ce_submission_t *sub = NULL;

    if (!sub_out) {
        errno = EINVAL;
        return -1;
    }

    *sub_out = NULL;
    sub = (cxl_ce_submission_t *)calloc(1, sizeof(*sub));
    if (!sub)
        return -1;

    if ((entry_bytes > 0 &&
         cxl_copyengine_alloc_locked_aligned((void **)&sub->entry_src,
                                             64u, entry_bytes) != 0) ||
        cxl_copyengine_alloc_locked_aligned((void **)&sub->flag_src,
                                            64u,
                                            (size_t)CXL_CE_FLAG_SRC_BYTES) != 0) {
        cxl_copyengine_submission_free(sub);
        return -1;
    }

    if (desc_count > 0 &&
        cxl_copyengine_alloc_locked_aligned((void **)&sub->desc_pool,
                                            64u,
                                            desc_count * sizeof(cxl_ce_desc_t)) != 0) {
        cxl_copyengine_submission_free(sub);
        return -1;
    }

    *sub_out = sub;
    return 0;
}

static int
cxl_copyengine_get_flag_submission(cxl_ce_channel_state_t *chan,
                                   cxl_ce_submission_t **sub_out)
{
    cxl_ce_submission_t *sub = NULL;

    if (!chan || !sub_out)
        return -1;

    *sub_out = NULL;
    if (chan->flag_free_head) {
        sub = chan->flag_free_head;
        chan->flag_free_head = sub->next;
        sub->next = NULL;
        *sub_out = sub;
        return 0;
    }

    if (cxl_copyengine_alloc_submission(0, 1, &sub) != 0)
        return -1;

    if (cxl_virt_to_phys(sub->flag_src, &sub->flag_src_phys) != 0 ||
        cxl_virt_to_phys(sub->desc_pool, &sub->first_desc_phys) != 0) {
        cxl_copyengine_submission_free(sub);
        return -1;
    }

    sub->reusable_flag_only = 1u;
    sub->tail_desc = &sub->desc_pool[0];
    *sub_out = sub;
    return 0;
}

static int
cxl_copyengine_get_response_submission(cxl_ce_channel_state_t *chan,
                                       size_t entry_bytes,
                                       size_t desc_capacity,
                                       cxl_ce_submission_t **sub_out)
{
    cxl_ce_submission_t *sub = NULL;

    if (!chan || !sub_out || entry_bytes == 0 || desc_capacity == 0)
        return -1;

    *sub_out = NULL;
    if (chan->response_free_head) {
        sub = chan->response_free_head;
        chan->response_free_head = sub->next;
        sub->next = NULL;
    } else {
        sub = (cxl_ce_submission_t *)calloc(1, sizeof(*sub));
        if (!sub)
            return -1;
        sub->reusable_response = 1u;
    }

    if (cxl_copyengine_prepare_flag_source(sub) != 0 ||
        cxl_copyengine_prepare_entry_source(sub, entry_bytes) != 0 ||
        cxl_copyengine_prepare_desc_pool(sub, desc_capacity) != 0) {
        cxl_copyengine_channel_recycle_submission(chan, sub);
        return -1;
    }

    *sub_out = sub;
    return 0;
}

static int
cxl_copyengine_enqueue_submission(cxl_ce_channel_state_t *chan,
                                  cxl_ce_submission_t *sub)
{
    volatile uint8_t *bar0 = NULL;
    cxl_ce_desc_t *prev_tail_desc = NULL;

    if (!chan || !sub || !chan->engine || !chan->engine->bar0)
        return -1;

    bar0 = chan->engine->bar0;
    prev_tail_desc = chan->last_desc;
    sub->next = NULL;
    if (chan->chain_started && !prev_tail_desc) {
        if (cxl_copyengine_error_log_permitted()) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine append path missing tail"
                    " engine=%s channel=%u\n",
                    chan->engine->resource0_path, chan->chan_id);
        }
        chan->poisoned = 1;
        return -1;
    }

    if (chan->submit_tail)
        chan->submit_tail->next = sub;
    else
        chan->submit_head = sub;
    chan->submit_tail = sub;
    chan->last_desc = sub->tail_desc;

    if (chan->chain_started) {
        prev_tail_desc->next = sub->first_desc_phys;
        __asm__ __volatile__("sfence" ::: "memory");
        cxl_ce_mmio_write8(bar0,
                           chan->chan_mmio_base + CXL_CE_CHAN_COMMAND,
                           CXL_CE_CMD_APPEND_DMA);
    } else {
        cxl_ce_mmio_write64(bar0,
                            chan->chan_mmio_base + CXL_CE_CHAN_CHAINADDR,
                            sub->first_desc_phys);
        cxl_ce_mmio_write8(bar0,
                           chan->chan_mmio_base + CXL_CE_CHAN_COMMAND,
                           CXL_CE_CMD_START_DMA);
    }
    chan->chain_started = 1;
    return 0;
}

int
cxl_copyengine_submit_response_async(cxl_connection_t *conn,
                                     uint16_t rpc_id,
                                     const void *data,
                                     size_t len,
                                     uint64_t producer_cursor,
                                     size_t dst_resp_offset)
{
    cxl_ce_channel_state_t *chan = NULL;
    cxl_ce_submission_t *sub = NULL;
    uint64_t dst_flag_phys = 0;
    uint64_t logical_resp_addr = 0;
    uint64_t logical_copy_addr = 0;
    uint64_t src_page_phys = 0;
    size_t resp_publish_bytes = 0;
    size_t copy_remaining = 0;
    uintptr_t copy_src_virt = 0;
    size_t desc_idx = 0;
    size_t desc_count = 0;
    size_t desc_capacity = 0;
    size_t src_pages_touched = 0;
    size_t src_page_size = 0;
    size_t xfer_cap_bytes = 0;
    size_t xfer_desc_limit = 0;
    size_t xfer_chunks = 0;

    chan = cxl_copyengine_get_assigned_channel(conn);
    if (!chan || chan->poisoned || !chan->engine || !chan->engine->bar0)
        return -1;

    if (cxl_copyengine_channel_reap_if_idle(conn, chan) < 0)
        return -1;

    if (!conn->ce_peer_resp_page_phys || conn->ce_peer_resp_page_count == 0 ||
        conn->ce_peer_resp_page_size == 0 ||
        !conn->ce_peer_flag_phys_valid || conn->ce_peer_flag_phys == 0 ||
        dst_resp_offset > conn->peer_response_data_size) {
        if (cxl_copyengine_error_log_permitted()) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine response mapping state invalid\n");
        }
        return -1;
    }

    resp_publish_bytes = CXL_RESP_HEADER_LEN + len;
    if (resp_publish_bytes > conn->peer_response_data_size - dst_resp_offset)
        return -1;

    dst_flag_phys = conn->ce_peer_flag_phys;
    logical_resp_addr = conn->peer_response_data_addr + dst_resp_offset;
    if (logical_resp_addr < conn->ce_peer_resp_logical_page_base) {
        if (cxl_copyengine_error_log_permitted()) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine logical response address below page base\n");
        }
        return -1;
    }
    size_t page_delta =
        (size_t)(logical_resp_addr - conn->ce_peer_resp_logical_page_base);
    size_t page_index = page_delta / conn->ce_peer_resp_page_size;
    size_t page_off = page_delta % conn->ce_peer_resp_page_size;
    if (page_index >= conn->ce_peer_resp_page_count ||
        (page_off + CXL_RESP_HEADER_LEN) > conn->ce_peer_resp_page_size) {
        if (cxl_copyengine_error_log_permitted()) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine response header translation failed offset=%zu page_index=%zu page_count=%zu\n",
                    dst_resp_offset, page_index, conn->ce_peer_resp_page_count);
        }
        return -1;
    }

    const uint64_t header = cxl_pack_response_header((uint32_t)len, rpc_id);

    size_t dst_pages_touched =
        (page_off + resp_publish_bytes + conn->ce_peer_resp_page_size - 1u) /
        conn->ce_peer_resp_page_size;
    if (dst_pages_touched == 0)
        dst_pages_touched = 1;
    if (cxl_copyengine_page_size(&src_page_size) != 0)
        return -1;
    xfer_cap_bytes = chan->engine->xfer_cap_bytes;
    if (xfer_cap_bytes == 0)
        return -1;
    xfer_desc_limit = xfer_cap_bytes;
    if (xfer_desc_limit > (size_t)UINT32_MAX)
        xfer_desc_limit = (size_t)UINT32_MAX;
    src_pages_touched =
        (resp_publish_bytes + src_page_size - 1u) / src_page_size;
    if (src_pages_touched == 0)
        src_pages_touched = 1;
    xfer_chunks = (resp_publish_bytes + xfer_desc_limit - 1u) / xfer_desc_limit;
    if (xfer_chunks == 0)
        xfer_chunks = 1;
    desc_capacity = src_pages_touched + dst_pages_touched + xfer_chunks + 1u;

    /*
     * One response submission owns its staging entry, flag source, and
     * descriptor pool across reuses. After the lane warms up, large response
     * publishes no longer allocate or resolve local PFNs on the hot path.
     */
    if (cxl_copyengine_get_response_submission(chan, resp_publish_bytes,
                                               desc_capacity,
                                               &sub) != 0) {
        return -1;
    }

    memcpy(sub->entry_src, &header, sizeof(header));
    if (len > 0)
        memcpy(sub->entry_src + sizeof(header), data, len);
    memcpy(sub->flag_src, &producer_cursor, CXL_FLAG_PUBLISH_LEN);

    if (cxl_copyengine_debug_enabled()) {
        const size_t first_dump = resp_publish_bytes < 32u ?
            resp_publish_bytes : 32u;
        size_t tail_off = 0;
        size_t tail_dump = 0;

        if (resp_publish_bytes > 32u) {
            tail_dump = 32u;
            tail_off = resp_publish_bytes - tail_dump;
        }

        cxl_copyengine_debug_dump_bytes("entry_src_first32",
                                        sub->entry_src,
                                        first_dump);
        if (resp_publish_bytes > 64u) {
            cxl_copyengine_debug_dump_bytes("entry_src_line64_32",
                                            sub->entry_src + 64u,
                                            32u);
        }
        if (tail_dump > 0) {
            cxl_copyengine_debug_dump_bytes("entry_src_tail32",
                                            sub->entry_src + tail_off,
                                            tail_dump);
        }
    }

    logical_copy_addr = logical_resp_addr;
    copy_remaining = resp_publish_bytes;
    copy_src_virt = (uintptr_t)sub->entry_src;
    while (copy_remaining > 0) {
        uint64_t dst_copy_page_phys = 0;
        uint64_t chunk_src_phys = 0;
        uint64_t chunk_dst_phys = 0;
        size_t src_page_delta = 0;
        size_t src_page_index = 0;
        size_t src_page_off = 0;
        size_t dst_page_delta = 0;
        size_t dst_page_index = 0;
        size_t dst_page_off = 0;
        size_t chunk = copy_remaining;
        size_t src_page_rem = 0;
        size_t dst_page_rem = 0;

        src_page_delta = (size_t)((uint64_t)copy_src_virt -
                                  (uint64_t)sub->entry_page_base);
        src_page_index = src_page_delta / sub->entry_page_size;
        src_page_off = src_page_delta % sub->entry_page_size;
        dst_page_delta =
            (size_t)(logical_copy_addr - conn->ce_peer_resp_logical_page_base);
        dst_page_index = dst_page_delta / conn->ce_peer_resp_page_size;
        dst_page_off = dst_page_delta % conn->ce_peer_resp_page_size;

        if (desc_idx >= desc_capacity ||
            src_page_index >= sub->entry_page_count ||
            dst_page_index >= conn->ce_peer_resp_page_count) {
            cxl_copyengine_channel_recycle_submission(chan, sub);
            return -1;
        }

        src_page_phys = sub->entry_page_phys[src_page_index];
        if (src_page_phys == UINT64_MAX ||
            cxl_copyengine_get_peer_response_page_phys(conn, dst_page_index,
                                                       &dst_copy_page_phys) != 0) {
            cxl_copyengine_channel_recycle_submission(chan, sub);
            return -1;
        }

        src_page_rem = sub->entry_page_size - src_page_off;
        dst_page_rem = conn->ce_peer_resp_page_size - dst_page_off;
        if (chunk > src_page_rem)
            chunk = src_page_rem;
        if (chunk > dst_page_rem)
            chunk = dst_page_rem;
        if (chunk > xfer_desc_limit)
            chunk = xfer_desc_limit;

        chunk_src_phys = src_page_phys + (uint64_t)src_page_off;
        chunk_dst_phys = dst_copy_page_phys + (uint64_t)dst_page_off;

        if (desc_idx > 0) {
            cxl_ce_desc_t *prev_desc = &sub->desc_pool[desc_idx - 1u];
            size_t merged_len = (size_t)prev_desc->len + chunk;
            if (prev_desc->src + (uint64_t)prev_desc->len == chunk_src_phys &&
                prev_desc->dest + (uint64_t)prev_desc->len == chunk_dst_phys &&
                merged_len <= xfer_desc_limit) {
                if (chunk > (size_t)UINT32_MAX - (size_t)prev_desc->len) {
                    cxl_copyengine_channel_recycle_submission(chan, sub);
                    return -1;
                }
                prev_desc->len += (uint32_t)chunk;
            } else {
                sub->desc_pool[desc_idx].len = (uint32_t)chunk;
                sub->desc_pool[desc_idx].command = 0;
                sub->desc_pool[desc_idx].src = chunk_src_phys;
                sub->desc_pool[desc_idx].dest = chunk_dst_phys;
                desc_idx++;
            }
        } else {
            sub->desc_pool[desc_idx].len = (uint32_t)chunk;
            sub->desc_pool[desc_idx].command = 0;
            sub->desc_pool[desc_idx].src = chunk_src_phys;
            sub->desc_pool[desc_idx].dest = chunk_dst_phys;
            desc_idx++;
        }

        copy_src_virt += chunk;
        logical_copy_addr += (uint64_t)chunk;
        copy_remaining -= chunk;
    }

    if (desc_idx >= desc_capacity) {
        cxl_copyengine_channel_recycle_submission(chan, sub);
        return -1;
    }

    sub->desc_pool[desc_idx].len = (uint32_t)CXL_FLAG_PUBLISH_LEN;
    sub->desc_pool[desc_idx].command = 0;
    sub->desc_pool[desc_idx].src = sub->flag_src_phys;
    sub->desc_pool[desc_idx].dest = dst_flag_phys;
    desc_count = desc_idx + 1u;

    for (size_t i = 0; i < desc_count; ++i) {
        if (i == 0)
            sub->first_desc_phys = sub->desc_phys[i];
        if (i > 0)
            sub->desc_pool[i - 1u].next = sub->desc_phys[i];
    }
    sub->desc_pool[desc_count - 1u].next = 0;
    sub->tail_desc = &sub->desc_pool[desc_count - 1u];

    if (cxl_copyengine_debug_enabled()) {
        fprintf(stderr,
                "[libcxlrpc][CE] response_submit engine=%zu channel=%u rpc_id=%u payload_len=%zu publish_bytes=%zu dst_resp_offset=%zu producer_cursor=%llu desc_count=%zu xfer_cap=%zu\n",
                chan->engine_index,
                chan->chan_id,
                (unsigned)rpc_id,
                len,
                resp_publish_bytes,
                dst_resp_offset,
                (unsigned long long)producer_cursor,
                desc_count,
                xfer_cap_bytes);
        for (size_t i = 0; i < desc_count; ++i) {
            fprintf(stderr,
                    "[libcxlrpc][CE] desc[%zu] len=%u src=%#llx dst=%#llx next=%#llx\n",
                    i,
                    (unsigned)sub->desc_pool[i].len,
                    (unsigned long long)sub->desc_pool[i].src,
                    (unsigned long long)sub->desc_pool[i].dest,
                    (unsigned long long)sub->desc_pool[i].next);
        }
    }

    /*
     * CopyEngine fetches descriptors and DMA-reads staging pages from memory
     * backing, so source-side cachelines must be published before the channel
     * start/append MMIO write.
     */
    cxl_copyengine_flush_cpu_range((volatile uint8_t *)sub->entry_src,
                                   resp_publish_bytes);
    cxl_copyengine_flush_cpu_range((volatile uint8_t *)sub->flag_src,
                                   CXL_FLAG_PUBLISH_LEN);
    cxl_copyengine_flush_cpu_range((volatile uint8_t *)sub->desc_pool,
                                   desc_count * sizeof(cxl_ce_desc_t));

    if (cxl_copyengine_enqueue_submission(chan, sub) != 0) {
        cxl_copyengine_channel_recycle_submission(chan, sub);
        return -1;
    }

    return 0;
}

int
cxl_copyengine_submit_flag_async(cxl_connection_t *conn,
                                 uint64_t producer_cursor)
{
    cxl_ce_channel_state_t *chan = NULL;
    cxl_ce_submission_t *sub = NULL;

    chan = cxl_copyengine_get_assigned_channel(conn);
    if (!chan || chan->poisoned || !chan->engine || !chan->engine->bar0)
        return -1;

    if (cxl_copyengine_channel_reap_if_idle(conn, chan) < 0)
        return -1;
    if (!conn->ce_peer_flag_phys_valid || conn->ce_peer_flag_phys == 0)
        return -1;

    if (cxl_copyengine_get_flag_submission(chan, &sub) != 0)
        return -1;

    memcpy(sub->flag_src, &producer_cursor, CXL_FLAG_PUBLISH_LEN);
    sub->desc_pool[0].len = (uint32_t)CXL_FLAG_PUBLISH_LEN;
    sub->desc_pool[0].command = 0;
    sub->desc_pool[0].src = sub->flag_src_phys;
    sub->desc_pool[0].dest = conn->ce_peer_flag_phys;
    sub->desc_pool[0].next = 0;
    sub->tail_desc = &sub->desc_pool[0];

    cxl_copyengine_flush_cpu_range((volatile uint8_t *)sub->flag_src,
                                   CXL_FLAG_PUBLISH_LEN);
    cxl_copyengine_flush_cpu_range((volatile uint8_t *)sub->desc_pool,
                                   sizeof(cxl_ce_desc_t));

    if (cxl_copyengine_enqueue_submission(chan, sub) != 0) {
        cxl_copyengine_channel_recycle_submission(chan, sub);
        return -1;
    }

    return 0;
}
