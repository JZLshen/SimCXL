#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import html
import math
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class ExperimentMeta:
    exp_id: str
    source: str
    request_size: int
    response_size: int
    clients: int
    requests_per_client: int
    status: str


@dataclass(frozen=True)
class TickRow:
    exp_id: str
    request_size: int
    response_size: int
    clients: int
    requests_per_client: int
    client_id: int
    req_index: int
    start_ns: int
    end_ns: int
    latency_ns: int
    output_dir: str


@dataclass(frozen=True)
class ChartRect:
    x: float
    y: float
    w: float
    h: float


def escape(text: str) -> str:
    return html.escape(text, quote=True)


def svg_text(parts: list[str], x: float, y: float, text: str,
             klass: str = "label", anchor: str = "start") -> None:
    parts.append(
        f"<text x='{x:.1f}' y='{y:.1f}' class='{klass}' text-anchor='{anchor}'>"
        f"{escape(text)}</text>"
    )


def percentile(values: Iterable[float], p: float) -> float:
    vals = sorted(values)
    if not vals:
        return 0.0
    if len(vals) == 1:
        return float(vals[0])
    k = (len(vals) - 1) * (p / 100.0)
    lo = math.floor(k)
    hi = math.ceil(k)
    if lo == hi:
        return float(vals[lo])
    return float(vals[lo] * (hi - k) + vals[hi] * (k - lo))


def nice_step(raw_step: float) -> float:
    if raw_step <= 0:
        return 1.0
    exponent = math.floor(math.log10(raw_step))
    fraction = raw_step / (10 ** exponent)
    if fraction <= 1:
        nice_fraction = 1
    elif fraction <= 2:
        nice_fraction = 2
    elif fraction <= 5:
        nice_fraction = 5
    else:
        nice_fraction = 10
    return nice_fraction * (10 ** exponent)


def linear_ticks(min_v: float, max_v: float, count: int = 7) -> list[float]:
    if math.isclose(min_v, max_v):
        return [min_v]
    step = nice_step((max_v - min_v) / max(count - 1, 1))
    start = math.floor(min_v / step) * step
    end = math.ceil(max_v / step) * step
    ticks = []
    cur = start
    for _ in range(256):
        ticks.append(cur)
        cur += step
        if cur > end + step * 0.5:
            break
    return ticks


def integer_ticks(max_value: int, count: int = 8) -> list[int]:
    if max_value <= 0:
        return [0]
    step = max(1, int(nice_step(max_value / max(count - 1, 1))))
    ticks = list(range(0, max_value + 1, step))
    if ticks[-1] != max_value:
        ticks.append(max_value)
    return ticks


def rect_map_x(rect: ChartRect, value: float, min_v: float, max_v: float) -> float:
    if math.isclose(min_v, max_v):
        return rect.x + rect.w / 2.0
    return rect.x + (value - min_v) / (max_v - min_v) * rect.w


def rect_map_y(rect: ChartRect, value: float, min_v: float, max_v: float) -> float:
    if math.isclose(min_v, max_v):
        return rect.y + rect.h / 2.0
    return rect.y + rect.h - (value - min_v) / (max_v - min_v) * rect.h


def format_duration_ns(ns: float) -> str:
    abs_ns = abs(ns)
    if abs_ns >= 1_000_000:
        return f"{ns / 1_000_000.0:.3f} ms"
    if abs_ns >= 1_000:
        return f"{ns / 1_000.0:.1f} us"
    return f"{ns:.0f} ns"


def duration_ms(ns: float) -> float:
    return ns / 1_000_000.0


def hsl_color(index: int, total: int, sat: int = 66, light: int = 48) -> str:
    denom = max(total, 1)
    hue = int(round((360.0 * index) / denom)) % 360
    return f"hsl({hue} {sat}% {light}%)"


