#!/bin/bash
# run_rpc_test.sh - Run CXL RPC tests in gem5 full-system simulation
#
# Usage:
#   ./scripts/run_rpc_test.sh SCENARIO [OPTIONS]
#
# Scenarios:
#   save-checkpoint  Boot with KVM and save checkpoint (one-time setup)
#   benchmark        Run dual-process client/server benchmark (latency P50/P90/P99)
#   server-client    Run server + client pair
#   all              Run all test scenarios sequentially
#
# Options:
#   --cpu-type TIMING|O3|KVM  CPU type (default: TIMING)
#   --num-cpus N                 Number of CPUs (default: 1)
#   --checkpoint DIR             Restore from checkpoint (skip boot)
#   --debug                      Enable CXL debug flags
#   --output-dir DIR             Override output base directory

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RPC_DIR="$(dirname "$SCRIPT_DIR")"
SIMCXL_DIR="$(cd "$RPC_DIR/../../.." && pwd)"

# Defaults
GEM5_BIN="$SIMCXL_DIR/build/X86/gem5.opt"
CONFIG="$SIMCXL_DIR/configs/example/gem5_library/x86-cxl-rpc-test.py"
SAVE_CPT_CONFIG="$SIMCXL_DIR/configs/example/gem5_library/x86-cxl-rpc-save-checkpoint.py"
TRACE_PARSER="$SIMCXL_DIR/tools/parse_crosscore_cxl_trace.py"
CPU_TYPE="TIMING"
NUM_CPUS=1
DEBUG_FLAGS=""
OUTPUT_BASE="$SIMCXL_DIR/output"
CHECKPOINT_DIR=""

usage() {
    echo "Usage: $0 SCENARIO [OPTIONS]"
    echo ""
    echo "Scenarios:"
    echo "  save-checkpoint  Boot with KVM, save checkpoint (one-time)"
    echo "  benchmark        Dual-process benchmark (P50/P90/P99)"
    echo "  server-client    Server + client pair"
    echo "  all              Run all test scenarios"
    echo ""
    echo "Options:"
    echo "  --cpu-type TYPE      TIMING, O3, or KVM (default: TIMING)"
    echo "  --num-cpus N         Number of CPUs (default: 1)"
    echo "  --checkpoint DIR     Restore from checkpoint (skip boot)"
    echo "  --debug              Enable CXL debug flags"
    echo "  --output-dir DIR     Output base directory (default: $OUTPUT_BASE)"
    echo ""
    echo "Examples:"
    echo "  # Save checkpoint once (~2-5 min)"
    echo "  $0 save-checkpoint"
    echo ""
    echo "  # Reuse checkpoint to run tests (fast)"
    echo "  $0 server-client --checkpoint $OUTPUT_BASE/cxl_rpc_checkpoint"
    echo ""
    echo "  # Or: stay on KVM after restore"
    echo "  $0 server-client --cpu-type KVM"
}

# Parse scenario
SCENARIO="${1:-}"
if [ -z "$SCENARIO" ] || [ "$SCENARIO" = "--help" ] || [ "$SCENARIO" = "-h" ]; then
    usage
    exit 0
fi
shift

# Parse options
while [ $# -gt 0 ]; do
    case "$1" in
        --cpu-type)    CPU_TYPE="$2"; shift 2 ;;
        --num-cpus)    NUM_CPUS="$2"; shift 2 ;;
        --checkpoint)  CHECKPOINT_DIR="$2"; shift 2 ;;
        --debug)       DEBUG_FLAGS="--debug-flags=CXLRPCEngine,CXLMemCtrl --debug-file=cxl_trace.log"; shift ;;
        --output-dir)  OUTPUT_BASE="$2"; shift 2 ;;
        *)
            echo "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Verify gem5 binary
if [ ! -x "$GEM5_BIN" ]; then
    echo "ERROR: gem5 binary not found: $GEM5_BIN"
    echo "Build with: scons build/X86/gem5.opt -j\$(nproc)"
    exit 1
fi

# Resolve checkpoint path - verify m5.cpt exists in the directory
resolve_checkpoint() {
    local dir="$1"
    if [ -f "$dir/m5.cpt" ]; then
        echo "$dir"
    elif [ -d "$dir" ]; then
        echo "ERROR: Directory exists but no m5.cpt found: $dir"
        exit 1
    else
        echo "ERROR: Checkpoint not found: $dir"
        exit 1
    fi
}

detect_checkpoint_cpus() {
    local dir="$1"
    local cfg="$dir/config.ini"
    local count=""

    if [ ! -f "$cfg" ]; then
        echo ""
        return 0
    fi

    # Count serialized start-core sections in the checkpoint config.
    count=$(rg -n '^\[board\.processor\.start[0-9]+\]$' "$cfg" 2>/dev/null | wc -l | tr -d ' ')
    if [ -n "$count" ] && [ "$count" -gt 0 ] 2>/dev/null; then
        echo "$count"
    else
        echo ""
    fi
}

