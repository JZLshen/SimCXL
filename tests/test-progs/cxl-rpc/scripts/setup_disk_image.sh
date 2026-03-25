#!/bin/bash
# setup_disk_image.sh - Copy CXL RPC test binaries to gem5 disk image
#
# Usage:
#   ./scripts/setup_disk_image.sh /path/to/parsec.img
#
# Binaries are copied to /home/test_code/ inside the image.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RPC_DIR="$(dirname "$SCRIPT_DIR")"
SIMCXL_DIR="$(cd "$RPC_DIR/../../.." && pwd)"

# Configurable
DISK_IMAGE="${1:-}"
MOUNT_POINT="/tmp/gem5_disk_mount_$$"
DEST_DIR="/home/test_code"
M5_BIN="$SIMCXL_DIR/util/m5/build/x86/out/m5"
LOOP_DEVICE=""

# Core binaries to copy (default path).
BINARIES=(
    "cxl_mem_ldst_latency"
    "cxl_mem_paper_latency"
    "rpc_client_example"
    "rpc_server_example"
)

# Optional utility binary for standalone memcpy-vs-copyengine experiments.
if [ "${CXL_RPC_INCLUDE_COPY_CMP:-0}" != "0" ]; then
    BINARIES+=("cxl_mem_copy_cmp")
fi

# Optional standalone CopyEngine sanity test binary.
if [ "${CXL_RPC_INCLUDE_CE_SANITY:-0}" != "0" ]; then
    BINARIES+=("cxl_copyengine_sanity")
fi

# Optional standalone CopyEngine bandwidth benchmark.
if [ "${CXL_RPC_INCLUDE_CE_BW:-0}" != "0" ]; then
    BINARIES+=("cxl_copyengine_bw")
fi

# Optional standalone CPU memmove bandwidth benchmark.
if [ "${CXL_RPC_INCLUDE_CPU_MEMMOVE_BW:-0}" != "0" ]; then
    BINARIES+=("cpu_memmove_bw")
fi

# Optional hygiene mode: remove unrelated files under /home/test_code before
# copying the current whitelist binaries.
CLEAN_TEST_CODE_DIR="${CXL_RPC_CLEAN_TEST_CODE_DIR:-0}"

usage() {
    echo "Usage: $0 DISK_IMAGE_PATH"
    echo ""
    echo "Copy CXL RPC test binaries to gem5 disk image."
    echo "Requires sudo for mounting."
    echo "Set CXL_RPC_INCLUDE_COPY_CMP=1 to also copy cxl_mem_copy_cmp."
    echo "Set CXL_RPC_INCLUDE_CE_BW=1 to also copy cxl_copyengine_bw."
    echo "Set CXL_RPC_INCLUDE_CPU_MEMMOVE_BW=1 to also copy cpu_memmove_bw."
    echo "Set CXL_RPC_CLEAN_TEST_CODE_DIR=1 to remove non-whitelist files in ${DEST_DIR}."
    echo ""
    echo "Arguments:"
    echo "  DISK_IMAGE_PATH  Path to disk image file (e.g., parsec.img)"
    echo ""
    echo "Binaries copied to ${DEST_DIR}/ inside image:"
    for b in "${BINARIES[@]}"; do
        echo "  - $b"
    done
}

if [ -z "$DISK_IMAGE" ] || [ "$DISK_IMAGE" = "--help" ] || [ "$DISK_IMAGE" = "-h" ]; then
    usage
    exit 0
fi

echo "=== CXL RPC Disk Image Setup ==="
echo "Disk image: $DISK_IMAGE"
echo "RPC dir:    $RPC_DIR"
echo ""

# Verify disk image
if [ ! -f "$DISK_IMAGE" ]; then
    echo "ERROR: Disk image not found: $DISK_IMAGE"
    exit 1
fi

# Backup disk image
BACKUP="${DISK_IMAGE}.bak"
if [ ! -f "$BACKUP" ]; then
    echo "[0/4] Backing up disk image..."
    cp "$DISK_IMAGE" "$BACKUP"
    echo "  Backup: $BACKUP"
else
    echo "[0/4] Backup already exists: $BACKUP (skip)"
fi
echo ""

# Build binaries
echo "[1/5] Building test binaries..."
make -C "$RPC_DIR" clean
make -C "$RPC_DIR" all

# Build m5 utility if needed
if [ ! -f "$M5_BIN" ]; then
    echo "  Building m5 utility..."
    (cd "$SIMCXL_DIR/util/m5" && scons build/x86/out/m5 -j$(nproc))
