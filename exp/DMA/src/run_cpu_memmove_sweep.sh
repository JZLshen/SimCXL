#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT_TAG="${1:-$(date +%Y%m%d_%H%M%S)}"
RAW_DIR="${ROOT_DIR}/exp/DMA/data/raw/cpu_memmove_${OUT_TAG}"
DISK_IMAGE="${DISK_IMAGE:-${ROOT_DIR}/files/parsec.img}"
SYNC_GUEST_BIN="${SYNC_GUEST_BIN:-1}"

CPU_SWEEP="${CPU_SWEEP:-1 2 4 8 16}"
TOTAL_BYTES_PER_THREAD="${TOTAL_BYTES_PER_THREAD:-16777216}"
LOOPS="${LOOPS:-3}"
WARMUP="${WARMUP:-1}"
CPU_TYPE="${CPU_TYPE:-TIMING}"
TEST_TIMEOUT_SEC="${TEST_TIMEOUT_SEC:-14400}"
PIN_THREADS="${PIN_THREADS:-1}"
VERIFY="${VERIFY:-0}"

mkdir -p "${RAW_DIR}"

if [ "${SYNC_GUEST_BIN}" != "0" ]; then
  env CXL_RPC_INCLUDE_CPU_MEMMOVE_BW=1 \
    bash "${ROOT_DIR}/tests/test-progs/cxl-rpc/scripts/setup_disk_image.sh" \
    "${DISK_IMAGE}"
fi

for n in ${CPU_SWEEP}; do
  out_dir="${ROOT_DIR}/output/cpu_memmove_${n}c_${OUT_TAG}"
  mkdir -p "${out_dir}"

  read -r -d '' TEST_CMD <<EOF || true
set -e
echo "[cpu-memmove] sweep N=${n}"
/home/test_code/cpu_memmove_bw \
  --threads ${n} \
  --total-bytes ${TOTAL_BYTES_PER_THREAD} \
  --loops ${LOOPS} \
  --warmup ${WARMUP} \
  --pin ${PIN_THREADS} \
  --verify ${VERIFY}
EOF

  echo "[cpu-memmove] run N=${n}"

  sudo -n bash -lc "
    set -euo pipefail
    cd '${ROOT_DIR}'
    build/X86/gem5.opt -d '${out_dir}' \
      configs/example/gem5_library/x86-cxl-rpc-test.py \
      --cpu_type '${CPU_TYPE}' \
      --num_cpus '${n}' \
      --disk '${DISK_IMAGE}' \
      --test_timeout_sec '${TEST_TIMEOUT_SEC}' \
      --fail_on_test_error true \
      --test_cmd $(printf '%q' "${TEST_CMD}") \
      > '${out_dir}/host.log' 2>&1
  "

  cp "${out_dir}/board.pc.com_1.device" \
    "${RAW_DIR}/cpu_memmove_${n}c_${OUT_TAG}.board.pc.com_1.device"
  cp "${out_dir}/host.log" \
    "${RAW_DIR}/cpu_memmove_${n}c_${OUT_TAG}.host.log"
done

python3 "${ROOT_DIR}/exp/DMA/src/extract_and_plot_cpu_memmove.py" \
  "${RAW_DIR}" \
  "${ROOT_DIR}/exp/DMA/data" \
  "${ROOT_DIR}/exp/DMA/images"

echo "cpu_memmove_raw_dir=${RAW_DIR}"
