/*
 * CXL Shared Memory Load/Store Latency Microbenchmark
 *
 * Measures:
 *   1) raw store/load latency without flush/fence in the timed window
 *   2) one-way store latency (store + clflushopt + sfence)
 *   3) one-way load latency (clflush + load)
 *   4) a store->load roundtrip.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <sys/resource.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cxl_rpc.h"
#include "cxl_timing.h"

#define CXL_BASE_ADDR      0x100000000ULL
#define CXL_MAP_SIZE       0x10000000ULL   /* 256MB */
#define DEFAULT_OFFSET     0x00F00000ULL
#define DEFAULT_SPAN       0x00800000ULL   /* 8MB working set */
#define DEFAULT_ITERS      10000
#define DEFAULT_CPU        1
#define CACHELINE_SIZE     64ULL

typedef enum {
    MODE_ALL = 0,
    MODE_RAW_STORE,
    MODE_RAW_LOAD,
    MODE_STORE_FLUSH,
    MODE_LOAD_AFTER_CLFLUSH,
    MODE_ROUNDTRIP,
} bench_mode_t;

static inline void
store_sfence(void)
{
    __asm__ __volatile__("sfence" ::: "memory");
}

static inline void
flushopt_line(const volatile void *addr)
{
    __asm__ __volatile__("clflushopt (%0)" :: "r"(addr) : "memory");
}

static inline void
flush_line(const volatile void *addr)
{
    __asm__ __volatile__("clflush (%0)" :: "r"(addr) : "memory");
}

static inline uint64_t
rdtsc_begin(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("lfence\n\t"
                         "rdtsc\n\t"
                         : "=a"(lo), "=d"(hi)
                         :
                         : "memory");
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t
rdtsc_end(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtscp\n\t"
                         "lfence\n\t"
                         : "=a"(lo), "=d"(hi)
                         :
                         : "rcx", "memory");
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t
mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static int
parse_int_arg_range(int argc, char **argv, const char *flag, int default_value,
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

static const char *
mode_to_str(bench_mode_t mode)
{
    switch (mode) {
      case MODE_RAW_STORE:
        return "raw_store";
      case MODE_RAW_LOAD:
        return "raw_load";
      case MODE_STORE_FLUSH:
        return "store_flush";
      case MODE_LOAD_AFTER_CLFLUSH:
        return "load_after_clflush";
      case MODE_ROUNDTRIP:
        return "roundtrip";
      case MODE_ALL:
      default:
        return "all";
    }
}

static bench_mode_t
parse_mode_arg(int argc, char **argv, bench_mode_t default_mode)
{
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--mode") == 0) {
            const char *v = argv[i + 1];
            if (strcmp(v, "all") == 0)
                return MODE_ALL;
            if (strcmp(v, "raw_store") == 0)
                return MODE_RAW_STORE;
            if (strcmp(v, "raw_load") == 0)
                return MODE_RAW_LOAD;
            if (strcmp(v, "store_flush") == 0)
                return MODE_STORE_FLUSH;
            if (strcmp(v, "load_after_clflush") == 0)
                return MODE_LOAD_AFTER_CLFLUSH;
            if (strcmp(v, "roundtrip") == 0)
                return MODE_ROUNDTRIP;
            return default_mode;
        }
    }
    return default_mode;
}

static uint64_t
parse_u64_arg(int argc, char **argv, const char *flag, uint64_t default_value)
{
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0) {
            char *end = NULL;
            unsigned long long v = strtoull(argv[i + 1], &end, 0);
            if (end && *end == '\0')
                return (uint64_t)v;
            return default_value;
        }
    }
    return default_value;
}

static void
print_usage(const char *prog)
{
    printf("Usage: %s [--iters N] [--offset BYTES] [--span BYTES] [--cpu ID] [--mode MODE] [--no-warmup]\n",
           prog);
    printf("  --iters   Number of samples (default: %d)\n", DEFAULT_ITERS);
    printf("  --offset  Offset within CXL map (default: 0x%llx)\n",
           (unsigned long long)DEFAULT_OFFSET);
    printf("  --span    Working-set span for random lines (default: 0x%llx)\n",
           (unsigned long long)DEFAULT_SPAN);
    printf("  --cpu     CPU affinity (default: %d)\n", DEFAULT_CPU);
    printf("  --mode    all|raw_store|raw_load|store_flush|load_after_clflush|roundtrip\n");
    printf("  --no-warmup  Disable startup warm-up touch loop\n");
    printf("  --fault-probe  Print first-sample minor/major fault deltas (diagnostic)\n");
}