fi
echo ""

# Mount disk image
echo "[2/5] Mounting disk image..."
mkdir -p "$MOUNT_POINT"

cleanup() {
    echo "[5/5] Unmounting disk image..."
    sudo umount "$MOUNT_POINT" 2>/dev/null || true
    if [ -n "$LOOP_DEVICE" ]; then
        sudo losetup -d "$LOOP_DEVICE" 2>/dev/null || true
    fi
    rmdir "$MOUNT_POINT" 2>/dev/null || true
}
trap cleanup EXIT

# Try direct mount first, then use a loop device with partition scanning.
if sudo mount -o loop "$DISK_IMAGE" "$MOUNT_POINT" 2>/dev/null; then
    echo "  Mounted directly"
else
    LOOP_DEVICE="$(sudo losetup --find --show -Pf "$DISK_IMAGE" 2>/dev/null || true)"
    LOOP_PARTITION=""
    if [ -n "$LOOP_DEVICE" ]; then
        LOOP_PARTITION="$(sudo lsblk -lnpo NAME,TYPE "$LOOP_DEVICE" 2>/dev/null | \
                         awk '$2 == "part" {print $1; exit}')"
    fi

    if [ -n "$LOOP_PARTITION" ]; then
        sudo mount "$LOOP_PARTITION" "$MOUNT_POINT"
        echo "  Mounted via loop partition ${LOOP_PARTITION}"
    else
        echo "ERROR: Failed to mount disk image"
        exit 1
    fi
fi

# Copy binaries
echo "[3/5] Copying test binaries to ${DEST_DIR}..."
sudo mkdir -p "${MOUNT_POINT}${DEST_DIR}"

removed_non_whitelist=0
if [ "$CLEAN_TEST_CODE_DIR" != "0" ]; then
    echo "  Full clean mode: remove non-whitelist files in ${DEST_DIR}"
    while IFS= read -r existing_path; do
        [ -n "$existing_path" ] || continue
        existing_name="$(basename "$existing_path")"
        keep=0
        for allowed_name in "${BINARIES[@]}"; do
            if [ "$existing_name" = "$allowed_name" ]; then
                keep=1
                break
            fi
        done
        if [ "$keep" -eq 0 ]; then
            sudo rm -f "$existing_path"
            echo "  Removed non-whitelist: $existing_name"
            removed_non_whitelist=$((removed_non_whitelist + 1))
        fi
    done < <(sudo find "${MOUNT_POINT}${DEST_DIR}" -maxdepth 1 -type f 2>/dev/null || true)
fi

copied=0
skipped=0
for bin in "${BINARIES[@]}"; do
    src="$RPC_DIR/$bin"
    if [ -f "$src" ]; then
        sudo cp "$src" "${MOUNT_POINT}${DEST_DIR}/$bin"
        size=$(stat -c%s "$src")
        echo "  Copied: $bin ($size bytes)"
        copied=$((copied + 1))
    else
        echo "  SKIP:   $bin (not found)"
        skipped=$((skipped + 1))
    fi
done

# Create unified server-client helper script inside disk image.
sudo tee "${MOUNT_POINT}${DEST_DIR}/run_rpc_server_clients.sh" > /dev/null << 'UNIFIED_HELPER_SCRIPT'
#!/bin/bash
# Run one server + N clients (N=1 for single-client mode).
set -u

SERVER="${1:-/home/test_code/rpc_server_example}"
CLIENT="${2:-/home/test_code/rpc_client_example}"
shift 2 || true