def read_experiments(csv_path: Path) -> list[ExperimentMeta]:
    metas: list[ExperimentMeta] = []
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            metas.append(
                ExperimentMeta(
                    exp_id=row["exp_id"],
                    source=row["source"],
                    request_size=int(row["request_size"]),
                    response_size=int(row["response_size"]),
                    clients=int(row["clients"]),
                    requests_per_client=int(row["requests_per_client"]),
                    status=row["status"],
                )
            )
    return metas


def read_ticks(csv_path: Path) -> list[TickRow]:
    rows: list[TickRow] = []
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(
                TickRow(
                    exp_id=row["exp_id"],
                    request_size=int(row["request_size"]),
                    response_size=int(row["response_size"]),
                    clients=int(row["clients"]),
                    requests_per_client=int(row["requests_per_client"]),
                    client_id=int(row["node_id"]),
                    req_index=int(row["req_index"]),
                    start_ns=int(row["start_tick"]),
                    end_ns=int(row["end_tick"]),
                    latency_ns=int(row["delta_tick"]),
                    output_dir=row["output_dir"],
                )
            )
    return rows


def draw_axes(parts: list[str], rect: ChartRect,
              x_ticks: list[float], y_ticks: list[int],
              x_min: float, x_max: float,
              y_min: float, y_max: float,
              x_label: str, y_label: str) -> None:
    parts.append(
        f"<rect x='{rect.x:.1f}' y='{rect.y:.1f}' width='{rect.w:.1f}' "
        f"height='{rect.h:.1f}' class='panel' />"
    )

    for tick in y_ticks:
        y = rect_map_y(rect, float(tick), y_min, y_max)
        parts.append(
            f"<line x1='{rect.x:.1f}' y1='{y:.1f}' x2='{rect.x + rect.w:.1f}' "
            f"y2='{y:.1f}' class='grid' />"
        )
        svg_text(parts, rect.x - 10, y + 4, str(int(tick)), anchor="end")

    for tick in x_ticks:
        x = rect_map_x(rect, tick, x_min, x_max)
        parts.append(
            f"<line x1='{x:.1f}' y1='{rect.y:.1f}' x2='{x:.1f}' "
            f"y2='{rect.y + rect.h:.1f}' class='grid' />"
        )
        svg_text(parts, x, rect.y + rect.h + 22, f"{tick:.1f}", anchor="middle")

    parts.append(
        f"<line x1='{rect.x:.1f}' y1='{rect.y + rect.h:.1f}' "
        f"x2='{rect.x + rect.w:.1f}' y2='{rect.y + rect.h:.1f}' class='axis' />"
    )
    parts.append(
        f"<line x1='{rect.x:.1f}' y1='{rect.y:.1f}' "
        f"x2='{rect.x:.1f}' y2='{rect.y + rect.h:.1f}' class='axis' />"
    )

    svg_text(parts, rect.x + rect.w / 2.0, rect.y + rect.h + 50, x_label,
             klass="axis-label", anchor="middle")
    parts.append(
        f"<text x='{rect.x - 60:.1f}' y='{rect.y + rect.h / 2.0:.1f}' "
        f"class='axis-label' text-anchor='middle' "
        f"transform='rotate(-90 {rect.x - 60:.1f} {rect.y + rect.h / 2.0:.1f})'>"
        f"{escape(y_label)}</text>"
    )


