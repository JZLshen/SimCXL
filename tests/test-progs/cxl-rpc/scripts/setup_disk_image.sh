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

# Optional hygiene mode: remove unrelated files under /home/test_code before
# copying the current whitelist binaries.
CLEAN_TEST_CODE_DIR="${CXL_RPC_CLEAN_TEST_CODE_DIR:-0}"

usage() {
    echo "Usage: $0 DISK_IMAGE_PATH"
    echo ""
    echo "Copy CXL RPC test binaries to gem5 disk image."
    echo "Requires sudo for mounting."
    echo "Set CXL_RPC_INCLUDE_COPY_CMP=1 to also copy cxl_mem_copy_cmp."
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
    rmdir "$MOUNT_POINT" 2>/dev/null || true
}
trap cleanup EXIT

# Try direct mount first, then with partition offset
if sudo mount -o loop "$DISK_IMAGE" "$MOUNT_POINT" 2>/dev/null; then
    echo "  Mounted directly"
else
    OFFSET=$(sudo fdisk -l "$DISK_IMAGE" 2>/dev/null | \
             awk '/Linux/ {for(i=2;i<=NF;i++){if($i~/^[0-9]+$/){print $i*512; exit}}}')
    if [ -n "$OFFSET" ] && [ "$OFFSET" -gt 0 ]; then
        sudo mount -o loop,offset="$OFFSET" "$DISK_IMAGE" "$MOUNT_POINT"
        echo "  Mounted with offset=$OFFSET"
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

emit_prefixed_file() {
    local file="$1"
    local prefix="$2"

    if [ -f "$file" ]; then
        sed -u "s/^/${prefix}/" "$file"
    fi
}

if [ "$CLIENT_COUNT" -eq 1 ]; then
    echo "=== CXL RPC Server-Client Test ==="
else
    echo "=== CXL RPC Multi-Client Test ==="
fi
echo "Server: $SERVER"
echo "Client: $CLIENT"
echo "Client count: $CLIENT_COUNT"
echo "Client timeout sec: $CLIENT_TIMEOUT_SEC"
echo "Server CXL map node: $SERVER_NUMA_NODE"
echo "Client CXL map node: $CLIENT_NUMA_NODE"
echo "Server log: $SERVER_LOG"
if [ "$CLIENT_COUNT" -eq 1 ]; then
    echo "Client log: $CLIENT_LOG"
else
    echo "Client log prefix: $CLIENT_LOG_PREFIX"
fi
echo "Server args: $SERVER_ARGS"
if [ "$#" -gt 0 ]; then
    echo "Client base args: $*"
fi
echo ""

echo "Starting server..."
: > "$SERVER_LOG"
# shellcheck disable=SC2086
CXL_RPC_NUMA_NODE="$SERVER_NUMA_NODE" "$SERVER" $SERVER_ARGS >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

echo "Waiting for server ready marker: $SERVER_READY_MARKER"
if ! wait_for_server_ready; then
    echo "[error] server did not report ready marker before timeout/exit"
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
    fi
    set +e
    wait "$SERVER_PID"
    SERVER_RC=$?
    set -e
    emit_prefixed_file "$SERVER_LOG" "[SERVER] "
    echo "Server exit code: $SERVER_RC"
    if [ "$SERVER_RC" -ne 0 ]; then
        exit "$SERVER_RC"
    fi
    exit 125
fi

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
    echo "Starting client[$i]..."

    client_cmd=("$CLIENT" "$@")
    if [ "$CLIENT_COUNT" -gt 1 ]; then
        client_cmd+=("--num-clients" "$CLIENT_COUNT" "--client-id" "$i")
    fi

    if [ "$CLIENT_TIMEOUT_SEC" -gt 0 ]; then
        CXL_RPC_NUMA_NODE="$CLIENT_NUMA_NODE" timeout --signal=TERM --kill-after=2 \
            "${CLIENT_TIMEOUT_SEC}s" \
            "${client_cmd[@]}" >"$log" 2>&1 &
    else
        CXL_RPC_NUMA_NODE="$CLIENT_NUMA_NODE" \
            "${client_cmd[@]}" >"$log" 2>&1 &
    fi
    CLIENT_PIDS[$i]=$!
done

overall_rc=0
for ((i = 0; i < CLIENT_COUNT; i++)); do
    set +e
    wait "${CLIENT_PIDS[$i]}"
    CLIENT_RCS[$i]=$?
    set -e
    if [ "${CLIENT_RCS[$i]}" -eq 124 ]; then
        echo "Client[$i] timed out after ${CLIENT_TIMEOUT_SEC}s"
    fi
    if [ "${CLIENT_RCS[$i]}" -ne 0 ] && [ "$overall_rc" -eq 0 ]; then
        overall_rc="${CLIENT_RCS[$i]}"
    fi
done

if [ "$overall_rc" -ne 0 ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "[cleanup] terminating server after client failure"
    kill -TERM "$SERVER_PID" 2>/dev/null || true
fi

set +e
wait "$SERVER_PID"
SERVER_RC=$?
set -e

if [ "$SERVER_RC" -ne 0 ] && [ "$overall_rc" -eq 0 ]; then
    overall_rc="$SERVER_RC"
fi

emit_prefixed_file "$SERVER_LOG" "[SERVER] "
for ((i = 0; i < CLIENT_COUNT; i++)); do
    if [ "$CLIENT_COUNT" -eq 1 ]; then
        emit_prefixed_file "${CLIENT_LOGS[$i]}" "[CLIENT] "
    else
        emit_prefixed_file "${CLIENT_LOGS[$i]}" "[CLIENT[$i]] "
    fi
done

echo ""
if [ "$CLIENT_COUNT" -eq 1 ]; then
    echo "Client exit code: ${CLIENT_RCS[0]}"
else
    for ((i = 0; i < CLIENT_COUNT; i++)); do
        echo "Client[$i] exit code: ${CLIENT_RCS[$i]}"
    done
fi
echo "Server exit code: $SERVER_RC"
echo "Overall rc: $overall_rc"
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

echo "[gem5-readfile] fetching payload via m5 readfile"
/sbin/m5 readfile > "$TMP_FILE" 2>/dev/null || true

if [ ! -s "$TMP_FILE" ]; then
    rm -f "$TMP_FILE"
    echo "[gem5-readfile] no payload supplied"
    exit 0
fi

mv -f "$TMP_FILE" "$SCRIPT_FILE"
chmod +x "$SCRIPT_FILE"
PAYLOAD_HASH="$(sha256sum "$SCRIPT_FILE" | awk '{print $1}')"
echo "[gem5-readfile] launching payload hash=$PAYLOAD_HASH"
nohup /bin/bash "$SCRIPT_FILE" >/dev/ttyS0 2>&1 </dev/null &
PAYLOAD_PID=$!
echo "[gem5-readfile] payload pid=$PAYLOAD_PID"
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
