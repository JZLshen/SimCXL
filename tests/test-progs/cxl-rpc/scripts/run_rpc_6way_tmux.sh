#!/bin/bash
#
# Launch the current in-project experiment set across 6 machines.
#
# Usage on machine N (N in 1..6):
#   bash tests/test-progs/cxl-rpc/scripts/run_rpc_6way_tmux.sh --slot N
#
# Shard layout:
#   slot 1: bare 1-14
#   slot 2: bare 15-28
#   slot 3: bare 29-42
#   slot 4: bare 43-56
#   slot 5: bare 57-70
#   slot 6: bare 71-81, app 1-5
#
# The script defaults to:
#   - inject the current binaries into files/parsec.img once on this machine
#   - start one detached tmux session
#   - write launcher logs to output/launcher_logs/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
BARE_RUNNER="$REPO_ROOT/tests/test-progs/cxl-rpc/scripts/run_rpc_matrix_kvm_timing_ckpt.py"
APP_RUNNER="$REPO_ROOT/tests/test-progs/cxl-rpc/scripts/run_rpc_app_matrix_kvm_timing_ckpt.py"
INJECT_SCRIPT="$REPO_ROOT/tests/test-progs/cxl-rpc/scripts/setup_disk_image.sh"

SLOT=""
SESSION_PREFIX="rpc6"
BATCH_TAG="$(date +%Y%m%d_%H%M%S)"
OUTPUT_BASE="output"
DISK="$REPO_ROOT/files/parsec.img"
COPY_ENGINE_CHANNELS=0
CHECKPOINT_HANDOFF_DEADLINE_SIM_SECONDS=0
INTER_EXPERIMENT_SLEEP_SEC=0
BOOT_CPU_TYPE="KVM"
SKIP_INJECT=0
FORCE_RERUN=0
DRY_RUN=0
FOREGROUND=0

usage() {
    cat <<EOF
Usage:
  bash tests/test-progs/cxl-rpc/scripts/run_rpc_6way_tmux.sh --slot N [options]

Required:
  --slot N                               Machine slot in 1..6

Options:
  --session-prefix NAME                  tmux session prefix (default: ${SESSION_PREFIX})
  --batch-tag TAG                        Batch suffix tag (default: current time)
  --output-base PATH                     Output base path (default: ${OUTPUT_BASE})
  --disk PATH                            Guest disk image (default: ${DISK})
  --copy-engine-channels N               Pass-through to both runners
  --checkpoint-handoff-deadline-sim-seconds N
                                         Pass-through to both runners
  --boot-cpu-type TYPE                   KVM, TIMING, or ATOMIC (default: ${BOOT_CPU_TYPE})
  --inter-experiment-sleep-sec N         Pass-through to both runners (default: ${INTER_EXPERIMENT_SLEEP_SEC})
  --skip-inject                          Reuse the current disk image contents
  --force-rerun                          Pass-through to both runners
  --dry-run                              Pass-through to both runners
  --foreground                           Internal: run in the current shell instead of tmux
  -h, --help                             Show this help

Examples:
  bash tests/test-progs/cxl-rpc/scripts/run_rpc_6way_tmux.sh --slot 1
  bash tests/test-progs/cxl-rpc/scripts/run_rpc_6way_tmux.sh --slot 6 --skip-inject
EOF
}

die() {
    echo "[fatal] $*" >&2
    exit 1
}

quote_cmd() {
    local out=""
    local arg
    for arg in "$@"; do
        printf -v out '%s%q ' "$out" "$arg"
    done
    printf '%s' "${out% }"
}