def write_experiment_svg(meta: ExperimentMeta, rows: list[TickRow],
                         out_path: Path) -> dict[str, str]:
    rows = sorted(rows, key=lambda row: (row.client_id, row.req_index))
    start_min = min(row.start_ns for row in rows)
    end_max = max(row.end_ns for row in rows)
    max_client = max(row.client_id for row in rows)
    max_req = max(row.req_index for row in rows)
    span_ns = end_max - start_min

    latency_vals = [float(row.latency_ns) for row in rows]
    req0_rows = [row for row in rows if row.req_index == 0]
    req_last_rows = [row for row in rows if row.req_index == max_req]
    req0_end_spread = max(row.end_ns for row in req0_rows) - min(row.end_ns for row in req0_rows)
    req_last_end_spread = max(row.end_ns for row in req_last_rows) - min(row.end_ns for row in req_last_rows)

    width = 1880
    height = 1540
    top_rect = ChartRect(110, 160, 1660, 520)
    bottom_rect = ChartRect(110, 860, 1660, 520)

    x_max_ms = max(duration_ms(span_ns) * 1.02, 0.1)
    x_ticks = linear_ticks(0.0, x_max_ms, 8)
    client_ticks = integer_ticks(max_client, 8)
    req_ticks = integer_ticks(max_req, 7)

    parts = [
        f"<svg xmlns='http://www.w3.org/2000/svg' width='{width}' height='{height}' "
        f"viewBox='0 0 {width} {height}'>",
        "<style>",
        "svg { background: #fffdf8; }",
        ".title { font: 700 24px sans-serif; fill: #1d1d1b; }",
        ".subtitle { font: 500 14px sans-serif; fill: #57534e; }",
        ".section { font: 700 16px sans-serif; fill: #1f2937; }",
        ".label { font: 12px sans-serif; fill: #444; }",
        ".axis-label { font: 13px sans-serif; fill: #222; }",
        ".panel { fill: #fff; stroke: #d7cfc1; stroke-width: 1.2; }",
        ".grid { stroke: #ece4d8; stroke-width: 1; }",
        ".axis { stroke: #766b5d; stroke-width: 1.4; }",
        ".hint { font: 12px sans-serif; fill: #6a6258; }",
        ".stats { font: 13px sans-serif; fill: #303030; }",
        "</style>",
    ]

    title = (
        f"{meta.exp_id}  |  full request-response round-trip timeline  |  "
        f"req={meta.request_size}B resp={meta.response_size}B "
        f"clients={meta.clients} reqs/client={meta.requests_per_client}"
    )
    svg_text(parts, 110, 52, title, klass="title")
    svg_text(
        parts,
        110,
        78,
        "Top: grouped by client, color=request index. Bottom: grouped by request index, color=client id.",
        klass="subtitle",
    )
    svg_text(
        parts,
        110,
        100,
        "Each thin bar spans start_tick -> end_tick. Bars are slightly offset inside each row to avoid total overlap.",
        klass="subtitle",
    )

    stat_lines = [
        f"makespan: {format_duration_ns(span_ns)}",
        f"latency p50: {format_duration_ns(percentile(latency_vals, 50.0))}",
        f"latency p95: {format_duration_ns(percentile(latency_vals, 95.0))}",
        f"latency max: {format_duration_ns(max(latency_vals))}",
        f"req0 completion spread: {format_duration_ns(req0_end_spread)}",
        f"req{max_req} completion spread: {format_duration_ns(req_last_end_spread)}",
    ]
    for idx, line in enumerate(stat_lines):
        svg_text(parts, 1120, 52 + idx * 18, line, klass="stats")

    svg_text(parts, top_rect.x, top_rect.y - 18,
             "Per-client full round-trip intervals", klass="section")
    draw_axes(
        parts,
        top_rect,
        x_ticks,
        client_ticks,
        0.0,
        x_max_ms,
        0.0,
        float(max(max_client, 1)),
        "Relative Time From Earliest Start (ms)",
        "Client ID",
    )
    svg_text(
        parts,
        top_rect.x + 8,
        top_rect.y + 18,
        "Within each client row, request intervals are vertically offset by request index.",
        klass="hint",
    )

    top_band_half = min(6.0, max(top_rect.h / max(meta.clients, 1) / 2.6, 2.0))
    for row in rows:
        x1 = rect_map_x(top_rect, duration_ms(row.start_ns - start_min), 0.0, x_max_ms)
        x2 = rect_map_x(top_rect, duration_ms(row.end_ns - start_min), 0.0, x_max_ms)
        base_y = rect_map_y(top_rect, float(row.client_id), 0.0, float(max(max_client, 1)))
        if max_req > 0:
            rel = row.req_index / float(max_req)
            y_offset = (rel - 0.5) * 2.0 * top_band_half
        else:
            y_offset = 0.0
        y = base_y + y_offset
        color = hsl_color(row.req_index, max_req + 1, sat=70, light=48)
        parts.append(
            f"<line x1='{x1:.2f}' y1='{y:.2f}' x2='{x2:.2f}' y2='{y:.2f}' "
            f"stroke='{color}' stroke-width='2.0' stroke-opacity='0.82' "
            f"stroke-linecap='round'>"
            f"<title>client={row.client_id} req={row.req_index} "
            f"start={format_duration_ns(row.start_ns - start_min)} "
            f"end={format_duration_ns(row.end_ns - start_min)} "
            f"latency={format_duration_ns(row.latency_ns)}</title></line>"
        )

    svg_text(parts, bottom_rect.x, bottom_rect.y - 18,
             "Per-request-index full round-trip intervals", klass="section")
    draw_axes(
        parts,
        bottom_rect,
        x_ticks,
        req_ticks,
        0.0,
        x_max_ms,
        0.0,
        float(max(max_req, 1)),
        "Relative Time From Earliest Start (ms)",
        "Request Index",
    )
    svg_text(
        parts,
        bottom_rect.x + 8,
        bottom_rect.y + 18,
        "Within each request-index row, client intervals are vertically offset by client id.",
        klass="hint",
    )

    bottom_band_half = min(6.0, max(bottom_rect.h / max(meta.requests_per_client, 1) / 2.6, 2.0))
    for row in rows:
        x1 = rect_map_x(bottom_rect, duration_ms(row.start_ns - start_min), 0.0, x_max_ms)
        x2 = rect_map_x(bottom_rect, duration_ms(row.end_ns - start_min), 0.0, x_max_ms)
        base_y = rect_map_y(bottom_rect, float(row.req_index), 0.0, float(max(max_req, 1)))
        if max_client > 0:
            rel = row.client_id / float(max_client)
            y_offset = (rel - 0.5) * 2.0 * bottom_band_half
        else:
            y_offset = 0.0
        y = base_y + y_offset
        color = hsl_color(row.client_id, max_client + 1, sat=60, light=42)
        parts.append(
            f"<line x1='{x1:.2f}' y1='{y:.2f}' x2='{x2:.2f}' y2='{y:.2f}' "
            f"stroke='{color}' stroke-width='1.8' stroke-opacity='0.72' "
            f"stroke-linecap='round'>"
            f"<title>req={row.req_index} client={row.client_id} "
            f"start={format_duration_ns(row.start_ns - start_min)} "
            f"end={format_duration_ns(row.end_ns - start_min)} "
            f"latency={format_duration_ns(row.latency_ns)}</title></line>"
        )

    parts.append("</svg>")
    out_path.write_text("\n".join(parts), encoding="utf-8")

    return {
        "exp_id": meta.exp_id,
        "source": meta.source,
        "request_size": str(meta.request_size),
        "response_size": str(meta.response_size),
        "clients": str(meta.clients),
        "requests_per_client": str(meta.requests_per_client),
        "makespan": format_duration_ns(span_ns),
        "latency_p50": format_duration_ns(percentile(latency_vals, 50.0)),
        "latency_p95": format_duration_ns(percentile(latency_vals, 95.0)),
        "req0_completion_spread": format_duration_ns(req0_end_spread),
        "req_last_completion_spread": format_duration_ns(req_last_end_spread),
        "svg_name": out_path.name,
    }


