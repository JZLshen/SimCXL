#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import fcntl
import json
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional


DEFAULT_REPO_ROOT = Path("/home/wq/sh/SimCXL")
DEFAULT_BATCH_ROOT = (
    DEFAULT_REPO_ROOT / "output" / "rpc_current_flat_queue_shared_aligned"
)
DEFAULT_STATE_DIR = DEFAULT_BATCH_ROOT / "queue_run_p13_shared_aligned"

BARE_RUNNER = (
    DEFAULT_REPO_ROOT
    / "tests/test-progs/cxl-rpc/scripts/run_rpc_matrix_kvm_timing_ckpt.py"
)
APP_RUNNER = (
    DEFAULT_REPO_ROOT
    / "tests/test-progs/cxl-rpc/scripts/run_rpc_app_matrix_kvm_timing_ckpt.py"
)

QUEUE_FIELDS = ["kind", "exp_id", "dir_name", "checkpoint_dir"]
RESULT_FIELDS = [
    "timestamp",
    "worker_id",
    "kind",
    "exp_id",
    "dir_name",
    "status",
    "returncode",
    "elapsed_sec",
]


@dataclass(frozen=True)
class Task:
    kind: str
    exp_id: str
    dir_name: str
    checkpoint_dir: str

    def as_row(self) -> Dict[str, str]:
        return {
            "kind": self.kind,
            "exp_id": self.exp_id,
            "dir_name": self.dir_name,
            "checkpoint_dir": self.checkpoint_dir,
        }


def now_ts() -> str:
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def write_rows(path: Path, rows: List[Dict[str, str]], fieldnames: List[str]) -> None:
    with path.open("w", encoding="utf-8", newline="") as fp:
        writer = csv.DictWriter(fp, fieldnames=fieldnames, delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)


def append_row(path: Path, row: Dict[str, str], fieldnames: List[str]) -> None:
    lock_path = path.with_suffix(path.suffix + ".lock")
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a+", encoding="utf-8") as lock_fp:
        fcntl.flock(lock_fp.fileno(), fcntl.LOCK_EX)
        file_exists = path.exists()
        with path.open("a", encoding="utf-8", newline="") as fp:
            writer = csv.DictWriter(fp, fieldnames=fieldnames, delimiter="\t")
            if not file_exists:
                writer.writeheader()
            writer.writerow(row)
        fcntl.flock(lock_fp.fileno(), fcntl.LOCK_UN)


def load_rows(path: Path) -> List[Dict[str, str]]:
    if not path.exists():
        return []
    with path.open("r", encoding="utf-8", newline="") as fp:
        reader = csv.DictReader(fp, delimiter="\t")
        return list(reader)


def latest_experiment_row(exp_dir: Path) -> Optional[Dict[str, str]]:
    experiments_path = exp_dir / "experiments.csv"
    if not experiments_path.exists():
        return None
    with experiments_path.open("r", encoding="utf-8", newline="") as fp:
        rows = list(csv.DictReader(fp))
    if not rows:
        return None
    return rows[-1]


def experiment_completed(exp_dir: Path) -> bool:
    row = latest_experiment_row(exp_dir)
    if row is None:
        return False
    return row.get("gem5_rc") == "0" and row.get("test_cmd_exit") == "0"


def running_dirs(batch_root: Path, repo_root: Path) -> set[str]:
    cmd = ["ps", "-eo", "cmd"]
    try:
        output = subprocess.check_output(cmd, text=True)
    except subprocess.CalledProcessError:
        return set()

    gem5_bin = str(repo_root / "build/X86/gem5.opt")
    batch_root_str = str(batch_root)
    active = set()
    for line in output.splitlines():
        if gem5_bin not in line or batch_root_str not in line:
            continue
        for part in line.split():
            if part.startswith(batch_root_str + "/"):
                rel = part[len(batch_root_str) + 1 :]
                dir_name = rel.split("/", 1)[0]
                if dir_name.startswith(("bare_", "app_")):
                    active.add(dir_name)
    return active


def bare_checkpoint_dir(batch_root: Path, row: Dict[str, str]) -> Path:
    return batch_root / "checkpoints" / (
        f"clients_{row['clients']}"
        f"_lanes_{row['response_lane_count']}"
        f"_mq_{row['mq_entries']}"
        f"_cxlp_{row['cxl_extra_latency_ns']}ns"
    )


def app_checkpoint_dir(batch_root: Path) -> Path:
    return batch_root / "checkpoints" / "clients_32_lanes_32_mq_1024_cxlp_0ns"


