#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash tests/test-progs/cxl-rpc/scripts/run_rpc_moti_overall_pool.sh [options]

Options:
  --repo-root <dir>         Repo root. Default: auto-detect
  --output-base <dir>       Output base directory relative to repo root. Default: output
  --batch-name <name>       Batch root name. Default: rpc_moti_overall_pool_<timestamp>
  --checkpoint-root <dir>   Root containing clients_{N}_lanes_{N}_mq_1024_cxlp_0ns checkpoints. Required.
  --disk <path>             Guest disk image. Default: <repo>/files/parsec.img
  --parallel-jobs <N>       Max concurrent gem5 jobs. Default: 18
  --requests <N>            Requests per client. Default: 30
  --client-window <N>       Guest client sliding window override. Default: 1
  --skip-build              Reuse the existing gem5 binary.
  --skip-inject             Reuse the already injected guest binaries.
  --force-rerun             Re-run even if the batch output already exists.
  --continue-on-failure     Record failures and keep advancing the queue.
  --help                    Show this message.

Scope:
  - canonical SimCXL moti+overall unique points
  - overall only, 18 total experiments
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT_DEFAULT="$(cd "${SCRIPT_DIR}/../../../.." && pwd)"

REPO_ROOT="$REPO_ROOT_DEFAULT"
OUTPUT_BASE="output"
BATCH_NAME=""
CHECKPOINT_ROOT=""
DISK=""
PARALLEL_JOBS=18
REQUESTS=30
CLIENT_WINDOW=1
SKIP_BUILD=0
SKIP_INJECT=0
FORCE_RERUN=0
CONTINUE_ON_FAILURE=0
ANY_FAILURES=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo-root)
      REPO_ROOT="$2"
      shift 2
      ;;
    --output-base)
      OUTPUT_BASE="$2"
      shift 2
      ;;
    --batch-name)
      BATCH_NAME="$2"
      shift 2
      ;;
    --checkpoint-root)
      CHECKPOINT_ROOT="$2"
      shift 2
      ;;
    --disk)
      DISK="$2"
      shift 2
      ;;
    --parallel-jobs)
      PARALLEL_JOBS="$2"
      shift 2
      ;;
    --requests)
      REQUESTS="$2"
      shift 2
      ;;
    --client-window)
      CLIENT_WINDOW="$2"
      shift 2
      ;;
    --skip-build)
      SKIP_BUILD=1
      shift 1
      ;;
    --skip-inject)
      SKIP_INJECT=1
      shift 1
      ;;
    --force-rerun)
      FORCE_RERUN=1
      shift 1
      ;;
    --continue-on-failure)
      CONTINUE_ON_FAILURE=1
      shift 1
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

REPO_ROOT="$(cd "$REPO_ROOT" && pwd)"
cd "$REPO_ROOT"

if [[ -z "$CHECKPOINT_ROOT" ]]; then
  echo "--checkpoint-root is required" >&2
  exit 1
fi
CHECKPOINT_ROOT="$(cd "$CHECKPOINT_ROOT" && pwd)"
if [[ ! -d "$CHECKPOINT_ROOT" ]]; then
  echo "checkpoint root not found: $CHECKPOINT_ROOT" >&2
  exit 1
fi
if [[ "$PARALLEL_JOBS" -lt 1 ]]; then
  echo "--parallel-jobs must be >= 1" >&2
  exit 1
fi
if [[ "$REQUESTS" -lt 1 ]]; then
  echo "--requests must be >= 1" >&2
  exit 1
fi
if [[ "$CLIENT_WINDOW" -lt 1 ]]; then
  echo "--client-window must be >= 1" >&2
  exit 1
fi

if [[ -z "$DISK" ]]; then
  DISK="$REPO_ROOT/files/parsec.img"
fi
DISK="$(cd "$(dirname "$DISK")" && pwd)/$(basename "$DISK")"
if [[ ! -f "$DISK" ]]; then
  echo "disk image not found: $DISK" >&2
  exit 1
fi

if [[ -z "$BATCH_NAME" ]]; then
  BATCH_NAME="rpc_moti_overall_pool_$(date +%Y%m%d_%H%M%S)"
fi

OUTPUT_ROOT="$REPO_ROOT/$OUTPUT_BASE/$BATCH_NAME"
MANIFEST="$OUTPUT_ROOT/cases.tsv"
RUN_LOG="$OUTPUT_ROOT/run.log"
FAIL_LOG="$OUTPUT_ROOT/failures.tsv"
mkdir -p "$OUTPUT_ROOT"
: >"$RUN_LOG"
: >"$FAIL_LOG"

log_msg() {
  printf '[%s] %s\n' "$(date '+%F %T')" "$*" | tee -a "$RUN_LOG"
}

prepare_prereqs() {
  if [[ "$SKIP_BUILD" -eq 0 ]]; then
    log_msg "BUILD build/X86/gem5.opt"
    scons build/X86/gem5.opt -j"$(nproc)"
  fi

  if [[ "$SKIP_INJECT" -eq 0 ]]; then
    log_msg "SETUP-IMAGE $DISK"
    bash tests/test-progs/cxl-rpc/scripts/setup_disk_image.sh "$DISK"
  fi
}

