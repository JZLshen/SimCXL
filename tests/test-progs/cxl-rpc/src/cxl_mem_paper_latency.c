/*
 * CXL "paper-style" random-read latency microbenchmark.
 *
 * This benchmark approximates LMbench lat_mem_rd semantics with a dependent
 * random-load chain over a large region (64B stride). It reports both CXL
 * mapped memory and local malloc memory for side-by-side comparison.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cxl_rpc.h"
#include "cxl_timing.h"

#define CXL_BASE_ADDR         0x100000000ULL
#define CXL_MAP_SIZE_DEFAULT  0x20000000ULL   /* 512MB */
#define CXL_OFFSET_DEFAULT    0x01000000ULL   /* 16MB */
#define STRIDE_BYTES          64ULL
#define DEFAULT_ARRAY_MB      64
#define DEFAULT_ITERS         4000
#define DEFAULT_UNROLL        8
#define DEFAULT_CPU           1
#define SEED_WRITE_LINES      4096U

static volatile uint64_t g_sink = 0;

static inline uint64_t
rdtsc_begin(void)
{
    uint32_t lo = 0, hi = 0;
    __asm__ __volatile__(
        "lfence\n\t"
        "rdtsc\n\t"
        : "=a"(lo), "=d"(hi)
        :
        : "memory");
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t
rdtsc_end(void)
{
    uint32_t lo = 0, hi = 0;
    __asm__ __volatile__(
        "rdtscp\n\t"
        "lfence\n\t"
        : "=a"(lo), "=d"(hi)
        :
        : "rcx", "memory");
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t
xorshift64(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static int
parse_int_arg(int argc, char **argv, const char *flag, int default_value,
              int min_value, int max_value)
{
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0) {
            char *end = NULL;
            long v = strtol(argv[i + 1], &end, 0);
            if (end && *end == '\0' && v >= min_value && v <= max_value)
                return (int)v;
            return default_value;
        }
    }
    return default_value;
}

static int
has_flag(int argc, char **argv, const char *flag)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0)
            return 1;
    }
    return 0;
}

static uint64_t
floor_pow2_u64(uint64_t x)
{
    uint64_t p = 1;
    while ((p << 1) <= x) {
        p <<= 1;
    }
    return p;
}

static void
seed_lines(volatile uint8_t *base, uint64_t line_mask, uint64_t seed)
{
    uint32_t lines = SEED_WRITE_LINES;
    uint64_t max_lines = line_mask + 1;
    if (lines > max_lines) {
        lines = (uint32_t)max_lines;
    }
    for (uint32_t i = 0; i < lines; i++) {
        uint64_t idx = (xorshift64(&seed) & line_mask);
        volatile uint64_t *slot =
            (volatile uint64_t *)(base + idx * STRIDE_BYTES);
        *slot = xorshift64(&seed);
    }
}

static void
measure_dependent_random_reads(volatile uint8_t *base,
                               uint64_t line_mask,
                               int iters,
                               int unroll,
                               uint64_t *samples,
                               uint64_t seed)
{
    const uint64_t mul = 11400714819323198485ULL;   /* odd constant */
    const uint64_t add = 0x9E3779B97F4A7C15ULL;
    uint64_t idx = seed & line_mask;

    for (int i = 0; i < iters; i++) {
        uint64_t t0 = rdtsc_begin();
        for (int u = 0; u < unroll; u++) {
            volatile uint64_t *slot =
                (volatile uint64_t *)(base + idx * STRIDE_BYTES);
            uint64_t v = *slot;
            idx = (idx * mul + v + add) & line_mask;
        }
        uint64_t t1 = rdtsc_end();
        samples[i] = (t1 - t0) / (uint64_t)unroll;
    }

    g_sink ^= idx;
}

