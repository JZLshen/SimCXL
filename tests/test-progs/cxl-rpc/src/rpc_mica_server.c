/*
 * Minimal application-path CXL RPC server.
 *
 * This server keeps a bucketed, in-memory KV index that is
 * intentionally MICA-like in structure without importing an external MICA
 * codebase. The preload phase runs before server_ready=1 so measured server
 * breakdowns still cover only poll / execute / response.
 *
 * Output contract (only):
 *   server_req_<i>_poll_tick=<u64>
 *   server_req_<i>_execute_tick=<u64>
 *   server_req_<i>_response_tick=<u64>
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#include "rpc_mica_common.h"

#define DEFAULT_MAX_REQUESTS 0
#define DEFAULT_IDLE_PAUSE_ITERS 0
#define DEFAULT_MQ_ENTRIES METADATA_Q_ENTRIES
#define DEFAULT_CLIENTS_PER_DMA_LANE 1

typedef struct {
    uint32_t record_index;
    uint16_t tag;
    uint8_t valid;
    uint8_t reserved0;
} mica_slot_t;

typedef struct {
    mica_slot_t slots[RPC_APP_BUCKET_SLOTS];
} mica_bucket_t;

typedef struct {
    uint64_t poll_tick;
    uint64_t execute_tick;
    uint64_t response_tick;
} server_request_timing_t;

typedef struct {
    uint8_t *keys;
    uint8_t *values;
    uint16_t *key_lengths;
    uint32_t *value_lengths;
    uint64_t *hashes;
    uint64_t *checksums;
    uint32_t *versions;
    mica_bucket_t *buckets;
    size_t record_count;
    size_t preload_count;
    size_t key_size;
    size_t value_size;
    size_t max_key_size;
    size_t max_value_size;
    size_t bucket_count;
    uint64_t dataset_seed;
    const char *profile_name;
} mica_store_t;

static volatile int keep_running = 1;

static const char *
request_rx_prefetch_mode_name(cxl_request_rx_prefetch_mode_t mode)
{
    switch (mode) {
    case CXL_REQUEST_RX_PREFETCH_FULL:
        return "full";
    case CXL_REQUEST_RX_PREFETCH_NO_REQUEST:
        return "no-request";
    case CXL_REQUEST_RX_PREFETCH_NONE:
        return "none";
    default:
        return "unknown";
    }
}

static inline uint64_t
current_tick(void)
{
    return m5_rpns();
}

static uint64_t
total_insert_count(uint64_t total_request_count, double insert_ratio)
{
    long long rounded;

    if (total_request_count == 0 || insert_ratio <= 0.0)
        return 0;

    rounded = llround(insert_ratio * (double)total_request_count);
    if (rounded < 0)
        rounded = 0;
    if ((uint64_t)rounded > total_request_count)
        rounded = (long long)total_request_count;
    return (uint64_t)rounded;
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

    fprintf(stderr, "rpc_marker,role=server,phase=%s,tick=%lu",
            phase ? phase : "unknown", current_tick());
    if (fmt && fmt[0] != '\0') {
        fputc(',', stderr);
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    fputc('\n', stderr);
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

static int
parse_int_arg(int argc, char **argv, const char *flag, int default_val)
{
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0) {
            int val = atoi(argv[i + 1]);
            return (val >= 0) ? val : default_val;
        }
    }
    return default_val;
}

static int
parse_int_arg_range(int argc, char **argv, const char *flag, int default_val,
                    int min_val, int max_val)
{
    for (int i = 1; i < argc - 1; i++) {
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
parse_u64_arg(int argc, char **argv, const char *flag, uint64_t default_val,
              uint64_t min_val, uint64_t max_val, uint64_t *out)
{
    if (!out)
        return -1;
    *out = default_val;

    for (int i = 1; i < argc - 1; i++) {
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
parse_double_arg(int argc, char **argv, const char *flag, double default_val,
                 double min_val, double max_val, double *out)
{
    if (!out)
        return -1;
    *out = default_val;

    for (int i = 1; i < argc - 1; i++) {
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
parse_size_literal(const char *s, size_t min_val, size_t max_val, size_t *out)
{
    if (!s || !out)
        return -1;

    char *end = NULL;
    unsigned long long raw = strtoull(s, &end, 10);
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
parse_size_arg(int argc, char **argv, const char *flag, size_t default_val,
               size_t min_val, size_t max_val, size_t *out)
{
    if (!out)
        return -1;
    *out = default_val;

    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0) {
            if (parse_size_literal(argv[i + 1], min_val, max_val, out) != 0)
                return -1;
            return 0;
        }
    }
    return 0;
}

static int
parse_prefetch_mode_arg(int argc, char **argv,
                        cxl_request_rx_prefetch_mode_t *out_mode)
{
    if (!out_mode)
        return -1;

    *out_mode = CXL_REQUEST_RX_PREFETCH_FULL;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--prefetch-mode") != 0)
            continue;

        if (strcmp(argv[i + 1], "full") == 0) {
            *out_mode = CXL_REQUEST_RX_PREFETCH_FULL;
            return 0;
        }
        if (strcmp(argv[i + 1], "no-request") == 0) {
            *out_mode = CXL_REQUEST_RX_PREFETCH_NO_REQUEST;
            return 0;
        }
        if (strcmp(argv[i + 1], "none") == 0) {
            *out_mode = CXL_REQUEST_RX_PREFETCH_NONE;
            return 0;
        }
        return -1;
    }

    return 0;
}

static int
parse_profile_arg(int argc, char **argv, const char *default_name,
                  rpc_app_profile_t *out)
{
    const char *profile_name = default_name;

    if (!out)
        return -1;

    for (int i = 1; i < argc - 1; i++) {
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

static void
signal_handler(int sig)
{
    (void)sig;
    keep_running = 0;
}

static size_t
next_power_of_two(size_t value)
{
    size_t out = 1;

    while (out < value)
        out <<= 1u;
    return out;
}

static uint16_t
mica_tag_from_hash(uint64_t hash)
{
    uint16_t tag = (uint16_t)((hash >> 48) & 0xFFFFu);
    return tag ? tag : 1u;
}

static int
mica_store_init(mica_store_t *store, const char *profile_name,
                size_t record_count, size_t preload_count, size_t key_size,
                size_t value_size, size_t max_key_size,
                size_t max_value_size, uint64_t dataset_seed)
{
    size_t bucket_count;

    if (!store || !profile_name || record_count == 0 || preload_count == 0 ||
        preload_count > record_count || key_size == 0 || value_size == 0 ||
        max_key_size < key_size ||
        max_value_size < value_size) {
        return -1;
    }

    memset(store, 0, sizeof(*store));
    store->profile_name = profile_name;
    store->record_count = record_count;
    store->preload_count = preload_count;
    store->key_size = key_size;
    store->value_size = value_size;
    store->max_key_size = max_key_size;
    store->max_value_size = max_value_size;
    store->dataset_seed = dataset_seed;

    bucket_count = next_power_of_two((record_count / 4u) + 1u);
    if (bucket_count < 64u)
        bucket_count = 64u;
    store->bucket_count = bucket_count;

    store->keys = (uint8_t *)malloc(record_count * max_key_size);
    store->values = (uint8_t *)malloc(record_count * max_value_size);
    store->key_lengths =
        (uint16_t *)malloc(record_count * sizeof(*store->key_lengths));
    store->value_lengths =
        (uint32_t *)malloc(record_count * sizeof(*store->value_lengths));
    store->hashes = (uint64_t *)malloc(record_count * sizeof(*store->hashes));
    store->checksums =
        (uint64_t *)malloc(record_count * sizeof(*store->checksums));
    store->versions =
        (uint32_t *)calloc(record_count, sizeof(*store->versions));
    store->buckets =
        (mica_bucket_t *)calloc(bucket_count, sizeof(*store->buckets));
    if (!store->keys || !store->values || !store->key_lengths ||
        !store->value_lengths || !store->hashes || !store->checksums ||
        !store->versions || !store->buckets) {
        return -1;
    }

    return 0;
}

static void
mica_store_destroy(mica_store_t *store)
{
    if (!store)
        return;

    free(store->keys);
    free(store->values);
    free(store->key_lengths);
    free(store->value_lengths);
    free(store->hashes);
    free(store->checksums);
    free(store->versions);
    free(store->buckets);
    memset(store, 0, sizeof(*store));
}

static int
mica_store_insert_index(mica_store_t *store, size_t record_index)
{
    uint64_t hash;
    uint16_t tag;
    size_t bucket_index;
    size_t probe;
    const uint8_t *key;

    if (!store || record_index >= store->record_count ||
        store->key_lengths[record_index] == 0 ||
        store->value_lengths[record_index] == 0) {
        return -1;
    }

    key = store->keys + (record_index * store->max_key_size);
    hash = store->hashes[record_index];
    tag = mica_tag_from_hash(hash);
    bucket_index = (size_t)(hash & (uint64_t)(store->bucket_count - 1u));

    for (probe = 0; probe < store->bucket_count; probe++) {
        mica_bucket_t *bucket =
            &store->buckets[(bucket_index + probe) & (store->bucket_count - 1u)];
        size_t slot_index;

        for (slot_index = 0; slot_index < RPC_APP_BUCKET_SLOTS; slot_index++) {
            mica_slot_t *slot = &bucket->slots[slot_index];
            if (!slot->valid) {
                slot->record_index = (uint32_t)record_index;
                slot->tag = tag;
                slot->valid = 1u;
                return 0;
            }
            if (slot->tag == tag &&
                store->hashes[slot->record_index] == hash &&
                store->key_lengths[slot->record_index] ==
                    store->key_lengths[record_index] &&
                memcmp(store->keys + ((size_t)slot->record_index *
                                      store->max_key_size),
                       key, store->key_lengths[record_index]) == 0) {
                return 0;
            }
        }
    }

    return -1;
}

static int
mica_store_preload(mica_store_t *store)
{
    size_t record_index;

    if (!store)
        return -1;

    for (record_index = 0; record_index < store->preload_count; record_index++) {
        uint8_t *key = store->keys + (record_index * store->max_key_size);
        uint8_t *value = store->values + (record_index * store->max_value_size);
        size_t key_len = 0;
        size_t value_len = 0;
        uint64_t hash;
        uint64_t value_seed;

        rpc_app_record_layout(store->profile_name, store->dataset_seed,
                         (uint64_t)record_index, store->key_size,
                         store->value_size, &key_len, &value_len);
        if (key_len == 0 || key_len > store->max_key_size ||
            value_len == 0 || value_len > store->max_value_size) {
            return -1;
        }
        store->key_lengths[record_index] = (uint16_t)key_len;
        store->value_lengths[record_index] = (uint32_t)value_len;

        rpc_app_fill_key(key, key_len, store->dataset_seed,
                         (uint64_t)record_index);
        hash = rpc_app_hash_bytes(key, key_len);
        value_seed = rpc_app_value_seed(store->dataset_seed, hash, 0u);
        rpc_app_fill_value(value, value_len, value_seed);
        store->hashes[record_index] = hash;
        store->checksums[record_index] =
            rpc_app_checksum_bytes(value, value_len);

        if (mica_store_insert_index(store, record_index) != 0)
            return -1;
    }

    return 0;
}

static int
mica_store_insert_value(mica_store_t *store, uint32_t record_index,
                        const uint8_t *key, size_t key_len, uint64_t key_hash,
                        const uint8_t *value, size_t value_len)
{
    uint8_t *dst_key;
    uint8_t *dst_value;

    if (!store || !key || !value || record_index >= store->record_count ||
        key_len == 0 || key_len > store->max_key_size ||
        value_len == 0 || value_len > store->max_value_size ||
        store->key_lengths[record_index] != 0 ||
        store->value_lengths[record_index] != 0) {
        return -1;
    }

    dst_key = store->keys + ((size_t)record_index * store->max_key_size);
    dst_value = store->values + ((size_t)record_index * store->max_value_size);
    memcpy(dst_key, key, key_len);
    memcpy(dst_value, value, value_len);
    store->key_lengths[record_index] = (uint16_t)key_len;
    store->value_lengths[record_index] = (uint32_t)value_len;
    store->hashes[record_index] = key_hash;
    store->checksums[record_index] =
        rpc_app_checksum_bytes(dst_value, value_len);
    store->versions[record_index] = 0;
    if (mica_store_insert_index(store, record_index) != 0) {
        store->key_lengths[record_index] = 0;
        store->value_lengths[record_index] = 0;
        store->hashes[record_index] = 0;
        store->checksums[record_index] = 0;
        store->versions[record_index] = 0;
        return -1;
    }
    return 0;
}

static int
mica_store_find(const mica_store_t *store, const uint8_t *key, size_t key_len,
                uint64_t key_hash, uint32_t *out_record_index)
{
    size_t bucket_index;
    size_t probe;
    uint16_t tag;

    if (!store || !key || key_len == 0 || key_len > store->max_key_size)
        return 0;

    tag = mica_tag_from_hash(key_hash);
    bucket_index = (size_t)(key_hash & (uint64_t)(store->bucket_count - 1u));

    for (probe = 0; probe < store->bucket_count; probe++) {
        const mica_bucket_t *bucket =
            &store->buckets[(bucket_index + probe) & (store->bucket_count - 1u)];
        size_t slot_index;
        int saw_empty = 0;

        for (slot_index = 0; slot_index < RPC_APP_BUCKET_SLOTS; slot_index++) {
            const mica_slot_t *slot = &bucket->slots[slot_index];
            if (!slot->valid) {
                saw_empty = 1;
                continue;
            }
            if (slot->tag != tag)
                continue;
            if (store->hashes[slot->record_index] != key_hash)
                continue;
            if (store->key_lengths[slot->record_index] != key_len)
                continue;
            if (memcmp(store->keys + ((size_t)slot->record_index *
                                      store->max_key_size),
                       key, key_len) != 0) {
                continue;
            }
            if (out_record_index)
                *out_record_index = slot->record_index;
            return 1;
        }

        if (saw_empty)
            return 0;
    }

    return 0;
}

static int
mica_store_update_value(mica_store_t *store, uint32_t record_index,
                        const uint8_t *value, size_t value_len)
{
    uint8_t *dst;

    if (!store || !value || record_index >= store->record_count ||
        value_len != store->value_lengths[record_index]) {
        return -1;
    }

    dst = store->values + ((size_t)record_index * store->max_value_size);
    memcpy(dst, value, value_len);
    store->versions[record_index]++;
    store->checksums[record_index] =
        rpc_app_checksum_bytes(dst, value_len);
    return 0;
}

int
main(int argc, char **argv)
{
    int rc = 0;
    int first_poll_marker_emitted = 0;
    int first_resp_marker_emitted = 0;
    uint64_t requests_processed = 0;
    cxl_context_t *ctx = NULL;
    cxl_connection_t *poll_conn = NULL;
    cxl_connection_t *resp_conns[MAX_CLIENTS] = {0};
    uint8_t *resp_payload = NULL;
    server_request_timing_t *timings = NULL;
    uint64_t poll_phase_start_tick = 0;
    mica_store_t store;

    int max_requests = parse_int_arg(argc, argv, "--max-requests",
                                     DEFAULT_MAX_REQUESTS);
    int idle_pause_iters = parse_int_arg(argc, argv, "--idle-pause",
                                         DEFAULT_IDLE_PAUSE_ITERS);
    int num_clients = parse_int_arg_range(argc, argv, "--num-clients", 1,
                                          1, MAX_CLIENTS);
    int mq_entries = parse_int_arg_range(argc, argv, "--mq-entries",
                                         DEFAULT_MQ_ENTRIES,
                                         1, (int)METADATA_Q_ENTRIES);
    int head_sync_threshold_default = mq_entries / 4;
    int head_sync_threshold =
        has_flag(argc, argv, "--head-sync-threshold")
            ? parse_int_arg_range(argc, argv, "--head-sync-threshold",
                                  head_sync_threshold_default,
                                  0, mq_entries)
            : head_sync_threshold_default;
    int clients_per_dma_lane = parse_int_arg_range(argc, argv,
                                                   "--clients-per-dma-lane",
                                                   DEFAULT_CLIENTS_PER_DMA_LANE,
                                                   1, MAX_CLIENTS);
    cxl_request_rx_prefetch_mode_t prefetch_mode =
        CXL_REQUEST_RX_PREFETCH_FULL;
    rpc_app_profile_t profile;
    size_t key_size = 0;
    size_t value_size = 0;
    size_t max_key_size = 0;
    size_t max_response_size = 0;
    size_t max_value_size = 0;
    size_t response_dma_threshold =
        (size_t)CXL_RESPONSE_DMA_PAYLOAD_THRESHOLD_DEFAULT;
    uint64_t record_count = RPC_APP_DEFAULT_RECORD_COUNT;
    uint64_t dataset_seed = RPC_APP_DEFAULT_DATASET_SEED;
    double insert_ratio = 0.0;
    int key_size_overridden = has_flag(argc, argv, "--key-size");
    int value_size_overridden = has_flag(argc, argv, "--value-size");
    int variable_layout = 0;

    memset(&store, 0, sizeof(store));

    if (parse_profile_arg(argc, argv, RPC_APP_PROFILE_YCSB_C_1K,
                          &profile) != 0) {
        fprintf(stderr,
                "server: invalid --profile "
                "(use %s|%s|%s|%s|%s|%s|%s|%s; alias %s also accepted)\n",
                RPC_APP_PROFILE_YCSB_C_1K,
                RPC_APP_PROFILE_YCSB_A_1K,
                RPC_APP_PROFILE_YCSB_B_1K,
                RPC_APP_PROFILE_YCSB_D_1K,
                RPC_APP_PROFILE_UDB_A,
                RPC_APP_PROFILE_UDB_B,
                RPC_APP_PROFILE_UDB_C,
                RPC_APP_PROFILE_UDB_D,
                RPC_APP_PROFILE_YCSB_1K_RO);
        return 1;
    }

    key_size = profile.key_size;
    value_size = profile.value_size;
    insert_ratio = profile.insert_ratio;
    if (parse_size_arg(argc, argv, "--key-size", key_size,
                       1u, RPC_APP_MAX_KEY_SIZE, &key_size) != 0) {
        fprintf(stderr,
                "server: invalid --key-size (range 1B..%uB)\n",
                RPC_APP_MAX_KEY_SIZE);
        return 1;
    }
    if (parse_size_arg(argc, argv, "--value-size", value_size,
                       1u, (size_t)(RESPONSE_DATA_BYTES / 2u),
                       &value_size) != 0) {
        fprintf(stderr,
                "server: invalid --value-size (range 1B..%zuB)\n",
                (size_t)(RESPONSE_DATA_BYTES / 2u));
        return 1;
    }
    if (parse_size_arg(argc, argv, "--response-dma-threshold",
                       (size_t)CXL_RESPONSE_DMA_PAYLOAD_THRESHOLD_DEFAULT,
                       1, RESPONSE_DATA_BYTES,
                       &response_dma_threshold) != 0) {
        fprintf(stderr,
                "server: invalid --response-dma-threshold (range 1B..%zuB)\n",
                (size_t)RESPONSE_DATA_BYTES);
        return 1;
    }
    if (parse_prefetch_mode_arg(argc, argv, &prefetch_mode) != 0) {
        fprintf(stderr,
                "server: invalid --prefetch-mode "
                "(use full|no-request|none)\n");
        return 1;
    }
    if (parse_u64_arg(argc, argv, "--record-count",
                      RPC_APP_DEFAULT_RECORD_COUNT,
                      1u, 100000000u,
                      &record_count) != 0) {
        fprintf(stderr,
                "server: invalid --record-count "
                "(range 1..100000000)\n");
        return 1;
    }
    if (parse_u64_arg(argc, argv, "--dataset-seed",
                      RPC_APP_DEFAULT_DATASET_SEED,
                      1u, UINT64_MAX,
                      &dataset_seed) != 0) {
        fprintf(stderr, "server: invalid --dataset-seed\n");
        return 1;
    }
    if (parse_double_arg(argc, argv, "--insert-ratio",
                         insert_ratio, 0.0, 1.0, &insert_ratio) != 0) {
        fprintf(stderr, "server: invalid --insert-ratio\n");
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

    max_response_size =
        rpc_app_response_wire_size(RPC_APP_STATUS_OK, RPC_APP_OP_GET,
                                   max_value_size);
    if (max_response_size > RESPONSE_DATA_BYTES) {
        fprintf(stderr,
                "server: response payload exceeds response ring capacity\n");
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    setlinebuf(stdout);
    setvbuf(stderr, NULL, _IONBF, 0);
    rpc_markerf("init_begin",
                "profile=%s,num_clients=%d,max_requests=%d,key_size=%zu,value_size=%zu,max_key_size=%zu,max_value_size=%zu,record_count=%llu,insert_ratio=%.3f,mq_entries=%d,head_sync_threshold=%d,response_dma_threshold=%zu,clients_per_dma_lane=%d,prefetch_mode=%s,variable_layout=%d",
                profile.name, num_clients, max_requests, key_size, value_size,
                max_key_size, max_value_size,
                (unsigned long long)record_count, insert_ratio, mq_entries,
                head_sync_threshold, response_dma_threshold,
                clients_per_dma_lane,
                request_rx_prefetch_mode_name(prefetch_mode),
                variable_layout);

    rpc_markerf("preload_begin", "profile=%s,record_count=%llu",
                profile.name, (unsigned long long)record_count);
    {
        uint64_t insert_count = total_insert_count(
            (uint64_t)num_clients * (uint64_t)max_requests,
            insert_ratio);
        size_t preload_count = (size_t)record_count;

        if (profile.key_dist == RPC_APP_KEY_DIST_LATEST) {
            if (insert_count == 0 || insert_count >= record_count) {
                fprintf(stderr, "server: invalid latest workload preload sizing\n");
                rc = 1;
                goto cleanup;
            }
            preload_count = (size_t)(record_count - insert_count);
        }

        if (mica_store_init(&store, profile.name, (size_t)record_count,
                            preload_count,
                            key_size, value_size, max_key_size, max_value_size,
                            dataset_seed) != 0 ||
            mica_store_preload(&store) != 0) {
            fprintf(stderr, "server: preload KV initialization failed\n");
            rc = 1;
            goto cleanup;
        }
    }
    rpc_markerf("preload_done", "profile=%s,record_count=%llu,buckets=%zu",
                profile.name, (unsigned long long)record_count,
                store.bucket_count);

    ctx = cxl_rpc_init(CXL_BASE, CXL_SIZE);
    if (!ctx) {
        fprintf(stderr, "server: cxl_rpc_init failed\n");
        rc = 1;
        goto cleanup;
    }
    rpc_markerf("ctx_ready", "num_clients=%d", num_clients);

    {
        uint64_t server_base = SERVER_REGION_BASE;
        cxl_connection_addrs_t addrs = {
            .doorbell_addr = server_base + DOORBELL_OFFSET,
            .metadata_queue_addr = server_base + METADATA_Q_OFFSET,
            .metadata_queue_size = METADATA_Q_SIZE_BYTES,
            .request_data_addr = 0,
            .request_data_size = 0,
            .response_data_addr = server_base + RESPONSE_DATA_OFFSET,
            .response_data_size = RESPONSE_DATA_BYTES,
            .flag_addr = server_base + FLAG_OFFSET,
            .node_id = 0,
        };

        rpc_markerf("poll_conn_begin", "mq_entries=%d", mq_entries);
        poll_conn = cxl_connection_create_server_poll_owner(ctx, &addrs,
                                                            (uint32_t)mq_entries);
        if (!poll_conn) {
            fprintf(stderr, "server: connection_create_server_poll_owner failed\n");
            rc = 1;
            goto cleanup;
        }
        if (cxl_connection_set_head_sync_threshold(poll_conn,
                                                   (uint32_t)head_sync_threshold) < 0) {
            fprintf(stderr, "server: set head sync threshold failed\n");
            rc = 1;
            goto cleanup;
        }
        if (cxl_connection_set_request_rx_prefetch_mode(poll_conn,
                                                        prefetch_mode) < 0) {
            fprintf(stderr, "server: set request-rx prefetch mode failed\n");
            rc = 1;
            goto cleanup;
        }
        rpc_markerf("poll_conn_ready",
                    "mq_entries=%d,head_sync_threshold=%d,prefetch_mode=%s",
                    mq_entries, head_sync_threshold,
                    request_rx_prefetch_mode_name(prefetch_mode));
    }

    {
        int i;
        for (i = 0; i < num_clients; i++) {
            resp_conns[i] = cxl_connection_create_response_tx(ctx);
            if (!resp_conns[i]) {
                fprintf(stderr, "server: create response-tx connection failed\n");
                rc = 1;
                goto cleanup;
            }

            if (cxl_connection_set_response_dma_threshold(resp_conns[i],
                                                          response_dma_threshold) < 0) {
                fprintf(stderr, "server: set response DMA threshold failed\n");
                rc = 1;
                goto cleanup;
            }

            if (max_response_size >= response_dma_threshold &&
                cxl_connection_bind_copyengine_lane_index(resp_conns[i],
                                                          (size_t)(i / clients_per_dma_lane)) < 0) {
                fprintf(stderr, "server: bind CopyEngine lane failed\n");
                rc = 1;
                goto cleanup;
            }

            if (cxl_connection_set_peer_response_data(resp_conns[i],
                                                      node_region_base(i) +
                                                          RESPONSE_DATA_OFFSET,
                                                      RESPONSE_DATA_BYTES) < 0) {
                fprintf(stderr, "server: set peer response range failed\n");
                rc = 1;
                goto cleanup;
            }
            if (cxl_connection_set_peer_response_flag_addr(resp_conns[i],
                                                           node_region_base(i) +
                                                               FLAG_OFFSET) < 0) {
                fprintf(stderr, "server: set peer flag failed\n");
                rc = 1;
                goto cleanup;
            }
        }
    }

    if (max_requests > 0) {
        timings = (server_request_timing_t *)calloc((size_t)max_requests,
                                                    sizeof(*timings));
        if (!timings) {
            fprintf(stderr, "server: allocate timing buffer failed\n");
            rc = 1;
            goto cleanup;
        }
    }

    resp_payload = (uint8_t *)malloc(max_response_size);
    if (!resp_payload) {
        fprintf(stderr, "server: allocate response buffer failed\n");
        rc = 1;
        goto cleanup;
    }

    printf("server_ready=1\n");
    rpc_markerf("server_ready", "profile=%s,num_clients=%d,prefetch_mode=%s",
                profile.name, num_clients,
                request_rx_prefetch_mode_name(prefetch_mode));
    poll_phase_start_tick = current_tick();

    while (keep_running) {
        uint16_t node_id = 0;
        uint16_t rpc_id = 0;
        const void *req_data_view = NULL;
        size_t req_len = 0;
        cxl_request_poll_timing_t poll_timing = {0};
        uint64_t poll_done_tick = 0;
        uint64_t exec_end_tick = 0;
        uint64_t resp_end_tick = 0;
        int ret = cxl_poll_request_timed(poll_conn,
                                         &node_id,
                                         &rpc_id,
                                         &req_data_view,
                                         &req_len,
                                         &poll_timing);
        if (ret == 1) {
            const rpc_app_request_hdr_t *req_hdr;
            const uint8_t *key_ptr;
            const uint8_t *value_ptr;
            uint32_t record_index = 0;
            rpc_app_response_hdr_t *resp_hdr;
            size_t response_len;
            int found;

            poll_done_tick = (poll_timing.poll_done_tick != 0) ?
                             poll_timing.poll_done_tick :
                             current_tick();
            if (!first_poll_marker_emitted) {
                rpc_markerf("first_request_polled",
                            "node=%u,rpc_id=%u,req_len=%zu",
                            (unsigned)node_id, (unsigned)rpc_id, req_len);
                first_poll_marker_emitted = 1;
            }
            if (node_id >= (uint16_t)num_clients || req_len < sizeof(*req_hdr) ||
                !req_data_view) {
                fprintf(stderr, "server: invalid request envelope\n");
                rc = 1;
                break;
            }

            req_hdr = (const rpc_app_request_hdr_t *)req_data_view;
            key_ptr = (const uint8_t *)(req_hdr + 1);
            value_ptr = key_ptr + req_hdr->key_len;
            if (req_hdr->key_len == 0 || req_hdr->key_len > max_key_size ||
                (req_hdr->op != RPC_APP_OP_GET &&
                 req_hdr->op != RPC_APP_OP_PUT &&
                 req_hdr->op != RPC_APP_OP_RMW &&
                 req_hdr->op != RPC_APP_OP_INSERT) ||
                rpc_app_request_wire_size(req_hdr->op, req_hdr->key_len,
                                          req_hdr->value_len) != req_len) {
                fprintf(stderr, "server: malformed application request\n");
                rc = 1;
                break;
            }
            if ((req_hdr->op == RPC_APP_OP_PUT ||
                 req_hdr->op == RPC_APP_OP_RMW ||
                 req_hdr->op == RPC_APP_OP_INSERT) &&
                (req_hdr->value_len == 0 ||
                 req_hdr->value_len > max_value_size)) {
                fprintf(stderr, "server: PUT/RMW/INSERT value size out of range\n");
                rc = 1;
                break;
            }

            found = mica_store_find(&store, key_ptr, req_hdr->key_len,
                                    req_hdr->key_hash, &record_index);
            resp_hdr = (rpc_app_response_hdr_t *)resp_payload;
            resp_hdr->status = found ? RPC_APP_STATUS_OK : RPC_APP_STATUS_MISS;
            resp_hdr->op = req_hdr->op;
            resp_hdr->reserved0 = 0;
            resp_hdr->value_len = 0;
            resp_hdr->value_checksum = 0;

            if (found && req_hdr->op == RPC_APP_OP_GET) {
                uint8_t *dst = (uint8_t *)(resp_hdr + 1);
                const uint8_t *src =
                    store.values + ((size_t)record_index * store.max_value_size);
                uint32_t actual_value_len = store.value_lengths[record_index];

                memcpy(dst, src, actual_value_len);
                resp_hdr->value_len = actual_value_len;
                resp_hdr->value_checksum = store.checksums[record_index];
            } else if (found && req_hdr->op == RPC_APP_OP_PUT) {
                if (req_hdr->value_len != store.value_lengths[record_index]) {
                    fprintf(stderr, "server: PUT value size mismatch\n");
                    rc = 1;
                    break;
                }
                if (mica_store_update_value(&store, record_index,
                                            value_ptr, req_hdr->value_len) != 0) {
                    fprintf(stderr, "server: PUT update failed\n");
                    rc = 1;
                    break;
                }
            } else if (found && req_hdr->op == RPC_APP_OP_RMW) {
                uint8_t *dst = (uint8_t *)(resp_hdr + 1);
                const uint8_t *src =
                    store.values + ((size_t)record_index * store.max_value_size);
                uint32_t actual_value_len = store.value_lengths[record_index];

                if (req_hdr->value_len != actual_value_len) {
                    fprintf(stderr, "server: RMW value size mismatch\n");
                    rc = 1;
                    break;
                }
                memcpy(dst, src, actual_value_len);
                resp_hdr->value_len = actual_value_len;
                resp_hdr->value_checksum = store.checksums[record_index];
                if (mica_store_update_value(&store, record_index,
                                            value_ptr, req_hdr->value_len) != 0) {
                    fprintf(stderr, "server: RMW update failed\n");
                    rc = 1;
                    break;
                }
            } else if (!found && req_hdr->op == RPC_APP_OP_INSERT) {
                uint64_t key_id = 0;

                if (req_hdr->key_len < sizeof(uint64_t)) {
                    fprintf(stderr, "server: INSERT key too short\n");
                    rc = 1;
                    break;
                }
                memcpy(&key_id, key_ptr, sizeof(key_id));
                if (key_id >= store.record_count) {
                    fprintf(stderr, "server: INSERT key id out of range\n");
                    rc = 1;
                    break;
                }
                if (mica_store_insert_value(&store,
                                            (uint32_t)key_id,
                                            key_ptr,
                                            req_hdr->key_len,
                                            req_hdr->key_hash,
                                            value_ptr,
                                            req_hdr->value_len) != 0) {
                    fprintf(stderr, "server: INSERT failed\n");
                    rc = 1;
                    break;
                }
                resp_hdr->status = RPC_APP_STATUS_OK;
            }
            exec_end_tick = current_tick();

            response_len = rpc_app_response_wire_size(resp_hdr->status,
                                                      resp_hdr->op,
                                                      resp_hdr->value_len);
            if (cxl_send_response(resp_conns[node_id], rpc_id,
                                  resp_payload, response_len) < 0) {
                fprintf(stderr, "server: send response failed\n");
                rc = 1;
                break;
            }
            resp_end_tick = current_tick();
            if (!first_resp_marker_emitted) {
                rpc_markerf("first_response_submitted",
                            "node=%u,rpc_id=%u,response_len=%zu",
                            (unsigned)node_id, (unsigned)rpc_id, response_len);
                first_resp_marker_emitted = 1;
            }
            if (timings && requests_processed < (uint64_t)max_requests) {
                server_request_timing_t *record = &timings[requests_processed];
                record->poll_tick =
                    (poll_done_tick >= poll_phase_start_tick) ?
                    (poll_done_tick - poll_phase_start_tick) : 0;
                record->execute_tick =
                    (exec_end_tick >= poll_done_tick) ?
                    (exec_end_tick - poll_done_tick) : 0;
                record->response_tick =
                    (resp_end_tick >= exec_end_tick) ?
                    (resp_end_tick - exec_end_tick) : 0;
            }
            poll_phase_start_tick = resp_end_tick;
            requests_processed++;
            if (max_requests > 0 &&
                requests_processed >= (uint64_t)max_requests) {
                break;
            }
        } else if (ret == 0) {
            int i;
            for (i = 0; i < idle_pause_iters && keep_running; i++)
                __asm__ __volatile__("pause" ::: "memory");
        } else {
            fprintf(stderr, "server: poll request failed\n");
            rc = 1;
            break;
        }
    }

cleanup:
    if (timings) {
        uint64_t i;
        for (i = 0; i < requests_processed && i < (uint64_t)max_requests; i++) {
            const server_request_timing_t *record = &timings[i];
            printf("server_req_%lu_poll_tick=%lu\n",
                   (unsigned long)i, record->poll_tick);
            printf("server_req_%lu_execute_tick=%lu\n",
                   (unsigned long)i, record->execute_tick);
            printf("server_req_%lu_response_tick=%lu\n",
                   (unsigned long)i, record->response_tick);
        }
    }
    fflush(stdout);
    fflush(stderr);

    free(timings);
    free(resp_payload);

    {
        int i;
        for (i = 0; i < num_clients; i++) {
            if (resp_conns[i] && resp_conns[i] != poll_conn) {
                cxl_connection_destroy(resp_conns[i]);
                resp_conns[i] = NULL;
            }
        }
    }

    if (poll_conn) {
        cxl_connection_destroy(poll_conn);
        poll_conn = NULL;
    }
    if (ctx) {
        cxl_rpc_destroy(ctx);
        ctx = NULL;
    }

    mica_store_destroy(&store);
    return rc ? 1 : 0;
}
