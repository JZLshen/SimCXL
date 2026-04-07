#ifndef RPC_MICA_COMMON_H
#define RPC_MICA_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RPC_APP_PROFILE_YCSB_C_1K "ycsb_c_1k"
#define RPC_APP_PROFILE_YCSB_1K_RO "ycsb_1k_ro"
#define RPC_APP_PROFILE_YCSB_A_1K "ycsb_a_1k"
#define RPC_APP_PROFILE_YCSB_B_1K "ycsb_b_1k"
#define RPC_APP_PROFILE_YCSB_D_1K "ycsb_d_1k"
#define RPC_APP_PROFILE_UDB_A "udb_a"
#define RPC_APP_PROFILE_UDB_B "udb_b"
#define RPC_APP_PROFILE_UDB_C "udb_c"
#define RPC_APP_PROFILE_UDB_D "udb_d"

#define RPC_APP_DEFAULT_RECORD_COUNT 10000ULL
#define RPC_APP_DEFAULT_DATASET_SEED 0x9B5D3A4781C26EF1ULL
#define RPC_APP_DEFAULT_WORKLOAD_SEED 0xC7D51A32049EF68BULL
#define RPC_APP_DEFAULT_ZIPF_THETA 0.99
#define RPC_APP_DEFAULT_YCSB_KEY_SIZE 16U
#define RPC_APP_DEFAULT_YCSB_VALUE_SIZE 1024U
#define RPC_APP_DEFAULT_UDB_KEY_SIZE 27U
#define RPC_APP_DEFAULT_UDB_VALUE_SIZE 127U
#define RPC_APP_DEFAULT_UDB_MAX_KEY_SIZE 39U
#define RPC_APP_DEFAULT_UDB_MAX_VALUE_SIZE 179U
#define RPC_APP_UDB_BIN_COUNT 10U
#define RPC_APP_MAX_KEY_SIZE 255U

#define RPC_APP_OP_GET 1u
#define RPC_APP_OP_PUT 2u
#define RPC_APP_OP_RMW 3u
#define RPC_APP_OP_INSERT 4u

#define RPC_APP_STATUS_OK 0u
#define RPC_APP_STATUS_MISS 1u
#define RPC_APP_STATUS_INVALID 2u

#define RPC_APP_BUCKET_SLOTS 8u

typedef enum {
    RPC_APP_KEY_DIST_UNIFORM = 0,
    RPC_APP_KEY_DIST_ZIPF = 1,
    RPC_APP_KEY_DIST_LATEST = 2,
} rpc_app_key_dist_t;

typedef struct {
    const char *name;
    size_t key_size;
    size_t value_size;
    double read_ratio;
    double update_ratio;
    double rmw_ratio;
    double insert_ratio;
    rpc_app_key_dist_t key_dist;
    double zipf_theta;
} rpc_app_profile_t;

typedef struct __attribute__((packed)) {
    uint8_t op;
    uint8_t key_len;
    uint16_t reserved0;
    uint32_t value_len;
    uint64_t key_hash;
} rpc_app_request_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t status;
    uint8_t op;
    uint16_t reserved0;
    uint32_t value_len;
    uint64_t value_checksum;
} rpc_app_response_hdr_t;

static const uint16_t rpc_app_udb_key_lengths[RPC_APP_UDB_BIN_COUNT] = {
    16u, 18u, 21u, 23u, 26u, 28u, 31u, 33u, 36u, 39u,
};

static const uint16_t rpc_app_udb_value_lengths[RPC_APP_UDB_BIN_COUNT] = {
    74u, 86u, 97u, 109u, 121u, 133u, 145u, 156u, 167u, 179u,
};

static inline uint64_t
rpc_app_rotl64(uint64_t v, unsigned int shift)
{
    return (v << shift) | (v >> (64u - shift));
}

static inline uint64_t
rpc_app_mix64(uint64_t v)
{
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33;
    v *= 0xc4ceb9fe1a85ec53ULL;
    v ^= v >> 33;
    return v;
}

