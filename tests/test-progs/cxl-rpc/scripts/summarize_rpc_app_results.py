#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path

import summarize_rpc_matrix_results as bare_summary


@dataclass(frozen=True)
class ExperimentMeta:
    exp_id: str
    source: str
    profile: str
    clients: int
    requests_per_client: int
    record_count: int
    key_size: int
    value_size: int
    size_mode: str
    key_dist: str
    read_ratio: float
    update_ratio: float
    rmw_ratio: float
    insert_ratio: float
    zipf_theta: float
    dataset_seed: int
    workload_seed: int
    mq_entries: int
    head_sync_threshold: int
    cxl_extra_latency_ns: int
    response_lane_count: int
    clients_per_dma_lane: int
    response_dma_threshold: int
    prefetch_mode: str
    output_dir: str
    status: str


def default_key_dist(profile: str) -> str:
    if profile in {"ycsb_d_1k", "udb_d"}:
        return "latest"
    return "zipf"


def format_zipf_field(key_dist: str, zipf_theta: float) -> str:
    if key_dist != "zipf":
        return ""
    return f"{zipf_theta:.6f}"


def read_latest_ok_experiments(experiments_csv: Path) -> list[ExperimentMeta]:
    latest_ok: dict[str, ExperimentMeta] = {}

    with experiments_csv.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            key_dist = row.get("key_dist", "").strip() or default_key_dist(
                row["profile"]
            )
            zipf_theta = (
                0.0
                if key_dist != "zipf"
                else float(row.get("zipf_theta", "0.99"))
            )
            meta = ExperimentMeta(
                exp_id=row["exp_id"],
                source=row["source"],
                profile=row["profile"],
                clients=int(row["clients"]),
                requests_per_client=int(row["requests_per_client"]),
                record_count=int(row["record_count"]),
                key_size=int(row["key_size"]),
                value_size=int(row["value_size"]),
                size_mode=row.get("size_mode", "fixed"),
                key_dist=key_dist,
                read_ratio=float(row["read_ratio"]),
                update_ratio=float(row["update_ratio"]),
                rmw_ratio=float(row.get("rmw_ratio", "0.0")),
                insert_ratio=float(row.get("insert_ratio", "0.0")),
                zipf_theta=zipf_theta,
                dataset_seed=int(row["dataset_seed"]),
                workload_seed=int(row["workload_seed"]),
                mq_entries=int(row["mq_entries"]),
                head_sync_threshold=int(row["head_sync_threshold"]),
                cxl_extra_latency_ns=int(row["cxl_extra_latency_ns"]),
                response_lane_count=int(row["response_lane_count"]),
                clients_per_dma_lane=int(row["clients_per_dma_lane"]),
                response_dma_threshold=int(row["response_dma_threshold"]),
                prefetch_mode=row["prefetch_mode"],
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
        "profile": meta.profile,
        "clients": str(meta.clients),
        "requests_per_client": str(meta.requests_per_client),
        "record_count": str(meta.record_count),
        "key_size": str(meta.key_size),
        "value_size": str(meta.value_size),
        "size_mode": meta.size_mode,
        "key_dist": meta.key_dist,
        "read_ratio": f"{meta.read_ratio:.6f}",
        "update_ratio": f"{meta.update_ratio:.6f}",
        "rmw_ratio": f"{meta.rmw_ratio:.6f}",
        "insert_ratio": f"{meta.insert_ratio:.6f}",
        "zipf_theta": format_zipf_field(meta.key_dist, meta.zipf_theta),
        "dataset_seed": str(meta.dataset_seed),
        "workload_seed": str(meta.workload_seed),
        "mq_entries": str(meta.mq_entries),
        "head_sync_threshold": str(meta.head_sync_threshold),
        "cxl_extra_latency_ns": str(meta.cxl_extra_latency_ns),
        "response_lane_count": str(meta.response_lane_count),
        "clients_per_dma_lane": str(meta.clients_per_dma_lane),
        "response_dma_threshold": str(meta.response_dma_threshold),
        "prefetch_mode": meta.prefetch_mode,
        "output_dir": meta.output_dir,
    }


