#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
from datetime import datetime
from pathlib import Path


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


def default_repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def load_manifest(manifest_path: Path) -> list[dict[str, str]]:
    with manifest_path.open("r", encoding="utf-8", newline="") as fp:
        rows = list(csv.DictReader(fp, delimiter="\t"))

    if not rows:
        return []

    required = {"kind", "exp_id", "dir_name"}
    if "checkpoint_rel" in rows[0]:
        required.add("checkpoint_rel")
    elif "checkpoint_dir" in rows[0]:
        required.add("checkpoint_dir")
    else:
        raise ValueError(
            f"manifest must contain checkpoint_rel or checkpoint_dir: {manifest_path}"
        )

    missing = required.difference(rows[0].keys())
    if missing:
        raise ValueError(
            f"manifest is missing required columns {sorted(missing)}: {manifest_path}"
        )

    return rows


def write_rows(
    path: Path,
    fieldnames: list[str],
    rows: list[dict[str, str]],
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as fp:
        writer = csv.DictWriter(fp, fieldnames=fieldnames, delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)


def checkpoint_abs_path(repo_root: Path, row: dict[str, str]) -> Path:
    if "checkpoint_rel" in row and row["checkpoint_rel"]:
        return (repo_root / row["checkpoint_rel"]).resolve()
    return Path(row["checkpoint_dir"]).resolve()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Materialize a SimCXL queue state directory from a committed manifest."
    )
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--state-dir", type=Path, required=True)
    parser.add_argument("--batch-root", type=Path, required=True)
    parser.add_argument("--repo-root", type=Path, default=default_repo_root())
    parser.add_argument(
        "--force",
        action="store_true",
        help="Overwrite an existing state directory.",
    )
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    manifest = args.manifest.resolve()
    state_dir = args.state_dir.resolve()
    batch_root = args.batch_root.resolve()

    if not manifest.is_file():
        raise SystemExit(f"manifest not found: {manifest}")

    if state_dir.exists() and any(state_dir.iterdir()) and not args.force:
        raise SystemExit(
            f"state dir already exists and is non-empty: {state_dir} "
            "(pass --force to overwrite)"
        )

    rows = load_manifest(manifest)

    queue_rows: list[dict[str, str]] = []
    checkpoint_paths: list[str] = []
    checkpoint_rel_paths: list[str] = []

    for row in rows:
        checkpoint_path = checkpoint_abs_path(repo_root, row)
        checkpoint_paths.append(str(checkpoint_path))
        if "checkpoint_rel" in row and row["checkpoint_rel"]:
            checkpoint_rel_paths.append(row["checkpoint_rel"])
        else:
            checkpoint_rel_paths.append(
                str(checkpoint_path.relative_to(repo_root.resolve()))
            )
        queue_rows.append(
            {
                "kind": row["kind"],
                "exp_id": row["exp_id"],
                "dir_name": row["dir_name"],
                "checkpoint_dir": str(checkpoint_path),
            }
        )

    if state_dir.exists():
        for child in state_dir.iterdir():
            if child.is_dir():
                for nested in child.rglob("*"):
                    if nested.is_file() or nested.is_symlink():
                        nested.unlink()
                for nested in sorted(child.rglob("*"), reverse=True):
                    if nested.is_dir():
                        nested.rmdir()
                child.rmdir()
            else:
                child.unlink()
    state_dir.mkdir(parents=True, exist_ok=True)

    write_rows(state_dir / "queue.tsv", QUEUE_FIELDS, queue_rows)
    write_rows(state_dir / "done.tsv", RESULT_FIELDS, [])
    write_rows(state_dir / "failed.tsv", RESULT_FIELDS, [])
    write_rows(state_dir / "claimed.tsv", RESULT_FIELDS, [])

    (state_dir / "checkpoint_dirs.txt").write_text(
        "".join(f"{path}\n" for path in sorted(set(checkpoint_paths))),
        encoding="utf-8",
    )
    (state_dir / "checkpoint_rels.txt").write_text(
        "".join(f"{path}\n" for path in sorted(set(checkpoint_rel_paths))),
        encoding="utf-8",
    )

    meta = {
        "created_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "repo_root": str(repo_root),
        "batch_root": str(batch_root),
        "task_count": len(queue_rows),
        "source_manifest": str(manifest),
    }
    (state_dir / "meta.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")

    print(f"state_dir={state_dir}")
    print(f"task_count={len(queue_rows)}")
    print(f"manifest={manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
