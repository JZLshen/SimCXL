/*
 * Minimal CXL RPC client example.
 *
 * Output contract (only):
 *   req_<i>_start_tick=<u64>
 *   req_<i>_end_tick=<u64>
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
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
#include "rpc_message_profile.h"

#define DEFAULT_NUM_REQUESTS 20
#define DEFAULT_MAX_POLLS 1000000
#define DEFAULT_POLL_PAUSE_ITERS 0
#define DEFAULT_REQUEST_SIZE 64
#define DEFAULT_RESPONSE_SIZE 16
#define MIN_REQUEST_SIZE 8
#define MIN_RESPONSE_SIZE 8
#define MAX_REQUEST_SIZE (256u * 1024u)
#define MAX_RESPONSE_SIZE (RESPONSE_DATA_BYTES - 8ULL)
#define CLIENT_RPC_ID_MAX 32767u
#define CLIENT_RPC_ID_SPACE (CLIENT_RPC_ID_MAX + 1u)
#define DEFAULT_SLIDING_WINDOW 16
#define DEFAULT_SLOW_CLIENT_COUNT 0
#define DEFAULT_SLOW_CLIENT_SEND_PAUSE_ITERS 0

typedef struct __attribute__((packed)) {
    uint32_t lhs;
    uint32_t rhs;
} add_request_t;

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

static inline uint32_t
next_random_u32(uint32_t *state)
{
    uint32_t value = (state && *state != 0) ? *state : 0x6D2B79F5u;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    if (state)
        *state = value;
    return value;
}

static int
parse_int_arg(int argc, char **argv, const char *flag, int default_val)
{
    for (int i = 1; i < argc - 1; i++) {
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
parse_required_int_arg_range(int argc, char **argv, const char *flag,
                             int default_val, int min_val, int max_val,
                             int *out)
{
    if (!out)
        return -1;

    *out = default_val;
    for (int i = 1; i < argc - 1; i++) {
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
parse_size_literal(const char *s, size_t min_val, size_t max_val, size_t *out)
{
    if (!s || !out)
        return -1;

    char *end = NULL;
    unsigned long long raw = strtoull(s, &end, 10);
    if (end == s)
        return -1;

    unsigned long long mul = 1;
    if (*end != '\0') {
        char c0 = (char)toupper((unsigned char)end[0]);
        if (c0 == 'B' && end[1] == '\0') {
            mul = 1;
        } else if (c0 == 'K') {
            if (end[1] == '\0' ||
                (toupper((unsigned char)end[1]) == 'B' && end[2] == '\0')) {
                mul = 1024ULL;
            } else {
                return -1;
            }
        } else if (c0 == 'M') {
            if (end[1] == '\0' ||
                (toupper((unsigned char)end[1]) == 'B' && end[2] == '\0')) {
                mul = 1024ULL * 1024ULL;
            } else {
                return -1;
            }
        } else {
            return -1;
        }
    }

    unsigned long long v = raw * mul;
    if (mul != 0 && raw != 0 && (v / mul) != raw)
        return -1;
    if (v < (unsigned long long)min_val || v > (unsigned long long)max_val)
        return -1;

    *out = (size_t)v;
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
client_is_selected_evenly(int client_id,
                          int client_count,
                          int selected_client_count)
{
    int selected_index;

    if (selected_client_count <= 0)
        return 0;

    if (selected_client_count >= client_count)
        return 1;

    for (selected_index = 0;
         selected_index < selected_client_count;
         selected_index++) {
        if ((selected_index * client_count) / selected_client_count == client_id)
            return 1;
    }

    return 0;
}

static int
parse_message_profile_arg(int argc, char **argv,
                          rpc_message_profile_t *out_profile)
{
    if (!out_profile)
        return -1;

    *out_profile = RPC_MESSAGE_PROFILE_FIXED;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--message-profile") != 0)
            continue;
        return rpc_message_profile_parse(argv[i + 1], out_profile);
    }
    return 0;
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
    for (int i = 0; i < poll_pause_iters; i++)
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

static int
send_one_request(cxl_connection_t *conn,
                 int node_id,
                 uint8_t *req_payload,
                 size_t fixed_request_size,
                 size_t fixed_response_size,
                 rpc_message_profile_t message_profile,
                 const rpc_length_plan_t *request_length_plan,
                 const rpc_length_plan_t *response_length_plan,
                 uint32_t *rng_state,
                 int *rpc_id_to_request_index,
                 size_t *request_expected_response_sizes,
                 uint64_t *request_start_ticks,
                 int sent_requests)
{
    if (!conn || !req_payload || !rng_state ||
        !rpc_id_to_request_index || !request_expected_response_sizes ||
        !request_start_ticks ||
        sent_requests < 0) {
        return -1;
    }

    size_t request_size = fixed_request_size;
    size_t expected_response_size = fixed_response_size;
    const uint32_t lhs = next_random_u32(rng_state);
    const uint32_t rhs = next_random_u32(rng_state);
    const int use_profiled_header =
        rpc_runtime_uses_profiled_header(message_profile,
                                        request_length_plan,
                                        response_length_plan);

    if (use_profiled_header) {
        rpc_profiled_request_t profiled_req = {
            .lhs = lhs,
            .rhs = rhs,
            .op_index = (uint32_t)sent_requests,
        };
        if (rpc_runtime_sample_sizes(message_profile,
                                     (uint16_t)node_id,
                                     (uint32_t)sent_requests,
                                     request_length_plan,
                                     response_length_plan,
                                     fixed_request_size,
                                     fixed_response_size,
                                     &request_size,
                                     &expected_response_size) != 0) {
            fprintf(stderr, "client: sample request sizing failed\n");
            return -1;
        }
        if (request_size < sizeof(profiled_req)) {
            fprintf(stderr,
                    "client: profiled request size too small size=%zu hdr=%zu\n",
                    request_size, sizeof(profiled_req));
            return -1;
        }
        memcpy(req_payload, &profiled_req, sizeof(profiled_req));
    } else {
        add_request_t add_req = {
            .lhs = lhs,
            .rhs = rhs,
        };
        memcpy(req_payload, &add_req, sizeof(add_req));
    }
    /*
     * Only the semantic request header is refreshed per send. The rest of the
     * buffer stays zero-initialized so transport/copy length still matches the
     * configured request size without adding synthetic padding work here.
     */

    uint64_t start_tick = current_tick();
    int rpc_id = cxl_send_request(conn, req_payload, request_size);
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
    request_expected_response_sizes[sent_requests] = expected_response_size;
    request_start_ticks[sent_requests] = start_tick;
    return rpc_id;
}

