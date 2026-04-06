#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import math
import statistics
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

STEADY_DROP_VALUES = (1,)


@dataclass(frozen=True)
class ExperimentMeta:
    exp_id: str
    source: str
    request_size: int
    response_size: int
    clients: int
    requests_per_client: int
    mq_entries: int
    head_sync_threshold: int
    slow_client_count: int
    slow_count_per_client: int
    slow_client_send_pause_iters: int
    cxl_extra_latency_ns: int
    response_lane_count: int
    clients_per_dma_lane: int
    response_dma_threshold: int
    prefetch_mode: str
    message_profile: str
    output_dir: str
    status: str


@dataclass(frozen=True)
class TickRow:
    node_id: int
    req_index: int
    start_tick: int
    end_tick: int


@dataclass(frozen=True)
class ThroughputSummary:
    total_requests: int
    first_start_tick: int
    last_end_tick: int
    total_span_ns: int
    overall_throughput_req_per_s: float
    overall_throughput_kops: float


def percentile(values: Iterable[float], p: float) -> float:
    vals = sorted(values)
    if not vals:
        return 0.0
    rank = max(1, math.ceil((p / 100.0) * len(vals)))
    return float(vals[min(len(vals) - 1, rank - 1)])


def format_metric(value: float) -> str:
    return f"{value:.3f}"


def metric_fields(prefix: str) -> list[str]:
    return [
        f"{prefix}_avg",
        f"{prefix}_median",
        f"{prefix}_p50",
        f"{prefix}_p90",
        f"{prefix}_p99",
    ]


def compute_metrics(values: list[int], prefix: str) -> dict[str, str]:
    metric_values = [float(item) for item in values]
    return {
        f"{prefix}_avg": format_metric(sum(metric_values) / len(metric_values)),
        f"{prefix}_median": format_metric(statistics.median(metric_values)),
        f"{prefix}_p50": format_metric(percentile(metric_values, 50.0)),
        f"{prefix}_p90": format_metric(percentile(metric_values, 90.0)),
        f"{prefix}_p99": format_metric(percentile(metric_values, 99.0)),
    }


def read_latest_ok_experiments(experiments_csv: Path) -> list[ExperimentMeta]:
    latest_ok: dict[str, ExperimentMeta] = {}

    with experiments_csv.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            meta = ExperimentMeta(
                exp_id=row["exp_id"],
                source=row["source"],
                request_size=int(row["request_size"]),
                response_size=int(row["response_size"]),
                clients=int(row["clients"]),
                requests_per_client=int(row["requests_per_client"]),
                mq_entries=int(row["mq_entries"]),
                head_sync_threshold=int(row["head_sync_threshold"]),
                slow_client_count=int(row.get("slow_client_count", "0")),
                slow_count_per_client=int(row.get("slow_count_per_client", "0")),
                slow_client_send_pause_iters=int(
                    row.get("slow_client_send_pause_iters", "0")
                ),
                cxl_extra_latency_ns=int(row["cxl_extra_latency_ns"]),
                response_lane_count=int(row["response_lane_count"]),
                clients_per_dma_lane=int(row["clients_per_dma_lane"]),
                response_dma_threshold=int(row["response_dma_threshold"]),
                prefetch_mode=row.get("prefetch_mode", "full"),
                message_profile=row.get("message_profile", "fixed"),
                output_dir=row.get("output_dir", ""),
                status=row["status"],
            )
            if meta.status == "ok":
                latest_ok[meta.exp_id] = meta

    return list(latest_ok.values())


def base_row(meta: ExperimentMeta) -> dict[str, str]:
    return {
        "exp_id": meta.exp_id,
        "source": meta.source,
        "request_size": str(meta.request_size),
        "response_size": str(meta.response_size),
        "clients": str(meta.clients),
        "requests_per_client": str(meta.requests_per_client),
        "mq_entries": str(meta.mq_entries),
        "head_sync_threshold": str(meta.head_sync_threshold),
        "slow_client_count": str(meta.slow_client_count),
        "slow_count_per_client": str(meta.slow_count_per_client),
        "slow_client_send_pause_iters": str(meta.slow_client_send_pause_iters),
        "cxl_extra_latency_ns": str(meta.cxl_extra_latency_ns),
        "response_lane_count": str(meta.response_lane_count),
        "clients_per_dma_lane": str(meta.clients_per_dma_lane),
        "response_dma_threshold": str(meta.response_dma_threshold),
        "prefetch_mode": meta.prefetch_mode,
        "message_profile": meta.message_profile,
        "output_dir": meta.output_dir,
    }


