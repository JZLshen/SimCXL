#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cxl_rpc.h"

#define CXL_BASE 0x100000000ULL
#define CXL_SIZE 0x42000000ULL

#define CLIENT_REGION_SIZE 0x02000000ULL
#define SERVER_REGION_INDEX 0
#define CLIENT_REGION_INDEX(client_id) ((uint64_t)(client_id) + 1ULL)

#define DOORBELL_OFFSET 0x00000000ULL
#define METADATA_Q_OFFSET 0x00001000ULL
#define REQUEST_DATA_OFFSET 0x00005000ULL
#define RESPONSE_DATA_OFFSET 0x00A05000ULL
#define FLAG_OFFSET 0x01405000ULL

#define REQUEST_DATA_BYTES (10ULL * 1024ULL * 1024ULL)
#define RESPONSE_DATA_BYTES (10ULL * 1024ULL * 1024ULL)

#define DEFAULT_MAX_POLLS 2000000
#define DEFAULT_POLL_PAUSE 0
#define TEST_REQ_ID 0x1234u

static volatile int keep_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    keep_running = 0;
}

static uint64_t region_base(uint64_t index)
{
    return CXL_BASE + index * CLIENT_REGION_SIZE;
}

typedef struct __attribute__((packed)) {
    int64_t sum;
    uint32_t seq;
    uint32_t status;
} sanity_resp_t;

static int parse_int_arg(int argc, char **argv, const char *flag, int default_val)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], flag) == 0)
            return atoi(argv[i + 1]);
    }
    return default_val;
}

int main(int argc, char **argv)
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    setlinebuf(stdout);
    setvbuf(stderr, NULL, _IONBF, 0);

    int max_polls = parse_int_arg(argc, argv, "--max-polls", DEFAULT_MAX_POLLS);
    int poll_pause = parse_int_arg(argc, argv, "--poll-pause", DEFAULT_POLL_PAUSE);

    cxl_context_t *ctx = cxl_rpc_init(CXL_BASE, CXL_SIZE);
    if (!ctx) {
        fprintf(stderr, "sanity: cxl_rpc_init failed\n");
        return 1;
    }

    uint64_t server_base = region_base(SERVER_REGION_INDEX);
    uint64_t client_base = region_base(CLIENT_REGION_INDEX(0));

    cxl_connection_addrs_t tx_addrs = {
        .doorbell_addr = server_base + DOORBELL_OFFSET,
        .metadata_queue_addr = server_base + METADATA_Q_OFFSET,
        .metadata_queue_size = 16 * 1024,
        .request_data_addr = server_base + REQUEST_DATA_OFFSET,
        .request_data_size = REQUEST_DATA_BYTES,
        .response_data_addr = server_base + RESPONSE_DATA_OFFSET,
        .response_data_size = RESPONSE_DATA_BYTES,
        .flag_addr = server_base + FLAG_OFFSET,
    };

    cxl_connection_addrs_t rx_addrs = {
        .doorbell_addr = client_base + DOORBELL_OFFSET,
        .metadata_queue_addr = client_base + METADATA_Q_OFFSET,
        .metadata_queue_size = 16 * 1024,
        .request_data_addr = client_base + REQUEST_DATA_OFFSET,
        .request_data_size = REQUEST_DATA_BYTES,
        .response_data_addr = client_base + RESPONSE_DATA_OFFSET,
        .response_data_size = RESPONSE_DATA_BYTES,
        .flag_addr = client_base + FLAG_OFFSET,
    };

    cxl_connection_t *tx = cxl_connection_create_fixed(ctx, &tx_addrs, 1024);
    cxl_connection_t *rx = cxl_connection_create_fixed(ctx, &rx_addrs, 1024);
    if (!tx || !rx) {
        fprintf(stderr, "sanity: failed to create fixed connections\n");
        cxl_connection_destroy(tx);
        cxl_connection_destroy(rx);
        cxl_rpc_destroy(ctx);
        return 1;
    }

    if (cxl_connection_set_peer_addrs(tx,
                                      client_base + RESPONSE_DATA_OFFSET,
                                      RESPONSE_DATA_BYTES) < 0) {
        fprintf(stderr, "sanity: set_peer_addrs failed\n");
        cxl_connection_destroy(tx);
        cxl_connection_destroy(rx);
        cxl_rpc_destroy(ctx);
        return 1;
    }
    if (cxl_connection_set_peer_flag_addr(tx, client_base + FLAG_OFFSET) < 0) {
        fprintf(stderr, "sanity: set_peer_flag_addr failed\n");
        cxl_connection_destroy(tx);
        cxl_connection_destroy(rx);
        cxl_rpc_destroy(ctx);
        return 1;
    }

    sanity_resp_t resp = {
        .sum = 0x1122334455667788LL,
        .seq = 0xA5A5A5A5u,
        .status = 0x600DCAFEu,
    };

    printf("=== COPYENGINE SANITY ===\n");
    printf("req_id=%u\n", TEST_REQ_ID);
    printf("dst_response=%#llx\n", (unsigned long long)(client_base + RESPONSE_DATA_OFFSET));
    printf("dst_flag=%#llx\n", (unsigned long long)(client_base + FLAG_OFFSET));

    if (cxl_send_response(tx, TEST_REQ_ID, &resp, sizeof(resp)) < 0) {
        fprintf(stderr, "sanity: cxl_send_response failed\n");
        cxl_connection_destroy(tx);
        cxl_connection_destroy(rx);
        cxl_rpc_destroy(ctx);
        return 1;
    }

    const void *payload_view = NULL;
    size_t out_len = 0;
    uint32_t got_req_id = 0;
    int ok = 0;
    for (int poll = 0; poll < max_polls && keep_running; poll++) {
        out_len = 0;
        payload_view = NULL;
        got_req_id = 0;
        int rc = cxl_poll_response_view(rx, TEST_REQ_ID,
                                        &payload_view, &out_len,
                                        &got_req_id);
        if (rc < 0) {
            fprintf(stderr,
                    "sanity: cxl_poll_response_view failed poll=%d got_req_id=%u\n",
                    poll, got_req_id);
            break;
        }
        if (rc == 1) {
            if (!payload_view || out_len != sizeof(resp) || got_req_id != TEST_REQ_ID) {
                fprintf(stderr,
                        "sanity: invalid response view len=%zu got_req_id=%u\n",
                        out_len, got_req_id);
                break;
            }
            const sanity_resp_t *got = (const sanity_resp_t *)payload_view;
            if (memcmp(got, &resp, sizeof(resp)) != 0) {
                fprintf(stderr,
                        "sanity: payload mismatch sum=%lld seq=%u status=%#x\n",
                        (long long)got->sum, got->seq, got->status);
                break;
            }
            printf("sanity_poll_hit=%d\n", poll);
            printf("sanity_result=PASS\n");
            ok = 1;
            break;
        }
        for (int i = 0; i < poll_pause; i++)
            __asm__ __volatile__("pause" ::: "memory");
    }

    if (!ok)
        printf("sanity_result=FAIL\n");

    cxl_connection_destroy(tx);
    cxl_connection_destroy(rx);
    cxl_rpc_destroy(ctx);
    return ok ? 0 : 1;
}
