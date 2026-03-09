/*
 * CXL RPC Global Memory Allocator - Implementation
 *
 * First-fit free list with address-ordered coalescing.
 * - Free blocks maintained in a sorted linked list by address
 * - Allocation: first-fit search, split remainder
 * - Deallocation: insert at correct position, merge adjacent blocks
 */

#include "cxl_alloc.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ================================================================
 * Tier 1: Global Allocator
 * ================================================================ */

/* Free block node (metadata stored in host heap, not in CXL memory) */
struct free_block {
    uint64_t addr;          /* Physical address in CXL space */
    size_t   size;          /* Size in bytes (64B aligned) */
    struct free_block *next;
};

/* Allocation record for tracking allocated region sizes */
struct allocation_record {
    uint64_t addr;          /* Physical address in CXL space */
    size_t   size;          /* Allocated size in bytes */
    struct allocation_record *next;
};

struct cxl_global_allocator {
    uint64_t base_addr;
    size_t   total_size;
    struct free_block *free_list;   /* Address-sorted free list */
    size_t   allocated_bytes;
    uint32_t num_regions;
    struct allocation_record *alloc_table;  /* Tracks all allocations */
    pthread_mutex_t lock;                   /* Protects all shared state */
};

/* Round up to 64B alignment */
static inline size_t align_up_64(size_t size)
{
    return (size + 63) & ~(size_t)63;
}

static size_t
cxl_connection_span_align(size_t size)
{
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        return align_up_64(size);

    size_t page = (size_t)page_size;
    return (size + page - 1u) & ~(page - 1u);
}

cxl_global_alloc_t *cxl_global_alloc_init(uint64_t base_addr,
                                           size_t total_size)
{
    cxl_global_alloc_t *alloc = calloc(1, sizeof(*alloc));
    if (!alloc)
        return NULL;

    alloc->base_addr = base_addr;
    alloc->total_size = total_size;

    /* Create initial free block spanning entire region */
    struct free_block *block = malloc(sizeof(*block));
    if (!block) {
        free(alloc);
        return NULL;
    }
    block->addr = base_addr;
    block->size = total_size;
    block->next = NULL;
    alloc->free_list = block;

    /* Initialize mutex for thread safety */
    if (pthread_mutex_init(&alloc->lock, NULL) != 0) {
        free(block);
        free(alloc);
        return NULL;
    }

    return alloc;
}

void cxl_global_alloc_destroy(cxl_global_alloc_t *alloc)
{
    if (!alloc)
        return;

    /* Free all free-list nodes */
    struct free_block *cur = alloc->free_list;
    while (cur) {
        struct free_block *next = cur->next;
        free(cur);
        cur = next;
    }

    /* Free allocation tracking table */
    struct allocation_record *rec = alloc->alloc_table;
    while (rec) {
        struct allocation_record *next = rec->next;
        free(rec);
        rec = next;
    }

    /* Destroy mutex */
    pthread_mutex_destroy(&alloc->lock);

    free(alloc);
}

/* Forward declarations */
static void cxl_global_free_region_internal(cxl_global_alloc_t *alloc,
                                             uint64_t addr, size_t size);
static uint64_t cxl_global_alloc_region_internal(cxl_global_alloc_t *alloc,
                                                   size_t size);
static int cxl_global_take_alloc_record_internal(cxl_global_alloc_t *alloc,
                                                  uint64_t addr,
                                                  size_t *size_out);

/*
 * Internal helper: Allocate a region (no locking, caller must hold lock)
 */
static uint64_t cxl_global_alloc_region_internal(cxl_global_alloc_t *alloc,
                                                   size_t size)
{
    if (!alloc || size == 0)
        return 0;

    size = align_up_64(size);

    /* First-fit search */
    struct free_block *prev = NULL;
    struct free_block *cur = alloc->free_list;

    while (cur) {
        if (cur->size >= size) {
            /* Allocation must be trackable for later free(). If record allocation
             * fails, treat as allocation failure and keep allocator state intact. */
            struct allocation_record *rec = malloc(sizeof(*rec));
            if (!rec)
                return 0;

            uint64_t addr = cur->addr;

            if (cur->size == size) {
                /* Exact fit: remove block */
                if (prev)
                    prev->next = cur->next;
                else
                    alloc->free_list = cur->next;
                free(cur);
            } else {
                /* Split: shrink current block */
                cur->addr += size;
                cur->size -= size;
            }

            rec->addr = addr;
            rec->size = size;
            rec->next = alloc->alloc_table;
            alloc->alloc_table = rec;

            alloc->allocated_bytes += size;
            alloc->num_regions++;

            return addr;
        }
        prev = cur;
        cur = cur->next;
    }

    return 0;  /* Out of memory */
}

/*
 * Internal helper: remove allocation record and return its size.
 * Returns 0 on success, -1 if addr was not found.
 */
