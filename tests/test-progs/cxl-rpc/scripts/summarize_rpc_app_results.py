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
    read_ratio: float
    update_ratio: float
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


def read_latest_ok_experiments(experiments_csv: Path) -> list[ExperimentMeta]:
    latest_ok: dict[str, ExperimentMeta] = {}

    with experiments_csv.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
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
                read_ratio=float(row["read_ratio"]),
                update_ratio=float(row["update_ratio"]),
                zipf_theta=float(row["zipf_theta"]),
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
        "read_ratio": f"{meta.read_ratio:.6f}",
        "update_ratio": f"{meta.update_ratio:.6f}",
        "zipf_theta": f"{meta.zipf_theta:.6f}",
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


def sort_key(meta: ExperimentMeta) -> tuple[str, int, int, int, int, int, int, str]:
    return (
        meta.profile,
        meta.clients,
        meta.requests_per_client,
        meta.record_count,
        meta.mq_entries,
        meta.head_sync_threshold,
        meta.response_dma_threshold,
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

    if not experiments_csv.exists():
        raise FileNotFoundError(f"missing experiments.csv: {experiments_csv}")

    experiments = sorted(read_latest_ok_experiments(experiments_csv), key=sort_key)
    client_latencies = bare_summary.read_client_latencies(ticks_csv)
    server_breakdowns = bare_summary.read_server_breakdowns(server_ticks_csv)
    tick_rows = bare_summary.read_tick_rows(ticks_csv)

    client_rows: list[dict[str, str]] = []
    server_rows: list[dict[str, str]] = []
    throughput_rows: list[dict[str, str]] = []

    for meta in experiments:
        latency_map = client_latencies.get(meta.output_dir, {})
        if latency_map:
            latency_values = list(latency_map.values())
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

        rows = tick_rows.get(meta.output_dir, [])
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
        "read_ratio",
        "update_ratio",
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
    ]

    bare_summary.write_csv(client_summary_csv, client_fields, client_rows)
    bare_summary.write_csv(server_summary_csv, server_fields, server_rows)
    bare_summary.write_csv(throughput_summary_csv, throughput_fields,
                           throughput_rows)

    print(f"client summary: {client_summary_csv}")
    print(f"server summary: {server_summary_csv}")
    print(f"throughput summary: {throughput_summary_csv}")
    print(f"client rows: {len(client_rows)}")
    print(f"server rows: {len(server_rows)}")
    print(f"throughput rows: {len(throughput_rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