static inline uint64_t
rpc_app_hash_bytes(const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint64_t acc = 0xCBF29CE484222325ULL ^ (uint64_t)len;
    size_t index = 0;

    while (index + sizeof(uint64_t) <= len) {
        uint64_t word = 0;
        memcpy(&word, bytes + index, sizeof(word));
        acc ^= rpc_app_mix64(word + index + 1u);
        acc = rpc_app_rotl64(acc, 7u) * 0x100000001B3ULL;
        index += sizeof(uint64_t);
    }

    if (index < len) {
        uint64_t tail = 0;
        memcpy(&tail, bytes + index, len - index);
        acc ^= rpc_app_mix64(tail + len);
        acc = rpc_app_rotl64(acc, 11u) * 0x100000001B3ULL;
    }

    return rpc_app_mix64(acc ^ len);
}

static inline uint64_t
rpc_app_checksum_bytes(const void *data, size_t len)
{
    return rpc_app_hash_bytes(data, len);
}

static inline void
rpc_app_fill_key(uint8_t *dst, size_t len, uint64_t dataset_seed,
                 uint64_t key_id)
{
    size_t offset = 0;

    if (!dst || len == 0)
        return;

    while (offset < len) {
        uint64_t word = rpc_app_mix64(dataset_seed ^
                                      (key_id * 0x9E3779B185EBCA87ULL) ^
                                      (uint64_t)(offset + 1));
        size_t chunk = len - offset;
        if (chunk > sizeof(word))
            chunk = sizeof(word);
        memcpy(dst + offset, &word, chunk);
        offset += chunk;
    }

    if (len >= sizeof(key_id))
        memcpy(dst, &key_id, sizeof(key_id));
}

static inline uint64_t
rpc_app_value_seed(uint64_t dataset_seed, uint64_t key_hash,
                   uint64_t update_version)
{
    return rpc_app_mix64(dataset_seed ^
                         (key_hash * 0xD6E8FEB86659FD93ULL) ^
                         (update_version * 0xA0761D6478BD642FULL));
}

static inline void
rpc_app_fill_value(uint8_t *dst, size_t len, uint64_t value_seed)
{
    size_t offset = 0;

    if (!dst || len == 0)
        return;

    while (offset < len) {
        uint64_t word = rpc_app_mix64(value_seed ^
                                      (uint64_t)(offset + 1) *
                                          0x94D049BB133111EBULL);
        size_t chunk = len - offset;
        if (chunk > sizeof(word))
            chunk = sizeof(word);
        memcpy(dst + offset, &word, chunk);
        offset += chunk;
    }
}

static inline size_t
rpc_app_request_wire_size(uint8_t op, size_t key_len, size_t value_len)
{
    return sizeof(rpc_app_request_hdr_t) + key_len +
           ((op == RPC_APP_OP_PUT || op == RPC_APP_OP_RMW ||
             op == RPC_APP_OP_INSERT) ? value_len : 0u);
}

static inline size_t
rpc_app_response_wire_size(uint8_t status, uint8_t op, size_t value_len)
{
    return sizeof(rpc_app_response_hdr_t) +
           ((status == RPC_APP_STATUS_OK &&
             (op == RPC_APP_OP_GET || op == RPC_APP_OP_RMW)) ?
                value_len :
                0u);
}

static inline int
rpc_app_profile_uses_udb_layout(const char *name)
{
    return name &&
           (strcmp(name, RPC_APP_PROFILE_UDB_A) == 0 ||
            strcmp(name, RPC_APP_PROFILE_UDB_B) == 0 ||
            strcmp(name, RPC_APP_PROFILE_UDB_C) == 0 ||
            strcmp(name, RPC_APP_PROFILE_UDB_D) == 0);
}

static inline int
rpc_app_profile_has_variable_layout(const char *name)
{
    return rpc_app_profile_uses_udb_layout(name);
}

static inline size_t
rpc_app_profile_max_key_size(const char *name, size_t default_key_size)
{
    return rpc_app_profile_has_variable_layout(name) ?
               RPC_APP_DEFAULT_UDB_MAX_KEY_SIZE :
               default_key_size;
}

static inline size_t
rpc_app_profile_max_value_size(const char *name, size_t default_value_size)
{
    return rpc_app_profile_has_variable_layout(name) ?
               RPC_APP_DEFAULT_UDB_MAX_VALUE_SIZE :
               default_value_size;
}