static int
drain_completed_responses(cxl_connection_t *conn,
                          int node_id,
                          int *rpc_id_to_request_index,
                          size_t *request_expected_response_sizes,
                          uint64_t *request_end_ticks,
                          int *completed_requests,
                          int *first_response_marker_emitted)
{
    if (!conn || !rpc_id_to_request_index ||
        !request_expected_response_sizes ||
        !request_end_ticks || !completed_requests) {
        return -1;
    }

    int drained = 0;
    while (keep_running) {
        const void *response_view = NULL;
        size_t response_len = 0;
        uint16_t consumed_rpc_id = 0;
        int peek_rc = cxl_peek_next_response_view(conn,
                                                  &response_view,
                                                  &response_len,
                                                  &consumed_rpc_id);
        if (peek_rc < 0) {
            fprintf(stderr,
                    "client: cxl_peek_next_response_view failed\n");
            return -1;
        }
        if (peek_rc == 0) {
            break;
        }

        if ((unsigned int)consumed_rpc_id > CLIENT_RPC_ID_MAX) {
            fprintf(stderr, "client: invalid consumed rpc_id=%u\n",
                    (unsigned)consumed_rpc_id);
            return -1;
        }

        int idx = rpc_id_to_request_index[consumed_rpc_id];
        if (idx < 0 || request_end_ticks[idx] != UINT64_MAX) {
            fprintf(stderr, "client: unmatched or duplicate response rpc_id=%u\n",
                    (unsigned)consumed_rpc_id);
            return -1;
        }

        size_t expected_response_size = request_expected_response_sizes[idx];

        if (response_len != expected_response_size) {
            fprintf(stderr,
                    "client: response size mismatch expect=%zu got=%zu\n",
                    expected_response_size, response_len);
            return -1;
        }
        if (response_len > 0 && !response_view) {
            fprintf(stderr, "client: zero-copy response view is NULL\n");
            return -1;
        }

        /*
         * Measure the zero-copy payload-view receive path: response completion
         * is recorded only after the shared-memory payload view has been
         * prepared and the local consumer head has advanced.
         */
        if (cxl_advance_response_head(conn,
                                      consumed_rpc_id,
                                      response_len) != 1) {
            fprintf(stderr, "client: cxl_advance_response_head failed\n");
            return -1;
        }
        request_end_ticks[idx] = current_tick();
        if (first_response_marker_emitted &&
            !(*first_response_marker_emitted)) {
            rpc_markerf("first_response_seen",
                        "node=%d,rpc_id=%u,response_len=%zu,completed=%d",
                        node_id, (unsigned)consumed_rpc_id,
                        response_len, *completed_requests + 1);
            *first_response_marker_emitted = 1;
        }
        rpc_id_to_request_index[consumed_rpc_id] = -1;
        (*completed_requests)++;
        drained = 1;
    }

    return drained;
}

