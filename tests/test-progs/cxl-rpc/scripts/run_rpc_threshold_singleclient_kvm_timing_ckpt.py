#!/usr/bin/env python3
"""
Run the single-client threshold microbenchmark with:
  - KVM boot + TIMING CPU test phase
  - explicit server response policy: CPU-only vs DMA-only
  - response sizes: 128B / 256B / 512B
  - one client, 30 requests, default window 16

This script is intentionally separate from the main RPC matrix runner so the
default experiment path and output schema stay unchanged.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import os
import shlex
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional

from run_rpc_matrix_kvm_timing_ckpt import (
    acquire_matrix_lock,
    append_row,
    now_tag,
    parse_run_results,
    read_success_exp_ids,
    release_matrix_lock,
    resolve_checkpoint_dir,
    resolve_repo_root,
    run_and_tee,
    sudo_nopass_available,
)


DEFAULT_CLIENTS = 1
DEFAULT_REQUEST_SIZE = 64
DEFAULT_REQUESTS = 30
DEFAULT_WINDOW = 16
DEFAULT_MAX_POLLS = 2_000_000
DEFAULT_RESPONSE_SIZES = (128, 256, 512)
DEFAULT_SERVER_BIN = "/home/test_code/rpc_server_threshold_mode"
DEFAULT_CLIENT_BIN = "/home/test_code/rpc_client_example"
DEFAULT_POLICIES = ("cpu", "dma")


@dataclass(frozen=True)
class ThresholdExperimentKey:
    request_size: int
    response_size: int
    clients: int
    requests_per_client: int
    window: int
    policy: str


@dataclass
class ThresholdExperiment:
    key: ThresholdExperimentKey
    exp_id: str
    source: str


def parse_response_sizes(csv_text: str) -> List[int]:
    sizes: List[int] = []
    for chunk in csv_text.split(","):
        value = chunk.strip()
        if not value:
            continue
        parsed = int(value, 0)
        if parsed <= 0:
            raise ValueError(f"response size must be > 0: {value}")
        sizes.append(parsed)
    if not sizes:
        raise ValueError("empty response size list")
    return sizes


def build_matrix(request_size: int,
                 response_sizes: List[int],
                 requests_per_client: int,
                 window: int) -> List[ThresholdExperiment]:
    experiments: List[ThresholdExperiment] = []

    for policy in DEFAULT_POLICIES:
        for response_size in response_sizes:
            key = ThresholdExperimentKey(
                request_size=request_size,
                response_size=response_size,
                clients=DEFAULT_CLIENTS,
                requests_per_client=requests_per_client,
                window=window,
                policy=policy,
            )
            experiments.append(
                ThresholdExperiment(
                    key=key,
                    exp_id=(
                        f"{policy}_req{request_size}_resp{response_size}"
                        f"_c{DEFAULT_CLIENTS}_r{requests_per_client}_w{window}"
                    ),
                    source="threshold_singleclient",
                )
            )

    return experiments


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Run single-client CPU-only vs DMA-only response threshold tests "
            "(KVM + checkpoint + TIMING)"
        )
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
    parser.add_argument("--requests", type=int, default=DEFAULT_REQUESTS)
    parser.add_argument("--request-size", type=int, default=DEFAULT_REQUEST_SIZE)
    parser.add_argument("--response-sizes", type=str,
                        default=",".join(str(v) for v in DEFAULT_RESPONSE_SIZES))
    parser.add_argument("--window", type=int, default=DEFAULT_WINDOW)
    parser.add_argument("--max-polls", type=int, default=DEFAULT_MAX_POLLS)
    parser.add_argument("--server-bin", type=str, default=DEFAULT_SERVER_BIN)
    parser.add_argument("--client-bin", type=str, default=DEFAULT_CLIENT_BIN)
    parser.add_argument(
        "--only-exp-id",
        type=str,
        default="",
        help="Run only the experiment whose exp_id exactly matches this value",
    )
    parser.add_argument(
        "--start-index",
        type=int,
        default=1,
        help="1-based experiment index to start from (skip earlier ones)",
    )
    parser.add_argument(
        "--end-index",
        type=int,
        default=0,
        help="1-based experiment index to stop at (0 means run through the end)",
    )
    parser.add_argument(
        "--copy-engine-channels",
        type=int,
        default=0,
        help=(
            "Pass --copy_engine_channels through to both checkpoint and timing "
            "configs. 0 keeps their auto-derived topology."
        ),
    )
    parser.add_argument(
        "--checkpoint-handoff-deadline-sim-seconds",
        type=int,
        default=0,
        help=(
            "Pass --handoff_deadline_sim_seconds to checkpoint generation. "
            "0 keeps the checkpoint script default."
        ),
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
    parser.add_argument(
        "--allow-concurrent-runs",
        action="store_true",
        help="Skip the default exclusive wrapper lock.",
    )
    parser.add_argument(
        "--inter-experiment-sleep-sec",
        type=int,
        default=5,
        help="Sleep this many seconds between actually executed experiments",
    )
    args = parser.parse_args()

    if args.requests <= 0:
        print("[fatal] --requests must be > 0")
        return 2
    if args.request_size <= 0:
        print("[fatal] --request-size must be > 0")
        return 2
    if args.window <= 0:
        print("[fatal] --window must be > 0")
        return 2
    if args.max_polls <= 0:
        print("[fatal] --max-polls must be > 0")
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
    if args.copy_engine_channels < 0:
        print("[fatal] --copy-engine-channels must be >= 0")
        return 2
    if args.checkpoint_handoff_deadline_sim_seconds < 0:
        print("[fatal] --checkpoint-handoff-deadline-sim-seconds must be >= 0")
        return 2

    try:
        response_sizes = parse_response_sizes(args.response_sizes)
    except ValueError as exc:
        print(f"[fatal] invalid --response-sizes: {exc}")
        return 2

    repo_root = resolve_repo_root(args.repo_root)
    output_base = (repo_root / args.output_base).resolve()
    batch_name = (
        args.batch_name or
        f"rpc_threshold_singleclient_kvm_timing_ckpt_{now_tag()}"
    )
    batch_dir = output_base / batch_name
    batch_dir.mkdir(parents=True, exist_ok=True)

    gem5_bin = repo_root / "build/X86/gem5.opt"
    test_cfg = repo_root / "configs/example/gem5_library/x86-cxl-rpc-test.py"
    save_ckpt_cfg = (
        repo_root / "configs/example/gem5_library/x86-cxl-rpc-save-checkpoint.py"
    )
    inject_script = repo_root / "tests/test-progs/cxl-rpc/scripts/setup_disk_image.sh"
    disk_img = (
        Path(args.disk).resolve()
        if args.disk else
        (repo_root / "files" / "parsec.img").resolve()
    )

    experiments = build_matrix(args.request_size,
                               response_sizes,
                               args.requests,
                               args.window)
    if args.only_exp_id:
        experiments = [exp for exp in experiments if exp.exp_id == args.only_exp_id]
        if not experiments:
            print(f"[fatal] no experiment matched --only-exp-id={args.only_exp_id}")
            return 2

    max_required_cpus = DEFAULT_CLIENTS + 1
    plan_csv = batch_dir / "plan.csv"
    with plan_csv.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "exp_id",
                "source",
                "policy",
                "disk_image",
                "request_size",
                "response_size",
                "clients",
                "requests_per_client",
                "window",
                "server_bin",
                "client_bin",
            ],
        )
        writer.writeheader()
        for exp in experiments:
            writer.writerow(
                {
                    "exp_id": exp.exp_id,
                    "source": exp.source,
                    "policy": exp.key.policy,
                    "disk_image": str(disk_img),
                    "request_size": exp.key.request_size,
                    "response_size": exp.key.response_size,
                    "clients": exp.key.clients,
                    "requests_per_client": exp.key.requests_per_client,
                    "window": exp.key.window,
                    "server_bin": args.server_bin,
                    "client_bin": args.client_bin,
                }
            )

    print(f"[threshold] total experiments: {len(experiments)}")
    print(f"[threshold] max required cores (server+clients): {max_required_cpus}")
    print(f"[threshold] batch dir: {batch_dir}")

    experiments_csv = batch_dir / "experiments.csv"
    ticks_csv = batch_dir / "results_ticks.csv"
    server_ticks_csv = batch_dir / "results_server_ticks.csv"
    done_ids = set()
    if not args.force_rerun:
        done_ids = read_success_exp_ids(experiments_csv)

    exp_fields = [
        "exp_id",
        "source",
        "policy",
        "disk_image",
        "request_size",
        "response_size",
        "clients",
        "requests_per_client",
        "window",
        "server_bin",
        "client_bin",
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
        "policy",
        "request_size",
        "response_size",
        "clients",
        "requests_per_client",
        "window",
        "node_id",
        "req_index",
        "start_tick",
        "end_tick",
        "output_dir",
    ]
    server_tick_fields = [
        "exp_id",
        "policy",
        "request_size",
        "response_size",
        "clients",
        "requests_per_client",
        "window",
        "server_req_index",
        "node_id",
        "poll_notify_tick",
        "poll_req_data_tick",
        "exec_tick",
        "resp_submit_tick",
        "output_dir",
    ]

    started_experiments = 0
    boot_requires_kvm = args.boot_cpu_type == "KVM"
    direct_kvm_access = os.access("/dev/kvm", os.R_OK | os.W_OK)
    sudo_n_available = sudo_nopass_available()
    kvm_cmd_prefix: List[str] = []
    checkpoint_dir: Optional[Path] = None
    checkpoint_failure: Optional[Tuple[str, int]] = None
    kvm_signal_prefix: Optional[List[str]] = None
    lock_fp = None

    if not boot_requires_kvm:
        print(
            f"[threshold] boot CPU type is {args.boot_cpu_type}; "
            "/dev/kvm not required"
        )
    elif direct_kvm_access:
        print("[threshold] /dev/kvm is directly accessible by current user")
    else:
        print("[threshold] /dev/kvm is not directly accessible by current user")
        if not sudo_n_available:
            print("[fatal] /dev/kvm requires elevated access, but `sudo -n` is unavailable")
            return 2
        kvm_cmd_prefix = ["sudo", "-n"]
        kvm_signal_prefix = kvm_cmd_prefix
        print("[threshold] checkpoint builds will run under `sudo -n`")

    if not args.allow_concurrent_runs:
        lock_path = output_base / ".rpc_threshold_singleclient.lock"
        lock_fp = acquire_matrix_lock(lock_path, batch_name)
        if lock_fp is None:
            return 2
        print(f"[threshold] acquired wrapper lock: {lock_path}")
    else:
        print("[threshold] concurrent wrapper runs allowed; skipping exclusive lock")

    try:
        if not args.skip_inject:
            inject_log = batch_dir / "inject.log"
            inject_cmd = [
                "env",
                "CXL_RPC_INCLUDE_THRESHOLD_SERVER=1",
                "bash",
                str(inject_script),
                str(disk_img),
            ]
            if not args.dry_run:
                try:
                    rc = run_and_tee(inject_cmd, inject_log)
                except RuntimeError as exc:
                    print(f"[fatal] {exc}")
                    return 2
                if rc != 0:
                    print(f"[fatal] inject failed rc={rc}")
                    return rc
            else:
                print("[dry-run] skip inject execution")

        checkpoint_label = (
            f"boot_{args.boot_cpu_type.lower()}_clients_{DEFAULT_CLIENTS}"
        )
        ckpt_outdir = (
            batch_dir / "checkpoints" / checkpoint_label / "cxl_rpc_checkpoint"
        )

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
                    f"[stop] reached --end-index={args.end_index}; "
                    "ending batch run"
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
                        f"[threshold] sleeping {args.inter_experiment_sleep_sec}s "
                        f"before starting next experiment: {exp.exp_id}"
                    )
                    time.sleep(args.inter_experiment_sleep_sec)
            started_experiments += 1

            run_outdir = batch_dir / f"run_{idx:02d}_{exp.exp_id}"
            run_outdir.mkdir(parents=True, exist_ok=True)
            run_log = run_outdir / "gem5_run.log"
            required_cpus = key.clients + 1

            if checkpoint_dir is None:
                if checkpoint_failure is not None:
                    failed_status, failed_rc = checkpoint_failure
                    cached_status = f"{failed_status}_cached"
                    append_row(
                        experiments_csv,
                        {
                            "exp_id": exp.exp_id,
                            "source": exp.source,
                            "policy": key.policy,
                            "disk_image": str(disk_img),
                            "request_size": key.request_size,
                            "response_size": key.response_size,
                            "clients": key.clients,
                            "requests_per_client": key.requests_per_client,
                            "window": key.window,
                            "server_bin": args.server_bin,
                            "client_bin": args.client_bin,
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
                            "expected_server_rows":
                                key.clients * key.requests_per_client,
                            "status": cached_status,
                        },
                        exp_fields,
                    )
                    print(
                        f"[done] {exp.exp_id} status={cached_status} "
                        f"gem5_rc={failed_rc}"
                    )
                    continue

                resolved = None if args.dry_run else resolve_checkpoint_dir(ckpt_outdir)
                if resolved is not None:
                    checkpoint_dir = resolved
                    print(
                        f"[threshold] reuse checkpoint for {key.clients} client(s): "
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
                            ckpt_rc = run_and_tee(
                                ckpt_cmd,
                                ckpt_log,
                                sudo_signal_prefix=kvm_signal_prefix,
                            )
                        except RuntimeError as exc:
                            print(f"[fatal] {exc}")
                            return 2
                        if ckpt_rc != 0:
                            checkpoint_failure = ("checkpoint_failed", ckpt_rc)
                            append_row(
                                experiments_csv,
                                {
                                    "exp_id": exp.exp_id,
                                    "source": exp.source,
                                    "policy": key.policy,
                                    "disk_image": str(disk_img),
                                    "request_size": key.request_size,
                                    "response_size": key.response_size,
                                    "clients": key.clients,
                                    "requests_per_client": key.requests_per_client,
                                    "window": key.window,
                                    "server_bin": args.server_bin,
                                    "client_bin": args.client_bin,
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
                                    "expected_server_rows":
                                        key.clients * key.requests_per_client,
                                    "status": "checkpoint_failed",
                                },
                                exp_fields,
                            )
                            print(
                                f"[done] {exp.exp_id} status=checkpoint_failed "
                                f"gem5_rc={ckpt_rc}"
                            )
                            continue
                        resolved = resolve_checkpoint_dir(ckpt_outdir)
                        if resolved is None:
                            checkpoint_failure = ("checkpoint_missing", 2)
                            append_row(
                                experiments_csv,
                                {
                                    "exp_id": exp.exp_id,
                                    "source": exp.source,
                                    "policy": key.policy,
                                    "disk_image": str(disk_img),
                                    "request_size": key.request_size,
                                    "response_size": key.response_size,
                                    "clients": key.clients,
                                    "requests_per_client": key.requests_per_client,
                                    "window": key.window,
                                    "server_bin": args.server_bin,
                                    "client_bin": args.client_bin,
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
                                    "expected_server_rows":
                                        key.clients * key.requests_per_client,
                                    "status": "checkpoint_missing",
                                },
                                exp_fields,
                            )
                            print(
                                f"[done] {exp.exp_id} status=checkpoint_missing "
                                "gem5_rc=2"
                            )
                            continue
                        checkpoint_dir = resolved

            server_args = (
                f"--silent --response-size {key.response_size} "
                f"--response-mode {key.policy}"
            )
            test_cmd = (
                f"CXL_RPC_CLIENT_COUNT={key.clients} "
                f"CXL_RPC_CLIENT_TIMEOUT_SEC=0 "
                f"CXL_RPC_SERVER_READY_TIMEOUT_SEC=0 "
                f"CXL_RPC_SERVER_MAX_REQUESTS={key.requests_per_client} "
                f"CXL_RPC_PIN_CORES=1 "
                f"CXL_RPC_SERVER_CORE=0 "
                f"CXL_RPC_CLIENT_CORE_BASE=1 "
                f"CXL_RPC_SERVER_ARGS={shlex.quote(server_args)} "
                f"bash /home/test_code/run_rpc_server_clients.sh "
                f"{shlex.quote(args.server_bin)} {shlex.quote(args.client_bin)} "
                f"--requests {key.requests_per_client} "
                f"--window {key.window} "
                f"--max-polls {args.max_polls} "
                f"--request-size {key.request_size} "
                f"--response-size {key.response_size} "
                f"--silent"
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
                f"(policy={key.policy}, req={key.request_size}, "
                f"resp={key.response_size}, clients={key.clients}, "
                f"reqs/client={key.requests_per_client}, window={key.window}, "
                f"cpus={required_cpus})"
            )

            if args.dry_run:
                gem5_rc = 0
                test_cmd_rc = 0
                client_rows: List[Dict[str, int]] = []
                server_rows: List[Dict[str, int]] = []
            else:
                try:
                    gem5_rc = run_and_tee(
                        gem5_cmd,
                        run_log,
                        sudo_signal_prefix=kvm_signal_prefix,
                    )
                except RuntimeError as exc:
                    print(f"[fatal] {exc}")
                    return 2
                test_cmd_rc, client_rows, server_rows = parse_run_results(
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

            append_row(
                experiments_csv,
                {
                    "exp_id": exp.exp_id,
                    "source": exp.source,
                    "policy": key.policy,
                    "disk_image": str(disk_img),
                    "request_size": key.request_size,
                    "response_size": key.response_size,
                    "clients": key.clients,
                    "requests_per_client": key.requests_per_client,
                    "window": key.window,
                    "server_bin": args.server_bin,
                    "client_bin": args.client_bin,
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
                append_row(
                    ticks_csv,
                    {
                        "exp_id": exp.exp_id,
                        "policy": key.policy,
                        "request_size": key.request_size,
                        "response_size": key.response_size,
                        "clients": key.clients,
                        "requests_per_client": key.requests_per_client,
                        "window": key.window,
                        "node_id": row["node_id"],
                        "req_index": row["req_index"],
                        "start_tick": row["start_tick"],
                        "end_tick": row["end_tick"],
                        "output_dir": str(run_outdir),
                    },
                    tick_fields,
                )

            for row in server_rows:
                append_row(
                    server_ticks_csv,
                    {
                        "exp_id": exp.exp_id,
                        "policy": key.policy,
                        "request_size": key.request_size,
                        "response_size": key.response_size,
                        "clients": key.clients,
                        "requests_per_client": key.requests_per_client,
                        "window": key.window,
                        "server_req_index": row["server_req_index"],
                        "node_id": row["node_id"],
                        "poll_notify_tick": row["poll_notify_tick"],
                        "poll_req_data_tick": row["poll_req_data_tick"],
                        "exec_tick": row["exec_tick"],
                        "resp_submit_tick": row["resp_submit_tick"],
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
        release_matrix_lock(lock_fp)

    print("\n[threshold] finished")
    print(f"[threshold] plan: {plan_csv}")
    print(f"[threshold] experiments: {experiments_csv}")
    print(f"[threshold] ticks: {ticks_csv}")
    print(f"[threshold] server ticks: {server_ticks_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
