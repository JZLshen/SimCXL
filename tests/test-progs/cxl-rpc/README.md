# CXL RPC

This directory contains the CPU-to-CPU RPC workflow built on top of CXL memory
in SimCXL.

The main pieces are:

- `tests/test-progs/cxl-rpc/`: example client/server programs, helper scripts,
  and guest-image setup flow
- `tests/test-progs/lib/libcxlrpc/`: the user-space RPC library used by those
  programs

This document is the single README for both the workflow and the public RPC
library interface.

## Canonical Client Response Model

The client response path uses exactly one model:

1. The client tracks its local consumer cursor in `response_data`.
2. The client polls only the shared producer cursor flag.
3. If the producer cursor is ahead of the local consumer cursor, the client
   drains `response_data` strictly in producer order.
4. Draining stops when the local consumer cursor catches the producer cursor
   snapshot read from the flag.

A response is not considered complete until its payload has been copied into
client-owned local memory.

## Public API Summary

### Connection setup API

The public `libcxlrpc` flow uses caller-provided fixed shared-memory ranges.
There is no public dynamic allocator path anymore.

Preferred role-specific constructors:

```c
cxl_connection_t *cxl_connection_create_server_poll_owner(
    cxl_context_t *ctx,
    const cxl_connection_addrs_t *addrs,
    uint32_t mq_entries);

cxl_connection_t *cxl_connection_create_client_attach(
    cxl_context_t *ctx,
    const cxl_connection_addrs_t *addrs);

cxl_connection_t *cxl_connection_create_response_tx(
    cxl_context_t *ctx);
```

Notes:

- `create_server_poll_owner()` is the destructive bootstrap path for the
  shared request queue. It initializes only server request-poll state.
- `create_client_attach()` initializes only the client request-send and
  response-drain state.
- `create_response_tx()` initializes only server response transmit state and
  does not map any local fixed-layout doorbell / metadata / response / flag
  ranges.

### Doorbell / RPC ID layout

The metadata/doorbell entry is always 16 bytes:

```text
bytes 0..7:
  bit[0]      method   (0=request, 1=head_update)
  bit[1]      inline
  bit[2]      phase
  bits[34:3]  length   (32 bits)
  bits[48:35] node_id  (14 bits)
  bits[63:49] rpc_id   (15 bits, 0 reserved as invalid)

bytes 8..15:
  inline      request payload bytes
  non-inline  request_data logical address
```

- `rpc_id` is a connection-local logical request ID in `[1, 32767]`
- `node_id` identifies which client/node the server should route the response to
- `service_id`, `method_id`, and `client_tag` are no longer part of the public
  protocol

### Client-side API

```c
int cxl_connection_bind_copyengine_lane(
    cxl_connection_t *conn,
    size_t engine_index,
    uint32_t channel_index);

int cxl_send_request(
    cxl_connection_t *conn,
    const void *data,
    size_t len);

int cxl_peek_response_producer_cursor(
    cxl_connection_t *conn,
    uint64_t *out_cursor);

int cxl_consume_next_response(
    cxl_connection_t *conn,
    void *out_data,
    size_t *out_len,
    uint16_t *out_rpc_id);
```

Notes:

- `cxl_send_request()` returns the allocated `rpc_id` on success.
- The current public `request_data` path is append-only for one connection
  lifetime. It does not reclaim or wrap `request_data` at runtime.
- `cxl_peek_response_producer_cursor()` reads only the flag and returns the
  committed response producer cursor.
- `cxl_consume_next_response()` consumes exactly one response in producer order.
- `cxl_consume_next_response()` copies the payload into caller-owned local
  memory and then advances the local response consumer cursor.
- If `*out_len` is too small, `cxl_consume_next_response()` returns `-1`,
  writes back the exact required size, and does not consume the entry.
- Server-side response connections always need peer `response_data` / `flag`
  setup.
- Response entries whose `header + payload` size is at most `4 KiB` publish by
  CPU store+flush only.
- Response entries whose `header + payload` size exceeds `4 KiB` publish by one
  ordered asynchronous CopyEngine chain and therefore require one dedicated lane
  to be bound first.

### Server-side API

```c
int cxl_poll_request(
    cxl_connection_t *conn,
    uint16_t *node_id,
    uint16_t *rpc_id,
    const void **out_data_view,
    size_t *out_len);

int cxl_poll_request_timed(
    cxl_connection_t *conn,
    uint16_t *node_id,
    uint16_t *rpc_id,
    const void **out_data_view,
    size_t *out_len,
    cxl_request_poll_timing_t *out_timing);

int cxl_send_response(
    cxl_connection_t *conn,
    uint16_t rpc_id,
    const void *data,
    size_t len);

int cxl_connection_set_peer_response_data(
    cxl_connection_t *conn,
    uint64_t peer_response_data_addr,
    size_t peer_response_data_size);

int cxl_connection_set_peer_response_flag_addr(
    cxl_connection_t *conn,
    uint64_t peer_flag_addr);
```

Notes:

- `cxl_poll_request()` is non-blocking.
- `cxl_poll_request()` returns the source `node_id` and request `rpc_id`.
- `cxl_poll_request_timed()` additionally returns three success-path cut points:
  notification parsed, current request-data view ready, and poll tail done.
- `cxl_send_response()` writes variable-length, cacheline-aligned entries into
  one large shared response ring.
- Small responses publish directly by CPU copy plus `clflushopt`/`sfence`.
- Large response entries (`header + payload > 4 KiB`) publish by one
  asynchronous CopyEngine descriptor chain: response entry, then producer cursor
  flag.