run_gem5() {
    local name="$1"
    local test_cmd="$2"
    local cpus="${3:-$NUM_CPUS}"
    local output_dir="$OUTPUT_BASE/rpc_${name}"
    local console_log="$output_dir/gem5.log"
    local effective_test_cmd="$test_cmd"

    echo "=== Running: $name ==="
    echo "  Command:    $effective_test_cmd"
    echo "  CPUs:       $cpus"
    echo "  CPU type:   $CPU_TYPE"
    echo "  Checkpoint: ${CHECKPOINT_DIR:-none}"
    echo "  Output:     $output_dir"
    echo "  Debug:      ${DEBUG_FLAGS:-none}"
    echo ""

    mkdir -p "$output_dir"

    local checkpoint_arg=""
    if [ -n "$CHECKPOINT_DIR" ]; then
        local cpt_path
        local cpt_cpus
        cpt_path=$(resolve_checkpoint "$CHECKPOINT_DIR")
        checkpoint_arg="--checkpoint $cpt_path"
        cpt_cpus=$(detect_checkpoint_cpus "$cpt_path")
        if [ -n "$cpt_cpus" ] && [ "$cpus" != "$cpt_cpus" ]; then
            echo "INFO: checkpoint was created with $cpt_cpus CPUs; overriding run CPUs from $cpus to $cpt_cpus"
            cpus="$cpt_cpus"
        fi
    fi

    "$GEM5_BIN" \
        -d "$output_dir" \
        $DEBUG_FLAGS \
        "$CONFIG" \
        --test_cmd "$effective_test_cmd" \
        --cpu_type "$CPU_TYPE" \
        --num_cpus "$cpus" \
        $checkpoint_arg \
        2>&1 | tee "$console_log"

    echo ""
    echo "=== $name complete ==="
    echo "  Output:     $output_dir"
    echo "  Console:    $console_log"
    echo "  Stats:      $output_dir/stats.txt"
    if [ -f "$output_dir/cxl_trace.log" ] && [ -f "$TRACE_PARSER" ]; then
        echo "  CXL trace:  $output_dir/cxl_trace.log"
        echo "=== Global-tick trace summary ($name) ==="
        python3 "$TRACE_PARSER" "$output_dir/cxl_trace.log" || true
    fi
    echo ""
}

case "$SCENARIO" in
    save-checkpoint)
        output_dir="$OUTPUT_BASE/cxl_rpc_checkpoint"
        console_log="$output_dir/gem5.log"
        echo "=== Saving CXL RPC Boot Checkpoint ==="
        echo "  CPUs:     $NUM_CPUS"
        echo "  Output:   $output_dir"
        echo "  Note:     Checkpoint saves KVM state, restore can use TIMING, O3, or KVM"
        echo ""

        mkdir -p "$output_dir"

        "$GEM5_BIN" \
            -d "$output_dir" \
            "$SAVE_CPT_CONFIG" \
            --num_cpus "$NUM_CPUS" \
            --rpc_client_count "$(( NUM_CPUS > 1 ? NUM_CPUS - 1 : 1 ))" \
            2>&1 | tee "$console_log"

        echo ""
        echo "=== Checkpoint saved ==="
        echo "  Console: $console_log"
        echo "  Use with: $0 server-client --checkpoint $output_dir"
        ;;

    server-client)
        cpus=$NUM_CPUS
        if [ "$cpus" -lt 2 ]; then
            cpus=2
        fi
        run_gem5 "server_client" \
            "bash /home/test_code/run_rpc_server_client.sh /home/test_code/rpc_server_example /home/test_code/rpc_client_example --requests 2 --max-polls 2000000 --silent" \
            "$cpus"
        ;;

    benchmark)
        cpus=$NUM_CPUS
        if [ "$cpus" -lt 2 ]; then
            cpus=2
        fi
        run_gem5 "benchmark" \
            "bash /home/test_code/run_rpc_server_client.sh /home/test_code/rpc_server_example /home/test_code/rpc_client_example --requests 500 --max-polls 2000000 --silent" \
            "$cpus"
        ;;

    all)
        echo "=== Running All CXL RPC Test Scenarios ==="
        echo ""

        CPT_ARG=""
        if [ -n "$CHECKPOINT_DIR" ]; then
            CPT_ARG="--checkpoint $CHECKPOINT_DIR"
        fi

        "$0" benchmark     --cpu-type "$CPU_TYPE" --num-cpus "$NUM_CPUS" --output-dir "$OUTPUT_BASE" ${DEBUG_FLAGS:+--debug} $CPT_ARG
        "$0" server-client --cpu-type "$CPU_TYPE" --num-cpus 2           --output-dir "$OUTPUT_BASE" ${DEBUG_FLAGS:+--debug} $CPT_ARG
        echo "=== All scenarios complete ==="
        echo "Output: $OUTPUT_BASE/rpc_*"
        ;;

    *)
        echo "Unknown scenario: $SCENARIO"
        echo ""
        usage
        exit 1
        ;;
esac
