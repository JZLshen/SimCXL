#!/usr/bin/env python3
"""
Run MICA-like application-path CXL RPC experiments with:
  - KVM boot + TIMING CPU test phase
  - guest binary injection via setup_disk_image.sh
  - checkpoint reuse by hardware topology
  - raw client latency and server breakdown extraction

Default experiment scope follows the current documented application-path
workload set:
  - fixed 32 clients
  - YCSB-A 1KB KV workload
  - YCSB-B 1KB KV workload
  - YCSB-C 1KB KV workload
  - YCSB-F 1KB KV workload
  - UDB-like read-only 27B/127B KV workload
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import os
import shlex
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import run_rpc_matrix_kvm_timing_ckpt as bare_rpc


DEFAULT_CLIENTS = 32
DEFAULT_REQUESTS = 30
DEFAULT_MQ_ENTRIES = bare_rpc.DEFAULT_MQ_ENTRIES
DEFAULT_RESPONSE_DMA_THRESHOLD = bare_rpc.DEFAULT_RESPONSE_DMA_THRESHOLD
DEFAULT_PREFETCH_MODE = bare_rpc.DEFAULT_PREFETCH_MODE
DEFAULT_RECORD_COUNT = 10000
DEFAULT_DATASET_SEED = 0x9B5D3A4781C26EF1
DEFAULT_WORKLOAD_SEED = 0xC7D51A32049EF68B
DEFAULT_PROFILES = ("ycsb_a_1k", "ycsb_b_1k", "ycsb_c_1k", "ycsb_f_1k", "udb_ro")


@dataclass(frozen=True)
class ProfileConfig:
    name: str
    key_size: int
    value_size: int
    size_mode: str
    key_dist: str
    read_ratio: float
    update_ratio: float
    rmw_ratio: float
    zipf_theta: float


PROFILE_CONFIGS: Dict[str, ProfileConfig] = {
    "ycsb_c_1k": ProfileConfig(
        name="ycsb_c_1k",
        key_size=16,
        value_size=1024,
        size_mode="fixed",
        key_dist="zipf",
        read_ratio=1.0,
        update_ratio=0.0,
        rmw_ratio=0.0,
        zipf_theta=0.99,
    ),
    "ycsb_1k_ro": ProfileConfig(
        name="ycsb_c_1k",
        key_size=16,
        value_size=1024,
        size_mode="fixed",
        key_dist="zipf",
        read_ratio=1.0,
        update_ratio=0.0,
        rmw_ratio=0.0,
        zipf_theta=0.99,
    ),
    "ycsb_a_1k": ProfileConfig(
        name="ycsb_a_1k",
        key_size=16,
        value_size=1024,
        size_mode="fixed",
        key_dist="zipf",
        read_ratio=0.5,
        update_ratio=0.5,
        rmw_ratio=0.0,
        zipf_theta=0.99,
    ),
    "ycsb_b_1k": ProfileConfig(
        name="ycsb_b_1k",
        key_size=16,
        value_size=1024,
        size_mode="fixed",
        key_dist="zipf",
        read_ratio=0.95,
        update_ratio=0.05,
        rmw_ratio=0.0,
        zipf_theta=0.99,
    ),
    "ycsb_f_1k": ProfileConfig(
        name="ycsb_f_1k",
        key_size=16,
        value_size=1024,
        size_mode="fixed",
        key_dist="zipf",
        read_ratio=0.0,
        update_ratio=0.0,
        rmw_ratio=1.0,
        zipf_theta=0.99,
    ),
    "udb_ro": ProfileConfig(
        name="udb_ro",
        key_size=27,
        value_size=127,
        size_mode="variable",
        key_dist="uniform",
        read_ratio=1.0,
        update_ratio=0.0,
        rmw_ratio=0.0,
        zipf_theta=0.0,
    ),
}


@dataclass(frozen=True)
class ExperimentKey:
    profile: str
    clients: int
    requests_per_client: int
    record_count: int
    key_size: int
    value_size: int
    size_mode: str
    key_dist: str
    read_ratio: float
    update_ratio: float
    rmw_ratio: float
    zipf_theta: float
    dataset_seed: int = DEFAULT_DATASET_SEED
    workload_seed: int = DEFAULT_WORKLOAD_SEED
    mq_entries: int = DEFAULT_MQ_ENTRIES
    head_sync_threshold: int = bare_rpc.DEFAULT_MQ_ENTRIES // 4
    cxl_extra_latency_ns: int = 0
    response_lane_count: int = 0
    clients_per_dma_lane: int = 1
    response_dma_threshold: int = DEFAULT_RESPONSE_DMA_THRESHOLD
    prefetch_mode: str = DEFAULT_PREFETCH_MODE


@dataclass
class Experiment:
    key: ExperimentKey
    exp_id: str
    source: str


def parse_profiles_arg(raw: str) -> List[str]:
    if not raw:
        return list(DEFAULT_PROFILES)

    profiles = [item.strip() for item in raw.split(",") if item.strip()]
    if not profiles:
        return list(DEFAULT_PROFILES)

    invalid = [item for item in profiles if item not in PROFILE_CONFIGS]
    if invalid:
        raise ValueError(
            "invalid profile(s): "
            f"{', '.join(invalid)}; valid={','.join(sorted(PROFILE_CONFIGS))}"
        )

    return profiles


def format_float_token(value: float) -> str:
    text = f"{value:.3f}".rstrip("0").rstrip(".")
    return text.replace(".", "p")


def format_zipf_field(key_dist: str, zipf_theta: float) -> str:
    if key_dist != "zipf":
        return ""
    return f"{zipf_theta:.6f}"


def format_exp_id(key: ExperimentKey) -> str:
    parts = [
        key.profile,
        f"c{key.clients}",
        f"r{key.requests_per_client}",
        f"rec{key.record_count}",
    ]
    if key.mq_entries != DEFAULT_MQ_ENTRIES:
        parts.append(f"mq{key.mq_entries}")
    if key.head_sync_threshold != bare_rpc.default_head_sync_threshold(
        key.mq_entries
    ):
        parts.append(f"hs{key.head_sync_threshold}")
    if key.cxl_extra_latency_ns > 0:
        parts.append(f"cxlp{key.cxl_extra_latency_ns}ns")
    if key.clients_per_dma_lane != 1:
        parts.append(f"cpl{key.clients_per_dma_lane}")
    if key.response_dma_threshold != DEFAULT_RESPONSE_DMA_THRESHOLD:
        parts.append(f"dmath{key.response_dma_threshold}")
    if key.prefetch_mode != DEFAULT_PREFETCH_MODE:
        parts.append(f"pf{key.prefetch_mode.replace('-', '_')}")
    if key.dataset_seed != DEFAULT_DATASET_SEED:
        parts.append(f"dseed{key.dataset_seed:x}")
    if key.workload_seed != DEFAULT_WORKLOAD_SEED:
        parts.append(f"wseed{key.workload_seed:x}")
    return "_".join(parts)


def build_matrix(
    profiles: List[str],
    clients: int,
    requests_per_client: int,
    record_count: int,
    mq_entries: int,
    head_sync_threshold: Optional[int],
    cxl_extra_latency_ns: int,
    clients_per_dma_lane: int,
    response_dma_threshold: int,
    prefetch_mode: str,
    dataset_seed: int,
    workload_seed: int,
) -> List[Experiment]:
    experiments: List[Experiment] = []
    response_lane_count = bare_rpc.ceil_div(clients, clients_per_dma_lane)
    effective_head_sync_threshold = (
        bare_rpc.default_head_sync_threshold(mq_entries)
        if head_sync_threshold is None
        else head_sync_threshold
    )

    for profile_name in profiles:
        profile = PROFILE_CONFIGS[profile_name]
        key = ExperimentKey(
            profile=profile.name,
            clients=clients,
            requests_per_client=requests_per_client,
            record_count=record_count,
            key_size=profile.key_size,
            value_size=profile.value_size,
            size_mode=profile.size_mode,
            key_dist=profile.key_dist,
            read_ratio=profile.read_ratio,
            update_ratio=profile.update_ratio,
            rmw_ratio=profile.rmw_ratio,
            zipf_theta=profile.zipf_theta,
            dataset_seed=dataset_seed,
            workload_seed=workload_seed,
            mq_entries=mq_entries,
            head_sync_threshold=effective_head_sync_threshold,
            cxl_extra_latency_ns=cxl_extra_latency_ns,
            response_lane_count=response_lane_count,
            clients_per_dma_lane=clients_per_dma_lane,
            response_dma_threshold=response_dma_threshold,
            prefetch_mode=prefetch_mode,
        )
        experiments.append(
            Experiment(
                key=key,
                exp_id=format_exp_id(key),
                source="application",
            )
        )

    return experiments


def experiment_metadata_row(key: ExperimentKey) -> Dict[str, object]:
    return {
        "profile": key.profile,
        "clients": key.clients,
        "requests_per_client": key.requests_per_client,
        "record_count": key.record_count,
        "key_size": key.key_size,
        "value_size": key.value_size,
        "size_mode": key.size_mode,
        "key_dist": key.key_dist,
        "read_ratio": f"{key.read_ratio:.6f}",
        "update_ratio": f"{key.update_ratio:.6f}",
        "rmw_ratio": f"{key.rmw_ratio:.6f}",
        "zipf_theta": format_zipf_field(key.key_dist, key.zipf_theta),
        "dataset_seed": str(key.dataset_seed),
        "workload_seed": str(key.workload_seed),
        "mq_entries": key.mq_entries,
        "head_sync_threshold": key.head_sync_threshold,
        "cxl_extra_latency_ns": key.cxl_extra_latency_ns,
        "response_lane_count": key.response_lane_count,
        "clients_per_dma_lane": key.clients_per_dma_lane,
        "response_dma_threshold": key.response_dma_threshold,
        "prefetch_mode": key.prefetch_mode,
    }


def profile_arg_list(key: ExperimentKey) -> List[str]:
    args = [
        f"--profile {key.profile}",
        f"--record-count {key.record_count}",
        f"--dataset-seed {key.dataset_seed}",
    ]
    if key.size_mode == "fixed":
        args.extend(
            [
                f"--key-size {key.key_size}",
                f"--value-size {key.value_size}",
            ]
        )
    return args


def workload_arg_list(key: ExperimentKey) -> List[str]:
    args = [
        f"--workload-seed {key.workload_seed}",
        f"--read-ratio {key.read_ratio:.6f}",
        f"--update-ratio {key.update_ratio:.6f}",
        f"--rmw-ratio {key.rmw_ratio:.6f}",
    ]
    if key.key_dist == "zipf":
        args.append(f"--zipf-theta {key.zipf_theta:.6f}")
    return args


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run CXL RPC application matrix (KVM+TIMING+checkpoint)."
    )
    parser.add_argument("--repo-root", type=str, default=None)
    parser.add_argument("--output-base", type=str, default="output")
    parser.add_argument("--batch-name", type=str, default="")
    parser.add_argument(
        "--disk",
        type=str,
        default="",
        help="Path to guest disk image. Empty uses repo_root/files/parsec.img.",
    )
    parser.add_argument(
        "--checkpoint-dir",
        type=str,
        default="",
        help=(
            "Reuse this existing checkpoint for every experiment in the batch. "
            "Caller is responsible for ensuring topology compatibility."
        ),
    )
    parser.add_argument("--profiles", type=str, default=",".join(DEFAULT_PROFILES))
    parser.add_argument("--clients", type=int, default=DEFAULT_CLIENTS)
    parser.add_argument("--requests", type=int, default=DEFAULT_REQUESTS)
    parser.add_argument("--record-count", type=int, default=DEFAULT_RECORD_COUNT)
    parser.add_argument("--dataset-seed", type=lambda x: int(x, 0),
                        default=DEFAULT_DATASET_SEED)
    parser.add_argument("--workload-seed", type=lambda x: int(x, 0),
                        default=DEFAULT_WORKLOAD_SEED)
    parser.add_argument("--mq-entries", type=int, default=DEFAULT_MQ_ENTRIES)
    parser.add_argument("--head-sync-threshold", type=int, default=None)
    parser.add_argument("--cxl-extra-latency-ns", type=int, default=0)
    parser.add_argument("--clients-per-dma-lane", type=int, default=1)
    parser.add_argument(
        "--response-dma-threshold",
        type=int,
        default=DEFAULT_RESPONSE_DMA_THRESHOLD,
    )
    parser.add_argument(
        "--prefetch-mode",
        type=str,
        default=DEFAULT_PREFETCH_MODE,
        help="Server request-RX prefetch mode: full|no-request|none.",
    )
    parser.add_argument("--only-exp-id", type=str, default="")
    parser.add_argument("--start-index", type=int, default=1)
    parser.add_argument("--end-index", type=int, default=0)
    parser.add_argument("--copy-engine-channels", type=int, default=0)
    parser.add_argument(
        "--checkpoint-handoff-deadline-sim-seconds",
        type=int,
        default=0,
    )
    parser.add_argument(
        "--boot-cpu-type",
        type=str,
        choices=["KVM", "TIMING", "ATOMIC"],
        default="KVM",
        help=(
            "CPU type used during checkpoint creation and pre-test restore. "
            "TIMING/ATOMIC avoid /dev/kvm but are much slower than KVM."
        ),
    )
    parser.add_argument("--skip-inject", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--force-rerun", action="store_true")
    parser.add_argument("--allow-concurrent-runs", action="store_true")
    parser.add_argument("--inter-experiment-sleep-sec", type=int, default=30)
    args = parser.parse_args()

    try:
        profiles = parse_profiles_arg(args.profiles)
    except ValueError as exc:
        print(f"[fatal] {exc}")
        return 2

    if args.start_index < 1:
        print("[fatal] --start-index must be >= 1")
        return 2
    if args.end_index < 0:
        print("[fatal] --end-index must be >= 0")
        return 2
    if args.end_index != 0 and args.end_index < args.start_index:
        print("[fatal] --end-index must be >= --start-index when set")
        return 2
    if args.clients < 1:
        print("[fatal] --clients must be >= 1")
        return 2
    if args.requests < 1:
        print("[fatal] --requests must be >= 1")
        return 2
    if args.record_count < 1:
        print("[fatal] --record-count must be >= 1")
        return 2
    if args.mq_entries < 1 or args.mq_entries > bare_rpc.DEFAULT_MQ_ENTRIES:
        print(
            f"[fatal] --mq-entries must be in 1..{bare_rpc.DEFAULT_MQ_ENTRIES}"
        )
        return 2
    if (
        args.head_sync_threshold is not None and
        (args.head_sync_threshold < 0 or
         args.head_sync_threshold > args.mq_entries)
    ):
        print("[fatal] --head-sync-threshold must be in 0..mq-entries")
        return 2
    if args.cxl_extra_latency_ns < 0:
        print("[fatal] --cxl-extra-latency-ns must be >= 0")
        return 2
    if args.clients_per_dma_lane < 1 or args.clients_per_dma_lane > args.clients:
        print("[fatal] --clients-per-dma-lane must be in 1..clients")
        return 2
    if args.response_dma_threshold < 1:
        print("[fatal] --response-dma-threshold must be >= 1")
        return 2
    if args.prefetch_mode not in {"full", "no-request", "none"}:
        print("[fatal] --prefetch-mode must be one of: full, no-request, none")
        return 2
    if args.copy_engine_channels < 0:
        print("[fatal] --copy-engine-channels must be >= 0")
        return 2
    if args.checkpoint_handoff_deadline_sim_seconds < 0:
        print("[fatal] --checkpoint-handoff-deadline-sim-seconds must be >= 0")
        return 2
    if args.inter_experiment_sleep_sec < 0:
        print("[fatal] --inter-experiment-sleep-sec must be >= 0")
        return 2

    repo_root = bare_rpc.resolve_repo_root(args.repo_root)
    output_base = (repo_root / args.output_base).resolve()
    batch_name = args.batch_name or f"rpc_app_matrix_kvm_timing_ckpt_{bare_rpc.now_tag()}"
    batch_dir = output_base / batch_name
    batch_dir.mkdir(parents=True, exist_ok=True)

    gem5_bin = repo_root / "build/X86/gem5.opt"
    test_cfg = repo_root / "configs/example/gem5_library/x86-cxl-rpc-test.py"
    save_ckpt_cfg = (
        repo_root / "configs/example/gem5_library/x86-cxl-rpc-save-checkpoint.py"
    )
    inject_script = repo_root / "tests/test-progs/cxl-rpc/scripts/setup_disk_image.sh"
    summary_script = (
        repo_root / "tests/test-progs/cxl-rpc/scripts/summarize_rpc_app_results.py"
    )
    disk_img = (
        Path(args.disk).resolve()
        if args.disk else
        (repo_root / "files" / "parsec.img").resolve()
    )
    external_checkpoint_dir: Optional[Path] = None
    if args.checkpoint_dir:
        external_checkpoint_dir = bare_rpc.resolve_checkpoint_dir(
            Path(args.checkpoint_dir).resolve()
        )
        if external_checkpoint_dir is None:
            print(
                f"[fatal] --checkpoint-dir does not resolve to a checkpoint: "
                f"{args.checkpoint_dir}"
            )
            return 2

    experiments = build_matrix(
        profiles=profiles,
        clients=args.clients,
        requests_per_client=args.requests,
        record_count=args.record_count,
        mq_entries=args.mq_entries,
        head_sync_threshold=args.head_sync_threshold,
        cxl_extra_latency_ns=args.cxl_extra_latency_ns,
        clients_per_dma_lane=args.clients_per_dma_lane,
        response_dma_threshold=args.response_dma_threshold,
        prefetch_mode=args.prefetch_mode,
        dataset_seed=args.dataset_seed,
        workload_seed=args.workload_seed,
    )
    if args.only_exp_id:
        experiments = [exp for exp in experiments if exp.exp_id == args.only_exp_id]
        if not experiments:
            print(f"[fatal] no experiment matched --only-exp-id={args.only_exp_id}")
            return 2

    max_clients = max(exp.key.clients for exp in experiments)
    max_required_cpus = max_clients + 1

    plan_csv = batch_dir / "plan.csv"
    with plan_csv.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "exp_id",
                "source",
                "profile",
                "clients",
                "requests_per_client",
                "record_count",
                "key_size",
                "value_size",
                "size_mode",
                "key_dist",
                "read_ratio",
                "update_ratio",
                "rmw_ratio",
                "zipf_theta",
                "dataset_seed",
                "workload_seed",
                "mq_entries",
                "head_sync_threshold",
                "cxl_extra_latency_ns",
                "response_lane_count",
                "clients_per_dma_lane",
                "response_dma_threshold",
                "prefetch_mode",
            ],
        )
        writer.writeheader()
        for exp in experiments:
            writer.writerow(
                {
                    "exp_id": exp.exp_id,
                    "source": exp.source,
                    **experiment_metadata_row(exp.key),
                }
            )

    print(f"[app-matrix] total experiments: {len(experiments)}")
    print(f"[app-matrix] profiles: {', '.join(profiles)}")
    print(f"[app-matrix] max required cores (server+clients): {max_required_cpus}")
    print(f"[app-matrix] batch dir: {batch_dir}")

    experiments_csv = batch_dir / "experiments.csv"
    ticks_csv = batch_dir / "results_ticks.csv"
    server_ticks_csv = batch_dir / "results_server_ticks.csv"
    done_ids = set()
    if not args.force_rerun:
        done_ids = bare_rpc.read_success_exp_ids(experiments_csv)

    exp_fields = [
        "exp_id",
        "source",
        "profile",
        "clients",
        "requests_per_client",
        "record_count",
        "key_size",
        "value_size",
        "size_mode",
        "key_dist",
        "read_ratio",
        "update_ratio",
        "rmw_ratio",
        "zipf_theta",
        "dataset_seed",
        "workload_seed",
        "mq_entries",
        "head_sync_threshold",
        "cxl_extra_latency_ns",
        "response_lane_count",
        "clients_per_dma_lane",
        "response_dma_threshold",
        "prefetch_mode",
        "checkpoint_dir",
        "num_cpus",
        "output_dir",
        "start_time",
        "end_time",
        "elapsed_sec",
        "gem5_rc",
        "test_cmd_exit",
        "tick_rows",
        "expected_rows",
        "server_tick_rows",
        "expected_server_rows",
        "status",
    ]
    tick_fields = [
        "exp_id",
        "profile",
        "clients",
        "requests_per_client",
        "record_count",
        "key_size",
        "value_size",
        "size_mode",
        "key_dist",
        "read_ratio",
        "update_ratio",
        "rmw_ratio",
        "zipf_theta",
        "dataset_seed",
        "workload_seed",
        "mq_entries",
        "head_sync_threshold",
        "cxl_extra_latency_ns",
        "response_lane_count",
        "clients_per_dma_lane",
        "response_dma_threshold",
        "prefetch_mode",
        "node_id",
        "req_index",
        "start_tick",
        "end_tick",
        "output_dir",
    ]
    server_tick_fields = [
        "exp_id",
        "profile",
        "clients",
        "requests_per_client",
        "record_count",
        "key_size",
        "value_size",
        "size_mode",
        "key_dist",
        "read_ratio",
        "update_ratio",
        "rmw_ratio",
        "zipf_theta",
        "dataset_seed",
        "workload_seed",
        "mq_entries",
        "head_sync_threshold",
        "cxl_extra_latency_ns",
        "response_lane_count",
        "clients_per_dma_lane",
        "response_dma_threshold",
        "prefetch_mode",
        "server_req_index",
        "poll_tick",
        "execute_tick",
        "response_tick",
        "output_dir",
    ]

    started_experiments = 0
    running_as_root = hasattr(os, "geteuid") and os.geteuid() == 0
    boot_requires_kvm = args.boot_cpu_type == "KVM"
    direct_kvm_access = os.access("/dev/kvm", os.R_OK | os.W_OK)
    sudo_n_available = bare_rpc.sudo_nopass_available()
    kvm_cmd_prefix: List[str] = []
    checkpoint_cache: Dict[Tuple[str, int, int, int, int], Path] = {}
    checkpoint_failure_cache: Dict[Tuple[str, int, int, int, int], Tuple[str, int]] = {}
    kvm_signal_prefix: Optional[List[str]] = None
    lock_fp = None

    if not boot_requires_kvm:
        print(
            f"[app-matrix] boot CPU type is {args.boot_cpu_type}; "
            "/dev/kvm not required"
        )
    elif direct_kvm_access:
        print("[app-matrix] /dev/kvm is directly accessible by current user")
    else:
        print("[app-matrix] /dev/kvm is not directly accessible by current user")
        if running_as_root:
            print("[app-matrix] current user is root; continuing without `sudo -n`")
        else:
            if not sudo_n_available:
                print("[fatal] /dev/kvm requires elevated access, but `sudo -n` is unavailable")
                return 2
            kvm_cmd_prefix = ["sudo", "-n"]
            kvm_signal_prefix = kvm_cmd_prefix
            print("[app-matrix] checkpoint builds will run under `sudo -n`")

    if not args.allow_concurrent_runs:
        lock_path = output_base / ".rpc_matrix.lock"
        lock_fp = bare_rpc.acquire_matrix_lock(lock_path, batch_name)
        if lock_fp is None:
            return 2
        print(f"[app-matrix] acquired wrapper lock: {lock_path}")
    else:
        print("[app-matrix] concurrent wrapper runs allowed; skipping exclusive lock")

    try:
        if not args.skip_inject:
            inject_log = batch_dir / "inject.log"
            inject_cmd = ["bash", str(inject_script), str(disk_img)]
            if not args.dry_run:
                try:
                    rc = bare_rpc.run_and_tee(inject_cmd, inject_log)
                except RuntimeError as exc:
                    print(f"[fatal] {exc}")
                    return 2
                if rc != 0:
                    print(f"[fatal] inject failed rc={rc}")
                    return rc
            else:
                print("[dry-run] skip inject execution")

        for idx, exp in enumerate(experiments, start=1):
            key = exp.key
            if idx < args.start_index:
                print(
                    f"[skip {idx}/{len(experiments)}] "
                    f"before --start-index={args.start_index}: {exp.exp_id}"
                )
                continue
            if args.end_index != 0 and idx > args.end_index:
                print(
                    f"[stop] reached --end-index={args.end_index}; ending batch run"
                )
                break
            if exp.exp_id in done_ids:
                print(f"[skip {idx}/{len(experiments)}] already done: {exp.exp_id}")
                continue

            if started_experiments > 0 and args.inter_experiment_sleep_sec > 0:
                if args.dry_run:
                    print(
                        f"[dry-run] would sleep {args.inter_experiment_sleep_sec}s "
                        f"before starting next experiment: {exp.exp_id}"
                    )
                else:
                    print(
                        f"[app-matrix] sleeping {args.inter_experiment_sleep_sec}s "
                        f"before starting next experiment: {exp.exp_id}"
                    )
                    time.sleep(args.inter_experiment_sleep_sec)
            started_experiments += 1

            run_outdir = batch_dir / f"run_{idx:02d}_{exp.exp_id}"
            run_outdir.mkdir(parents=True, exist_ok=True)
            run_log = run_outdir / "gem5_run.log"
            required_cpus = key.clients + 1
            checkpoint_key = (
                args.boot_cpu_type,
                key.clients,
                key.response_lane_count,
                key.mq_entries,
                key.cxl_extra_latency_ns,
            )
            checkpoint_label = (
                f"boot_{args.boot_cpu_type.lower()}"
                f"_"
                f"clients_{key.clients}"
                f"_lanes_{key.response_lane_count}"
                f"_mq_{key.mq_entries}"
                f"_cxlp_{key.cxl_extra_latency_ns}ns"
            )
            if external_checkpoint_dir is not None:
                checkpoint_dir = external_checkpoint_dir
                checkpoint_failure = None
                ckpt_outdir = external_checkpoint_dir
            else:
                checkpoint_dir = checkpoint_cache.get(checkpoint_key)
                checkpoint_failure = checkpoint_failure_cache.get(checkpoint_key)
                ckpt_outdir = (
                    batch_dir / "checkpoints" / checkpoint_label /
                    "cxl_rpc_app_checkpoint"
                )

            if external_checkpoint_dir is None and checkpoint_dir is None:
                if checkpoint_failure is not None:
                    failed_status, failed_rc = checkpoint_failure
                    cached_status = f"{failed_status}_cached"
                    bare_rpc.append_row(
                        experiments_csv,
                        {
                            "exp_id": exp.exp_id,
                            "source": exp.source,
                            **experiment_metadata_row(key),
                            "checkpoint_dir": str(ckpt_outdir),
                            "num_cpus": required_cpus,
                            "output_dir": str(run_outdir),
                            "start_time": "",
                            "end_time": "",
                            "elapsed_sec": "0.000",
                            "gem5_rc": failed_rc,
                            "test_cmd_exit": "",
                            "tick_rows": 0,
                            "expected_rows": key.clients * key.requests_per_client,
                            "server_tick_rows": 0,
                            "expected_server_rows": key.clients * key.requests_per_client,
                            "status": cached_status,
                        },
                        exp_fields,
                    )
                    print(
                        f"[done] {exp.exp_id} status={cached_status} "
                        f"gem5_rc={failed_rc} test_rc=None ticks=0/"
                        f"{key.clients * key.requests_per_client} elapsed=0.0s"
                    )
                    continue

                resolved = None if args.dry_run else bare_rpc.resolve_checkpoint_dir(
                    ckpt_outdir
                )
                if resolved is not None:
                    checkpoint_dir = resolved
                    checkpoint_cache[checkpoint_key] = checkpoint_dir
                    print(
                        f"[app-matrix] reuse checkpoint for {checkpoint_label}: "
                        f"{checkpoint_dir}"
                    )
                else:
                    ckpt_cmd = kvm_cmd_prefix + [
                        str(gem5_bin),
                        "-d",
                        str(ckpt_outdir),
                        str(save_ckpt_cfg),
                        "--disk",
                        str(disk_img),
                        "--boot_cpu_type",
                        args.boot_cpu_type,
                        "--num_cpus",
                        str(required_cpus),
                        "--rpc_client_count",
                        str(key.clients),
                        "--rpc_response_lane_count",
                        str(key.response_lane_count),
                        "--rpc_metadata_entries",
                        str(key.mq_entries),
                        "--cxl_extra_latency_ns",
                        str(key.cxl_extra_latency_ns),
                    ]
                    if args.copy_engine_channels > 0:
                        ckpt_cmd.extend(
                            [
                                "--copy_engine_channels",
                                str(args.copy_engine_channels),
                            ]
                        )
                    if args.checkpoint_handoff_deadline_sim_seconds > 0:
                        ckpt_cmd.extend(
                            [
                                "--handoff_deadline_sim_seconds",
                                str(args.checkpoint_handoff_deadline_sim_seconds),
                            ]
                        )

                    if args.dry_run:
                        checkpoint_dir = ckpt_outdir
                        print(
                            "[dry-run] would build checkpoint:",
                            " ".join(shlex.quote(c) for c in ckpt_cmd),
                        )
                    else:
                        ckpt_outdir.mkdir(parents=True, exist_ok=True)
                        ckpt_log = (
                            batch_dir / "checkpoints" / checkpoint_label /
                            "checkpoint_build.log"
                        )
                        try:
                            ckpt_rc = bare_rpc.run_and_tee(
                                ckpt_cmd,
                                ckpt_log,
                                sudo_signal_prefix=kvm_signal_prefix,
                            )
                        except RuntimeError as exc:
                            print(f"[fatal] {exc}")
                            return 2
                        if ckpt_rc != 0:
                            checkpoint_failure_cache[checkpoint_key] = (
                                "checkpoint_failed",
                                ckpt_rc,
                            )
                            bare_rpc.append_row(
                                experiments_csv,
                                {
                                    "exp_id": exp.exp_id,
                                    "source": exp.source,
                                    **experiment_metadata_row(key),
                                    "checkpoint_dir": str(ckpt_outdir),
                                    "num_cpus": required_cpus,
                                    "output_dir": str(run_outdir),
                                    "start_time": "",
                                    "end_time": "",
                                    "elapsed_sec": "0.000",
                                    "gem5_rc": ckpt_rc,
                                    "test_cmd_exit": "",
                                    "tick_rows": 0,
                                    "expected_rows": key.clients * key.requests_per_client,
                                    "server_tick_rows": 0,
                                    "expected_server_rows": key.clients * key.requests_per_client,
                                    "status": "checkpoint_failed",
                                },
                                exp_fields,
                            )
                            print(
                                f"[done] {exp.exp_id} status=checkpoint_failed "
                                f"gem5_rc={ckpt_rc} test_rc=None ticks=0/"
                                f"{key.clients * key.requests_per_client} elapsed=0.0s"
                            )
                            continue
                        resolved = bare_rpc.resolve_checkpoint_dir(ckpt_outdir)
                        if resolved is None:
                            checkpoint_failure_cache[checkpoint_key] = (
                                "checkpoint_missing",
                                2,
                            )
                            bare_rpc.append_row(
                                experiments_csv,
                                {
                                    "exp_id": exp.exp_id,
                                    "source": exp.source,
                                    **experiment_metadata_row(key),
                                    "checkpoint_dir": str(ckpt_outdir),
                                    "num_cpus": required_cpus,
                                    "output_dir": str(run_outdir),
                                    "start_time": "",
                                    "end_time": "",
                                    "elapsed_sec": "0.000",
                                    "gem5_rc": 2,
                                    "test_cmd_exit": "",
                                    "tick_rows": 0,
                                    "expected_rows": key.clients * key.requests_per_client,
                                    "server_tick_rows": 0,
                                    "expected_server_rows": key.clients * key.requests_per_client,
                                    "status": "checkpoint_missing",
                                },
                                exp_fields,
                            )
                            print(
                                f"[done] {exp.exp_id} status=checkpoint_missing "
                                f"gem5_rc=2 test_rc=None ticks=0/"
                                f"{key.clients * key.requests_per_client} elapsed=0.0s"
                            )
                            continue
                        checkpoint_dir = resolved

                    checkpoint_cache[checkpoint_key] = checkpoint_dir

            server_args_parts = [
                "--silent",
                *profile_arg_list(key),
                f"--mq-entries {key.mq_entries}",
                f"--head-sync-threshold {key.head_sync_threshold}",
                f"--response-dma-threshold {key.response_dma_threshold}",
                f"--clients-per-dma-lane {key.clients_per_dma_lane}",
                f"--prefetch-mode {key.prefetch_mode}",
            ]
            server_args = " ".join(server_args_parts)

            client_args_parts = [
                f"--requests {key.requests_per_client}",
                "--silent",
                *profile_arg_list(key),
                *workload_arg_list(key),
            ]
            client_args = " ".join(client_args_parts)

            test_cmd = (
                f"CXL_RPC_CLIENT_COUNT={key.clients} "
                f"CXL_RPC_CLIENT_TIMEOUT_SEC=0 "
                f"CXL_RPC_SERVER_READY_TIMEOUT_SEC=0 "
                f"CXL_RPC_PIN_CORES=1 "
                f"CXL_RPC_SERVER_CORE=0 "
                f"CXL_RPC_CLIENT_CORE_BASE=1 "
                f"CXL_RPC_SERVER_ARGS={shlex.quote(server_args)} "
                f"bash /home/test_code/run_rpc_server_clients.sh "
                f"/home/test_code/rpc_mica_server /home/test_code/rpc_mica_client "
                f"{client_args}"
            )

            gem5_cmd = kvm_cmd_prefix + [
                str(gem5_bin),
                "-d",
                str(run_outdir),
                str(test_cfg),
                "--disk",
                str(disk_img),
                "--boot_cpu_type",
                args.boot_cpu_type,
                "--cpu_type",
                "TIMING",
                "--num_cpus",
                str(required_cpus),
                "--rpc_client_count",
                str(key.clients),
                "--rpc_response_lane_count",
                str(key.response_lane_count),
                "--rpc_metadata_entries",
                str(key.mq_entries),
                "--cxl_extra_latency_ns",
                str(key.cxl_extra_latency_ns),
                "--checkpoint",
                str(checkpoint_dir),
                "--test_cmd",
                test_cmd,
            ]
            if args.copy_engine_channels > 0:
                gem5_cmd.extend(
                    [
                        "--copy_engine_channels",
                        str(args.copy_engine_channels),
                    ]
                )

            start_ts = dt.datetime.now().isoformat(timespec="seconds")
            start_time = time.time()
            print(
                f"[run {idx}/{len(experiments)}] {exp.exp_id} "
                f"(profile={key.profile}, clients={key.clients}, "
                f"reqs/client={key.requests_per_client}, rec={key.record_count}, "
                f"kv={key.key_size}/{key.value_size}, rr={format_float_token(key.read_ratio)}, "
                f"ur={format_float_token(key.update_ratio)}, "
                f"rmw={format_float_token(key.rmw_ratio)}, "
                f"dist={key.key_dist}, "
                f"{f'zipf={format_float_token(key.zipf_theta)}, ' if key.key_dist == 'zipf' else ''}"
                f"mq={key.mq_entries}, "
                f"hs={key.head_sync_threshold}, "
                f"cxl+={key.cxl_extra_latency_ns}ns, lanes={key.response_lane_count}, "
                f"cpl={key.clients_per_dma_lane}, dmath={key.response_dma_threshold}, "
                f"pf={key.prefetch_mode}, cpus={required_cpus})"
            )

            if args.dry_run:
                gem5_rc = 0
                test_cmd_rc = 0
                client_rows: List[Dict[str, int]] = []
                server_rows: List[Dict[str, int]] = []
            else:
                try:
                    gem5_rc = bare_rpc.run_and_tee(
                        gem5_cmd,
                        run_log,
                        sudo_signal_prefix=kvm_signal_prefix,
                    )
                except RuntimeError as exc:
                    print(f"[fatal] {exc}")
                    return 2
                test_cmd_rc, client_rows, server_rows = bare_rpc.parse_run_results(
                    run_outdir
                )

            end_time = time.time()
            end_ts = dt.datetime.now().isoformat(timespec="seconds")
            elapsed = end_time - start_time

            expected_rows = key.clients * key.requests_per_client
            expected_server_rows = expected_rows
            if args.dry_run:
                status = "dry_run"
            else:
                status = "ok"
                if gem5_rc != 0:
                    status = "gem5_failed"
                elif test_cmd_rc is None:
                    status = "test_rc_missing"
                elif test_cmd_rc != 0:
                    status = "test_failed"
                elif len(client_rows) != expected_rows:
                    status = "incomplete_ticks"
                elif len(server_rows) != expected_server_rows:
                    status = "incomplete_server_ticks"

            bare_rpc.append_row(
                experiments_csv,
                {
                    "exp_id": exp.exp_id,
                    "source": exp.source,
                    **experiment_metadata_row(key),
                    "checkpoint_dir": str(checkpoint_dir),
                    "num_cpus": required_cpus,
                    "output_dir": str(run_outdir),
                    "start_time": start_ts,
                    "end_time": end_ts,
                    "elapsed_sec": f"{elapsed:.3f}",
                    "gem5_rc": gem5_rc,
                    "test_cmd_exit": "" if test_cmd_rc is None else test_cmd_rc,
                    "tick_rows": len(client_rows),
                    "expected_rows": expected_rows,
                    "server_tick_rows": len(server_rows),
                    "expected_server_rows": expected_server_rows,
                    "status": status,
                },
                exp_fields,
            )

            for row in client_rows:
                bare_rpc.append_row(
                    ticks_csv,
                    {
                        "exp_id": exp.exp_id,
                        **experiment_metadata_row(key),
                        "node_id": row["node_id"],
                        "req_index": row["req_index"],
                        "start_tick": row["start_tick"],
                        "end_tick": row["end_tick"],
                        "output_dir": str(run_outdir),
                    },
                    tick_fields,
                )

            for row in server_rows:
                bare_rpc.append_row(
                    server_ticks_csv,
                    {
                        "exp_id": exp.exp_id,
                        **experiment_metadata_row(key),
                        "server_req_index": row["server_req_index"],
                        "poll_tick": row["poll_tick"],
                        "execute_tick": row["execute_tick"],
                        "response_tick": row["response_tick"],
                        "output_dir": str(run_outdir),
                    },
                    server_tick_fields,
                )

            print(
                f"[done] {exp.exp_id} status={status} "
                f"gem5_rc={gem5_rc} test_rc={test_cmd_rc} "
                f"ticks={len(client_rows)}/{expected_rows} "
                f"server_ticks={len(server_rows)}/{expected_server_rows} "
                f"elapsed={elapsed:.1f}s"
            )
    finally:
        bare_rpc.release_matrix_lock(lock_fp)

    if not args.dry_run:
        summary_cmd = [
            sys.executable,
            str(summary_script),
            "--batch-dir",
            str(batch_dir),
        ]
        print("+", " ".join(shlex.quote(c) for c in summary_cmd), flush=True)
        summary_proc = subprocess.run(summary_cmd, check=False)
        if summary_proc.returncode != 0:
            print(f"[fatal] summary generation failed rc={summary_proc.returncode}")
            return summary_proc.returncode

    print("\n[app-matrix] finished")
    print(f"[app-matrix] plan: {plan_csv}")
    print(f"[app-matrix] experiments: {experiments_csv}")
    print(f"[app-matrix] ticks: {ticks_csv}")
    print(f"[app-matrix] server ticks: {server_ticks_csv}")
    if not args.dry_run:
        print(f"[app-matrix] client summary: {batch_dir / 'summary_client_latency.csv'}")
        print(f"[app-matrix] server summary: {batch_dir / 'summary_server_breakdown.csv'}")
        print(f"[app-matrix] throughput summary: {batch_dir / 'summary_throughput.csv'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
