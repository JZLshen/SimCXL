"""
CXL RPC test configuration (simplified).

Flow:
    - Boot from KVM.
    - If target cpu != KVM, one guest-side m5 exit triggers host switch.
    - Run one test command in guest.
    - One final guest-side m5 exit terminates simulation.
"""

import argparse
import os
import re
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
MAX_COPY_ENGINES = 29
MAX_COPY_ENGINE_CHANNELS = 64

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
    "--rpc_client_count",
    type=int,
    default=0,
    help=(
        "RPC client count. "
        "0 means infer from test_cmd and bind one engine per client."
    ),
)
parser.add_argument(
    "--copy_engine_channels",
    type=int,
    default=0,
    help=(
        "Channels per CopyEngine. 0 auto-derives the minimum channel count "
        "needed to provide at least one response lane per client."
    ),
)
parser.add_argument(
    "--copy_engine_xfercap",
    type=str,
    default="4KiB",
    help="Per-CopyEngine maximum transfer size advertised through XFERCAP.",
)
parser.add_argument(
    "--cpu_type",
    type=str,
    choices=["TIMING", "O3", "KVM", "ATOMIC"],
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

if args.rpc_client_count < 0:
    parser.error("--rpc_client_count must be >= 0")

KVM_UNSUPPORTED_M5_RPNS_BINS = {
    "rpc_client_example",
    "cxl_mem_copy_cmp",
    "cpu_memmove_bw",
}


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


def _validate_kvm_guest_compat(test_cmd: str, cpu_type: str) -> None:
    if cpu_type != "KVM":
        return

    try:
        tokens = shlex.split(test_cmd, posix=True)
    except ValueError:
        tokens = test_cmd.split()

    unsupported = []
    for token in tokens:
        base = os.path.basename(token)
        if base in KVM_UNSUPPORTED_M5_RPNS_BINS:
            unsupported.append(base)

    if not unsupported:
        return

    bins = ", ".join(sorted(set(unsupported)))
    parser.error(
        "KVM post-boot execution is unsupported for the selected test command "
        "because these guest binaries execute m5_rpns(), which traps as an "
        f"invalid opcode under KVM: {bins}. Use a non-KVM post-boot CPU "
        "type (TIMING/O3/ATOMIC) for this RPC path."
    )


_validate_kvm_guest_compat(args.test_cmd, args.cpu_type)


def _parse_positive_int(text: str) -> int | None:
    try:
        value = int(text, 0)
    except (TypeError, ValueError):
        return None
    return value if value > 0 else None


def _infer_rpc_num_clients(test_cmd: str) -> int:
    try:
        tokens = shlex.split(test_cmd, posix=True)
    except ValueError:
        return 1

    for token in tokens:
        if token.startswith("CXL_RPC_CLIENT_COUNT="):
            value = _parse_positive_int(token.split("=", 1)[1])
            if value is not None:
                return value

    for idx, token in enumerate(tokens[:-1]):
        if token == "--num-clients":
            value = _parse_positive_int(tokens[idx + 1])
            if value is not None:
                return value

    if any(token.endswith("run_rpc_server_multi_client.sh") for token in tokens):
        return 4

    return 1


def _load_checkpoint_copyengine_topology(
    checkpoint_dir: str | None,
) -> tuple[int, int] | None:
    if not checkpoint_dir:
        return None

    cfg = Path(checkpoint_dir) / "config.ini"
    if not cfg.is_file():
        parser.error(
            "restored checkpoint is missing config.ini for CopyEngine topology: "
            f"{cfg}"
        )

    engine_sections = 0
    channel_count = 0
    current_is_engine = False

    with cfg.open("r", encoding="utf-8", errors="ignore") as fp:
        for raw_line in fp:
            line = raw_line.strip()
            if not line:
                continue

            if re.match(
                r"^\[board\.pc\.south_bridge\.copy_engines(?:\d+)?\]$",
                line,
            ):
                engine_sections += 1
                current_is_engine = True
                continue

            if line.startswith("["):
                current_is_engine = False
                continue

            if current_is_engine and line.startswith("ChanCnt="):
                parsed = _parse_positive_int(line.split("=", 1)[1])
                if parsed is not None and parsed > channel_count:
                    channel_count = parsed

    if engine_sections == 0:
        parser.error(
            "restored checkpoint does not describe any CopyEngine sections: "
            f"{cfg}"
        )
    if channel_count == 0:
        parser.error(
            "restored checkpoint is missing ChanCnt for CopyEngine topology: "
            f"{cfg}"
        )
    return engine_sections, channel_count


def _ceil_div(numer: int, denom: int) -> int:
    return (numer + denom - 1) // denom


def _derive_copyengine_topology(
    requested_clients: int,
    requested_channels: int,
) -> tuple[int, int]:
    if requested_clients < 1:
        requested_clients = 1
    if requested_channels < 0:
        parser.error("--copy_engine_channels must be >= 0")

    channels = requested_channels
    if channels == 0:
        channels = max(1, _ceil_div(requested_clients, MAX_COPY_ENGINES))

    if channels > MAX_COPY_ENGINE_CHANNELS:
        parser.error(
            "--copy_engine_channels exceeds board limit: "
            f"{channels} > {MAX_COPY_ENGINE_CHANNELS}"
        )

    num_engines = _ceil_div(requested_clients, channels)
    if num_engines > MAX_COPY_ENGINES:
        max_clients = MAX_COPY_ENGINES * channels
        parser.error(
            "requested rpc_client_count exceeds current X86Board lane "
            f"capacity: clients={requested_clients}, channels/engine={channels}, "
            f"max_supported_clients={max_clients}"
        )

    return num_engines, channels

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
    "KVM": CPUTypes.KVM,
    "ATOMIC": CPUTypes.ATOMIC,
}

