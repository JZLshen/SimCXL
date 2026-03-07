"""
CXL RPC test configuration (simplified).

Flow:
    - Boot from KVM.
    - If target cpu != KVM, one guest-side m5 exit triggers host switch.
    - Run one test command in guest.
    - One final guest-side m5 exit terminates simulation.
"""

import argparse
import shlex
from pathlib import Path

import m5
from gem5.components.boards.x86_board import X86Board
from gem5.components.cachehierarchies.classic.private_l1_private_l2_shared_l3_cache_hierarchy import (
    PrivateL1PrivateL2SharedL3CacheHierarchy,
)
from gem5.components.memory.single_channel import DIMM_DDR5_4400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.isas import ISA
from gem5.resources.resource import DiskImageResource, KernelResource
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

requires(isa_required=ISA.X86)

repo_root = Path(__file__).resolve().parents[3]
default_kernel = repo_root / "files" / "vmlinux"
default_disk = repo_root / "files" / "parsec.img"

parser = argparse.ArgumentParser(description="CXL RPC test parameters.")
parser.add_argument(
    "--is_asic",
    type=str,
    choices=["True", "False"],
    default="True",
    help="ASIC or FPGA device.",
)
parser.add_argument(
    "--test_cmd",
    type=str,
    default=(
        "/home/test_code/run_rpc_server_client.sh "
        "/home/test_code/rpc_server_example /home/test_code/rpc_client_example "
        "--requests 50 --max-polls 2000000 --silent"
    ),
    help="Test command to run after boot.",
)
parser.add_argument(
    "--kernel", type=str, default=str(default_kernel), help="Path to Linux kernel image"
)
parser.add_argument(
    "--disk", type=str, default=str(default_disk), help="Path to guest disk image"
)
parser.add_argument("--num_cpus", type=int, default=1, help="Number of CPUs")
parser.add_argument(
    "--cpu_type",
    type=str,
    choices=["TIMING", "O3", "ATOMIC", "KVM"],
    default="TIMING",
    help="CPU type after KVM boot",
)
parser.add_argument(
    "--checkpoint",
    type=str,
    default=None,
    help="Checkpoint directory to restore from (skip boot)",
)
parser.add_argument(
    "--pre_switch_settle_sec",
    type=int,
    default=1,
    help="Seconds to wait before guest m5 exit/switch.",
)
parser.add_argument(
    "--fail_on_test_error",
    type=str,
    default="true",
    choices=["true", "false"],
    help="If true, non-zero test cmd exits trigger m5 fail.",
)
parser.add_argument(
    "--test_timeout_sec",
    type=int,
    default=0,
    help="Default guest-side timeout seconds for test command (0 disables).",
)
args = parser.parse_args()


def _validate_test_mode(test_cmd: str) -> None:
    if ("run_rpc_server_client.sh" in test_cmd or
            "run_rpc_server_multi_client.sh" in test_cmd):
        return

    client_only_bins = (
        "/home/test_code/cxl_rpc_benchmark",
        "/home/test_code/rpc_client_example",
    )

    if any(bin_name in test_cmd for bin_name in client_only_bins):
        print("ERROR: CPU-to-CPU mode requires a server-client command.")
        print("  Recommended: run_rpc_server_client.sh <server> <client> [args]")
        print(f"  Current test_cmd: {test_cmd}")
        raise SystemExit(2)


_validate_test_mode(args.test_cmd)

cache_hierarchy = PrivateL1PrivateL2SharedL3CacheHierarchy(
    l1d_size="48kB",
    l1d_assoc=6,
    l1i_size="32kB",
    l1i_assoc=8,
    l2_size="2MB",
    l2_assoc=16,
    l3_size="96MB",
    l3_assoc=48,
)
memory = DIMM_DDR5_4400(size="3GB")
cxl_dram = DIMM_DDR5_4400(size="8GB")

cpu_type_map = {
    "TIMING": CPUTypes.TIMING,
    "O3": CPUTypes.O3,
    "ATOMIC": CPUTypes.ATOMIC,
    "KVM": CPUTypes.KVM,
}

processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=cpu_type_map[args.cpu_type],
    isa=ISA.X86,
    num_cores=args.num_cpus,
)

for proc in processor.start:
    proc.core.usePerf = False
for core_list in getattr(processor, "_switchable_cores", {}).values():
    for proc in core_list:
        if hasattr(proc, "core") and hasattr(proc.core, "usePerf"):
            proc.core.usePerf = False

board = X86Board(
    clk_freq="2.4GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    cxl_memory=cxl_dram,
    is_asic=(args.is_asic == "True"),
)