CLIENT_COUNT="${CXL_RPC_CLIENT_COUNT:-1}"
SERVER_NUMA_NODE="${CXL_RPC_SERVER_NUMA_NODE:-1}"
CLIENT_NUMA_NODE="${CXL_RPC_CLIENT_NUMA_NODE:-1}"
CLIENT_TIMEOUT_SEC="${CXL_RPC_CLIENT_TIMEOUT_SEC:-600}"
SERVER_ARGS="${CXL_RPC_SERVER_ARGS:---silent}"
SERVER_MAX_REQUESTS="${CXL_RPC_SERVER_MAX_REQUESTS:-}"
SERVER_LOG="${CXL_RPC_SERVER_LOG:-/home/test_code/cxl_rpc_server_runtime.log}"
CLIENT_LOG="${CXL_RPC_CLIENT_LOG:-/home/test_code/cxl_rpc_client_runtime.log}"
CLIENT_LOG_PREFIX="${CXL_RPC_CLIENT_LOG_PREFIX:-/home/test_code/cxl_rpc_client_runtime}"
SERVER_READY_MARKER="${CXL_RPC_SERVER_READY_MARKER:-server_ready=1}"
SERVER_READY_TIMEOUT_SEC="${CXL_RPC_SERVER_READY_TIMEOUT_SEC:-60}"
FIRST_REQ_BARRIER_PATH="${CXL_RPC_FIRST_REQ_BARRIER_PATH:-/tmp/cxl_rpc_first_req_${$}_$RANDOM}"
PIN_CORES="${CXL_RPC_PIN_CORES:-0}"
SERVER_CORE="${CXL_RPC_SERVER_CORE:-0}"
CLIENT_CORE_BASE="${CXL_RPC_CLIENT_CORE_BASE:-1}"
DEBUG_LIVE="${CXL_RPC_DEBUG_LIVE:-0}"

if ! [[ "$CLIENT_COUNT" =~ ^[0-9]+$ ]] || [ "$CLIENT_COUNT" -le 0 ]; then
    echo "ERROR: CXL_RPC_CLIENT_COUNT must be a positive integer"
    exit 2
fi

if ! [[ "$CLIENT_TIMEOUT_SEC" =~ ^[0-9]+$ ]]; then
    echo "ERROR: CXL_RPC_CLIENT_TIMEOUT_SEC must be a non-negative integer"
    exit 2
fi

if [ "$CLIENT_TIMEOUT_SEC" -gt 0 ] && ! command -v timeout >/dev/null 2>&1; then
    echo "ERROR: timeout command not found in guest while timeout is enabled"
    exit 2
fi

if [ "$PIN_CORES" != "0" ]; then
    if ! command -v taskset >/dev/null 2>&1; then
        echo "ERROR: taskset command not found in guest while CXL_RPC_PIN_CORES is enabled"
        exit 2
    fi
    cpu_total="$(nproc 2>/dev/null || echo 0)"
    cpu_need=$((CLIENT_COUNT + 1))
    if [[ "$cpu_total" =~ ^[0-9]+$ ]] && [ "$cpu_total" -lt "$cpu_need" ]; then
        echo "ERROR: need at least ${cpu_need} CPUs (server+clients), but nproc=${cpu_total}"
        exit 2
    fi
fi

if [ -z "$SERVER_MAX_REQUESTS" ]; then
    client_requests=""
    prev=""
    for arg in "$@"; do
        if [ "$prev" = "--requests" ]; then
            client_requests="$arg"
            break
        fi
        prev="$arg"
    done
    if [[ "$client_requests" =~ ^[0-9]+$ ]] && [ "$client_requests" -gt 0 ]; then
        SERVER_MAX_REQUESTS=$((CLIENT_COUNT * client_requests))
    fi
fi

case " $SERVER_ARGS " in
    *" --max-requests "*) ;;
    *)
        if [ -n "$SERVER_MAX_REQUESTS" ]; then
            SERVER_ARGS="$SERVER_ARGS --max-requests $SERVER_MAX_REQUESTS"
        fi
        ;;
esac

if [[ "$SERVER_ARGS" != *"--num-clients"* ]]; then
    SERVER_ARGS="${SERVER_ARGS} --num-clients ${CLIENT_COUNT}"
fi

wrapper_log() {
    if [ "${DEBUG_LIVE:-0}" != "0" ]; then
        echo "[rpc-wrapper] $*"
    fi
}

wait_for_server_ready() {
    if [ "$SERVER_READY_TIMEOUT_SEC" -gt 0 ] && command -v timeout >/dev/null 2>&1; then
        timeout --signal=TERM --kill-after=1 "${SERVER_READY_TIMEOUT_SEC}s" \
            grep -F -m1 -q -- "$SERVER_READY_MARKER" \
            < <(tail -n +1 -F --pid="$SERVER_PID" "$SERVER_LOG" 2>/dev/null)
        return $?
    fi

    grep -F -m1 -q -- "$SERVER_READY_MARKER" \
        < <(tail -n +1 -F --pid="$SERVER_PID" "$SERVER_LOG" 2>/dev/null)
}

emit_tick_lines() {
    local file="$1"
    local prefix="$2"

    if [ ! -f "$file" ]; then
        return 0
    fi

    sed -nE "s/^req_([0-9]+)_(start|end|delta)_tick=([0-9]+)$/${prefix}req_\\1_\\2_tick=\\3/p" "$file"
}

