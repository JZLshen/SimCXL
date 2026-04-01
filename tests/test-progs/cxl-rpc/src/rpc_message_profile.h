#ifndef RPC_MESSAGE_PROFILE_H
#define RPC_MESSAGE_PROFILE_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Bare-RPC message profiles used for overall/sensitivity experiments.
 *
 * These are deterministic approximations derived from published workload
 * summaries, not trace replays. The runtime uses a per-client per-op hash to
 * sample one request/response size pair from the chosen profile.
 */

typedef enum {
    RPC_MESSAGE_PROFILE_FIXED = 0,
    RPC_MESSAGE_PROFILE_GOOGLE_RPC = 1,
    RPC_MESSAGE_PROFILE_TWITTER_TWEMCACHE = 2,
} rpc_message_profile_t;

typedef struct __attribute__((packed)) {
    uint32_t lhs;
    uint32_t rhs;
    uint32_t op_index;
} rpc_profiled_request_t;

static inline const char *
rpc_message_profile_name(rpc_message_profile_t profile)
{
    switch (profile) {
    case RPC_MESSAGE_PROFILE_FIXED:
        return "fixed";
    case RPC_MESSAGE_PROFILE_GOOGLE_RPC:
        return "google-rpc";
    case RPC_MESSAGE_PROFILE_TWITTER_TWEMCACHE:
        return "twitter-twemcache";
    default:
        return "unknown";
    }
}

static inline int
rpc_message_profile_parse(const char *name, rpc_message_profile_t *out)
{
    if (!name || !out)
        return -1;
    if (strcmp(name, "fixed") == 0) {
        *out = RPC_MESSAGE_PROFILE_FIXED;
        return 0;
    }
    if (strcmp(name, "google-rpc") == 0) {
        *out = RPC_MESSAGE_PROFILE_GOOGLE_RPC;
        return 0;
    }
    if (strcmp(name, "twitter-twemcache") == 0) {
        *out = RPC_MESSAGE_PROFILE_TWITTER_TWEMCACHE;
        return 0;
    }
    return -1;
}

static inline int
rpc_message_profile_is_distribution(rpc_message_profile_t profile)
{
    return profile != RPC_MESSAGE_PROFILE_FIXED;
}

static inline size_t
rpc_message_profile_nominal_request_size(rpc_message_profile_t profile)
{
    switch (profile) {
    case RPC_MESSAGE_PROFILE_GOOGLE_RPC:
        return 1530u;
    case RPC_MESSAGE_PROFILE_TWITTER_TWEMCACHE:
        return 38u;
    case RPC_MESSAGE_PROFILE_FIXED:
    default:
        return 0u;
    }
}

static inline size_t
rpc_message_profile_nominal_response_size(rpc_message_profile_t profile)
{
    switch (profile) {
    case RPC_MESSAGE_PROFILE_GOOGLE_RPC:
        return 315u;
    case RPC_MESSAGE_PROFILE_TWITTER_TWEMCACHE:
        return 230u;
    case RPC_MESSAGE_PROFILE_FIXED:
    default:
        return 0u;
    }
}

static inline size_t
rpc_message_profile_max_request_size(rpc_message_profile_t profile)
{
    switch (profile) {
    case RPC_MESSAGE_PROFILE_GOOGLE_RPC:
        return 256u * 1024u;
    case RPC_MESSAGE_PROFILE_TWITTER_TWEMCACHE:
        return 256u;
    case RPC_MESSAGE_PROFILE_FIXED:
    default:
        return 0u;
    }
}

static inline size_t
rpc_message_profile_max_response_size(rpc_message_profile_t profile)
{
    switch (profile) {
    case RPC_MESSAGE_PROFILE_GOOGLE_RPC:
        return 1024u * 1024u;
    case RPC_MESSAGE_PROFILE_TWITTER_TWEMCACHE:
        return 10u * 1024u;
    case RPC_MESSAGE_PROFILE_FIXED:
    default:
        return 0u;
    }
}