static inline size_t
rpc_app_select_discrete_index(uint64_t selector_seed, uint64_t key_id,
                              size_t choice_count)
{
    uint64_t mixed;

    if (choice_count == 0)
        return 0;

    mixed = rpc_app_mix64(selector_seed ^
                          (key_id * 0x9E3779B185EBCA87ULL));
    return (size_t)(mixed % choice_count);
}

static inline size_t
rpc_app_select_discrete_length(uint64_t selector_seed, uint64_t key_id,
                               const uint16_t *choices, size_t choice_count)
{
    if (!choices || choice_count == 0)
        return 0;

    return (size_t)choices[rpc_app_select_discrete_index(
        selector_seed, key_id, choice_count)];
}

static inline size_t
rpc_app_udb_key_bin_index(uint64_t dataset_seed, uint64_t key_id)
{
    return rpc_app_select_discrete_index(
        dataset_seed ^ 0xB8FE6C391B5C4F2DULL,
        key_id,
        RPC_APP_UDB_BIN_COUNT);
}

static inline size_t
rpc_app_udb_value_bin_index(uint64_t dataset_seed, uint64_t key_id)
{
    return rpc_app_select_discrete_index(
        dataset_seed ^ 0x4F1BBCDCBFA54001ULL,
        key_id,
        RPC_APP_UDB_BIN_COUNT);
}

static inline void
rpc_app_record_layout(const char *profile_name, uint64_t dataset_seed,
                      uint64_t key_id, size_t default_key_size,
                      size_t default_value_size, size_t *out_key_len,
                      size_t *out_value_len)
{
    if (rpc_app_profile_has_variable_layout(profile_name)) {
        /*
         * Use equal-probability bins for the UDB profile so the dataset layout
         * follows the same "uniform bins with the right average" contract as
         * the dedicated uniform payload experiments. The chosen bins keep the
         * mean close to the FAST'20 UDB RO point: key=27.1B, value=126.7B.
         */
        if (out_key_len) {
            *out_key_len = rpc_app_select_discrete_length(
                dataset_seed ^ 0xB8FE6C391B5C4F2DULL,
                key_id,
                rpc_app_udb_key_lengths,
                RPC_APP_UDB_BIN_COUNT);
        }
        if (out_value_len) {
            *out_value_len = rpc_app_select_discrete_length(
                dataset_seed ^ 0x4F1BBCDCBFA54001ULL,
                key_id,
                rpc_app_udb_value_lengths,
                RPC_APP_UDB_BIN_COUNT);
        }
        return;
    }

    if (out_key_len)
        *out_key_len = default_key_size;
    if (out_value_len)
        *out_value_len = default_value_size;
}

