#!/usr/bin/env python3
"""Extract DMA sweep results from gem5 serial log and generate CSV/SVG plots."""

from __future__ import annotations

import csv
import math
import re
import sys
from pathlib import Path


ENGINE_MARKER_RE = re.compile(r"\[ce-matrix\] engine_sweep N=(\d+)")
CHANNEL_MARKER_RE = re.compile(r"\[ce-matrix\] channel_sweep C=(\d+)")
SUMMARY_RE = re.compile(
    r"copyengine_bw_parallel_summary,"
    r"engines=(?P<engines>\d+),"
    r"channels_per_engine=(?P<channels>\d+),"
    r"lanes=(?P<lanes>\d+),"
    r"total_bytes_per_lane=(?P<total_bytes_per_lane>\d+),"
    r"aggregate_bytes=(?P<aggregate_bytes>\d+),"
    r"loops=(?P<loops>\d+),"
    r"min_ns=(?P<min_ns>\d+),"
    r"avg_ns=(?P<avg_ns>\d+),"
    r"p50_ns=(?P<p50_ns>\d+),"
    r"p90_ns=(?P<p90_ns>\d+),"
    r"p95_ns=(?P<p95_ns>\d+),"
    r"p99_ns=(?P<p99_ns>\d+),"
    r"max_ns=(?P<max_ns>\d+),"
    r"best_total_gib_per_s=(?P<best_total_gib_per_s>[0-9.]+),"
    r"avg_total_gib_per_s=(?P<avg_total_gib_per_s>[0-9.]+),"
    r"p50_total_gib_per_s=(?P<p50_total_gib_per_s>[0-9.]+),"
    r"p99_total_gib_per_s=(?P<p99_total_gib_per_s>[0-9.]+),"
    r"worst_total_gib_per_s=(?P<worst_total_gib_per_s>[0-9.]+),"
    r"avg_per_lane_gib_per_s=(?P<avg_per_lane_gib_per_s>[0-9.]+)"
)


def parse_log(board_log: Path) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    engine_records: list[dict[str, str]] = []
    channel_records: list[dict[str, str]] = []
    current_mode: str | None = None
    current_n: int | None = None

    for lineno, line in enumerate(board_log.read_text(errors="ignore").splitlines(), start=1):
        marker = ENGINE_MARKER_RE.search(line)
        if marker:
            current_mode = "engine"
            current_n = int(marker.group(1))
            continue

        marker = CHANNEL_MARKER_RE.search(line)
        if marker:
            current_mode = "channel"
            current_n = int(marker.group(1))
            continue

        summary = SUMMARY_RE.search(line)
        if not summary or current_mode is None or current_n is None:
            continue

        row = summary.groupdict()
        row["line"] = str(lineno)
        row["n"] = str(current_n)
        row["board_log"] = str(board_log)

        if current_mode == "engine":
            engine_records.append(row)
        else:
            channel_records.append(row)

    if not engine_records:
        raise SystemExit(f"no engine sweep records found in {board_log}")
    if not channel_records:
        raise SystemExit(f"no channel sweep records found in {board_log}")
    return engine_records, channel_records


