/*
 * Minimal application-path CXL RPC client.
 *
 * The client pre-generates a deterministic YCSB-like request stream before the
 * measured send/receive loop. Request timing still follows the existing
 * contract:
 *   req_<i>_start_tick=<u64>
 *   req_<i>_end_tick=<u64>
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#include "cxl_rpc.h"
#include "cxl_rpc_layout.h"
#include "rpc_first_round_barrier.h"
#include "rpc_mica_common.h"

#define DEFAULT_NUM_REQUESTS 20
#define DEFAULT_POLL_PAUSE_ITERS 0
#define DEFAULT_SLIDING_WINDOW 16
#define DEFAULT_SLOW_CLIENT_COUNT 0
#define DEFAULT_SLOW_CLIENT_SEND_PAUSE_ITERS 0
#define CLIENT_RPC_ID_MAX 32767u
#define CLIENT_RPC_ID_SPACE (CLIENT_RPC_ID_MAX + 1u)
#define FIXED_UNIFORM_PLAN_LEN 30u

typedef struct {
    uint8_t op;
    uint16_t key_len;
    uint32_t value_len;
    uint64_t key_id;
    uint64_t key_hash;
    uint64_t value_seed;
} app_operation_t;

static volatile int keep_running = 1;

static inline uint64_t
current_tick(void)
{
    return m5_rpns();
}

static int
rpc_marker_enabled(void)
{
    static int initialized = 0;
    static int enabled = 0;

    if (!initialized) {
        const char *env = getenv("CXL_RPC_MARKERS");
        enabled = (env && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
        initialized = 1;
    }

    return enabled;
}

static void
rpc_markerf(const char *phase, const char *fmt, ...)
{
    va_list ap;

    if (!rpc_marker_enabled())
        return;

    fprintf(stderr, "rpc_marker,role=client,phase=%s,tick=%lu",
            phase ? phase : "unknown", current_tick());
    if (fmt && fmt[0] != '\0') {
        fputc(',', stderr);
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    fputc('\n', stderr);
}

static inline uint64_t
next_random_u64(uint64_t *state)
{
    uint64_t x = (state && *state != 0) ? *state : 0xA0761D6478BD642FULL;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    x *= 2685821657736338717ULL;
    if (state)
        *state = x;
    return x;
}

static inline double
next_random_double(uint64_t *state)
{
    uint64_t raw = next_random_u64(state) >> 11;
    return (double)raw * (1.0 / 9007199254740992.0);
}

static int
parse_int_arg(int argc, char **argv, const char *flag, int default_val)
{
    int i;
    for (i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0) {
            int val = atoi(argv[i + 1]);
            return (val > 0) ? val : default_val;
        }
    }
    return default_val;
}

static int
parse_nonneg_int_arg(int argc, char **argv, const char *flag, int default_val)
{
    int i;
    for (i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0) {
            int val = atoi(argv[i + 1]);
            return (val >= 0) ? val : default_val;
        }
    }
    return default_val;
}

static int
has_flag(int argc, char **argv, const char *flag)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0)
            return 1;
    }

    return 0;
}

static int
parse_int_arg_range(int argc, char **argv, const char *flag, int default_val,
                    int min_val, int max_val)
{
    int i;
    for (i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0) {
            char *end = NULL;
            long v = strtol(argv[i + 1], &end, 0);
            if (end && *end == '\0' && v >= min_val && v <= max_val)
                return (int)v;
            return default_val;
        }
    }
    return default_val;
}

static int
parse_required_int_arg_range(int argc, char **argv, const char *flag,
                             int default_val, int min_val, int max_val,
                             int *out)
{
    int i;

    if (!out)
        return -1;

    *out = default_val;
    for (i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0) {
            char *end = NULL;
            long v = strtol(argv[i + 1], &end, 0);
            if (end && *end == '\0' && v >= min_val && v <= max_val) {
                *out = (int)v;
                return 0;
            }
            return -1;
        }
    }

    return 0;
}

static int
parse_u64_arg(int argc, char **argv, const char *flag, uint64_t default_val,
              uint64_t min_val, uint64_t max_val, uint64_t *out)
{
    int i;

    if (!out)
        return -1;
    *out = default_val;

    for (i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0) {
            char *end = NULL;
            unsigned long long v = strtoull(argv[i + 1], &end, 0);
            if (end && *end == '\0' && v >= min_val && v <= max_val) {
                *out = (uint64_t)v;
                return 0;
            }
            return -1;
        }
    }
    return 0;
}

static int
parse_size_literal(const char *s, size_t min_val, size_t max_val, size_t *out)
{
    char *end = NULL;
    unsigned long long raw;

    if (!s || !out)
        return -1;

    raw = strtoull(s, &end, 10);
    if (end == s)
        return -1;

    {
        unsigned long long mul = 1;
        if (*end != '\0') {
            char c0 = (char)toupper((unsigned char)end[0]);
            if (c0 == 'B' && end[1] == '\0') {
                mul = 1;
            } else if (c0 == 'K' &&
                       (end[1] == '\0' ||
                        (toupper((unsigned char)end[1]) == 'B' &&
                         end[2] == '\0'))) {
                mul = 1024ULL;
            } else if (c0 == 'M' &&
                       (end[1] == '\0' ||
                        (toupper((unsigned char)end[1]) == 'B' &&
                         end[2] == '\0'))) {
                mul = 1024ULL * 1024ULL;
            } else {
                return -1;
            }
        }
        raw *= mul;
    }

    if (raw < (unsigned long long)min_val ||
        raw > (unsigned long long)max_val)
        return -1;

    *out = (size_t)raw;
    return 0;
}

static int
first_round_barrier_participants(int num_clients,
                                 int num_requests,
                                 int slow_client_count,
                                 int slow_count_per_client)
{
    if (num_clients <= 0 || num_requests <= 0)
        return 0;

    if (slow_client_count <= 0 || slow_count_per_client > 0)
        return num_clients;

    if (slow_client_count >= num_clients)
        return 0;

    return num_clients - slow_client_count;
}

static int
parse_size_arg(int argc, char **argv, const char *flag, size_t default_val,
               size_t min_val, size_t max_val, size_t *out)
{
    int i;

    if (!out)
        return -1;
    *out = default_val;

    for (i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0) {
            if (parse_size_literal(argv[i + 1], min_val, max_val, out) != 0)
                return -1;
            return 0;
        }
    }
    return 0;
}

static int
parse_double_arg(int argc, char **argv, const char *flag, double default_val,
                 double min_val, double max_val, double *out)
{
    int i;

    if (!out)
        return -1;
    *out = default_val;

    for (i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0) {
            char *end = NULL;
            double v = strtod(argv[i + 1], &end);
            if (end && *end == '\0' && v >= min_val && v <= max_val) {
                *out = v;
                return 0;
            }
            return -1;
        }
    }
    return 0;
}

static int
parse_profile_arg(int argc, char **argv, const char *default_name,
                  rpc_app_profile_t *out)
{
    const char *profile_name = default_name;
    int i;

    if (!out)
        return -1;

    for (i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--profile") == 0) {
            profile_name = argv[i + 1];
            break;
        }
    }

    return rpc_app_lookup_profile(profile_name, out);
}

static inline uint64_t
node_region_base(int node_id)
{
    uint64_t slot = (uint64_t)node_id + 1ULL;
    return CXL_BASE + (slot * CLIENT_REGION_SIZE);
}

static inline void
spin_pause_iters(int poll_pause_iters)
{
    int i;
    for (i = 0; i < poll_pause_iters; i++)
        __asm__ __volatile__("pause" ::: "memory");
}

static inline int
scale_sparse_send_pause_iters(int base_pause_iters,
                              int request_count,
                              int client_request_count)
{
    uint64_t total_pause_iters = 0;

    if (base_pause_iters <= 0 ||
        request_count <= 1 ||
        client_request_count <= 1 ||
        client_request_count >= request_count) {
        return base_pause_iters;
    }

    // Keep the sparse-client issue span aligned with the baseline
    // request-count schedule. Fewer requests therefore implies more
    // application-side think time before each send.
    total_pause_iters =
        (uint64_t)(request_count - 1) * (uint64_t)base_pause_iters;
    return (int)((total_pause_iters + (uint64_t)(client_request_count - 2)) /
                 (uint64_t)(client_request_count - 1));
}

static void
signal_handler(int sig)
{
    (void)sig;
    keep_running = 0;
}

static int
build_zipf_cdf(double theta, uint64_t record_count, double **out_cdf)
{
    double normalizer = 0.0;
    double running = 0.0;
    double *cdf;
    uint64_t i;

    if (!out_cdf || record_count == 0 || theta <= 0.0)
        return -1;

    cdf = (double *)malloc((size_t)record_count * sizeof(*cdf));
    if (!cdf)
        return -1;

    for (i = 1; i <= record_count; i++)
        normalizer += 1.0 / pow((double)i, theta);
    if (normalizer <= 0.0) {
        free(cdf);
        return -1;
    }

    for (i = 1; i <= record_count; i++) {
        running += (1.0 / pow((double)i, theta)) / normalizer;
        cdf[i - 1] = running;
    }
    cdf[record_count - 1] = 1.0;
    *out_cdf = cdf;
    return 0;
}

static uint64_t
sample_zipf_key_id(const double *cdf, uint64_t record_count, uint64_t *rng_state)
{
    double needle = next_random_double(rng_state);
    uint64_t lo = 0;
    uint64_t hi = record_count - 1;

    while (lo < hi) {
        uint64_t mid = lo + ((hi - lo) / 2u);
        if (needle <= cdf[mid])
            hi = mid;
        else
            lo = mid + 1u;
    }

    return lo;
}

static int
sample_uniform_key_id(uint64_t record_count, uint64_t *rng_state,
                      uint64_t *out_key_id)
{
    if (!rng_state || !out_key_id || record_count == 0)
        return -1;

    *out_key_id = next_random_u64(rng_state) % record_count;
    return 0;
}

static int
uses_fixed_uniform_plan(const char *profile_name, rpc_app_key_dist_t key_dist)
{
    return key_dist == RPC_APP_KEY_DIST_UNIFORM &&
           rpc_app_profile_has_variable_layout(profile_name);
}

static uint64_t
fixed_uniform_plan_slot(uint64_t client_id, uint64_t req_index)
{
    return (req_index + (client_id % FIXED_UNIFORM_PLAN_LEN)) %
           FIXED_UNIFORM_PLAN_LEN;
}

static void
fixed_uniform_plan_bins(uint64_t plan_slot,
                        size_t *out_key_bin,
                        size_t *out_value_bin)
{
    size_t key_bin = (size_t)(plan_slot % RPC_APP_UDB_BIN_COUNT);
    size_t block = (size_t)((plan_slot / RPC_APP_UDB_BIN_COUNT) % 3u);
    size_t value_bin = (key_bin + (block * 3u)) % RPC_APP_UDB_BIN_COUNT;

    if (out_key_bin)
        *out_key_bin = key_bin;
    if (out_value_bin)
        *out_value_bin = value_bin;
}

static int
fixed_uniform_key_used(const app_operation_t *ops,
                       uint64_t used_count,
                       uint64_t key_id)
{
    uint64_t idx;

    if (!ops)
        return 0;

    for (idx = 0; idx < used_count; idx++) {
        if (ops[idx].key_id == key_id)
            return 1;
    }

    return 0;
}

static int
find_fixed_uniform_key_id(const app_operation_t *ops,
                          uint64_t used_count,
                          uint64_t record_count,
                          uint64_t client_id,
                          uint64_t plan_slot,
                          uint64_t dataset_seed,
                          uint64_t *out_key_id)
{
    uint64_t start_key_id;
    uint64_t step;
    size_t target_key_bin = 0;
    size_t target_value_bin = 0;

    if (!ops || !out_key_id || record_count == 0)
        return -1;

    fixed_uniform_plan_bins(plan_slot, &target_key_bin, &target_value_bin);
    start_key_id = rpc_app_mix64(
        dataset_seed ^
        ((client_id + 1u) * 0x9E3779B185EBCA87ULL) ^
        ((plan_slot + 1u) * 0xD6E8FEB86659FD93ULL)) % record_count;

    for (step = 0; step < record_count; step++) {
        uint64_t key_id = (start_key_id + step) % record_count;

        if (fixed_uniform_key_used(ops, used_count, key_id))
            continue;
        if (rpc_app_udb_key_bin_index(dataset_seed, key_id) != target_key_bin)
            continue;
        if (rpc_app_udb_value_bin_index(dataset_seed, key_id) !=
            target_value_bin) {
            continue;
        }

        *out_key_id = key_id;
        return 0;
    }

    return -1;
}

static int
build_operations(app_operation_t *ops, uint64_t num_requests,
                 const char *profile_name,
                 uint64_t record_count, size_t key_size, size_t value_size,
                 uint64_t client_id,
                 size_t max_key_size,
                 double read_ratio, double update_ratio, double rmw_ratio,
                 rpc_app_key_dist_t key_dist,
                 double zipf_theta,
                 uint64_t dataset_seed, uint64_t workload_seed)
{
    double *cdf = NULL;
    uint8_t *key_buf = NULL;
    uint64_t rng_state = workload_seed;
    uint64_t req_index;

    if (!ops || !profile_name || num_requests == 0 || record_count == 0 ||
        key_size == 0 || value_size == 0 || max_key_size == 0) {
        return -1;
    }
    if (read_ratio < 0.0 || update_ratio < 0.0 || rmw_ratio < 0.0 ||
        fabs((read_ratio + update_ratio + rmw_ratio) - 1.0) > 1e-6) {
        return -1;
    }

    key_buf = (uint8_t *)malloc(max_key_size);
    if (!key_buf)
        return -1;

    if (key_dist == RPC_APP_KEY_DIST_ZIPF &&
        build_zipf_cdf(zipf_theta, record_count, &cdf) != 0) {
        free(key_buf);
        return -1;
    }

    for (req_index = 0; req_index < num_requests; req_index++) {
        size_t record_key_len = 0;
        size_t record_value_len = 0;
        uint64_t key_id = 0;
        uint64_t key_hash;
        double selector;

        if (uses_fixed_uniform_plan(profile_name, key_dist)) {
            uint64_t plan_slot = fixed_uniform_plan_slot(client_id, req_index);

            if (find_fixed_uniform_key_id(ops,
                                          req_index,
                                          record_count,
                                          client_id,
                                          plan_slot,
                                          dataset_seed,
                                          &key_id) != 0) {
                free(key_buf);
                free(cdf);
                return -1;
            }
        } else if (key_dist == RPC_APP_KEY_DIST_ZIPF) {
            key_id = sample_zipf_key_id(cdf, record_count, &rng_state);
        } else if (sample_uniform_key_id(record_count, &rng_state, &key_id) != 0) {
            free(key_buf);
            free(cdf);
            return -1;
        }

        rpc_app_record_layout(profile_name, dataset_seed, key_id,
                              key_size, value_size,
                              &record_key_len, &record_value_len);
        if (record_key_len == 0 || record_key_len > max_key_size ||
            record_key_len > RPC_APP_MAX_KEY_SIZE || record_value_len == 0) {
            free(key_buf);
            free(cdf);
            return -1;
        }

        rpc_app_fill_key(key_buf, record_key_len, dataset_seed, key_id);
        key_hash = rpc_app_hash_bytes(key_buf, record_key_len);
        selector = next_random_double(&rng_state);

        if (selector < read_ratio) {
            ops[req_index].op = RPC_APP_OP_GET;
        } else if (selector < (read_ratio + update_ratio)) {
            ops[req_index].op = RPC_APP_OP_PUT;
        } else {
            ops[req_index].op = RPC_APP_OP_RMW;
        }
        ops[req_index].key_len = (uint16_t)record_key_len;
        ops[req_index].value_len = (uint32_t)record_value_len;
        ops[req_index].key_id = key_id;
        ops[req_index].key_hash = key_hash;
        ops[req_index].value_seed =
            rpc_app_value_seed(workload_seed, key_hash, req_index + 1u);
    }

    free(key_buf);
    free(cdf);
    return 0;
}

static int
send_one_request(cxl_connection_t *conn,
                 uint8_t *req_buf,
                 size_t req_buf_size,
                 const app_operation_t *op,
                 uint64_t dataset_seed,
                 int *rpc_id_to_request_index,
                 uint64_t *request_start_ticks,
                 int sent_requests)
{
    rpc_app_request_hdr_t *hdr;
    uint8_t *key_ptr;
    size_t req_len;
    int rpc_id;

    if (!conn || !req_buf || !op || !rpc_id_to_request_index ||
        !request_start_ticks || sent_requests < 0) {
        return -1;
    }

    req_len = rpc_app_request_wire_size(
        op->op,
        op->key_len,
        (op->op == RPC_APP_OP_PUT || op->op == RPC_APP_OP_RMW) ?
            op->value_len :
            0u);
    if (req_len > req_buf_size)
        return -1;

    memset(req_buf, 0, req_len);
    hdr = (rpc_app_request_hdr_t *)req_buf;
    hdr->op = op->op;
    hdr->key_len = (uint8_t)op->key_len;
    hdr->reserved0 = 0;
    hdr->value_len = (uint32_t)(
        (op->op == RPC_APP_OP_PUT || op->op == RPC_APP_OP_RMW) ?
            op->value_len :
            0u
    );
    hdr->key_hash = op->key_hash;

    key_ptr = (uint8_t *)(hdr + 1);
    rpc_app_fill_key(key_ptr, op->key_len, dataset_seed, op->key_id);
    if (op->op == RPC_APP_OP_PUT || op->op == RPC_APP_OP_RMW) {
        rpc_app_fill_value(key_ptr + op->key_len,
                           op->value_len, op->value_seed);
    }

    request_start_ticks[sent_requests] = current_tick();
    rpc_id = cxl_send_request(conn, req_buf, req_len);
    if (rpc_id <= 0) {
        fprintf(stderr, "client: cxl_send_request failed\n");
        return -1;
    }
    if ((unsigned int)rpc_id > CLIENT_RPC_ID_MAX ||
        rpc_id_to_request_index[rpc_id] >= 0) {
        fprintf(stderr, "client: rpc_id tracking overflow/duplicate\n");
        return -1;
    }

    rpc_id_to_request_index[rpc_id] = sent_requests;
    return rpc_id;
}

static int
drain_completed_responses(cxl_connection_t *conn,
                          const app_operation_t *ops,
                          int allow_miss,
                          int *rpc_id_to_request_index,
                          uint64_t *request_end_ticks,
                          int *completed_requests,
                          int *first_response_marker_emitted)
{
    int drained = 0;

    if (!conn || !ops || !rpc_id_to_request_index || !request_end_ticks ||
        !completed_requests) {
        return -1;
    }

    while (keep_running) {
        const void *response_view = NULL;
        size_t response_len = 0;
        uint16_t consumed_rpc_id = 0;
        int idx;
        const rpc_app_response_hdr_t *resp_hdr;
        size_t expected_response_len;
        const app_operation_t *expected_op;
        const uint8_t *value_ptr;

        int peek_rc = cxl_peek_next_response_view(conn,
                                                  &response_view,
                                                  &response_len,
                                                  &consumed_rpc_id);
        if (peek_rc < 0) {
            fprintf(stderr, "client: cxl_peek_next_response_view failed\n");
            return -1;
        }
        if (peek_rc == 0)
            break;

        if (response_len < sizeof(*resp_hdr) || !response_view) {
            fprintf(stderr, "client: malformed response envelope\n");
            return -1;
        }

        if ((unsigned int)consumed_rpc_id > CLIENT_RPC_ID_MAX) {
            fprintf(stderr, "client: invalid consumed rpc_id=%u\n",
                    (unsigned)consumed_rpc_id);
            return -1;
        }
        idx = rpc_id_to_request_index[consumed_rpc_id];
        if (idx < 0 || request_end_ticks[idx] != UINT64_MAX) {
            fprintf(stderr, "client: unmatched or duplicate response rpc_id=%u\n",
                    (unsigned)consumed_rpc_id);
            return -1;
        }

        expected_op = &ops[idx];
        resp_hdr = (const rpc_app_response_hdr_t *)response_view;
        expected_response_len = rpc_app_response_wire_size(resp_hdr->status,
                                                           resp_hdr->op,
                                                           resp_hdr->value_len);
        if (resp_hdr->op != expected_op->op || expected_response_len != response_len) {
            fprintf(stderr, "client: response header mismatch\n");
            return -1;
        }
        if (resp_hdr->status != RPC_APP_STATUS_OK &&
            resp_hdr->status != RPC_APP_STATUS_MISS) {
            fprintf(stderr, "client: invalid application response status=%u\n",
                    (unsigned)resp_hdr->status);
            return -1;
        }
        if (!allow_miss && resp_hdr->status != RPC_APP_STATUS_OK) {
            fprintf(stderr,
                    "client: unexpected application miss "
                    "rpc_id=%u op=%u status=%u key_id=%llu\n",
                    (unsigned)consumed_rpc_id, (unsigned)expected_op->op,
                    (unsigned)resp_hdr->status,
                    (unsigned long long)expected_op->key_id);
            return -1;
        }

        if (resp_hdr->status == RPC_APP_STATUS_OK &&
            (resp_hdr->op == RPC_APP_OP_GET ||
             resp_hdr->op == RPC_APP_OP_RMW)) {
            if (resp_hdr->value_len != expected_op->value_len) {
                fprintf(stderr, "client: GET/RMW value size mismatch\n");
                return -1;
            }
            value_ptr = (const uint8_t *)(resp_hdr + 1);
            if (rpc_app_checksum_bytes(value_ptr, resp_hdr->value_len) !=
                resp_hdr->value_checksum) {
                fprintf(stderr, "client: GET/RMW value checksum mismatch\n");
                return -1;
            }
        } else if (resp_hdr->value_len != 0 || resp_hdr->value_checksum != 0) {
            fprintf(stderr,
                    "client: unexpected response payload for non-GET/RMW\n");
            return -1;
        }

        if (cxl_advance_response_head(conn,
                                      consumed_rpc_id,
                                      response_len) != 1) {
            fprintf(stderr, "client: cxl_advance_response_head failed\n");
            return -1;
        }

        request_end_ticks[idx] = current_tick();
        if (first_response_marker_emitted && !(*first_response_marker_emitted)) {
            rpc_markerf("first_response_seen",
                        "rpc_id=%u,response_len=%zu,completed=%d",
                        (unsigned)consumed_rpc_id, response_len,
                        *completed_requests + 1);
            *first_response_marker_emitted = 1;
        }
        rpc_id_to_request_index[consumed_rpc_id] = -1;
        (*completed_requests)++;
        drained = 1;
    }

    return drained;
}

int
main(int argc, char **argv)
{
    int rc = 0;
    int completed_requests = 0;
    int first_response_marker_emitted = 0;
    int sent_requests = 0;
    cxl_context_t *ctx = NULL;
    cxl_connection_t *conn = NULL;
    int *rpc_id_to_request_index = NULL;
    uint64_t *request_start_ticks = NULL;
    uint64_t *request_end_ticks = NULL;
    app_operation_t *ops = NULL;
    uint8_t *req_buf = NULL;
    rpc_app_profile_t profile;
    size_t key_size = 0;
    size_t value_size = 0;
    size_t max_key_size = 0;
    size_t max_value_size = 0;
    size_t max_request_size = 0;
    uint64_t record_count = RPC_APP_DEFAULT_RECORD_COUNT;
    uint64_t dataset_seed = RPC_APP_DEFAULT_DATASET_SEED;
    uint64_t workload_seed = RPC_APP_DEFAULT_WORKLOAD_SEED;
    double read_ratio = 0.0;
    double update_ratio = 0.0;
    double rmw_ratio = 0.0;
    rpc_app_key_dist_t key_dist = RPC_APP_KEY_DIST_ZIPF;
    double zipf_theta = 0.0;
    size_t reserve_n;
    int num_requests = parse_int_arg(argc, argv, "--requests",
                                     DEFAULT_NUM_REQUESTS);
    int poll_pause_iters = parse_nonneg_int_arg(argc, argv, "--poll-pause",
                                                DEFAULT_POLL_PAUSE_ITERS);
    int sliding_window = DEFAULT_SLIDING_WINDOW;
    int num_clients = parse_int_arg_range(argc, argv, "--num-clients", 1,
                                          1, MAX_CLIENTS);
    int node_id = parse_int_arg_range(argc, argv, "--node-id", 0,
                                      0, MAX_CLIENTS - 1);
    int slow_client_count = DEFAULT_SLOW_CLIENT_COUNT;
    int slow_count_per_client = 0;
    int slow_client_send_pause_iters = DEFAULT_SLOW_CLIENT_SEND_PAUSE_ITERS;
    int key_size_overridden = has_flag(argc, argv, "--key-size");
    int value_size_overridden = has_flag(argc, argv, "--value-size");
    int allow_miss = has_flag(argc, argv, "--allow-miss");
    int variable_layout = 0;
    int is_slow_client;
    int client_request_count;
    int barrier_participant_count;
    int send_pause_iters;

    if (parse_profile_arg(argc, argv, RPC_APP_PROFILE_YCSB_C_1K,
                          &profile) != 0) {
        fprintf(stderr,
                "client: invalid --profile "
                "(use %s|%s|%s|%s|%s; alias %s also accepted)\n",
                RPC_APP_PROFILE_YCSB_C_1K,
                RPC_APP_PROFILE_YCSB_A_1K,
                RPC_APP_PROFILE_YCSB_B_1K,
                RPC_APP_PROFILE_YCSB_F_1K,
                RPC_APP_PROFILE_UDB_RO,
                RPC_APP_PROFILE_YCSB_1K_RO);
        return 1;
    }
    key_size = profile.key_size;
    value_size = profile.value_size;
    read_ratio = profile.read_ratio;
    update_ratio = profile.update_ratio;
    rmw_ratio = profile.rmw_ratio;
    key_dist = profile.key_dist;
    zipf_theta = profile.zipf_theta;

    if (parse_required_int_arg_range(argc, argv, "--window",
                                     DEFAULT_SLIDING_WINDOW,
                                     1, (int)CLIENT_RPC_ID_MAX,
                                     &sliding_window) != 0) {
        fprintf(stderr,
                "client: invalid --window (range 1..%u)\n",
                CLIENT_RPC_ID_MAX);
        return 1;
    }
    if (parse_required_int_arg_range(argc, argv, "--slow-client-count",
                                     DEFAULT_SLOW_CLIENT_COUNT,
                                     0, MAX_CLIENTS,
                                     &slow_client_count) != 0) {
        fprintf(stderr,
                "client: invalid --slow-client-count (range 0..%d)\n",
                MAX_CLIENTS);
        return 1;
    }
    if (parse_required_int_arg_range(argc, argv, "--slow-count-per-client",
                                     0,
                                     0, INT_MAX,
                                     &slow_count_per_client) != 0) {
        fprintf(stderr,
                "client: invalid --slow-count-per-client "
                "(range 0..%d)\n",
                INT_MAX);
        return 1;
    }
    if (parse_required_int_arg_range(argc, argv,
                                     "--slow-client-send-pause-iters",
                                     DEFAULT_SLOW_CLIENT_SEND_PAUSE_ITERS,
                                     0, INT_MAX,
                                     &slow_client_send_pause_iters) != 0) {
        fprintf(stderr,
                "client: invalid --slow-client-send-pause-iters "
                "(range 0..%d)\n",
                INT_MAX);
        return 1;
    }
    if (node_id >= num_clients || slow_client_count > num_clients) {
        fprintf(stderr, "client: invalid num-clients / node-id / slow-client-count\n");
        return 1;
    }
    if (slow_client_count > 0 &&
        (slow_count_per_client < 0 || slow_count_per_client > num_requests)) {
        fprintf(stderr,
                "client: slow_count_per_client=%d must be in range 0..%d "
                "when slow_client_count > 0\n",
                slow_count_per_client, num_requests);
        return 1;
    }
    if (parse_size_arg(argc, argv, "--key-size", key_size,
                       1u, RPC_APP_MAX_KEY_SIZE, &key_size) != 0) {
        fprintf(stderr,
                "client: invalid --key-size (range 1B..%uB)\n",
                RPC_APP_MAX_KEY_SIZE);
        return 1;
    }
    if (parse_size_arg(argc, argv, "--value-size", value_size,
                       1u, (size_t)(RESPONSE_DATA_BYTES / 2u),
                       &value_size) != 0) {
        fprintf(stderr,
                "client: invalid --value-size (range 1B..%zuB)\n",
                (size_t)(RESPONSE_DATA_BYTES / 2u));
        return 1;
    }
    if (parse_u64_arg(argc, argv, "--record-count",
                      RPC_APP_DEFAULT_RECORD_COUNT,
                      1u, 100000000u, &record_count) != 0) {
        fprintf(stderr, "client: invalid --record-count\n");
        return 1;
    }
    if (parse_u64_arg(argc, argv, "--dataset-seed",
                      RPC_APP_DEFAULT_DATASET_SEED,
                      1u, UINT64_MAX, &dataset_seed) != 0) {
        fprintf(stderr, "client: invalid --dataset-seed\n");
        return 1;
    }
    if (parse_u64_arg(argc, argv, "--workload-seed",
                      RPC_APP_DEFAULT_WORKLOAD_SEED,
                      1u, UINT64_MAX, &workload_seed) != 0) {
        fprintf(stderr, "client: invalid --workload-seed\n");
        return 1;
    }
    if (parse_double_arg(argc, argv, "--read-ratio",
                         read_ratio, 0.0, 1.0, &read_ratio) != 0 ||
        parse_double_arg(argc, argv, "--update-ratio",
                         update_ratio, 0.0, 1.0, &update_ratio) != 0 ||
        parse_double_arg(argc, argv, "--rmw-ratio",
                         rmw_ratio, 0.0, 1.0, &rmw_ratio) != 0 ||
        (key_dist == RPC_APP_KEY_DIST_ZIPF &&
         parse_double_arg(argc, argv, "--zipf-theta",
                          zipf_theta, 0.01, 1.50, &zipf_theta) != 0)) {
        fprintf(stderr, "client: invalid workload ratio/theta argument\n");
        return 1;
    }
    if (fabs((read_ratio + update_ratio + rmw_ratio) - 1.0) > 1e-6) {
        fprintf(stderr,
                "client: read-ratio + update-ratio + rmw-ratio must equal 1.0\n");
        return 1;
    }
    variable_layout = rpc_app_profile_has_variable_layout(profile.name) &&
                      !key_size_overridden && !value_size_overridden;
    max_key_size = variable_layout ?
                   rpc_app_profile_max_key_size(profile.name, key_size) :
                   key_size;
    max_value_size = variable_layout ?
                     rpc_app_profile_max_value_size(profile.name, value_size) :
                     value_size;
    max_request_size = rpc_app_request_wire_size(RPC_APP_OP_PUT,
                                                 max_key_size, max_value_size);
    if (max_request_size > REQUEST_DATA_BYTES) {
        fprintf(stderr, "client: request size exceeds request-data capacity\n");
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    setlinebuf(stdout);
    setvbuf(stderr, NULL, _IONBF, 0);
    is_slow_client = (slow_client_count > 0 && node_id < slow_client_count);
    client_request_count = is_slow_client ? slow_count_per_client : num_requests;
    barrier_participant_count =
        first_round_barrier_participants(num_clients,
                                         num_requests,
                                         slow_client_count,
                                         slow_count_per_client);
    send_pause_iters = is_slow_client
        ? scale_sparse_send_pause_iters(slow_client_send_pause_iters,
                                        num_requests,
                                        client_request_count)
        : 0;
    rpc_markerf("init_begin",
                "profile=%s,node=%d,num_clients=%d,requests=%d,base_requests=%d,key_size=%zu,value_size=%zu,max_key_size=%zu,max_value_size=%zu,record_count=%llu,read_ratio=%.3f,update_ratio=%.3f,rmw_ratio=%.3f,window=%d,slow_client_count=%d,slow_count_per_client=%d,is_slow_client=%d,slow_client_send_pause_iters=%d,allow_miss=%d,variable_layout=%d",
                profile.name, node_id, num_clients, client_request_count,
                num_requests, key_size,
                value_size, max_key_size, max_value_size,
                (unsigned long long)record_count, read_ratio,
                update_ratio, rmw_ratio, sliding_window, slow_client_count,
                slow_count_per_client, is_slow_client, send_pause_iters,
                allow_miss, variable_layout);

    reserve_n = (client_request_count > 0) ? (size_t)client_request_count : 1u;
    ops = (app_operation_t *)calloc(reserve_n, sizeof(*ops));
    request_start_ticks = (uint64_t *)calloc(reserve_n, sizeof(uint64_t));
    request_end_ticks = (uint64_t *)calloc(reserve_n, sizeof(uint64_t));
    rpc_id_to_request_index =
        (int *)malloc((size_t)CLIENT_RPC_ID_SPACE *
                      sizeof(*rpc_id_to_request_index));
    req_buf = (uint8_t *)malloc(max_request_size);
    if (!ops || !request_start_ticks || !request_end_ticks ||
        !rpc_id_to_request_index || !req_buf) {
        fprintf(stderr, "client: allocation failed\n");
        rc = 1;
        goto cleanup;
    }
    memset(request_end_ticks, 0xFF, reserve_n * sizeof(*request_end_ticks));
    {
        size_t i;
        for (i = 0; i < (size_t)CLIENT_RPC_ID_SPACE; i++)
            rpc_id_to_request_index[i] = -1;
    }

    if (client_request_count > 0 &&
        build_operations(ops, (uint64_t)client_request_count, profile.name,
                         record_count, key_size, value_size, (uint64_t)node_id,
                         max_key_size,
                         read_ratio, update_ratio, rmw_ratio, key_dist,
                         zipf_theta,
                         dataset_seed,
                         workload_seed ^ (uint64_t)(node_id + 1)) != 0) {
        fprintf(stderr, "client: build workload operations failed\n");
        rc = 1;
        goto cleanup;
    }
    rpc_markerf("workload_ready", "profile=%s,requests=%d,record_count=%llu",
                profile.name, client_request_count,
                (unsigned long long)record_count);

    ctx = cxl_rpc_init(CXL_BASE, CXL_SIZE);
    if (!ctx) {
        fprintf(stderr, "client: cxl_rpc_init failed\n");
        rc = 1;
        goto cleanup;
    }

    {
        uint64_t client_base = node_region_base(node_id);
        uint64_t server_base = SERVER_REGION_BASE;
        cxl_connection_addrs_t addrs = {
            .doorbell_addr = server_base + DOORBELL_OFFSET +
                             ((uint64_t)(node_id + 1) * DOORBELL_STRIDE),
            .metadata_queue_addr = server_base + METADATA_Q_OFFSET,
            .metadata_queue_size = (uint32_t)METADATA_Q_SIZE_BYTES,
            .request_data_addr = client_base + REQUEST_DATA_OFFSET,
            .request_data_size = REQUEST_DATA_BYTES,
            .response_data_addr = client_base + RESPONSE_DATA_OFFSET,
            .response_data_size = RESPONSE_DATA_BYTES,
            .flag_addr = client_base + FLAG_OFFSET,
            .node_id = (uint16_t)node_id,
        };

        conn = cxl_connection_create_client_attach(ctx, &addrs);
        if (!conn) {
            fprintf(stderr, "client: connection_create_client_attach failed\n");
            rc = 1;
            goto cleanup;
        }
    }

    if (keep_running && client_request_count > 0) {
        int first_req_id = send_one_request(conn,
                                            req_buf, max_request_size,
                                            &ops[sent_requests],
                                            dataset_seed,
                                            rpc_id_to_request_index,
                                            request_start_ticks,
                                            sent_requests);
        if (first_req_id <= 0) {
            rc = 1;
            goto cleanup;
        }
        rpc_markerf("first_request_sent", "node=%d,rpc_id=%d,op=%u",
                    node_id, first_req_id, (unsigned)ops[sent_requests].op);
        sent_requests++;

        for (;;) {
            int round_rc = drain_completed_responses(conn,
                                                     ops,
                                                     allow_miss,
                                                     rpc_id_to_request_index,
                                                     request_end_ticks,
                                                     &completed_requests,
                                                     &first_response_marker_emitted);
            if (round_rc < 0) {
                rc = 1;
                goto cleanup;
            }
            if (completed_requests > 0)
                break;
            spin_pause_iters(poll_pause_iters);
        }

        if (completed_requests > 0 && barrier_participant_count > 1) {
            rpc_markerf("first_response_barrier_enter",
                        "node=%d,participants=%d,completed=%d",
                        node_id, barrier_participant_count,
                        completed_requests);
            if (rpc_wait_for_first_round_barrier(barrier_participant_count,
                                                 node_id,
                                                 &keep_running,
                                                 poll_pause_iters) != 0) {
                rc = 1;
                goto cleanup;
            }
            rpc_markerf("first_response_barrier_exit",
                        "node=%d,participants=%d,completed=%d",
                        node_id, barrier_participant_count,
                        completed_requests);
        }

    }

    while (keep_running && rc == 0 &&
           completed_requests < client_request_count) {
        int inflight = sent_requests - completed_requests;
        int can_send = (sent_requests < client_request_count);
        int should_poll = (inflight >= sliding_window) || !can_send;

        if (can_send && !should_poll) {
            int req_id;

            if (send_pause_iters > 0)
                spin_pause_iters(send_pause_iters);
            req_id = send_one_request(conn,
                                      req_buf, max_request_size,
                                      &ops[sent_requests],
                                      dataset_seed,
                                      rpc_id_to_request_index,
                                      request_start_ticks,
                                      sent_requests);
            if (req_id <= 0) {
                rc = 1;
                break;
            }
            sent_requests++;
            continue;
        }

        {
            int round_rc = drain_completed_responses(conn,
                                                     ops,
                                                     allow_miss,
                                                     rpc_id_to_request_index,
                                                     request_end_ticks,
                                                     &completed_requests,
                                                     &first_response_marker_emitted);
            if (round_rc < 0) {
                rc = 1;
                break;
            }
            if (round_rc == 0)
                spin_pause_iters(poll_pause_iters);
        }
    }

    if (sent_requests != client_request_count ||
        completed_requests != client_request_count)
        rc = 1;

cleanup:
    if (conn) {
        cxl_connection_destroy(conn);
        conn = NULL;
    }
    if (ctx) {
        cxl_rpc_destroy(ctx);
        ctx = NULL;
    }

    {
        int i;
        for (i = 0; i < sent_requests; i++) {
            if (request_end_ticks && request_end_ticks[i] == UINT64_MAX)
                continue;
            if (!request_start_ticks || !request_end_ticks)
                break;
            printf("req_%d_start_tick=%lu\n", i, request_start_ticks[i]);
            printf("req_%d_end_tick=%lu\n", i, request_end_ticks[i]);
        }
    }
    fflush(stdout);
    fflush(stderr);

    free(ops);
    free(request_start_ticks);
    free(request_end_ticks);
    free(rpc_id_to_request_index);
    free(req_buf);
    return rc ? 1 : 0;
}
