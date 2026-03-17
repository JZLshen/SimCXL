#include "cxl_rpc_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* CopyEngine (IOAT-like) MMIO register constants. */
#define CXL_CE_GEN_CHANCOUNT  0x00u
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
 * Slot sizing for async response offload:
 * - one response payload slot per request (4 KiB max transfer)
 * - one 64B flag slot per request
 * - up to three descriptors per request (resp split across two pages + flag)
 *
 * Slot pools are sized once per dedicated lane from the connection-visible
 * outstanding-response capacity, but the preallocated pool is intentionally
 * capped to keep setup work bounded. The fast path still does not do
 * per-response reclaim; descriptors/sources are reused only after the channel
 * drains completely.
 */
#define CXL_CE_RESP_SLOT_BYTES CXL_RESP_SLOT_BYTES
#define CXL_CE_FLAG_SLOT_BYTES CXL_FLAG_CACHELINE_BYTES
#define CXL_CE_MAX_RESP_SEGMENTS 2u
#define CXL_CE_DESC_PER_REQ    (CXL_CE_MAX_RESP_SEGMENTS + 1u)
#define CXL_CE_MAX_PREALLOC_SLOTS 64u
#define CXL_CE_FIXED_DRAIN_SPINS (1u << 22)
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
    char resource0_path[PATH_MAX];
} cxl_ce_engine_state_t;

typedef struct cxl_ce_channel_state {
    cxl_ce_engine_state_t *engine;
    cxl_connection_t *owner_conn;
    size_t engine_index;
    size_t channel_index;
    uint32_t chan_id;
    uint32_t chan_mmio_base;
    int chain_started;
    int poisoned;
    size_t slots;
    size_t next_slot;
    uint8_t *resp_src_pool;
    uint8_t *flag_src_pool;
    cxl_ce_desc_t *desc_pool;
    uint64_t *resp_src_slot_phys;
    uint64_t *flag_src_slot_phys;
    uint64_t *desc_phys;
    cxl_ce_desc_t *last_desc;
} cxl_ce_channel_state_t;

typedef struct {
    uint64_t dst_phys;
    uint32_t len;
    uint32_t src_offset;
} cxl_ce_dma_segment_t;

static cxl_ce_shared_state_t g_cxl_ce = {
};

