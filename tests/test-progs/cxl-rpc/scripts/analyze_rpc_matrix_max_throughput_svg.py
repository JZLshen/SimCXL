#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import html
import math
from bisect import insort
from dataclasses import dataclass
from pathlib import Path


PALETTE = [
    "#0f4c81",
    "#d1495b",
    "#2f7d32",
    "#d17b0f",
    "#008b8b",
    "#8b1e3f",
    "#4f6d7a",
    "#6d597a",
]

GROUP_SPECS = [
    {
        "slug": "client_sweep",
        "title": "Client Sweep",
        "subtitle": "req=64B, resp=64B; compare different client counts",
        "x_label": "Clients",
        "x_field": "clients",
    },
    {
        "slug": "response_size_sweep",
        "title": "Response Size Sweep",
        "subtitle": "req=64B, clients=16; compare different response sizes",
        "x_label": "Response Size (B)",
        "x_field": "response_size",
    },
    {
        "slug": "request_size_sweep",
        "title": "Request Size Sweep",
        "subtitle": "resp=64B, clients=16; compare different request sizes",
        "x_label": "Request Size (B)",
        "x_field": "request_size",
    },
]


@dataclass(frozen=True)
class ExperimentMeta:
    exp_id: str
    source: str
    request_size: int
    response_size: int
    clients: int
    requests_per_client: int
    status: str
    output_dir: str


@dataclass(frozen=True)
class TickRow:
    exp_id: str
    request_size: int
    response_size: int
    clients: int
    requests_per_client: int
    client_id: int
    req_index: int
    start_tick: int
    end_tick: int
    delta_tick: int
    output_dir: str


@dataclass(frozen=True)
class ChartRect:
    x: float
    y: float
    w: float
    h: float


@dataclass(frozen=True)
class WindowSummary:
    exp_id: str
    source: str
    group_slug: str
    request_size: int
    response_size: int
    clients: int
    requests_per_client: int
    output_dir: str
    total_requests: int
    window_request_count: int
    window_start_tick: int
    window_end_tick: int
    window_span_ns: int
    window_start_offset_ns: int
    window_end_offset_ns: int
    avg_latency_ns: float
    max_throughput_req_per_s: float
    first_client_id: int
    first_req_index: int
    last_client_id: int
    last_req_index: int


def escape(text: str) -> str:
    return html.escape(text, quote=True)


def svg_text(parts: list[str], x: float, y: float, text: str,
             klass: str = "label", anchor: str = "start") -> None:
    parts.append(
        f"<text x='{x:.1f}' y='{y:.1f}' class='{klass}' text-anchor='{anchor}'>"
        f"{escape(text)}</text>"
    )


def line_path(points: list[tuple[float, float]]) -> str:
    if not points:
        return ""
    return "M " + " L ".join(f"{x:.2f} {y:.2f}" for x, y in points)


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


def linear_ticks(min_v: float, max_v: float, count: int = 6) -> list[float]:
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
    if abs_ns >= 1_000_000_000:
        return f"{ns / 1_000_000_000.0:.3f} s"
    if abs_ns >= 1_000_000:
        return f"{ns / 1_000_000.0:.3f} ms"
    if abs_ns >= 1_000:
        return f"{ns / 1_000.0:.3f} us"
    return f"{ns:.0f} ns"


def format_throughput_kreq(throughput_req_per_s: float) -> str:
    return f"{throughput_req_per_s / 1000.0:.3f}"


def read_experiments(csv_path: Path) -> dict[str, ExperimentMeta]:
    metas: dict[str, ExperimentMeta] = {}
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            metas[row["exp_id"]] = ExperimentMeta(
                exp_id=row["exp_id"],
                source=row["source"],
                request_size=int(row["request_size"]),
                response_size=int(row["response_size"]),
                clients=int(row["clients"]),
                requests_per_client=int(row["requests_per_client"]),
                status=row["status"],
                output_dir=row["output_dir"],
            )
    return metas