static void
print_stats(const char *name, uint64_t *samples, int count)
{
    uint64_t min = 0, p50 = 0, p90 = 0, p95 = 0, p99 = 0, max = 0;
    uint64_t avg = compute_avg_latency(samples, (size_t)count);
    compute_latency_stats(samples, (size_t)count, &min, &p50, &p90, &p95, &p99,
                          &max);

    printf("%s (cycles): min=%" PRIu64 " avg=%" PRIu64
           " p50=%" PRIu64 " p90=%" PRIu64 " p95=%" PRIu64
           " p99=%" PRIu64 " max=%" PRIu64 "\n",
           name, min, avg, p50, p90, p95, p99, max);
    printf("%s (ns):     min=%" PRIu64 " avg=%" PRIu64
           " p50=%" PRIu64 " p90=%" PRIu64 " p95=%" PRIu64
           " p99=%" PRIu64 " max=%" PRIu64 "\n",
           name, cycles_to_ns(min), cycles_to_ns(avg), cycles_to_ns(p50),
           cycles_to_ns(p90), cycles_to_ns(p95), cycles_to_ns(p99),
           cycles_to_ns(max));
}

int
main(int argc, char **argv)
{
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: cxl_mem_paper_latency [options]\n");
        printf("  --array-mb <8..384>   Array size in MB (default: %d)\n",
               DEFAULT_ARRAY_MB);
        printf("  --iters <1..1000000>  Number of timed samples "
               "(default: %d)\n", DEFAULT_ITERS);
        printf("  --unroll <1..64>      Dependent loads per sample "
               "(default: %d)\n", DEFAULT_UNROLL);
        printf("  --cpu <0..4095>       CPU affinity id (default: %d)\n",
               DEFAULT_CPU);
        return 0;
    }

    int array_mb = parse_int_arg(argc, argv, "--array-mb", DEFAULT_ARRAY_MB, 8,
                                 384);
    int iters = parse_int_arg(argc, argv, "--iters", DEFAULT_ITERS, 1,
                              1000000);
    int unroll = parse_int_arg(argc, argv, "--unroll", DEFAULT_UNROLL, 1, 64);
    int cpu = parse_int_arg(argc, argv, "--cpu", DEFAULT_CPU, 0, 4095);

    size_t array_bytes = (size_t)array_mb * 1024ULL * 1024ULL;
    if (array_bytes < STRIDE_BYTES) {
        fprintf(stderr, "Error: array size too small\n");
        return 1;
    }
    array_bytes = (array_bytes / STRIDE_BYTES) * STRIDE_BYTES;

    if (CXL_OFFSET_DEFAULT + array_bytes > CXL_MAP_SIZE_DEFAULT) {
        fprintf(stderr,
                "Error: array exceeds CXL mapped range "
                "(offset=0x%llx array=0x%llx map=0x%llx)\n",
                (unsigned long long)CXL_OFFSET_DEFAULT,
                (unsigned long long)array_bytes,
                (unsigned long long)CXL_MAP_SIZE_DEFAULT);
        return 1;
    }

    uint64_t line_count = array_bytes / STRIDE_BYTES;
    uint64_t effective_lines = floor_pow2_u64(line_count);
    if (effective_lines < 2) {
        fprintf(stderr, "Error: effective line count too small\n");
        return 1;
    }
    uint64_t line_mask = effective_lines - 1;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
        fprintf(stderr, "warn: sched_setaffinity cpu=%d failed: %s\n",
                cpu, strerror(errno));
    }

    cxl_context_t *ctx = cxl_rpc_init(CXL_BASE_ADDR, CXL_MAP_SIZE_DEFAULT);
    if (!ctx) {
        fprintf(stderr, "Error: cxl_rpc_init failed\n");
        return 1;
    }
    volatile uint8_t *cxl_base = (volatile uint8_t *)cxl_rpc_get_base(ctx);
    volatile uint8_t *cxl_region = cxl_base + CXL_OFFSET_DEFAULT;

    void *local_mem_raw = NULL;
    if (posix_memalign(&local_mem_raw, STRIDE_BYTES, array_bytes) != 0 ||
        !local_mem_raw) {
        fprintf(stderr, "Error: posix_memalign failed\n");
        cxl_rpc_destroy(ctx);
        return 1;
    }
    volatile uint8_t *local_region = (volatile uint8_t *)local_mem_raw;

    uint64_t *samples_cxl = (uint64_t *)malloc((size_t)iters * sizeof(uint64_t));
    uint64_t *samples_local = (uint64_t *)malloc((size_t)iters * sizeof(uint64_t));
    if (!samples_cxl || !samples_local) {
        fprintf(stderr, "Error: malloc failed\n");
        free(samples_cxl);
        free(samples_local);
        free(local_mem_raw);
        cxl_rpc_destroy(ctx);
        return 1;
    }

    /* Light-weight seeding to avoid degenerate all-zero access patterns. */
    seed_lines(cxl_region, line_mask, 0x123456789ABCDEFFULL);
    seed_lines(local_region, line_mask, 0xFEDCBA9876543211ULL);

    /* Warm up a little so first-touch noise does not dominate. */
    uint64_t warmup_seed = 0x9E3779B97F4A7C15ULL;
    uint64_t warmup_idx = warmup_seed & line_mask;
    for (int i = 0; i < 2048; i++) {
        volatile uint64_t *slot_cxl =
            (volatile uint64_t *)(cxl_region + warmup_idx * STRIDE_BYTES);
        volatile uint64_t *slot_local =
            (volatile uint64_t *)(local_region + warmup_idx * STRIDE_BYTES);
        warmup_idx = (warmup_idx * 11400714819323198485ULL +
                      *slot_cxl + *slot_local + 1ULL) & line_mask;
    }

    measure_dependent_random_reads(cxl_region, line_mask, iters, unroll,
                                   samples_cxl, 0xCAFEBABE11223344ULL);
    measure_dependent_random_reads(local_region, line_mask, iters, unroll,
                                   samples_local, 0x445566778899AABBULL);

    printf("=== CXL Paper-Style Dependent Random-Read Latency ===\n");
    printf("Config: array_mb=%d iters=%d unroll=%d stride=%llu cpu=%d\n",
           array_mb, iters, unroll, (unsigned long long)STRIDE_BYTES, cpu);
    printf("Region: line_count=%llu effective_lines=%llu (pow2)\n",
           (unsigned long long)line_count,
           (unsigned long long)effective_lines);
    printf("\n");

    print_stats("dependent_random_read_cxl", samples_cxl, iters);
    print_stats("dependent_random_read_local", samples_local, iters);

    uint64_t avg_cxl = compute_avg_latency(samples_cxl, (size_t)iters);
    uint64_t avg_local = compute_avg_latency(samples_local, (size_t)iters);
    uint64_t avg_cxl_ns = cycles_to_ns(avg_cxl);
    uint64_t avg_local_ns = cycles_to_ns(avg_local);
    uint64_t extra_ns = (avg_cxl_ns > avg_local_ns) ?
                        (avg_cxl_ns - avg_local_ns) : 0;
    uint64_t ratio_x1000 = avg_local_ns ?
                           (avg_cxl_ns * 1000ULL) / avg_local_ns : 0;

    printf("\n=== Summary (parseable) ===\n");
    printf("paper_array_mb=%d\n", array_mb);
    printf("paper_iters=%d\n", iters);
    printf("paper_unroll=%d\n", unroll);
    printf("paper_stride_bytes=%llu\n", (unsigned long long)STRIDE_BYTES);
    printf("paper_cxl_avg_cycles=%" PRIu64 "\n", avg_cxl);
    printf("paper_cxl_avg_ns=%" PRIu64 "\n", avg_cxl_ns);
    printf("paper_local_avg_cycles=%" PRIu64 "\n", avg_local);
    printf("paper_local_avg_ns=%" PRIu64 "\n", avg_local_ns);
    printf("paper_cxl_extra_vs_local_ns=%" PRIu64 "\n", extra_ns);
    printf("paper_cxl_vs_local_ratio_x1000=%" PRIu64 "\n", ratio_x1000);
    printf("paper_sink=%" PRIu64 "\n", g_sink);

    free(samples_cxl);
    free(samples_local);
    free(local_mem_raw);
    cxl_rpc_destroy(ctx);
    return 0;
}