static inline int
cxl_ce_env_flag_enabled(const char *name)
{
    const char *env = getenv(name);
    return (env && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
}

static int
cxl_ce_debug_enabled(void)
{
    static int initialized = 0;
    static int enabled = 0;

    if (!initialized) {
        const char *lib_debug = getenv("CXL_RPC_LIB_DEBUG");

        if (lib_debug) {
            enabled = cxl_ce_env_flag_enabled("CXL_RPC_LIB_DEBUG");
        } else {
            enabled = cxl_ce_env_flag_enabled("CXL_RPC_PROGRESS_DEBUG");
        }
        initialized = 1;
    }

    return enabled;
}

static void
cxl_ce_debugf(const char *fmt, ...)
{
    va_list ap;

    if (!cxl_ce_debug_enabled())
        return;

    va_start(ap, fmt);
    fprintf(stderr, "[libcxlrpc][CE] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
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
cxl_ce_flush_cacheline(const void *addr)
{
    __asm__ __volatile__(
        "clflushopt (%0)"
        :
        : "r"(addr)
        : "memory");
}

static inline void
cxl_ce_flush_range(const void *addr, size_t len)
{
    uintptr_t line = 0;
    uintptr_t line_end = 0;

    if (!addr || len == 0)
        return;

    line = ((uintptr_t)addr) & ~((uintptr_t)63);
    line_end = (((uintptr_t)addr + len) + 63u) & ~((uintptr_t)63);
    for (; line < line_end; line += 64u)
        cxl_ce_flush_cacheline((const void *)line);
}

static int cxl_copyengine_wait_idle(cxl_connection_t *conn);
static int cxl_copyengine_channel_wait_idle(cxl_connection_t *conn,
                                            cxl_ce_channel_state_t *chan);

static cxl_ce_channel_state_t *
cxl_copyengine_get_assigned_channel(const cxl_connection_t *conn)
{
    if (!conn || !g_cxl_ce.ready || !conn->ce_lane_assigned)
        return NULL;
    if (conn->ce_channel_index >= g_cxl_ce.channel_count)
        return NULL;

    cxl_ce_channel_state_t *chan = &g_cxl_ce.channels[conn->ce_channel_index];
    if (chan->owner_conn != conn)
        return NULL;
    return chan;
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

    if (!chan->resp_src_slot_phys || !chan->flag_src_slot_phys ||
        !chan->desc_phys || !chan->resp_src_pool || !chan->flag_src_pool ||
        !chan->desc_pool || chan->slots == 0) {
        if (!g_cxl_ce.warned) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine submit invariants missing"
                    " engine=%s channel=%u\n",
                    chan->engine->resource0_path, chan->chan_id);
            g_cxl_ce.warned = 1;
        }
        return -1;
    }

    if (!conn->ce_peer_resp_page_phys || conn->ce_peer_resp_page_count == 0 ||
        conn->ce_peer_resp_page_size == 0 ||
        conn->ce_peer_resp_virt_page_base == 0) {
        if (!g_cxl_ce.warned) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine peer response map is not ready\n");
            g_cxl_ce.warned = 1;
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
cxl_copyengine_channel_release_buffers(cxl_ce_channel_state_t *chan)
{
    if (!chan)
        return;

    free(chan->resp_src_pool);
    free(chan->flag_src_pool);
    free(chan->desc_pool);
    free(chan->resp_src_slot_phys);
    free(chan->flag_src_slot_phys);
    free(chan->desc_phys);

    chan->resp_src_pool = NULL;
    chan->flag_src_pool = NULL;
    chan->desc_pool = NULL;
    chan->resp_src_slot_phys = NULL;
    chan->flag_src_slot_phys = NULL;
    chan->desc_phys = NULL;
    chan->slots = 0;
    chan->next_slot = 0;
    chan->chain_started = 0;
    chan->last_desc = NULL;
}

static void
cxl_copyengine_channel_prime_slot_templates(cxl_ce_channel_state_t *chan)
{
    if (!chan || !chan->resp_src_slot_phys || !chan->flag_src_slot_phys ||
        !chan->desc_phys || !chan->desc_pool) {
        return;
    }

    for (size_t slot = 0; slot < chan->slots; slot++) {
        const size_t desc_base_idx = slot * CXL_CE_DESC_PER_REQ;
        cxl_ce_desc_t *resp_desc0 = chan->desc_pool + desc_base_idx;
        cxl_ce_desc_t *resp_desc1 = resp_desc0 + 1;
        cxl_ce_desc_t *flag_desc = resp_desc0 + 2;
        const uint64_t src_resp_phys = chan->resp_src_slot_phys[slot];
        const uint64_t src_flag_phys = chan->flag_src_slot_phys[slot];
        const uint64_t flag_desc_phys = chan->desc_phys[desc_base_idx + 2];

        resp_desc0->command = 0;
        resp_desc0->src = src_resp_phys;
        resp_desc0->next = flag_desc_phys;

        resp_desc1->command = 0;
        resp_desc1->next = flag_desc_phys;

        flag_desc->len = (uint32_t)CXL_CE_FLAG_SLOT_BYTES;
        flag_desc->command = 0;
        flag_desc->src = src_flag_phys;
        flag_desc->next = 0;
    }
}

static void
cxl_copyengine_sync_conn_state(cxl_connection_t *conn)
{
    cxl_ce_channel_state_t *chan = NULL;

    if (!conn)
        return;

    chan = cxl_copyengine_get_assigned_channel(conn);
    conn->ce_init_attempted = g_cxl_ce.init_attempted;
    conn->ce_ready = (g_cxl_ce.ready && chan) ? 1 : 0;
    conn->ce_warned = g_cxl_ce.warned;
    conn->ce_chain_started = chan ? chan->chain_started : 0;
    conn->ce_slots = chan ? chan->slots : 0;
    conn->ce_next_slot = chan ? chan->next_slot : 0;

    if (chan && chan->engine) {
        conn->ce_bar_fd = chan->engine->bar_fd;
        conn->ce_bar0 = chan->engine->bar0;
        conn->ce_bar_map_base = chan->engine->bar_map_base;
        conn->ce_bar_map_len = chan->engine->bar_map_len;
        conn->ce_engine_index = chan->engine_index;
        conn->ce_channel_index = chan->channel_index;
        conn->ce_hw_channel_id = chan->chan_id;
        conn->ce_resp_src_pool = chan->resp_src_pool;
        conn->ce_flag_src_pool = chan->flag_src_pool;
        conn->ce_desc_pool = chan->desc_pool;
        conn->ce_last_desc = chan->last_desc;
    } else {
        conn->ce_bar_fd = -1;
        conn->ce_bar0 = NULL;
        conn->ce_bar_map_base = NULL;
        conn->ce_bar_map_len = 0;
        conn->ce_engine_index = 0;
        conn->ce_channel_index = 0;
        conn->ce_hw_channel_id = 0;
        conn->ce_resp_src_pool = NULL;
        conn->ce_flag_src_pool = NULL;
        conn->ce_desc_pool = NULL;
        conn->ce_last_desc = NULL;
    }
}

static void
cxl_copyengine_clear_conn_state(cxl_connection_t *conn)
{
    if (!conn)
        return;

    conn->ce_init_attempted = 0;
    conn->ce_ready = 0;
    conn->ce_warned = 0;
    conn->ce_chain_started = 0;
    conn->ce_lane_assigned = 0;
    conn->resp_tx_ready = 0;
    conn->ce_bar_fd = -1;
    conn->ce_bar0 = NULL;
    conn->ce_bar_map_base = NULL;
    conn->ce_bar_map_len = 0;
    conn->ce_engine_index = 0;
    conn->ce_channel_index = 0;
    conn->ce_hw_channel_id = 0;
    conn->ce_slots = 0;
    conn->ce_next_slot = 0;
    conn->ce_resp_src_pool = NULL;
    conn->ce_flag_src_pool = NULL;
    conn->ce_desc_pool = NULL;
    conn->ce_last_desc = NULL;
    cxl_copyengine_clear_peer_phys_cache(conn);
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
                           uint64_t *page_phys_out)
{
    if (!page_phys_out || page_size == 0)
        return -1;

    volatile uint8_t page_touch =
        *(const volatile uint8_t *)(uintptr_t)page_virt;
    (void)page_touch;
    (void)mlock((const void *)page_virt, page_size);

    uint64_t phys = 0;
    if (cxl_virt_to_phys((const void *)page_virt, &phys) != 0)
        return -1;

    *page_phys_out = phys & ~((uint64_t)page_size - 1u);

    /*
     * Faulting in one byte resolves the page frame, but the zero-fill/page-fault
     * path may leave other cachelines from the same page resident in CPU caches
     * before CopyEngine later DMA-writes them. Flush the whole page once at the
     * moment the PFN is first resolved so subsequent DMA targets within that
     * page do not inherit stale zero lines.
     */
    cxl_ce_flush_range((const void *)page_virt, page_size);
    __asm__ __volatile__("sfence" ::: "memory");
    return 0;
}

static int
cxl_copyengine_translate_peer_response_logical(cxl_connection_t *conn,
                                               uint64_t logical_addr,
                                               uint64_t *phys_out)
{
    if (!conn || !phys_out || !conn->ce_peer_resp_page_phys ||
        conn->ce_peer_resp_page_count == 0 ||
        conn->ce_peer_resp_page_size == 0 ||
        conn->ce_peer_resp_virt_page_base == 0) {
        return -1;
    }

    if (logical_addr < conn->ce_peer_resp_logical_page_base)
        return -1;

    const uint64_t page_delta_u64 =
        logical_addr - conn->ce_peer_resp_logical_page_base;
    if (page_delta_u64 > (uint64_t)SIZE_MAX)
        return -1;

    const size_t page_delta = (size_t)page_delta_u64;
    const size_t page_index = page_delta / conn->ce_peer_resp_page_size;
    if (page_index >= conn->ce_peer_resp_page_count)
        return -1;

    uint64_t page_phys = conn->ce_peer_resp_page_phys[page_index];
    if (page_phys == UINT64_MAX) {
        const uintptr_t page_virt =
            conn->ce_peer_resp_virt_page_base +
            page_index * (uintptr_t)conn->ce_peer_resp_page_size;
        if (cxl_resolve_virt_page_phys(page_virt,
                                       conn->ce_peer_resp_page_size,
                                       &page_phys) != 0) {
            return -1;
        }
        conn->ce_peer_resp_page_phys[page_index] = page_phys;
    }

    *phys_out = page_phys +
        (uint64_t)(page_delta % conn->ce_peer_resp_page_size);
    return 0;
}

int
cxl_copyengine_update_peer_response_mapping(cxl_connection_t *conn)
{
    if (!conn || !conn->peer_response_data || conn->peer_response_data_size == 0)
        return -1;

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

    free(conn->ce_peer_resp_page_phys);
    conn->ce_peer_resp_page_phys = page_phys;
    conn->ce_peer_resp_logical_page_base =
        conn->peer_response_data_addr -
        (uint64_t)((uintptr_t)conn->peer_response_data - virt_page_base);
    conn->ce_peer_resp_page_size = page_size;
    conn->ce_peer_resp_page_count = page_count;
    conn->ce_peer_resp_virt_page_base = virt_page_base;
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
    return 0;
}

static int
cxl_copyengine_channel_configure_slots(cxl_ce_channel_state_t *chan,
                                       size_t slots)
{
    if (!chan || !chan->engine)
        return -1;
    if (slots == 0) {
        errno = EINVAL;
        return -1;
    }

    long page_size_l = sysconf(_SC_PAGESIZE);
    const size_t resp_bytes = slots * (size_t)CXL_CE_RESP_SLOT_BYTES;
    const size_t flag_bytes = slots * (size_t)CXL_CE_FLAG_SLOT_BYTES;
    const size_t desc_count = slots * (size_t)CXL_CE_DESC_PER_REQ;
    const size_t desc_bytes = desc_count * sizeof(cxl_ce_desc_t);
    size_t resp_align = (page_size_l > 0) ? (size_t)page_size_l :
                        (size_t)CXL_CE_RESP_SLOT_BYTES;
    if (resp_align < (size_t)CXL_CE_RESP_SLOT_BYTES)
        resp_align = (size_t)CXL_CE_RESP_SLOT_BYTES;

    cxl_copyengine_channel_release_buffers(chan);

    int memalign_rc = posix_memalign(
        (void **)&chan->resp_src_pool, resp_align, resp_bytes);
    if (memalign_rc != 0) {
        errno = memalign_rc;
        return -1;
    }
    memalign_rc = posix_memalign(
        (void **)&chan->flag_src_pool, 64, flag_bytes);
    if (memalign_rc != 0) {
        errno = memalign_rc;
        cxl_copyengine_channel_release_buffers(chan);
        return -1;
    }
    memalign_rc = posix_memalign(
        (void **)&chan->desc_pool, 64, desc_bytes);
    if (memalign_rc != 0) {
        errno = memalign_rc;
        cxl_copyengine_channel_release_buffers(chan);
        return -1;
    }

    memset(chan->resp_src_pool, 0, resp_bytes);
    memset(chan->flag_src_pool, 0, flag_bytes);
    memset(chan->desc_pool, 0, desc_bytes);

    (void)mlock(chan->resp_src_pool, resp_bytes);
    (void)mlock(chan->flag_src_pool, flag_bytes);
    (void)mlock(chan->desc_pool, desc_bytes);

    chan->resp_src_slot_phys = (uint64_t *)calloc(slots, sizeof(uint64_t));
    chan->flag_src_slot_phys = (uint64_t *)calloc(slots, sizeof(uint64_t));
    chan->desc_phys = (uint64_t *)calloc(desc_count, sizeof(uint64_t));
    if (!chan->resp_src_slot_phys || !chan->flag_src_slot_phys ||
        !chan->desc_phys) {
        errno = errno ? errno : ENOMEM;
        cxl_copyengine_channel_release_buffers(chan);
        return -1;
    }

    for (size_t slot = 0; slot < slots; slot++) {
        uint8_t *src_resp =
            chan->resp_src_pool + slot * (size_t)CXL_CE_RESP_SLOT_BYTES;
        uint8_t *src_flag =
            chan->flag_src_pool + slot * (size_t)CXL_CE_FLAG_SLOT_BYTES;
        uint64_t src_resp_phys = 0;
        uint64_t src_flag_phys = 0;
        if (cxl_virt_to_phys(src_resp, &src_resp_phys) != 0 ||
            cxl_virt_to_phys(src_flag, &src_flag_phys) != 0) {
            errno = errno ? errno : EFAULT;
            cxl_copyengine_channel_release_buffers(chan);
            return -1;
        }
        chan->resp_src_slot_phys[slot] = src_resp_phys;
        chan->flag_src_slot_phys[slot] = src_flag_phys;
    }

    for (size_t idx = 0; idx < desc_count; idx++) {
        uint64_t desc_phys = 0;
        if (cxl_virt_to_phys(chan->desc_pool + idx, &desc_phys) != 0) {
            errno = errno ? errno : EFAULT;
            cxl_copyengine_channel_release_buffers(chan);
            return -1;
        }
        chan->desc_phys[idx] = desc_phys;
    }

    chan->slots = slots;
    cxl_copyengine_channel_prime_slot_templates(chan);
    chan->next_slot = 0;
    chan->chain_started = 0;
    chan->last_desc = NULL;
    return 0;
}

static int
cxl_copyengine_channel_init(cxl_ce_channel_state_t *chan)
{
    if (!chan)
        return -1;

    chan->poisoned = 0;
    chan->slots = 0;
    chan->next_slot = 0;
    chan->chain_started = 0;
    chan->last_desc = NULL;
    chan->resp_src_pool = NULL;
    chan->flag_src_pool = NULL;
    chan->desc_pool = NULL;
    chan->resp_src_slot_phys = NULL;
    chan->flag_src_slot_phys = NULL;
    chan->desc_phys = NULL;
    return 0;
}

void
cxl_connection_init_runtime_defaults(cxl_connection_t *conn)
{
    if (!conn)
        return;

    conn->rpc_id_seq_mask = (uint16_t)CXL_RPC_ID_MASK;
    conn->rpc_id_next = 1;
    conn->rpc_id_inflight_count = 0;
    conn->rpc_id_capacity = (uint32_t)CXL_RPC_ID_MASK;
    conn->rpc_id_inflight_bitmap = NULL;
    conn->mq_prefetch_start_line = 0;
    conn->mq_prefetch_nr_lines = 0;
    conn->mq_prefetch_window_valid = 0;
    conn->req_entry_offsets = NULL;
    conn->req_entry_sizes = NULL;
    conn->req_entry_next = NULL;
    conn->req_entry_complete = NULL;
    conn->ce_lane_bind_valid = 0;
    conn->ce_bind_engine_index = 0;
    conn->ce_bind_channel_id = 0;

    cxl_copyengine_clear_conn_state(conn);
}

void
cxl_copyengine_disable(cxl_connection_t *conn)
{
    cxl_ce_channel_state_t *chan = NULL;
    int wait_rc = 0;

    if (!conn)
        return;

    wait_rc = cxl_copyengine_wait_idle(conn);

    chan = cxl_copyengine_get_assigned_channel(conn);
    if (chan) {
        if (wait_rc != 0)
            chan->poisoned = 1;
        chan->owner_conn = NULL;
        if (g_cxl_ce.attached_conns > 0)
            g_cxl_ce.attached_conns--;
    }
    cxl_copyengine_clear_conn_state(conn);

    if (wait_rc != 0)
        return;

    if (g_cxl_ce.attached_conns == 0)
        cxl_copyengine_global_reset();
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
    if (conn->ce_ready) {
        cxl_copyengine_sync_conn_state(conn);
        return 1;
    }
    if (g_cxl_ce.ready) {
        cxl_copyengine_sync_conn_state(conn);
        return 1;
    }
    if (g_cxl_ce.init_attempted) {
        cxl_copyengine_sync_conn_state(conn);
        return 0;
    }

    g_cxl_ce.init_attempted = 1;

    if (cxl_find_copyengine_resource0_paths(&resource_paths,
                                            &resource_count) != 0) {
        fail_stage = "find_copyengine_resource0_paths";
        fail_errno = errno;
        goto fail;
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
        resource = resource_paths[i];

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
        if (chan_count == 0) {
            fail_stage = "read_channel_count";
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
            chan->owner_conn = NULL;
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
    cxl_free_copyengine_paths(resource_paths, resource_count);
    cxl_copyengine_sync_conn_state(conn);
    return 1;

fail:
    if (!g_cxl_ce.warned) {
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
        g_cxl_ce.warned = 1;
    }
    cxl_free_copyengine_paths(resource_paths, resource_count);
    cxl_copyengine_global_reset();
    cxl_copyengine_sync_conn_state(conn);
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
        cxl_copyengine_sync_conn_state(conn);
        return 0;
    }

    if (!conn->ce_lane_bind_valid) {
        if (!g_cxl_ce.warned) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine lane is not bound before response setup\n");
            g_cxl_ce.warned = 1;
        }
        cxl_copyengine_sync_conn_state(conn);
        return -1;
    }

    for (size_t i = 0; i < g_cxl_ce.channel_count; i++) {
        chan = &g_cxl_ce.channels[i];
        if (chan->engine_index != conn->ce_bind_engine_index ||
            chan->chan_id != conn->ce_bind_channel_id) {
            continue;
        }
        if (chan->poisoned) {
            if (!g_cxl_ce.warned) {
                fprintf(stderr,
                        "cxl_rpc: ERROR: requested CopyEngine lane is poisoned"
                        " engine=%zu channel=%u\n",
                        conn->ce_bind_engine_index,
                        conn->ce_bind_channel_id);
                g_cxl_ce.warned = 1;
            }
            cxl_copyengine_sync_conn_state(conn);
            return -1;
        }

        if (chan->owner_conn && chan->owner_conn != conn) {
            if (!g_cxl_ce.warned) {
                fprintf(stderr,
                        "cxl_rpc: ERROR: requested CopyEngine lane already in use"
                        " engine=%zu channel=%u\n",
                        conn->ce_bind_engine_index,
                        conn->ce_bind_channel_id);
                g_cxl_ce.warned = 1;
            }
            cxl_copyengine_sync_conn_state(conn);
            return -1;
        }

        if (!chan->owner_conn) {
            chan->owner_conn = conn;
            g_cxl_ce.attached_conns++;
        }
        conn->ce_lane_assigned = 1;
        conn->ce_engine_index = chan->engine_index;
        conn->ce_channel_index = chan->channel_index;
        conn->ce_hw_channel_id = chan->chan_id;
        cxl_copyengine_sync_conn_state(conn);
        return 0;
    }

    if (!g_cxl_ce.warned) {
        fprintf(stderr,
                "cxl_rpc: ERROR: requested CopyEngine lane does not exist"
                " engine=%zu channel=%u available_channels=%zu\n",
                conn->ce_bind_engine_index,
                conn->ce_bind_channel_id,
                g_cxl_ce.channel_count);
        g_cxl_ce.warned = 1;
    }
    cxl_copyengine_sync_conn_state(conn);
    return -1;
}

static void
cxl_copyengine_channel_reset_slot_state(cxl_ce_channel_state_t *chan,
                                        uint64_t ce_status)
{
    if (!chan)
        return;
    (void)ce_status;

    chan->next_slot = 0;
    chan->chain_started = 0;
    chan->last_desc = NULL;
}

static size_t
cxl_copyengine_drain_spins(void)
{
    return CXL_CE_FIXED_DRAIN_SPINS;
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
            if (!g_cxl_ce.warned) {
                fprintf(stderr,
                        "cxl_rpc: ERROR: CopyEngine engine=%s channel=%u error while draining=0x%08x\n",
                        chan->engine->resource0_path, chan->chan_id, ce_err);
                g_cxl_ce.warned = 1;
            }
            chan->poisoned = 1;
            cxl_copyengine_sync_conn_state(conn);
            return -1;
        }

        uint64_t ce_status = cxl_ce_mmio_read64(chan->engine->bar0,
                                                chan->chan_mmio_base +
                                                CXL_CE_CHAN_STATUS);
        if ((ce_status & 0x1ULL) != 0) {
            cxl_copyengine_channel_reset_slot_state(chan, ce_status);
            cxl_copyengine_sync_conn_state(conn);
            return 0;
        }
        __asm__ __volatile__("pause" ::: "memory");
    }

    fprintf(stderr,
            "cxl_rpc: ERROR: timed out waiting for CopyEngine idle"
            " engine=%s channel=%u next_slot=%zu chain_started=%d\n",
            chan->engine->resource0_path, chan->chan_id,
            chan->next_slot, chan->chain_started);
    chan->poisoned = 1;
    cxl_copyengine_sync_conn_state(conn);
    return -1;
}

static int
cxl_copyengine_wait_idle(cxl_connection_t *conn)
{
    cxl_ce_channel_state_t *chan = NULL;

    if (!conn || !g_cxl_ce.ready)
        return 0;

    chan = cxl_copyengine_get_assigned_channel(conn);
    if (!chan)
        return 0;
    return cxl_copyengine_channel_wait_idle(conn, chan);
}

int
cxl_copyengine_ensure_response_slots(cxl_connection_t *conn)
{
    cxl_ce_channel_state_t *chan = NULL;
    size_t response_slots = 0;
    size_t needed_slots = 0;

    if (!conn)
        return -1;

    chan = cxl_copyengine_get_assigned_channel(conn);
    if (!chan || chan->poisoned)
        return -1;

    response_slots = conn->peer_response_data_size / (size_t)CXL_RESP_SLOT_BYTES;
    needed_slots = (response_slots > 0) ? response_slots : 1;
    if (needed_slots > (size_t)CXL_CE_MAX_PREALLOC_SLOTS)
        needed_slots = (size_t)CXL_CE_MAX_PREALLOC_SLOTS;
    if (conn->rpc_id_capacity > 0 && needed_slots > conn->rpc_id_capacity) {
        needed_slots = conn->rpc_id_capacity;
    }
    if (needed_slots == 0) {
        errno = EINVAL;
        return -1;
    }

    if (chan->slots >= needed_slots &&
        chan->resp_src_pool && chan->flag_src_pool && chan->desc_pool &&
        chan->resp_src_slot_phys && chan->flag_src_slot_phys &&
        chan->desc_phys) {
        return 0;
    }

    if (chan->chain_started &&
        cxl_copyengine_channel_wait_idle(conn, chan) != 0) {
        return -1;
    }

    if (cxl_copyengine_channel_configure_slots(chan, needed_slots) != 0)
        return -1;

    cxl_copyengine_sync_conn_state(conn);
    return 0;
}

int
cxl_copyengine_submit_response_async(cxl_connection_t *conn,
                                     uint16_t rpc_id,
                                     const void *data,
                                     size_t len,
                                     size_t dst_resp_offset,
                                     size_t resp_transfer_size)
{
    cxl_ce_channel_state_t *chan = NULL;
    volatile uint8_t *bar0 = NULL;
    const char *submit_mode = "start";
    uint64_t dst_flag_phys = 0;
    uint64_t dst_resp_phys0 = 0;
    uint64_t dst_resp_phys1 = 0;
    uint64_t logical_resp_addr = 0;
    size_t slot = 0;
    size_t desc_base_idx = 0;
    uint8_t *src_resp = NULL;
    uint8_t *src_flag = NULL;
    cxl_ce_desc_t *resp_desc0 = NULL;
    cxl_ce_desc_t *resp_desc1 = NULL;
    cxl_ce_desc_t *flag_desc = NULL;
    uint64_t desc_phys0 = 0;
    uint64_t desc_phys1 = 0;
    uint64_t flag_desc_phys = 0;
    uint64_t src_resp_phys = 0;
    uint64_t src_flag_phys = 0;
    size_t first_len = 0;
    size_t second_len = 0;
    size_t page_delta = 0;
    size_t page_index = 0;
    size_t page_off = 0;
    size_t page_size = 0;
    size_t resp_segment_count = 0;

    chan = cxl_copyengine_get_assigned_channel(conn);
    if (chan->poisoned)
        return 0;
    bar0 = conn->ce_bar0;

    if (chan->next_slot >= chan->slots) {
        if (cxl_copyengine_channel_wait_idle(conn, chan) != 0)
            return 0;
    }
    if (chan->next_slot >= chan->slots) {
        if (!g_cxl_ce.warned) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: dedicated CopyEngine lane exhausted"
                    " engine=%s channel=%u slots=%zu\n",
                    chan->engine->resource0_path, chan->chan_id, chan->slots);
            g_cxl_ce.warned = 1;
        }
        return 0;
    }

    if (!conn->ce_peer_resp_page_phys || conn->ce_peer_resp_page_count == 0 ||
        conn->ce_peer_resp_page_size == 0 ||
        dst_resp_offset > conn->peer_response_data_size ||
        resp_transfer_size > conn->peer_response_data_size - dst_resp_offset) {
        if (!g_cxl_ce.warned) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine response segment state invalid\n");
            g_cxl_ce.warned = 1;
        }
        return 0;
    }

    dst_flag_phys = conn->ce_peer_flag_phys;
    page_size = conn->ce_peer_resp_page_size;
    logical_resp_addr = conn->peer_response_data_addr + dst_resp_offset;
    if (logical_resp_addr < conn->ce_peer_resp_logical_page_base) {
        if (!g_cxl_ce.warned) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine logical response address below page base\n");
            g_cxl_ce.warned = 1;
        }
        return 0;
    }
    page_delta = (size_t)(logical_resp_addr -
                          conn->ce_peer_resp_logical_page_base);
    page_index = page_delta / page_size;
    page_off = page_delta % page_size;
    if (page_index >= conn->ce_peer_resp_page_count) {
        if (!g_cxl_ce.warned) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine response page index out of range offset=%zu page_index=%zu page_count=%zu\n",
                    dst_resp_offset, page_index, conn->ce_peer_resp_page_count);
            g_cxl_ce.warned = 1;
        }
        return 0;
    }
    first_len = resp_transfer_size;
    if (page_off + first_len > page_size)
        first_len = page_size - page_off;
    second_len = resp_transfer_size - first_len;
    resp_segment_count = (second_len > 0) ? 2u : 1u;
    if (cxl_copyengine_translate_peer_response_logical(
            conn,
            logical_resp_addr,
            &dst_resp_phys0) != 0) {
        if (!g_cxl_ce.warned) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine first response translation failed\n");
            g_cxl_ce.warned = 1;
        }
        return 0;
    }
    if (second_len > 0) {
        if (page_index + 1 >= conn->ce_peer_resp_page_count ||
            cxl_copyengine_translate_peer_response_logical(
                conn,
                logical_resp_addr + first_len,
                &dst_resp_phys1) != 0) {
            if (!g_cxl_ce.warned) {
                fprintf(stderr,
                        "cxl_rpc: ERROR: CopyEngine second response translation failed\n");
                g_cxl_ce.warned = 1;
            }
            return 0;
        }
    }

    slot = chan->next_slot++;
    src_resp = chan->resp_src_pool + slot * (size_t)CXL_CE_RESP_SLOT_BYTES;
    src_flag = chan->flag_src_pool + slot * (size_t)CXL_CE_FLAG_SLOT_BYTES;
    desc_base_idx = slot * CXL_CE_DESC_PER_REQ;
    src_resp_phys = chan->resp_src_slot_phys[slot];
    src_flag_phys = chan->flag_src_slot_phys[slot];
    resp_desc0 = chan->desc_pool + desc_base_idx;
    resp_desc1 = resp_desc0 + 1;
    flag_desc = resp_desc0 + 2;
    desc_phys0 = chan->desc_phys[desc_base_idx];
    desc_phys1 = chan->desc_phys[desc_base_idx + 1];
    flag_desc_phys = chan->desc_phys[desc_base_idx + 2];

    struct __attribute__((packed)) {
        uint32_t payload_len;
        uint16_t rpc_id;
        uint16_t response_id;
    } hdr = {(uint32_t)len, rpc_id, rpc_id};

    memcpy(src_resp, &hdr, sizeof(hdr));
    if (len > 0)
        memcpy(src_resp + sizeof(hdr), data, len);
    memcpy(src_flag, &rpc_id, CXL_FLAG_PUBLISH_LEN);

    resp_desc0->len = (uint32_t)first_len;
    resp_desc0->dest = dst_resp_phys0;
    resp_desc0->next = (second_len > 0) ? desc_phys1 : flag_desc_phys;
    if (second_len > 0) {
        resp_desc1->len = (uint32_t)second_len;
        resp_desc1->src = src_resp_phys + first_len;
        resp_desc1->dest = dst_resp_phys1;
        resp_desc1->next = flag_desc_phys;
    }

    (void)src_flag_phys;
    flag_desc->dest = dst_flag_phys;
    flag_desc->next = 0;

    /*
     * Keep the response payload, publish flag, and descriptor cachelines
     * resident for this experiment; only preserve store ordering before the
     * MMIO start command.
     */
    __asm__ __volatile__("sfence" ::: "memory");

    if (cxl_ce_debug_enabled()) {
        cxl_ce_debugf("submit_response rpc_id=%u slot=%zu dst_resp_offset=%zu resp_transfer_size=%zu resp_segments=%zu dst_flag_phys=%#llx chain_started=%d",
                      rpc_id,
                      slot,
                      dst_resp_offset,
                      resp_transfer_size,
                      resp_segment_count,
                      (unsigned long long)dst_flag_phys,
                      chan->chain_started);
        cxl_ce_debugf("submit_response seg[0] src_phys=%#llx src_off=0 dst_phys=%#llx len=%u next=%#llx",
                      (unsigned long long)resp_desc0->src,
                      (unsigned long long)resp_desc0->dest,
                      resp_desc0->len,
                      (unsigned long long)resp_desc0->next);
        if (second_len > 0) {
            cxl_ce_debugf("submit_response seg[1] src_phys=%#llx src_off=%zu dst_phys=%#llx len=%u next=%#llx",
                          (unsigned long long)resp_desc1->src,
                          first_len,
                          (unsigned long long)resp_desc1->dest,
                          resp_desc1->len,
                          (unsigned long long)resp_desc1->next);
        }
        cxl_ce_debugf("submit_response flag_desc src_phys=%#llx dst_phys=%#llx len=%u desc_phys=%#llx",
                      (unsigned long long)flag_desc->src,
                      (unsigned long long)flag_desc->dest,
                      flag_desc->len,
                      (unsigned long long)flag_desc_phys);
    }

    if (chan->chain_started) {
        if (!chan->last_desc) {
            if (!g_cxl_ce.warned) {
                fprintf(stderr,
                        "cxl_rpc: ERROR: CopyEngine append path missing tail"
                        " engine=%s channel=%u\n",
                        chan->engine->resource0_path, chan->chan_id);
                g_cxl_ce.warned = 1;
            }
            chan->poisoned = 1;
            cxl_copyengine_sync_conn_state(conn);
            return 0;
        }
        chan->last_desc->next = desc_phys0;
        submit_mode = "append";
        cxl_ce_mmio_write8(bar0,
                           chan->chan_mmio_base + CXL_CE_CHAN_COMMAND,
                           CXL_CE_CMD_APPEND_DMA);
    } else {
        cxl_ce_mmio_write64(bar0,
                            chan->chan_mmio_base + CXL_CE_CHAN_CHAINADDR,
                            desc_phys0);
        cxl_ce_mmio_write8(bar0,
                           chan->chan_mmio_base + CXL_CE_CHAN_COMMAND,
                           CXL_CE_CMD_START_DMA);
    }
    chan->chain_started = 1;

    chan->last_desc = flag_desc;
    conn->ce_chain_started = chan->chain_started;
    conn->ce_next_slot = chan->next_slot;
    conn->ce_last_desc = chan->last_desc;
    if (cxl_ce_debug_enabled()) {
        cxl_ce_debugf("submit_response queued rpc_id=%u slot=%zu first_desc=%#llx last_desc=%#llx next_slot=%zu mode=%s",
                      rpc_id,
                      slot,
                      (unsigned long long)desc_phys0,
                      (unsigned long long)flag_desc_phys,
                      chan->next_slot,
                      submit_mode);
    }
    return 1;
}
