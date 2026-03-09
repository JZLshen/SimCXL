/*
 * Minimal CXL RPC client example.
 *
 * Output contract (only):
 *   req_<i>_start_tick=<u64>
 *   req_<i>_end_tick=<u64>
 *   req_<i>_delta_tick=<u64>
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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

/* Global CXL shared-memory aperture. */
#define CXL_BASE 0x100000000ULL
#define CXL_SIZE 0x42000000ULL

/* N-client : 1-server layout. */
#define CLIENT_REGION_SIZE 0x02000000ULL
#define TOTAL_REGIONS (CXL_SIZE / CLIENT_REGION_SIZE)
#define SERVER_REGION_INDEX 0ULL
#define SERVER_REGION_BASE (CXL_BASE + SERVER_REGION_INDEX * CLIENT_REGION_SIZE)
#define MAX_CLIENTS ((int)(TOTAL_REGIONS - 1ULL))

#define DOORBELL_OFFSET 0x00000000ULL
#define DOORBELL_STRIDE 0x00000040ULL
#define METADATA_Q_OFFSET 0x00001000ULL
#define REQUEST_DATA_OFFSET 0x00005000ULL
#define RESPONSE_DATA_OFFSET 0x00A05000ULL
#define FLAG_OFFSET 0x01405000ULL

#define METADATA_Q_SIZE_BYTES 16384ULL
#define REQUEST_DATA_BYTES (10ULL * 1024ULL * 1024ULL)
#define RESPONSE_DATA_BYTES (10ULL * 1024ULL * 1024ULL)

#define DEFAULT_NUM_REQUESTS 20
#define DEFAULT_MAX_POLLS 1000000
#define DEFAULT_POLL_PAUSE_ITERS 0
#define DEFAULT_REQUEST_SIZE 64
#define DEFAULT_RESPONSE_SIZE 16
#define DEFAULT_FIRST_REQ_BARRIER_TIMEOUT_MS 600000
#define MIN_REQUEST_SIZE 8
#define MIN_RESPONSE_SIZE 8
#define MAX_REQUEST_SIZE (256u * 1024u)
#define MAX_RESPONSE_SIZE (4096u - 8u)
#define FIXED_SLIDING_WINDOW 4

#define SERVICE_ID 1
#define METHOD_ID_ADD 100

typedef struct __attribute__((packed)) {
    uint32_t lhs;
    uint32_t rhs;
} add_request_t;

#define REQ_ID_SPACE (1u << 16)
#define FIRST_REQ_BARRIER_MAGIC 0x46524231u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t num_clients;
    uint32_t arrived_mask;
    uint32_t reserved;
} first_req_barrier_state_t;

static volatile int keep_running = 1;

