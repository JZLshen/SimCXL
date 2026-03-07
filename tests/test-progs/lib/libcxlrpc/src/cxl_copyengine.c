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

/* CopyEngine (IOAT-like) MMIO register constants for channel 0. */
#define CXL_CE_CHAN_BASE      0x80u
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
 * - two descriptors per request (resp + flag)
 *
 * Descriptors/sources are reused only after the channel drains completely.
 * This keeps the async fast path simple while avoiding permanent pool
 * exhaustion across long sequential runs.
 */
#define CXL_CE_RESP_SLOT_BYTES 4096u
#define CXL_CE_FLAG_SLOT_BYTES 64u
#define CXL_CE_DESC_PER_REQ    2u
#define CXL_CE_FIXED_SLOTS     1024u
#define CXL_CE_FIXED_DRAIN_SPINS (1u << 22)
/* Smallest BAR window covering MMIO offsets we actually access. */
#define CXL_CE_MMIO_REQUIRED_BYTES \
    (CXL_CE_CHAN_BASE + CXL_CE_CHAN_COMMAND + sizeof(uint8_t))

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
    int chain_started;
    int bar_fd;
    volatile uint8_t *bar0;
    void *bar_map_base;
    size_t bar_map_len;
    size_t slots;
    size_t next_slot;
    uint8_t *resp_src_pool;
    uint8_t *flag_src_pool;
    cxl_ce_desc_t *desc_pool;
    uint64_t *resp_src_slot_phys;
    uint64_t *flag_src_slot_phys;
    uint64_t *desc_phys;
    cxl_ce_desc_t *last_desc;
    size_t attached_conns;
} cxl_ce_shared_state_t;

static cxl_ce_shared_state_t g_cxl_ce = {
    .bar_fd = -1,
};

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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

static inline uint64_t
cxl_ce_mmio_read64(volatile uint8_t *bar0, uint32_t off)
{
    return *(volatile uint64_t *)(bar0 + off);
}

static int cxl_copyengine_wait_idle(cxl_connection_t *conn);

static void
cxl_copyengine_clear_peer_phys_cache(cxl_connection_t *conn)
{
    if (!conn)
        return;

    free(conn->ce_peer_resp_page_phys);
    conn->ce_peer_resp_page_phys = NULL;
    conn->ce_peer_resp_virt_page_base = 0;
    conn->ce_peer_resp_page_size = 0;
    conn->ce_peer_resp_page_count = 0;
    conn->ce_peer_flag_phys = 0;
    conn->ce_peer_flag_phys_valid = 0;
}

static void
cxl_copyengine_sync_conn_state(cxl_connection_t *conn)
{
    if (!conn)
        return;

    conn->ce_init_attempted = g_cxl_ce.init_attempted;
    conn->ce_ready = g_cxl_ce.ready;
    conn->ce_warned = g_cxl_ce.warned;
    conn->ce_chain_started = g_cxl_ce.chain_started;
    conn->ce_bar_fd = g_cxl_ce.bar_fd;
    conn->ce_bar0 = g_cxl_ce.bar0;
    conn->ce_bar_map_base = g_cxl_ce.bar_map_base;
    conn->ce_bar_map_len = g_cxl_ce.bar_map_len;
    conn->ce_slots = g_cxl_ce.slots;
    conn->ce_next_slot = g_cxl_ce.next_slot;
    conn->ce_resp_src_pool = g_cxl_ce.resp_src_pool;
    conn->ce_flag_src_pool = g_cxl_ce.flag_src_pool;
    conn->ce_desc_pool = g_cxl_ce.desc_pool;
    conn->ce_last_desc = g_cxl_ce.last_desc;
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
    conn->ce_bar_fd = -1;
    conn->ce_bar0 = NULL;
    conn->ce_bar_map_base = NULL;
    conn->ce_bar_map_len = 0;
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
    if (g_cxl_ce.bar_map_base && g_cxl_ce.bar_map_len > 0) {
        munmap(g_cxl_ce.bar_map_base, g_cxl_ce.bar_map_len);
    }
    if (g_cxl_ce.bar_fd >= 0) {
        close(g_cxl_ce.bar_fd);
    }

    free(g_cxl_ce.resp_src_pool);
    free(g_cxl_ce.flag_src_pool);
    free(g_cxl_ce.desc_pool);
    free(g_cxl_ce.resp_src_slot_phys);
    free(g_cxl_ce.flag_src_slot_phys);
    free(g_cxl_ce.desc_phys);

    memset(&g_cxl_ce, 0, sizeof(g_cxl_ce));
    g_cxl_ce.bar_fd = -1;
}

