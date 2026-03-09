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

1. The client keeps a local pending `req_id` set or list.
2. The client polls only the completion flag.
3. If the flag `req_id` is still pending, the client drains `resp_data` in
   producer order.
4. Draining stops when the consumed response `req_id` matches the flag snapshot.

A response is not considered complete until its payload has been copied into
client-owned local memory.

## Public API Summary

### Request ID layout

`request_id` is always 16 bits:

```text
request_id = (client_tag << (16 - client_tag_bits)) | seq_low_bits
```

- `client_tag` occupies the high bits
- `seq_low_bits` is allocated per connection
- a `request_id` stays reserved until the response payload has been consumed
  into local memory

For a single-client deployment, use `client_tag = 0` and
`client_tag_bits = 0`.

### Client-side API

```c
int cxl_connection_set_client_tag(
    cxl_connection_t *conn,
    uint16_t client_tag,
    uint8_t client_tag_bits);

int cxl_connection_bind_copyengine_lane(
    cxl_connection_t *conn,
    size_t engine_index,
    uint32_t channel_index);

int cxl_send_request(
    cxl_connection_t *conn,
    uint16_t service_id,
    uint16_t method_id,
    const void *data,
    size_t len);

int cxl_peek_latest_completed_request_id(
    cxl_connection_t *conn,
    uint16_t *out_request_id);

int cxl_consume_next_response(
    cxl_connection_t *conn,
    void *out_data,
    size_t *out_len,
    uint16_t *out_request_id);
```

Notes:

- `cxl_peek_latest_completed_request_id()` reads only the flag.
- `cxl_consume_next_response()` consumes exactly one response in producer order.
- `cxl_consume_next_response()` copies the payload into caller-owned local
  memory before reclaiming the `request_id`.
- If `*out_len` is too small, `cxl_consume_next_response()` returns `-1`,
  writes back the exact required size, and does not consume the entry.
- Server-side response connections must bind one dedicated CopyEngine lane
  explicitly before response-data / flag setup.

### Server-side API

```c
int cxl_poll_request(
    cxl_connection_t *conn,
    uint16_t *service_id,
    uint16_t *method_id,
    uint16_t *request_id,
    const void **out_data_view,
    size_t *out_len);

int cxl_send_response(
    cxl_connection_t *conn,
    uint16_t request_id,
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
- With the current backend, `cxl_send_response()` supports payloads up to
  `4088` bytes.
- Server responses are sent through `CopyEngine` after one fixed lane is
  bound and both peer `response_data` and peer `flag` are configured.

## CopyEngine Lane Binding

Each server-side response connection binds one dedicated CopyEngine lane, where
one lane means one `(engine, channel)` pair.

Implications:

- response DMA for one client does not round-robin onto lanes used by other
  clients
- total available lanes must be at least the number of active response
  connections
- the public path uses `1 client : 1 engine : 1 channel`
- the default runtime topology keeps `channel_index = 0` and derives the
  engine count automatically from the client count

If you want the strictest isolation model, run with one channel per engine and
one engine per active client. In that default public setup, bind
`(engine_index = client_id, channel_index = 0)`.

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
save now fails instead of emitting a fallback checkpoint from the wrong state.

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

### 5. Read results

The main guest console output is:

- `output/<run_dir>/board.pc.com_1.device`

Important markers in that file:

- `server_ready=1`
- `req_<n>_start_tick=<...>`
- `req_<n>_end_tick=<...>`
- `req_<n>_delta_tick=<...>`
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
