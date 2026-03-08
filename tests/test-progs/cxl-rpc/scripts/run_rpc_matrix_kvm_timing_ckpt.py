#!/usr/bin/env python3
"""
Run CXL RPC matrix experiments with:
  - KVM boot + TIMING CPU test phase
  - rebuild checkpoint per experiment (exact CPU count)
  - automatic result extraction from board.pc.com_1.device

Matrix (deduplicated):
  A) req_size in [8B,64B,256B,1KB,4KB,16KB,64KB,256KB], clients=16, reqs=15
  B) req_size=64B, clients in [1,2,4,8,16,32], reqs=15
  response_size is fixed to 16B.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import re
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple


REQ_SWEEP_SIZES = [8, 64, 256, 1024, 4096, 16384, 65536, 262144]
CLIENT_SWEEP_COUNTS = [1, 2, 4, 8, 16, 32]


@dataclass(frozen=True)
class ExperimentKey:
    request_size: int
    response_size: int
    clients: int
    requests_per_client: int


@dataclass
class Experiment:
    key: ExperimentKey
    exp_id: str
    source: str


def now_tag() -> str:
    return dt.datetime.now().strftime("%Y%m%d_%H%M%S")


def resolve_repo_root(cli_repo_root: Optional[str]) -> Path:
    if cli_repo_root:
        return Path(cli_repo_root).resolve()
    # .../tests/test-progs/cxl-rpc/scripts/run_rpc_matrix_kvm_timing_ckpt.py
    return Path(__file__).resolve().parents[4]


def build_matrix(requests_per_client: int, response_size: int) -> List[Experiment]:
    dedup: Dict[ExperimentKey, Experiment] = {}

    for size in REQ_SWEEP_SIZES:
        key = ExperimentKey(
            request_size=size,
            response_size=response_size,
            clients=16,
            requests_per_client=requests_per_client,
        )
        dedup.setdefault(
            key,
            Experiment(
                key=key,
                exp_id=f"req{size}_resp{response_size}_c16_r{requests_per_client}",
                source="size_sweep",
            ),
        )

    for clients in CLIENT_SWEEP_COUNTS:
        key = ExperimentKey(
            request_size=64,
            response_size=response_size,
            clients=clients,
            requests_per_client=requests_per_client,
        )
        dedup.setdefault(
            key,
            Experiment(
                key=key,
                exp_id=f"req64_resp{response_size}_c{clients}_r{requests_per_client}",
                source="client_sweep",
            ),
        )

    return sorted(dedup.values(), key=lambda x: (x.key.clients, x.key.request_size))


def run_and_tee(cmd: List[str], log_path: Path) -> int:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    print("+", " ".join(shlex.quote(c) for c in cmd), flush=True)
    with log_path.open("w", encoding="utf-8") as logf:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        assert proc.stdout is not None
        for line in proc.stdout:
            sys.stdout.write(line)
            logf.write(line)
        proc.wait()
        return int(proc.returncode)


def resolve_checkpoint_dir(path: Path) -> Optional[Path]:
    if (path / "m5.cpt").exists():
        return path
    if path.is_file() and path.name == "m5.cpt":
        return path.parent
    if path.exists():
        candidates = sorted(
            path.rglob("m5.cpt"),
            key=lambda p: p.stat().st_mtime,
            reverse=True,
        )
        if candidates:
            return candidates[0].parent
    return None


def detect_checkpoint_cpus(checkpoint_dir: Path) -> Optional[int]:
    cfg = checkpoint_dir / "config.ini"
    if not cfg.exists():
        return None
    text = cfg.read_text(encoding="utf-8", errors="ignore")
    count = len(re.findall(r"^\[board\.processor\.start\d+\]$", text, flags=re.M))
    return count if count > 0 else None


def parse_board_ticks(board_path: Path) -> Tuple[Optional[int], List[Dict[str, int]]]:
    if not board_path.exists():
        return None, []

    test_rc: Optional[int] = None
    fields: Dict[Tuple[int, int], Dict[str, int]] = {}
    re_multi = re.compile(
        r"^\[CLIENT\[(\d+)\]\]\s+req_(\d+)_(start|end|delta)_tick=(\d+)\s*$"
    )
    re_single = re.compile(
        r"^\[CLIENT\]\s+req_(\d+)_(start|end|delta)_tick=(\d+)\s*$"
    )

    for line in board_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        if "TEST_CMD_EXIT_CODE=" in line:
            try:
                test_rc = int(line.split("TEST_CMD_EXIT_CODE=", 1)[1].strip())
            except ValueError:
                pass

        m = re_multi.match(line)
        if m:
            cid = int(m.group(1))
            ridx = int(m.group(2))
            kind = m.group(3)
            val = int(m.group(4))
            fields.setdefault((cid, ridx), {})[kind] = val
            continue

        m = re_single.match(line)
        if m:
            cid = 0
            ridx = int(m.group(1))
            kind = m.group(2)
            val = int(m.group(3))
            fields.setdefault((cid, ridx), {})[kind] = val

    rows: List[Dict[str, int]] = []
    for (cid, ridx), vals in sorted(fields.items()):
        if "start" in vals and "end" in vals and "delta" in vals:
            rows.append(
                {
                    "client_id": cid,
                    "req_index": ridx,
                    "start_tick": vals["start"],
                    "end_tick": vals["end"],
                    "delta_tick": vals["delta"],
                }
            )
    return test_rc, rows


def read_success_exp_ids(csv_path: Path) -> set[str]:
    if not csv_path.exists():
        return set()
    done: set[str] = set()
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row.get("status") == "ok":
                exp_id = row.get("exp_id", "")
                if exp_id:
                    done.add(exp_id)
    return done


def append_row(csv_path: Path, row: dict, fieldnames: List[str]) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    need_header = not csv_path.exists()
    with csv_path.open("a", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        if need_header:
            writer.writeheader()
        writer.writerow(row)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run CXL RPC matrix (KVM+TIMING+checkpoint) and extract ticks"
    )
    parser.add_argument("--repo-root", type=str, default=None)
    parser.add_argument("--output-base", type=str, default="output")
    parser.add_argument("--batch-name", type=str, default="")
    parser.add_argument("--requests", type=int, default=15)
    parser.add_argument("--response-size", type=int, default=16)
    parser.add_argument("--max-polls", type=int, default=2000000)
    parser.add_argument(
        "--start-index",
        type=int,
        default=1,
        help="1-based experiment index to start from (skip earlier ones)",
    )
    parser.add_argument("--checkpoint", type=str, default="")
    parser.add_argument("--checkpoint-cpus", type=int, default=0)
    parser.add_argument("--skip-inject", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--force-rerun", action="store_true")
    args = parser.parse_args()

    repo_root = resolve_repo_root(args.repo_root)
    output_base = (repo_root / args.output_base).resolve()
    batch_name = args.batch_name or f"rpc_matrix_kvm_timing_ckpt_{now_tag()}"
    batch_dir = output_base / batch_name
    batch_dir.mkdir(parents=True, exist_ok=True)
    if args.start_index < 1:
        print("[fatal] --start-index must be >= 1")
        return 2

    gem5_bin = repo_root / "build/X86/gem5.opt"
    test_cfg = repo_root / "configs/example/gem5_library/x86-cxl-rpc-test.py"
    save_ckpt_cfg = (
        repo_root / "configs/example/gem5_library/x86-cxl-rpc-save-checkpoint.py"
    )
    inject_script = repo_root / "tests/test-progs/cxl-rpc/scripts/setup_disk_image.sh"
    disk_img = repo_root / "files/parsec.img"

    experiments = build_matrix(args.requests, args.response_size)
    max_clients = max(exp.key.clients for exp in experiments)
    max_required_cpus = max_clients + 1  # server core + one core per client

    plan_csv = batch_dir / "plan.csv"
    with plan_csv.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "exp_id",
                "source",
                "request_size",
                "response_size",
                "clients",
                "requests_per_client",
            ],
        )
        writer.writeheader()
        for exp in experiments:
            writer.writerow(
                {
                    "exp_id": exp.exp_id,
                    "source": exp.source,
                    "request_size": exp.key.request_size,
                    "response_size": exp.key.response_size,
                    "clients": exp.key.clients,
                    "requests_per_client": exp.key.requests_per_client,
                }
            )

    print(f"[matrix] total unique experiments: {len(experiments)}")
    print(f"[matrix] max required cores (server+clients): {max_required_cpus}")
    print(f"[matrix] start index: {args.start_index}")
    print(f"[matrix] batch dir: {batch_dir}")

    if not args.skip_inject:
        inject_log = batch_dir / "inject.log"
        inject_cmd = ["bash", str(inject_script), str(disk_img)]
        if not args.dry_run:
            rc = run_and_tee(inject_cmd, inject_log)
            if rc != 0:
                print(f"[fatal] inject failed rc={rc}")
                return rc
        else:
            print("[dry-run] skip inject execution")

    if args.checkpoint:
        print(
            "[matrix] note: --checkpoint is ignored; "
            "this script rebuilds checkpoint per experiment with exact CPU count."
        )
    if args.checkpoint_cpus:
        print(
            "[matrix] note: --checkpoint-cpus is ignored; "
            "CPU count is derived per experiment as clients+1."
        )

    experiments_csv = batch_dir / "experiments.csv"
    ticks_csv = batch_dir / "results_ticks.csv"
    done_ids = set()
    if not args.force_rerun:
        done_ids = read_success_exp_ids(experiments_csv)

    exp_fields = [
        "exp_id",
        "source",
        "request_size",
        "response_size",
        "clients",
        "requests_per_client",
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
        "status",
    ]
    tick_fields = [
        "exp_id",
        "request_size",
        "response_size",
        "clients",
        "requests_per_client",
        "client_id",
        "req_index",
        "start_tick",
        "end_tick",
        "delta_tick",
        "output_dir",
    ]

    for idx, exp in enumerate(experiments, start=1):
        key = exp.key
        if idx < args.start_index:
            print(
                f"[skip {idx}/{len(experiments)}] "
                f"before --start-index={args.start_index}: {exp.exp_id}"
            )
            continue
        if exp.exp_id in done_ids:
            print(f"[skip {idx}/{len(experiments)}] already done: {exp.exp_id}")
            continue

        run_outdir = batch_dir / f"run_{idx:02d}_{exp.exp_id}"
        run_outdir.mkdir(parents=True, exist_ok=True)
        run_log = run_outdir / "gem5_run.log"
        required_cpus = key.clients + 1
        ckpt_outdir = (
            batch_dir / f"ckpt_{idx:02d}_{exp.exp_id}" / "cxl_rpc_checkpoint"
        )
        checkpoint_dir: Path = ckpt_outdir

        ckpt_cmd = [
            str(gem5_bin),
            "-d",
            str(ckpt_outdir),
            str(save_ckpt_cfg),
            "--num_cpus",
            str(required_cpus),
        ]
        if args.dry_run:
            print(
                "[dry-run] would build checkpoint:",
                " ".join(shlex.quote(c) for c in ckpt_cmd),
            )
        else:
            ckpt_outdir.mkdir(parents=True, exist_ok=True)
            ckpt_log = run_outdir / "checkpoint_build.log"
            ckpt_rc = run_and_tee(ckpt_cmd, ckpt_log)
            if ckpt_rc != 0:
                append_row(
                    experiments_csv,
                    {
                        "exp_id": exp.exp_id,
                        "source": exp.source,
                        "request_size": key.request_size,
                        "response_size": key.response_size,
                        "clients": key.clients,
                        "requests_per_client": key.requests_per_client,
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
            resolved = resolve_checkpoint_dir(ckpt_outdir)
            if resolved is None:
                append_row(
                    experiments_csv,
                    {
                        "exp_id": exp.exp_id,
                        "source": exp.source,
                        "request_size": key.request_size,
                        "response_size": key.response_size,
                        "clients": key.clients,
                        "requests_per_client": key.requests_per_client,
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

        server_args = f"--silent --response-size {key.response_size}"
        test_cmd = (
            f"CXL_RPC_CLIENT_COUNT={key.clients} "
            f"CXL_RPC_PIN_CORES=1 "
            f"CXL_RPC_SERVER_CORE=0 "
            f"CXL_RPC_CLIENT_CORE_BASE=1 "
            f"CXL_RPC_SERVER_ARGS={shlex.quote(server_args)} "
            f"bash /home/test_code/run_rpc_server_multi_client.sh "
            f"/home/test_code/rpc_server_example /home/test_code/rpc_client_example "
            f"--requests {key.requests_per_client} "
            f"--max-polls {args.max_polls} "
            f"--request-size {key.request_size} "
            f"--response-size {key.response_size} "
            f"--silent"
        )

        gem5_cmd = [
            str(gem5_bin),
            "-d",
            str(run_outdir),
            str(test_cfg),
            "--cpu_type",
            "TIMING",
            "--num_cpus",
            str(required_cpus),
            "--checkpoint",
            str(checkpoint_dir),
            "--test_cmd",
            test_cmd,
        ]

        start_ts = dt.datetime.now().isoformat(timespec="seconds")
        start_time = time.time()
        print(
            f"[run {idx}/{len(experiments)}] {exp.exp_id} "
            f"(req={key.request_size}, resp={key.response_size}, "
            f"clients={key.clients}, reqs/client={key.requests_per_client}, "
            f"cpus={required_cpus})"
        )

        if args.dry_run:
            gem5_rc = 0
            test_cmd_rc = 0
            rows: List[Dict[str, int]] = []
        else:
            gem5_rc = run_and_tee(gem5_cmd, run_log)
            board_path = run_outdir / "board.pc.com_1.device"
            test_cmd_rc, rows = parse_board_ticks(board_path)

        end_time = time.time()
        end_ts = dt.datetime.now().isoformat(timespec="seconds")
        elapsed = end_time - start_time

        expected_rows = key.clients * key.requests_per_client
        if args.dry_run:
            status = "dry_run"
        else:
            status = "ok"
            if gem5_rc != 0:
                status = "gem5_failed"
            elif test_cmd_rc is None:
                status = "missing_test_exit"
            elif test_cmd_rc != 0:
                status = "test_failed"
            elif len(rows) != expected_rows:
                status = "incomplete_ticks"

        append_row(
            experiments_csv,
            {
                "exp_id": exp.exp_id,
                "source": exp.source,
                "request_size": key.request_size,
                "response_size": key.response_size,
                "clients": key.clients,
                "requests_per_client": key.requests_per_client,
                "checkpoint_dir": str(checkpoint_dir),
                "num_cpus": required_cpus,
                "output_dir": str(run_outdir),
                "start_time": start_ts,
                "end_time": end_ts,
                "elapsed_sec": f"{elapsed:.3f}",
                "gem5_rc": gem5_rc,
                "test_cmd_exit": "" if test_cmd_rc is None else test_cmd_rc,
                "tick_rows": len(rows),
                "expected_rows": expected_rows,
                "status": status,
            },
            exp_fields,
        )

        for row in rows:
            append_row(
                ticks_csv,
                {
                    "exp_id": exp.exp_id,
                    "request_size": key.request_size,
                    "response_size": key.response_size,
                    "clients": key.clients,
                    "requests_per_client": key.requests_per_client,
                    "client_id": row["client_id"],
                    "req_index": row["req_index"],
                    "start_tick": row["start_tick"],
                    "end_tick": row["end_tick"],
                    "delta_tick": row["delta_tick"],
                    "output_dir": str(run_outdir),
                },
                tick_fields,
            )

        print(
            f"[done] {exp.exp_id} status={status} "
            f"gem5_rc={gem5_rc} test_rc={test_cmd_rc} "
            f"ticks={len(rows)}/{expected_rows} elapsed={elapsed:.1f}s"
        )

    print("\n[matrix] finished")
    print(f"[matrix] plan: {plan_csv}")
    print(f"[matrix] experiments: {experiments_csv}")
    print(f"[matrix] ticks: {ticks_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