static int cxl_global_take_alloc_record_internal(cxl_global_alloc_t *alloc,
                                                  uint64_t addr,
                                                  size_t *size_out)
{
    struct allocation_record **prev_ptr = &alloc->alloc_table;
    struct allocation_record *rec = alloc->alloc_table;

    while (rec) {
        if (rec->addr == addr) {
            if (size_out)
                *size_out = rec->size;
            *prev_ptr = rec->next;
            free(rec);
            return 0;
        }
        prev_ptr = &rec->next;
        rec = rec->next;
    }

    return -1;
}

/* Internal helper: free a region with known size (caller holds lock). */
static void cxl_global_free_region_internal(cxl_global_alloc_t *alloc,
                                             uint64_t addr, size_t size)
{
    size = align_up_64(size);

    /* Find insertion point (sorted by address) */
    struct free_block *prev = NULL;
    struct free_block *cur = alloc->free_list;

    while (cur && cur->addr < addr) {
        prev = cur;
        cur = cur->next;
    }

    /* Create new free block */
    struct free_block *block = malloc(sizeof(*block));
    if (!block)
        return;  /* Leak on malloc failure, acceptable for simulator */

    block->addr = addr;
    block->size = size;
    block->next = cur;

    if (prev)
        prev->next = block;
    else
        alloc->free_list = block;

    /* Coalesce with next block */
    if (block->next && block->addr + block->size == block->next->addr) {
        struct free_block *next = block->next;
        block->size += next->size;
        block->next = next->next;
        free(next);
    }

    /* Coalesce with previous block */
    if (prev && prev->addr + prev->size == block->addr) {
        prev->size += block->size;
        prev->next = block->next;
        free(block);
    }

    if (alloc->allocated_bytes >= size)
        alloc->allocated_bytes -= size;
    else
        alloc->allocated_bytes = 0;
    if (alloc->num_regions > 0)
        alloc->num_regions--;
}

int cxl_global_alloc_connection(cxl_global_alloc_t *alloc,
                                 size_t mq_size,
                                 size_t req_size,
                                 size_t resp_size,
                                 cxl_connection_addrs_t *out)
{
    if (!alloc || !out)
        return -1;

    /* Use defaults if 0 */
    if (mq_size == 0)   mq_size   = CXL_DEFAULT_METADATA_Q_SIZE;
    if (req_size == 0)   req_size  = CXL_DEFAULT_REQUEST_DATA_SIZE;
    if (resp_size == 0)  resp_size = CXL_DEFAULT_RESPONSE_DATA_SIZE;

    memset(out, 0, sizeof(*out));

    /*
     * Allocate one contiguous per-connection block, then carve logical
     * sub-regions using fixed offsets. This keeps registration simple for
     * controller/client/server while preserving dynamic base assignment.
     */
    const size_t doorbell_off = CXL_CONN_OFF_DOORBELL;
    const size_t mq_off = CXL_CONN_OFF_METADATA_QUEUE;
    const size_t req_off = CXL_CONN_OFF_REQUEST_DATA;
    const size_t resp_off = align_up_64(req_off + req_size);
    const size_t flag_off = align_up_64(resp_off + resp_size);
    const size_t conn_span =
        cxl_connection_span_align(flag_off + CXL_DEFAULT_FLAG_SIZE);

    if (mq_off < doorbell_off + CXL_DEFAULT_DOORBELL_SIZE)
        return -1;
    if (req_off < mq_off + mq_size)
        return -1;

    pthread_mutex_lock(&alloc->lock);
    uint64_t base_addr = cxl_global_alloc_region_internal(
        alloc, conn_span);
    if (!base_addr) {
        pthread_mutex_unlock(&alloc->lock);
        memset(out, 0, sizeof(*out));
        return -1;
    }

    out->alloc_base_addr = base_addr;
    out->alloc_size = conn_span;
    out->doorbell_addr = base_addr + doorbell_off;
    out->metadata_queue_addr = base_addr + mq_off;
    out->request_data_addr = base_addr + req_off;
    out->response_data_addr = base_addr + resp_off;
    out->flag_addr = base_addr + flag_off;
    out->metadata_queue_size = mq_size;
    out->request_data_size = req_size;
    out->response_data_size = resp_size;

    pthread_mutex_unlock(&alloc->lock);
    return 0;
}

void cxl_global_free_connection(cxl_global_alloc_t *alloc,
                                 const cxl_connection_addrs_t *addrs)
{
    if (!alloc || !addrs)
        return;

    pthread_mutex_lock(&alloc->lock);

    size_t size = 0;

    if (!(addrs->alloc_base_addr && addrs->alloc_size > 0)) {
        pthread_mutex_unlock(&alloc->lock);
        return;
    }
    if (cxl_global_take_alloc_record_internal(alloc, addrs->alloc_base_addr,
                                              &size) != 0) {
        pthread_mutex_unlock(&alloc->lock);
        return;
    }
    cxl_global_free_region_internal(alloc, addrs->alloc_base_addr, size);

    pthread_mutex_unlock(&alloc->lock);
}