static inline void
cxl_ce_flush_range(const void *ptr, size_t len)
{
    if (!ptr || len == 0)
        return;
    uintptr_t start = (uintptr_t)ptr & ~((uintptr_t)63);
    uintptr_t end = ((uintptr_t)ptr + len + 63u) & ~((uintptr_t)63);
    for (uintptr_t p = start; p < end; p += 64u) {
        __asm__ __volatile__(
            "clflushopt (%0)"
            :
            : "r"((const void *)p)
            : "memory");
    }
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

static int
cxl_find_copyengine_resource0(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        errno = EINVAL;
        return -1;
    }

    const char *sysfs_root = "/sys/bus/pci/devices";
    DIR *dir = opendir(sysfs_root);
    if (!dir)
        return -1;

    int found = 0;
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

        if ((size_t)snprintf(out, out_len, "%s", resource0_path) >= out_len) {
            errno = ENAMETOOLONG;
            closedir(dir);
            return -1;
        }
        found = 1;
        break;
    }

    int saved = errno;
    closedir(dir);
    if (!found) {
        errno = ENOENT;
        return -1;
    }
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
cxl_build_virt_page_phys_map(const void *virt_base,
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
    uint64_t *page_phys = (uint64_t *)calloc(page_count, sizeof(uint64_t));
    if (!page_phys)
        return -1;

    for (size_t i = 0; i < page_count; i++) {
        const uintptr_t page_virt = page_start + i * (uintptr_t)page_size;
        volatile uint8_t page_touch =
            *(const volatile uint8_t *)(uintptr_t)page_virt;
        (void)page_touch;

        uint64_t phys = 0;
        if (cxl_virt_to_phys((const void *)page_virt, &phys) != 0) {
            free(page_phys);
            return -1;
        }
        page_phys[i] = phys & ~((uint64_t)page_size - 1u);
    }

    *virt_page_base_out = page_start;
    *page_size_out = page_size;
    *page_count_out = page_count;
    *page_phys_out = page_phys;
    return 0;
}

static int
cxl_translate_with_page_phys_map(const void *vaddr,
                                 uintptr_t virt_page_base,
                                 size_t page_size,
                                 size_t page_count,
                                 const uint64_t *page_phys,
                                 uint64_t *phys_out)
{
    if (!vaddr || !phys_out || !page_phys ||
        page_size == 0 || page_count == 0) {
        return -1;
    }

    const uintptr_t virt = (uintptr_t)vaddr;
    if (virt < virt_page_base)
        return -1;

    const size_t page_delta = (size_t)(virt - virt_page_base);
    const size_t page_index = page_delta / page_size;
    if (page_index >= page_count)
        return -1;

    *phys_out = page_phys[page_index] + (uint64_t)(page_delta % page_size);
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
    if (cxl_build_virt_page_phys_map(
            (const void *)conn->peer_response_data,
            conn->peer_response_data_size,
            &virt_page_base, &page_size, &page_count, &page_phys) != 0) {
        return -1;
    }

    free(conn->ce_peer_resp_page_phys);
    conn->ce_peer_resp_page_phys = page_phys;
    conn->ce_peer_resp_virt_page_base = virt_page_base;
    conn->ce_peer_resp_page_size = page_size;
    conn->ce_peer_resp_page_count = page_count;
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

    uint64_t flag_phys = 0;
    if (cxl_virt_to_phys((const void *)conn->peer_flag, &flag_phys) != 0)
        return -1;

    conn->ce_peer_flag_phys = flag_phys;
    conn->ce_peer_flag_phys_valid = 1;
    return 0;
}