static inline uint64_t
current_tick(void)
{
    return m5_rpns();
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
client_region_base(int client_id)
{
    uint64_t slot = (uint64_t)client_id + 1ULL;
    return CXL_BASE + (slot * CLIENT_REGION_SIZE);
}

static uint8_t
client_tag_bits(int num_clients)
{
    uint8_t bits = 0;
    int v = (num_clients > 1) ? (num_clients - 1) : 0;
    while (v > 0) {
        bits++;
        v >>= 1;
    }
    return bits;
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
build_first_req_barrier_path(char *buf, size_t buf_len, int num_clients)
{
    if (!buf || buf_len == 0)
        return -1;

    const char *env_path = getenv("CXL_RPC_FIRST_REQ_BARRIER_PATH");
    if (env_path && *env_path != '\0') {
        int n = snprintf(buf, buf_len, "%s", env_path);
        return (n > 0 && (size_t)n < buf_len) ? 0 : -1;
    }

    pid_t sid = getsid(0);
    if (sid < 0)
        sid = 0;

    int n = snprintf(buf, buf_len, "/tmp/cxl_rpc_first_req_barrier_s%ld_c%d",
                     (long)sid, num_clients);
    return (n > 0 && (size_t)n < buf_len) ? 0 : -1;
}

static int
wait_all_clients_first_request(int num_clients, int client_id)
{
    if (num_clients <= 1)
        return 0;
    if (num_clients > 32 || client_id < 0 || client_id >= num_clients) {
        fprintf(stderr, "client: invalid first-request barrier arguments\n");
        return -1;
    }

    char barrier_path[256];
    if (build_first_req_barrier_path(barrier_path, sizeof(barrier_path),
                                     num_clients) != 0) {
        fprintf(stderr, "client: build first-request barrier path failed\n");
        return -1;
    }

    int fd = open(barrier_path, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        fprintf(stderr,
                "client: open first-request barrier failed path=%s errno=%d\n",
                barrier_path, errno);
        return -1;
    }

    if (ftruncate(fd, (off_t)sizeof(first_req_barrier_state_t)) != 0) {
        fprintf(stderr, "client: ftruncate first-request barrier failed\n");
        close(fd);
        return -1;
    }

    first_req_barrier_state_t *state =
        (first_req_barrier_state_t *)mmap(NULL,
                                          sizeof(first_req_barrier_state_t),
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED, fd, 0);
    if (state == MAP_FAILED) {
        fprintf(stderr, "client: mmap first-request barrier failed\n");
        close(fd);
        return -1;
    }

    uint32_t full_mask =
        (num_clients == 32) ? 0xFFFFFFFFu : ((1u << num_clients) - 1u);
    uint32_t my_bit = (1u << client_id);

    if (flock(fd, LOCK_EX) != 0) {
        fprintf(stderr, "client: flock first-request barrier failed\n");
        munmap(state, sizeof(first_req_barrier_state_t));
        close(fd);
        return -1;
    }

    if (state->magic != FIRST_REQ_BARRIER_MAGIC ||
        state->num_clients != (uint32_t)num_clients ||
        state->arrived_mask == full_mask) {
        state->magic = FIRST_REQ_BARRIER_MAGIC;
        state->num_clients = (uint32_t)num_clients;
        state->arrived_mask = 0;
        state->reserved = 0;
    }

    state->arrived_mask |= my_bit;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    if (flock(fd, LOCK_UN) != 0) {
        fprintf(stderr, "client: unlock first-request barrier failed\n");
        munmap(state, sizeof(first_req_barrier_state_t));
        close(fd);
        return -1;
    }

    uint64_t timeout_ms = parse_env_u64_or_default(
        "CXL_RPC_FIRST_REQ_BARRIER_TIMEOUT_MS",
        DEFAULT_FIRST_REQ_BARRIER_TIMEOUT_MS);
    uint64_t start_ms = monotonic_time_ms();

    while (keep_running) {
        uint32_t arrived = __atomic_load_n(&state->arrived_mask,
                                           __ATOMIC_ACQUIRE);
        if ((arrived & full_mask) == full_mask)
            break;

        if (timeout_ms > 0) {
            uint64_t now_ms = monotonic_time_ms();
            if (now_ms >= start_ms && (now_ms - start_ms) >= timeout_ms) {
                fprintf(stderr,
                        "client: first-request barrier timeout path=%s "
                        "arrived_mask=0x%08x full_mask=0x%08x\n",
                        barrier_path, arrived, full_mask);
                munmap(state, sizeof(first_req_barrier_state_t));
                close(fd);
                return -1;
            }
        }
        spin_pause_iters(64);
    }

    munmap(state, sizeof(first_req_barrier_state_t));
    close(fd);
    return keep_running ? 0 : -1;
}

static int
send_one_request(cxl_connection_t *conn,
                 uint8_t *req_payload,
                 size_t request_size,
                 uint32_t *rng_state,
                 uint32_t *pending_slot_by_req_id,
                 uint64_t *request_start_ticks,
                 int sent_requests)
{
    if (!conn || !req_payload || !rng_state || !pending_slot_by_req_id ||
        !request_start_ticks || sent_requests < 0) {
        return -1;
    }

    add_request_t add_req = {
        .lhs = next_random_u32(rng_state),
        .rhs = next_random_u32(rng_state),
    };
    memcpy(req_payload, &add_req, sizeof(add_req));

    for (size_t k = sizeof(add_req); k < request_size;) {
        uint32_t random_word = next_random_u32(rng_state);
        size_t remain = request_size - k;
        size_t chunk = remain < sizeof(random_word) ? remain :
                       sizeof(random_word);
        memcpy(req_payload + k, &random_word, chunk);
        k += chunk;
    }

    uint64_t start_tick = current_tick();
    int req_id = cxl_send_request(conn, SERVICE_ID, METHOD_ID_ADD,
                                  req_payload, request_size);
    if (req_id <= 0 || req_id >= (int)REQ_ID_SPACE) {
        fprintf(stderr, "client: cxl_send_request failed\n");
        return -1;
    }

    pending_slot_by_req_id[req_id] = (uint32_t)sent_requests + 1u;
    request_start_ticks[sent_requests] = start_tick;
    return req_id;
}

static int
drain_completed_responses(cxl_connection_t *conn,
                          uint8_t *response_buf,
                          size_t response_buf_size,
                          uint32_t *pending_slot_by_req_id,
                          uint64_t *request_end_ticks,
                          int *completed_requests)
{
    if (!conn || !response_buf || response_buf_size == 0 ||
        !pending_slot_by_req_id || !request_end_ticks ||
        !completed_requests) {
        return -1;
    }

    uint16_t latest_completed_req_id = 0;
    int peek_rc =
        cxl_peek_latest_completed_request_id(conn, &latest_completed_req_id);
    if (peek_rc < 0) {
        fprintf(stderr, "client: cxl_peek_latest_completed_request_id failed\n");
        return -1;
    }
    if (peek_rc == 0)
        return 0;

    if (pending_slot_by_req_id[latest_completed_req_id] == 0)
        return 0;

    int drained = 0;
    while (keep_running) {
        size_t response_len = response_buf_size;
        uint16_t consumed_req_id = 0;
        int consume_rc = cxl_consume_next_response(conn,
                                                   response_buf,
                                                   &response_len,
                                                   &consumed_req_id);
        if (consume_rc < 0) {
            fprintf(stderr, "client: cxl_consume_next_response failed\n");
            return -1;
        }
        if (consume_rc == 0) {
            break;
        }

        if (response_len != response_buf_size) {
            fprintf(stderr,
                    "client: response size mismatch expect=%zu got=%zu\n",
                    response_buf_size, response_len);
            return -1;
        }

        uint32_t slot = pending_slot_by_req_id[consumed_req_id];
        if (slot == 0) {
            fprintf(stderr,
                    "client: unexpected response req_id=%u not in pending list\n",
                    consumed_req_id);
            return -1;
        }
        int idx = (int)(slot - 1u);

        request_end_ticks[idx] = current_tick();
        pending_slot_by_req_id[consumed_req_id] = 0;
        (*completed_requests)++;
        drained = 1;

        if (consumed_req_id == latest_completed_req_id)
            break;
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
    int sent_requests = 0;

    cxl_context_t *ctx = NULL;
    cxl_connection_t *conn = NULL;
    uint64_t *request_start_ticks = NULL;
    uint64_t *request_end_ticks = NULL;
    uint32_t *pending_slot_by_req_id = NULL;
    uint8_t *req_payload = NULL;
    uint8_t *response_buf = NULL;

    int num_requests = parse_int_arg(argc, argv, "--requests",
                                     DEFAULT_NUM_REQUESTS);
    int max_polls = parse_int_arg(argc, argv, "--max-polls",
                                  DEFAULT_MAX_POLLS);
    int poll_pause_iters = parse_nonneg_int_arg(argc, argv, "--poll-pause",
                                                DEFAULT_POLL_PAUSE_ITERS);
    int num_clients = parse_int_arg_range(argc, argv, "--num-clients", 1,
                                          1, MAX_CLIENTS);
    int client_id = parse_int_arg_range(argc, argv, "--client-id", -1,
                                        0, MAX_CLIENTS - 1);
    if (client_id < 0) {
        client_id = parse_int_arg_range(argc, argv, "--pair-id", 0,
                                        0, MAX_CLIENTS - 1);
    }

    if (client_id >= num_clients) {
        fprintf(stderr,
                "client: client_id=%d must be < num_clients=%d\n",
                client_id, num_clients);
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
        fprintf(stderr, "client: invalid --response-size (range 8B..4088B)\n");
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    setlinebuf(stdout);
    setvbuf(stderr, NULL, _IONBF, 0);

    ctx = cxl_rpc_init(CXL_BASE, CXL_SIZE);
    if (!ctx) {
        fprintf(stderr, "client: cxl_rpc_init failed\n");
        rc = 1;
        goto cleanup;
    }

    uint64_t client_base = client_region_base(client_id);
    uint64_t server_base = SERVER_REGION_BASE;

    cxl_connection_addrs_t addrs = {
        .doorbell_addr = server_base + DOORBELL_OFFSET +
                         ((uint64_t)(client_id + 1) * DOORBELL_STRIDE),
        .metadata_queue_addr = server_base + METADATA_Q_OFFSET,
        .metadata_queue_size = (uint32_t)METADATA_Q_SIZE_BYTES,
        .request_data_addr = client_base + REQUEST_DATA_OFFSET,
        .request_data_size = REQUEST_DATA_BYTES,
        .response_data_addr = client_base + RESPONSE_DATA_OFFSET,
        .response_data_size = RESPONSE_DATA_BYTES,
        .flag_addr = client_base + FLAG_OFFSET,
    };

    conn = cxl_connection_create_fixed_attach(ctx, &addrs, 1024);
    if (!conn) {
        fprintf(stderr, "client: connection_create_fixed_attach failed\n");
        rc = 1;
        goto cleanup;
    }

    if (cxl_connection_set_client_tag(conn, (uint16_t)client_id,
                                      client_tag_bits(num_clients)) < 0) {
        fprintf(stderr, "client: set client tag failed\n");
        rc = 1;
        goto cleanup;
    }

    size_t reserve_n = (num_requests > 0) ? (size_t)num_requests : 1;
    request_start_ticks = (uint64_t *)calloc(reserve_n, sizeof(uint64_t));
    request_end_ticks = (uint64_t *)calloc(reserve_n, sizeof(uint64_t));
    pending_slot_by_req_id =
        (uint32_t *)calloc(REQ_ID_SPACE, sizeof(uint32_t));
    if (!request_start_ticks || !request_end_ticks || !pending_slot_by_req_id) {
        fprintf(stderr, "client: allocate tick buffers failed\n");
        rc = 1;
        goto cleanup;
    }
    memset(request_end_ticks, 0xFF, reserve_n * sizeof(*request_end_ticks));

    req_payload = (uint8_t *)malloc(request_size);
    if (!req_payload) {
        fprintf(stderr, "client: allocate request payload failed\n");
        rc = 1;
        goto cleanup;
    }

    response_buf = (uint8_t *)malloc(response_size);
    if (!response_buf) {
        fprintf(stderr, "client: allocate response buffer failed\n");
        rc = 1;
        goto cleanup;
    }

    uint32_t rng_state =
        (uint32_t)(current_tick() ^
                   ((uint64_t)(client_id + 1) * 0x9E3779B97F4A7C15ULL));
    if (rng_state == 0)
        rng_state = 0xA5A5A5A5u ^ (uint32_t)(client_id + 1);

    uint64_t max_poll_rounds = (uint64_t)((num_requests > 0) ?
                             (uint64_t)num_requests : 1ULL) *
                             (uint64_t)((max_polls > 0) ? max_polls : 1);
    uint64_t poll_rounds = 0;

    if (keep_running && num_requests > 0) {
        int first_req_id = send_one_request(conn,
                                            req_payload, request_size,
                                            &rng_state, pending_slot_by_req_id,
                                            request_start_ticks,
                                            sent_requests);
        if (first_req_id <= 0) {
            rc = 1;
            goto cleanup;
        }
        sent_requests++;

        if (wait_all_clients_first_request(num_clients, client_id) != 0) {
            fprintf(stderr, "client: first-request barrier failed\n");
            rc = 1;
            goto cleanup;
        }
    }

    while (keep_running && rc == 0 && completed_requests < num_requests) {
        int inflight = sent_requests - completed_requests;
        int can_send = (sent_requests < num_requests);
        int should_poll = (inflight >= FIXED_SLIDING_WINDOW) || !can_send;

        if (can_send && !should_poll) {
            int req_id = send_one_request(conn,
                                          req_payload, request_size,
                                          &rng_state, pending_slot_by_req_id,
                                          request_start_ticks,
                                          sent_requests);
            if (req_id <= 0) {
                rc = 1;
                break;
            }
            sent_requests++;
            continue;
        }

        if (poll_rounds >= max_poll_rounds) {
            uint16_t latest_completed_req_id = 0;
            int latest_rc =
                cxl_peek_latest_completed_request_id(conn,
                                                     &latest_completed_req_id);
            int pending_req_id = -1;
            uint32_t pending_slot = 0;
            for (int rid = 1; rid < REQ_ID_SPACE; rid++) {
                if (pending_slot_by_req_id[rid] != 0) {
                    pending_req_id = rid;
                    pending_slot = pending_slot_by_req_id[rid];
                    break;
                }
            }
            fprintf(stderr,
                    "client[%d]: timeout waiting response sent=%d completed=%d poll_rounds=%lu latest_flag_rc=%d latest_flag_req_id=%u pending_req_id=%d pending_slot=%u\n",
                    client_id,
                    sent_requests,
                    completed_requests,
                    poll_rounds,
                    latest_rc,
                    latest_completed_req_id,
                    pending_req_id,
                    pending_slot);
            rc = 1;
            break;
        }

        int round_rc = drain_completed_responses(conn,
                                                 response_buf,
                                                 response_size,
                                                 pending_slot_by_req_id,
                                                 request_end_ticks,
                                                 &completed_requests);
        if (round_rc < 0) {
            rc = 1;
            break;
        }
        poll_rounds++;
        if (round_rc == 0)
            spin_pause_iters(poll_pause_iters);
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
        uint64_t delta =
            (request_end_ticks[i] >= request_start_ticks[i]) ?
            (request_end_ticks[i] - request_start_ticks[i]) : 0;
        printf("req_%d_start_tick=%lu\n", i, request_start_ticks[i]);
        printf("req_%d_end_tick=%lu\n", i, request_end_ticks[i]);
        printf("req_%d_delta_tick=%lu\n", i, delta);
    }
    fflush(stdout);
    fflush(stderr);

    free(request_start_ticks);
    free(request_end_ticks);
    free(pending_slot_by_req_id);
    free(req_payload);
    free(response_buf);

    return rc ? 1 : 0;
}