def prepare_tasks(batch_root: Path, repo_root: Path) -> List[Task]:
    active = running_dirs(batch_root, repo_root)
    tasks: List[Task] = []

    app_ckpt = app_checkpoint_dir(batch_root)
    if not (app_ckpt / "m5.cpt").exists():
        raise RuntimeError(f"missing app checkpoint: {app_ckpt}")

    app_plan = batch_root / "plans" / "plan_app" / "plan.csv"
    with app_plan.open("r", encoding="utf-8", newline="") as fp:
        for row in csv.DictReader(fp):
            dir_name = f"app_{row['exp_id']}"
            exp_dir = batch_root / dir_name
            if experiment_completed(exp_dir) or dir_name in active:
                continue
            tasks.append(
                Task(
                    kind="app",
                    exp_id=row["exp_id"],
                    dir_name=dir_name,
                    checkpoint_dir=str(app_ckpt),
                )
            )

    bare_plan = batch_root / "plans" / "plan_bare" / "plan.csv"
    with bare_plan.open("r", encoding="utf-8", newline="") as fp:
        for row in csv.DictReader(fp):
            dir_name = f"bare_{row['exp_id']}"
            exp_dir = batch_root / dir_name
            if experiment_completed(exp_dir) or dir_name in active:
                continue
            ckpt_dir = bare_checkpoint_dir(batch_root, row)
            if not (ckpt_dir / "m5.cpt").exists():
                raise RuntimeError(f"missing bare checkpoint for {dir_name}: {ckpt_dir}")
            tasks.append(
                Task(
                    kind="bare",
                    exp_id=row["exp_id"],
                    dir_name=dir_name,
                    checkpoint_dir=str(ckpt_dir),
                )
            )

    return tasks


def prepare_state(batch_root: Path, repo_root: Path, state_dir: Path) -> int:
    tasks = prepare_tasks(batch_root, repo_root)
    state_dir.mkdir(parents=True, exist_ok=True)

    queue_path = state_dir / "queue.tsv"
    done_path = state_dir / "done.tsv"
    failed_path = state_dir / "failed.tsv"
    claimed_path = state_dir / "claimed.tsv"
    meta_path = state_dir / "meta.json"

    write_rows(queue_path, [task.as_row() for task in tasks], QUEUE_FIELDS)
    write_rows(done_path, [], RESULT_FIELDS)
    write_rows(failed_path, [], RESULT_FIELDS)
    write_rows(claimed_path, [], RESULT_FIELDS)

    meta = {
        "created_at": now_ts(),
        "repo_root": str(repo_root),
        "batch_root": str(batch_root),
        "task_count": len(tasks),
    }
    meta_path.write_text(json.dumps(meta, indent=2), encoding="utf-8")

    print(f"[prepare] state_dir={state_dir}")
    print(f"[prepare] queued_tasks={len(tasks)}")
    for task in tasks:
        print(f"[prepare] {task.dir_name} -> {task.checkpoint_dir}")
    return 0


def claim_task(state_dir: Path) -> Optional[Task]:
    queue_path = state_dir / "queue.tsv"
    lock_path = state_dir / "queue.lock"
    lock_path.parent.mkdir(parents=True, exist_ok=True)

    with lock_path.open("a+", encoding="utf-8") as lock_fp:
        fcntl.flock(lock_fp.fileno(), fcntl.LOCK_EX)
        rows = load_rows(queue_path)
        if not rows:
            fcntl.flock(lock_fp.fileno(), fcntl.LOCK_UN)
            return None
        row = rows.pop(0)
        write_rows(queue_path, rows, QUEUE_FIELDS)
        fcntl.flock(lock_fp.fileno(), fcntl.LOCK_UN)
    return Task(**row)


def requeue_task(state_dir: Path, task: Task) -> None:
    queue_path = state_dir / "queue.tsv"
    lock_path = state_dir / "queue.lock"
    with lock_path.open("a+", encoding="utf-8") as lock_fp:
        fcntl.flock(lock_fp.fileno(), fcntl.LOCK_EX)
        rows = load_rows(queue_path)
        rows.append(task.as_row())
        write_rows(queue_path, rows, QUEUE_FIELDS)
        fcntl.flock(lock_fp.fileno(), fcntl.LOCK_UN)


def build_command(repo_root: Path, batch_root: Path, task: Task) -> List[str]:
    common = [
        "--repo-root",
        str(repo_root),
        "--output-base",
        str(batch_root),
        "--batch-name",
        task.dir_name,
        "--checkpoint-dir",
        task.checkpoint_dir,
        "--skip-inject",
        "--allow-concurrent-runs",
        "--inter-experiment-sleep-sec",
        "0",
        "--force-rerun",
        "--only-exp-id",
        task.exp_id,
    ]
    if task.kind == "bare":
        return ["python3", str(BARE_RUNNER), *common]
    if task.kind == "app":
        return ["python3", str(APP_RUNNER), *common]
    raise ValueError(f"unsupported task kind: {task.kind}")