emit_server_timing_lines() {
    local file="$1"

    if [ ! -f "$file" ]; then
        return 0
    fi

    sed -nE "s/^(server_req_[0-9]+_(node_id|rpc_id|poll_tick|exec_tick|resp_submit_tick)=[0-9]+)$/\\1/p" "$file"
}

: > "$SERVER_LOG"
wrapper_log "launch_server clients=${CLIENT_COUNT} pin=${PIN_CORES} core=${SERVER_CORE} log=${SERVER_LOG}"
if [ "$PIN_CORES" != "0" ]; then
    if [ "$DEBUG_LIVE" != "0" ]; then
        # shellcheck disable=SC2086
        CXL_RPC_NUMA_NODE="$SERVER_NUMA_NODE" taskset -c "$SERVER_CORE" \
            "$SERVER" $SERVER_ARGS > >(tee "$SERVER_LOG") 2>&1 &
    else
        # shellcheck disable=SC2086
        CXL_RPC_NUMA_NODE="$SERVER_NUMA_NODE" taskset -c "$SERVER_CORE" \
            "$SERVER" $SERVER_ARGS >"$SERVER_LOG" 2>&1 &
    fi
else
    if [ "$DEBUG_LIVE" != "0" ]; then
        # shellcheck disable=SC2086
        CXL_RPC_NUMA_NODE="$SERVER_NUMA_NODE" \
            "$SERVER" $SERVER_ARGS > >(tee "$SERVER_LOG") 2>&1 &
    else
        # shellcheck disable=SC2086
        CXL_RPC_NUMA_NODE="$SERVER_NUMA_NODE" "$SERVER" $SERVER_ARGS >"$SERVER_LOG" 2>&1 &
    fi
fi
SERVER_PID=$!
wrapper_log "server_pid=${SERVER_PID}"

if ! wait_for_server_ready; then
    echo "server_ready_timeout" >&2
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
    fi
    set +e
    wait "$SERVER_PID"
    SERVER_RC=$?
    set -e
    if [ "$SERVER_RC" -ne 0 ]; then
        exit "$SERVER_RC"
    fi
    exit 125
fi
wrapper_log "server_ready pid=${SERVER_PID}"

declare -a CLIENT_PIDS=()
declare -a CLIENT_LOGS=()
declare -a CLIENT_RCS=()