void
cxl_connection_init_runtime_defaults(cxl_connection_t *conn)
{
    cxl_copyengine_clear_conn_state(conn);
}

void
cxl_copyengine_disable(cxl_connection_t *conn)
{
    if (!conn)
        return;

    (void)cxl_copyengine_wait_idle(conn);

    if (conn->ce_ready && g_cxl_ce.attached_conns > 0)
        g_cxl_ce.attached_conns--;
    cxl_copyengine_clear_conn_state(conn);

    if (g_cxl_ce.attached_conns == 0)
        cxl_copyengine_global_reset();
}

static int
cxl_copyengine_init(cxl_connection_t *conn)
{
    size_t map_len = 0;
    size_t page_size = 0;
    size_t bar_page_off = 0;
    size_t slots = 0;
    struct stat st;
    int fail_errno = 0;
    const char *fail_stage = "unknown";
    const char *resource = NULL;
    char autodetect_resource0[PATH_MAX];

    if (!conn)
        return 0;
    if (conn->ce_ready) {
        cxl_copyengine_sync_conn_state(conn);
        return 1;
    }
    if (g_cxl_ce.ready) {
        g_cxl_ce.attached_conns++;
        cxl_copyengine_sync_conn_state(conn);
        return 1;
    }
    if (g_cxl_ce.init_attempted) {
        cxl_copyengine_sync_conn_state(conn);
        return 0;
    }

    g_cxl_ce.init_attempted = 1;
    g_cxl_ce.bar_fd = -1;

    if (cxl_find_copyengine_resource0(autodetect_resource0,
                                      sizeof(autodetect_resource0)) != 0) {
        fail_stage = "find_copyengine_resource0";
        fail_errno = errno;
        goto fail;
    }
    resource = autodetect_resource0;

    if (cxl_enable_copyengine_pci_command(resource) != 0) {
        fail_stage = "enable_copyengine_pci_command";
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
    if (cxl_read_copyengine_resource0_page_off(resource, page_size,
                                               &bar_page_off) != 0) {
        fail_stage = "read_resource0_page_off";
        fail_errno = errno;
        goto fail;
    }
    map_len = CXL_CE_MMIO_REQUIRED_BYTES + bar_page_off;
    map_len = ((map_len + page_size - 1u) / page_size) * page_size;

    slots = CXL_CE_FIXED_SLOTS;
    size_t resp_bytes = slots * (size_t)CXL_CE_RESP_SLOT_BYTES;
    size_t flag_bytes = slots * (size_t)CXL_CE_FLAG_SLOT_BYTES;
    size_t desc_count = slots * (size_t)CXL_CE_DESC_PER_REQ;
    size_t desc_bytes = desc_count * sizeof(cxl_ce_desc_t);

    int memalign_rc = posix_memalign(
        (void **)&g_cxl_ce.resp_src_pool, 64, resp_bytes);
    if (memalign_rc != 0) {
        fail_stage = "posix_memalign(resp_src_pool)";
        fail_errno = memalign_rc;
        goto fail;
    }
    memalign_rc = posix_memalign(
        (void **)&g_cxl_ce.flag_src_pool, 64, flag_bytes);
    if (memalign_rc != 0) {
        fail_stage = "posix_memalign(flag_src_pool)";
        fail_errno = memalign_rc;
        goto fail;
    }
    memalign_rc = posix_memalign(
        (void **)&g_cxl_ce.desc_pool, 64, desc_bytes);
    if (memalign_rc != 0) {
        fail_stage = "posix_memalign(desc_pool)";
        fail_errno = memalign_rc;
        goto fail;
    }
    memset(g_cxl_ce.resp_src_pool, 0, resp_bytes);
    memset(g_cxl_ce.flag_src_pool, 0, flag_bytes);
    memset(g_cxl_ce.desc_pool, 0, desc_bytes);

    (void)mlock(g_cxl_ce.resp_src_pool, resp_bytes);
    (void)mlock(g_cxl_ce.flag_src_pool, flag_bytes);
    (void)mlock(g_cxl_ce.desc_pool, desc_bytes);

    g_cxl_ce.resp_src_slot_phys = (uint64_t *)calloc(slots, sizeof(uint64_t));
    if (!g_cxl_ce.resp_src_slot_phys) {
        fail_stage = "calloc(resp_src_slot_phys)";
        fail_errno = errno ? errno : ENOMEM;
        goto fail;
    }
    g_cxl_ce.flag_src_slot_phys = (uint64_t *)calloc(slots, sizeof(uint64_t));
    if (!g_cxl_ce.flag_src_slot_phys) {
        fail_stage = "calloc(flag_src_slot_phys)";
        fail_errno = errno ? errno : ENOMEM;
        goto fail;
    }
    g_cxl_ce.desc_phys = (uint64_t *)calloc(desc_count, sizeof(uint64_t));
    if (!g_cxl_ce.desc_phys) {
        fail_stage = "calloc(desc_phys)";
        fail_errno = errno ? errno : ENOMEM;
        goto fail;
    }

    for (size_t slot = 0; slot < slots; slot++) {
        uint8_t *src_resp =
            g_cxl_ce.resp_src_pool + slot * (size_t)CXL_CE_RESP_SLOT_BYTES;
        uint8_t *src_flag =
            g_cxl_ce.flag_src_pool + slot * (size_t)CXL_CE_FLAG_SLOT_BYTES;
        uint64_t src_resp_phys = 0;
        uint64_t src_flag_phys = 0;
        if (cxl_virt_to_phys(src_resp, &src_resp_phys) != 0 ||
            cxl_virt_to_phys(src_flag, &src_flag_phys) != 0) {
            fail_stage = "virt_to_phys(src slot pool)";
            fail_errno = errno ? errno : EFAULT;
            goto fail;
        }
        g_cxl_ce.resp_src_slot_phys[slot] = src_resp_phys;
        g_cxl_ce.flag_src_slot_phys[slot] = src_flag_phys;
    }

    for (size_t idx = 0; idx < desc_count; idx++) {
        uint64_t desc_phys = 0;
        if (cxl_virt_to_phys(g_cxl_ce.desc_pool + idx, &desc_phys) != 0) {
            fail_stage = "virt_to_phys(desc pool)";
            fail_errno = errno ? errno : EFAULT;
            goto fail;
        }
        g_cxl_ce.desc_phys[idx] = desc_phys;
    }

    g_cxl_ce.bar_fd = open(resource, O_RDWR | O_SYNC | O_CLOEXEC);
    if (g_cxl_ce.bar_fd < 0) {
        fail_stage = "open(resource0)";
        fail_errno = errno;
        goto fail;
    }

    if (fstat(g_cxl_ce.bar_fd, &st) == 0 && st.st_size > 0 &&
        (uint64_t)st.st_size < (uint64_t)CXL_CE_MMIO_REQUIRED_BYTES) {
        fail_stage = "fstat(resource0 size too small)";
        fail_errno = 0;
        goto fail;
    }

    void *map_base = mmap(NULL, map_len,
                          PROT_READ | PROT_WRITE, MAP_SHARED,
                          g_cxl_ce.bar_fd, 0);
    if (map_base == MAP_FAILED) {
        fail_stage = "mmap(resource0)";
        fail_errno = errno;
        goto fail;
    }
    g_cxl_ce.bar_map_base = map_base;
    g_cxl_ce.bar0 = (volatile uint8_t *)map_base + bar_page_off;
    g_cxl_ce.bar_map_len = map_len;
    g_cxl_ce.slots = slots;
    g_cxl_ce.next_slot = 0;
    g_cxl_ce.chain_started = 0;
    g_cxl_ce.last_desc = NULL;
    g_cxl_ce.ready = 1;
    g_cxl_ce.attached_conns = 1;
    cxl_copyengine_sync_conn_state(conn);
    return 1;

fail:
    if (!g_cxl_ce.warned) {
        if (fail_errno != 0) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine init failed"
                    " stage=%s resource=%s map_len=%zu bar_page_off=%zu"
                    " slots=%zu errno=%d (%s)\n",
                    fail_stage,
                    resource ? resource : "(null)",
                    map_len, bar_page_off, slots,
                    fail_errno, strerror(fail_errno));
        } else {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine init failed"
                    " stage=%s resource=%s map_len=%zu bar_page_off=%zu"
                    " slots=%zu\n",
                    fail_stage,
                    resource ? resource : "(null)",
                    map_len, bar_page_off, slots);
        }
        g_cxl_ce.warned = 1;
    }
    cxl_copyengine_global_reset();
    cxl_copyengine_sync_conn_state(conn);
    return 0;
}