def read_ticks(csv_path: Path) -> dict[str, list[TickRow]]:
    rows_by_exp: dict[str, list[TickRow]] = {}
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            tick = TickRow(
                exp_id=row["exp_id"],
                request_size=int(row["request_size"]),
                response_size=int(row["response_size"]),
                clients=int(row["clients"]),
                requests_per_client=int(row["requests_per_client"]),
                client_id=int(row["node_id"]),
                req_index=int(row["req_index"]),
                start_tick=int(row["start_tick"]),
                end_tick=int(row["end_tick"]),
                delta_tick=int(row["delta_tick"]),
                output_dir=row["output_dir"],
            )
            rows_by_exp.setdefault(tick.exp_id, []).append(tick)
    return rows_by_exp


def source_has_group(source: str, group_slug: str) -> bool:
    return group_slug in {item.strip() for item in source.split(",") if item.strip()}


def group_meta_sort_key(spec: dict[str, object], meta: ExperimentMeta) -> tuple[object, ...]:
    x_field = str(spec["x_field"])
    return (
        getattr(meta, x_field),
        meta.clients,
        meta.request_size,
        meta.response_size,
        meta.requests_per_client,
        meta.exp_id,
    )


def select_group_metas(spec: dict[str, object],
                       metas: dict[str, ExperimentMeta]) -> list[ExperimentMeta]:
    group_slug = str(spec["slug"])
    selected = [
        meta for meta in metas.values()
        if source_has_group(meta.source, group_slug)
    ]
    return sorted(selected, key=lambda meta: group_meta_sort_key(spec, meta))


def compare_ratio(left_count: int, left_span: int,
                  right_count: int, right_span: int) -> int:
    left = left_count * right_span
    right = right_count * left_span
    if left > right:
        return 1
    if left < right:
        return -1
    return 0


def compute_best_window(meta: ExperimentMeta,
                        rows: list[TickRow],
                        group_slug: str) -> tuple[WindowSummary, list[TickRow]]:
    if not rows:
        raise ValueError(f"no rows for experiment {meta.exp_id}")

    intervals = sorted(rows, key=lambda row: (row.start_tick, row.end_tick))
    starts_desc = sorted(rows, key=lambda row: row.start_tick, reverse=True)

    best_start = 0
    best_end = 0
    best_span = 0
    best_count = 0
    best_seen = False

    inserted = 0
    ends: list[tuple[int, int, int, int]] = []
    unique_starts = sorted({row.start_tick for row in rows}, reverse=True)
    for start_tick in unique_starts:
        while inserted < len(starts_desc) and starts_desc[inserted].start_tick >= start_tick:
            row = starts_desc[inserted]
            insort(ends, (row.end_tick, row.start_tick, row.client_id, row.req_index))
            inserted += 1

        for idx, item in enumerate(ends, start=1):
            end_tick = item[0]
            span = end_tick - start_tick
            if span <= 0:
                continue
            if not best_seen:
                best_seen = True
                best_start = start_tick
                best_end = end_tick
                best_span = span
                best_count = idx
                continue

            cmp = compare_ratio(idx, span, best_count, best_span)
            if cmp > 0:
                best_start = start_tick
                best_end = end_tick
                best_span = span
                best_count = idx
                continue
            if cmp < 0:
                continue

            if idx > best_count:
                best_start = start_tick
                best_end = end_tick
                best_span = span
                best_count = idx
                continue
            if idx < best_count:
                continue

            if span < best_span:
                best_start = start_tick
                best_end = end_tick
                best_span = span
                best_count = idx
                continue
            if span > best_span:
                continue

            if start_tick < best_start:
                best_start = start_tick
                best_end = end_tick
                best_span = span
                best_count = idx

    if not best_seen:
        raise ValueError(f"failed to compute window for experiment {meta.exp_id}")

    included = [
        row for row in intervals
        if row.start_tick >= best_start and row.end_tick <= best_end
    ]
    included.sort(key=lambda row: (row.end_tick, row.start_tick, row.client_id, row.req_index))
    if len(included) != best_count:
        raise ValueError(
            f"window count mismatch for {meta.exp_id}: expected {best_count}, got {len(included)}"
        )

    min_start = min(row.start_tick for row in rows)
    earliest_in_window = min(included, key=lambda row: (row.start_tick, row.end_tick, row.client_id, row.req_index))
    latest_in_window = max(included, key=lambda row: (row.end_tick, row.start_tick, row.client_id, row.req_index))

    summary = WindowSummary(
        exp_id=meta.exp_id,
        source=meta.source,
        group_slug=group_slug,
        request_size=meta.request_size,
        response_size=meta.response_size,
        clients=meta.clients,
        requests_per_client=meta.requests_per_client,
        output_dir=meta.output_dir,
        total_requests=len(rows),
        window_request_count=best_count,
        window_start_tick=best_start,
        window_end_tick=best_end,
        window_span_ns=best_span,
        window_start_offset_ns=best_start - min_start,
        window_end_offset_ns=best_end - min_start,
        avg_latency_ns=best_span / best_count,
        max_throughput_req_per_s=best_count * 1_000_000_000.0 / best_span,
        first_client_id=earliest_in_window.client_id,
        first_req_index=earliest_in_window.req_index,
        last_client_id=latest_in_window.client_id,
        last_req_index=latest_in_window.req_index,
    )
    return summary, included


