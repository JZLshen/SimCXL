#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  bash tests/test-progs/cxl-rpc/scripts/run_rpc_current_flat_queue.sh [options]

Options:
  --root-outdir <dir>      Root output directory.
                           Default: output/rpc_current_flat_queue_<timestamp>
  --checkpoint-dir <dir>   Existing checkpoint directory to reuse, or a
                           checkpoint root containing topology subdirs.
  --max-procs <N>          Concurrent gem5 jobs for the SimCXL current set.
                           Default: 12
  --disk <path>            Guest disk image. Default: repo-local files/parsec.img
  --skip-build             Reuse existing gem5 binary and injected guest binaries.
  --skip-inject            Reuse the current disk image contents.
  --skip-bare              Skip non-application current experiments.
  --skip-app               Skip application-path current experiments.
  --force-rerun            Forward to per-experiment runners.
  --continue-on-failure    Record failing experiment wrappers and keep going.
  --copy-engine-channels N Pass-through to checkpoint build and bare/app runners.
  --checkpoint-handoff-deadline-sim-seconds N
                           Pass-through to checkpoint generation and runners.
  --help                   Show this message.

Current SimCXL set:
  - unified current scope from run_rpc_matrix_kvm_timing_ckpt.py:
      overall + application + sensitivity + technical-analysis points
      DMA-lane sensitivity excluded

Default total tasks from this launcher follow the unified matrix plan.
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
BARE_RUNNER="$REPO_ROOT/tests/test-progs/cxl-rpc/scripts/run_rpc_matrix_kvm_timing_ckpt.py"
INJECT_SCRIPT="$REPO_ROOT/tests/test-progs/cxl-rpc/scripts/setup_disk_image.sh"
GEM5_BIN="$REPO_ROOT/build/X86/gem5.opt"
SAVE_CKPT_CFG="$REPO_ROOT/configs/example/gem5_library/x86-cxl-rpc-save-checkpoint.py"

ROOT_OUTDIR=""
CHECKPOINT_DIR=""
CHECKPOINT_ROOT=""
MAX_PROCS=12
DISK="$REPO_ROOT/files/parsec.img"
SKIP_BUILD=0
SKIP_INJECT=0
SKIP_BARE=0
SKIP_APP=0
FORCE_RERUN=0
CONTINUE_ON_FAILURE=0
COPY_ENGINE_CHANNELS=0
CHECKPOINT_HANDOFF_DEADLINE_SIM_SECONDS=0
ANY_FAILURES=0
DEFAULT_TOPOLOGY_LABEL="clients_32_lanes_32_mq_1024_cxlp_0ns"
DEFAULT_TOPOLOGY_CLIENTS=32
DEFAULT_TOPOLOGY_LANES=32
DEFAULT_TOPOLOGY_MQ=1024
DEFAULT_TOPOLOGY_CXLP=0

declare -A CHECKPOINT_BY_LABEL=()
declare -A CHECKPOINT_FAILED_BY_LABEL=()
declare -A TOPOLOGY_SEEN=()
declare -a TOPOLOGY_ROWS=()

