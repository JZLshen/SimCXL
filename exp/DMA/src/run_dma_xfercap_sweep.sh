#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT_TAG="${1:-$(date +%Y%m%d_%H%M%S)}"
RAW_DIR="${ROOT_DIR}/exp/DMA/data/raw/xfercap_${OUT_TAG}"

# Keep the default payload size aligned with the existing DMA matrix sweep.
# Override TOTAL_BYTES explicitly if a smaller transfer is needed for faster
# exploratory runs at very small XFERCAP settings.
TOTAL_BYTES="${TOTAL_BYTES:-4194304}"
LOOPS="${LOOPS:-3}"
WARMUP="${WARMUP:-1}"
CPU_TYPE="${CPU_TYPE:-TIMING}"
TEST_TIMEOUT_SEC="${TEST_TIMEOUT_SEC:-7200}"
XFERCAP_SWEEP="${XFERCAP_SWEEP:-64B 128B 256B 512B 1KiB 2KiB 4KiB}"

mkdir -p "${RAW_DIR}"

for xfercap in ${XFERCAP_SWEEP}; do
  out_dir="${ROOT_DIR}/output/ce_xfercap_${xfercap}_${OUT_TAG}"
  mkdir -p "${out_dir}"

  read -r -d '' TEST_CMD <<EOF || true
set -e
echo "[ce-xfercap] configured_xfercap=${xfercap}"
/home/test_code/cxl_copyengine_bw \
  --engines 1 \
  --channels-per-engine 1 \
  --channel 0 \
  --total-bytes ${TOTAL_BYTES} \
  --loops ${LOOPS} \
  --warmup ${WARMUP} \
  --mode parallel \
  --verify 0
EOF

  echo "[ce-xfercap] run xfercap=${xfercap}"

  sudo -n bash -lc "
    set -euo pipefail
    cd '${ROOT_DIR}'
    build/X86/gem5.opt -d '${out_dir}' \
      configs/example/gem5_library/x86-cxl-rpc-test.py \
      --cpu_type '${CPU_TYPE}' \
      --num_cpus 1 \
      --num_copy_engines 1 \
      --copy_engine_channels 1 \
      --copy_engine_xfercap '${xfercap}' \
      --test_timeout_sec '${TEST_TIMEOUT_SEC}' \
      --fail_on_test_error false \
      --test_cmd $(printf '%q' "${TEST_CMD}") \
      > '${out_dir}/host.log' 2>&1
  "

  cp "${out_dir}/board.pc.com_1.device" \
    "${RAW_DIR}/ce_xfercap_${xfercap}_${OUT_TAG}.board.pc.com_1.device"
  cp "${out_dir}/host.log" \
    "${RAW_DIR}/ce_xfercap_${xfercap}_${OUT_TAG}.host.log"
done

python3 "${ROOT_DIR}/exp/DMA/src/extract_and_plot_dma_xfercap.py" \
  "${RAW_DIR}" \
  "${ROOT_DIR}/exp/DMA/data" \
  "${ROOT_DIR}/exp/DMA/images"

echo "xfercap_raw_dir=${RAW_DIR}"