if not os.access("/dev/kvm", os.R_OK | os.W_OK):
    parser.error(
        "x86-cxl-rpc-test.py requires read/write access to /dev/kvm for the "
        "mandatory KVM boot path"
    )

checkpoint_topology = _load_checkpoint_copyengine_topology(args.checkpoint)
requested_rpc_clients = (
    args.rpc_client_count
    if args.rpc_client_count > 0
    else _infer_rpc_num_clients(args.test_cmd)
)

if checkpoint_topology is not None:
    num_copy_engines, copy_engine_channels = checkpoint_topology
    available_lanes = num_copy_engines * copy_engine_channels
    if (args.copy_engine_channels > 0 and
            args.copy_engine_channels != copy_engine_channels):
        parser.error(
            "--copy_engine_channels conflicts with restored checkpoint "
            f"topology: requested {args.copy_engine_channels}, "
            f"checkpoint has {copy_engine_channels}"
        )
    if requested_rpc_clients > available_lanes:
        parser.error(
            "checkpoint CopyEngine topology is smaller than rpc_client_count: "
            f"{requested_rpc_clients} client(s) need at least "
            f"{requested_rpc_clients} lane(s), checkpoint has "
            f"{available_lanes} lane(s) "
            f"({num_copy_engines} engine(s) x {copy_engine_channels} "
            "channel(s))"
        )
else:
    num_copy_engines, copy_engine_channels = _derive_copyengine_topology(
        requested_rpc_clients,
        args.copy_engine_channels,
    )

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
    num_copy_engines=num_copy_engines,
    copy_engine_channels=copy_engine_channels,
    copy_engine_xfercap=args.copy_engine_xfercap,
)

pre_switch_settle_sec = max(0, args.pre_switch_settle_sec)
fail_on_test_error = args.fail_on_test_error == "true"
test_timeout_sec = max(0, args.test_timeout_sec)
effective_test_cmd = args.test_cmd

script_lines = []
if args.cpu_type != "KVM":
    script_lines.extend(
        [
            "sync",
            f"sleep {pre_switch_settle_sec}",
            "m5 exit",
        ]
    )
elif pre_switch_settle_sec > 0:
    script_lines.extend(
        [
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
        "if [ \"${CXL_RPC_TEST_TIMEOUT_SEC}\" -gt 0 ]; then",
        "  command -v timeout >/dev/null 2>&1 || {",
        "    echo \"[gem5-test] timeout command is required when CXL_RPC_TEST_TIMEOUT_SEC > 0\"",
        "    exit 127",
        "  }",
        "  timeout --signal=TERM --kill-after=5 \"${CXL_RPC_TEST_TIMEOUT_SEC}s\" bash -c \"$TEST_CMD\"",
        "  TEST_CMD_RC=$?",
        "else",
        "  bash -c \"$TEST_CMD\"",
        "  TEST_CMD_RC=$?",
        "fi",
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
print(f"  CPU type: KVM -> {cpu_type_map[args.cpu_type].name}")
print(f"  Kernel: {args.kernel}")
print(f"  Disk: {args.disk}")
print(f"  Test command: {args.test_cmd}")
print(f"  Effective command: {effective_test_cmd}")
print(f"  RPC clients: {requested_rpc_clients}")
print(
    "  CopyEngine topology: "
    f"{num_copy_engines} engine(s) x {copy_engine_channels} channel(s)"
)
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
