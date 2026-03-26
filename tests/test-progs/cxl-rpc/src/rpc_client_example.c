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
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
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

#include "cxl_rpc.h"
#include "cxl_rpc_layout.h"

#define DEFAULT_NUM_REQUESTS 20
#define DEFAULT_MAX_POLLS 1000000
#define DEFAULT_POLL_PAUSE_ITERS 0
#define DEFAULT_REQUEST_SIZE 64
#define DEFAULT_RESPONSE_SIZE 16
#define DEFAULT_FIRST_COMPLETION_BARRIER_TIMEOUT_MS 0
#define MIN_REQUEST_SIZE 8
#define MIN_RESPONSE_SIZE 8
#define MAX_REQUEST_SIZE (256u * 1024u)
#define MAX_RESPONSE_SIZE (RESPONSE_DATA_BYTES - 8ULL)
#define CLIENT_RPC_ID_MAX 32767u
#define CLIENT_RPC_ID_SPACE (CLIENT_RPC_ID_MAX + 1u)
#define FIXED_SLIDING_WINDOW 16

typedef struct __attribute__((packed)) {
    uint32_t lhs;
    uint32_t rhs;
} add_request_t;

#define FIRST_COMPLETION_BARRIER_MAGIC 0x46434231u
#define FIRST_COMPLETION_BARRIER_WORDS (((MAX_CLIENTS) + 63) / 64)

typedef struct {
    uint32_t magic;
    uint32_t num_clients;
    uint32_t bitmap_words;
    uint32_t reserved0;
    uint64_t arrived_bitmap[FIRST_COMPLETION_BARRIER_WORDS];
} first_completion_barrier_state_t;

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

static inline size_t
first_completion_barrier_active_words(int num_clients)
{
    return (size_t)((num_clients + 63) / 64);
}

static inline uint64_t
first_completion_barrier_full_mask_for_word(int num_clients, size_t word_index)
{
    int remaining = num_clients - (int)(word_index * 64u);
    if (remaining >= 64)
        return UINT64_MAX;
    if (remaining <= 0)
        return 0;
    return (1ULL << remaining) - 1ULL;
}

static void
first_completion_barrier_reset(first_completion_barrier_state_t *state,
                               int num_clients)
{
    if (!state)
        return;

    state->magic = FIRST_COMPLETION_BARRIER_MAGIC;
    state->num_clients = (uint32_t)num_clients;
    state->bitmap_words = (uint32_t)FIRST_COMPLETION_BARRIER_WORDS;
    state->reserved0 = 0;
    memset(state->arrived_bitmap, 0, sizeof(state->arrived_bitmap));
}

static int
first_completion_barrier_is_complete(
    const first_completion_barrier_state_t *state,
    int num_clients)
{
    size_t words = first_completion_barrier_active_words(num_clients);

    if (!state)
        return 0;

    for (size_t i = 0; i < words; i++) {
        uint64_t full_mask =
            first_completion_barrier_full_mask_for_word(num_clients, i);
        uint64_t arrived =
            __atomic_load_n(&state->arrived_bitmap[i], __ATOMIC_ACQUIRE);
        if ((arrived & full_mask) != full_mask)
            return 0;
    }

    return 1;
}

static size_t
first_completion_barrier_count_arrived(
    const first_completion_barrier_state_t *state,
    int num_clients)
{
    size_t words = first_completion_barrier_active_words(num_clients);
    size_t arrived_count = 0;

    if (!state)
        return 0;

    for (size_t i = 0; i < words; i++) {
        uint64_t full_mask =
            first_completion_barrier_full_mask_for_word(num_clients, i);
        uint64_t arrived =
            __atomic_load_n(&state->arrived_bitmap[i], __ATOMIC_ACQUIRE);
        arrived_count += (size_t)__builtin_popcountll(arrived & full_mask);
    }

    return arrived_count;
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

static uint64_t
monotonic_time_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000ULL +
           (uint64_t)ts.tv_nsec / 1000000ULL;
}

static uint64_t
parse_env_u64_or_default(const char *name, uint64_t default_val)
{
    const char *val = getenv(name);
    if (!val || *val == '\0')
        return default_val;

    char *end = NULL;
    unsigned long long parsed = strtoull(val, &end, 10);
    if (!end || *end != '\0')
        return default_val;

    return (uint64_t)parsed;
}

