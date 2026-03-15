#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_include)
#if __has_include(<gem5/m5ops.h>)
#include <gem5/m5ops.h>
#elif __has_include("gem5/m5ops.h")
#include "gem5/m5ops.h"
#elif __has_include(<m5ops.h>)
#include <m5ops.h>
#else
#error "m5ops.h is not found"
#endif
#else
#include <gem5/m5ops.h>
#endif

#define CACHELINE_SIZE 64u

typedef struct {
    uint64_t min_ns;
    uint64_t avg_ns;
    uint64_t p50_ns;
    uint64_t p90_ns;
    uint64_t p95_ns;
    uint64_t p99_ns;
    uint64_t max_ns;
} bench_stats_t;

typedef struct {
    size_t thread_index;
    size_t thread_count;
    size_t bytes_per_thread;
    int warmup_loops;
    int measured_loops;
    int pin_threads;
    int verify;
    pthread_barrier_t *phase_barrier;
    volatile int *error_flag;
    volatile uint64_t *iter_start_ns;
    volatile uint64_t *iter_end_ns;
    uint8_t *src;
    uint8_t *dst;
} worker_args_t;

static inline void
bench_memmove(void *dst, const void *src, size_t len)
{
    (void)memmove(dst, src, len);
}

static int
phase_barrier_wait(pthread_barrier_t *barrier)
{
    const int rc = pthread_barrier_wait(barrier);
    if (rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD)
        return 0;
    return rc;
}

static int
cmp_u64(const void *lhs, const void *rhs)
{
    const uint64_t a = *(const uint64_t *)lhs;
    const uint64_t b = *(const uint64_t *)rhs;
    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

static void
compute_stats(uint64_t *samples, size_t count, bench_stats_t *stats)
{
    uint64_t sum = 0;

    memset(stats, 0, sizeof(*stats));
    if (!samples || count == 0)
        return;

    qsort(samples, count, sizeof(uint64_t), cmp_u64);
    for (size_t i = 0; i < count; i++)
        sum += samples[i];

    stats->min_ns = samples[0];
    stats->avg_ns = sum / (uint64_t)count;
    stats->p50_ns = samples[count * 50 / 100];
    stats->p90_ns = samples[count * 90 / 100];
    stats->p95_ns = samples[count * 95 / 100];
    stats->p99_ns = samples[count * 99 / 100];
    stats->max_ns = samples[count - 1];
}

static inline uint64_t
ns_now(void)
{
    return m5_rpns();
}

static inline double
bytes_per_ns_to_gib_per_s(uint64_t bytes, uint64_t ns)
{
    if (ns == 0)
        return 0.0;
    return ((double)bytes / (double)ns) * (1.0e9 / (double)(1ull << 30));
}

static inline uint64_t
round_up_u64(uint64_t value, uint64_t align)
{
    if (align == 0)
        return value;
    return ((value + align - 1u) / align) * align;
}

static int
parse_int_arg(int argc, char **argv, const char *flag,
              int default_value, int min_value, int max_value)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], flag) == 0) {
            char *end = NULL;
            long value = strtol(argv[i + 1], &end, 0);
            if (!end || *end != '\0' || value < min_value || value > max_value)
                return default_value;
            return (int)value;
        }
    }
    return default_value;
}

static uint64_t
parse_u64_arg(int argc, char **argv, const char *flag, uint64_t default_value)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], flag) == 0) {
            char *end = NULL;
            unsigned long long value = strtoull(argv[i + 1], &end, 0);
            if (!end || *end != '\0')
                return default_value;
            return (uint64_t)value;
        }
    }
    return default_value;
}

static int
pin_to_cpu(size_t cpu_index)
{
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET((int)cpu_index, &mask);
    return pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
}

static void
fill_pattern(uint8_t *buf, size_t size, uint8_t seed)
{
    for (size_t i = 0; i < size; i++)
        buf[i] = (uint8_t)(seed + (uint8_t)(i * 17u));
}

static void
touch_pages(uint8_t *buf, size_t size)
{
    const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    const size_t step = page_size > 0 ? page_size : 4096u;

    for (size_t i = 0; i < size; i += step)
        buf[i] ^= 0u;
    if (size != 0)
        buf[size - 1] ^= 0u;
}

