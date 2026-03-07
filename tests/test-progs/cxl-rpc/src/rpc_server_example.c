/*
 * Minimal CXL RPC server example.
 *
 * Keeps only request -> response handling.
 * No summary output.
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

/* N-client : 1-server layout (must match rpc_client_example.c). */
#define CLIENT_REGION_SIZE 0x02000000ULL
#define TOTAL_REGIONS (CXL_SIZE / CLIENT_REGION_SIZE)
#define SERVER_REGION_INDEX 0ULL
#define SERVER_REGION_BASE (CXL_BASE + SERVER_REGION_INDEX * CLIENT_REGION_SIZE)
#define MAX_CLIENTS ((int)(TOTAL_REGIONS - 1ULL))

#define DOORBELL_OFFSET 0x00000000ULL
#define METADATA_Q_OFFSET 0x00001000ULL
#define REQUEST_DATA_OFFSET 0x00005000ULL
#define RESPONSE_DATA_OFFSET 0x00A05000ULL
#define FLAG_OFFSET 0x01405000ULL

#define REQUEST_DATA_BYTES (10ULL * 1024ULL * 1024ULL)
#define RESPONSE_DATA_BYTES (10ULL * 1024ULL * 1024ULL)

#define DEFAULT_MAX_REQUESTS 0
#define DEFAULT_IDLE_PAUSE_ITERS 0

#define ADD_REQ_MAGIC 0xADDA110Cu
#define ADD_RESP_OK 0x600DCAFEu
#define ADD_RESP_BAD 0xBAD00BADu

typedef struct __attribute__((packed)) {
    int64_t a;
    int64_t b;
    uint32_t seq;
    uint32_t magic;
} add_request_t;

typedef struct __attribute__((packed)) {
    int64_t sum;
    uint32_t seq;
    uint32_t status;
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
resolve_client_id_from_request_id(uint32_t request_id, uint8_t bits,
                                  int num_clients)
{
    if (bits == 0)
        return 0;
    if (bits >= 16)
        return -1;

    uint32_t cid = (request_id >> (16 - bits)) & ((1u << bits) - 1u);
    if ((int)cid >= num_clients)
        return -1;
    return (int)cid;
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

    int max_requests = parse_int_arg(argc, argv, "--max-requests",
                                     DEFAULT_MAX_REQUESTS);
    int idle_pause_iters = parse_int_arg(argc, argv, "--idle-pause",
                                         DEFAULT_IDLE_PAUSE_ITERS);
    int num_clients = parse_int_arg_range(argc, argv, "--num-clients", 1,
                                          1, MAX_CLIENTS);

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
        .metadata_queue_size = 16384,
        .request_data_addr = server_base + REQUEST_DATA_OFFSET,
        .request_data_size = REQUEST_DATA_BYTES,
        .response_data_addr = server_base + RESPONSE_DATA_OFFSET,
        .response_data_size = RESPONSE_DATA_BYTES,
        .flag_addr = server_base + FLAG_OFFSET,
    };

    poll_conn = cxl_connection_create_fixed_owner(ctx, &addrs, 1024);
    if (!poll_conn) {
        fprintf(stderr, "server: connection_create_fixed_owner failed\n");
        rc = 1;
        goto cleanup;
    }

    if (cxl_connection_set_peer_request_data(poll_conn, CXL_BASE, CXL_SIZE) < 0) {
        fprintf(stderr, "server: set peer request range failed\n");
        rc = 1;
        goto cleanup;
    }

    for (int i = 0; i < num_clients; i++) {
        if (num_clients == 1) {
            resp_conns[i] = poll_conn;
        } else {
            resp_conns[i] = cxl_connection_create_fixed_attach(ctx, &addrs, 1024);
            if (!resp_conns[i]) {
                fprintf(stderr, "server: create response connection failed\n");
                rc = 1;
                goto cleanup;
            }
        }

        if (cxl_connection_set_peer_addrs(resp_conns[i],
                                          client_region_base(i) +
                                              RESPONSE_DATA_OFFSET,
                                          RESPONSE_DATA_BYTES) < 0) {
            fprintf(stderr, "server: set peer response range failed\n");
            rc = 1;
            goto cleanup;
        }

        if (cxl_connection_set_peer_flag_addr(resp_conns[i],
                                              client_region_base(i) +
                                                  FLAG_OFFSET) < 0) {
            fprintf(stderr, "server: set peer flag failed\n");
            rc = 1;
            goto cleanup;
        }
    }

    printf("server_ready=1\n");

    uint8_t req_id_tag_bits = client_tag_bits(num_clients);
    while (keep_running) {
        uint32_t request_id = 0;
        const void *req_data_view = NULL;
        size_t req_len = 0;

        int ret = cxl_poll_request(poll_conn,
                                   NULL,
                                   NULL,
                                   &request_id,
                                   &req_data_view,
                                   &req_len);
        if (ret == 1) {
            int client_id = resolve_client_id_from_request_id(
                request_id, req_id_tag_bits, num_clients);
            if (client_id < 0 || client_id >= num_clients) {
                fprintf(stderr, "server: invalid request_id prefix\n");
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
            add_response_t resp = {
                .sum = req_view->a + req_view->b,
                .seq = req_view->seq,
                .status = (req_view->magic == ADD_REQ_MAGIC) ?
                          ADD_RESP_OK : ADD_RESP_BAD,
            };

            if (cxl_send_response(resp_conns[client_id], request_id,
                                  &resp, sizeof(resp)) < 0) {
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

