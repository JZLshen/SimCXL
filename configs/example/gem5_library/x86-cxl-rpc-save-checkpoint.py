"""
CXL RPC Checkpoint Save

Boot Linux with KVM, save checkpoint in KVM state (before CPU switch).
This allows checkpoint restore to switch to the supported post-boot CPU
types (TIMING/O3/KVM).

Checkpoint is saved after boot completes but BEFORE CPU switch, so:
  - save-checkpoint: KVM boot -> save checkpoint (KVM state)
  - test.py restore: restore checkpoint (KVM state) -> switch CPU -> run test

Usage:
    ./build/X86/gem5.opt -d output/cxl_rpc_checkpoint \\
        configs/example/gem5_library/x86-cxl-rpc-save-checkpoint.py

    # Then run tests from checkpoint:
    ./build/X86/gem5.opt \\
        configs/example/gem5_library/x86-cxl-rpc-test.py \\
        --checkpoint output/cxl_rpc_checkpoint \\
        --test_cmd "/home/test_code/cxl_rpc_benchmark --requests 50 --warmup 10 --msg-size 64 --max-polls 2000000"
"""

import argparse
import os
import time
import m5
from pathlib import Path
from gem5.utils.requires import requires
from gem5.components.boards.x86_board import X86Board
from gem5.components.memory.single_channel import DIMM_DDR5_4400
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.cachehierarchies.classic.\
    private_l1_private_l2_shared_l3_cache_hierarchy import (
    PrivateL1PrivateL2SharedL3CacheHierarchy,
)
from gem5.isas import ISA
from gem5.simulate.simulator import Simulator
from gem5.simulate.exit_event import ExitEvent

from gem5.resources.resource import DiskImageResource, KernelResource

requires(isa_required=ISA.X86)

repo_root = Path(__file__).resolve().parents[3]
default_kernel = repo_root / "files" / "vmlinux"
default_disk = repo_root / "files" / "parsec.img"
MAX_COPY_ENGINES = 29
MAX_COPY_ENGINE_CHANNELS = 64

parser = argparse.ArgumentParser(description='Save CXL RPC boot checkpoint.')
parser.add_argument('--num_cpus', type=int, default=1, help='Number of CPUs')
parser.add_argument(
    '--rpc_client_count',
    type=int,
    default=0,
    help=('RPC client count encoded into the checkpoint hardware topology. '
          '0 means auto: max(1, num_cpus - 1).'),
)
parser.add_argument(
    '--copy_engine_channels',
    type=int,
    default=0,
    help=('Channels per CopyEngine. 0 auto-derives the minimum channel count '
          'needed to provide at least one response lane per client.'),
)
parser.add_argument(
    '--copy_engine_xfercap',
    type=str,
    default='4KiB',
    help='Per-CopyEngine maximum transfer size advertised through XFERCAP.',
)
parser.add_argument('--is_asic', type=str, choices=['True', 'False'],
                    default='True', help='ASIC or FPGA device.')
parser.add_argument(
    '--fallback_sim_seconds',
    type=int,
    default=90,
    help=('Fail checkpoint build after this many simulated seconds if the '
          'guest readfile path never emits the handoff m5 exit. '
          'Set 0 to disable.'),
)
parser.add_argument(
    '--run_step_ticks',
    type=int,
    default=5_000_000_000_000,
    help=('Maximum ticks per simulator.run() chunk while waiting for trigger '
          'or settle completion.'),
)
parser.add_argument('--kernel', type=str, default=str(default_kernel),
                    help='Path to Linux kernel image')
parser.add_argument('--disk', type=str, default=str(default_disk),
                    help='Path to guest disk image')

args = parser.parse_args()

if args.rpc_client_count < 0:
    parser.error('--rpc_client_count must be >= 0')
if not os.access('/dev/kvm', os.R_OK | os.W_OK):
    parser.error(
        'x86-cxl-rpc-save-checkpoint.py requires read/write access to '
        '/dev/kvm on the host'
    )


def ceil_div(numer: int, denom: int) -> int:
    return (numer + denom - 1) // denom