static void *
worker_main(void *opaque)
{
    worker_args_t *args = (worker_args_t *)opaque;
    const int total_loops = args->warmup_loops + args->measured_loops;

    if (args->pin_threads) {
        const int rc = pin_to_cpu(args->thread_index);
        if (rc != 0) {
            fprintf(stderr,
                    "cpu_memmove_bw: failed to pin thread %zu to cpu %zu: %s\n",
                    args->thread_index, args->thread_index, strerror(rc));
            *args->error_flag = 1;
        }
    }

    for (int iter = 0; iter < total_loops; iter++) {
        int rc = phase_barrier_wait(args->phase_barrier);
        if (rc != 0) {
            fprintf(stderr,
                    "cpu_memmove_bw: barrier wait failed before start on "
                    "thread %zu: %s\n",
                    args->thread_index, strerror(rc));
            *args->error_flag = 1;
            return NULL;
        }

        rc = phase_barrier_wait(args->phase_barrier);
        if (rc != 0) {
            fprintf(stderr,
                    "cpu_memmove_bw: barrier release failed on thread %zu: %s\n",
                    args->thread_index, strerror(rc));
            *args->error_flag = 1;
            return NULL;
        }

        bench_memmove(args->dst, args->src, args->bytes_per_thread);

        rc = phase_barrier_wait(args->phase_barrier);
        if (rc != 0) {
            fprintf(stderr,
                    "cpu_memmove_bw: barrier wait failed after copy on "
                    "thread %zu: %s\n",
                    args->thread_index, strerror(rc));
            *args->error_flag = 1;
            return NULL;
        }
    }

    if (args->verify &&
        memcmp(args->src, args->dst, args->bytes_per_thread) != 0) {
        fprintf(stderr,
                "cpu_memmove_bw: verification failed on thread %zu\n",
                args->thread_index);
        *args->error_flag = 1;
    }

    return NULL;
}