## CopyEngine Lane Binding

Dedicated CopyEngine lanes are required for the large-response DMA path. One
lane still means one `(engine, channel)` pair.

Implications:

- response DMA for one client does not round-robin onto lanes used by other
  clients
- total available lanes must be at least the number of active response
  connections
- the public path binds client `i` to the global lane with index `i`
- the default runtime topology auto-derives the minimum
  `(engine_count, channels_per_engine)` pair whose total lane count is large
  enough for the requested client count
- the current public X86 board keeps all CopyEngines on PCI bus 0, function 0,
  and therefore supports at most `29` single-function engines or
  `29 * channels_per_engine` active clients in the default multi-channel
  topology

If you want the strictest isolation model, run with one channel per engine and
one engine per active client. In that default public setup, bind
`(engine_index = node_id, channel_index = 0)`.

## Local Build

Build the RPC library:

```bash
make -C tests/test-progs/lib/libcxlrpc all
```

Build the example programs:

```bash
make -C tests/test-progs/cxl-rpc rpc_client_example rpc_server_example
```

Build the CopyEngine sanity test when needed:

```bash
make -C tests/test-progs/cxl-rpc CXL_RPC_INCLUDE_CE_SANITY=1 cxl_copyengine_sanity
```

## End-to-End Workflow

### 1. Build gem5

```bash
scons build/X86/gem5.opt -j$(nproc)
```

### 2. Build and inject RPC binaries into the guest image

```bash
bash tests/test-progs/cxl-rpc/scripts/setup_disk_image.sh files/parsec.img
```

This script compiles `tests/test-progs/cxl-rpc` and injects binaries into
`/home/test_code` inside the guest image.

It also installs helper launch scripts in the guest:

- `/home/test_code/run_rpc_server_clients.sh` for the unified server+clients launcher
- `/home/test_code/run_rpc_server_client.sh` as the single-client wrapper
- `/home/test_code/run_rpc_server_multi_client.sh` as the multi-client wrapper

### 3. Save a reusable checkpoint

```bash
build/X86/gem5.opt \
  -d output/rebuild_ckpt_4c_example \
  configs/example/gem5_library/x86-cxl-rpc-save-checkpoint.py \
  --num_cpus 4 \
  --rpc_client_count 2
```

The checkpoint directory is typically:

`output/rebuild_ckpt_4c_example/cxl_rpc_checkpoint`

If the guest never reaches the intended readfile handoff `m5 exit`, checkpoint
save now fails instead of writing a checkpoint from the wrong state.

### 4. Run experiments from the checkpoint

#### `1x1` example

One client sends one request:

```bash
build/X86/gem5.opt \
  -d output/reco_1x1_timing_example \
  configs/example/gem5_library/x86-cxl-rpc-test.py \
  --cpu_type TIMING \
  --num_cpus 4 \
  --checkpoint output/rebuild_ckpt_4c_example/cxl_rpc_checkpoint \
  --test_cmd "bash /home/test_code/run_rpc_server_client.sh /home/test_code/rpc_server_example /home/test_code/rpc_client_example --requests 1 --max-polls 2000000 --silent"
```

#### `2x1` example

Two clients each send one request:

```bash
build/X86/gem5.opt \
  -d output/reco_2x1_timing_example \
  configs/example/gem5_library/x86-cxl-rpc-test.py \
  --cpu_type TIMING \
  --num_cpus 4 \
  --checkpoint output/rebuild_ckpt_4c_example/cxl_rpc_checkpoint \
  --test_cmd "CXL_RPC_CLIENT_COUNT=2 bash /home/test_code/run_rpc_server_multi_client.sh /home/test_code/rpc_server_example /home/test_code/rpc_client_example --requests 1 --max-polls 2000000 --silent"
```

The test config derives the CopyEngine topology from `rpc_client_count`.
If `--rpc_client_count` is not passed explicitly, it infers the client count
from `CXL_RPC_CLIENT_COUNT=...`, `--num-clients`, or the standard multi-client
launcher name inside `--test_cmd`.
For client counts above `29`, the public configs automatically increase the
channel count per engine so the total number of response lanes still covers all
clients, while keeping the board on the existing bus-0 single-function engine
topology.

`rpc_client_example`, `cxl_mem_copy_cmp`, and `cpu_memmove_bw` use
`m5_rpns()` for tick capture. Because that pseudo-instruction traps as an
invalid opcode under guest `KVM`, `x86-cxl-rpc-test.py` rejects
`--cpu_type KVM` when the guest command launches any of those binaries. This
is intentional fail-fast behavior for the current RPC path.

### 5. Read results

The main guest console output is:

- `output/<run_dir>/board.pc.com_1.device`

Important markers in that file:

- `server_ready=1`
- `req_<n>_start_tick=<...>`
- `req_<n>_end_tick=<...>`
- `TEST_CMD_EXIT_CODE=<...>`

Tick conversion in this setup:

- `1 tick = 1 ps`
- `1000 ticks = 1 ns`

## Optional Wrapper

You can also use the wrapper script:

```bash
tests/test-progs/cxl-rpc/scripts/run_rpc_test.sh save-checkpoint --num-cpus 4
tests/test-progs/cxl-rpc/scripts/run_rpc_test.sh server-client --cpu-type TIMING --checkpoint output/cxl_rpc_checkpoint
```

If `cxl_trace.log` is generated, the wrapper can print a post-run CXL trace
summary.