int
cxl_copyengine_prepare(cxl_connection_t *conn)
{
    if (!conn)
        return -1;

    cxl_copyengine_sync_conn_state(conn);
    return cxl_copyengine_init(conn) ? 0 : -1;
}

static void
cxl_copyengine_reset_slot_state(uint64_t ce_status)
{
    (void)ce_status;

    g_cxl_ce.next_slot = 0;
    g_cxl_ce.chain_started = 0;
    g_cxl_ce.last_desc = NULL;
}

static int
cxl_copyengine_try_recycle_slots(cxl_connection_t *conn)
{
    if (!conn || !g_cxl_ce.ready || !g_cxl_ce.bar0)
        return 0;

    uint32_t ce_err = cxl_ce_mmio_read32(g_cxl_ce.bar0,
                                         CXL_CE_CHAN_BASE +
                                         CXL_CE_CHAN_ERROR);
    if (ce_err != 0) {
        if (!g_cxl_ce.warned) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine channel error=0x%08x\n",
                    ce_err);
            g_cxl_ce.warned = 1;
        }
        cxl_copyengine_sync_conn_state(conn);
        return -1;
    }

    if (!g_cxl_ce.chain_started) {
        cxl_copyengine_sync_conn_state(conn);
        return 0;
    }

    uint64_t ce_status = cxl_ce_mmio_read64(g_cxl_ce.bar0,
                                            CXL_CE_CHAN_BASE +
                                            CXL_CE_CHAN_STATUS);
    if ((ce_status & 0x1ULL) == 0) {
        cxl_copyengine_sync_conn_state(conn);
        return 0;
    }

    cxl_copyengine_reset_slot_state(ce_status);
    cxl_copyengine_sync_conn_state(conn);
    return 0;
}

