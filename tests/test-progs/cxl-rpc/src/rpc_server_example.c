/*
 * Minimal CXL RPC server example.
 *
 * Keeps only request -> response handling.
 * No summary output.
 */

#define _GNU_SOURCE
#include <ctype.h>
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

#define DEFAULT_MAX_REQUESTS 0
#define DEFAULT_IDLE_PAUSE_ITERS 0
#define DEFAULT_RESPONSE_SIZE 16
#define MIN_RESPONSE_SIZE 8
#define MAX_RESPONSE_SIZE (RESPONSE_DATA_BYTES - 8ULL)
#define RESPONSE_DMA_THRESHOLD 4096u
#define RESPONSE_HEADER_BYTES 8u

typedef struct __attribute__((packed)) {
    uint32_t lhs;
    uint32_t rhs;
} add_request_t;

typedef struct __attribute__((packed)) {
    uint64_t sum;
} add_response_t;

typedef struct {
    uint16_t node_id;
    uint64_t poll_notify_tick;
    uint64_t poll_req_data_tick;
    uint64_t exec_tick;
    uint64_t resp_submit_tick;
} server_request_timing_t;

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
    int first_poll_marker_emitted = 0;
    int first_resp_marker_emitted = 0;
    uint64_t requests_processed = 0;
    cxl_context_t *ctx = NULL;
    cxl_connection_t *poll_conn = NULL;
    cxl_connection_t *resp_conns[MAX_CLIENTS] = {0};
    uint8_t *resp_payload = NULL;
    server_request_timing_t *timings = NULL;
    uint64_t poll_phase_start_tick = 0;

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
        fprintf(stderr,
                "server: invalid --response-size (range %uB..%zuB)\n",
                MIN_RESPONSE_SIZE, (size_t)MAX_RESPONSE_SIZE);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    setlinebuf(stdout);
    setvbuf(stderr, NULL, _IONBF, 0);
    rpc_markerf("init_begin",
                "num_clients=%d,max_requests=%d,response_size=%zu",
                num_clients, max_requests, response_size);

    ctx = cxl_rpc_init(CXL_BASE, CXL_SIZE);
    if (!ctx) {
        fprintf(stderr, "server: cxl_rpc_init failed\n");
        rc = 1;
        goto cleanup;
    }
    rpc_markerf("ctx_ready", "num_clients=%d", num_clients);

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

    rpc_markerf("poll_conn_begin", "mq_entries=%u", METADATA_Q_ENTRIES);
    poll_conn = cxl_connection_create_server_poll_owner(ctx, &addrs, 1024);
    if (!poll_conn) {
        fprintf(stderr, "server: connection_create_server_poll_owner failed\n");
        rc = 1;
        goto cleanup;
    }
    rpc_markerf("poll_conn_ready", "mq_entries=%u", METADATA_Q_ENTRIES);

    for (int i = 0; i < num_clients; i++) {
        resp_conns[i] = cxl_connection_create_response_tx(ctx);
        if (!resp_conns[i]) {
            fprintf(stderr, "server: create response-tx connection failed\n");
            rc = 1;
            goto cleanup;
        }

        if ((response_size + (size_t)RESPONSE_HEADER_BYTES) >
                (size_t)RESPONSE_DMA_THRESHOLD &&
            cxl_connection_bind_copyengine_lane_index(resp_conns[i],
                                                      (size_t)i) < 0) {
            fprintf(stderr, "server: bind dedicated CopyEngine lane failed\n");
            rc = 1;
            goto cleanup;
        }

        rpc_markerf("peer_response_data_begin", "node=%d,bytes=%zu",
                    i, (size_t)RESPONSE_DATA_BYTES);
        if (cxl_connection_set_peer_response_data(resp_conns[i],
                                                  node_region_base(i) +
                                                      RESPONSE_DATA_OFFSET,
                                                  RESPONSE_DATA_BYTES) < 0) {
            fprintf(stderr, "server: set peer response range failed\n");
            rc = 1;
            goto cleanup;
        }
        rpc_markerf("peer_response_data_ready", "node=%d,bytes=%zu",
                    i, (size_t)RESPONSE_DATA_BYTES);

        if (cxl_connection_set_peer_response_flag_addr(resp_conns[i],
                                                       node_region_base(i) +
                                                           FLAG_OFFSET) < 0) {
            fprintf(stderr, "server: set peer flag failed\n");
            rc = 1;
            goto cleanup;
        }
        rpc_markerf("peer_flag_ready", "node=%d", i);

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

    if (response_size != sizeof(add_response_t)) {
        resp_payload = (uint8_t *)calloc(1, response_size);
        if (!resp_payload) {
            fprintf(stderr, "server: allocate response buffer failed\n");
            rc = 1;
            goto cleanup;
        }
    }

    printf("server_ready=1\n");
    rpc_markerf("server_ready", "num_clients=%d", num_clients);
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
            poll_done_tick = (poll_timing.poll_done_tick != 0) ?
                             poll_timing.poll_done_tick :
                             current_tick();
            if (!first_poll_marker_emitted) {
                rpc_markerf("first_request_polled",
                            "node=%u,rpc_id=%u,req_len=%zu",
                            (unsigned)node_id, (unsigned)rpc_id, req_len);
                first_poll_marker_emitted = 1;
            }
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
            const void *resp_data = (const void *)&add_resp;
            if (resp_payload) {
                memcpy(resp_payload, &add_resp, sizeof(add_resp));
                resp_data = (const void *)resp_payload;
            }
            exec_end_tick = current_tick();

            if (cxl_send_response(resp_conns[node_id], rpc_id,
                                  resp_data, response_size) < 0) {
                fprintf(stderr, "server: send response failed\n");
                rc = 1;
                break;
            }
            resp_end_tick = current_tick();
            if (!first_resp_marker_emitted) {
                rpc_markerf("first_response_submitted",
                            "node=%u,rpc_id=%u,response_size=%zu",
                            (unsigned)node_id, (unsigned)rpc_id, response_size);
                first_resp_marker_emitted = 1;
            }
            if (timings &&
                requests_processed < (uint64_t)max_requests) {
                server_request_timing_t *record =
                    &timings[requests_processed];
                record->node_id = node_id;
                record->poll_notify_tick =
                    (poll_timing.notify_ready_tick >= poll_phase_start_tick) ?
                    (poll_timing.notify_ready_tick - poll_phase_start_tick) : 0;
                record->poll_req_data_tick =
                    (poll_done_tick >= poll_timing.notify_ready_tick) ?
                    (poll_done_tick - poll_timing.notify_ready_tick) : 0;
                record->exec_tick =
                    (exec_end_tick >= poll_done_tick) ?
                    (exec_end_tick - poll_done_tick) : 0;
                record->resp_submit_tick =
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
            for (int i = 0; i < idle_pause_iters && keep_running; i++)
                __asm__ __volatile__("pause" ::: "memory");
        } else {
            fprintf(stderr, "server: poll request failed\n");
            rc = 1;
            break;
        }
    }

cleanup:
    if (timings) {
        for (uint64_t i = 0;
             i < requests_processed && i < (uint64_t)max_requests;
             i++) {
            const server_request_timing_t *record = &timings[i];
            printf("server_req_%lu_node_id=%u\n",
                   (unsigned long)i, (unsigned)record->node_id);
            printf("server_req_%lu_poll_notify_tick=%lu\n",
                   (unsigned long)i, record->poll_notify_tick);
            printf("server_req_%lu_poll_req_data_tick=%lu\n",
                   (unsigned long)i, record->poll_req_data_tick);
            printf("server_req_%lu_exec_tick=%lu\n",
                   (unsigned long)i, record->exec_tick);
            printf("server_req_%lu_resp_submit_tick=%lu\n",
                   (unsigned long)i, record->resp_submit_tick);
        }
    }
    fflush(stdout);
    fflush(stderr);

    free(timings);
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