def read_tick_rows(ticks_csv: Path) -> dict[str, list[TickRow]]:
    by_output_dir: dict[str, list[TickRow]] = {}
    if not ticks_csv.exists():
        return by_output_dir

    with ticks_csv.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            output_dir = row.get("output_dir", "")
            if not output_dir:
                continue
            by_output_dir.setdefault(output_dir, []).append(
                TickRow(
                    node_id=int(row["node_id"]),
                    req_index=int(row["req_index"]),
                    start_tick=int(row["start_tick"]),
                    end_tick=int(row["end_tick"]),
                )
            )

    return by_output_dir


def read_client_latencies(ticks_csv: Path) -> dict[str, dict[tuple[int, int], int]]:
    by_output_dir: dict[str, dict[tuple[int, int], int]] = {}
    for output_dir, rows in read_tick_rows(ticks_csv).items():
        latencies: dict[tuple[int, int], int] = {}
        for row in rows:
            latencies[(row.node_id, row.req_index)] = max(
                0, row.end_tick - row.start_tick
            )
        by_output_dir[output_dir] = latencies

    return by_output_dir


def select_steady_rows(
    rows: list[TickRow],
    drop_first_per_client: int,
) -> tuple[list[TickRow], int]:
    by_node: dict[int, list[TickRow]] = {}
    kept_rows: list[TickRow] = []
    dropped_requests_total = 0

    for row in rows:
        by_node.setdefault(row.node_id, []).append(row)

    for node_rows in by_node.values():
        ordered = sorted(
            node_rows,
            key=lambda row: (row.req_index, row.start_tick, row.end_tick),
        )
        dropped_requests_total += min(drop_first_per_client, len(ordered))
        kept_rows.extend(ordered[drop_first_per_client:])

    kept_rows.sort(key=lambda row: (row.end_tick, row.node_id, row.req_index))
    return kept_rows, dropped_requests_total


def read_server_breakdowns(
    server_ticks_csv: Path,
) -> dict[str, dict[int, tuple[int, int, int]]]:
    by_output_dir: dict[str, dict[int, tuple[int, int, int]]] = {}
    if not server_ticks_csv.exists():
        return by_output_dir

    with server_ticks_csv.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            output_dir = row.get("output_dir", "")
            if not output_dir:
                continue

            key = int(row["server_req_index"])
            values = (
                int(row["poll_tick"]),
                int(row["execute_tick"]),
                int(row["response_tick"]),
            )
            by_output_dir.setdefault(output_dir, {})[key] = values

    return by_output_dir


def select_steady_server_values(
    server_map: dict[int, tuple[int, int, int]],
    dropped_requests_total: int,
) -> list[tuple[int, int, int]]:
    ordered_values = [server_map[idx] for idx in sorted(server_map)]
    return ordered_values[dropped_requests_total:]


def write_csv(csv_path: Path, fieldnames: list[str], rows: list[dict[str, str]]) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def compute_throughput_summary(rows: list[TickRow]) -> ThroughputSummary:
    if not rows:
        return ThroughputSummary(
            total_requests=0,
            first_start_tick=0,
            last_end_tick=0,
            total_span_ns=0,
            overall_throughput_req_per_s=0.0,
            overall_throughput_kops=0.0,
        )

    first_start = min(row.start_tick for row in rows)
    last_end = max(row.end_tick for row in rows)
    total_span = max(1, last_end - first_start)

    return ThroughputSummary(
        total_requests=len(rows),
        first_start_tick=first_start,
        last_end_tick=last_end,
        total_span_ns=total_span,
        overall_throughput_req_per_s=len(rows) * 1_000_000_000.0 / total_span,
        overall_throughput_kops=len(rows) * 1_000_000.0 / total_span,
    )