def write_summary_csv(out_dir: Path, summaries: list[dict[str, str]]) -> None:
    fields = [
        "exp_id",
        "source",
        "request_size",
        "response_size",
        "clients",
        "requests_per_client",
        "makespan",
        "latency_p50",
        "latency_p95",
        "req0_completion_spread",
        "req_last_completion_spread",
        "svg_name",
    ]
    with (out_dir / "summary.csv").open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for item in summaries:
            writer.writerow({field: item[field] for field in fields})


def write_index(out_dir: Path, summaries: list[dict[str, str]], batch_dir: Path) -> None:
    rows = []
    rows.append("<!doctype html>")
    rows.append("<html lang='en'><head><meta charset='utf-8'>")
    rows.append("<title>RPC Round-Trip Timing Plots</title>")
    rows.append("<style>")
    rows.append("body { font-family: sans-serif; margin: 24px; background: #fffdf8; color: #222; }")
    rows.append("table { border-collapse: collapse; width: 100%; margin-bottom: 28px; }")
    rows.append("th, td { border: 1px solid #d9cfbf; padding: 8px 10px; text-align: left; }")
    rows.append("th { background: #f4eee2; }")
    rows.append("img { width: 100%; max-width: 1880px; border: 1px solid #d9cfbf; background: #fff; }")
    rows.append("code { background: #f3efe5; padding: 1px 4px; }")
    rows.append("</style></head><body>")
    rows.append("<h1>RPC Full Round-Trip Timing Plots</h1>")
    rows.append(f"<p>Batch directory: <code>{escape(str(batch_dir))}</code></p>")
    rows.append(f"<p>Completed experiments plotted: <strong>{len(summaries)}</strong></p>")
    rows.append("<table><thead><tr>")
    for head in [
        "exp_id",
        "source",
        "request_size",
        "response_size",
        "clients",
        "requests_per_client",
        "makespan",
        "latency_p50",
        "latency_p95",
        "req0_completion_spread",
        "req_last_completion_spread",
        "svg",
    ]:
        rows.append(f"<th>{escape(head)}</th>")
    rows.append("</tr></thead><tbody>")
    for item in summaries:
        rows.append("<tr>")
        for key in [
            "exp_id",
            "source",
            "request_size",
            "response_size",
            "clients",
            "requests_per_client",
            "makespan",
            "latency_p50",
            "latency_p95",
            "req0_completion_spread",
            "req_last_completion_spread",
        ]:
            rows.append(f"<td>{escape(item[key])}</td>")
        rows.append(
            f"<td><a href='{escape(item['svg_name'])}'>{escape(item['svg_name'])}</a></td>"
        )
        rows.append("</tr>")
    rows.append("</tbody></table>")

    for item in summaries:
        rows.append(f"<h2>{escape(item['exp_id'])}</h2>")
        rows.append(
            f"<p><img src='{escape(item['svg_name'])}' alt='{escape(item['exp_id'])} round-trip plot' /></p>"
        )

    rows.append("</body></html>")
    (out_dir / "index.html").write_text("\n".join(rows), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Plot full request-response round-trip timing as standalone SVG files."
    )
    parser.add_argument("batch_dir", type=Path, help="Matrix output directory")
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=None,
        help="Output directory for generated SVG files (default: <batch>/round_trip_timing_plots)",
    )
    args = parser.parse_args()

    batch_dir = args.batch_dir.resolve()
    experiments_csv = batch_dir / "experiments.csv"
    ticks_csv = batch_dir / "results_ticks.csv"
    if not experiments_csv.exists():
        raise SystemExit(f"missing file: {experiments_csv}")
    if not ticks_csv.exists():
        raise SystemExit(f"missing file: {ticks_csv}")

    out_dir = (args.out_dir.resolve()
               if args.out_dir
               else batch_dir / "round_trip_timing_plots")
    out_dir.mkdir(parents=True, exist_ok=True)

    metas = read_experiments(experiments_csv)
    ticks = read_ticks(ticks_csv)
    rows_by_exp: dict[str, list[TickRow]] = defaultdict(list)
    for row in ticks:
        rows_by_exp[row.exp_id].append(row)

    summaries: list[dict[str, str]] = []
    for meta in metas:
        if meta.status != "ok":
            continue
        rows = rows_by_exp.get(meta.exp_id)
        if not rows:
            continue
        svg_path = out_dir / f"{meta.exp_id}.svg"
        summaries.append(write_experiment_svg(meta, rows, svg_path))

    write_summary_csv(out_dir, summaries)
    write_index(out_dir, summaries, batch_dir)

    print(f"generated {len(summaries)} SVG plots under {out_dir}")
    print(f"index: {out_dir / 'index.html'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
