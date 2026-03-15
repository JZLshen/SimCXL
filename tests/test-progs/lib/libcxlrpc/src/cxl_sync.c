/*
 * CXL RPC Head Synchronization - Implementation
 *
 * Fixed policy: sync once per N/4 processed entries.
 * HEAD_UPDATE is sent via the same software publish path as other stores:
 * store -> clflushopt(range) -> sfence.
 */

#include "cxl_sync.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ===== Doorbell method types ===== */

#define CXL_METHOD_HEAD_UPDATE  1
#define CXL_DOORBELL_PUBLISH_LEN 16

/* ===== Internal state ===== */

struct cxl_adaptive_sync {
    /* Fixed policy threshold: sync every N/4 processed entries. */
    uint32_t threshold;
    uint32_t entries_since_sync;

    /* Doorbell address for HEAD_UPDATE */
    volatile void *doorbell_addr;
};

static void
build_head_update_doorbell(uint8_t *buf, uint32_t head)
{
    uint64_t header = 0;
    uint64_t data = (uint64_t)head;

    if (!buf)
        return;

    header |= (uint64_t)CXL_METHOD_HEAD_UPDATE;
    header |= (uint64_t)1u << 1;  /* inline */
    memcpy(buf, &header, sizeof(header));
    memcpy(buf + sizeof(header), &data, sizeof(data));
}

/* Send HEAD_UPDATE with explicit store + clflushopt + sfence ordering. */
static void do_head_sync(cxl_adaptive_sync_t *s, uint32_t head)
{
    uint8_t buf[16] __attribute__((aligned(16)));
    build_head_update_doorbell(buf, head);
    memcpy((void *)s->doorbell_addr, buf, CXL_DOORBELL_PUBLISH_LEN);
    uintptr_t line_start = ((uintptr_t)s->doorbell_addr) & ~((uintptr_t)63);
    uintptr_t line_end =
        (((uintptr_t)s->doorbell_addr + CXL_DOORBELL_PUBLISH_LEN) + 63u) &
        ~((uintptr_t)63);
    for (uintptr_t line = line_start; line < line_end; line += 64u) {
        __asm__ __volatile__(
            "clflushopt (%0)"
            :
            : "r"((void *)line)
            : "memory");
    }
    __asm__ __volatile__("sfence" ::: "memory");

    s->entries_since_sync = 0;
}

/* ===== Public API ===== */

cxl_adaptive_sync_t *cxl_sync_init(const cxl_sync_config_t *config,
                                    volatile void *doorbell_addr)
{
    if (!config || !doorbell_addr)
        return NULL;

    cxl_adaptive_sync_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    uint32_t n = config->queue_size_n;

    /* Fixed N/4 policy, clamped to at least 1 entry. */
    s->threshold = (n / 4) > 0 ? (n / 4) : 1;

    s->doorbell_addr = doorbell_addr;

    return s;
}

void cxl_sync_destroy(cxl_adaptive_sync_t *sync)
{
    free(sync);
}

int cxl_sync_entry_processed(cxl_adaptive_sync_t *sync,
                              uint32_t current_head)
{
    if (!sync)
        return 0;

    sync->entries_since_sync++;

    if (sync->entries_since_sync >= sync->threshold) {
        do_head_sync(sync, current_head);
        return 1;
    }
    return 0;
}