static size_t
cxl_copyengine_drain_spins(void)
{
    return CXL_CE_FIXED_DRAIN_SPINS;
}

static int
cxl_copyengine_refresh_chain_state(cxl_connection_t *conn,
                                   int allow_slot_rewind)
{
    if (!conn || !g_cxl_ce.ready || !g_cxl_ce.bar0)
        return 0;

    uint32_t ce_err = cxl_ce_mmio_read32(g_cxl_ce.bar0,
                                         CXL_CE_CHAN_BASE +
                                         CXL_CE_CHAN_ERROR);
    if (ce_err != 0) {
        if (!g_cxl_ce.warned) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine channel error=0x%08x\n",
                    ce_err);
            g_cxl_ce.warned = 1;
        }
        cxl_copyengine_sync_conn_state(conn);
        return -1;
    }

    if (!g_cxl_ce.chain_started) {
        cxl_copyengine_sync_conn_state(conn);
        return 0;
    }

    uint64_t ce_status = cxl_ce_mmio_read64(g_cxl_ce.bar0,
                                            CXL_CE_CHAN_BASE +
                                            CXL_CE_CHAN_STATUS);
    if ((ce_status & 0x1ULL) == 0) {
        cxl_copyengine_sync_conn_state(conn);
        return 0;
    }

    if (allow_slot_rewind) {
        cxl_copyengine_reset_slot_state(ce_status);
    } else {
        g_cxl_ce.chain_started = 0;
        g_cxl_ce.last_desc = NULL;
    }
    cxl_copyengine_sync_conn_state(conn);
    return 0;
}