static void
report_stats(const char *name, uint64_t *samples, int iters)
{
    uint64_t min = 0, p50 = 0, p90 = 0, p95 = 0, p99 = 0, max = 0;
    uint64_t avg = compute_avg_latency(samples, (size_t)iters);
    compute_latency_stats(samples, (size_t)iters, &min, &p50, &p90, &p95, &p99,
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
    if (argc > 1 &&
        (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        print_usage(argv[0]);
        return 0;
    }

    int iters = parse_int_arg_range(argc, argv, "--iters", DEFAULT_ITERS, 1,
                                    1000000000);
    int cpu = parse_int_arg_range(argc, argv, "--cpu", DEFAULT_CPU, 0, 4095);
    uint64_t offset = parse_u64_arg(argc, argv, "--offset", DEFAULT_OFFSET);
    uint64_t span = parse_u64_arg(argc, argv, "--span", DEFAULT_SPAN);
    bench_mode_t mode = parse_mode_arg(argc, argv, MODE_ALL);
    int no_warmup = has_flag(argc, argv, "--no-warmup");
    int fault_probe = has_flag(argc, argv, "--fault-probe");
    int do_raw_store = (mode == MODE_ALL || mode == MODE_RAW_STORE);
    int do_raw_load = (mode == MODE_ALL || mode == MODE_RAW_LOAD);
    int do_store_flush = (mode == MODE_ALL || mode == MODE_STORE_FLUSH);
    int do_load_after_clflush =
        (mode == MODE_ALL || mode == MODE_LOAD_AFTER_CLFLUSH);
    int do_roundtrip = (mode == MODE_ALL || mode == MODE_ROUNDTRIP);

    if (span < CACHELINE_SIZE || (span % CACHELINE_SIZE) != 0) {
        fprintf(stderr, "Error: span must be >=64 and 64B-aligned (span=0x%llx)\n",
                (unsigned long long)span);
        return 1;
    }

    if (offset + span > CXL_MAP_SIZE) {
        fprintf(stderr,
                "Error: offset+span out of CXL map range "
                "(offset=0x%llx span=0x%llx)\n",
                (unsigned long long)offset, (unsigned long long)span);
        return 1;
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
        fprintf(stderr, "Error: cxl_rpc_init failed\n");
        return 1;
    }

    volatile uint8_t *base = (volatile uint8_t *)cxl_rpc_get_base(ctx);
    size_t line_count = (size_t)(span / CACHELINE_SIZE);

    uint64_t *store_cycles = (uint64_t *)malloc((size_t)iters * sizeof(uint64_t));
    uint64_t *load_cycles = (uint64_t *)malloc((size_t)iters * sizeof(uint64_t));
    uint64_t *raw_store_cycles =
        (uint64_t *)malloc((size_t)iters * sizeof(uint64_t));
    uint64_t *raw_load_cycles =
        (uint64_t *)malloc((size_t)iters * sizeof(uint64_t));
    uint64_t *roundtrip_cycles =
        (uint64_t *)malloc((size_t)iters * sizeof(uint64_t));
    if (!store_cycles || !load_cycles || !raw_store_cycles || !raw_load_cycles ||
        !roundtrip_cycles) {
        fprintf(stderr, "Error: malloc failed\n");
        free(store_cycles);
        free(load_cycles);
        free(raw_store_cycles);
        free(raw_load_cycles);
        free(roundtrip_cycles);
        cxl_rpc_destroy(ctx);
        return 1;
    }

    printf("=== CXL Shared Memory Load/Store Latency ===\n");
    printf("Config: iters=%d cpu=%d offset=0x%llx span=0x%llx lines=%zu addr=0x%llx\n",
           iters, cpu, (unsigned long long)offset,
           (unsigned long long)span, line_count,
           (unsigned long long)(CXL_BASE_ADDR + offset));
    printf("Config: mode=%s\n", mode_to_str(mode));
    printf("Config: warmup=%s\n", no_warmup ? "off" : "on");

    size_t warm_lines = 0;
    if (!no_warmup) {
        /* Warm-up random lines to reduce first-touch noise. */
        warm_lines = line_count < 4096 ? line_count : 4096;
        for (size_t i = 0; i < warm_lines; i++) {
            size_t idx = (size_t)(mix64(i) % line_count);
            volatile uint64_t *ptr =
                (volatile uint64_t *)(base + offset + idx * CACHELINE_SIZE);
            *ptr = (uint64_t)i;
            flushopt_line(ptr);
            store_sfence();
            flush_line(ptr);
            (void)(*ptr);
        }
    }

    uint64_t seed = 0x1234567800000000ULL;
    uint64_t mismatches = 0;
    volatile uint64_t sink = 0;
    long first_raw_store_minflt = -1, first_raw_store_majflt = -1;
    long first_raw_load_minflt = -1, first_raw_load_majflt = -1;
    long first_store_flush_minflt = -1, first_store_flush_majflt = -1;
    long first_load_clflush_minflt = -1, first_load_clflush_majflt = -1;
    long first_roundtrip_minflt = -1, first_roundtrip_majflt = -1;

    for (int i = 0; i < iters; i++) {
        seed += 0x9E3779B97F4A7C15ULL;
        uint64_t v = seed ^ ((uint64_t)i << 17);
        size_t idx_store = (size_t)(mix64(seed ^ 0xA5A5A5A5A5A5A5A5ULL) %
                                    line_count);
        size_t idx_load = (size_t)(mix64(seed ^ 0x5A5A5A5A5A5A5A5AULL) %
                                   line_count);
        size_t idx_rt = (size_t)(mix64(seed ^ 0xDEADBEEF12345678ULL) %
                                 line_count);
        volatile uint64_t *ptr_store =
            (volatile uint64_t *)(base + offset + idx_store * CACHELINE_SIZE);
        volatile uint64_t *ptr_load =
            (volatile uint64_t *)(base + offset + idx_load * CACHELINE_SIZE);
        volatile uint64_t *ptr_rt =
            (volatile uint64_t *)(base + offset + idx_rt * CACHELINE_SIZE);

        if (do_raw_store) {
            /* Raw store only (no flush/fence in measured region). */
            struct rusage ru0, ru1;
            if (fault_probe && i == 0)
                getrusage(RUSAGE_SELF, &ru0);
            uint64_t tr0 = rdtsc_begin();
            *ptr_store = v ^ 0x0102030405060708ULL;
            uint64_t tr1 = rdtsc_end();
            if (fault_probe && i == 0) {
                getrusage(RUSAGE_SELF, &ru1);
                first_raw_store_minflt = ru1.ru_minflt - ru0.ru_minflt;
                first_raw_store_majflt = ru1.ru_majflt - ru0.ru_majflt;
            }
            raw_store_cycles[i] = tr1 - tr0;
        }

        if (do_raw_load) {
            volatile uint64_t *ptr_raw_load = do_raw_store ? ptr_store : ptr_load;
            struct rusage ru0, ru1;
            if (fault_probe && i == 0)
                getrusage(RUSAGE_SELF, &ru0);
            uint64_t tr2 = rdtsc_begin();
            uint64_t raw_got = *ptr_raw_load;
            uint64_t tr3 = rdtsc_end();
            if (fault_probe && i == 0) {
                getrusage(RUSAGE_SELF, &ru1);
                first_raw_load_minflt = ru1.ru_minflt - ru0.ru_minflt;
                first_raw_load_majflt = ru1.ru_majflt - ru0.ru_majflt;
            }
            raw_load_cycles[i] = tr3 - tr2;
            sink ^= raw_got;
        }

        if (do_store_flush) {
            struct rusage ru0, ru1;
            if (fault_probe && i == 0)
                getrusage(RUSAGE_SELF, &ru0);
            uint64_t t0 = rdtsc_begin();
            *ptr_store = v;
            flushopt_line(ptr_store);
            store_sfence();
            uint64_t t1 = rdtsc_end();
            if (fault_probe && i == 0) {
                getrusage(RUSAGE_SELF, &ru1);
                first_store_flush_minflt = ru1.ru_minflt - ru0.ru_minflt;
                first_store_flush_majflt = ru1.ru_majflt - ru0.ru_majflt;
            }
            store_cycles[i] = t1 - t0;
        }

        if (do_load_after_clflush) {
            flush_line(ptr_load);
            struct rusage ru0, ru1;
            if (fault_probe && i == 0)
                getrusage(RUSAGE_SELF, &ru0);
            uint64_t t2 = rdtsc_begin();
            uint64_t got = *ptr_load;
            uint64_t t3 = rdtsc_end();
            if (fault_probe && i == 0) {
                getrusage(RUSAGE_SELF, &ru1);
                first_load_clflush_minflt = ru1.ru_minflt - ru0.ru_minflt;
                first_load_clflush_majflt = ru1.ru_majflt - ru0.ru_majflt;
            }
            load_cycles[i] = t3 - t2;
            sink ^= got;
        }

        if (do_roundtrip) {
            uint64_t rt_expect = v ^ 0xDEADBEEFCAFEBABEULL;
            struct rusage ru0, ru1;
            if (fault_probe && i == 0)
                getrusage(RUSAGE_SELF, &ru0);
            uint64_t t4 = rdtsc_begin();
            *ptr_rt = rt_expect;
            flushopt_line(ptr_rt);
            store_sfence();
            flush_line(ptr_rt);
            uint64_t rt_got = *ptr_rt;
            uint64_t t5 = rdtsc_end();
            if (fault_probe && i == 0) {
                getrusage(RUSAGE_SELF, &ru1);
                first_roundtrip_minflt = ru1.ru_minflt - ru0.ru_minflt;
                first_roundtrip_majflt = ru1.ru_majflt - ru0.ru_majflt;
            }
            roundtrip_cycles[i] = t5 - t4;
            if (rt_got != rt_expect)
                mismatches++;
            sink ^= rt_got;
        }
    }

    printf("\n");
    if (do_raw_store)
        report_stats("raw_store_only", raw_store_cycles, iters);
    if (do_raw_load)
        report_stats("raw_load_only", raw_load_cycles, iters);
    if (do_store_flush)
        report_stats("store_flush", store_cycles, iters);
    if (do_load_after_clflush)
        report_stats("load_after_clflush", load_cycles, iters);
    if (do_roundtrip)
        report_stats("store_load_roundtrip", roundtrip_cycles, iters);

    /* Parseable summary */
    {
        uint64_t avg_store = do_store_flush ?
                             compute_avg_latency(store_cycles, (size_t)iters) : 0;
        uint64_t avg_load = do_load_after_clflush ?
                            compute_avg_latency(load_cycles, (size_t)iters) : 0;
        uint64_t avg_raw_store = do_raw_store ?
                                 compute_avg_latency(raw_store_cycles, (size_t)iters) : 0;
        uint64_t avg_raw_load = do_raw_load ?
                                compute_avg_latency(raw_load_cycles, (size_t)iters) : 0;
        uint64_t avg_rt = do_roundtrip ?
                          compute_avg_latency(roundtrip_cycles, (size_t)iters) : 0;

        printf("\n=== Summary (parseable) ===\n");
        printf("iters=%d\n", iters);
        printf("mode=%s\n", mode_to_str(mode));
        printf("warmup_lines=%zu\n", warm_lines);
        if (do_raw_store) {
            printf("raw_store_avg_cycles=%" PRIu64 "\n", avg_raw_store);
            printf("raw_store_avg_ns=%" PRIu64 "\n", cycles_to_ns(avg_raw_store));
        }
        if (do_raw_load) {
            printf("raw_load_avg_cycles=%" PRIu64 "\n", avg_raw_load);
            printf("raw_load_avg_ns=%" PRIu64 "\n", cycles_to_ns(avg_raw_load));
        }
        if (do_store_flush) {
            printf("store_avg_cycles=%" PRIu64 "\n", avg_store);
            printf("store_avg_ns=%" PRIu64 "\n", cycles_to_ns(avg_store));
        }
        if (do_load_after_clflush) {
            printf("load_avg_cycles=%" PRIu64 "\n", avg_load);
            printf("load_avg_ns=%" PRIu64 "\n", cycles_to_ns(avg_load));
        }
        if (do_roundtrip) {
            printf("roundtrip_avg_cycles=%" PRIu64 "\n", avg_rt);
            printf("roundtrip_avg_ns=%" PRIu64 "\n", cycles_to_ns(avg_rt));
        }
        if (fault_probe) {
            if (do_raw_store) {
                printf("first_raw_store_minflt_delta=%ld\n", first_raw_store_minflt);
                printf("first_raw_store_majflt_delta=%ld\n", first_raw_store_majflt);
            }
            if (do_raw_load) {
                printf("first_raw_load_minflt_delta=%ld\n", first_raw_load_minflt);
                printf("first_raw_load_majflt_delta=%ld\n", first_raw_load_majflt);
            }
            if (do_store_flush) {
                printf("first_store_flush_minflt_delta=%ld\n", first_store_flush_minflt);
                printf("first_store_flush_majflt_delta=%ld\n", first_store_flush_majflt);
            }
            if (do_load_after_clflush) {
                printf("first_load_clflush_minflt_delta=%ld\n", first_load_clflush_minflt);
                printf("first_load_clflush_majflt_delta=%ld\n", first_load_clflush_majflt);
            }
            if (do_roundtrip) {
                printf("first_roundtrip_minflt_delta=%ld\n", first_roundtrip_minflt);
                printf("first_roundtrip_majflt_delta=%ld\n", first_roundtrip_majflt);
            }
        }
        printf("mismatch_count=%" PRIu64 "\n", mismatches);
        printf("sink=%" PRIu64 "\n", sink);
    }

    free(store_cycles);
    free(load_cycles);
    free(raw_store_cycles);
    free(raw_load_cycles);
    free(roundtrip_cycles);
    cxl_rpc_destroy(ctx);
    return 0;
}