def stream_process(cmd: List[str], log_path: Path) -> int:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("a", encoding="utf-8") as log_fp:
        log_fp.write(f"[{now_ts()}] + {' '.join(cmd)}\n")
        log_fp.flush()
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
            log_fp.write(line)
        rc = proc.wait()
        log_fp.write(f"[{now_ts()}] returncode={rc}\n")
        log_fp.flush()
        return rc


def worker_loop(
    batch_root: Path,
    repo_root: Path,
    state_dir: Path,
    worker_id: str,
    retry_delay_sec: int,
) -> int:
    claimed_path = state_dir / "claimed.tsv"
    done_path = state_dir / "done.tsv"
    failed_path = state_dir / "failed.tsv"
    worker_log = state_dir / "logs" / f"worker_{worker_id}.log"

    while True:
        task = claim_task(state_dir)
        if task is None:
            print(f"[worker {worker_id}] queue empty")
            return 0

        exp_dir = batch_root / task.dir_name
        if experiment_completed(exp_dir):
            append_row(
                done_path,
                {
                    "timestamp": now_ts(),
                    "worker_id": worker_id,
                    "kind": task.kind,
                    "exp_id": task.exp_id,
                    "dir_name": task.dir_name,
                    "status": "already_done",
                    "returncode": "0",
                    "elapsed_sec": "0.000",
                },
                RESULT_FIELDS,
            )
            print(f"[worker {worker_id}] already done: {task.dir_name}")
            continue

        if task.dir_name in running_dirs(batch_root, repo_root):
            requeue_task(state_dir, task)
            print(
                f"[worker {worker_id}] {task.dir_name} is already running elsewhere; "
                f"sleep {retry_delay_sec}s then continue"
            )
            time.sleep(retry_delay_sec)
            continue

        append_row(
            claimed_path,
            {
                "timestamp": now_ts(),
                "worker_id": worker_id,
                "kind": task.kind,
                "exp_id": task.exp_id,
                "dir_name": task.dir_name,
                "status": "claimed",
                "returncode": "",
                "elapsed_sec": "",
            },
            RESULT_FIELDS,
        )

        cmd = build_command(repo_root, batch_root, task)
        print(f"[worker {worker_id}] start {task.dir_name}")
        start = time.time()
        rc = stream_process(cmd, worker_log)
        elapsed = time.time() - start

        result_row = {
            "timestamp": now_ts(),
            "worker_id": worker_id,
            "kind": task.kind,
            "exp_id": task.exp_id,
            "dir_name": task.dir_name,
            "status": "",
            "returncode": str(rc),
            "elapsed_sec": f"{elapsed:.3f}",
        }

        if rc == 0 and experiment_completed(exp_dir):
            result_row["status"] = "done"
            append_row(done_path, result_row, RESULT_FIELDS)
            print(
                f"[worker {worker_id}] done {task.dir_name} "
                f"elapsed={elapsed:.1f}s"
            )
            continue

        if rc == 0:
            result_row["status"] = "missing_summary"
        else:
            result_row["status"] = "failed"
        append_row(failed_path, result_row, RESULT_FIELDS)
        print(
            f"[worker {worker_id}] {result_row['status']} {task.dir_name} "
            f"rc={rc} elapsed={elapsed:.1f}s"
        )


def print_status(state_dir: Path) -> int:
    queue_rows = load_rows(state_dir / "queue.tsv")
    done_rows = load_rows(state_dir / "done.tsv")
    failed_rows = load_rows(state_dir / "failed.tsv")
    claimed_rows = load_rows(state_dir / "claimed.tsv")
    print(f"state_dir={state_dir}")
    print(f"queued={len(queue_rows)}")
    print(f"claimed={len(claimed_rows)}")
    print(f"done={len(done_rows)}")
    print(f"failed={len(failed_rows)}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Queue runner for SimCXL experiments.")
    parser.add_argument(
        "mode",
        choices=["prepare", "worker", "status"],
        help="prepare queue, run one worker, or print queue status",
    )
    parser.add_argument("--repo-root", type=Path, default=DEFAULT_REPO_ROOT)
    parser.add_argument("--batch-root", type=Path, default=DEFAULT_BATCH_ROOT)
    parser.add_argument("--state-dir", type=Path, default=DEFAULT_STATE_DIR)
    parser.add_argument("--worker-id", type=str, default="00")
    parser.add_argument("--retry-delay-sec", type=int, default=60)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    batch_root = args.batch_root.resolve()
    state_dir = args.state_dir.resolve()

    if args.mode == "prepare":
        return prepare_state(batch_root, repo_root, state_dir)
    if args.mode == "worker":
        return worker_loop(
            batch_root=batch_root,
            repo_root=repo_root,
            state_dir=state_dir,
            worker_id=args.worker_id,
            retry_delay_sec=args.retry_delay_sec,
        )
    return print_status(state_dir)


if __name__ == "__main__":
    raise SystemExit(main())