usage_die() {
    echo "[fatal] $*" >&2
    usage >&2
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

resolve_checkpoint_dir() {
    local path="$1"
    local found=""

    if [[ -f "$path/m5.cpt" ]]; then
        printf '%s\n' "$path"
        return 0
    fi
    if [[ -f "$path" && "$(basename "$path")" == "m5.cpt" ]]; then
        dirname "$path"
        return 0
    fi
    if [[ -d "$path" ]]; then
        found="$(find "$path" -name m5.cpt -print | sort | tail -n 1 || true)"
        if [[ -n "$found" ]]; then
            dirname "$found"
            return 0
        fi
    fi
    return 1
}

run_cmd() {
    printf '[%s] + %s\n' "$(date '+%F %T')" "$(quote_cmd "$@")"
    "$@"
}

wait_for_background_or_fail() {
    local wait_rc=0

    set +e
    wait -n
    wait_rc=$?
    set -e

    if [[ "$wait_rc" -ne 0 ]]; then
        ANY_FAILURES=1
        if [[ "$CONTINUE_ON_FAILURE" -eq 1 ]]; then
            printf '[%s] CONTINUE-FAIL background wrapper rc=%s\n' \
                "$(date '+%F %T')" "$wait_rc"
            return 0
        fi
        jobs -pr | xargs -r kill 2>/dev/null || true
        wait || true
        exit "$wait_rc"
    fi
}

wait_for_cap() {
    local cap="$1"
    while [[ "$(jobs -pr | wc -l)" -ge "$cap" ]]; do
        wait_for_background_or_fail
    done
}

wait_for_all_background() {
    while [[ "$(jobs -pr | wc -l)" -gt 0 ]]; do
        wait_for_background_or_fail
    done
}

topology_label() {
    local clients="$1"
    local lanes="$2"
    local mq_entries="$3"
    local cxl_extra_latency_ns="$4"
    printf 'clients_%s_lanes_%s_mq_%s_cxlp_%sns\n' \
        "$clients" "$lanes" "$mq_entries" "$cxl_extra_latency_ns"
}

base_topology_label() {
    local clients="$1"
    local lanes="$2"
    topology_label "$clients" "$lanes" \
        "$DEFAULT_TOPOLOGY_MQ" "$DEFAULT_TOPOLOGY_CXLP"
}

build_topology_checkpoint() {
    local clients="$1"
    local lanes="$2"
    local mq_entries="$3"
    local cxl_extra_latency_ns="$4"
    local preferred_checkpoint="${5:-}"
    local label=""
    local outdir=""
    local resolved=""
    local num_cpus=0
    local -a ckpt_cmd=()

    label="$(topology_label "$clients" "$lanes" "$mq_entries" "$cxl_extra_latency_ns")"
    if [[ -n "$preferred_checkpoint" ]]; then
        printf '[%s] reuse checkpoint %s -> %s\n' \
            "$(date '+%F %T')" "$label" "$preferred_checkpoint"
        return 0
    fi

    outdir="$ROOT_OUTDIR/checkpoints/$label"
    mkdir -p "$outdir"
    if resolved="$(resolve_checkpoint_dir "$outdir")"; then
        printf '[%s] reuse checkpoint %s -> %s\n' \
            "$(date '+%F %T')" "$label" "$resolved"
        return 0
    fi

    num_cpus=$((clients + 1))
    ckpt_cmd=(
        "${PRIV_CMD[@]}" "$GEM5_BIN"
        -d "$outdir"
        "$SAVE_CKPT_CFG"
        --disk "$DISK"
        --num_cpus "$num_cpus"
        --rpc_client_count "$clients"
        --rpc_response_lane_count "$lanes"
        --rpc_metadata_entries "$mq_entries"
        --cxl_extra_latency_ns "$cxl_extra_latency_ns"
    )
    if [[ "$COPY_ENGINE_CHANNELS" -gt 0 ]]; then
        ckpt_cmd+=(--copy_engine_channels "$COPY_ENGINE_CHANNELS")
    fi
    if [[ "$CHECKPOINT_HANDOFF_DEADLINE_SIM_SECONDS" -gt 0 ]]; then
        ckpt_cmd+=(--handoff_deadline_sim_seconds
                   "$CHECKPOINT_HANDOFF_DEADLINE_SIM_SECONDS")
    fi

    printf '[%s] build checkpoint %s\n' "$(date '+%F %T')" "$label"
    if ! run_cmd "${ckpt_cmd[@]}"; then
        : >"$outdir/.checkpoint_build_failed"
        return 1
    fi
    if ! resolve_checkpoint_dir "$outdir" >/dev/null; then
        : >"$outdir/.checkpoint_missing_after_build"
        return 1
    fi
    return 0
}

submit_checkpoint_build() {
    local label="$1"
    shift
    printf '[%s] submit checkpoint %s\n' "$(date '+%F %T')" "$label"
    build_topology_checkpoint "$@"
}

register_topology_row() {
    local clients="$1"
    local lanes="$2"
    local mq_entries="$3"
    local cxl_extra_latency_ns="$4"
    local preferred_checkpoint="${5:-}"
    local label=""
    local row=""

    label="$(topology_label "$clients" "$lanes" "$mq_entries" "$cxl_extra_latency_ns")"
    if [[ -n "${TOPOLOGY_SEEN[$label]:-}" ]]; then
        return 0
    fi
    TOPOLOGY_SEEN["$label"]=1
    row="$label"$'\t'"$clients"$'\t'"$lanes"$'\t'"$mq_entries"$'\t'"$cxl_extra_latency_ns"$'\t'"$preferred_checkpoint"
    TOPOLOGY_ROWS+=("$row")
}

emit_matrix_plan_tsv() {
    local plan_csv="$1"
    python3 - "$plan_csv" <<'PY'
import csv
import sys

with open(sys.argv[1], encoding="utf-8", newline="") as fp:
    for row in csv.DictReader(fp):
        print(
            "\t".join(
                [
                    row["exp_id"],
                    row["workload_kind"],
                    row["clients"],
                    row["response_lane_count"],
                    row["mq_entries"],
                    row["cxl_extra_latency_ns"],
                ]
            )
        )
PY
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --root-outdir)
            [[ $# -ge 2 ]] || usage_die "--root-outdir requires a value"
            ROOT_OUTDIR="$2"
            shift 2
            ;;
        --checkpoint-dir)
            [[ $# -ge 2 ]] || usage_die "--checkpoint-dir requires a value"
            CHECKPOINT_DIR="$2"
            shift 2
            ;;
        --max-procs)
            [[ $# -ge 2 ]] || usage_die "--max-procs requires a value"
            MAX_PROCS="$2"
            shift 2
            ;;
        --disk)
            [[ $# -ge 2 ]] || usage_die "--disk requires a value"
            DISK="$2"
            shift 2
            ;;
        --skip-build)
            SKIP_BUILD=1
            shift
            ;;
        --skip-inject)
            SKIP_INJECT=1
            shift
            ;;
        --skip-bare)
            SKIP_BARE=1
            shift
            ;;
        --skip-app)
            SKIP_APP=1
            shift
            ;;
        --force-rerun)
            FORCE_RERUN=1
            shift
            ;;
        --continue-on-failure)
            CONTINUE_ON_FAILURE=1
            shift
            ;;
        --copy-engine-channels)
            [[ $# -ge 2 ]] || usage_die "--copy-engine-channels requires a value"
            COPY_ENGINE_CHANNELS="$2"
            shift 2
            ;;
        --checkpoint-handoff-deadline-sim-seconds)
            [[ $# -ge 2 ]] || usage_die "--checkpoint-handoff-deadline-sim-seconds requires a value"
            CHECKPOINT_HANDOFF_DEADLINE_SIM_SECONDS="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            usage_die "unknown argument: $1"
            ;;
    esac
done

[[ "$MAX_PROCS" =~ ^[0-9]+$ ]] || usage_die "--max-procs must be >= 1"
[[ "$MAX_PROCS" -ge 1 ]] || usage_die "--max-procs must be >= 1"
[[ "$COPY_ENGINE_CHANNELS" =~ ^[0-9]+$ ]] || usage_die "--copy-engine-channels must be >= 0"
[[ "$CHECKPOINT_HANDOFF_DEADLINE_SIM_SECONDS" =~ ^[0-9]+$ ]] || \
    usage_die "--checkpoint-handoff-deadline-sim-seconds must be >= 0"
[[ -f "$BARE_RUNNER" ]] || usage_die "bare runner not found: $BARE_RUNNER"
[[ -f "$INJECT_SCRIPT" ]] || usage_die "inject script not found: $INJECT_SCRIPT"
[[ -f "$SAVE_CKPT_CFG" ]] || usage_die "checkpoint config not found: $SAVE_CKPT_CFG"
[[ -f "$DISK" ]] || usage_die "disk image not found: $DISK"
if [[ "$SKIP_BARE" -eq 1 && "$SKIP_APP" -eq 1 ]]; then
    usage_die "cannot skip both bare and app"
fi

if [[ -z "$ROOT_OUTDIR" ]]; then
    ROOT_OUTDIR="$REPO_ROOT/output/rpc_current_flat_queue_$(date +%Y%m%d_%H%M%S)"
fi
mkdir -p "$ROOT_OUTDIR"
ROOT_OUTDIR="$(cd "$ROOT_OUTDIR" && pwd)"
DISK="$(cd "$(dirname "$DISK")" && pwd)/$(basename "$DISK")"

PRIV_CMD=()
if [[ "$(id -u)" -ne 0 ]]; then
    command -v sudo >/dev/null 2>&1 || usage_die "sudo is required when not running as root"
    sudo -n true >/dev/null 2>&1 || usage_die "sudo -n failed; configure passwordless sudo or pre-authenticate"
    PRIV_CMD=(sudo -n)
fi

if [[ "$SKIP_BUILD" -eq 0 ]]; then
    run_cmd scons build/X86/gem5.opt -j"$(nproc)"
fi

if [[ "$SKIP_INJECT" -eq 0 ]]; then
    run_cmd "${PRIV_CMD[@]}" env TMPDIR=/dev/shm CXL_RPC_CLEAN_TEST_CODE_DIR=1 \
        bash "$INJECT_SCRIPT" "$DISK"
fi

if [[ -n "$CHECKPOINT_DIR" ]]; then
    if [[ -d "$CHECKPOINT_DIR" ]] && \
       find "$CHECKPOINT_DIR" -mindepth 1 -maxdepth 1 -type d -name 'clients_*' \
           | grep -q .; then
        CHECKPOINT_ROOT="$(cd "$CHECKPOINT_DIR" && pwd)"
        CHECKPOINT_DIR=""
    elif ! CHECKPOINT_DIR="$(resolve_checkpoint_dir "$CHECKPOINT_DIR")"; then
        usage_die "--checkpoint-dir does not resolve to a checkpoint or checkpoint root: $CHECKPOINT_DIR"
    fi
fi

PLAN_ROOT="$ROOT_OUTDIR/plans"
mkdir -p "$PLAN_ROOT"

run_cmd python3 "$BARE_RUNNER" \
    --repo-root "$REPO_ROOT" \
    --output-base "$PLAN_ROOT" \
    --batch-name plan_matrix \
    --skip-inject \
    --allow-concurrent-runs \
    --inter-experiment-sleep-sec 0 \
    --dry-run >/dev/null

readarray -t MATRIX_PLAN_ROWS < <(
    emit_matrix_plan_tsv "$PLAN_ROOT/plan_matrix/plan.csv"
)

DEFAULT_APP_CHECKPOINT=""
for row in "${MATRIX_PLAN_ROWS[@]}"; do
    [[ -n "$row" ]] || continue
    IFS=$'\t' read -r _exp_id workload_kind clients lanes mq_entries cxl_extra_latency_ns <<<"$row"
    if [[ "$workload_kind" == "application" && "$SKIP_APP" -eq 1 ]]; then
        continue
    fi
    if [[ "$workload_kind" != "application" && "$SKIP_BARE" -eq 1 ]]; then
        continue
    fi
    label="$(topology_label "$clients" "$lanes" "$mq_entries" "$cxl_extra_latency_ns")"
    preferred=""
    if [[ -n "$CHECKPOINT_ROOT" ]] && \
       resolved="$(resolve_checkpoint_dir "$CHECKPOINT_ROOT/$label" 2>/dev/null)"; then
        preferred="$resolved"
    elif [[ -n "$CHECKPOINT_ROOT" ]]; then
        base_label="$(base_topology_label "$clients" "$lanes")"
        if resolved="$(resolve_checkpoint_dir "$CHECKPOINT_ROOT/$base_label" 2>/dev/null)"; then
            preferred="$resolved"
        fi
    elif [[ "$clients" -eq "$DEFAULT_TOPOLOGY_CLIENTS" && \
          "$lanes" -eq "$DEFAULT_TOPOLOGY_LANES" && \
          "$mq_entries" -eq "$DEFAULT_TOPOLOGY_MQ" && \
          "$cxl_extra_latency_ns" -eq "$DEFAULT_TOPOLOGY_CXLP" && \
          -n "$CHECKPOINT_DIR" ]]; then
        preferred="$CHECKPOINT_DIR"
    fi
    register_topology_row \
        "$clients" "$lanes" "$mq_entries" "$cxl_extra_latency_ns" "$preferred"
done

for row in "${TOPOLOGY_ROWS[@]}"; do
    [[ -n "$row" ]] || continue
    IFS=$'\t' read -r label clients lanes mq_entries cxl_extra_latency_ns preferred <<<"$row"
    submit_checkpoint_build \
        "$label" \
        "$clients" "$lanes" "$mq_entries" "$cxl_extra_latency_ns" "$preferred"
done

for row in "${TOPOLOGY_ROWS[@]}"; do
    [[ -n "$row" ]] || continue
    IFS=$'\t' read -r label clients lanes mq_entries cxl_extra_latency_ns preferred <<<"$row"
    if [[ -n "$preferred" ]]; then
        CHECKPOINT_BY_LABEL["$label"]="$preferred"
        continue
    fi

    outdir="$ROOT_OUTDIR/checkpoints/$label"
    if resolved="$(resolve_checkpoint_dir "$outdir")"; then
        CHECKPOINT_BY_LABEL["$label"]="$resolved"
        continue
    fi

    if [[ -f "$outdir/.checkpoint_build_failed" ]]; then
        CHECKPOINT_FAILED_BY_LABEL["$label"]="checkpoint_build_failed"
    elif [[ -f "$outdir/.checkpoint_missing_after_build" ]]; then
        CHECKPOINT_FAILED_BY_LABEL["$label"]="checkpoint_missing_after_build"
    else
        CHECKPOINT_FAILED_BY_LABEL["$label"]="checkpoint_missing"
    fi

    ANY_FAILURES=1
    if [[ "$CONTINUE_ON_FAILURE" -eq 1 ]]; then
        printf '[%s] CONTINUE-FAIL checkpoint %s (%s)\n' \
            "$(date '+%F %T')" "$label" "${CHECKPOINT_FAILED_BY_LABEL[$label]}"
        continue
    fi
    usage_die "failed to prepare checkpoint for $label"
done

DEFAULT_APP_CHECKPOINT="${CHECKPOINT_BY_LABEL[$DEFAULT_TOPOLOGY_LABEL]:-}"
if [[ -z "$DEFAULT_APP_CHECKPOINT" ]]; then
    usage_die "failed to resolve default app checkpoint"
fi

submit_runner() {
    local kind="$1"
    local exp_id="$2"
    shift 2
    local cmd=("$@" --only-exp-id "$exp_id")

    printf '[%s] submit %s %s\n' "$(date '+%F %T')" "$kind" "$exp_id"
    "${cmd[@]}" &
    wait_for_cap "$MAX_PROCS"
}

echo "ROOT_OUTDIR=$ROOT_OUTDIR"
echo "DISK=$DISK"
echo "DEFAULT_APP_CHECKPOINT=$DEFAULT_APP_CHECKPOINT"
echo "MAX_PROCS=$MAX_PROCS"
bare_task_count=0
app_task_count=0
for row in "${MATRIX_PLAN_ROWS[@]}"; do
    [[ -n "$row" ]] || continue
    IFS=$'\t' read -r _exp_id workload_kind _clients _lanes _mq_entries _cxl_extra_latency_ns <<<"$row"
    if [[ "$workload_kind" == "application" ]]; then
        if [[ "$SKIP_APP" -eq 0 ]]; then
            app_task_count=$((app_task_count + 1))
        fi
    else
        if [[ "$SKIP_BARE" -eq 0 ]]; then
            bare_task_count=$((bare_task_count + 1))
        fi
    fi
done
echo "BARE_TASKS=$bare_task_count"
echo "APP_TASKS=$app_task_count"
echo "TOTAL_TASKS=$((bare_task_count + app_task_count))"

for row in "${MATRIX_PLAN_ROWS[@]}"; do
    [[ -n "$row" ]] || continue
    IFS=$'\t' read -r exp_id workload_kind clients lanes mq_entries cxl_extra_latency_ns <<<"$row"
    if [[ "$workload_kind" == "application" && "$SKIP_APP" -eq 1 ]]; then
        continue
    fi
    if [[ "$workload_kind" != "application" && "$SKIP_BARE" -eq 1 ]]; then
        continue
    fi
    label="$(topology_label "$clients" "$lanes" "$mq_entries" "$cxl_extra_latency_ns")"
    checkpoint_for_exp="${CHECKPOINT_BY_LABEL[$label]:-}"
    if [[ -z "$checkpoint_for_exp" ]]; then
        ANY_FAILURES=1
        printf '[%s] skip %s %s due to missing checkpoint for %s\n' \
            "$(date '+%F %T')" "$workload_kind" "$exp_id" "$label"
        continue
    fi
    runner_cmd=(
        python3 "$BARE_RUNNER"
        --repo-root "$REPO_ROOT"
        --output-base "$ROOT_OUTDIR"
        --batch-name "$([[ "$workload_kind" == "application" ]] && printf 'app_%s' "$exp_id" || printf 'bare_%s' "$exp_id")"
        --checkpoint-dir "$checkpoint_for_exp"
        --skip-inject
        --allow-concurrent-runs
        --inter-experiment-sleep-sec 0
    )
    if [[ "$COPY_ENGINE_CHANNELS" -gt 0 ]]; then
        bare_cmd+=(--copy-engine-channels "$COPY_ENGINE_CHANNELS")
    fi
    if [[ "$CHECKPOINT_HANDOFF_DEADLINE_SIM_SECONDS" -gt 0 ]]; then
        runner_cmd+=(--checkpoint-handoff-deadline-sim-seconds
                     "$CHECKPOINT_HANDOFF_DEADLINE_SIM_SECONDS")
    fi
    if [[ "$FORCE_RERUN" -eq 1 ]]; then
        runner_cmd+=(--force-rerun)
    fi
    submit_runner "$workload_kind" "$exp_id" "${runner_cmd[@]}"
done

wait_for_all_background

if [[ "$ANY_FAILURES" -ne 0 ]]; then
    echo "SimCXL current flat queue completed with failures."
else
    echo "SimCXL current flat queue completed."
fi
