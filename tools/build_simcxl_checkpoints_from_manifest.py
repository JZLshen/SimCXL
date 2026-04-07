#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import os
import re
import subprocess
from pathlib import Path


LABEL_RE = re.compile(
    r"^clients_(?P<clients>\d+)_lanes_(?P<lanes>\d+)_mq_(?P<mq>\d+)_cxlp_(?P<cxlp>\d+)ns$"
)


def default_repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def load_manifest_paths(manifest_path: Path) -> list[str]:
    with manifest_path.open("r", encoding="utf-8", newline="") as fp:
        rows = list(csv.DictReader(fp, delimiter="\t"))

    if not rows:
        return []

    if "checkpoint_rel" in rows[0]:
        key = "checkpoint_rel"
    elif "checkpoint_dir" in rows[0]:
        key = "checkpoint_dir"
    else:
        raise SystemExit(
            f"manifest must contain checkpoint_rel or checkpoint_dir: {manifest_path}"
        )

    return sorted({row[key] for row in rows if row.get(key)})


def quote_cmd(parts: list[str]) -> str:
    return " ".join(subprocess.list2cmdline([part]) for part in parts)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build missing SimCXL checkpoints required by a queue manifest."
    )
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--repo-root", type=Path, default=default_repo_root())
    parser.add_argument("--gem5-bin", type=Path, default=None)
    parser.add_argument("--save-config", type=Path, default=None)
    parser.add_argument("--disk", type=Path, default=None)
    parser.add_argument("--kernel", type=Path, default=None)
    parser.add_argument(
        "--boot-cpu-type",
        choices=["KVM", "TIMING", "ATOMIC"],
        default="KVM",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Rebuild checkpoints even if m5.cpt already exists.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print checkpoint build commands without executing them.",
    )
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    manifest = args.manifest.resolve()
    gem5_bin = (args.gem5_bin or (repo_root / "build/X86/gem5.opt")).resolve()
    save_cfg = (
        args.save_config
        or (repo_root / "configs/example/gem5_library/x86-cxl-rpc-save-checkpoint.py")
    ).resolve()
    disk = (args.disk or (repo_root / "files/parsec.img")).resolve()
    kernel = (args.kernel or (repo_root / "files/vmlinux")).resolve()

    if not manifest.is_file():
        raise SystemExit(f"manifest not found: {manifest}")
    if not gem5_bin.is_file():
        raise SystemExit(f"gem5 binary not found: {gem5_bin}")
    if not save_cfg.is_file():
        raise SystemExit(f"save-checkpoint config not found: {save_cfg}")
    if not disk.is_file():
        raise SystemExit(f"disk image not found: {disk}")
    if not kernel.is_file():
        raise SystemExit(f"kernel image not found: {kernel}")

    command_prefix: list[str] = []
    if os.geteuid() != 0:
        sudo_path = subprocess.run(
            ["bash", "-lc", "command -v sudo >/dev/null 2>&1 && printf yes || printf no"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        if sudo_path != "yes":
            raise SystemExit("sudo is required when not running as root")
        probe = subprocess.run(
            ["sudo", "-n", "true"],
            capture_output=True,
            text=True,
        )
        if probe.returncode != 0:
            raise SystemExit(
                "sudo -n failed; configure passwordless sudo or pre-authenticate first"
            )
        command_prefix = ["sudo", "-n"]

    checkpoint_entries = load_manifest_paths(manifest)
    print(f"manifest={manifest}")
    print(f"checkpoint_count={len(checkpoint_entries)}")

    for entry in checkpoint_entries:
        ckpt_dir = (repo_root / entry).resolve() if not entry.startswith("/") else Path(entry).resolve()
        label = ckpt_dir.name
        match = LABEL_RE.match(label)
        if match is None:
            raise SystemExit(f"cannot parse checkpoint label: {ckpt_dir}")

        if (ckpt_dir / "m5.cpt").exists() and not args.force:
            print(f"[skip] {ckpt_dir}")
            continue

        ckpt_dir.mkdir(parents=True, exist_ok=True)
        clients = int(match.group("clients"))
        lanes = int(match.group("lanes"))
        mq_entries = int(match.group("mq"))
        cxl_extra_latency_ns = int(match.group("cxlp"))
        num_cpus = clients + 1

        cmd = [
            *command_prefix,
            str(gem5_bin),
            "-d",
            str(ckpt_dir),
            str(save_cfg),
            "--disk",
            str(disk),
            "--kernel",
            str(kernel),
            "--boot_cpu_type",
            args.boot_cpu_type,
            "--num_cpus",
            str(num_cpus),
            "--rpc_client_count",
            str(clients),
            "--rpc_response_lane_count",
            str(lanes),
            "--rpc_metadata_entries",
            str(mq_entries),
            "--cxl_extra_latency_ns",
            str(cxl_extra_latency_ns),
        ]
        print(f"[build] {ckpt_dir}")
        print(f"[cmd] {' '.join(cmd)}")
        if not args.dry_run:
            subprocess.run(cmd, check=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