def summary_row(summary: WindowSummary,
                group_slug: str | None = None,
                group_membership: str | None = None) -> dict[str, str]:
    group_slug = summary.group_slug if group_slug is None else group_slug
    group_membership = (
        summary.group_slug if group_membership is None else group_membership
    )
    return {
        "exp_id": summary.exp_id,
        "source": summary.source,
        "group_slug": group_slug,
        "group_membership": group_membership,
        "request_size": str(summary.request_size),
        "response_size": str(summary.response_size),
        "clients": str(summary.clients),
        "requests_per_client": str(summary.requests_per_client),
        "output_dir": summary.output_dir,
        "total_requests": str(summary.total_requests),
        "window_request_count": str(summary.window_request_count),
        "window_start_tick": str(summary.window_start_tick),
        "window_end_tick": str(summary.window_end_tick),
        "window_span_ns": str(summary.window_span_ns),
        "window_start_offset_ns": str(summary.window_start_offset_ns),
        "window_end_offset_ns": str(summary.window_end_offset_ns),
        "avg_latency_ns": f"{summary.avg_latency_ns:.3f}",
        "max_throughput_req_per_s": f"{summary.max_throughput_req_per_s:.6f}",
        "max_throughput_kreq_per_s": f"{summary.max_throughput_req_per_s / 1000.0:.6f}",
        "first_client_id": str(summary.first_client_id),
        "first_req_index": str(summary.first_req_index),
        "last_client_id": str(summary.last_client_id),
        "last_req_index": str(summary.last_req_index),
    }


def member_row(summary: WindowSummary, row: TickRow) -> dict[str, str]:
    return {
        "exp_id": summary.exp_id,
        "group_slug": summary.group_slug,
        "request_size": str(summary.request_size),
        "response_size": str(summary.response_size),
        "clients": str(summary.clients),
        "requests_per_client": str(summary.requests_per_client),
        "client_id": str(row.client_id),
        "req_index": str(row.req_index),
        "start_tick": str(row.start_tick),
        "end_tick": str(row.end_tick),
        "delta_tick": str(row.delta_tick),
        "start_offset_in_exp_ns": str(row.start_tick - summary.window_start_tick + summary.window_start_offset_ns),
        "end_offset_in_exp_ns": str(row.end_tick - summary.window_start_tick + summary.window_start_offset_ns),
        "start_offset_in_window_ns": str(row.start_tick - summary.window_start_tick),
        "end_offset_in_window_ns": str(row.end_tick - summary.window_start_tick),
        "output_dir": row.output_dir,
    }