static inline uint64_t
rpc_message_profile_mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static inline double
rpc_message_profile_u01(uint64_t x)
{
    const uint64_t mantissa = rpc_message_profile_mix64(x) >> 11;
    return (double)mantissa * (1.0 / 9007199254740992.0);
}

static inline size_t
rpc_message_profile_interp_log_quantile(double u,
                                        const double *quantiles,
                                        const size_t *sizes,
                                        size_t count)
{
    if (!quantiles || !sizes || count == 0)
        return 0u;
    if (u <= quantiles[0])
        return sizes[0];

    for (size_t i = 1; i < count; i++) {
        if (u > quantiles[i] && i + 1 < count)
            continue;

        const double lo_q = quantiles[i - 1];
        const double hi_q = quantiles[i];
        const size_t lo_size = sizes[i - 1];
        const size_t hi_size = sizes[i];
        const double span = hi_q - lo_q;
        const double t = (span > 0.0) ? ((u - lo_q) / span) : 0.0;
        const double value = exp(log((double)lo_size) +
                                 ((log((double)hi_size) -
                                   log((double)lo_size)) * t));
        long rounded = lround(value);
        if (rounded < 1L)
            rounded = 1L;
        return (size_t)rounded;
    }

    return sizes[count - 1];
}

static inline int
rpc_message_profile_sample_sizes(rpc_message_profile_t profile,
                                 uint16_t node_id,
                                 uint32_t op_index,
                                 size_t *out_request_size,
                                 size_t *out_response_size)
{
    if (!out_request_size || !out_response_size)
        return -1;

    const uint64_t base =
        ((uint64_t)(profile + 1u) << 56) ^
        ((uint64_t)(node_id + 1u) << 32) ^
        (uint64_t)(op_index + 1u);
    const double u = rpc_message_profile_u01(base);

    switch (profile) {
    case RPC_MESSAGE_PROFILE_GOOGLE_RPC: {
        static const double req_quantiles[] = {0.0, 0.5, 0.9, 0.99, 1.0};
        static const size_t req_sizes[] = {
            64u,
            1530u,
            11800u,
            196000u,
            256u * 1024u,
        };
        static const double resp_quantiles[] = {0.0, 0.5, 0.9, 0.99, 1.0};
        static const size_t resp_sizes[] = {
            64u,
            315u,
            10000u,
            563000u,
            1024u * 1024u,
        };
        *out_request_size = rpc_message_profile_interp_log_quantile(
            u, req_quantiles, req_sizes, sizeof(req_sizes) / sizeof(req_sizes[0]));
        *out_response_size = rpc_message_profile_interp_log_quantile(
            u, resp_quantiles, resp_sizes,
            sizeof(resp_sizes) / sizeof(resp_sizes[0]));
        return 0;
    }
    case RPC_MESSAGE_PROFILE_TWITTER_TWEMCACHE: {
        static const double req_quantiles[] = {0.0, 0.5, 0.85, 0.99, 1.0};
        static const size_t req_sizes[] = {
            16u,
            38u,
            50u,
            72u,
            256u,
        };
        static const double resp_quantiles[] = {0.0, 0.25, 0.5, 0.9, 0.99, 1.0};
        static const size_t resp_sizes[] = {
            10u,
            100u,
            230u,
            1120u,
            4096u,
            10u * 1024u,
        };
        *out_request_size = rpc_message_profile_interp_log_quantile(
            u, req_quantiles, req_sizes, sizeof(req_sizes) / sizeof(req_sizes[0]));
        *out_response_size = rpc_message_profile_interp_log_quantile(
            u, resp_quantiles, resp_sizes,
            sizeof(resp_sizes) / sizeof(resp_sizes[0]));
        return 0;
    }
    case RPC_MESSAGE_PROFILE_FIXED:
    default:
        return -1;
    }
}

#endif