pre_switch_settle_sec = max(0, args.pre_switch_settle_sec)
fail_on_test_error = args.fail_on_test_error == "true"
test_timeout_sec = max(0, args.test_timeout_sec)
effective_test_cmd = args.test_cmd

script_lines = []
if args.cpu_type != "KVM":
    script_lines.extend(
        [
            f"echo \"[gem5-test] pre-switch settle: {pre_switch_settle_sec}s\"",
            "sync",
            f"sleep {pre_switch_settle_sec}",
            "m5 exit",
        ]
    )
elif pre_switch_settle_sec > 0:
    script_lines.extend(
        [
            f"echo \"[gem5-test] pre-test settle: {pre_switch_settle_sec}s\"",
            "sync",
            f"sleep {pre_switch_settle_sec}",
        ]
    )

script_lines.extend(
    [
        "set -u",
        f"export CXL_RPC_TEST_TIMEOUT_SEC=\"${{CXL_RPC_TEST_TIMEOUT_SEC:-{test_timeout_sec}}}\"",
        "exec >/dev/ttyS0 2>&1",
        f"TEST_CMD={shlex.quote(effective_test_cmd)}",
        "echo '=== CXL RPC Evaluation ==='",
        "echo \"Test command: ${TEST_CMD}\"",
        "echo '[gem5-test] RUN_TEST_CMD_BEGIN'",
        "if [ \"${CXL_RPC_TEST_TIMEOUT_SEC}\" -gt 0 ] && command -v timeout >/dev/null 2>&1; then",
        "  timeout --signal=TERM --kill-after=5 \"${CXL_RPC_TEST_TIMEOUT_SEC}s\" bash -c \"$TEST_CMD\"",
        "  TEST_CMD_RC=$?",
        "else",
        "  bash -c \"$TEST_CMD\"",
        "  TEST_CMD_RC=$?",
        "fi",
        "echo '[gem5-test] RUN_TEST_CMD_END'",
        "echo \"TEST_CMD_EXIT_CODE=${TEST_CMD_RC}\"",
    ]
)

if fail_on_test_error:
    script_lines.extend(
        [
            "if [ \"$TEST_CMD_RC\" -ne 0 ]; then",
            "  echo \"[gem5-test] m5 fail due to test failure rc=${TEST_CMD_RC}\"",
            "  m5 fail \"$TEST_CMD_RC\"",
            "fi",
        ]
    )
else:
    script_lines.extend(
        [
            "if [ \"$TEST_CMD_RC\" -ne 0 ]; then",
            "  echo \"[gem5-test] non-zero test rc=${TEST_CMD_RC}; exiting without m5 fail\"",
            "fi",
        ]
    )

script_lines.extend(["m5 exit"])
command = "\n".join(script_lines)

workload_kwargs = {}
if args.checkpoint:
    workload_kwargs["checkpoint"] = Path(args.checkpoint)

board.set_kernel_disk_workload(
    kernel=KernelResource(local_path=args.kernel),
    disk_image=DiskImageResource(local_path=args.disk),
    kernel_args=[
        "earlyprintk=ttyS0",
        "console=ttyS0",
        "lpj=7999923",
        "root={root_value}",
        "idle=poll",
    ],
    readfile_contents=command,
    **workload_kwargs,
)

if args.checkpoint:
    print("Running CXL RPC test (checkpoint restore)...")
    print(f"  Checkpoint: {args.checkpoint}")
else:
    print("Running CXL RPC test simulation...")
print(f"  CPU type: KVM -> {args.cpu_type}")
print(f"  Kernel: {args.kernel}")
print(f"  Disk: {args.disk}")
print(f"  Test command: {args.test_cmd}")
print(f"  Effective command: {effective_test_cmd}")
print(f"  Guest test timeout sec: {test_timeout_sec}")


def _switch_exit_handler():
    switched = False
    while True:
        if not switched:
            processor.switch()
            switched = True
            yield False
        else:
            yield True


def _terminate_on_first_exit_handler():
    while True:
        yield True


exit_handler = (
    _terminate_on_first_exit_handler()
    if args.cpu_type == "KVM"
    else _switch_exit_handler()
)

simulator = Simulator(
    board=board,
    on_exit_event={ExitEvent.EXIT: exit_handler},
)

m5.stats.reset()
simulator.run()
last_cause = simulator.get_last_exit_event_cause()
if ExitEvent.translate_exit_status(last_cause) == ExitEvent.FAIL:
    print(f"Simulation ended with FAIL event: {last_cause}")
    raise SystemExit(1)