def sort_key(
    meta: ExperimentMeta,
) -> tuple[int, int, int, int, int, int, int, int, int, int, int, str, str]:
    return (
        meta.clients,
        meta.request_size,
        meta.response_size,
        meta.mq_entries,
        meta.head_sync_threshold,
        meta.slow_client_count,
        meta.slow_count_per_client,
        meta.slow_client_send_pause_iters,
        meta.cxl_extra_latency_ns,
        meta.response_lane_count,
        meta.clients_per_dma_lane,
        meta.response_dma_threshold,
        meta.prefetch_mode,
        meta.message_profile,
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Summarize CXL RPC raw tick CSVs into exp.txt metrics."
    )
    parser.add_argument(
        "--batch-dir",
        type=str,
        required=True,
        help="Batch directory containing experiments.csv and raw result CSVs.",
    )
    args = parser.parse_args()

    batch_dir = Path(args.batch_dir).resolve()
    experiments_csv = batch_dir / "experiments.csv"
    ticks_csv = batch_dir / "results_ticks.csv"
    server_ticks_csv = batch_dir / "results_server_ticks.csv"
    client_summary_csv = batch_dir / "summary_client_latency.csv"
    server_summary_csv = batch_dir / "summary_server_breakdown.csv"
    throughput_summary_csv = batch_dir / "summary_throughput.csv"
    steady_client_summary_csv = batch_dir / "summary_client_latency_steady.csv"
    steady_server_summary_csv = batch_dir / "summary_server_breakdown_steady.csv"
    steady_throughput_summary_csv = batch_dir / "summary_throughput_steady.csv"

    if not experiments_csv.exists():
        raise FileNotFoundError(f"missing experiments.csv: {experiments_csv}")

    experiments = sorted(read_latest_ok_experiments(experiments_csv), key=sort_key)
    server_breakdowns = read_server_breakdowns(server_ticks_csv)
    tick_rows = read_tick_rows(ticks_csv)

    client_rows: list[dict[str, str]] = []
    server_rows: list[dict[str, str]] = []
    throughput_rows: list[dict[str, str]] = []
    steady_client_rows: list[dict[str, str]] = []
    steady_server_rows: list[dict[str, str]] = []
    steady_throughput_rows: list[dict[str, str]] = []

    for meta in experiments:
        rows = tick_rows.get(meta.output_dir, [])
        if rows:
            latency_values = [max(0, row.end_tick - row.start_tick) for row in rows]
            client_rows.append(
                {
                    **base_row(meta),
                    **compute_metrics(latency_values, "latency"),
                }
            )

        server_map = server_breakdowns.get(meta.output_dir, {})
        if server_map:
            values = list(server_map.values())
            poll_values = [item[0] for item in values]
            execute_values = [item[1] for item in values]
            response_values = [item[2] for item in values]
            server_rows.append(
                {
                    **base_row(meta),
                    **compute_metrics(poll_values, "poll"),
                    **compute_metrics(execute_values, "execute"),
                    **compute_metrics(response_values, "response"),
                }
            )

        if rows:
            throughput = compute_throughput_summary(rows)
            throughput_rows.append(
                {
                    **base_row(meta),
                    "total_requests": str(throughput.total_requests),
                    "first_start_tick": str(throughput.first_start_tick),
                    "last_end_tick": str(throughput.last_end_tick),
                    "total_span_ns": str(throughput.total_span_ns),
                    "overall_throughput_req_per_s": (
                        f"{throughput.overall_throughput_req_per_s:.6f}"
                    ),
                    "overall_throughput_kops": (
                        f"{throughput.overall_throughput_kops:.3f}"
                    ),
                }
            )

        for drop_first_per_client in STEADY_DROP_VALUES:
            steady_rows, dropped_requests_total = select_steady_rows(
                rows, drop_first_per_client
            )
            if not steady_rows:
                continue

            steady_base = {
                **base_row(meta),
                "drop_first_requests_per_client": str(drop_first_per_client),
                "dropped_requests_total": str(dropped_requests_total),
                "steady_requests": str(len(steady_rows)),
            }
            steady_latency_values = [
                max(0, row.end_tick - row.start_tick) for row in steady_rows
            ]
            steady_client_rows.append(
                {
                    **steady_base,
                    **compute_metrics(steady_latency_values, "latency"),
                }
            )

            steady_throughput = compute_throughput_summary(steady_rows)
            steady_throughput_rows.append(
                {
                    **steady_base,
                    "total_requests": str(steady_throughput.total_requests),
                    "first_start_tick": str(steady_throughput.first_start_tick),
                    "last_end_tick": str(steady_throughput.last_end_tick),
                    "total_span_ns": str(steady_throughput.total_span_ns),
                    "overall_throughput_req_per_s": (
                        f"{steady_throughput.overall_throughput_req_per_s:.6f}"
                    ),
                    "overall_throughput_kops": (
                        f"{steady_throughput.overall_throughput_kops:.3f}"
                    ),
                }
            )

            if server_map:
                steady_server_values = select_steady_server_values(
                    server_map, dropped_requests_total
                )
                if steady_server_values:
                    poll_values = [item[0] for item in steady_server_values]
                    execute_values = [item[1] for item in steady_server_values]
                    response_values = [item[2] for item in steady_server_values]
                    steady_server_rows.append(
                        {
                            **steady_base,
                            **compute_metrics(poll_values, "poll"),
                            **compute_metrics(execute_values, "execute"),
                            **compute_metrics(response_values, "response"),
                        }
                    )

    common_fields = [
        "exp_id",
        "source",
        "request_size",
        "response_size",
        "clients",
        "requests_per_client",
        "mq_entries",
        "head_sync_threshold",
        "slow_client_count",
        "slow_count_per_client",
        "slow_client_send_pause_iters",
        "cxl_extra_latency_ns",
        "response_lane_count",
        "clients_per_dma_lane",
        "response_dma_threshold",
        "prefetch_mode",
        "message_profile",
        "output_dir",
    ]
    client_fields = common_fields + metric_fields("latency")
    server_fields = (
        common_fields +
        metric_fields("poll") +
        metric_fields("execute") +
        metric_fields("response")
    )
    throughput_fields = common_fields + [
        "total_requests",
        "first_start_tick",
        "last_end_tick",
        "total_span_ns",
        "overall_throughput_req_per_s",
        "overall_throughput_kops",
    ]
    steady_common_fields = common_fields + [
        "drop_first_requests_per_client",
        "dropped_requests_total",
        "steady_requests",
    ]
    steady_client_fields = steady_common_fields + metric_fields("latency")
    steady_server_fields = (
        steady_common_fields +
        metric_fields("poll") +
        metric_fields("execute") +
        metric_fields("response")
    )
    steady_throughput_fields = steady_common_fields + [
        "total_requests",
        "first_start_tick",
        "last_end_tick",
        "total_span_ns",
        "overall_throughput_req_per_s",
        "overall_throughput_kops",
    ]

    write_csv(client_summary_csv, client_fields, client_rows)
    write_csv(server_summary_csv, server_fields, server_rows)
    write_csv(throughput_summary_csv, throughput_fields, throughput_rows)
    write_csv(steady_client_summary_csv, steady_client_fields, steady_client_rows)
    write_csv(steady_server_summary_csv, steady_server_fields, steady_server_rows)
    write_csv(
        steady_throughput_summary_csv,
        steady_throughput_fields,
        steady_throughput_rows,
    )

    print(f"client summary: {client_summary_csv}")
    print(f"server summary: {server_summary_csv}")
    print(f"throughput summary: {throughput_summary_csv}")
    print(f"steady client summary: {steady_client_summary_csv}")
    print(f"steady server summary: {steady_server_summary_csv}")
    print(f"steady throughput summary: {steady_throughput_summary_csv}")
    print(f"client rows: {len(client_rows)}")
    print(f"server rows: {len(server_rows)}")
    print(f"throughput rows: {len(throughput_rows)}")
    print(f"steady client rows: {len(steady_client_rows)}")
    print(f"steady server rows: {len(steady_server_rows)}")
    print(f"steady throughput rows: {len(steady_throughput_rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
