#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import html
import math
import statistics
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


PALETTE = [
    "#0f4c81",
    "#d1495b",
    "#2f7d32",
    "#7c4dff",
    "#d17b0f",
    "#008b8b",
    "#8b1e3f",
    "#4f6d7a",
    "#6d597a",
    "#1f7a8c",
    "#c1121f",
    "#3a5a40",
    "#bc6c25",
    "#6a4c93",
    "#33658a",
    "#7f5539",
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


def percentile(values: Iterable[float], p: float) -> float:
    vals = sorted(values)
    if not vals:
        return 0.0
    if len(vals) == 1:
        return float(vals[0])
    k = (len(vals) - 1) * (p / 100.0)
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return float(vals[f])
    return float(vals[f] * (c - k) + vals[c] * (k - f))


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


def request_ticks(max_req_index: int) -> list[int]:
    if max_req_index <= 10:
        return list(range(max_req_index + 1))
    step = int(nice_step(max_req_index / 6.0))
    ticks = list(range(0, max_req_index + 1, max(step, 1)))
    if ticks[-1] != max_req_index:
        ticks.append(max_req_index)
    return ticks


def format_duration_ns(ns: float) -> str:
    abs_ns = abs(ns)
    if abs_ns >= 1_000_000:
        return f"{ns / 1_000_000.0:.3f} ms"
    if abs_ns >= 1_000:
        return f"{ns / 1_000.0:.1f} us"
    return f"{ns:.0f} ns"


def format_ms(ns: float) -> str:
    return f"{ns / 1_000_000.0:.3f}"


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


def rect_map_x(rect: ChartRect, value: float, min_v: float, max_v: float) -> float:
    if math.isclose(min_v, max_v):
        return rect.x + rect.w / 2.0
    return rect.x + (value - min_v) / (max_v - min_v) * rect.w


def rect_map_y(rect: ChartRect, value: float, min_v: float, max_v: float) -> float:
    if math.isclose(min_v, max_v):
        return rect.y + rect.h / 2.0
    return rect.y + rect.h - (value - min_v) / (max_v - min_v) * rect.h


def rect_map_y_log(rect: ChartRect, value: float, min_v: float, max_v: float) -> float:
    low = max(min_v, 1e-6)
    high = max(max_v, low * 10.0)
    log_low = math.log10(low)
    log_high = math.log10(high)
    log_val = math.log10(max(value, low))
    if math.isclose(log_low, log_high):
        return rect.y + rect.h / 2.0
    return rect.y + rect.h - (log_val - log_low) / (log_high - log_low) * rect.h


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


def experiment_stats(rows: list[TickRow]) -> dict[str, float]:
    ordered = sorted(rows, key=lambda row: row.start_ns)
    start_min = ordered[0].start_ns
    start_max = ordered[-1].start_ns

    by_idx: dict[int, list[int]] = defaultdict(list)
    by_client: dict[int, list[TickRow]] = defaultdict(list)
    for row in rows:
        by_idx[row.req_index].append(row.start_ns)
        by_client[row.client_id].append(row)

    gap01_ns: list[int] = []
    steady_gap_ns: list[int] = []
    for client_rows in by_client.values():
        client_rows.sort(key=lambda row: row.req_index)
        if len(client_rows) >= 2:
            gap01_ns.append(client_rows[1].start_ns - client_rows[0].start_ns)
        for left, right in zip(client_rows[1:], client_rows[2:]):
            steady_gap_ns.append(right.start_ns - left.start_ns)

    req0_spread = max(by_idx[0]) - min(by_idx[0]) if 0 in by_idx else 0
    req1_spread = max(by_idx[1]) - min(by_idx[1]) if 1 in by_idx else 0
    req_last = max(by_idx)
    req_last_spread = max(by_idx[req_last]) - min(by_idx[req_last])

    return {
        "start_min_ns": start_min,
        "start_max_ns": start_max,
        "total_send_span_ns": start_max - start_min,
        "req0_spread_ns": req0_spread,
        "req1_spread_ns": req1_spread,
        "req_last_spread_ns": req_last_spread,
        "gap01_p50_ns": percentile(gap01_ns, 50),
        "steady_gap_p50_ns": percentile(steady_gap_ns, 50),
        "steady_gap_p95_ns": percentile(steady_gap_ns, 95),
    }


def draw_axes_linear(parts: list[str], rect: ChartRect, x_ticks: list[float],
                     y_ticks: list[float], x_min: float, x_max: float,
                     y_min: float, y_max: float, x_label: str, y_label: str,
                     x_fmt=None, y_fmt=None) -> None:
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
        label = y_fmt(tick) if y_fmt else str(tick)
        svg_text(parts, rect.x - 10, y + 4, label, anchor="end")

    for tick in x_ticks:
        x = rect_map_x(rect, tick, x_min, x_max)
        parts.append(
            f"<line x1='{x:.1f}' y1='{rect.y:.1f}' x2='{x:.1f}' "
            f"y2='{rect.y + rect.h:.1f}' class='grid' />"
        )
        label = x_fmt(tick) if x_fmt else str(tick)
        svg_text(parts, x, rect.y + rect.h + 22, label, anchor="middle")

    parts.append(
        f"<line x1='{rect.x:.1f}' y1='{rect.y + rect.h:.1f}' x2='{rect.x + rect.w:.1f}' "
        f"y2='{rect.y + rect.h:.1f}' class='axis' />"
    )
    parts.append(
        f"<line x1='{rect.x:.1f}' y1='{rect.y:.1f}' x2='{rect.x:.1f}' "
        f"y2='{rect.y + rect.h:.1f}' class='axis' />"
    )
    svg_text(parts, rect.x + rect.w / 2.0, rect.y + rect.h + 48, x_label,
             klass="axis-label", anchor="middle")
    parts.append(
        f"<text x='{rect.x - 58:.1f}' y='{rect.y + rect.h / 2.0:.1f}' "
        f"class='axis-label' text-anchor='middle' "
        f"transform='rotate(-90 {rect.x - 58:.1f} {rect.y + rect.h / 2.0:.1f})'>"
        f"{escape(y_label)}</text>"
    )


def draw_axes_log_y(parts: list[str], rect: ChartRect, x_ticks: list[float],
                    y_ticks: list[float], x_min: float, x_max: float,
                    y_min: float, y_max: float, x_label: str,
                    y_label: str) -> None:
    parts.append(
        f"<rect x='{rect.x:.1f}' y='{rect.y:.1f}' width='{rect.w:.1f}' "
        f"height='{rect.h:.1f}' class='panel' />"
    )
    for tick in y_ticks:
        y = rect_map_y_log(rect, tick, y_min, y_max)
        parts.append(
            f"<line x1='{rect.x:.1f}' y1='{y:.1f}' x2='{rect.x + rect.w:.1f}' "
            f"y2='{y:.1f}' class='grid' />"
        )
        svg_text(parts, rect.x - 10, y + 4, f"{tick:g}", anchor="end")

    for tick in x_ticks:
        x = rect_map_x(rect, tick, x_min, x_max)
        parts.append(
            f"<line x1='{x:.1f}' y1='{rect.y:.1f}' x2='{x:.1f}' "
            f"y2='{rect.y + rect.h:.1f}' class='grid' />"
        )
        svg_text(parts, x, rect.y + rect.h + 22, str(int(tick)), anchor="middle")

    parts.append(
        f"<line x1='{rect.x:.1f}' y1='{rect.y + rect.h:.1f}' x2='{rect.x + rect.w:.1f}' "
        f"y2='{rect.y + rect.h:.1f}' class='axis' />"
    )
    parts.append(
        f"<line x1='{rect.x:.1f}' y1='{rect.y:.1f}' x2='{rect.x:.1f}' "
        f"y2='{rect.y + rect.h:.1f}' class='axis' />"
    )
    svg_text(parts, rect.x + rect.w / 2.0, rect.y + rect.h + 48, x_label,
             klass="axis-label", anchor="middle")
    parts.append(
        f"<text x='{rect.x - 58:.1f}' y='{rect.y + rect.h / 2.0:.1f}' "
        f"class='axis-label' text-anchor='middle' "
        f"transform='rotate(-90 {rect.x - 58:.1f} {rect.y + rect.h / 2.0:.1f})'>"
        f"{escape(y_label)}</text>"
    )


def write_experiment_svg(meta: ExperimentMeta, rows: list[TickRow], out_path: Path) -> dict[str, str]:
    rows = sorted(rows, key=lambda row: (row.client_id, row.req_index))
    ordered = sorted(rows, key=lambda row: row.start_ns)
    stats = experiment_stats(rows)
    start_min = int(stats["start_min_ns"])
    max_req = max(row.req_index for row in rows)
    max_client = max(row.client_id for row in rows)

    by_client: dict[int, list[TickRow]] = defaultdict(list)
    by_index: dict[int, list[int]] = defaultdict(list)
    for row in rows:
        by_client[row.client_id].append(row)
        by_index[row.req_index].append(row.start_ns - start_min)

    for client_rows in by_client.values():
        client_rows.sort(key=lambda row: row.req_index)

    spread_us = {
        idx: (max(vals) - min(vals)) / 1_000.0
        for idx, vals in by_index.items()
    }

    width = 1680
    height = 1240
    top_rect = ChartRect(90, 120, 1060, 430)
    legend_x = 1190
    legend_y = 120
    order_rect = ChartRect(90, 680, 760, 430)
    spread_rect = ChartRect(930, 680, 600, 430)

    parts = [
        f"<svg xmlns='http://www.w3.org/2000/svg' width='{width}' height='{height}' "
        f"viewBox='0 0 {width} {height}'>",
        "<style>",
        "svg { background: #fffdf8; }",
        ".title { font: 700 24px sans-serif; fill: #1d1d1b; }",
        ".subtitle { font: 500 14px sans-serif; fill: #555; }",
        ".section { font: 700 16px sans-serif; fill: #1d1d1b; }",
        ".label { font: 12px sans-serif; fill: #444; }",
        ".axis-label { font: 13px sans-serif; fill: #222; }",
        ".grid { stroke: #e7dfd1; stroke-width: 1; }",
        ".axis { stroke: #766b5d; stroke-width: 1.5; }",
        ".panel { fill: #fff; stroke: #d7cfc1; stroke-width: 1.2; }",
        ".hint { font: 12px sans-serif; fill: #6a6258; }",
        "</style>",
    ]

    title = (
        f"{meta.exp_id}  |  req={meta.request_size}B resp={meta.response_size}B "
        f"clients={meta.clients} reqs/client={meta.requests_per_client}"
    )
    svg_text(parts, 90, 50, title, klass="title")
    svg_text(
        parts,
        90,
        76,
        "Top: per-client send timeline  |  Bottom-left: global send order  |  "
        "Bottom-right: cross-client start spread",
        klass="subtitle",
    )

    y_top_max_ms = max((row.start_ns - start_min) / 1_000_000.0 for row in rows) * 1.04
    y_top_max_ms = max(y_top_max_ms, 0.05)
    x_ticks = [float(tick) for tick in request_ticks(max_req)]
    y_ticks = linear_ticks(0.0, y_top_max_ms, 7)
    draw_axes_linear(
        parts,
        top_rect,
        x_ticks,
        y_ticks,
        0.0,
        float(max_req),
        0.0,
        y_top_max_ms,
        "Request Index",
        "Relative Send Start (ms)",
        x_fmt=lambda x: str(int(round(x))),
        y_fmt=lambda y: f"{y:.1f}",
    )

    req0_left = rect_map_x(top_rect, -0.5, 0.0, float(max_req))
    req0_right = rect_map_x(top_rect, 0.5, 0.0, float(max_req))
    parts.append(
        f"<rect x='{req0_left:.1f}' y='{top_rect.y:.1f}' width='{req0_right - req0_left:.1f}' "
        f"height='{top_rect.h:.1f}' fill='#f8d7da' fill-opacity='0.25' stroke='none' />"
    )
    svg_text(parts, req0_left + 6, top_rect.y + 18, "req0 first-wave zone", klass="hint")

    for client_id in sorted(by_client):
        color = PALETTE[client_id % len(PALETTE)]
        pts = []
        for row in by_client[client_id]:
            x = rect_map_x(top_rect, float(row.req_index), 0.0, float(max_req))
            y = rect_map_y(
                top_rect,
                (row.start_ns - start_min) / 1_000_000.0,
                0.0,
                y_top_max_ms,
            )
            pts.append((x, y))
        parts.append(
            f"<path d='{line_path(pts)}' fill='none' stroke='{color}' "
            f"stroke-width='1.6' stroke-opacity='0.9' />"
        )
        for row, (x, y) in zip(by_client[client_id], pts):
            parts.append(
                f"<circle cx='{x:.2f}' cy='{y:.2f}' r='3.2' fill='{color}' "
                f"stroke='#fff' stroke-width='0.8'>"
                f"<title>client={row.client_id} req={row.req_index} "
                f"start={format_duration_ns(row.start_ns - start_min)} "
                f"latency={format_duration_ns(row.latency_ns)}</title></circle>"
            )

    svg_text(parts, legend_x, legend_y, "Key Stats", klass="section")
    stat_lines = [
        f"total send span: {format_duration_ns(stats['total_send_span_ns'])}",
        f"req0 spread: {format_duration_ns(stats['req0_spread_ns'])}",
        f"req1 spread: {format_duration_ns(stats['req1_spread_ns'])}",
        f"req{max_req} spread: {format_duration_ns(stats['req_last_spread_ns'])}",
        f"gap req0->1 p50: {format_duration_ns(stats['gap01_p50_ns'])}",
        f"steady send gap p50: {format_duration_ns(stats['steady_gap_p50_ns'])}",
        f"steady send gap p95: {format_duration_ns(stats['steady_gap_p95_ns'])}",
    ]
    for idx, line in enumerate(stat_lines):
        svg_text(parts, legend_x, legend_y + 28 + idx * 20, line)

    svg_text(parts, legend_x, legend_y + 200, "Client Colors", klass="section")
    legend_cols = 2 if meta.clients > 8 else 1
    legend_rows = math.ceil(meta.clients / legend_cols)
    for idx, client_id in enumerate(sorted(by_client)):
        col = idx // legend_rows
        row = idx % legend_rows
        x = legend_x + col * 130
        y = legend_y + 226 + row * 20
        color = PALETTE[client_id % len(PALETTE)]
        parts.append(
            f"<rect x='{x:.1f}' y='{y - 10:.1f}' width='14' height='14' "
            f"fill='{color}' stroke='none' />"
        )
        svg_text(parts, x + 22, y + 1, f"client {client_id}")

    order_x_ticks = [float(tick) for tick in request_ticks(len(ordered) - 1)]
    order_y_ticks = [float(tick) for tick in range(max_client + 1)]
    draw_axes_linear(
        parts,
        order_rect,
        order_x_ticks,
        order_y_ticks,
        0.0,
        float(max(len(ordered) - 1, 1)),
        0.0,
        float(max(max_client, 1)),
        "Global Send Order",
        "Client ID",
        x_fmt=lambda x: str(int(round(x))),
        y_fmt=lambda y: str(int(round(y))),
    )
    svg_text(parts, order_rect.x, order_rect.y - 16,
             "Global send order after time-sort", klass="section")
    if meta.clients > 1:
        divider = float(meta.clients - 0.5)
        x = rect_map_x(order_rect, divider, 0.0, float(max(len(ordered) - 1, 1)))
        parts.append(
            f"<line x1='{x:.1f}' y1='{order_rect.y:.1f}' x2='{x:.1f}' "
            f"y2='{order_rect.y + order_rect.h:.1f}' stroke='#b56576' "
            f"stroke-width='1.5' stroke-dasharray='6 5' />"
        )
        svg_text(parts, x + 6, order_rect.y + 18, "end of req0 wave", klass="hint")

    for order, row in enumerate(ordered):
        color = PALETTE[row.client_id % len(PALETTE)]
        x = rect_map_x(order_rect, float(order), 0.0, float(max(len(ordered) - 1, 1)))
        y = rect_map_y(order_rect, float(row.client_id), 0.0, float(max(max_client, 1)))
        parts.append(
            f"<circle cx='{x:.2f}' cy='{y:.2f}' r='3.0' fill='{color}' "
            f"fill-opacity='0.9' stroke='#fff' stroke-width='0.6'>"
            f"<title>order={order} client={row.client_id} req={row.req_index} "
            f"start={format_duration_ns(row.start_ns - start_min)}</title></circle>"
        )

    svg_text(parts, spread_rect.x, spread_rect.y - 16,
             "Cross-client start spread by request index (log scale, us)",
             klass="section")
    if len(by_client) == 1:
        parts.append(
            f"<rect x='{spread_rect.x:.1f}' y='{spread_rect.y:.1f}' width='{spread_rect.w:.1f}' "
            f"height='{spread_rect.h:.1f}' class='panel' />"
        )
        svg_text(
            parts,
            spread_rect.x + spread_rect.w / 2.0,
            spread_rect.y + spread_rect.h / 2.0,
            "single-client run: cross-client spread is always 0",
            klass="section",
            anchor="middle",
        )
    else:
        positive_spreads = [max(val, 1.0) for val in spread_us.values()]
        y_log_min = min(positive_spreads)
        y_log_max = max(positive_spreads)
        tick_powers = list(
            range(
                int(math.floor(math.log10(y_log_min))),
                int(math.ceil(math.log10(y_log_max))) + 1,
            )
        )
        y_log_ticks = [10.0 ** power for power in tick_powers]
        draw_axes_log_y(
            parts,
            spread_rect,
            x_ticks,
            y_log_ticks,
            0.0,
            float(max_req),
            y_log_min,
            y_log_max,
            "Request Index",
            "Spread (us)",
        )
        spread_points = []
        for idx in sorted(spread_us):
            x = rect_map_x(spread_rect, float(idx), 0.0, float(max_req))
            y = rect_map_y_log(spread_rect, max(spread_us[idx], 1.0), y_log_min, y_log_max)
            spread_points.append((x, y))
        parts.append(
            f"<path d='{line_path(spread_points)}' fill='none' stroke='#0f4c81' "
            f"stroke-width='2.0' />"
        )
        for idx in sorted(spread_us):
            x = rect_map_x(spread_rect, float(idx), 0.0, float(max_req))
            y = rect_map_y_log(spread_rect, max(spread_us[idx], 1.0), y_log_min, y_log_max)
            parts.append(
                f"<circle cx='{x:.2f}' cy='{y:.2f}' r='3.2' fill='#d1495b' stroke='#fff' "
                f"stroke-width='0.7'><title>req={idx} spread={spread_us[idx]:.1f} us</title></circle>"
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
        "svg_name": out_path.name,
        "total_send_span": format_duration_ns(stats["total_send_span_ns"]),
        "req0_spread": format_duration_ns(stats["req0_spread_ns"]),
        "req1_spread": format_duration_ns(stats["req1_spread_ns"]),
        "steady_gap_p50": format_duration_ns(stats["steady_gap_p50_ns"]),
        "steady_gap_p95": format_duration_ns(stats["steady_gap_p95_ns"]),
    }


def write_index(out_dir: Path, summaries: list[dict[str, str]], batch_dir: Path) -> None:
    rows = []
    rows.append("<!doctype html>")
    rows.append("<html lang='en'><head><meta charset='utf-8'>")
    rows.append("<title>RPC Request Timing Plots</title>")
    rows.append("<style>")
    rows.append("body { font-family: sans-serif; margin: 24px; background: #fffdf8; color: #222; }")
    rows.append("table { border-collapse: collapse; width: 100%; margin-bottom: 28px; }")
    rows.append("th, td { border: 1px solid #d9cfbf; padding: 8px 10px; text-align: left; }")
    rows.append("th { background: #f4eee2; }")
    rows.append("section { margin-top: 32px; }")
    rows.append("img { width: 100%; max-width: 1680px; border: 1px solid #d9cfbf; background: #fff; }")
    rows.append("code { background: #f3efe5; padding: 1px 4px; }")
    rows.append("</style></head><body>")
    rows.append(f"<h1>RPC Request Timing Plots</h1>")
    rows.append(f"<p>Batch directory: <code>{escape(str(batch_dir))}</code></p>")
    rows.append(f"<p>Completed experiments plotted: <strong>{len(summaries)}</strong></p>")
    rows.append("<table><thead><tr>")
    for head in [
        "Experiment",
        "Source",
        "Req B",
        "Resp B",
        "Clients",
        "Reqs/Client",
        "Total Send Span",
        "Req0 Spread",
        "Req1 Spread",
        "Steady Gap P50",
        "Steady Gap P95",
        "SVG",
    ]:
        rows.append(f"<th>{escape(head)}</th>")
    rows.append("</tr></thead><tbody>")
    for item in summaries:
        rows.append("<tr>")
        rows.append(f"<td>{escape(item['exp_id'])}</td>")
        rows.append(f"<td>{escape(item['source'])}</td>")
        rows.append(f"<td>{escape(item['request_size'])}</td>")
        rows.append(f"<td>{escape(item['response_size'])}</td>")
        rows.append(f"<td>{escape(item['clients'])}</td>")
        rows.append(f"<td>{escape(item['requests_per_client'])}</td>")
        rows.append(f"<td>{escape(item['total_send_span'])}</td>")
        rows.append(f"<td>{escape(item['req0_spread'])}</td>")
        rows.append(f"<td>{escape(item['req1_spread'])}</td>")
        rows.append(f"<td>{escape(item['steady_gap_p50'])}</td>")
        rows.append(f"<td>{escape(item['steady_gap_p95'])}</td>")
        rows.append(
            f"<td><a href='{escape(item['svg_name'])}'>{escape(item['svg_name'])}</a></td>"
        )
        rows.append("</tr>")
    rows.append("</tbody></table>")

    for item in summaries:
        rows.append(f"<section id='{escape(item['exp_id'])}'>")
        rows.append(
            f"<h2>{escape(item['exp_id'])}</h2>"
            f"<p>req={escape(item['request_size'])}B, resp={escape(item['response_size'])}B, "
            f"clients={escape(item['clients'])}, reqs/client={escape(item['requests_per_client'])}</p>"
        )
        rows.append(
            f"<img src='{escape(item['svg_name'])}' alt='{escape(item['exp_id'])} plot' />"
        )
        rows.append("</section>")
    rows.append("</body></html>")
    (out_dir / "index.html").write_text("\n".join(rows), encoding="utf-8")


def write_summary_csv(out_dir: Path, summaries: list[dict[str, str]]) -> None:
    fields = [
        "exp_id",
        "source",
        "request_size",
        "response_size",
        "clients",
        "requests_per_client",
        "total_send_span",
        "req0_spread",
        "req1_spread",
        "steady_gap_p50",
        "steady_gap_p95",
        "svg_name",
    ]
    with (out_dir / "summary.csv").open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for item in summaries:
            writer.writerow({field: item[field] for field in fields})


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Plot per-experiment RPC request timing as standalone SVG files."
    )
    parser.add_argument("batch_dir", type=Path, help="Matrix output directory")
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=None,
        help="Output directory for generated SVG files (default: <batch>/request_timing_plots)",
    )
    args = parser.parse_args()

    batch_dir = args.batch_dir.resolve()
    experiments_csv = batch_dir / "experiments.csv"
    ticks_csv = batch_dir / "results_ticks.csv"
    if not experiments_csv.exists():
        raise SystemExit(f"missing file: {experiments_csv}")
    if not ticks_csv.exists():
        raise SystemExit(f"missing file: {ticks_csv}")

    out_dir = (args.out_dir.resolve() if args.out_dir else batch_dir / "request_timing_plots")
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

    write_index(out_dir, summaries, batch_dir)
    write_summary_csv(out_dir, summaries)

    print(f"generated {len(summaries)} SVG plots under {out_dir}")
    print(f"index: {out_dir / 'index.html'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