static int
cxl_copyengine_wait_idle(cxl_connection_t *conn)
{
    if (!conn || !g_cxl_ce.ready || !g_cxl_ce.bar0 || !g_cxl_ce.chain_started)
        return 0;

    size_t spins = cxl_copyengine_drain_spins();
    for (size_t i = 0; i < spins; i++) {
        uint32_t ce_err = cxl_ce_mmio_read32(g_cxl_ce.bar0,
                                             CXL_CE_CHAN_BASE +
                                             CXL_CE_CHAN_ERROR);
        if (ce_err != 0) {
            if (!g_cxl_ce.warned) {
                fprintf(stderr,
                        "cxl_rpc: ERROR: CopyEngine channel error while draining=0x%08x\n",
                        ce_err);
                g_cxl_ce.warned = 1;
            }
            cxl_copyengine_sync_conn_state(conn);
            return -1;
        }

        uint64_t ce_status = cxl_ce_mmio_read64(g_cxl_ce.bar0,
                                                CXL_CE_CHAN_BASE +
                                                CXL_CE_CHAN_STATUS);
        if ((ce_status & 0x1ULL) != 0) {
            cxl_copyengine_reset_slot_state(ce_status);
            cxl_copyengine_sync_conn_state(conn);
            return 0;
        }
        __asm__ __volatile__("pause" ::: "memory");
    }

    fprintf(stderr,
            "cxl_rpc: ERROR: timed out waiting for CopyEngine idle (slots=%zu chain_started=%d)\n",
            g_cxl_ce.next_slot, g_cxl_ce.chain_started);
    cxl_copyengine_sync_conn_state(conn);
    return -1;
}

