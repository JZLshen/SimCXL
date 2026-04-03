#ifndef RPC_MESSAGE_PROFILE_H
#define RPC_MESSAGE_PROFILE_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Bare-RPC message profiles used for overall/sensitivity experiments.
 *
 * These are deterministic synthetic profiles, not trace replays.
 *
 * For the two non-fixed overall points, the runtime uses a 30-point uniform
 * size ladder whose arithmetic mean matches the nominal request/response size.
 * The ladder is rotated by client id so each client still sees the same 30
 * pairs per 30-request window, but not in lock-step with every other client.
 */

typedef enum {
    RPC_MESSAGE_PROFILE_FIXED = 0,
    RPC_MESSAGE_PROFILE_UNIFORM_1530_315 = 1,
    RPC_MESSAGE_PROFILE_UNIFORM_38_230 = 2,
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
    case RPC_MESSAGE_PROFILE_UNIFORM_1530_315:
        return "uniform-1530-315";
    case RPC_MESSAGE_PROFILE_UNIFORM_38_230:
        return "uniform-38-230";
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
    if (strcmp(name, "uniform-1530-315") == 0 ||
        strcmp(name, "google-rpc") == 0) {
        *out = RPC_MESSAGE_PROFILE_UNIFORM_1530_315;
        return 0;
    }
    if (strcmp(name, "uniform-38-230") == 0 ||
        strcmp(name, "twitter-twemcache") == 0) {
        *out = RPC_MESSAGE_PROFILE_UNIFORM_38_230;
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
    case RPC_MESSAGE_PROFILE_UNIFORM_1530_315:
        return 1530u;
    case RPC_MESSAGE_PROFILE_UNIFORM_38_230:
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
    case RPC_MESSAGE_PROFILE_UNIFORM_1530_315:
        return 315u;
    case RPC_MESSAGE_PROFILE_UNIFORM_38_230:
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
    case RPC_MESSAGE_PROFILE_UNIFORM_1530_315:
        return 2295u;
    case RPC_MESSAGE_PROFILE_UNIFORM_38_230:
        return 57u;
    case RPC_MESSAGE_PROFILE_FIXED:
    default:
        return 0u;
    }
}

static inline size_t
rpc_message_profile_max_response_size(rpc_message_profile_t profile)
{
    switch (profile) {
    case RPC_MESSAGE_PROFILE_UNIFORM_1530_315:
        return 472u;
    case RPC_MESSAGE_PROFILE_UNIFORM_38_230:
        return 345u;
    case RPC_MESSAGE_PROFILE_FIXED:
    default:
        return 0u;
    }
}

static inline size_t
rpc_message_profile_uniform30_ladder(uint16_t node_id,
                                     uint32_t op_index,
                                     size_t min_size,
                                     size_t max_size)
{
    const uint32_t point_count = 30u;
    uint32_t slot = 0;
    double value = 0.0;
    long rounded = 0L;

    if (min_size == 0u || max_size < min_size)
        return 0u;
    if (max_size == min_size || point_count <= 1u)
        return min_size;

    slot = (uint32_t)(((uint32_t)node_id + op_index) % point_count);
    value = (double)min_size +
            (((double)(max_size - min_size) * (double)slot) /
             (double)(point_count - 1u));
    rounded = lround(value);
    if (rounded < 1L)
        rounded = 1L;
    return (size_t)rounded;
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

    switch (profile) {
    case RPC_MESSAGE_PROFILE_UNIFORM_1530_315:
        *out_request_size = rpc_message_profile_uniform30_ladder(
            node_id, op_index, 765u, 2295u);
        *out_response_size = rpc_message_profile_uniform30_ladder(
            node_id, op_index, 158u, 472u);
        return 0;
    case RPC_MESSAGE_PROFILE_UNIFORM_38_230:
        *out_request_size = rpc_message_profile_uniform30_ladder(
            node_id, op_index, 19u, 57u);
        *out_response_size = rpc_message_profile_uniform30_ladder(
            node_id, op_index, 115u, 345u);
        return 0;
    case RPC_MESSAGE_PROFILE_FIXED:
    default:
        return -1;
    }
}

#endif
