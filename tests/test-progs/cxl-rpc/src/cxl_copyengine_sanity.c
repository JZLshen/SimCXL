#define _GNU_SOURCE
#include <ctype.h>
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
#define DEFAULT_RESPONSE_SIZE 4096u
#define RESPONSE_DMA_THRESHOLD 4096u
#define MIN_RESPONSE_SIZE 8u
#define MAX_RESPONSE_SIZE (RESPONSE_DATA_BYTES - 8ULL)
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

static int
has_flag_arg(int argc, char **argv, const char *flag)
{
    if (!flag)
        return 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], flag) == 0)
            return 1;
    }
    return 0;
}

static int
parse_size_literal(const char *text, size_t min_val, size_t max_val, size_t *out)
{
    char *end = NULL;
    unsigned long long raw = 0;
    unsigned long long mul = 1;

    if (!text || !out || text[0] == '\0')
        return -1;

    errno = 0;
    raw = strtoull(text, &end, 0);
    if (errno != 0 || end == text)
        return -1;

    if (*end != '\0') {
        const int c0 = toupper((unsigned char)end[0]);
        if (c0 == 'K') {
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

static inline size_t
response_publish_bytes(size_t payload_len)
{
    return 8u + payload_len;
}

static inline size_t
response_entry_size(size_t payload_len)
{
    return ((response_publish_bytes(payload_len) + 63u) / 64u) * 64u;
}

static inline void
invalidate_cacheline(const volatile void *ptr)
{
    __asm__ __volatile__("clflushopt (%0)" :: "r"((const void *)ptr) : "memory");
}

static inline void
invalidate_load_barrier(void)
{
    __asm__ __volatile__("sfence" ::: "memory");
}

static uint64_t
read_u64_remote(volatile const uint8_t *ptr)
{
    uint64_t val = 0;

    invalidate_cacheline(ptr);
    invalidate_load_barrier();
    memcpy(&val, (const void *)ptr, sizeof(val));
    return val;
}

static void
dump_hex_bytes(const char *label, volatile const uint8_t *ptr, size_t bytes)
{
    fprintf(stderr, "%s=", label ? label : "bytes");
    for (size_t i = 0; i < bytes; ++i)
        fprintf(stderr, "%02x", (unsigned)ptr[i]);
    fputc('\n', stderr);
}

static void
dump_hex_bytes_invalidated(const char *label,
                           volatile const uint8_t *ptr,
                           size_t bytes)
{
    if (!ptr || bytes == 0) {
        fprintf(stderr, "%s=\n", label ? label : "bytes");
        return;
    }

    uintptr_t line_start = ((uintptr_t)ptr) & ~((uintptr_t)63);
    uintptr_t line_end = (((uintptr_t)ptr + bytes) + 63u) & ~((uintptr_t)63);
    for (uintptr_t line = line_start; line < line_end; line += 64u)
        invalidate_cacheline((const volatile void *)line);
    invalidate_load_barrier();
    dump_hex_bytes(label, ptr, bytes);
}

static inline size_t
clamp_dump_len(size_t size, size_t max_dump)
{
    return size < max_dump ? size : max_dump;
}

static size_t
first_mismatch_offset(const uint8_t *expected,
                      const uint8_t *got,
                      size_t size)
{
    if (!expected || !got)
        return size;

    for (size_t i = 0; i < size; ++i) {
        if (expected[i] != got[i])
            return i;
    }
    return size;
}

int main(int argc, char **argv)
{
    int rc = 1;
    int force_copyengine = 0;
    uint8_t *resp = NULL;
    uint8_t *got_resp = NULL;
    volatile uint8_t *resp_slot = NULL;
    volatile uint8_t *flag_ptr = NULL;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    setlinebuf(stdout);
    setvbuf(stderr, NULL, _IONBF, 0);

    int max_polls = parse_int_arg(argc, argv, "--max-polls", DEFAULT_MAX_POLLS);
    int poll_pause = parse_int_arg(argc, argv, "--poll-pause", DEFAULT_POLL_PAUSE);
    size_t response_size = DEFAULT_RESPONSE_SIZE;
    force_copyengine = has_flag_arg(argc, argv, "--force-copyengine");
    if (parse_size_arg(argc, argv, "--response-size", DEFAULT_RESPONSE_SIZE,
                       MIN_RESPONSE_SIZE, MAX_RESPONSE_SIZE,
                       &response_size) != 0) {
        fprintf(stderr,
                "sanity: invalid --response-size (range %uB..%zuB)\n",
                MIN_RESPONSE_SIZE, (size_t)MAX_RESPONSE_SIZE);
        return 1;
    }

    if (force_copyengine &&
        setenv("CXL_RPC_FORCE_RESPONSE_DMA", "1", 1) != 0) {
        fprintf(stderr, "sanity: setenv(CXL_RPC_FORCE_RESPONSE_DMA) failed: %s\n",
                strerror(errno));
        return 1;
    }

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

    resp_slot = (volatile uint8_t *)cxl_rpc_phys_to_virt(ctx,
                                                         client_base +
                                                         RESPONSE_DATA_OFFSET);
    flag_ptr = (volatile uint8_t *)cxl_rpc_phys_to_virt(ctx,
                                                        client_base +
                                                        FLAG_OFFSET);
    if (!resp_slot || !flag_ptr) {
        fprintf(stderr, "sanity: failed to map response/flag pointers\n");
        cxl_connection_destroy(tx);
        cxl_connection_destroy(rx);
        cxl_rpc_destroy(ctx);
        return 1;
    }

    resp = (uint8_t *)malloc(response_size);
    got_resp = (uint8_t *)malloc(response_size);
    if (!resp || !got_resp) {
        fprintf(stderr, "sanity: malloc failed response_size=%zu\n",
                response_size);
        cxl_connection_destroy(tx);
        cxl_connection_destroy(rx);
        cxl_rpc_destroy(ctx);
        free(resp);
        free(got_resp);
        return 1;
    }
    memset(resp, 0x5A, response_size);
    memset(got_resp, 0, response_size);
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
        free(resp);
        free(got_resp);
        return 1;
    }

    printf("=== COPYENGINE SANITY ===\n");
    printf("rpc_id=%u\n", (unsigned)test_rpc_id);
    printf("response_size=%zu\n", response_size);
    printf("publish_bytes=%zu\n", response_publish_bytes(response_size));
    printf("entry_size=%zu\n", response_entry_size(response_size));
    printf("force_copyengine=%d\n", force_copyengine);
    printf("expect_dma=%d\n",
           (force_copyengine ||
            response_publish_bytes(response_size) > RESPONSE_DMA_THRESHOLD) ? 1 : 0);
    printf("dst_response=%#llx\n", (unsigned long long)(client_base + RESPONSE_DATA_OFFSET));
    printf("dst_flag=%#llx\n", (unsigned long long)(client_base + FLAG_OFFSET));

    if (cxl_send_response(tx, (uint16_t)test_rpc_id, resp, response_size) < 0) {
        fprintf(stderr, "sanity: cxl_send_response failed\n");
        cxl_connection_destroy(tx);
        cxl_connection_destroy(rx);
        cxl_rpc_destroy(ctx);
        free(resp);
        free(got_resp);
        return 1;
    }

    int request_pending = 1;
    int ok = 0;
    for (int poll = 0; poll < max_polls && keep_running; poll++) {
        size_t response_len = response_size;
        uint16_t consumed_rpc_id = 0;
        int consume_rc = cxl_consume_next_response(rx,
                                                   got_resp,
                                                   &response_len,
                                                   &consumed_rpc_id);
        if (consume_rc < 0) {
            uint64_t producer_cursor = 0;
            int producer_rc =
                cxl_peek_response_producer_cursor(rx, &producer_cursor);
            fprintf(stderr,
                    "sanity: cxl_consume_next_response failed poll=%d rpc_id=%u producer_rc=%d producer_cursor=%#llx raw_flag=%#llx raw_header=%#llx\n",
                    poll, consumed_rpc_id, producer_rc,
                    (unsigned long long)producer_cursor,
                    (unsigned long long)read_u64_remote(flag_ptr),
                    (unsigned long long)read_u64_remote(resp_slot));
            dump_hex_bytes("sanity_resp_first32", resp_slot, 32u);
            break;
        }
        if (consume_rc == 1 && request_pending) {
            if (response_len != response_size ||
                consumed_rpc_id != (uint16_t)test_rpc_id) {
                fprintf(stderr,
                        "sanity: invalid consumed response len=%zu rpc_id=%u expected_len=%zu expected_rpc_id=%u\n",
                        response_len, consumed_rpc_id, response_size,
                        (unsigned)test_rpc_id);
                break;
            }
            if (memcmp(got_resp, resp, response_size) != 0) {
                const size_t diff_off =
                    first_mismatch_offset(resp, got_resp, response_size);
                const size_t boundary_off =
                    (response_size >= 16u) ? (response_size - 16u) : 0u;
                fprintf(stderr,
                        "sanity: payload mismatch diff_off=%zu sum=%lld seq=%u status=%#x raw_flag=%#llx raw_header=%#llx\n",
                        diff_off,
                        (long long)((const sanity_resp_t *)got_resp)->sum,
                        ((const sanity_resp_t *)got_resp)->seq,
                        ((const sanity_resp_t *)got_resp)->status,
                        (unsigned long long)read_u64_remote(flag_ptr),
                        (unsigned long long)read_u64_remote(resp_slot));
                dump_hex_bytes("sanity_expected_first32", resp,
                               clamp_dump_len(response_size, 32u));
                dump_hex_bytes("sanity_got_first32", got_resp,
                               clamp_dump_len(response_size, 32u));
                dump_hex_bytes("sanity_resp_first32", resp_slot, 32u);
                dump_hex_bytes("sanity_expected_tail32", resp + boundary_off,
                               clamp_dump_len(response_size - boundary_off, 32u));
                dump_hex_bytes("sanity_got_tail32", got_resp + boundary_off,
                               clamp_dump_len(response_size - boundary_off, 32u));
                dump_hex_bytes("sanity_resp_boundary32",
                               resp_slot + 4080u,
                               32u);
                dump_hex_bytes_invalidated("sanity_resp_line64_inv32",
                                           resp_slot + 64u,
                                           32u);
                dump_hex_bytes_invalidated("sanity_resp_boundary_inv32",
                                           resp_slot + 4080u,
                                           32u);
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
    free(resp);
    free(got_resp);
    rc = ok ? 0 : 1;
    return rc;
}