def derive_copyengine_topology(
    requested_clients: int,
    requested_channels: int,
) -> tuple[int, int]:
    if requested_clients < 1:
        requested_clients = 1
    if requested_channels < 0:
        parser.error('--copy_engine_channels must be >= 0')

    channels = requested_channels
    if channels == 0:
        channels = max(1, ceil_div(requested_clients, MAX_COPY_ENGINES))

    if channels > MAX_COPY_ENGINE_CHANNELS:
        parser.error(
            '--copy_engine_channels exceeds board limit: '
            f'{channels} > {MAX_COPY_ENGINE_CHANNELS}'
        )

    num_engines = ceil_div(requested_clients, channels)
    if num_engines > MAX_COPY_ENGINES:
        max_clients = MAX_COPY_ENGINES * channels
        parser.error(
            'requested rpc_client_count exceeds current X86Board lane '
            f'capacity: clients={requested_clients}, '
            f'channels/engine={channels}, '
            f'max_supported_clients={max_clients}'
        )

    return num_engines, channels


requested_rpc_clients = (
    args.rpc_client_count if args.rpc_client_count > 0 else max(1, args.num_cpus - 1)
)
num_copy_engines, copy_engine_channels = derive_copyengine_topology(
    requested_rpc_clients,
    args.copy_engine_channels,
)

# --- Board/memory/cache config MUST match x86-cxl-rpc-test.py exactly ---

cache_hierarchy = PrivateL1PrivateL2SharedL3CacheHierarchy(
    l1d_size="48kB", l1d_assoc=6,
    l1i_size="32kB", l1i_assoc=8,
    l2_size="2MB", l2_assoc=16,
    l3_size="96MB", l3_assoc=48,
)

memory = DIMM_DDR5_4400(size="3GB")
cxl_dram = DIMM_DDR5_4400(size="8GB")

# Use TIMING as switch target - checkpoint is saved in KVM state,
# so this doesn't affect the checkpoint. Restore can use TIMING/O3/KVM.
processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=args.num_cpus,
)

for proc in processor.start:
    proc.core.usePerf = False

board = X86Board(
    clk_freq="2.4GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    cxl_memory=cxl_dram,
    is_asic=(args.is_asic == 'True'),
    num_copy_engines=num_copy_engines,
    copy_engine_channels=copy_engine_channels,
    copy_engine_xfercap=args.copy_engine_xfercap,
)

# Board-level wiring already handles IDE/CXL DMA and response ports.
# Keep config-side wiring minimal to avoid duplicate-port fatals.

# Boot with KVM and wait for guest SMP bring-up before checkpoint.
# If checkpoint is taken too early, only CPU0 may be online after restore,
# which breaks server/client co-run workflows. After the handoff EXIT is
# restored, this same script performs exactly one additional m5 readfile to
# fetch the real test payload and exec it.
checkpoint_readfile = f"""
set -u
CKPT_BOOTSTRAP_MAGIC="CXL_RPC_CKPT_BOOTSTRAP_V2"
TRIES=300
while [ "$TRIES" -gt 0 ]; do
  NPROC="$(nproc 2>/dev/null || echo 1)"
  if [ "$NPROC" -ge "{args.num_cpus}" ]; then
    break
  fi
  TRIES=$((TRIES - 1))
  sleep 1
done
TRIES=300
while [ "$TRIES" -gt 0 ]; do
  SYS_STATE="$(systemctl is-system-running 2>/dev/null || true)"
  if systemctl is-active --quiet multi-user.target; then
    if [ "$SYS_STATE" = "running" ] || [ "$SYS_STATE" = "degraded" ]; then
      break
    fi
  fi
  TRIES=$((TRIES - 1))
  sleep 1
done
m5 exit

RESTORE_SCRIPT=/tmp/gem5_restore_script.sh
RESTORE_TMP=/tmp/gem5_restore_script.sh.new
RESTORE_ATTEMPTS=3
while [ "$RESTORE_ATTEMPTS" -gt 0 ]; do
  /sbin/m5 readfile > "$RESTORE_TMP" 2>/dev/null || true
  if [ ! -s "$RESTORE_TMP" ]; then
    rm -f "$RESTORE_TMP"
    break
  fi
  if grep -q "$CKPT_BOOTSTRAP_MAGIC" "$RESTORE_TMP" 2>/dev/null; then
    rm -f "$RESTORE_TMP"
    RESTORE_ATTEMPTS=$((RESTORE_ATTEMPTS - 1))
    continue
  fi
  mv -f "$RESTORE_TMP" "$RESTORE_SCRIPT"
  chmod +x "$RESTORE_SCRIPT"
  exec /bin/bash "$RESTORE_SCRIPT"
done
rm -f "$RESTORE_TMP"
m5 exit
"""