def write_csv(path: Path, fieldnames: list[str], rows: list[dict[str, str]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def draw_axes(parts: list[str], rect: ChartRect, x_count: int,
              y_min: float, y_max: float, y_ticks: list[float],
              x_label: str, y_label: str, x_labels: list[str]) -> None:
    parts.append(
        f"<rect x='{rect.x:.1f}' y='{rect.y:.1f}' width='{rect.w:.1f}' "
        f"height='{rect.h:.1f}' class='panel' />"
    )
    for tick in y_ticks:
        y = rect_map_y(rect, tick, y_min, y_max)
        parts.append(
            f"<line x1='{rect.x:.1f}' y1='{y:.1f}' x2='{rect.x + rect.w:.1f}' "
            f"y2='{y:.1f}' class='grid' />"
        )
        svg_text(parts, rect.x - 10, y + 4, f"{tick:.0f}", anchor="end")

    x_min = 0.0
    x_max = float(max(x_count - 1, 1))
    for idx, label in enumerate(x_labels):
        x = rect_map_x(rect, float(idx), x_min, x_max)
        parts.append(
            f"<line x1='{x:.1f}' y1='{rect.y:.1f}' x2='{x:.1f}' "
            f"y2='{rect.y + rect.h:.1f}' class='grid vgrid' />"
        )
        svg_text(parts, x, rect.y + rect.h + 22, label, anchor="middle")

    parts.append(
        f"<line x1='{rect.x:.1f}' y1='{rect.y + rect.h:.1f}' x2='{rect.x + rect.w:.1f}' "
        f"y2='{rect.y + rect.h:.1f}' class='axis' />"
    )
    parts.append(
        f"<line x1='{rect.x:.1f}' y1='{rect.y:.1f}' x2='{rect.x:.1f}' "
        f"y2='{rect.y + rect.h:.1f}' class='axis' />"
    )
    svg_text(parts, rect.x + rect.w / 2.0, rect.y + rect.h + 54, x_label,
             klass="axis-label", anchor="middle")
    parts.append(
        f"<text x='{rect.x - 60:.1f}' y='{rect.y + rect.h / 2.0:.1f}' "
        f"class='axis-label' text-anchor='middle' "
        f"transform='rotate(-90 {rect.x - 60:.1f} {rect.y + rect.h / 2.0:.1f})'>"
        f"{escape(y_label)}</text>"
    )


def write_group_svg(spec: dict[str, object],
                    summaries: list[WindowSummary],
                    out_path: Path) -> None:
    width = 1600
    height = 980
    plot_rect = ChartRect(100, 130, 1380, 380)
    table_x = 100
    table_y = 590
    table_w = 1380
    row_h = 44
    header_h = 34

    x_field = str(spec["x_field"])
    x_labels = [str(getattr(summary, x_field)) for summary in summaries]
    y_values = [summary.max_throughput_req_per_s / 1000.0 for summary in summaries]
    y_max = max(y_values) if y_values else 1.0
    y_ticks = linear_ticks(0.0, y_max * 1.15, 7)
    y_top = y_ticks[-1] if y_ticks else y_max * 1.15
    x_min = 0.0
    x_max = float(max(len(summaries) - 1, 1))
    best_value = max(y_values) if y_values else 0.0

    parts = [
        f"<svg xmlns='http://www.w3.org/2000/svg' width='{width}' height='{height}' "
        f"viewBox='0 0 {width} {height}'>",
        "<style>",
        "svg { background: #fffdf8; }",
        ".title { font: 700 26px sans-serif; fill: #1d1d1b; }",
        ".subtitle { font: 500 14px sans-serif; fill: #555; }",
        ".section { font: 700 16px sans-serif; fill: #1d1d1b; }",
        ".label { font: 12px sans-serif; fill: #444; }",
        ".small { font: 11px sans-serif; fill: #666; }",
        ".axis-label { font: 13px sans-serif; fill: #222; }",
        ".grid { stroke: #e7dfd1; stroke-width: 1; }",
        ".vgrid { stroke-dasharray: 3 5; }",
        ".axis { stroke: #766b5d; stroke-width: 1.5; }",
        ".panel { fill: #fff; stroke: #d7cfc1; stroke-width: 1.2; }",
        ".table-head { fill: #f4eee2; stroke: #d9cfbf; stroke-width: 1; }",
        ".table-cell { fill: #fff; stroke: #e3dbcf; stroke-width: 1; }",
        ".best { fill: #ffe8a3; stroke: #d17b0f; stroke-width: 1.2; }",
        ".pt { stroke: #fff; stroke-width: 1.2; }",
        ".line { fill: none; stroke: #0f4c81; stroke-width: 3.0; }",
        "</style>",
    ]

    svg_text(parts, 100, 52, str(spec["title"]), klass="title")
    svg_text(parts, 100, 78, str(spec["subtitle"]), klass="subtitle")
    svg_text(
        parts,
        100,
        100,
        "Throughput uses pooled requests from all clients. Window throughput = requests in window / window span.",
        klass="subtitle",
    )

    draw_axes(
        parts,
        plot_rect,
        len(summaries),
        0.0,
        y_top,
        y_ticks,
        str(spec["x_label"]),
        "Peak Throughput (Kreq/s)",
        x_labels,
    )

    points = []
    for idx, summary in enumerate(summaries):
        x = rect_map_x(plot_rect, float(idx), x_min, x_max)
        y = rect_map_y(plot_rect, summary.max_throughput_req_per_s / 1000.0, 0.0, y_top)
        points.append((x, y))
    parts.append(f"<path d='{line_path(points)}' class='line' />")

    for idx, summary in enumerate(summaries):
        x, y = points[idx]
        is_best = math.isclose(summary.max_throughput_req_per_s / 1000.0, best_value)
        color = PALETTE[idx % len(PALETTE)]
        radius = 7.5 if is_best else 6.0
        parts.append(
            f"<circle cx='{x:.2f}' cy='{y:.2f}' r='{radius:.2f}' "
            f"class='pt' fill='{color}'>"
            f"<title>{escape(summary.exp_id)} peak={format_throughput_kreq(summary.max_throughput_req_per_s)} Kreq/s"
            f" window={summary.window_request_count} req in {format_duration_ns(summary.window_span_ns)}</title>"
            f"</circle>"
        )
        if is_best:
            parts.append(
                f"<circle cx='{x:.2f}' cy='{y:.2f}' r='{radius + 4:.2f}' class='best' fill='none' />"
            )
        svg_text(
            parts,
            x,
            y - 14,
            f"{summary.max_throughput_req_per_s / 1000.0:.1f}",
            anchor="middle",
        )
        svg_text(
            parts,
            x,
            y - 30,
            f"{summary.window_request_count} req",
            klass="small",
            anchor="middle",
        )

    svg_text(parts, table_x, table_y - 18, "Window Details", klass="section")
    columns = [
        ("Var", 80),
        ("Exp", 310),
        ("Peak Kreq/s", 150),
        ("Window", 210),
        ("Avg Lat", 150),
        ("Offsets", 340),
        ("Boundary", 140),
    ]
    cur_x = table_x
    for label, col_w in columns:
        parts.append(
            f"<rect x='{cur_x:.1f}' y='{table_y:.1f}' width='{col_w:.1f}' height='{header_h:.1f}' "
            f"class='table-head' />"
        )
        svg_text(parts, cur_x + 8, table_y + 22, label, klass="label")
        cur_x += col_w

    for row_idx, summary in enumerate(summaries):
        y = table_y + header_h + row_idx * row_h
        cur_x = table_x
        values = [
            str(getattr(summary, x_field)),
            summary.exp_id,
            f"{summary.max_throughput_req_per_s / 1000.0:.3f}",
            f"{summary.window_request_count} req / {format_duration_ns(summary.window_span_ns)}",
            format_duration_ns(summary.avg_latency_ns),
            f"{format_duration_ns(summary.window_start_offset_ns)} -> {format_duration_ns(summary.window_end_offset_ns)}",
            f"c{summary.first_client_id}/r{summary.first_req_index} ... c{summary.last_client_id}/r{summary.last_req_index}",
        ]
        for (label, col_w), value in zip(columns, values):
            parts.append(
                f"<rect x='{cur_x:.1f}' y='{y:.1f}' width='{col_w:.1f}' height='{row_h:.1f}' "
                f"class='table-cell' />"
            )
            klass = "small" if label in {"Exp", "Window", "Offsets", "Boundary"} else "label"
            svg_text(parts, cur_x + 8, y + 26, value, klass=klass)
            cur_x += col_w

    parts.append("</svg>")
    out_path.write_text("\n".join(parts), encoding="utf-8")


def write_index(out_dir: Path,
                summary_rows: list[dict[str, str]],
                group_files: list[tuple[str, str]],
                batch_dir: Path) -> None:
    rows = [
        "<!doctype html>",
        "<html lang='en'><head><meta charset='utf-8'>",
        "<title>RPC Max Throughput Windows</title>",
        "<style>",
        "body { font-family: sans-serif; margin: 24px; background: #fffdf8; color: #222; }",
        "table { border-collapse: collapse; width: 100%; margin-bottom: 28px; }",
        "th, td { border: 1px solid #d9cfbf; padding: 8px 10px; text-align: left; }",
        "th { background: #f4eee2; }",
        "section { margin-top: 28px; }",
        "img { width: 100%; max-width: 1600px; border: 1px solid #d9cfbf; background: #fff; }",
        "code { background: #f3efe5; padding: 1px 4px; }",
        "ul { line-height: 1.7; }",
        "</style></head><body>",
        "<h1>RPC Max Throughput Window Analysis</h1>",
        f"<p>Batch directory: <code>{escape(str(batch_dir))}</code></p>",
        "<p>All requests are pooled across clients. For each experiment, the selected window maximizes requests/window_span.</p>",
        "<h2>Generated Files</h2><ul>",
    ]
    rows.append("<li><a href='throughput_window_summary.csv'>throughput_window_summary.csv</a></li>")
    rows.append("<li><a href='throughput_window_members.csv'>throughput_window_members.csv</a></li>")
    for title, filename in group_files:
        rows.append(f"<li><a href='{escape(filename)}'>{escape(filename)}</a> - {escape(title)}</li>")
    rows.append("</ul>")

    rows.append("<h2>Experiment Summary</h2>")
    rows.append("<table><thead><tr>")
    heads = [
        "Experiment",
        "Group",
        "Req B",
        "Resp B",
        "Clients",
        "Peak Kreq/s",
        "Window Req",
        "Window Span",
        "Avg Lat",
        "Offsets",
    ]
    for head in heads:
        rows.append(f"<th>{escape(head)}</th>")
    rows.append("</tr></thead><tbody>")
    for item in summary_rows:
        rows.append("<tr>")
        rows.append(f"<td>{escape(item['exp_id'])}</td>")
        rows.append(f"<td>{escape(item['group_slug'])}</td>")
        rows.append(f"<td>{escape(item['request_size'])}</td>")
        rows.append(f"<td>{escape(item['response_size'])}</td>")
        rows.append(f"<td>{escape(item['clients'])}</td>")
        rows.append(f"<td>{escape(item['max_throughput_kreq_per_s'])}</td>")
        rows.append(f"<td>{escape(item['window_request_count'])}</td>")
        rows.append(f"<td>{escape(format_duration_ns(float(item['window_span_ns'])))}</td>")
        rows.append(f"<td>{escape(format_duration_ns(float(item['avg_latency_ns'])))}</td>")
        rows.append(
            f"<td>{escape(format_duration_ns(float(item['window_start_offset_ns'])))} -> "
            f"{escape(format_duration_ns(float(item['window_end_offset_ns'])))}</td>"
        )
        rows.append("</tr>")
    rows.append("</tbody></table>")

    for title, filename in group_files:
        if not filename.endswith(".svg"):
            continue
        rows.append(f"<section><h2>{escape(title)}</h2>")
        rows.append(f"<img src='{escape(filename)}' alt='{escape(title)}' /></section>")

    rows.append("</body></html>")
    (out_dir / "index.html").write_text("\n".join(rows), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compute and plot max throughput windows for RPC matrix runs."
    )
    parser.add_argument("batch_dir", type=Path, help="Matrix output directory")
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=None,
        help="Output directory (default: <batch>/throughput_window_analysis)",
    )
    args = parser.parse_args()

    batch_dir = args.batch_dir.resolve()
    experiments_csv = batch_dir / "experiments.csv"
    ticks_csv = batch_dir / "results_ticks.csv"
    if not experiments_csv.exists():
        raise SystemExit(f"missing file: {experiments_csv}")
    if not ticks_csv.exists():
        raise SystemExit(f"missing file: {ticks_csv}")

    out_dir = (args.out_dir.resolve() if args.out_dir else batch_dir / "throughput_window_analysis")
    out_dir.mkdir(parents=True, exist_ok=True)

    metas = read_experiments(experiments_csv)
    rows_by_exp = read_ticks(ticks_csv)

    summary_objects: list[WindowSummary] = []
    member_rows: list[dict[str, str]] = []

    skipped: list[str] = []
    for spec in GROUP_SPECS:
        group_slug = str(spec["slug"])
        group_metas = select_group_metas(spec, metas)
        if not group_metas:
            skipped.append(f"{group_slug}: no experiments matched this group")
            continue

        for meta in group_metas:
            exp_id = meta.exp_id
            if meta.status != "ok":
                skipped.append(f"{exp_id}: status={meta.status}")
                continue
            rows = rows_by_exp.get(exp_id)
            if not rows:
                skipped.append(f"{exp_id}: missing tick rows")
                continue

            summary, included = compute_best_window(meta, rows, group_slug)
            summary_objects.append(summary)
            for row in included:
                member_rows.append(member_row(summary, row))

    unique_summary_by_exp: dict[str, WindowSummary] = {}
    group_memberships: dict[str, list[str]] = {}
    for item in summary_objects:
        unique_summary_by_exp.setdefault(item.exp_id, item)
        group_memberships.setdefault(item.exp_id, [])
        if item.group_slug not in group_memberships[item.exp_id]:
            group_memberships[item.exp_id].append(item.group_slug)

    summary_rows = [
        summary_row(
            unique_summary_by_exp[exp_id],
            group_slug="all",
            group_membership=",".join(group_memberships[exp_id]),
        )
        for exp_id in sorted(unique_summary_by_exp)
    ]

    unique_member_rows: list[dict[str, str]] = []
    seen_member_keys: set[tuple[str, ...]] = set()
    for row in member_rows:
        key = (
            row["exp_id"],
            row["client_id"],
            row["req_index"],
            row["start_tick"],
            row["end_tick"],
        )
        if key in seen_member_keys:
            continue
        seen_member_keys.add(key)
        unique_member_rows.append(row)

    summary_fields = [
        "exp_id",
        "source",
        "group_slug",
        "group_membership",
        "request_size",
        "response_size",
        "clients",
        "requests_per_client",
        "output_dir",
        "total_requests",
        "window_request_count",
        "window_start_tick",
        "window_end_tick",
        "window_span_ns",
        "window_start_offset_ns",
        "window_end_offset_ns",
        "avg_latency_ns",
        "max_throughput_req_per_s",
        "max_throughput_kreq_per_s",
        "first_client_id",
        "first_req_index",
        "last_client_id",
        "last_req_index",
    ]
    write_csv(out_dir / "throughput_window_summary.csv", summary_fields, summary_rows)

    member_fields = [
        "exp_id",
        "group_slug",
        "request_size",
        "response_size",
        "clients",
        "requests_per_client",
        "client_id",
        "req_index",
        "start_tick",
        "end_tick",
        "delta_tick",
        "start_offset_in_exp_ns",
        "end_offset_in_exp_ns",
        "start_offset_in_window_ns",
        "end_offset_in_window_ns",
        "output_dir",
    ]
    write_csv(out_dir / "throughput_window_members.csv", member_fields, unique_member_rows)

    group_files: list[tuple[str, str]] = []
    for spec in GROUP_SPECS:
        group_slug = str(spec["slug"])
        group_items = [item for item in summary_objects if item.group_slug == group_slug]
        if not group_items:
            continue
        group_items.sort(
            key=lambda item: (
                getattr(item, str(spec["x_field"])),
                item.clients,
                item.request_size,
                item.response_size,
                item.requests_per_client,
                item.exp_id,
            )
        )
        group_csv_rows = [
            summary_row(
                item,
                group_slug=item.group_slug,
                group_membership=",".join(group_memberships[item.exp_id]),
            )
            for item in group_items
        ]
        group_csv_name = f"{group_slug}.csv"
        write_csv(out_dir / group_csv_name, summary_fields, group_csv_rows)
        group_files.append((str(spec["title"]) + " CSV", group_csv_name))

        svg_name = f"{group_slug}.svg"
        write_group_svg(spec, group_items, out_dir / svg_name)
        group_files.append((str(spec["title"]) + " Plot", svg_name))

    write_index(out_dir, summary_rows, group_files, batch_dir)

    print(f"analysis written to: {out_dir}")
    print(f"summary csv: {out_dir / 'throughput_window_summary.csv'}")
    print(f"members csv: {out_dir / 'throughput_window_members.csv'}")
    print(f"index: {out_dir / 'index.html'}")
    if skipped:
        print("skipped experiments:")
        for item in skipped:
            print(f"  - {item}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
