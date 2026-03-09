#!/usr/bin/env python3
"""Extract 1CE x 1ch XFERCAP sweep results and generate CSV/SVG outputs."""

from __future__ import annotations

import csv
import math
import re
import sys
from pathlib import Path


MARKER_RE = re.compile(r"\[ce-xfercap\] configured_xfercap=(?P<label>\S+)")
LANE_RE = re.compile(
    r"copyengine_bw_lane,"
    r"lane=(?P<lane>\d+),"
    r"engine=(?P<engine>\d+),"
    r"resource=(?P<resource>[^,]+),"
    r"channel=(?P<channel>\d+),"
    r"chan_count=(?P<chan_count>\d+),"
    r"xfercap_bytes=(?P<xfercap_bytes>\d+),"
    r"descriptors=(?P<descriptors>\d+),"
    r"dst_offset=(?P<dst_offset>0x[0-9a-fA-F]+)"
)
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


def format_bytes(num_bytes: int) -> str:
    if num_bytes >= 1024 and num_bytes % 1024 == 0:
        value = num_bytes // 1024
        if value >= 1024 and value % 1024 == 0:
            return f"{value // 1024}MiB"
        return f"{value}KiB"
    return f"{num_bytes}B"


def parse_board_log(board_log: Path) -> dict[str, str]:
    marker_label: str | None = None
    lane: dict[str, str] | None = None
    summary: dict[str, str] | None = None
    summary_line = 0

    for lineno, line in enumerate(board_log.read_text(errors="ignore").splitlines(), start=1):
        marker = MARKER_RE.search(line)
        if marker:
            marker_label = marker.group("label")

        lane_match = LANE_RE.search(line)
        if lane_match:
            lane = lane_match.groupdict()

        summary_match = SUMMARY_RE.search(line)
        if summary_match:
            summary = summary_match.groupdict()
            summary_line = lineno

    if lane is None:
        raise SystemExit(f"no copyengine_bw_lane record found in {board_log}")
    if summary is None:
        raise SystemExit(f"no copyengine_bw_parallel_summary record found in {board_log}")

    xfercap_bytes = int(lane["xfercap_bytes"])
    host_log = board_log.with_name(board_log.name.replace(".board.pc.com_1.device", ".host.log"))

    row = dict(summary)
    row["xfercap_bytes"] = str(xfercap_bytes)
    row["xfercap_label"] = marker_label or format_bytes(xfercap_bytes)
    row["descriptors"] = lane["descriptors"]
    row["line"] = str(summary_line)
    row["board_log"] = str(board_log)
    row["host_log"] = str(host_log)
    return row


def parse_raw_dir(raw_dir: Path) -> list[dict[str, str]]:
    board_logs = sorted(raw_dir.glob("*.board.pc.com_1.device"))
    if not board_logs:
        raise SystemExit(f"no board logs found in {raw_dir}")

    rows = [parse_board_log(path.resolve()) for path in board_logs]
    rows.sort(key=lambda row: int(row["xfercap_bytes"]))
    return rows


def write_csv(path: Path, rows: list[dict[str, str]], columns: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as fp:
        writer = csv.DictWriter(fp, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def _nice_ymax(values: list[float]) -> float:
    ymax = max(values)
    return float(max(1, math.ceil((ymax * 1.10) / 0.5) * 0.5))


def write_svg(path: Path, rows: list[dict[str, str]], subtitle: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)

    width = 980
    height = 620
    left = 90
    right = 40
    top = 80
    bottom = 90
    plot_w = width - left - right
    plot_h = height - top - bottom

    xs = [row["xfercap_label"] for row in rows]
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
    lines.append(
        f'<text x="{left}" y="38" font-size="28" font-family="monospace" fill="#111">'
        "DMA XFERCAP Sweep</text>"
    )
    lines.append(
        f'<text x="{left}" y="62" font-size="16" font-family="monospace" fill="#555">'
        f"{subtitle}</text>"
    )

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
        'font-size="18" font-family="monospace" fill="#111">XFERCAP</text>'
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
            "usage: extract_and_plot_dma_xfercap.py RAW_DIR DATA_DIR IMAGES_DIR"
        )

    raw_dir = Path(sys.argv[1]).resolve()
    data_dir = Path(sys.argv[2]).resolve()
    images_dir = Path(sys.argv[3]).resolve()

    rows = parse_raw_dir(raw_dir)

    columns = [
        "xfercap_bytes",
        "xfercap_label",
        "engines",
        "channels",
        "lanes",
        "descriptors",
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
        "host_log",
    ]
    write_csv(data_dir / "xfercap_sweep.csv", rows, columns)

    subtitle = f"gem5 KVM->TIMING, 1 CopyEngine x 1 channel, raw source: {raw_dir}"
    write_svg(images_dir / "xfercap_sweep.svg", rows, subtitle)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
