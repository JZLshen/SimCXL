/*
 * CXL RPC Timing Utilities
 *
 * RDTSC-based cycle-level timing for gem5 evaluation.
 * Provides latency measurement and statistical analysis.
 */

#ifndef CXL_TIMING_H
#define CXL_TIMING_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CPU frequency in MHz — must match gem5 board config.
 * Override at compile time with -DCXL_CPU_FREQ_MHZ=3000 if needed. */
#ifndef CXL_CPU_FREQ_MHZ
#define CXL_CPU_FREQ_MHZ 2400  /* 2.4 GHz, from x86-cxl-rpc-test.py */
#endif

/* ================================================================
 * RDTSC Timing Primitives
 * ================================================================ */

/**
 * Read Time-Stamp Counter (RDTSC).
 * Returns CPU cycles since boot.
 *
 * In gem5 TIMING CPU mode, this gives simulated cycle counts.
 */
static inline uint64_t rdtsc(void)
{
    /*
     * Use RDTSCP for stronger ordering than plain RDTSC in tight
     * store/flush micro-profiles. This reduces under-counting caused by
     * out-of-order timestamp sampling around memory instructions.
     */
    uint32_t lo, hi, aux;
    __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux) :: "memory");
    __asm__ __volatile__("lfence" ::: "memory");
    return ((uint64_t)hi << 32) | lo;
}

/**
 * Convert CPU cycles to nanoseconds.
 * Assumes 2.4 GHz CPU (from gem5 x86-cxl-rpc-test.py board config).
 *
 * @param cycles  CPU cycle count
 * @return        Nanoseconds
 */
static inline uint64_t cycles_to_ns(uint64_t cycles)
{
    return (cycles * 1000) / CXL_CPU_FREQ_MHZ;
}

/**
 * Convert nanoseconds to cycles (inverse of cycles_to_ns).
 *
 * @param ns  Nanoseconds
 * @return    CPU cycle count
 */
static inline uint64_t ns_to_cycles(uint64_t ns)
{
    return (ns * CXL_CPU_FREQ_MHZ) / 1000;
}

/* ================================================================
 * Latency Statistics
 * ================================================================ */

/**
 * Comparison function for qsort on uint64_t arrays.
 */
static inline int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t*)a;
    uint64_t y = *(const uint64_t*)b;

    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

/**
 * Compute latency statistics from a sorted array.
 *
 * @param latencies  Array of latency samples (will be sorted in-place)
 * @param count      Number of samples
 * @param min        Output: minimum latency
 * @param p50        Output: 50th percentile (median)
 * @param p90        Output: 90th percentile
 * @param p95        Output: 95th percentile
 * @param p99        Output: 99th percentile
 * @param max        Output: maximum latency
 */
static inline void compute_latency_stats(uint64_t *latencies, size_t count,
                                          uint64_t *min, uint64_t *p50,
                                          uint64_t *p90, uint64_t *p95,
                                          uint64_t *p99, uint64_t *max)
{
    if (!latencies || count == 0) {
        if (min) *min = 0;
        if (p50) *p50 = 0;
        if (p90) *p90 = 0;
        if (p95) *p95 = 0;
        if (p99) *p99 = 0;
        if (max) *max = 0;
        return;
    }

    /* Sort latencies in ascending order */
    qsort(latencies, count, sizeof(uint64_t), compare_u64);

    /* Extract percentiles */
    if (min) *min = latencies[0];
    if (p50) *p50 = latencies[count * 50 / 100];
    if (p90) *p90 = latencies[count * 90 / 100];
    if (p95) *p95 = latencies[count * 95 / 100];
    if (p99) *p99 = latencies[count * 99 / 100];
    if (max) *max = latencies[count - 1];
}

/**
 * Compute average latency.
 *
 * @param latencies  Array of latency samples
 * @param count      Number of samples
 * @return           Average latency (arithmetic mean)
 */
static inline uint64_t compute_avg_latency(const uint64_t *latencies, size_t count)
{
    if (!latencies || count == 0)
        return 0;

    uint64_t sum = 0;
    for (size_t i = 0; i < count; i++)
        sum += latencies[i];

    return sum / count;
}

/**
 * Compute throughput from total time and operation count.
 *
 * @param num_ops      Total number of operations
 * @param total_cycles Total elapsed cycles
 * @return             Throughput in operations per second
 */
static inline double compute_throughput(size_t num_ops, uint64_t total_cycles)
{
    if (total_cycles == 0)
        return 0.0;

    // Convert cycles to seconds
    double total_sec = (double)total_cycles / ((double)CXL_CPU_FREQ_MHZ * 1000000.0);

    return (double)num_ops / total_sec;
}

#ifdef __cplusplus
}
#endif

#endif /* CXL_TIMING_H */