static int
build_first_completion_barrier_path(char *buf, size_t buf_len, int num_clients)
{
    if (!buf || buf_len == 0)
        return -1;

    const char *env_path = getenv("CXL_RPC_FIRST_COMPLETION_BARRIER_PATH");
    if (env_path && *env_path != '\0') {
        int n = snprintf(buf, buf_len, "%s", env_path);
        return (n > 0 && (size_t)n < buf_len) ? 0 : -1;
    }

    pid_t sid = getsid(0);
    if (sid < 0)
        sid = 0;

    int n = snprintf(buf, buf_len,
                     "/tmp/cxl_rpc_first_completion_barrier_s%ld_c%d",
                     (long)sid, num_clients);
    return (n > 0 && (size_t)n < buf_len) ? 0 : -1;
}

static int
wait_all_clients_first_completion(int num_clients, int node_id)
{
    if (num_clients <= 1)
        return 0;
    if (num_clients > MAX_CLIENTS || node_id < 0 || node_id >= num_clients) {
        fprintf(stderr, "client: invalid first-completion barrier arguments\n");
        return -1;
    }

    char barrier_path[256];
    if (build_first_completion_barrier_path(barrier_path, sizeof(barrier_path),
                                            num_clients) != 0) {
        fprintf(stderr, "client: build first-completion barrier path failed\n");
        return -1;
    }

    int fd = open(barrier_path, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        fprintf(stderr,
                "client: open first-completion barrier failed path=%s errno=%d\n",
                barrier_path, errno);
        return -1;
    }

    if (ftruncate(fd, (off_t)sizeof(first_completion_barrier_state_t)) != 0) {
        fprintf(stderr, "client: ftruncate first-completion barrier failed\n");
        close(fd);
        return -1;
    }

    first_completion_barrier_state_t *state =
        (first_completion_barrier_state_t *)mmap(
            NULL,
            sizeof(first_completion_barrier_state_t),
            PROT_READ | PROT_WRITE,
            MAP_SHARED, fd, 0);
    if (state == MAP_FAILED) {
        fprintf(stderr, "client: mmap first-completion barrier failed\n");
        close(fd);
        return -1;
    }

    size_t my_word_index = (size_t)node_id / 64u;
    uint64_t my_bit = 1ULL << (node_id % 64);

    if (flock(fd, LOCK_EX) != 0) {
        fprintf(stderr, "client: flock first-completion barrier failed\n");
        munmap(state, sizeof(first_completion_barrier_state_t));
        close(fd);
        return -1;
    }

    if (state->magic != FIRST_COMPLETION_BARRIER_MAGIC ||
        state->num_clients != (uint32_t)num_clients ||
        state->bitmap_words != (uint32_t)FIRST_COMPLETION_BARRIER_WORDS ||
        first_completion_barrier_is_complete(state, num_clients)) {
        first_completion_barrier_reset(state, num_clients);
    }

    state->arrived_bitmap[my_word_index] |= my_bit;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    if (flock(fd, LOCK_UN) != 0) {
        fprintf(stderr, "client: unlock first-completion barrier failed\n");
        munmap(state, sizeof(first_completion_barrier_state_t));
        close(fd);
        return -1;
    }

    uint64_t timeout_ms = parse_env_u64_or_default(
        "CXL_RPC_FIRST_COMPLETION_BARRIER_TIMEOUT_MS",
        DEFAULT_FIRST_COMPLETION_BARRIER_TIMEOUT_MS);
    uint64_t start_ms = monotonic_time_ms();

    while (keep_running) {
        if (first_completion_barrier_is_complete(state, num_clients))
            break;

        if (timeout_ms > 0) {
            uint64_t now_ms = monotonic_time_ms();
            if (now_ms >= start_ms && (now_ms - start_ms) >= timeout_ms) {
                size_t arrived_clients =
                    first_completion_barrier_count_arrived(state, num_clients);
                fprintf(stderr,
                        "client: first-completion barrier timeout path=%s "
                        "arrived=%zu/%d\n",
                        barrier_path, arrived_clients, num_clients);
                munmap(state, sizeof(first_completion_barrier_state_t));
                close(fd);
                return -1;
            }
        }
        spin_pause_iters(64);
    }

    munmap(state, sizeof(first_completion_barrier_state_t));
    close(fd);
    return keep_running ? 0 : -1;
}