def sort_key(meta: ExperimentMeta) -> tuple[str, str, int, int, int, int, int, int, int, int, str]:
    return (
        meta.profile,
        meta.key_dist,
        meta.clients,
        meta.requests_per_client,
        meta.record_count,
        meta.mq_entries,
        meta.head_sync_threshold,
        meta.response_dma_threshold,
        int(meta.rmw_ratio * 1000000.0),
        int(meta.insert_ratio * 1000000.0),
        meta.prefetch_mode,
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Summarize application-path CXL RPC raw tick CSVs."
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
    server_breakdowns = bare_summary.read_server_breakdowns(server_ticks_csv)
    tick_rows = bare_summary.read_tick_rows(ticks_csv)

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
                    **bare_summary.compute_metrics(latency_values, "latency"),
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
                    **bare_summary.compute_metrics(poll_values, "poll"),
                    **bare_summary.compute_metrics(execute_values, "execute"),
                    **bare_summary.compute_metrics(response_values, "response"),
                }
            )

        if rows:
            throughput = bare_summary.compute_throughput_summary(rows)
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

        for drop_first_per_client in bare_summary.STEADY_DROP_VALUES:
            steady_rows, dropped_requests_total = bare_summary.select_steady_rows(
                rows,
                drop_first_per_client,
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
                    **bare_summary.compute_metrics(steady_latency_values, "latency"),
                }
            )

            steady_throughput = bare_summary.compute_throughput_summary(steady_rows)
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
                steady_server_values = bare_summary.select_steady_server_values(
                    server_map,
                    dropped_requests_total,
                )
                if steady_server_values:
                    poll_values = [item[0] for item in steady_server_values]
                    execute_values = [item[1] for item in steady_server_values]
                    response_values = [item[2] for item in steady_server_values]
                    steady_server_rows.append(
                        {
                            **steady_base,
                            **bare_summary.compute_metrics(poll_values, "poll"),
                            **bare_summary.compute_metrics(execute_values, "execute"),
                            **bare_summary.compute_metrics(response_values, "response"),
                        }
                    )

    common_fields = [
        "exp_id",
        "source",
        "profile",
        "clients",
        "requests_per_client",
        "record_count",
        "key_size",
        "value_size",
        "size_mode",
        "key_dist",
        "read_ratio",
        "update_ratio",
        "rmw_ratio",
        "insert_ratio",
        "zipf_theta",
        "dataset_seed",
        "workload_seed",
        "mq_entries",
        "head_sync_threshold",
        "cxl_extra_latency_ns",
        "response_lane_count",
        "clients_per_dma_lane",
        "response_dma_threshold",
        "prefetch_mode",
        "output_dir",
    ]
    client_fields = common_fields + bare_summary.metric_fields("latency")
    server_fields = (
        common_fields +
        bare_summary.metric_fields("poll") +
        bare_summary.metric_fields("execute") +
        bare_summary.metric_fields("response")
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
    steady_client_fields = steady_common_fields + bare_summary.metric_fields("latency")
    steady_server_fields = (
        steady_common_fields +
        bare_summary.metric_fields("poll") +
        bare_summary.metric_fields("execute") +
        bare_summary.metric_fields("response")
    )
    steady_throughput_fields = steady_common_fields + [
        "total_requests",
        "first_start_tick",
        "last_end_tick",
        "total_span_ns",
        "overall_throughput_req_per_s",
        "overall_throughput_kops",
    ]

    bare_summary.write_csv(client_summary_csv, client_fields, client_rows)
    bare_summary.write_csv(server_summary_csv, server_fields, server_rows)
    bare_summary.write_csv(throughput_summary_csv, throughput_fields, throughput_rows)
    bare_summary.write_csv(
        steady_client_summary_csv,
        steady_client_fields,
        steady_client_rows,
    )
    bare_summary.write_csv(
        steady_server_summary_csv,
        steady_server_fields,
        steady_server_rows,
    )
    bare_summary.write_csv(
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