for ((i = 0; i < CLIENT_COUNT; i++)); do
    if [ "$CLIENT_COUNT" -eq 1 ]; then
        log="$CLIENT_LOG"
    else
        log="${CLIENT_LOG_PREFIX}_${i}.log"
    fi
    CLIENT_LOGS[$i]="$log"
    : > "$log"
    client_core=$((CLIENT_CORE_BASE + i))

    client_cmd=("$CLIENT" "$@")
    if [ "$CLIENT_COUNT" -gt 1 ]; then
        client_cmd+=("--num-clients" "$CLIENT_COUNT" "--node-id" "$i")
    fi

    wrapper_log "launch_client idx=${i} pin=${PIN_CORES} core=${client_core} log=${log} barrier=${FIRST_REQ_BARRIER_PATH}"

    if [ "$CLIENT_TIMEOUT_SEC" -gt 0 ]; then
        if [ "$PIN_CORES" != "0" ]; then
            if [ "$DEBUG_LIVE" != "0" ]; then
                CXL_RPC_NUMA_NODE="$CLIENT_NUMA_NODE" \
                    CXL_RPC_FIRST_REQ_BARRIER_PATH="$FIRST_REQ_BARRIER_PATH" \
                    timeout --signal=TERM --kill-after=2 \
                    "${CLIENT_TIMEOUT_SEC}s" \
                    taskset -c "$client_core" "${client_cmd[@]}" \
                    > >(tee "$log") 2>&1 &
            else
                CXL_RPC_NUMA_NODE="$CLIENT_NUMA_NODE" \
                    CXL_RPC_FIRST_REQ_BARRIER_PATH="$FIRST_REQ_BARRIER_PATH" \
                    timeout --signal=TERM --kill-after=2 \
                    "${CLIENT_TIMEOUT_SEC}s" \
                    taskset -c "$client_core" "${client_cmd[@]}" >"$log" 2>&1 &
            fi
        else
            if [ "$DEBUG_LIVE" != "0" ]; then
                CXL_RPC_NUMA_NODE="$CLIENT_NUMA_NODE" \
                    CXL_RPC_FIRST_REQ_BARRIER_PATH="$FIRST_REQ_BARRIER_PATH" \
                    timeout --signal=TERM --kill-after=2 \
                    "${CLIENT_TIMEOUT_SEC}s" \
                    "${client_cmd[@]}" > >(tee "$log") 2>&1 &
            else
                CXL_RPC_NUMA_NODE="$CLIENT_NUMA_NODE" \
                    CXL_RPC_FIRST_REQ_BARRIER_PATH="$FIRST_REQ_BARRIER_PATH" \
                    timeout --signal=TERM --kill-after=2 \
                    "${CLIENT_TIMEOUT_SEC}s" \
                    "${client_cmd[@]}" >"$log" 2>&1 &
            fi
        fi
    else
        if [ "$PIN_CORES" != "0" ]; then
            if [ "$DEBUG_LIVE" != "0" ]; then
                CXL_RPC_NUMA_NODE="$CLIENT_NUMA_NODE" \
                    CXL_RPC_FIRST_REQ_BARRIER_PATH="$FIRST_REQ_BARRIER_PATH" \
                    taskset -c "$client_core" "${client_cmd[@]}" \
                    > >(tee "$log") 2>&1 &
            else
                CXL_RPC_NUMA_NODE="$CLIENT_NUMA_NODE" \
                    CXL_RPC_FIRST_REQ_BARRIER_PATH="$FIRST_REQ_BARRIER_PATH" \
                    taskset -c "$client_core" "${client_cmd[@]}" >"$log" 2>&1 &
            fi
        else
            if [ "$DEBUG_LIVE" != "0" ]; then
                CXL_RPC_NUMA_NODE="$CLIENT_NUMA_NODE" \
                    CXL_RPC_FIRST_REQ_BARRIER_PATH="$FIRST_REQ_BARRIER_PATH" \
                    "${client_cmd[@]}" > >(tee "$log") 2>&1 &
            else
                CXL_RPC_NUMA_NODE="$CLIENT_NUMA_NODE" \
                    CXL_RPC_FIRST_REQ_BARRIER_PATH="$FIRST_REQ_BARRIER_PATH" \
                    "${client_cmd[@]}" >"$log" 2>&1 &
            fi
        fi
    fi
    CLIENT_PIDS[$i]=$!
    wrapper_log "client_pid idx=${i} pid=${CLIENT_PIDS[$i]}"
done

overall_rc=0
for ((i = 0; i < CLIENT_COUNT; i++)); do
    set +e
    wait "${CLIENT_PIDS[$i]}"
    CLIENT_RCS[$i]=$?
    set -e
    wrapper_log "client_done idx=${i} rc=${CLIENT_RCS[$i]}"
    if [ "${CLIENT_RCS[$i]}" -ne 0 ] && [ "$overall_rc" -eq 0 ]; then
        overall_rc="${CLIENT_RCS[$i]}"
    fi
done

