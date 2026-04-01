/*
 * CXL RPC Head Synchronization
 *
 * Server-side fixed policy for syncing metadata queue head to the
 * CXLRPCEngine controller via HEAD_UPDATE doorbell writes.
 * Default policy: sync every N/4 processed entries, with an optional runtime
 * threshold override.
 */

#ifndef CXL_SYNC_H
#define CXL_SYNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sync configuration */
typedef struct {
    uint32_t queue_size_n;          /* N: metadata queue size in entries */
    uint32_t threshold;             /* 0 uses the default N/4 policy */
} cxl_sync_config_t;

/* Opaque sync state */
typedef struct cxl_adaptive_sync cxl_adaptive_sync_t;

/**
 * Initialize sync tracker.
 *
 * @param config        Sync configuration (queue size is required)
 * @param doorbell_addr Virtual address of doorbell for HEAD_UPDATE writes
 * @return              Allocated sync state, or NULL on failure
 */
cxl_adaptive_sync_t *cxl_sync_init(const cxl_sync_config_t *config,
                                    volatile void *doorbell_addr);

/**
 * Override the current HEAD_UPDATE threshold.
 *
 * `threshold = 0` resets the policy back to the default `N/4` value derived
 * from `queue_size_n`.
 */
void cxl_sync_set_threshold(cxl_adaptive_sync_t *sync, uint32_t threshold);

/**
 * Read the current HEAD_UPDATE threshold.
 */
uint32_t cxl_sync_get_threshold(const cxl_adaptive_sync_t *sync);

/**
 * Destroy adaptive sync tracker and free memory.
 */
void cxl_sync_destroy(cxl_adaptive_sync_t *sync);

/**
 * Notify that an entry has been processed.
 *
 * Increments the entries-since-last-sync counter. If the counter
 * reaches the current threshold, performs
 * a HEAD_UPDATE sync via the shared store publish path.
 *
 * @param sync         Sync state
 * @param current_head Current head position to sync
 * @return             1 if sync was performed, 0 otherwise
 */
int cxl_sync_entry_processed(cxl_adaptive_sync_t *sync,
                              uint32_t current_head);

#ifdef __cplusplus
}
#endif

#endif /* CXL_SYNC_H */