int
cxl_copyengine_submit_response_async(cxl_connection_t *conn,
                                     uint32_t request_id,
                                     const void *data,
                                     size_t len,
                                     const volatile void *dst_resp,
                                     size_t resp_transfer_size)
{
    if (!conn || !conn->ctx)
        return 0;
    if (!dst_resp)
        return 0;
    if (!cxl_copyengine_init(conn))
        return 0;
    if (!g_cxl_ce.ready || !g_cxl_ce.bar0)
        return 0;
    if (cxl_copyengine_try_recycle_slots(conn) != 0)
        return 0;

    if (resp_transfer_size > (size_t)CXL_CE_RESP_SLOT_BYTES) {
        if (!g_cxl_ce.warned) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: response entry %zu exceeds async slot %u\n",
                    resp_transfer_size, CXL_CE_RESP_SLOT_BYTES);
            g_cxl_ce.warned = 1;
        }
        cxl_copyengine_sync_conn_state(conn);
        return 0;
    }

    if (g_cxl_ce.next_slot >= g_cxl_ce.slots) {
        if (!g_cxl_ce.warned) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine descriptor pool exhausted"
                    " (slots=%zu) while channel still busy\n",
                    g_cxl_ce.slots);
            g_cxl_ce.warned = 1;
        }
        cxl_copyengine_sync_conn_state(conn);
        return 0;
    }

    const size_t slot = g_cxl_ce.next_slot++;
    uint8_t *src_resp =
        g_cxl_ce.resp_src_pool + slot * (size_t)CXL_CE_RESP_SLOT_BYTES;
    uint8_t *src_flag =
        g_cxl_ce.flag_src_pool + slot * (size_t)CXL_CE_FLAG_SLOT_BYTES;
    cxl_ce_desc_t *d0 = g_cxl_ce.desc_pool + slot * CXL_CE_DESC_PER_REQ;
    cxl_ce_desc_t *d1 = d0 + 1;

    struct __attribute__((packed)) {
        uint32_t payload_len;
        uint32_t request_id;
        uint32_t response_id;
    } hdr = {(uint32_t)len, request_id, request_id};

    memset(src_resp, 0, resp_transfer_size);
    memcpy(src_resp, &hdr, sizeof(hdr));
    if (len > 0)
        memcpy(src_resp + sizeof(hdr), data, len);
    memset(src_flag, 0, (size_t)CXL_CE_FLAG_SLOT_BYTES);
    memcpy(src_flag, &request_id, CXL_FLAG_PUBLISH_LEN);

    uint64_t src_resp_phys = 0, src_flag_phys = 0;
    uint64_t dst_resp_phys = 0, dst_flag_phys = 0;
    uint64_t d0_phys = 0, d1_phys = 0;
    if (!g_cxl_ce.resp_src_slot_phys || !g_cxl_ce.flag_src_slot_phys ||
        !g_cxl_ce.desc_phys) {
        if (!g_cxl_ce.warned) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine phys cache is not initialized\n");
            g_cxl_ce.warned = 1;
        }
        cxl_copyengine_sync_conn_state(conn);
        return 0;
    }

    src_resp_phys = g_cxl_ce.resp_src_slot_phys[slot];
    src_flag_phys = g_cxl_ce.flag_src_slot_phys[slot];
    const size_t d0_idx = slot * CXL_CE_DESC_PER_REQ;
    d0_phys = g_cxl_ce.desc_phys[d0_idx];
    d1_phys = g_cxl_ce.desc_phys[d0_idx + 1];

    if (!conn->ce_peer_resp_page_phys || conn->ce_peer_resp_page_count == 0) {
        if (!g_cxl_ce.warned) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine peer response map is not ready\n");
            g_cxl_ce.warned = 1;
        }
        cxl_copyengine_sync_conn_state(conn);
        return 0;
    }

    if (cxl_translate_with_page_phys_map(
            (const void *)dst_resp,
            conn->ce_peer_resp_virt_page_base,
            conn->ce_peer_resp_page_size,
            conn->ce_peer_resp_page_count,
            conn->ce_peer_resp_page_phys,
            &dst_resp_phys) != 0) {
        if (!g_cxl_ce.warned) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine dst_resp translation failed\n");
            g_cxl_ce.warned = 1;
        }
        cxl_copyengine_sync_conn_state(conn);
        return 0;
    }

    dst_flag_phys = conn->ce_peer_flag_phys;
    if (!conn->ce_peer_flag_phys_valid || dst_flag_phys == 0) {
        if (!g_cxl_ce.warned) {
            fprintf(stderr,
                    "cxl_rpc: ERROR: CopyEngine peer flag map is not ready\n");
            g_cxl_ce.warned = 1;
        }
        cxl_copyengine_sync_conn_state(conn);
        return 0;
    }

    memset(d0, 0, sizeof(*d0));
    memset(d1, 0, sizeof(*d1));
    d0->len = (uint32_t)resp_transfer_size;
    d0->command = 0;
    d0->src = src_resp_phys;
    d0->dest = dst_resp_phys;
    d0->next = d1_phys;

    d1->len = (uint32_t)CXL_CE_FLAG_SLOT_BYTES;
    d1->command = 0;
    d1->src = src_flag_phys;
    d1->dest = dst_flag_phys;
    d1->next = 0;

    cxl_ce_flush_range(src_resp, resp_transfer_size);
    cxl_ce_flush_range(src_flag, (size_t)CXL_CE_FLAG_SLOT_BYTES);
    cxl_ce_flush_range(d0, sizeof(*d0));
    cxl_ce_flush_range(d1, sizeof(*d1));
    __asm__ __volatile__("sfence" ::: "memory");

    if (cxl_copyengine_refresh_chain_state(conn, 0) != 0)
        return 0;

    if (!g_cxl_ce.chain_started) {
        cxl_ce_mmio_write64(g_cxl_ce.bar0,
                            CXL_CE_CHAN_BASE + CXL_CE_CHAN_CHAINADDR,
                            d0_phys);
        cxl_ce_mmio_write8(g_cxl_ce.bar0,
                           CXL_CE_CHAN_BASE + CXL_CE_CHAN_COMMAND,
                           CXL_CE_CMD_START_DMA);
        g_cxl_ce.chain_started = 1;
    } else {
        g_cxl_ce.last_desc->next = d0_phys;
        cxl_ce_flush_range(g_cxl_ce.last_desc, sizeof(*g_cxl_ce.last_desc));
        __asm__ __volatile__("sfence" ::: "memory");
        cxl_ce_mmio_write8(g_cxl_ce.bar0,
                           CXL_CE_CHAN_BASE + CXL_CE_CHAN_COMMAND,
                           CXL_CE_CMD_APPEND_DMA);
    }

    g_cxl_ce.last_desc = d1;
    cxl_copyengine_sync_conn_state(conn);
    return 1;
}