static inline int
rpc_app_lookup_profile(const char *name, rpc_app_profile_t *out)
{
    rpc_app_profile_t profile;

    if (!name || !out)
        return -1;

    if (strcmp(name, RPC_APP_PROFILE_YCSB_C_1K) == 0 ||
        strcmp(name, RPC_APP_PROFILE_YCSB_1K_RO) == 0) {
        profile.name = RPC_APP_PROFILE_YCSB_C_1K;
        profile.key_size = RPC_APP_DEFAULT_YCSB_KEY_SIZE;
        profile.value_size = RPC_APP_DEFAULT_YCSB_VALUE_SIZE;
        profile.read_ratio = 1.0;
        profile.update_ratio = 0.0;
        profile.rmw_ratio = 0.0;
        profile.insert_ratio = 0.0;
        profile.key_dist = RPC_APP_KEY_DIST_ZIPF;
        profile.zipf_theta = RPC_APP_DEFAULT_ZIPF_THETA;
    } else if (strcmp(name, RPC_APP_PROFILE_YCSB_A_1K) == 0) {
        profile.name = RPC_APP_PROFILE_YCSB_A_1K;
        profile.key_size = RPC_APP_DEFAULT_YCSB_KEY_SIZE;
        profile.value_size = RPC_APP_DEFAULT_YCSB_VALUE_SIZE;
        profile.read_ratio = 0.5;
        profile.update_ratio = 0.5;
        profile.rmw_ratio = 0.0;
        profile.insert_ratio = 0.0;
        profile.key_dist = RPC_APP_KEY_DIST_ZIPF;
        profile.zipf_theta = RPC_APP_DEFAULT_ZIPF_THETA;
    } else if (strcmp(name, RPC_APP_PROFILE_YCSB_B_1K) == 0) {
        profile.name = RPC_APP_PROFILE_YCSB_B_1K;
        profile.key_size = RPC_APP_DEFAULT_YCSB_KEY_SIZE;
        profile.value_size = RPC_APP_DEFAULT_YCSB_VALUE_SIZE;
        profile.read_ratio = 0.95;
        profile.update_ratio = 0.05;
        profile.rmw_ratio = 0.0;
        profile.insert_ratio = 0.0;
        profile.key_dist = RPC_APP_KEY_DIST_ZIPF;
        profile.zipf_theta = RPC_APP_DEFAULT_ZIPF_THETA;
    } else if (strcmp(name, RPC_APP_PROFILE_YCSB_D_1K) == 0) {
        profile.name = RPC_APP_PROFILE_YCSB_D_1K;
        profile.key_size = RPC_APP_DEFAULT_YCSB_KEY_SIZE;
        profile.value_size = RPC_APP_DEFAULT_YCSB_VALUE_SIZE;
        profile.read_ratio = 0.95;
        profile.update_ratio = 0.0;
        profile.rmw_ratio = 0.0;
        profile.insert_ratio = 0.05;
        profile.key_dist = RPC_APP_KEY_DIST_LATEST;
        profile.zipf_theta = 0.0;
    } else if (strcmp(name, RPC_APP_PROFILE_UDB_A) == 0) {
        profile.name = RPC_APP_PROFILE_UDB_A;
        profile.key_size = RPC_APP_DEFAULT_UDB_KEY_SIZE;
        profile.value_size = RPC_APP_DEFAULT_UDB_VALUE_SIZE;
        profile.read_ratio = 0.5;
        profile.update_ratio = 0.5;
        profile.rmw_ratio = 0.0;
        profile.insert_ratio = 0.0;
        profile.key_dist = RPC_APP_KEY_DIST_ZIPF;
        profile.zipf_theta = RPC_APP_DEFAULT_ZIPF_THETA;
    } else if (strcmp(name, RPC_APP_PROFILE_UDB_B) == 0) {
        profile.name = RPC_APP_PROFILE_UDB_B;
        profile.key_size = RPC_APP_DEFAULT_UDB_KEY_SIZE;
        profile.value_size = RPC_APP_DEFAULT_UDB_VALUE_SIZE;
        profile.read_ratio = 0.95;
        profile.update_ratio = 0.05;
        profile.rmw_ratio = 0.0;
        profile.insert_ratio = 0.0;
        profile.key_dist = RPC_APP_KEY_DIST_ZIPF;
        profile.zipf_theta = RPC_APP_DEFAULT_ZIPF_THETA;
    } else if (strcmp(name, RPC_APP_PROFILE_UDB_C) == 0) {
        profile.name = RPC_APP_PROFILE_UDB_C;
        profile.key_size = RPC_APP_DEFAULT_UDB_KEY_SIZE;
        profile.value_size = RPC_APP_DEFAULT_UDB_VALUE_SIZE;
        profile.read_ratio = 1.0;
        profile.update_ratio = 0.0;
        profile.rmw_ratio = 0.0;
        profile.insert_ratio = 0.0;
        profile.key_dist = RPC_APP_KEY_DIST_ZIPF;
        profile.zipf_theta = RPC_APP_DEFAULT_ZIPF_THETA;
    } else if (strcmp(name, RPC_APP_PROFILE_UDB_D) == 0) {
        profile.name = RPC_APP_PROFILE_UDB_D;
        profile.key_size = RPC_APP_DEFAULT_UDB_KEY_SIZE;
        profile.value_size = RPC_APP_DEFAULT_UDB_VALUE_SIZE;
        profile.read_ratio = 0.95;
        profile.update_ratio = 0.0;
        profile.rmw_ratio = 0.0;
        profile.insert_ratio = 0.05;
        profile.key_dist = RPC_APP_KEY_DIST_LATEST;
        profile.zipf_theta = 0.0;
    } else {
        return -1;
    }

    *out = profile;
    return 0;
}

#endif