resolve_output_base() {
    if [[ "$OUTPUT_BASE" = /* ]]; then
        printf '%s\n' "$OUTPUT_BASE"
    else
        printf '%s\n' "$REPO_ROOT/$OUTPUT_BASE"
    fi
}

sanitize_token() {
    printf '%s' "$1" | tr -c '[:alnum:]_-' '_'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --slot)
            [[ $# -ge 2 ]] || die "--slot requires a value"
            SLOT="$2"
            shift 2
            ;;
        --session-prefix)
            [[ $# -ge 2 ]] || die "--session-prefix requires a value"
            SESSION_PREFIX="$2"
            shift 2
            ;;
        --batch-tag)
            [[ $# -ge 2 ]] || die "--batch-tag requires a value"
            BATCH_TAG="$2"
            shift 2
            ;;
        --output-base)
            [[ $# -ge 2 ]] || die "--output-base requires a value"
            OUTPUT_BASE="$2"
            shift 2
            ;;
        --disk)
            [[ $# -ge 2 ]] || die "--disk requires a value"
            DISK="$2"
            shift 2
            ;;
        --copy-engine-channels)
            [[ $# -ge 2 ]] || die "--copy-engine-channels requires a value"
            COPY_ENGINE_CHANNELS="$2"
            shift 2
            ;;
        --checkpoint-handoff-deadline-sim-seconds)
            [[ $# -ge 2 ]] || die "--checkpoint-handoff-deadline-sim-seconds requires a value"
            CHECKPOINT_HANDOFF_DEADLINE_SIM_SECONDS="$2"
            shift 2
            ;;
        --boot-cpu-type)
            [[ $# -ge 2 ]] || die "--boot-cpu-type requires a value"
            BOOT_CPU_TYPE="$2"
            shift 2
            ;;
        --inter-experiment-sleep-sec)
            [[ $# -ge 2 ]] || die "--inter-experiment-sleep-sec requires a value"
            INTER_EXPERIMENT_SLEEP_SEC="$2"
            shift 2
            ;;
        --skip-inject)
            SKIP_INJECT=1
            shift
            ;;
        --force-rerun)
            FORCE_RERUN=1
            shift
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        --foreground)
            FOREGROUND=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown argument: $1"
            ;;
    esac
done

[[ -n "$SLOT" ]] || die "--slot is required"
[[ "$SLOT" =~ ^[1-6]$ ]] || die "--slot must be in 1..6"
[[ "$COPY_ENGINE_CHANNELS" =~ ^[0-9]+$ ]] || die "--copy-engine-channels must be >= 0"
[[ "$CHECKPOINT_HANDOFF_DEADLINE_SIM_SECONDS" =~ ^[0-9]+$ ]] || \
    die "--checkpoint-handoff-deadline-sim-seconds must be >= 0"
[[ "$INTER_EXPERIMENT_SLEEP_SEC" =~ ^[0-9]+$ ]] || \
    die "--inter-experiment-sleep-sec must be >= 0"
[[ "$BOOT_CPU_TYPE" = "KVM" || "$BOOT_CPU_TYPE" = "TIMING" || "$BOOT_CPU_TYPE" = "ATOMIC" ]] || \
    die "--boot-cpu-type must be KVM, TIMING, or ATOMIC"
[[ -f "$BARE_RUNNER" ]] || die "bare runner not found: $BARE_RUNNER"
[[ -f "$APP_RUNNER" ]] || die "app runner not found: $APP_RUNNER"
[[ -f "$INJECT_SCRIPT" ]] || die "inject script not found: $INJECT_SCRIPT"
[[ -f "$DISK" ]] || die "disk image not found: $DISK"

case "$SLOT" in
    1) BARE_START=1;  BARE_END=14; APP_START=0; APP_END=0 ;;
    2) BARE_START=15; BARE_END=28; APP_START=0; APP_END=0 ;;
    3) BARE_START=29; BARE_END=42; APP_START=0; APP_END=0 ;;
    4) BARE_START=43; BARE_END=56; APP_START=0; APP_END=0 ;;
    5) BARE_START=57; BARE_END=70; APP_START=0; APP_END=0 ;;
    6) BARE_START=71; BARE_END=81; APP_START=1; APP_END=5 ;;
esac

HOST_SAFE="$(sanitize_token "$(hostname -s 2>/dev/null || hostname)")"
TAG_SAFE="$(sanitize_token "$BATCH_TAG")"
SESSION_NAME="${SESSION_PREFIX}_${HOST_SAFE}_s${SLOT}_${TAG_SAFE}"
OUTPUT_BASE_ABS="$(resolve_output_base)"
LOG_DIR="$OUTPUT_BASE_ABS/launcher_logs"
LOG_PATH="$LOG_DIR/${SESSION_NAME}.log"
BARE_BATCH="bare_${HOST_SAFE}_slot${SLOT}_${TAG_SAFE}"
APP_BATCH="app_${HOST_SAFE}_slot${SLOT}_${TAG_SAFE}"

PRIV_CMD=()
if [[ "$(id -u)" -eq 0 ]]; then
    echo "[launcher] running as root; skip sudo"
else
    command -v sudo >/dev/null 2>&1 || die "sudo is required when not running as root"
    sudo -n true >/dev/null 2>&1 || \
        die "sudo -n failed; configure passwordless sudo or pre-authenticate before using tmux mode"
    PRIV_CMD=(sudo -n)
fi

if [[ "$FOREGROUND" != "1" ]]; then
    command -v tmux >/dev/null 2>&1 || die "tmux is not installed"
    mkdir -p "$LOG_DIR"
    tmux has-session -t "$SESSION_NAME" 2>/dev/null && \
        die "tmux session already exists: $SESSION_NAME"

    child_cmd=(
        bash "$SCRIPT_DIR/run_rpc_6way_tmux.sh"
        --foreground
        --slot "$SLOT"
        --session-prefix "$SESSION_PREFIX"
        --batch-tag "$BATCH_TAG"
        --output-base "$OUTPUT_BASE"
        --disk "$DISK"
        --copy-engine-channels "$COPY_ENGINE_CHANNELS"
        --checkpoint-handoff-deadline-sim-seconds
        "$CHECKPOINT_HANDOFF_DEADLINE_SIM_SECONDS"
        --boot-cpu-type "$BOOT_CPU_TYPE"
        --inter-experiment-sleep-sec "$INTER_EXPERIMENT_SLEEP_SEC"
    )
    if [[ "$SKIP_INJECT" = "1" ]]; then
        child_cmd+=(--skip-inject)
    fi
    if [[ "$FORCE_RERUN" = "1" ]]; then
        child_cmd+=(--force-rerun)
    fi
    if [[ "$DRY_RUN" = "1" ]]; then
        child_cmd+=(--dry-run)
    fi

    tmux_cmd="$(quote_cmd "${child_cmd[@]}")"
    cd_cmd="$(quote_cmd cd "$REPO_ROOT")"
    tmux new-session -d -s "$SESSION_NAME" \
        "$(printf '%s && %s' "$cd_cmd" "$tmux_cmd")"

    echo "[launcher] started tmux session: $SESSION_NAME"
    echo "[launcher] slot=$SLOT bare=${BARE_START}-${BARE_END} app=${APP_START}-${APP_END}"
    echo "[launcher] log: $LOG_PATH"
    echo "[launcher] attach: tmux attach -t $SESSION_NAME"
    echo "[launcher] kill:   tmux kill-session -t $SESSION_NAME"
    exit 0
fi

mkdir -p "$LOG_DIR"
exec > >(tee -a "$LOG_PATH") 2>&1

echo "[launcher] session=$SESSION_NAME"
echo "[launcher] repo_root=$REPO_ROOT"
echo "[launcher] output_base=$OUTPUT_BASE_ABS"
echo "[launcher] disk=$DISK"
echo "[launcher] boot_cpu_type=$BOOT_CPU_TYPE"
echo "[launcher] slot=$SLOT bare=${BARE_START}-${BARE_END} app=${APP_START}-${APP_END}"
echo "[launcher] log=$LOG_PATH"
echo "[launcher] started_at=$(date --iso-8601=seconds)"

if [[ "$SKIP_INJECT" != "1" ]]; then
    inject_cmd=(
        "${PRIV_CMD[@]}" env
        TMPDIR=/dev/shm
        CXL_RPC_CLEAN_TEST_CODE_DIR=1
        bash "$INJECT_SCRIPT" "$DISK"
    )
    if [[ "$DRY_RUN" = "1" ]]; then
        echo "+ $(quote_cmd "${inject_cmd[@]}")"
    else
        echo "+ $(quote_cmd "${inject_cmd[@]}")"
        "${inject_cmd[@]}"
    fi
else
    echo "[launcher] skip disk injection"
fi

bare_cmd=(
    "${PRIV_CMD[@]}" python3 "$BARE_RUNNER"
    --repo-root "$REPO_ROOT"
    --output-base "$OUTPUT_BASE"
    --batch-name "$BARE_BATCH"
    --disk "$DISK"
    --boot-cpu-type "$BOOT_CPU_TYPE"
    --start-index "$BARE_START"
    --end-index "$BARE_END"
    --skip-inject
    --inter-experiment-sleep-sec "$INTER_EXPERIMENT_SLEEP_SEC"
)
if [[ "$COPY_ENGINE_CHANNELS" -gt 0 ]]; then
    bare_cmd+=(--copy-engine-channels "$COPY_ENGINE_CHANNELS")
fi
if [[ "$CHECKPOINT_HANDOFF_DEADLINE_SIM_SECONDS" -gt 0 ]]; then
    bare_cmd+=(--checkpoint-handoff-deadline-sim-seconds
               "$CHECKPOINT_HANDOFF_DEADLINE_SIM_SECONDS")
fi
if [[ "$FORCE_RERUN" = "1" ]]; then
    bare_cmd+=(--force-rerun)
fi
if [[ "$DRY_RUN" = "1" ]]; then
    bare_cmd+=(--dry-run)
fi

echo "+ $(quote_cmd "${bare_cmd[@]}")"
"${bare_cmd[@]}"

if [[ "$APP_START" -gt 0 ]]; then
    app_cmd=(
        "${PRIV_CMD[@]}" python3 "$APP_RUNNER"
        --repo-root "$REPO_ROOT"
        --output-base "$OUTPUT_BASE"
        --batch-name "$APP_BATCH"
        --disk "$DISK"
        --boot-cpu-type "$BOOT_CPU_TYPE"
        --start-index "$APP_START"
        --end-index "$APP_END"
        --skip-inject
        --inter-experiment-sleep-sec "$INTER_EXPERIMENT_SLEEP_SEC"
    )
    if [[ "$COPY_ENGINE_CHANNELS" -gt 0 ]]; then
        app_cmd+=(--copy-engine-channels "$COPY_ENGINE_CHANNELS")
    fi
    if [[ "$CHECKPOINT_HANDOFF_DEADLINE_SIM_SECONDS" -gt 0 ]]; then
        app_cmd+=(--checkpoint-handoff-deadline-sim-seconds
                  "$CHECKPOINT_HANDOFF_DEADLINE_SIM_SECONDS")
    fi
    if [[ "$FORCE_RERUN" = "1" ]]; then
        app_cmd+=(--force-rerun)
    fi
    if [[ "$DRY_RUN" = "1" ]]; then
        app_cmd+=(--dry-run)
    fi

    echo "+ $(quote_cmd "${app_cmd[@]}")"
    "${app_cmd[@]}"
fi

echo "[launcher] completed_at=$(date --iso-8601=seconds)"
