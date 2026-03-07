/*
 * Minimal CXL RPC client example.
 *
 * Output contract (only):
 *   req_<i>_start_tick=<u64>
 *   req_<i>_end_tick=<u64>
 *   req_<i>_delta_tick=<u64>
 */

#define _GNU_SOURCE
#include <signal.h>
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

#define METADATA_Q_BYTES (16384ULL * 16ULL)
#define REQUEST_DATA_BYTES (10ULL * 1024ULL * 1024ULL)
#define RESPONSE_DATA_BYTES (10ULL * 1024ULL * 1024ULL)

#define DEFAULT_NUM_REQUESTS 20
#define DEFAULT_MAX_POLLS 1000000
#define DEFAULT_POLL_PAUSE_ITERS 0

#define SERVICE_ID 1
#define METHOD_ID_ADD 100

typedef struct __attribute__((packed)) {
    int64_t a;
    int64_t b;
    uint32_t seq;
    uint32_t magic;
} add_request_t;

#define ADD_REQ_MAGIC 0xADDA110Cu
#define REQ_ID_SPACE (1u << 16)

static volatile int keep_running = 1;

static inline uint64_t
current_tick(void)
{
    return m5_rpns();
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

static int
wait_for_response_tick(cxl_connection_t *conn, int32_t req_id,
                       int max_polls, int poll_pause_iters,
                       uint64_t *end_tick_out)
{
    if (end_tick_out)
        *end_tick_out = 0;

    if (max_polls <= 0)
        return 0;

    const void *payload_view = NULL;
    size_t resp_len = 0;
    uint32_t got_req_id = 0;

    for (int poll = 0; poll < max_polls && keep_running; poll++) {
        int poll_ret = cxl_poll_response_view(conn, req_id,
                                              &payload_view, &resp_len,
                                              &got_req_id);
        if (poll_ret == 1) {
            if (end_tick_out)
                *end_tick_out = current_tick();
            return 1;
        }
        if (poll_ret < 0) {
            fprintf(stderr, "client: cxl_poll_response_view failed\n");
            return -1;
        }
        for (int i = 0; i < poll_pause_iters; i++)
            __asm__ __volatile__("pause" ::: "memory");
    }

    return 0;
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

    cxl_context_t *ctx = NULL;
    cxl_connection_t *conn = NULL;
    uint64_t *start_ticks = NULL;
    uint64_t *end_ticks = NULL;

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
        .metadata_queue_size = (uint32_t)(METADATA_Q_BYTES / 16ULL),
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

    if (cxl_connection_set_request_id_prefix(conn, (uint16_t)client_id,
                                             client_tag_bits(num_clients)) < 0) {
        fprintf(stderr, "client: set request-id prefix failed\n");
        rc = 1;
        goto cleanup;
    }

    size_t reserve_n = (num_requests > 0) ? (size_t)num_requests : 1;
    start_ticks = (uint64_t *)calloc(reserve_n, sizeof(uint64_t));
    end_ticks = (uint64_t *)calloc(reserve_n, sizeof(uint64_t));
    if (!start_ticks || !end_ticks) {
        fprintf(stderr, "client: allocate tick buffers failed\n");
        rc = 1;
        goto cleanup;
    }

    for (int i = 0; keep_running && i < num_requests; i++) {
        add_request_t req = {
            .a = (int64_t)(i + 1),
            .b = (int64_t)(0x10000 + i),
            .seq = (uint32_t)i,
            .magic = ADD_REQ_MAGIC,
        };

        uint64_t start_tick = current_tick();
        int32_t req_id = cxl_send_request(conn, SERVICE_ID, METHOD_ID_ADD,
                                          &req, sizeof(req));
        if (req_id <= 0 || req_id >= (int32_t)REQ_ID_SPACE) {
            fprintf(stderr, "client: cxl_send_request failed\n");
            rc = 1;
            break;
        }

        uint64_t end_tick = 0;
        int poll_rc = wait_for_response_tick(conn, req_id, max_polls,
                                             poll_pause_iters, &end_tick);
        if (poll_rc < 0) {
            rc = 1;
            break;
        }
        if (poll_rc == 0) {
            fprintf(stderr, "client: timeout waiting response\n");
            rc = 1;
            break;
        }

        if ((size_t)completed_requests >= reserve_n) {
            fprintf(stderr, "client: tick buffer overflow\n");
            rc = 1;
            break;
        }
        if (end_tick < start_tick)
            end_tick = start_tick;

        start_ticks[completed_requests] = start_tick;
        end_ticks[completed_requests] = end_tick;
        completed_requests++;
    }

    if (completed_requests != num_requests)
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

    size_t round_count = (completed_requests > 0) ?
                         (size_t)completed_requests : 0;
    for (size_t i = 0; i < round_count; i++) {
        uint64_t delta = (end_ticks[i] >= start_ticks[i]) ?
                         (end_ticks[i] - start_ticks[i]) : 0;
        printf("req_%zu_start_tick=%lu\n", i, start_ticks[i]);
        printf("req_%zu_end_tick=%lu\n", i, end_ticks[i]);
        printf("req_%zu_delta_tick=%lu\n", i, delta);
    }
    fflush(stdout);
    fflush(stderr);

    free(start_ticks);
    free(end_ticks);

    return rc ? 1 : 0;
}