int
main(int argc, char **argv)
{
    const int threads = parse_int_arg(argc, argv, "--threads", 1, 1, 256);
    const uint64_t total_bytes_per_thread = parse_u64_arg(
        argc, argv, "--total-bytes", 16ull * 1024ull * 1024ull
    );
    const int loops = parse_int_arg(argc, argv, "--loops", 3, 1, 1000000);
    const int warmup = parse_int_arg(argc, argv, "--warmup", 1, 0, 1000000);
    const int pin_threads = parse_int_arg(argc, argv, "--pin", 1, 0, 1);
    const int verify = parse_int_arg(argc, argv, "--verify", 0, 0, 1);
    const long online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    const size_t bytes_per_thread = (size_t)round_up_u64(
        total_bytes_per_thread, CACHELINE_SIZE
    );
    const uint64_t aggregate_bytes = (uint64_t)bytes_per_thread * (uint64_t)threads;
    uint64_t *samples = NULL;
    pthread_t *workers = NULL;
    worker_args_t *worker_args = NULL;
    pthread_barrier_t phase_barrier;
    volatile int error_flag = 0;
    volatile uint64_t iter_start_ns = 0;
    volatile uint64_t iter_end_ns = 0;
    bench_stats_t stats;

    if (pin_threads && online_cpus > 0 && threads > online_cpus) {
        fprintf(stderr,
                "cpu_memmove_bw: need %d CPUs for pinning, but guest reports %ld\n",
                threads, online_cpus);
        return 2;
    }

    samples = calloc((size_t)loops, sizeof(*samples));
    workers = calloc((size_t)threads, sizeof(*workers));
    worker_args = calloc((size_t)threads, sizeof(*worker_args));
    if (!samples || !workers || !worker_args) {
        fprintf(stderr, "cpu_memmove_bw: allocation failure for control data\n");
        free(samples);
        free(workers);
        free(worker_args);
        return 1;
    }

    if (pthread_barrier_init(&phase_barrier, NULL,
                             (unsigned)(threads + 1)) != 0) {
        fprintf(stderr, "cpu_memmove_bw: pthread_barrier_init failed\n");
        free(samples);
        free(workers);
        free(worker_args);
        return 1;
    }

    printf("cpu_memmove_bw_timer,m5_rpns\n");
    printf("cpu_memmove_bw_config,threads=%d,total_bytes_per_thread=%" PRIu64
           ",bytes_per_thread=%zu,aggregate_bytes=%" PRIu64
           ",warmup=%d,loops=%d,pin=%d,verify=%d\n",
           threads, total_bytes_per_thread, bytes_per_thread, aggregate_bytes,
           warmup, loops, pin_threads, verify);

    for (int thread_index = 0; thread_index < threads; thread_index++) {
        uint8_t *src = NULL;
        uint8_t *dst = NULL;

        if (posix_memalign((void **)&src, CACHELINE_SIZE, bytes_per_thread) != 0 ||
            posix_memalign((void **)&dst, CACHELINE_SIZE, bytes_per_thread) != 0 ||
            !src || !dst) {
            fprintf(stderr,
                    "cpu_memmove_bw: buffer allocation failed on thread %d\n",
                    thread_index);
            free(src);
            free(dst);
            error_flag = 1;
            break;
        }

        fill_pattern(src, bytes_per_thread, (uint8_t)(0x11u + thread_index));
        memset(dst, 0, bytes_per_thread);
        touch_pages(src, bytes_per_thread);
        touch_pages(dst, bytes_per_thread);

        worker_args[thread_index] = (worker_args_t){
            .thread_index = (size_t)thread_index,
            .thread_count = (size_t)threads,
            .bytes_per_thread = bytes_per_thread,
            .warmup_loops = warmup,
            .measured_loops = loops,
            .pin_threads = pin_threads,
            .verify = verify,
            .phase_barrier = &phase_barrier,
            .error_flag = &error_flag,
            .iter_start_ns = &iter_start_ns,
            .iter_end_ns = &iter_end_ns,
            .src = src,
            .dst = dst,
        };

        if (pthread_create(&workers[thread_index], NULL, worker_main,
                           &worker_args[thread_index]) != 0) {
            fprintf(stderr,
                    "cpu_memmove_bw: pthread_create failed for thread %d\n",
                    thread_index);
            return 1;
        }
    }

    if (!error_flag) {
        const int total_loops = warmup + loops;
        for (int iter = 0; iter < total_loops; iter++) {
            int rc = phase_barrier_wait(&phase_barrier);
            if (rc != 0) {
                fprintf(stderr,
                        "cpu_memmove_bw: main barrier wait failed before "
                        "start: %s\n",
                        strerror(rc));
                error_flag = 1;
                break;
            }
            iter_start_ns = ns_now();
            rc = phase_barrier_wait(&phase_barrier);
            if (rc != 0) {
                fprintf(stderr,
                        "cpu_memmove_bw: main barrier release failed: %s\n",
                        strerror(rc));
                error_flag = 1;
                break;
            }
            rc = phase_barrier_wait(&phase_barrier);
            if (rc != 0) {
                fprintf(stderr,
                        "cpu_memmove_bw: main barrier wait failed after copy: "
                        "%s\n",
                        strerror(rc));
                error_flag = 1;
                break;
            }
            iter_end_ns = ns_now();

            if (iter < warmup)
                continue;

            samples[iter - warmup] = iter_end_ns - iter_start_ns;
            printf("cpu_memmove_bw_loop,iter=%d,threads=%d,ns=%" PRIu64
                   ",total_gib_per_s=%.3f,per_thread_gib_per_s=%.3f\n",
                   iter - warmup,
                   threads,
                   samples[iter - warmup],
                   bytes_per_ns_to_gib_per_s(
                       aggregate_bytes, samples[iter - warmup]
                   ),
                   bytes_per_ns_to_gib_per_s(
                       aggregate_bytes, samples[iter - warmup]
                   ) / (double)threads);
        }
    }

    for (int thread_index = 0; thread_index < threads; thread_index++) {
        if (workers[thread_index] != 0)
            pthread_join(workers[thread_index], NULL);
    }

    if (error_flag) {
        for (int thread_index = 0; thread_index < threads; thread_index++) {
            free(worker_args[thread_index].src);
            free(worker_args[thread_index].dst);
        }
        free(samples);
        free(workers);
        free(worker_args);
        pthread_barrier_destroy(&phase_barrier);
        return 1;
    }

    for (int thread_index = 0; thread_index < threads; thread_index++) {
        printf("cpu_memmove_bw_thread,thread=%d,cpu=%d,total_bytes=%zu\n",
               thread_index, thread_index, bytes_per_thread);
    }

    compute_stats(samples, (size_t)loops, &stats);
    printf("cpu_memmove_bw_summary,threads=%d,total_bytes_per_thread=%" PRIu64
           ",bytes_per_thread=%zu,aggregate_bytes=%" PRIu64
           ",loops=%d,min_ns=%" PRIu64 ",avg_ns=%" PRIu64 ",p50_ns=%" PRIu64
           ",p90_ns=%" PRIu64 ",p95_ns=%" PRIu64 ",p99_ns=%" PRIu64
           ",max_ns=%" PRIu64 ",best_total_gib_per_s=%.3f,avg_total_gib_per_s=%.3f"
           ",p50_total_gib_per_s=%.3f,p99_total_gib_per_s=%.3f"
           ",worst_total_gib_per_s=%.3f,avg_per_thread_gib_per_s=%.3f\n",
           threads,
           total_bytes_per_thread,
           bytes_per_thread,
           aggregate_bytes,
           loops,
           stats.min_ns,
           stats.avg_ns,
           stats.p50_ns,
           stats.p90_ns,
           stats.p95_ns,
           stats.p99_ns,
           stats.max_ns,
           bytes_per_ns_to_gib_per_s(aggregate_bytes, stats.min_ns),
           bytes_per_ns_to_gib_per_s(aggregate_bytes, stats.avg_ns),
           bytes_per_ns_to_gib_per_s(aggregate_bytes, stats.p50_ns),
           bytes_per_ns_to_gib_per_s(aggregate_bytes, stats.p99_ns),
           bytes_per_ns_to_gib_per_s(aggregate_bytes, stats.max_ns),
           bytes_per_ns_to_gib_per_s(aggregate_bytes, stats.avg_ns) /
               (double)threads);

    for (int thread_index = 0; thread_index < threads; thread_index++) {
        free(worker_args[thread_index].src);
        free(worker_args[thread_index].dst);
    }
    free(samples);
    free(workers);
    free(worker_args);
    pthread_barrier_destroy(&phase_barrier);
    return 0;
}
