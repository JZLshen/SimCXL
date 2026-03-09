#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT_TAG="${1:-$(date +%Y%m%d_%H%M%S)}"
OUT_DIR="${ROOT_DIR}/output/ce_matrix_${OUT_TAG}"

NUM_COPY_ENGINES="${NUM_COPY_ENGINES:-16}"
COPY_ENGINE_CHANNELS="${COPY_ENGINE_CHANNELS:-16}"
TOTAL_BYTES="${TOTAL_BYTES:-4194304}"
LOOPS="${LOOPS:-3}"
WARMUP="${WARMUP:-1}"
CPU_TYPE="${CPU_TYPE:-TIMING}"
TEST_TIMEOUT_SEC="${TEST_TIMEOUT_SEC:-7200}"

ENGINE_SWEEP="${ENGINE_SWEEP:-1 2 4 8 16}"
CHANNEL_SWEEP="${CHANNEL_SWEEP:-1 2 4 8 16}"

read -r -d '' TEST_CMD <<EOF || true
set -e

echo "[ce-matrix] begin engine sweep"
for n in ${ENGINE_SWEEP}; do
  echo "[ce-matrix] engine_sweep N=\${n}"
  /home/test_code/cxl_copyengine_bw \
    --engines "\${n}" \
    --channels-per-engine 1 \
    --channel 0 \
    --total-bytes ${TOTAL_BYTES} \
    --loops ${LOOPS} \
    --warmup ${WARMUP} \
    --mode parallel \
    --verify 0
done

echo "[ce-matrix] begin channel sweep"
for c in ${CHANNEL_SWEEP}; do
  echo "[ce-matrix] channel_sweep C=\${c}"
  /home/test_code/cxl_copyengine_bw \
    --engines 1 \
    --channels-per-engine "\${c}" \
    --channel 0 \
    --total-bytes ${TOTAL_BYTES} \
    --loops ${LOOPS} \
    --warmup ${WARMUP} \
    --mode parallel \
    --verify 0
done
EOF

mkdir -p "${OUT_DIR}"

sudo -n bash -lc "
  set -euo pipefail
  cd '${ROOT_DIR}'
  build/X86/gem5.opt -d '${OUT_DIR}' \
    configs/example/gem5_library/x86-cxl-rpc-test.py \
    --cpu_type '${CPU_TYPE}' \
    --num_cpus 1 \
    --num_copy_engines '${NUM_COPY_ENGINES}' \
    --copy_engine_channels '${COPY_ENGINE_CHANNELS}' \
    --test_timeout_sec '${TEST_TIMEOUT_SEC}' \
    --fail_on_test_error false \
    --test_cmd $(printf '%q' "${TEST_CMD}") \
    > '${OUT_DIR}/host.log' 2>&1
"

echo "matrix_output=${OUT_DIR}"
