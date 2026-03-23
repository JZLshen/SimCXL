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

1. The client keeps a local pending `rpc_id` set or list.
2. The client polls only the completion flag.
3. If the flag `rpc_id` is still pending, the client drains `resp_data` in
   producer order.
4. Draining stops when the consumed response `rpc_id` matches the flag snapshot.

A response is not considered complete until its payload has been copied into
client-owned local memory.

## Public API Summary

### Connection setup API

The public `libcxlrpc` flow uses caller-provided fixed shared-memory ranges.
There is no public dynamic allocator path anymore.

Use one of these constructors:

```c
cxl_connection_t *cxl_connection_create_fixed_owner(
    cxl_context_t *ctx,
    const cxl_connection_addrs_t *addrs,
    uint32_t mq_entries);

cxl_connection_t *cxl_connection_create_fixed_attach(
    cxl_context_t *ctx,
    const cxl_connection_addrs_t *addrs,
    uint32_t mq_entries);

cxl_connection_t *cxl_connection_create_fixed(
    cxl_context_t *ctx,
    const cxl_connection_addrs_t *addrs,
    uint32_t mq_entries);
```

Notes:

- `fixed_owner` is the destructive bootstrap path for the owner of a fixed
  region. It may clear shared metadata / flag state and reset controller-side
  queue state.
- `fixed_attach` is the non-destructive attach path for peers joining an
  already-defined fixed layout.
- `fixed` is a convenience alias of `fixed_owner`.

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
- `rpc_id` stays reserved until the response payload has been consumed into
  local memory
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

int cxl_peek_latest_completed_rpc_id(
    cxl_connection_t *conn,
    uint16_t *out_rpc_id);

int cxl_consume_next_response(
    cxl_connection_t *conn,
    void *out_data,
    size_t *out_len,
    uint16_t *out_rpc_id);
```

Notes:

- `cxl_send_request()` returns the allocated `rpc_id` on success.
- `cxl_peek_latest_completed_rpc_id()` reads only the flag.
- `cxl_consume_next_response()` consumes exactly one response in producer order.
- `cxl_consume_next_response()` copies the payload into caller-owned local
  memory before reclaiming the `rpc_id`.
- If `*out_len` is too small, `cxl_consume_next_response()` returns `-1`,
  writes back the exact required size, and does not consume the entry.
- Server-side response connections must bind one dedicated CopyEngine lane
  explicitly before response-data / flag setup.

### Server-side API

```c
int cxl_poll_request(
    cxl_connection_t *conn,
    uint16_t *node_id,
    uint16_t *rpc_id,
    const void **out_data_view,
    size_t *out_len);

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