build_case_manifest() {
  python3 - "$REPO_ROOT" "$MANIFEST" "$REQUESTS" <<'PY'
import csv
import sys
from pathlib import Path

repo_root = Path(sys.argv[1])
manifest = Path(sys.argv[2])
requests = int(sys.argv[3])

sys.path.insert(0, str(repo_root / "tests/test-progs/cxl-rpc/scripts"))
import run_rpc_matrix_kvm_timing_ckpt as matrix

experiments = [
    exp for exp in matrix.build_matrix(
        requests,
        matrix.DEFAULT_RESPONSE_DMA_THRESHOLD,
        matrix.DEFAULT_PREFETCH_MODE,
        matrix.DEFAULT_REQUEST_SPARSITY_SLOW_CLIENT_SEND_PAUSE_ITERS,
    )
    if exp.source == "overall"
]

profile_order = {
    matrix.MESSAGE_PROFILE_FIXED: 0,
    matrix.MESSAGE_PROFILE_UNIFORM_1530_315: 1,
    matrix.MESSAGE_PROFILE_UNIFORM_38_230: 2,
}
experiments.sort(
    key=lambda exp: (
        -exp.key.clients,
        profile_order.get(exp.key.message_profile, 99),
        exp.exp_id,
    )
)

with manifest.open("w", newline="") as fh:
    writer = csv.writer(fh, delimiter="\t")
    writer.writerow([
        "exp_id",
        "clients",
        "request_size",
        "response_size",
        "request_min_size",
        "request_max_size",
        "response_min_size",
        "response_max_size",
        "message_profile",
    ])
    for exp in experiments:
        key = exp.key
        writer.writerow([
            exp.exp_id,
            key.clients,
            key.request_size,
            key.response_size,
            key.request_min_size,
            key.request_max_size,
            key.response_min_size,
            key.response_max_size,
            key.message_profile,
        ])

print(len(experiments))
PY
}

launch_case() {
  local exp_id="$1"
  local clients="$2"
  local checkpoint_dir="$CHECKPOINT_ROOT/clients_${clients}_lanes_${clients}_mq_1024_cxlp_0ns"
  local case_batch_name="$BATCH_NAME/$exp_id"
  local case_log="$OUTPUT_ROOT/${exp_id}.console.log"
  local cmd=()

  if [[ ! -d "$checkpoint_dir" ]]; then
    log_msg "MISSING-CHECKPOINT exp_id=$exp_id checkpoint=$checkpoint_dir"
    printf '%s\t%s\n' "$exp_id" "missing_checkpoint" >>"$FAIL_LOG"
    return 1
  fi

  cmd=(
    python3 tests/test-progs/cxl-rpc/scripts/run_rpc_matrix_kvm_timing_ckpt.py
    --repo-root "$REPO_ROOT"
    --output-base "$OUTPUT_BASE"
    --batch-name "$case_batch_name"
    --disk "$DISK"
    --checkpoint-dir "$checkpoint_dir"
    --requests "$REQUESTS"
    --client-window "$CLIENT_WINDOW"
    --only-exp-id "$exp_id"
    --allow-concurrent-runs
    --inter-experiment-sleep-sec 0
  )

  if [[ "$SKIP_INJECT" -eq 1 ]]; then
    cmd+=(--skip-inject)
  fi
  if [[ "$FORCE_RERUN" -eq 1 ]]; then
    cmd+=(--force-rerun)
  fi

  (
    set +e
    log_msg "START exp_id=$exp_id checkpoint=$checkpoint_dir"
    "${cmd[@]}" >"$case_log" 2>&1
    local rc=$?
    set -e
    if [[ "$rc" -ne 0 ]]; then
      printf '%s\t%s\n' "$exp_id" "$rc" >>"$FAIL_LOG"
      log_msg "RUN-FAIL exp_id=$exp_id rc=$rc"
    else
      log_msg "END exp_id=$exp_id"
    fi
    exit "$rc"
  ) &
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
      log_msg "CONTINUE-FAIL rc=${wait_rc}; continuing queue"
      return 0
    fi
    jobs -pr | xargs -r kill 2>/dev/null || true
    wait || true
    exit "$wait_rc"
  fi
}

wait_for_slot() {
  while [[ "$(jobs -pr | wc -l)" -ge "$PARALLEL_JOBS" ]]; do
    wait_for_background_or_fail
  done
}

prepare_prereqs
case_count="$(build_case_manifest)"
log_msg "QUEUED total_cases=${case_count} manifest=${MANIFEST}"

{
  read -r _
  while IFS=$'\t' read -r exp_id clients request_size response_size request_min_size request_max_size response_min_size response_max_size message_profile; do
    launch_case "$exp_id" "$clients"
    wait_for_slot
  done
} <"$MANIFEST"

while [[ "$(jobs -pr | wc -l)" -gt 0 ]]; do
  wait_for_background_or_fail
done

if [[ "$ANY_FAILURES" -ne 0 ]]; then
  log_msg "DONE with failures fail_log=${FAIL_LOG}"
  exit 1
fi

log_msg "DONE success"