static void
signal_handler(int sig)
{
    (void)sig;
    keep_running = 0;
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
    size_t *request_expected_response_sizes = NULL;
    uint8_t *req_payload = NULL;

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

    if (node_id >= num_clients) {
        fprintf(stderr,
                "client: node_id=%d must be < num_clients=%d\n",
                node_id, num_clients);
        return 1;
    }

    if (slow_client_count > num_clients) {
        fprintf(stderr,
                "client: slow_client_count=%d must be <= num_clients=%d\n",
                slow_client_count, num_clients);
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

    size_t request_size = DEFAULT_REQUEST_SIZE;
    if (parse_size_arg(argc, argv, "--request-size", DEFAULT_REQUEST_SIZE,
                       MIN_REQUEST_SIZE, MAX_REQUEST_SIZE,
                       &request_size) != 0) {
        fprintf(stderr, "client: invalid --request-size (range 8B..256KB)\n");
        return 1;
    }

    size_t response_size = DEFAULT_RESPONSE_SIZE;
    if (parse_size_arg(argc, argv, "--response-size", DEFAULT_RESPONSE_SIZE,
                       MIN_RESPONSE_SIZE, MAX_RESPONSE_SIZE,
                       &response_size) != 0) {
        fprintf(stderr,
                "client: invalid --response-size (range %uB..%zuB)\n",
                MIN_RESPONSE_SIZE, (size_t)MAX_RESPONSE_SIZE);
        return 1;
    }

    rpc_message_profile_t message_profile = RPC_MESSAGE_PROFILE_FIXED;
    if (parse_message_profile_arg(argc, argv, &message_profile) != 0) {
        fprintf(stderr,
                "client: invalid --message-profile "
                "(use fixed|uniform-1530-315|uniform-38-230; "
                "legacy google-rpc/twitter-twemcache aliases also work)\n");
        return 1;
    }

    rpc_length_plan_t request_length_plan = {0};
    rpc_length_plan_t response_length_plan = {0};
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--req-min-bytes") == 0) {
            request_length_plan.min_size = strtoull(argv[i + 1], NULL, 0);
        } else if (strcmp(argv[i], "--req-max-bytes") == 0) {
            request_length_plan.max_size = strtoull(argv[i + 1], NULL, 0);
        } else if (strcmp(argv[i], "--resp-min-bytes") == 0) {
            response_length_plan.min_size = strtoull(argv[i + 1], NULL, 0);
        } else if (strcmp(argv[i], "--resp-max-bytes") == 0) {
            response_length_plan.max_size = strtoull(argv[i + 1], NULL, 0);
        }
    }
    if (rpc_length_plan_has_bounds(&request_length_plan) &&
        request_length_plan.max_size < request_length_plan.min_size) {
        fprintf(stderr, "client: req-min-bytes must be <= req-max-bytes\n");
        return 1;
    }
    if (rpc_length_plan_has_bounds(&response_length_plan) &&
        response_length_plan.max_size < response_length_plan.min_size) {
        fprintf(stderr, "client: resp-min-bytes must be <= resp-max-bytes\n");
        return 1;
    }

    size_t request_payload_capacity =
        rpc_runtime_max_request_size(message_profile,
                                     &request_length_plan,
                                     request_size);
    if (rpc_runtime_uses_profiled_header(message_profile,
                                         &request_length_plan,
                                         &response_length_plan)) {
        if (request_payload_capacity < sizeof(rpc_profiled_request_t) ||
            request_payload_capacity > MAX_REQUEST_SIZE) {
            fprintf(stderr, "client: invalid profiled request capacity\n");
            return 1;
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    setlinebuf(stdout);
    setvbuf(stderr, NULL, _IONBF, 0);
    const int is_slow_client =
        client_is_selected_evenly(node_id, num_clients, slow_client_count);
    const int client_request_count =
        is_slow_client ? slow_count_per_client : num_requests;
    const int barrier_participant_count =
        first_round_barrier_participants(num_clients,
                                         num_requests,
                                         slow_client_count,
                                         slow_count_per_client);
    const int send_pause_iters =
        is_slow_client
            ? scale_sparse_send_pause_iters(slow_client_send_pause_iters,
                                            num_requests,
                                            client_request_count)
            : 0;
    rpc_markerf("init_begin",
                "node=%d,num_clients=%d,requests=%d,base_requests=%d,request_size=%zu,response_size=%zu,request_payload_capacity=%zu,window=%d,slow_client_count=%d,slow_count_per_client=%d,is_slow_client=%d,slow_client_send_pause_iters=%d,message_profile=%s",
                node_id, num_clients, client_request_count, num_requests,
                request_size, response_size,
                request_payload_capacity,
                sliding_window, slow_client_count, slow_count_per_client,
                is_slow_client,
                send_pause_iters, rpc_message_profile_name(message_profile));

    ctx = cxl_rpc_init(CXL_BASE, CXL_SIZE);
    if (!ctx) {
        fprintf(stderr, "client: cxl_rpc_init failed\n");
        rc = 1;
        goto cleanup;
    }
    rpc_markerf("ctx_ready", "node=%d", node_id);

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

    rpc_markerf("attach_begin", "node=%d", node_id);
    conn = cxl_connection_create_client_attach(ctx, &addrs);
    if (!conn) {
        fprintf(stderr, "client: connection_create_client_attach failed\n");
        rc = 1;
        goto cleanup;
    }
    rpc_markerf("attach_ready", "node=%d", node_id);

    size_t reserve_n = (client_request_count > 0) ? (size_t)client_request_count : 1;
    request_start_ticks = (uint64_t *)calloc(reserve_n, sizeof(uint64_t));
    request_end_ticks = (uint64_t *)calloc(reserve_n, sizeof(uint64_t));
    request_expected_response_sizes =
        (size_t *)calloc(reserve_n, sizeof(*request_expected_response_sizes));
    rpc_id_to_request_index =
        (int *)malloc((size_t)CLIENT_RPC_ID_SPACE *
                      sizeof(*rpc_id_to_request_index));
    if (!request_start_ticks || !request_end_ticks ||
        !request_expected_response_sizes || !rpc_id_to_request_index) {
        fprintf(stderr, "client: allocate tick buffers failed\n");
        rc = 1;
        goto cleanup;
    }
    memset(request_end_ticks, 0xFF, reserve_n * sizeof(*request_end_ticks));
    for (size_t i = 0; i < (size_t)CLIENT_RPC_ID_SPACE; i++)
        rpc_id_to_request_index[i] = -1;

    req_payload = (uint8_t *)calloc(1, request_payload_capacity);
    if (!req_payload) {
        fprintf(stderr, "client: allocate request payload failed\n");
        rc = 1;
        goto cleanup;
    }

    uint32_t rng_state =
        (uint32_t)(current_tick() ^
                   ((uint64_t)(node_id + 1) * 0x9E3779B97F4A7C15ULL));
    if (rng_state == 0)
        rng_state = 0xA5A5A5A5u ^ (uint32_t)(node_id + 1);

    if (keep_running && client_request_count > 0) {
        int first_req_id = send_one_request(conn,
                                            node_id,
                                            req_payload,
                                            request_size,
                                            response_size,
                                            message_profile,
                                            &request_length_plan,
                                            &response_length_plan,
                                            &rng_state,
                                            rpc_id_to_request_index,
                                            request_expected_response_sizes,
                                            request_start_ticks,
                                            sent_requests);
        if (first_req_id <= 0) {
            rc = 1;
            goto cleanup;
        }
        rpc_markerf("first_request_sent", "node=%d,rpc_id=%d",
                    node_id, first_req_id);
        sent_requests++;

        for (;;) {
            int round_rc = drain_completed_responses(conn,
                                                     node_id,
                                                     rpc_id_to_request_index,
                                                     request_expected_response_sizes,
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
            /*
             * Sparse-request experiments model slower clients by inserting
             * extra application-side think time before each subsequent send.
             */
            if (send_pause_iters > 0)
                spin_pause_iters(send_pause_iters);
            int req_id = send_one_request(conn,
                                          node_id,
                                          req_payload,
                                          request_size,
                                          response_size,
                                          message_profile,
                                          &request_length_plan,
                                          &response_length_plan,
                                          &rng_state,
                                          rpc_id_to_request_index,
                                          request_expected_response_sizes,
                                          request_start_ticks,
                                          sent_requests);
            if (req_id <= 0) {
                rc = 1;
                break;
            }
            sent_requests++;
            continue;
        }

        int round_rc = drain_completed_responses(conn,
                                                 node_id,
                                                 rpc_id_to_request_index,
                                                 request_expected_response_sizes,
                                                 request_end_ticks,
                                                 &completed_requests,
                                                 &first_response_marker_emitted);
        if (round_rc < 0) {
            rc = 1;
            break;
        }
        if (round_rc == 0) {
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

    for (int i = 0; i < sent_requests; i++) {
        if (request_end_ticks[i] == UINT64_MAX)
            continue;
        printf("req_%d_start_tick=%lu\n", i, request_start_ticks[i]);
        printf("req_%d_end_tick=%lu\n", i, request_end_ticks[i]);
    }
    fflush(stdout);
    fflush(stderr);

    free(request_start_ticks);
    free(request_end_ticks);
    free(request_expected_response_sizes);
    free(rpc_id_to_request_index);
    free(req_payload);

    return rc ? 1 : 0;
}