# Checkpoint saves KVM state, restore will switch CPU
board.set_kernel_disk_workload(
    kernel=KernelResource(local_path=args.kernel),
    disk_image=DiskImageResource(local_path=args.disk),
    kernel_args=[
        "earlyprintk=ttyS0",
        "console=ttyS0",
        "lpj=7999923",
        "root={root_value}",
        "idle=poll",
        "iomem=relaxed",
    ],
    readfile_contents=checkpoint_readfile,
)

# Event handlers
checkpoint_dir = Path(m5.options.outdir)
checkpoint_trigger = {"tick": None, "events": 0, "saved": False}
run_step_ticks = max(1, args.run_step_ticks)
status_wall_interval_sec = 15.0
last_status_wall = {"pre_exit": 0.0}

def checkpoint_handler():
    """Observe the guest handoff EXIT and save immediately.

    The bootstrap readfile script resumes after restore and performs exactly
    one follow-up m5 readfile. Saving from the first observed EXIT keeps that
    handoff intact and guarantees simulator.run() returns immediately.
    """
    while True:
        checkpoint_trigger["events"] += 1
        if checkpoint_trigger["saved"]:
            print(
                "Checkpoint trigger observed after save; stopping run "
                f"event_count={checkpoint_trigger['events']} curTick={m5.curTick()}"
            )
            yield True
            continue

        checkpoint_trigger["tick"] = m5.curTick()
        print(
            "Checkpoint trigger observed; saving immediately to preserve "
            f"one-shot restore readfile handoff curTick={m5.curTick()}"
        )
        save_checkpoint("guest-exit-readfile-handoff")
        checkpoint_trigger["saved"] = True
        yield True

def save_checkpoint(reason: str):
    print(f"Saving checkpoint to {checkpoint_dir}...")
    print(f"  Save reason: {reason}")
    print(f"  Save curTick: {m5.curTick()}")
    m5.checkpoint(str(checkpoint_dir))
    print("Checkpoint saved successfully!")
    print(f"  Location: {checkpoint_dir}")
    print("  State: KVM (bootstrap script paused at handoff EXIT)")
    print("  Restore can use: TIMING, O3, or KVM")

simulator = Simulator(
    board=board,
    on_exit_event={
        ExitEvent.EXIT: checkpoint_handler(),
    },
)

print("=== CXL RPC Checkpoint Save ===")
print(f"  CPUs: {args.num_cpus}")
print("  Mode: CPU-to-CPU")
print(f"  Kernel: {args.kernel}")
print(f"  Disk: {args.disk}")
print(f"  Checkpoint will be saved in KVM state (before CPU switch)")
print("  This allows restore with TIMING, O3, or KVM")
print(f"  Fallback sim-seconds: {args.fallback_sim_seconds}")
print("  Save policy: immediate at guest handoff EXIT")
print(f"  Run step ticks: {run_step_ticks}")
print(
    "  CopyEngine topology: "
    f"{num_copy_engines} engine(s) x {copy_engine_channels} channel(s)"
)

fallback_ticks = max(0, args.fallback_sim_seconds) * 1_000_000_000_000

while True:
    now_wall = time.monotonic()

    if checkpoint_trigger["saved"]:
        break

    if fallback_ticks != 0:
        remaining = fallback_ticks - m5.curTick()
        if remaining <= 0:
            print("ERROR: checkpoint handoff EXIT was not observed before "
                  f"fallback deadline at tick {m5.curTick()}.")
            print("ERROR: refusing to save a fallback checkpoint because it "
                  "would not represent the intended handoff state.")
            raise SystemExit(2)
        if now_wall - last_status_wall["pre_exit"] >= status_wall_interval_sec:
            print(
                "[ckpt-host] waiting for guest EXIT "
                f"curTick={m5.curTick()} fallback_remaining_ticks={remaining} "
                f"next_step_ticks={min(run_step_ticks, remaining)}"
            )
            last_status_wall["pre_exit"] = now_wall
        simulator.run(max_ticks=min(run_step_ticks, remaining))
    else:
        if now_wall - last_status_wall["pre_exit"] >= status_wall_interval_sec:
            print(
                "[ckpt-host] waiting for guest EXIT "
                f"curTick={m5.curTick()} next_step_ticks={run_step_ticks}"
            )
            last_status_wall["pre_exit"] = now_wall
        simulator.run(max_ticks=run_step_ticks)

print(f"  Use with: --checkpoint {checkpoint_dir}")
