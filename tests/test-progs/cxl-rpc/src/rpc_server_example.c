/*
 * Minimal CXL RPC server example.
 *
 * Keeps only request -> response handling.
 * No summary output.
 */

#define _GNU_SOURCE
#include <ctype.h>
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
#include "cxl_rpc_layout.h"

#define DEFAULT_MAX_REQUESTS 0
#define DEFAULT_IDLE_PAUSE_ITERS 0
#define DEFAULT_RESPONSE_SIZE 16
#define MIN_RESPONSE_SIZE 8
#define MAX_RESPONSE_SIZE (4096u - 8u)

typedef struct __attribute__((packed)) {
    uint32_t lhs;
    uint32_t rhs;
} add_request_t;

typedef struct __attribute__((packed)) {
    uint64_t sum;
} add_response_t;

static volatile int keep_running = 1;

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
    uint64_t requests_processed = 0;
    cxl_context_t *ctx = NULL;
    cxl_connection_t *poll_conn = NULL;
    cxl_connection_t *resp_conns[MAX_CLIENTS] = {0};
    uint8_t *resp_payload = NULL;

    int max_requests = parse_int_arg(argc, argv, "--max-requests",
                                     DEFAULT_MAX_REQUESTS);
    int idle_pause_iters = parse_int_arg(argc, argv, "--idle-pause",
                                         DEFAULT_IDLE_PAUSE_ITERS);
    int num_clients = parse_int_arg_range(argc, argv, "--num-clients", 1,
                                          1, MAX_CLIENTS);
    size_t response_size = DEFAULT_RESPONSE_SIZE;
    if (parse_size_arg(argc, argv, "--response-size", DEFAULT_RESPONSE_SIZE,
                       MIN_RESPONSE_SIZE, MAX_RESPONSE_SIZE,
                       &response_size) != 0) {
        fprintf(stderr, "server: invalid --response-size (range 8B..4088B)\n");
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    setlinebuf(stdout);
    setvbuf(stderr, NULL, _IONBF, 0);

    ctx = cxl_rpc_init(CXL_BASE, CXL_SIZE);
    if (!ctx) {
        fprintf(stderr, "server: cxl_rpc_init failed\n");
        rc = 1;
        goto cleanup;
    }

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

    poll_conn = cxl_connection_create_server_poll_owner(ctx, &addrs, 1024);
    if (!poll_conn) {
        fprintf(stderr, "server: connection_create_server_poll_owner failed\n");
        rc = 1;
        goto cleanup;
    }

    for (int i = 0; i < num_clients; i++) {
        if (num_clients == 1) {
            resp_conns[i] = cxl_connection_create_response_tx(ctx);
        } else {
            resp_conns[i] = cxl_connection_create_response_tx(ctx);
        }
        if (!resp_conns[i]) {
            fprintf(stderr, "server: create response-tx connection failed\n");
            rc = 1;
            goto cleanup;
        }

        if (cxl_connection_bind_copyengine_lane_index(resp_conns[i],
                                                      (size_t)i) < 0) {
            fprintf(stderr,
                    "server: bind CopyEngine lane index failed for client %d\n",
                    i);
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

    resp_payload = (uint8_t *)calloc(1, response_size);
    if (!resp_payload) {
        fprintf(stderr, "server: allocate response buffer failed\n");
        rc = 1;
        goto cleanup;
    }

    printf("server_ready=1\n");

    while (keep_running) {
        uint16_t node_id = 0;
        uint16_t rpc_id = 0;
        const void *req_data_view = NULL;
        size_t req_len = 0;

        int ret = cxl_poll_request(poll_conn,
                                   &node_id,
                                   &rpc_id,
                                   &req_data_view,
                                   &req_len);
        if (ret == 1) {
            if (node_id >= (uint16_t)num_clients) {
                fprintf(stderr, "server: invalid node_id=%u\n", node_id);
                rc = 1;
                break;
            }

            if (req_len < sizeof(add_request_t) || !req_data_view) {
                fprintf(stderr, "server: invalid request payload\n");
                rc = 1;
                break;
            }

            const volatile add_request_t *req_view =
                (const volatile add_request_t *)req_data_view;
            add_response_t add_resp = {
                .sum = (uint64_t)req_view->lhs + (uint64_t)req_view->rhs,
            };
            memcpy(resp_payload, &add_resp, sizeof(add_resp));

            if (cxl_send_response(resp_conns[node_id], rpc_id,
                                  resp_payload, response_size) < 0) {
                fprintf(stderr, "server: send response failed\n");
                rc = 1;
                break;
            }
            requests_processed++;
            if (max_requests > 0 &&
                requests_processed >= (uint64_t)max_requests) {
                break;
            }
        } else if (ret == 0) {
            for (int i = 0; i < idle_pause_iters && keep_running; i++)
                __asm__ __volatile__("pause" ::: "memory");
        } else {
            fprintf(stderr, "server: poll request failed\n");
            rc = 1;
            break;
        }
    }

cleanup:
    free(resp_payload);

    for (int i = 0; i < num_clients; i++) {
        if (resp_conns[i] && resp_conns[i] != poll_conn) {
            cxl_connection_destroy(resp_conns[i]);
            resp_conns[i] = NULL;
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

    return rc ? 1 : 0;
}