if [ "$overall_rc" -ne 0 ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -TERM "$SERVER_PID" 2>/dev/null || true
fi

set +e
wait "$SERVER_PID"
SERVER_RC=$?
set -e
wrapper_log "server_done rc=${SERVER_RC}"

if [ "$SERVER_RC" -ne 0 ] && [ "$overall_rc" -eq 0 ]; then
    overall_rc="$SERVER_RC"
fi

if [ "$overall_rc" -eq 0 ] && [ "$SERVER_RC" -eq 0 ]; then
    emit_server_timing_lines "$SERVER_LOG"
    for ((i = 0; i < CLIENT_COUNT; i++)); do
        if [ "$CLIENT_COUNT" -eq 1 ]; then
            emit_tick_lines "${CLIENT_LOGS[$i]}" ""
        else
            emit_tick_lines "${CLIENT_LOGS[$i]}" "[CLIENT[$i]] "
        fi
    done
else
    echo "rpc_test_failed rc=${overall_rc} server_rc=${SERVER_RC}" >&2
fi
rm -f "$FIRST_REQ_BARRIER_PATH" 2>/dev/null || true
exit "$overall_rc"
UNIFIED_HELPER_SCRIPT
sudo chmod +x "${MOUNT_POINT}${DEST_DIR}/run_rpc_server_clients.sh"
echo "  Created: run_rpc_server_clients.sh (unified helper)"

# Backward-compatible single-client entry.
sudo tee "${MOUNT_POINT}${DEST_DIR}/run_rpc_server_client.sh" > /dev/null << 'SINGLE_WRAPPER_SCRIPT'
#!/bin/bash
set -u
export CXL_RPC_CLIENT_COUNT="${CXL_RPC_CLIENT_COUNT:-1}"
exec /home/test_code/run_rpc_server_clients.sh "$@"
SINGLE_WRAPPER_SCRIPT
sudo chmod +x "${MOUNT_POINT}${DEST_DIR}/run_rpc_server_client.sh"
echo "  Created: run_rpc_server_client.sh (compat wrapper)"

# Backward-compatible multi-client entry.
sudo tee "${MOUNT_POINT}${DEST_DIR}/run_rpc_server_multi_client.sh" > /dev/null << 'MULTI_WRAPPER_SCRIPT'
#!/bin/bash
set -u
export CXL_RPC_CLIENT_COUNT="${CXL_RPC_CLIENT_COUNT:-4}"
exec /home/test_code/run_rpc_server_clients.sh "$@"
MULTI_WRAPPER_SCRIPT
sudo chmod +x "${MOUNT_POINT}${DEST_DIR}/run_rpc_server_multi_client.sh"
echo "  Created: run_rpc_server_multi_client.sh (compat wrapper)"

# Install m5 binary and gem5 one-shot readfile bootstrap service
echo ""
echo "[4/5] Installing m5 binary and gem5 readfile bootstrap..."

# Install m5 binary
if [ -f "$M5_BIN" ]; then
    sudo cp "$M5_BIN" "${MOUNT_POINT}/sbin/m5"
    sudo chmod 755 "${MOUNT_POINT}/sbin/m5"
    echo "  Installed: /sbin/m5"
else
    echo "  WARNING: m5 binary not found at $M5_BIN"
    echo "  Build with: cd $SIMCXL_DIR/util/m5 && scons build/x86/out/m5"
fi

# Install a one-shot readfile bootstrapper. On normal boot it fetches the
# initial payload once; on checkpoint save/restore, the bootstrap payload
# itself resumes after restore and performs exactly one follow-up readfile.
sudo mkdir -p "${MOUNT_POINT}/usr/local/sbin"
sudo rm -f "${MOUNT_POINT}/usr/local/sbin/gem5-readfile-runner.sh"
sudo tee "${MOUNT_POINT}/usr/local/sbin/gem5-readfile-once.sh" > /dev/null << 'RUNNER_EOF'
#!/bin/bash
set -u

SCRIPT_FILE=/tmp/gem5_script.sh
TMP_FILE=/tmp/gem5_script.sh.new

/sbin/m5 readfile > "$TMP_FILE" 2>/dev/null || true

if [ ! -s "$TMP_FILE" ]; then
    rm -f "$TMP_FILE"
    exit 0
fi

mv -f "$TMP_FILE" "$SCRIPT_FILE"
chmod +x "$SCRIPT_FILE"
nohup /bin/bash "$SCRIPT_FILE" >/dev/ttyS0 2>&1 </dev/null &
exit 0
RUNNER_EOF
sudo chmod 755 "${MOUNT_POINT}/usr/local/sbin/gem5-readfile-once.sh"

# Create systemd service for one-shot boot-time m5 readfile execution.
sudo tee "${MOUNT_POINT}/etc/systemd/system/gem5-readfile.service" > /dev/null << 'SVC_EOF'
[Unit]
Description=gem5 m5 readfile bootstrap
After=local-fs.target

[Service]
Type=simple
ExecStart=/usr/local/sbin/gem5-readfile-once.sh
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=multi-user.target
SVC_EOF

# Enable the service
sudo mkdir -p "${MOUNT_POINT}/etc/systemd/system/multi-user.target.wants"
sudo ln -sf /etc/systemd/system/gem5-readfile.service \
    "${MOUNT_POINT}/etc/systemd/system/multi-user.target.wants/gem5-readfile.service"
echo "  Installed: gem5-readfile.service (one-shot bootstrap)"

echo ""
echo "=== Setup Complete ==="
echo "Copied: $copied binaries, Skipped: $skipped"
if [ "$CLEAN_TEST_CODE_DIR" != "0" ]; then
    echo "Removed non-whitelist files: $removed_non_whitelist"
fi
echo "Destination: ${DEST_DIR}/ inside $DISK_IMAGE"
echo "gem5 integration: /sbin/m5 + gem5-readfile.service (one-shot) installed"