def write_csv(path: Path, rows: list[dict[str, str]], columns: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as fp:
        writer = csv.DictWriter(fp, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def _nice_ymax(values: list[float]) -> float:
    ymax = max(values)
    return float(max(5, math.ceil((ymax * 1.10) / 2.0) * 2))


def write_svg(
    path: Path,
    rows: list[dict[str, str]],
    x_label: str,
    title: str,
    subtitle: str,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)

    width = 980
    height = 620
    left = 90
    right = 40
    top = 80
    bottom = 90
    plot_w = width - left - right
    plot_h = height - top - bottom

    xs = [int(row["n"]) for row in rows]
    ys = [float(row["avg_total_gib_per_s"]) for row in rows]
    y_max = _nice_ymax(ys)
    y_ticks = 6

    step_x = plot_w / max(1, len(xs) - 1)
    points: list[tuple[float, float]] = []
    for idx, y in enumerate(ys):
        px = left + idx * step_x
        py = top + plot_h - (y / y_max) * plot_h
        points.append((px, py))

    polyline = " ".join(f"{px:.1f},{py:.1f}" for px, py in points)

    lines: list[str] = []
    lines.append(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}">'
    )
    lines.append('<rect width="100%" height="100%" fill="#fbfaf5"/>')
    lines.append(f'<text x="{left}" y="38" font-size="28" font-family="monospace" fill="#111">{title}</text>')
    lines.append(f'<text x="{left}" y="62" font-size="16" font-family="monospace" fill="#555">{subtitle}</text>')

    for i in range(y_ticks + 1):
        value = y_max * i / y_ticks
        py = top + plot_h - (value / y_max) * plot_h
        lines.append(
            f'<line x1="{left}" y1="{py:.1f}" x2="{left + plot_w}" y2="{py:.1f}" '
            'stroke="#d9d4c7" stroke-width="1"/>'
        )
        lines.append(
            f'<text x="{left - 12}" y="{py + 5:.1f}" text-anchor="end" '
            'font-size="14" font-family="monospace" fill="#444">'
            f'{value:.1f}</text>'
        )

    lines.append(
        f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top + plot_h}" stroke="#333" stroke-width="2"/>'
    )
    lines.append(
        f'<line x1="{left}" y1="{top + plot_h}" x2="{left + plot_w}" y2="{top + plot_h}" stroke="#333" stroke-width="2"/>'
    )

    for idx, x_val in enumerate(xs):
        px = left + idx * step_x
        lines.append(
            f'<line x1="{px:.1f}" y1="{top + plot_h}" x2="{px:.1f}" y2="{top + plot_h + 8}" '
            'stroke="#333" stroke-width="2"/>'
        )
        lines.append(
            f'<text x="{px:.1f}" y="{top + plot_h + 28}" text-anchor="middle" '
            'font-size="15" font-family="monospace" fill="#222">'
            f'{x_val}</text>'
        )

    lines.append(
        f'<polyline fill="none" stroke="#0c7c59" stroke-width="4" points="{polyline}"/>'
    )

    for (px, py), row in zip(points, rows):
        y = float(row["avg_total_gib_per_s"])
        lines.append(f'<circle cx="{px:.1f}" cy="{py:.1f}" r="5.5" fill="#c43d2f"/>')
        lines.append(
            f'<text x="{px:.1f}" y="{py - 12:.1f}" text-anchor="middle" '
            'font-size="14" font-family="monospace" fill="#222">'
            f'{y:.3f}</text>'
        )

    lines.append(
        f'<text x="{left + plot_w / 2:.1f}" y="{height - 28}" text-anchor="middle" '
        'font-size="18" font-family="monospace" fill="#111">'
        f'{x_label}</text>'
    )
    lines.append(
        f'<text x="26" y="{top + plot_h / 2:.1f}" text-anchor="middle" '
        'font-size="18" font-family="monospace" fill="#111" '
        f'transform="rotate(-90 26 {top + plot_h / 2:.1f})">Bandwidth (GiB/s)</text>'
    )
    lines.append("</svg>")

    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: extract_and_plot_dma.py BOARD_LOG DATA_DIR IMAGES_DIR"
        )

    board_log = Path(sys.argv[1]).resolve()
    data_dir = Path(sys.argv[2]).resolve()
    images_dir = Path(sys.argv[3]).resolve()

    engine_rows, channel_rows = parse_log(board_log)

    common_cols = [
        "n",
        "engines",
        "channels",
        "lanes",
        "total_bytes_per_lane",
        "aggregate_bytes",
        "loops",
        "min_ns",
        "avg_ns",
        "p50_ns",
        "p90_ns",
        "p95_ns",
        "p99_ns",
        "max_ns",
        "best_total_gib_per_s",
        "avg_total_gib_per_s",
        "p50_total_gib_per_s",
        "p99_total_gib_per_s",
        "worst_total_gib_per_s",
        "avg_per_lane_gib_per_s",
        "line",
        "board_log",
    ]
    write_csv(data_dir / "engine_sweep.csv", engine_rows, common_cols)
    write_csv(data_dir / "channel_sweep.csv", channel_rows, common_cols)

    subtitle = (
        "gem5 KVM->TIMING, raw source: data/raw/ce_matrix_16e16c_20260310_010640.board.pc.com_1.device"
    )
    write_svg(
        images_dir / "engine_sweep.svg",
        engine_rows,
        "N (CopyEngine count, 1 channel each)",
        "DMA Engine Sweep",
        subtitle,
    )
    write_svg(
        images_dir / "channel_sweep.svg",
        channel_rows,
        "N (channel count on a single CopyEngine)",
        "DMA Channel Sweep",
        subtitle,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