static int
send_one_request(cxl_connection_t *conn,
                 uint8_t *req_payload,
                 size_t request_size,
                 uint32_t *rng_state,
                 int *rpc_id_to_request_index,
                 uint64_t *request_start_ticks,
                 int sent_requests)
{
    if (!conn || !req_payload || !rng_state ||
        !rpc_id_to_request_index || !request_start_ticks ||
        sent_requests < 0) {
        return -1;
    }

    add_request_t add_req = {
        .lhs = next_random_u32(rng_state),
        .rhs = next_random_u32(rng_state),
    };
    /*
     * Only the semantic request header is refreshed per send. The rest of the
     * buffer stays zero-initialized so transport/copy length still matches the
     * configured request size without adding synthetic padding work here.
     */
    memcpy(req_payload, &add_req, sizeof(add_req));

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
    request_start_ticks[sent_requests] = start_tick;
    return rpc_id;
}

static int
drain_completed_responses(cxl_connection_t *conn,
                          int node_id,
                          size_t expected_response_size,
                          int *rpc_id_to_request_index,
                          uint64_t *request_end_ticks,
                          int *completed_requests,
                          int *first_response_marker_emitted)
{
    if (!conn || expected_response_size == 0 ||
        !rpc_id_to_request_index ||
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
    uint8_t *req_payload = NULL;

    int num_requests = parse_int_arg(argc, argv, "--requests",
                                     DEFAULT_NUM_REQUESTS);
    int poll_pause_iters = parse_nonneg_int_arg(argc, argv, "--poll-pause",
                                                DEFAULT_POLL_PAUSE_ITERS);
    int num_clients = parse_int_arg_range(argc, argv, "--num-clients", 1,
                                          1, MAX_CLIENTS);
    int node_id = parse_int_arg_range(argc, argv, "--node-id", 0,
                                      0, MAX_CLIENTS - 1);

    if (node_id >= num_clients) {
        fprintf(stderr,
                "client: node_id=%d must be < num_clients=%d\n",
                node_id, num_clients);
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

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    setlinebuf(stdout);
    setvbuf(stderr, NULL, _IONBF, 0);
    rpc_markerf("init_begin",
                "node=%d,num_clients=%d,requests=%d,request_size=%zu,response_size=%zu",
                node_id, num_clients, num_requests, request_size, response_size);

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

    size_t reserve_n = (num_requests > 0) ? (size_t)num_requests : 1;
    request_start_ticks = (uint64_t *)calloc(reserve_n, sizeof(uint64_t));
    request_end_ticks = (uint64_t *)calloc(reserve_n, sizeof(uint64_t));
    rpc_id_to_request_index =
        (int *)malloc((size_t)CLIENT_RPC_ID_SPACE *
                      sizeof(*rpc_id_to_request_index));
    if (!request_start_ticks || !request_end_ticks ||
        !rpc_id_to_request_index) {
        fprintf(stderr, "client: allocate tick buffers failed\n");
        rc = 1;
        goto cleanup;
    }
    memset(request_end_ticks, 0xFF, reserve_n * sizeof(*request_end_ticks));
    for (size_t i = 0; i < (size_t)CLIENT_RPC_ID_SPACE; i++)
        rpc_id_to_request_index[i] = -1;

    req_payload = (uint8_t *)calloc(1, request_size);
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

    if (keep_running && num_requests > 0) {
        int first_req_id = send_one_request(conn,
                                            req_payload, request_size,
                                            &rng_state,
                                            rpc_id_to_request_index,
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
                                                     response_size,
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

        rpc_markerf("first_completion_barrier_wait", "node=%d,num_clients=%d",
                    node_id, num_clients);
        if (wait_all_clients_first_completion(num_clients, node_id) != 0) {
            fprintf(stderr, "client: first-completion barrier failed\n");
            rc = 1;
            goto cleanup;
        }
        rpc_markerf("first_completion_barrier_done", "node=%d,num_clients=%d",
                    node_id, num_clients);
    }

    while (keep_running && rc == 0 && completed_requests < num_requests) {
        int inflight = sent_requests - completed_requests;
        int can_send = (sent_requests < num_requests);
        int should_poll = (inflight >= FIXED_SLIDING_WINDOW) || !can_send;

        if (can_send && !should_poll) {
            int req_id = send_one_request(conn,
                                          req_payload, request_size,
                                          &rng_state,
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

        int round_rc = drain_completed_responses(conn,
                                                 node_id,
                                                 response_size,
                                                 rpc_id_to_request_index,
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

    if (sent_requests != num_requests || completed_requests != num_requests)
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
    free(rpc_id_to_request_index);
    free(req_payload);

    return rc ? 1 : 0;
}
