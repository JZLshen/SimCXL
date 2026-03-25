#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cxl_rpc.h"
#include "cxl_rpc_layout.h"

#define NODE_REGION_INDEX(node_id) ((uint64_t)(node_id) + 1ULL)

#define DEFAULT_MAX_POLLS 2000000
#define DEFAULT_POLL_PAUSE 0
#define SANITY_RESPONSE_BYTES 4096u
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

    uint64_t client_base = region_base(NODE_REGION_INDEX(0));

    cxl_connection_addrs_t rx_addrs = {
        .doorbell_addr = client_base + DOORBELL_OFFSET,
        .metadata_queue_addr = client_base + METADATA_Q_OFFSET,
        .metadata_queue_size = METADATA_Q_SIZE_BYTES,
        .request_data_addr = client_base + REQUEST_DATA_OFFSET,
        .request_data_size = REQUEST_DATA_BYTES,
        .response_data_addr = client_base + RESPONSE_DATA_OFFSET,
        .response_data_size = RESPONSE_DATA_BYTES,
        .flag_addr = client_base + FLAG_OFFSET,
    };

    cxl_connection_t *tx = cxl_connection_create_response_tx(ctx);
    cxl_connection_t *rx = cxl_connection_create_client_attach(ctx, &rx_addrs);
    if (!tx || !rx) {
        fprintf(stderr, "sanity: failed to create role-specific connections\n");
        cxl_connection_destroy(tx);
        cxl_connection_destroy(rx);
        cxl_rpc_destroy(ctx);
        return 1;
    }

    if (cxl_connection_bind_copyengine_lane(tx, 0, 0) < 0) {
        fprintf(stderr, "sanity: bind_copyengine_lane failed\n");
        cxl_connection_destroy(tx);
        cxl_connection_destroy(rx);
        cxl_rpc_destroy(ctx);
        return 1;
    }

    if (cxl_connection_set_peer_response_data(tx,
                                              client_base + RESPONSE_DATA_OFFSET,
                                              RESPONSE_DATA_BYTES) < 0) {
        fprintf(stderr, "sanity: set_peer_response_data failed\n");
        cxl_connection_destroy(tx);
        cxl_connection_destroy(rx);
        cxl_rpc_destroy(ctx);
        return 1;
    }
    if (cxl_connection_set_peer_response_flag_addr(tx,
                                                   client_base + FLAG_OFFSET) < 0) {
        fprintf(stderr, "sanity: set_peer_response_flag_addr failed\n");
        cxl_connection_destroy(tx);
        cxl_connection_destroy(rx);
        cxl_rpc_destroy(ctx);
        return 1;
    }

    uint8_t resp[SANITY_RESPONSE_BYTES];
    uint8_t got_resp[SANITY_RESPONSE_BYTES];
    memset(resp, 0x5A, sizeof(resp));
    memset(got_resp, 0, sizeof(got_resp));
    sanity_resp_t *resp_hdr = (sanity_resp_t *)resp;
    resp_hdr->sum = 0x1122334455667788LL;
    resp_hdr->seq = 0xA5A5A5A5u;
    resp_hdr->status = 0x600DCAFEu;
    uint64_t dummy_request = 0x123456789ABCDEF0ULL;
    int test_rpc_id = cxl_send_request(rx, &dummy_request, sizeof(dummy_request));
    if (test_rpc_id <= 0) {
        fprintf(stderr, "sanity: cxl_send_request failed\n");
        cxl_connection_destroy(tx);
        cxl_connection_destroy(rx);
        cxl_rpc_destroy(ctx);
        return 1;
    }

    printf("=== COPYENGINE SANITY ===\n");
    printf("rpc_id=%u\n", (unsigned)test_rpc_id);
    printf("dst_response=%#llx\n", (unsigned long long)(client_base + RESPONSE_DATA_OFFSET));
    printf("dst_flag=%#llx\n", (unsigned long long)(client_base + FLAG_OFFSET));

    if (cxl_send_response(tx, (uint16_t)test_rpc_id, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "sanity: cxl_send_response failed\n");
        cxl_connection_destroy(tx);
        cxl_connection_destroy(rx);
        cxl_rpc_destroy(ctx);
        return 1;
    }

    int request_pending = 1;
    int ok = 0;
    for (int poll = 0; poll < max_polls && keep_running; poll++) {
        size_t response_len = sizeof(got_resp);
        uint16_t consumed_rpc_id = 0;
        int consume_rc = cxl_consume_next_response(rx,
                                                   &got_resp,
                                                   &response_len,
                                                   &consumed_rpc_id);
        if (consume_rc < 0) {
            fprintf(stderr,
                    "sanity: cxl_consume_next_response failed poll=%d rpc_id=%u\n",
                    poll, consumed_rpc_id);
            break;
        }
        if (consume_rc == 1 && request_pending) {
            if (response_len != sizeof(resp) ||
                consumed_rpc_id != (uint16_t)test_rpc_id) {
                fprintf(stderr,
                        "sanity: invalid consumed response len=%zu rpc_id=%u\n",
                        response_len, consumed_rpc_id);
                break;
            }
            if (memcmp(got_resp, resp, sizeof(resp)) != 0) {
                fprintf(stderr,
                        "sanity: payload mismatch sum=%lld seq=%u status=%#x\n",
                        (long long)((const sanity_resp_t *)got_resp)->sum,
                        ((const sanity_resp_t *)got_resp)->seq,
                        ((const sanity_resp_t *)got_resp)->status);
                break;
            }
            request_pending = 0;
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
