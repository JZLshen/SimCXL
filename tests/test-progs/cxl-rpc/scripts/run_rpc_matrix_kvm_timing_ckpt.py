#!/usr/bin/env python3
"""
Run CXL RPC matrix experiments with:
  - KVM boot + TIMING CPU test phase
  - inject guest binaries once per batch
  - reuse one checkpoint per hardware topology
  - automatic result extraction from guest writefile artifacts

Default experiment matrix follows the current shared-aligned SimCXL scope:
  - overall:
    client counts 1/2/4/8/16/32 for:
      fixed (64,64)
      uniform-1530-315
      uniform-38-230
  - application:
    shared-aligned 32-client KV/YCSB workloads:
      ycsb_a_1k
      ycsb_b_1k
      ycsb_c_1k
      ycsb_d_1k
      udb_a
      udb_b
      udb_c
      udb_d
  - sensitivity:
    shared-aligned public sweeps:
      request size / response size / ring size / CXL latency use the
      32-client uniform-38-230 baseline;
      request sparsity uses the 16-client uniform-38-230 baseline
  - motivation sparsity:
    shared-aligned 16-client req64/resp64 sparse points;
    `100%` density reuses the c16 req64/resp64 baseline
  - technical analysis:
    SimCXL-specific comparisons and knobs:
      cpu-only + no-prefetch
      cpu-only + current-prefetch
      current dma + no-prefetch
      current dma + current-prefetch
      head sync threshold

DMA-lane sensitivity is intentionally excluded from this current scope.
The matrix is deduplicated, so modes that collapse to the same runtime
configuration are submitted only once.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import fcntl
import os
import re
import shlex
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple


DEFAULT_MQ_ENTRIES = 1024
DEFAULT_RESPONSE_DMA_THRESHOLD = 256
DEFAULT_PREFETCH_MODE = "full"
DEFAULT_REQUEST_SPARSITY_SLOW_CLIENT_SEND_PAUSE_ITERS = 16384
RUN_DISABLE_SENTINEL_ENV = "SIMCXL_RPC_RUN_DISABLE_FILE"
RUN_DISABLE_SENTINEL_REL = "output/SIMCXL_RUNS_DISABLED"
WORKLOAD_KIND_BARE_RPC = "bare_rpc"
WORKLOAD_KIND_APPLICATION = "application"
MESSAGE_PROFILE_FIXED = "fixed"
MESSAGE_PROFILE_UNIFORM_1530_315 = "uniform-1530-315"
MESSAGE_PROFILE_UNIFORM_38_230 = "uniform-38-230"
CURRENT_CLIENTS = 32
APPLICATION_CLIENTS = 32
APPLICATION_RECORD_COUNT = 10000
APPLICATION_DATASET_SEED = 0x9B5D3A4781C26EF1
APPLICATION_WORKLOAD_SEED = 0xC7D51A32049EF68B
APPLICATION_PROFILES = (
    "ycsb_a_1k",
    "ycsb_b_1k",
    "ycsb_c_1k",
    "ycsb_d_1k",
    "udb_a",
    "udb_b",
    "udb_c",
    "udb_d",
)
OVERALL_CLIENT_COUNTS = [1, 2, 4, 8, 16, 32]
OVERALL_WORKLOADS = [
    (64, 64, MESSAGE_PROFILE_FIXED),
    (1530, 315, MESSAGE_PROFILE_UNIFORM_1530_315),
    (38, 230, MESSAGE_PROFILE_UNIFORM_38_230),
]
SENSITIVITY_REQUEST_SIZES = [8, 64, 256, 1024, 4096, 8192]
SENSITIVITY_RESPONSE_SIZES = [8, 64, 256, 1024, 4096, 8192]
SENSITIVITY_MQ_ENTRIES = [16, 32, 64, 128, 256, 512]
TECH_HEAD_SYNC_THRESHOLDS = [8, 16, 32, 64, 128, 256, 512]
SENSITIVITY_REQUEST_SPARSITY_CLIENTS = 16
SENSITIVITY_REQUEST_SPARSITY_SLOW_CLIENT_COUNTS = [1, 2, 4, 8]
SENSITIVITY_REQUEST_SPARSITY_SLOW_REQUEST_COUNTS = [0, 8, 15]
SENSITIVITY_CXL_LATENCIES_NS = [100, 200, 300, 400, 500, 600]
TECH_DISABLE_DMA_THRESHOLD = 10000
UNIFORM_1530_315_REQ_MIN = 765
UNIFORM_1530_315_REQ_MAX = 2295
UNIFORM_1530_315_RESP_MIN = 158
UNIFORM_1530_315_RESP_MAX = 472
UNIFORM_38_230_REQ_MIN = 19
UNIFORM_38_230_REQ_MAX = 57
UNIFORM_38_230_RESP_MIN = 115
UNIFORM_38_230_RESP_MAX = 345
OBSOLETE_REQUEST_SPARSITY_EXP_IDS = {
    "req64_resp64_c4_r30_slow1_sp16384",
    "req64_resp64_c4_r30_slow2_sp16384",
    "req64_resp64_c8_r30_slow2_sp16384",
    "req64_resp64_c8_r30_slow4_sp16384",
    "req64_resp64_c16_r30_slow4_sp16384",
    "req64_resp64_c16_r30_slow8_sp16384",
    "req64_resp64_c20_r30",
    "req64_resp64_c20_r30_slow5_sp16384",
    "req64_resp64_c20_r30_slow10_sp16384",
    "req64_resp64_c24_r30",
    "req64_resp64_c24_r30_slow6_sp16384",
    "req64_resp64_c24_r30_slow12_sp16384",
    "req64_resp64_c28_r30",
    "req64_resp64_c28_r30_slow7_sp16384",
    "req64_resp64_c28_r30_slow14_sp16384",
}


@dataclass(frozen=True)
class ApplicationProfileConfig:
    name: str
    key_size: int
    value_size: int
    size_mode: str
    key_dist: str
    read_ratio: float
    update_ratio: float
    rmw_ratio: float
    insert_ratio: float
    zipf_theta: float


APPLICATION_PROFILE_CONFIGS: Dict[str, ApplicationProfileConfig] = {
    "ycsb_c_1k": ApplicationProfileConfig(
        name="ycsb_c_1k",
        key_size=16,
        value_size=1024,
        size_mode="fixed",
        key_dist="zipf",
        read_ratio=1.0,
        update_ratio=0.0,
        rmw_ratio=0.0,
        insert_ratio=0.0,
        zipf_theta=0.99,
    ),
    "ycsb_a_1k": ApplicationProfileConfig(
        name="ycsb_a_1k",
        key_size=16,
        value_size=1024,
        size_mode="fixed",
        key_dist="zipf",
        read_ratio=0.5,
        update_ratio=0.5,
        rmw_ratio=0.0,
        insert_ratio=0.0,
        zipf_theta=0.99,
    ),
    "ycsb_b_1k": ApplicationProfileConfig(
        name="ycsb_b_1k",
        key_size=16,
        value_size=1024,
        size_mode="fixed",
        key_dist="zipf",
        read_ratio=0.95,
        update_ratio=0.05,
        rmw_ratio=0.0,
        insert_ratio=0.0,
        zipf_theta=0.99,
    ),
    "ycsb_d_1k": ApplicationProfileConfig(
        name="ycsb_d_1k",
        key_size=16,
        value_size=1024,
        size_mode="fixed",
        key_dist="latest",
        read_ratio=0.95,
        update_ratio=0.0,
        rmw_ratio=0.0,
        insert_ratio=0.05,
        zipf_theta=0.0,
    ),
    "udb_a": ApplicationProfileConfig(
        name="udb_a",
        key_size=27,
        value_size=127,
        size_mode="variable",
        key_dist="zipf",
        read_ratio=0.5,
        update_ratio=0.5,
        rmw_ratio=0.0,
        insert_ratio=0.0,
        zipf_theta=0.99,
    ),
    "udb_b": ApplicationProfileConfig(
        name="udb_b",
        key_size=27,
        value_size=127,
        size_mode="variable",
        key_dist="zipf",
        read_ratio=0.95,
        update_ratio=0.05,
        rmw_ratio=0.0,
        insert_ratio=0.0,
        zipf_theta=0.99,
    ),
    "udb_c": ApplicationProfileConfig(
        name="udb_c",
        key_size=27,
        value_size=127,
        size_mode="variable",
        key_dist="zipf",
        read_ratio=1.0,
        update_ratio=0.0,
        rmw_ratio=0.0,
        insert_ratio=0.0,
        zipf_theta=0.99,
    ),
    "udb_d": ApplicationProfileConfig(
        name="udb_d",
        key_size=27,
        value_size=127,
        size_mode="variable",
        key_dist="latest",
        read_ratio=0.95,
        update_ratio=0.0,
        rmw_ratio=0.0,
        insert_ratio=0.05,
        zipf_theta=0.0,
    ),
}


@dataclass(frozen=True)
class ExperimentKey:
    request_size: int
    response_size: int
    clients: int
    requests_per_client: int
    request_min_size: int = 0
    request_max_size: int = 0
    response_min_size: int = 0
    response_max_size: int = 0
    mq_entries: int = DEFAULT_MQ_ENTRIES
    head_sync_threshold: int = 0
    cxl_extra_latency_ns: int = 0
    response_lane_count: int = 0
    clients_per_dma_lane: int = 1
    response_dma_threshold: int = DEFAULT_RESPONSE_DMA_THRESHOLD
    prefetch_mode: str = DEFAULT_PREFETCH_MODE
    message_profile: str = MESSAGE_PROFILE_FIXED
    slow_client_count: int = 0
    slow_count_per_client: int = 0
    slow_client_send_pause_iters: int = 0
    workload_kind: str = WORKLOAD_KIND_BARE_RPC
    app_profile: str = ""
    record_count: int = 0
    key_size: int = 0
    value_size: int = 0
    size_mode: str = ""
    key_dist: str = ""
    read_ratio: float = 0.0
    update_ratio: float = 0.0
    rmw_ratio: float = 0.0
    insert_ratio: float = 0.0
    zipf_theta: float = 0.0
    dataset_seed: int = 0
    workload_seed: int = 0


@dataclass
class Experiment:
    key: ExperimentKey
    exp_id: str
    source: str


def now_tag() -> str:
    return dt.datetime.now().strftime("%Y%m%d_%H%M%S")


def resolve_repo_root(cli_repo_root: Optional[str]) -> Path:
    if cli_repo_root:
        return Path(cli_repo_root).resolve()
    # .../tests/test-progs/cxl-rpc/scripts/run_rpc_matrix_kvm_timing_ckpt.py
    return Path(__file__).resolve().parents[4]


def resolve_run_disable_sentinel(repo_root: Path) -> Path:
    override = os.environ.get(RUN_DISABLE_SENTINEL_ENV, "").strip()
    if override:
        return Path(override).expanduser().resolve()
    return (repo_root / RUN_DISABLE_SENTINEL_REL).resolve()


def read_run_disable_reason(repo_root: Path) -> Optional[str]:
    sentinel = resolve_run_disable_sentinel(repo_root)
    if not sentinel.exists():
        return None
    try:
        text = sentinel.read_text(encoding="utf-8", errors="ignore").strip()
    except OSError:
        return str(sentinel)
    if not text:
        return str(sentinel)
    first_line = text.splitlines()[0].strip()
    if not first_line:
        return str(sentinel)
    return f"{sentinel}: {first_line}"


def ceil_div(numer: int, denom: int) -> int:
    return (numer + denom - 1) // denom


def default_head_sync_threshold(mq_entries: int) -> int:
    return mq_entries // 4


def message_profile_max_response_size(message_profile: str,
                                      fixed_response_size: int) -> int:
    if message_profile == MESSAGE_PROFILE_FIXED:
        return fixed_response_size
    if message_profile == MESSAGE_PROFILE_UNIFORM_1530_315:
        return 472
    if message_profile == MESSAGE_PROFILE_UNIFORM_38_230:
        return 345
    return fixed_response_size


def cpu_only_dma_threshold(message_profile: str,
                           response_size: int,
                           default_threshold: int) -> int:
    if message_profile_max_response_size(message_profile, response_size) < default_threshold:
        return default_threshold
    return TECH_DISABLE_DMA_THRESHOLD


def format_exp_id(key: ExperimentKey) -> str:
    if key.workload_kind == WORKLOAD_KIND_APPLICATION:
        parts = [
            key.app_profile,
            f"c{key.clients}",
            f"r{key.requests_per_client}",
            f"rec{key.record_count}",
        ]
        if key.mq_entries != DEFAULT_MQ_ENTRIES:
            parts.append(f"mq{key.mq_entries}")
        if key.head_sync_threshold != default_head_sync_threshold(key.mq_entries):
            parts.append(f"hs{key.head_sync_threshold}")
        if key.cxl_extra_latency_ns > 0:
            parts.append(f"cxlp{key.cxl_extra_latency_ns}ns")
        if key.clients_per_dma_lane != 1:
            parts.append(f"cpl{key.clients_per_dma_lane}")
        if key.response_dma_threshold != DEFAULT_RESPONSE_DMA_THRESHOLD:
            parts.append(f"dmath{key.response_dma_threshold}")
        if key.prefetch_mode != DEFAULT_PREFETCH_MODE:
            parts.append(f"pf{key.prefetch_mode.replace('-', '_')}")
        if key.dataset_seed != APPLICATION_DATASET_SEED:
            parts.append(f"dseed{key.dataset_seed:x}")
        if key.workload_seed != APPLICATION_WORKLOAD_SEED:
            parts.append(f"wseed{key.workload_seed:x}")
        return "_".join(parts)

    def size_tag(prefix: str, base: int, min_size: int, max_size: int) -> str:
        if min_size > 0 and max_size > 0 and min_size != max_size:
            return f"{prefix}u{min_size}-{max_size}"
        return f"{prefix}{base}"

    parts = [
        size_tag("req", key.request_size, key.request_min_size, key.request_max_size),
        size_tag("resp", key.response_size, key.response_min_size, key.response_max_size),
        f"c{key.clients}",
        f"r{key.requests_per_client}",
    ]
    if key.mq_entries != DEFAULT_MQ_ENTRIES:
        parts.append(f"mq{key.mq_entries}")
    if key.head_sync_threshold != default_head_sync_threshold(key.mq_entries):
        parts.append(f"hs{key.head_sync_threshold}")
    if key.cxl_extra_latency_ns > 0:
        parts.append(f"cxlp{key.cxl_extra_latency_ns}ns")
    if key.clients_per_dma_lane != 1:
        parts.append(f"cpl{key.clients_per_dma_lane}")
    if key.response_dma_threshold != DEFAULT_RESPONSE_DMA_THRESHOLD:
        parts.append(f"dmath{key.response_dma_threshold}")
    if key.prefetch_mode != DEFAULT_PREFETCH_MODE:
        parts.append(f"pf{key.prefetch_mode.replace('-', '_')}")
    if key.message_profile != MESSAGE_PROFILE_FIXED:
        parts.append(f"mprof{key.message_profile.replace('-', '_')}")
    if key.slow_client_count > 0:
        parts.append(f"slow{key.slow_client_count}")
        parts.append(f"sq{key.slow_count_per_client}")
        if key.slow_client_send_pause_iters > 0:
            parts.append(f"sp{key.slow_client_send_pause_iters}")
    return "_".join(parts)


def expected_total_requests(key: ExperimentKey) -> int:
    slow_clients = min(key.slow_client_count, key.clients)
    if slow_clients <= 0:
        return key.clients * key.requests_per_client

    slow_requests = key.slow_count_per_client
    fast_clients = key.clients - slow_clients
    return (fast_clients * key.requests_per_client) + (slow_clients * slow_requests)


def format_zipf_field(key_dist: str, zipf_theta: float) -> str:
    if key_dist != "zipf":
        return ""
    return f"{zipf_theta:.6f}"


def is_application_experiment(key: ExperimentKey) -> bool:
    return key.workload_kind == WORKLOAD_KIND_APPLICATION


def build_matrix(requests_per_client: int,
                 response_dma_threshold: int,
                 prefetch_mode: str,
                 request_sparsity_slow_client_send_pause_iters: int) -> List[Experiment]:
    dedup: Dict[ExperimentKey, Experiment] = {}
    default_hs = default_head_sync_threshold(DEFAULT_MQ_ENTRIES)

    def add_experiment(key: ExperimentKey, source: str) -> None:
        existing = dedup.get(key)
        if existing is not None:
            existing_sources = set(existing.source.split(","))
            if source not in existing_sources:
                existing.source = f"{existing.source},{source}"
            return

        dedup[key] = Experiment(
            key=key,
            exp_id=format_exp_id(key),
            source=source,
        )

    for clients in OVERALL_CLIENT_COUNTS:
        for request_size, response_size, message_profile in OVERALL_WORKLOADS:
            add_experiment(
                ExperimentKey(
                    request_size=request_size,
                    response_size=response_size,
                    clients=clients,
                    requests_per_client=requests_per_client,
                    head_sync_threshold=default_hs,
                    response_lane_count=clients,
                    response_dma_threshold=response_dma_threshold,
                    prefetch_mode=prefetch_mode,
                    message_profile=message_profile,
                ),
                "overall",
            )

    for profile_name in APPLICATION_PROFILES:
        profile = APPLICATION_PROFILE_CONFIGS[profile_name]
        add_experiment(
            ExperimentKey(
                request_size=profile.key_size,
                response_size=profile.value_size,
                clients=APPLICATION_CLIENTS,
                requests_per_client=requests_per_client,
                head_sync_threshold=default_hs,
                response_lane_count=APPLICATION_CLIENTS,
                response_dma_threshold=response_dma_threshold,
                prefetch_mode=prefetch_mode,
                workload_kind=WORKLOAD_KIND_APPLICATION,
                app_profile=profile.name,
                record_count=APPLICATION_RECORD_COUNT,
                key_size=profile.key_size,
                value_size=profile.value_size,
                size_mode=profile.size_mode,
                key_dist=profile.key_dist,
                read_ratio=profile.read_ratio,
                update_ratio=profile.update_ratio,
                rmw_ratio=profile.rmw_ratio,
                insert_ratio=profile.insert_ratio,
                zipf_theta=profile.zipf_theta,
                dataset_seed=APPLICATION_DATASET_SEED,
                workload_seed=APPLICATION_WORKLOAD_SEED,
            ),
            "application",
        )

    for request_size, response_size, message_profile in OVERALL_WORKLOADS:
        no_dma_threshold = cpu_only_dma_threshold(message_profile,
                                                  response_size,
                                                  response_dma_threshold)
        for source, tech_prefetch_mode, tech_dma_threshold in (
            ("technical_cpu_only_no_prefetch", "none", no_dma_threshold),
            ("technical_cpu_only_with_prefetch", prefetch_mode, no_dma_threshold),
            ("technical_dma_no_prefetch", "none", response_dma_threshold),
            ("technical_dma_with_prefetch", prefetch_mode, response_dma_threshold),
        ):
            add_experiment(
                ExperimentKey(
                    request_size=request_size,
                    response_size=response_size,
                    clients=CURRENT_CLIENTS,
                    requests_per_client=requests_per_client,
                    head_sync_threshold=default_hs,
                    response_lane_count=CURRENT_CLIENTS,
                    response_dma_threshold=tech_dma_threshold,
                    prefetch_mode=tech_prefetch_mode,
                    message_profile=message_profile,
                ),
                source,
            )

    # `100%` sparse density is canonicalized to the overall c16 req64/resp64 point.
    for slow_client_count in SENSITIVITY_REQUEST_SPARSITY_SLOW_CLIENT_COUNTS:
        for slow_count_per_client in SENSITIVITY_REQUEST_SPARSITY_SLOW_REQUEST_COUNTS:
            add_experiment(
                ExperimentKey(
                    request_size=64,
                    response_size=64,
                    clients=SENSITIVITY_REQUEST_SPARSITY_CLIENTS,
                    requests_per_client=requests_per_client,
                    head_sync_threshold=default_hs,
                    response_lane_count=SENSITIVITY_REQUEST_SPARSITY_CLIENTS,
                    response_dma_threshold=response_dma_threshold,
                    prefetch_mode=prefetch_mode,
                    message_profile=MESSAGE_PROFILE_FIXED,
                    slow_client_count=slow_client_count,
                    slow_count_per_client=slow_count_per_client,
                    slow_client_send_pause_iters=(
                        request_sparsity_slow_client_send_pause_iters
                        if (slow_client_count > 0 and
                            slow_count_per_client > 0 and
                            slow_count_per_client < requests_per_client)
                        else 0
                    ),
                ),
                "motivation_request_sparsity",
            )

    for request_size in SENSITIVITY_REQUEST_SIZES:
        add_experiment(
            ExperimentKey(
                request_size=request_size,
                response_size=230,
                response_min_size=UNIFORM_38_230_RESP_MIN,
                response_max_size=UNIFORM_38_230_RESP_MAX,
                clients=CURRENT_CLIENTS,
                requests_per_client=requests_per_client,
                head_sync_threshold=default_hs,
                response_lane_count=CURRENT_CLIENTS,
                response_dma_threshold=response_dma_threshold,
                prefetch_mode=prefetch_mode,
                message_profile=MESSAGE_PROFILE_FIXED,
            ),
            "sensitivity_request_size",
        )

    for response_size in SENSITIVITY_RESPONSE_SIZES:
        add_experiment(
            ExperimentKey(
                request_size=38,
                request_min_size=UNIFORM_38_230_REQ_MIN,
                request_max_size=UNIFORM_38_230_REQ_MAX,
                response_size=response_size,
                clients=CURRENT_CLIENTS,
                requests_per_client=requests_per_client,
                head_sync_threshold=default_hs,
                response_lane_count=CURRENT_CLIENTS,
                response_dma_threshold=response_dma_threshold,
                prefetch_mode=prefetch_mode,
                message_profile=MESSAGE_PROFILE_FIXED,
            ),
            "sensitivity_response_size",
        )

    for mq_entries in SENSITIVITY_MQ_ENTRIES:
        add_experiment(
            ExperimentKey(
                request_size=38,
                response_size=230,
                request_min_size=UNIFORM_38_230_REQ_MIN,
                request_max_size=UNIFORM_38_230_REQ_MAX,
                response_min_size=UNIFORM_38_230_RESP_MIN,
                response_max_size=UNIFORM_38_230_RESP_MAX,
                clients=CURRENT_CLIENTS,
                requests_per_client=requests_per_client,
                mq_entries=mq_entries,
                head_sync_threshold=default_head_sync_threshold(mq_entries),
                response_lane_count=CURRENT_CLIENTS,
                response_dma_threshold=response_dma_threshold,
                prefetch_mode=prefetch_mode,
                message_profile=MESSAGE_PROFILE_FIXED,
            ),
            "sensitivity_mq_entries",
        )

    for head_sync_threshold in TECH_HEAD_SYNC_THRESHOLDS:
        add_experiment(
            ExperimentKey(
                request_size=64,
                response_size=64,
                clients=CURRENT_CLIENTS,
                requests_per_client=requests_per_client,
                mq_entries=DEFAULT_MQ_ENTRIES,
                head_sync_threshold=head_sync_threshold,
                response_lane_count=CURRENT_CLIENTS,
                response_dma_threshold=response_dma_threshold,
                prefetch_mode=prefetch_mode,
                message_profile=MESSAGE_PROFILE_FIXED,
            ),
            "technical_head_sync",
        )

    # `100%` sparse density is canonicalized to the overall c16 req38/resp230 point.
    for slow_client_count in SENSITIVITY_REQUEST_SPARSITY_SLOW_CLIENT_COUNTS:
        for slow_count_per_client in SENSITIVITY_REQUEST_SPARSITY_SLOW_REQUEST_COUNTS:
            add_experiment(
                ExperimentKey(
                    request_size=38,
                    response_size=230,
                    request_min_size=UNIFORM_38_230_REQ_MIN,
                    request_max_size=UNIFORM_38_230_REQ_MAX,
                    response_min_size=UNIFORM_38_230_RESP_MIN,
                    response_max_size=UNIFORM_38_230_RESP_MAX,
                    clients=SENSITIVITY_REQUEST_SPARSITY_CLIENTS,
                    requests_per_client=requests_per_client,
                    mq_entries=DEFAULT_MQ_ENTRIES,
                    head_sync_threshold=default_hs,
                    response_lane_count=SENSITIVITY_REQUEST_SPARSITY_CLIENTS,
                    response_dma_threshold=response_dma_threshold,
                    prefetch_mode=prefetch_mode,
                    message_profile=MESSAGE_PROFILE_FIXED,
                    slow_client_count=slow_client_count,
                    slow_count_per_client=slow_count_per_client,
                    slow_client_send_pause_iters=(
                        request_sparsity_slow_client_send_pause_iters
                        if (slow_client_count > 0 and
                            slow_count_per_client > 0 and
                            slow_count_per_client < requests_per_client)
                        else 0
                    ),
                ),
                "sensitivity_request_sparsity",
            )

    for cxl_extra_latency_ns in SENSITIVITY_CXL_LATENCIES_NS:
        add_experiment(
            ExperimentKey(
                request_size=38,
                response_size=230,
                request_min_size=UNIFORM_38_230_REQ_MIN,
                request_max_size=UNIFORM_38_230_REQ_MAX,
                response_min_size=UNIFORM_38_230_RESP_MIN,
                response_max_size=UNIFORM_38_230_RESP_MAX,
                clients=CURRENT_CLIENTS,
                requests_per_client=requests_per_client,
                mq_entries=DEFAULT_MQ_ENTRIES,
                head_sync_threshold=default_hs,
                cxl_extra_latency_ns=cxl_extra_latency_ns,
                response_lane_count=CURRENT_CLIENTS,
                response_dma_threshold=response_dma_threshold,
                prefetch_mode=prefetch_mode,
                message_profile=MESSAGE_PROFILE_FIXED,
            ),
            "sensitivity_cxl_latency",
        )

    return list(dedup.values())


def _signal_name(sig: signal.Signals) -> str:
    return sig.name[3:] if sig.name.startswith("SIG") else sig.name


def process_group_alive(pgid: int,
                        sudo_signal_prefix: Optional[List[str]] = None) -> bool:
    if pgid <= 0:
        return False

    if sudo_signal_prefix:
        proc = subprocess.run(
            sudo_signal_prefix + ["kill", "-0", "--", f"-{pgid}"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return proc.returncode == 0

    try:
        os.killpg(pgid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def terminate_process_group(pgid: int,
                            sig: signal.Signals,
                            sudo_signal_prefix: Optional[List[str]] = None) -> None:
    if pgid <= 0:
        return
    if not process_group_alive(pgid, sudo_signal_prefix):
        return

    if sudo_signal_prefix:
        subprocess.run(
            sudo_signal_prefix +
            ["kill", f"-{_signal_name(sig)}", "--", f"-{pgid}"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return

    try:
        os.killpg(pgid, sig)
    except ProcessLookupError:
        return


def wait_for_process_group_exit(pgid: int,
                                timeout_sec: float,
                                sudo_signal_prefix: Optional[List[str]] = None) -> bool:
    deadline = time.monotonic() + max(0.0, timeout_sec)
    while process_group_alive(pgid, sudo_signal_prefix):
        if time.monotonic() >= deadline:
            return False
        time.sleep(min(0.2, max(0.0, deadline - time.monotonic())))
    return True


def acquire_matrix_lock(lock_path: Path, batch_name: str):
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    lock_fp = lock_path.open("a+", encoding="utf-8")
    try:
        fcntl.flock(lock_fp.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        lock_fp.seek(0)
        holder = lock_fp.read().strip() or "unknown-owner"
        print(
            f"[fatal] another matrix wrapper already holds {lock_path}: {holder}"
        )
        lock_fp.close()
        return None

    lock_fp.seek(0)
    lock_fp.truncate()
    lock_fp.write(
        f"pid={os.getpid()} batch={batch_name} "
        f"started_at={dt.datetime.now().isoformat(timespec='seconds')}\n"
    )
    lock_fp.flush()
    return lock_fp


def release_matrix_lock(lock_fp) -> None:
    if lock_fp is None:
        return
    try:
        fcntl.flock(lock_fp.fileno(), fcntl.LOCK_UN)
    finally:
        lock_fp.close()


def run_and_tee(cmd: List[str],
                log_path: Path,
                sudo_signal_prefix: Optional[List[str]] = None) -> int:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    print("+", " ".join(shlex.quote(c) for c in cmd), flush=True)
    proc = None
    proc_group = 0
    with log_path.open("w", encoding="utf-8") as logf:
        try:
            proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                start_new_session=True,
            )
            proc_group = proc.pid
            assert proc.stdout is not None
            for line in proc.stdout:
                sys.stdout.write(line)
                logf.write(line)
            proc.wait()
        except BaseException:
            if proc_group > 0:
                terminate_process_group(
                    proc_group, signal.SIGTERM, sudo_signal_prefix
                )
                if not wait_for_process_group_exit(
                    proc_group, 5.0, sudo_signal_prefix
                ):
                    terminate_process_group(
                        proc_group, signal.SIGKILL, sudo_signal_prefix
                    )
                    wait_for_process_group_exit(
                        proc_group, 5.0, sudo_signal_prefix
                    )
            raise
        finally:
            if proc is not None and proc.stdout is not None:
                proc.stdout.close()

    if proc_group > 0 and not wait_for_process_group_exit(
        proc_group, 0.5, sudo_signal_prefix
    ):
        terminate_process_group(proc_group, signal.SIGTERM, sudo_signal_prefix)
        if not wait_for_process_group_exit(proc_group, 10.0, sudo_signal_prefix):
            terminate_process_group(
                proc_group, signal.SIGKILL, sudo_signal_prefix
            )
            if not wait_for_process_group_exit(
                proc_group, 5.0, sudo_signal_prefix
            ):
                raise RuntimeError(
                    "subprocess process-group did not exit cleanly "
                    f"pgid={proc_group} cmd={' '.join(shlex.quote(c) for c in cmd)}"
                )

    assert proc is not None
    return int(proc.returncode)


def sudo_nopass_available() -> bool:
    if hasattr(os, "geteuid") and os.geteuid() == 0:
        return True
    try:
        proc = subprocess.run(
            ["sudo", "-n", "true"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except OSError:
        return False
    return proc.returncode == 0


def resolve_checkpoint_dir(path: Path) -> Optional[Path]:
    if (path / "m5.cpt").exists():
        return path
    if path.is_file() and path.name == "m5.cpt":
        return path.parent
    if path.exists():
        candidates = sorted(
            path.rglob("m5.cpt"),
            key=lambda p: p.stat().st_mtime,
            reverse=True,
        )
        if candidates:
            return candidates[0].parent
    return None


CLIENT_BARE_RE = re.compile(
    r"^req_(\d+)_(start|end)_tick=(\d+)\s*$"
)
SERVER_LINE_RE = re.compile(
    r"^server_req_(\d+)_(poll_tick|execute_tick|response_tick)=(\d+)\s*$"
)
CLIENT_HOST_LOG_RE = re.compile(r"^rpc_client_runtime_(\d+)\.log$")


def parse_test_rc_lines(lines: List[str]) -> Optional[int]:
    test_rc: Optional[int] = None

    for line in lines:
        if "TEST_CMD_EXIT_CODE=" not in line:
            continue
        try:
            test_rc = int(line.split("TEST_CMD_EXIT_CODE=", 1)[1].strip())
        except ValueError:
            continue

    return test_rc


def build_client_rows(
    client_fields: Dict[Tuple[int, int], Dict[str, int]]
) -> List[Dict[str, int]]:
    client_rows: List[Dict[str, int]] = []

    for (node_id, ridx), vals in sorted(client_fields.items()):
        if "start" not in vals or "end" not in vals:
            continue
        client_rows.append(
            {
                "node_id": node_id,
                "req_index": ridx,
                "start_tick": vals["start"],
                "end_tick": vals["end"],
            }
        )

    return client_rows


def build_server_rows(
    server_fields: Dict[int, Dict[str, int]]
) -> List[Dict[str, int]]:
    server_rows: List[Dict[str, int]] = []

    for req_index, vals in sorted(server_fields.items()):
        required = ("poll_tick", "execute_tick", "response_tick")
        if not all(name in vals for name in required):
            continue
        server_rows.append(
            {
                "server_req_index": req_index,
                "poll_tick": vals["poll_tick"],
                "execute_tick": vals["execute_tick"],
                "response_tick": vals["response_tick"],
            }
        )

    return server_rows


def parse_status_file(status_path: Path) -> Optional[int]:
    if not status_path.exists():
        return None

    return parse_test_rc_lines(
        status_path.read_text(encoding="utf-8", errors="ignore").splitlines()
    )


def parse_client_log(
    log_path: Path,
    node_id: int,
) -> List[Dict[str, int]]:
    client_fields: Dict[Tuple[int, int], Dict[str, int]] = {}

    for line in log_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        m = CLIENT_BARE_RE.match(line.strip())
        if not m:
            continue
        ridx = int(m.group(1))
        kind = m.group(2)
        val = int(m.group(3))
        client_fields.setdefault((node_id, ridx), {})[kind] = val

    return build_client_rows(client_fields)


def parse_server_log(log_path: Path) -> List[Dict[str, int]]:
    server_fields: Dict[int, Dict[str, int]] = {}

    for line in log_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        m = SERVER_LINE_RE.match(line.strip())
        if not m:
            continue
        req_index = int(m.group(1))
        kind = m.group(2)
        val = int(m.group(3))
        server_fields.setdefault(req_index, {})[kind] = val

    return build_server_rows(server_fields)


def parse_writefile_results(
    run_outdir: Path,
) -> Tuple[Optional[int], List[Dict[str, int]], List[Dict[str, int]]]:
    status_path = run_outdir / "rpc_status.txt"
    server_log_path = run_outdir / "rpc_server_runtime.log"
    client_logs = sorted(run_outdir.glob("rpc_client_runtime_*.log"))

    if not status_path.exists() and not server_log_path.exists() and not client_logs:
        return None, [], []

    client_rows: List[Dict[str, int]] = []
    for client_log in client_logs:
        m = CLIENT_HOST_LOG_RE.match(client_log.name)
        if not m:
            continue
        client_rows.extend(parse_client_log(client_log, int(m.group(1))))

    server_rows = parse_server_log(server_log_path) if server_log_path.exists() else []
    test_rc = parse_status_file(status_path)
    return test_rc, client_rows, server_rows


def parse_run_results(
    run_outdir: Path,
) -> Tuple[Optional[int], List[Dict[str, int]], List[Dict[str, int]]]:
    return parse_writefile_results(run_outdir)


def read_success_exp_ids(csv_path: Path) -> set[str]:
    if not csv_path.exists():
        return set()
    done: set[str] = set()
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row.get("status") == "ok":
                exp_id = row.get("exp_id", "")
                if exp_id:
                    done.add(exp_id)
    return done


def append_row(csv_path: Path, row: dict, fieldnames: List[str]) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    need_header = not csv_path.exists()
    with csv_path.open("a", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        if need_header:
            writer.writeheader()
        writer.writerow(row)


def experiment_metadata_row(key: ExperimentKey) -> Dict[str, object]:
    is_app = is_application_experiment(key)
    return {
        "workload_kind": key.workload_kind,
        "request_size": key.request_size,
        "response_size": key.response_size,
        "request_min_size": key.request_min_size,
        "request_max_size": key.request_max_size,
        "response_min_size": key.response_min_size,
        "response_max_size": key.response_max_size,
        "clients": key.clients,
        "requests_per_client": key.requests_per_client,
        "mq_entries": key.mq_entries,
        "head_sync_threshold": key.head_sync_threshold,
        "slow_client_count": key.slow_client_count,
        "slow_count_per_client": key.slow_count_per_client,
        "slow_client_send_pause_iters": key.slow_client_send_pause_iters,
        "cxl_extra_latency_ns": key.cxl_extra_latency_ns,
        "response_lane_count": key.response_lane_count,
        "clients_per_dma_lane": key.clients_per_dma_lane,
        "response_dma_threshold": key.response_dma_threshold,
        "prefetch_mode": key.prefetch_mode,
        "message_profile": "" if is_app else key.message_profile,
        "app_profile": key.app_profile if is_app else "",
        "record_count": key.record_count if is_app else "",
        "key_size": key.key_size if is_app else "",
        "value_size": key.value_size if is_app else "",
        "size_mode": key.size_mode if is_app else "",
        "key_dist": key.key_dist if is_app else "",
        "read_ratio": f"{key.read_ratio:.6f}" if is_app else "",
        "update_ratio": f"{key.update_ratio:.6f}" if is_app else "",
        "rmw_ratio": f"{key.rmw_ratio:.6f}" if is_app else "",
        "insert_ratio": f"{key.insert_ratio:.6f}" if is_app else "",
        "zipf_theta": format_zipf_field(key.key_dist, key.zipf_theta) if is_app else "",
        "dataset_seed": str(key.dataset_seed) if is_app else "",
        "workload_seed": str(key.workload_seed) if is_app else "",
    }


def app_profile_arg_list(key: ExperimentKey) -> List[str]:
    args = [
        f"--profile {key.app_profile}",
        f"--record-count {key.record_count}",
        f"--dataset-seed {key.dataset_seed}",
    ]
    if key.size_mode == "fixed":
        args.extend(
            [
                f"--key-size {key.key_size}",
                f"--value-size {key.value_size}",
            ]
        )
    return args


def app_workload_arg_list(key: ExperimentKey) -> List[str]:
    args = [
        f"--workload-seed {key.workload_seed}",
        f"--read-ratio {key.read_ratio:.6f}",
        f"--update-ratio {key.update_ratio:.6f}",
        f"--rmw-ratio {key.rmw_ratio:.6f}",
        f"--insert-ratio {key.insert_ratio:.6f}",
    ]
    if key.key_dist == "zipf":
        args.append(f"--zipf-theta {key.zipf_theta:.6f}")
    return args


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run CXL RPC matrix (KVM+TIMING+checkpoint) and extract ticks"
    )
    parser.add_argument("--repo-root", type=str, default=None)
    parser.add_argument("--output-base", type=str, default="output")
    parser.add_argument("--batch-name", type=str, default="")
    parser.add_argument(
        "--disk",
        type=str,
        default="",
        help="Path to guest disk image. Empty uses repo_root/files/parsec.img.",
    )
    parser.add_argument(
        "--checkpoint-dir",
        type=str,
        default="",
        help=(
            "Reuse this existing checkpoint for every experiment in the batch. "
            "Caller is responsible for ensuring topology compatibility."
        ),
    )
    parser.add_argument("--requests", type=int, default=30)
    parser.add_argument(
        "--response-dma-threshold",
        type=int,
        default=DEFAULT_RESPONSE_DMA_THRESHOLD,
        help="Response payload size threshold for switching to DMA publish.",
    )
    parser.add_argument(
        "--prefetch-mode",
        type=str,
        default=DEFAULT_PREFETCH_MODE,
        help="Server request-RX prefetch mode: full|no-request|none.",
    )
    parser.add_argument(
        "--request-sparsity-slow-client-send-pause-iters",
        type=int,
        default=DEFAULT_REQUEST_SPARSITY_SLOW_CLIENT_SEND_PAUSE_ITERS,
        help=(
            "Extra pause iterations inserted before each send from slow clients "
            "in the request-sparsity sweep."
        ),
    )
    parser.add_argument(
        "--only-exp-id",
        type=str,
        default="",
        help="Run only the experiment whose exp_id exactly matches this value",
    )
    parser.add_argument(
        "--start-index",
        type=int,
        default=1,
        help="1-based experiment index to start from (skip earlier ones)",
    )
    parser.add_argument(
        "--end-index",
        type=int,
        default=0,
        help="1-based experiment index to stop at (0 means run through the end)",
    )
    parser.add_argument(
        "--copy-engine-channels",
        type=int,
        default=0,
        help=(
            "Pass --copy_engine_channels through to both checkpoint and timing "
            "configs. 0 keeps their auto-derived topology."
        ),
    )
    parser.add_argument(
        "--checkpoint-handoff-deadline-sim-seconds",
        type=int,
        default=0,
        help=(
            "Pass --handoff_deadline_sim_seconds to checkpoint generation. "
            "0 keeps the checkpoint script default."
        ),
    )
    parser.add_argument(
        "--boot-cpu-type",
        type=str,
        choices=["KVM", "TIMING", "ATOMIC"],
        default="KVM",
        help=(
            "CPU type used during checkpoint creation and pre-test restore. "
            "TIMING/ATOMIC avoid /dev/kvm but are much slower than KVM."
        ),
    )
    parser.add_argument("--skip-inject", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--force-rerun", action="store_true")
    parser.add_argument(
        "--allow-concurrent-runs",
        action="store_true",
        help=(
            "Skip the default exclusive wrapper lock so multiple matrix "
            "batches can run in parallel."
        ),
    )
    parser.add_argument(
        "--inter-experiment-sleep-sec",
        type=int,
        default=30,
        help="Sleep this many seconds between actually executed experiments",
    )
    args = parser.parse_args()

    if args.only_exp_id in OBSOLETE_REQUEST_SPARSITY_EXP_IDS:
        print(
            "[skip] obsolete sparse exp_id from old incorrect plan: "
            f"{args.only_exp_id}"
        )
        return 0

    repo_root = resolve_repo_root(args.repo_root)
    disable_reason = read_run_disable_reason(repo_root)
    if disable_reason is not None:
        if args.only_exp_id:
            print(
                "[skip] SimCXL experiments are disabled by sentinel for "
                f"exp_id={args.only_exp_id}: {disable_reason}"
            )
        else:
            print(f"[skip] SimCXL experiments are disabled by sentinel: {disable_reason}")
        return 0

    output_base = (repo_root / args.output_base).resolve()
    batch_name = args.batch_name or f"rpc_matrix_kvm_timing_ckpt_{now_tag()}"
    batch_dir = output_base / batch_name
    batch_dir.mkdir(parents=True, exist_ok=True)
    if args.start_index < 1:
        print("[fatal] --start-index must be >= 1")
        return 2
    if args.end_index < 0:
        print("[fatal] --end-index must be >= 0")
        return 2
    if args.end_index != 0 and args.end_index < args.start_index:
        print("[fatal] --end-index must be >= --start-index when set")
        return 2
    if args.copy_engine_channels < 0:
        print("[fatal] --copy-engine-channels must be >= 0")
        return 2
    if args.response_dma_threshold < 1:
        print("[fatal] --response-dma-threshold must be >= 1")
        return 2
    if args.prefetch_mode not in {"full", "no-request", "none"}:
        print("[fatal] --prefetch-mode must be one of: full, no-request, none")
        return 2
    if args.request_sparsity_slow_client_send_pause_iters < 0:
        print("[fatal] --request-sparsity-slow-client-send-pause-iters must be >= 0")
        return 2
    if args.checkpoint_handoff_deadline_sim_seconds < 0:
        print("[fatal] --checkpoint-handoff-deadline-sim-seconds must be >= 0")
        return 2

    gem5_bin = repo_root / "build/X86/gem5.opt"
    test_cfg = repo_root / "configs/example/gem5_library/x86-cxl-rpc-test.py"
    save_ckpt_cfg = (
        repo_root / "configs/example/gem5_library/x86-cxl-rpc-save-checkpoint.py"
    )
    inject_script = repo_root / "tests/test-progs/cxl-rpc/scripts/setup_disk_image.sh"
    summary_script = (
        repo_root / "tests/test-progs/cxl-rpc/scripts/summarize_rpc_matrix_results.py"
    )
    disk_img = (
        Path(args.disk).resolve()
        if args.disk else
        (repo_root / "files" / "parsec.img").resolve()
    )
    external_checkpoint_dir: Optional[Path] = None
    if args.checkpoint_dir:
        external_checkpoint_dir = resolve_checkpoint_dir(
            Path(args.checkpoint_dir).resolve()
        )
        if external_checkpoint_dir is None:
            print(
                f"[fatal] --checkpoint-dir does not resolve to a checkpoint: "
                f"{args.checkpoint_dir}"
            )
            return 2

    experiments = build_matrix(args.requests,
                               args.response_dma_threshold,
                               args.prefetch_mode,
                               args.request_sparsity_slow_client_send_pause_iters)
    if args.only_exp_id:
        experiments = [exp for exp in experiments if exp.exp_id == args.only_exp_id]
        if not experiments:
            if args.only_exp_id in OBSOLETE_REQUEST_SPARSITY_EXP_IDS:
                print(
                    "[skip] obsolete sparse exp_id from old incorrect plan: "
                    f"{args.only_exp_id}"
                )
                return 0
            print(f"[fatal] no experiment matched --only-exp-id={args.only_exp_id}")
            return 2
    max_clients = max(exp.key.clients for exp in experiments)
    max_required_cpus = max_clients + 1  # server core + one core per client

    plan_csv = batch_dir / "plan.csv"
    with plan_csv.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "exp_id",
                "source",
                "workload_kind",
                "request_size",
                "response_size",
                "request_min_size",
                "request_max_size",
                "response_min_size",
                "response_max_size",
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
                "app_profile",
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
            ],
        )
        writer.writeheader()
        for exp in experiments:
            writer.writerow(
                {
                    "exp_id": exp.exp_id,
                    "source": exp.source,
                    "workload_kind": exp.key.workload_kind,
                    "request_size": exp.key.request_size,
                    "response_size": exp.key.response_size,
                    "request_min_size": exp.key.request_min_size,
                    "request_max_size": exp.key.request_max_size,
                    "response_min_size": exp.key.response_min_size,
                    "response_max_size": exp.key.response_max_size,
                    "clients": exp.key.clients,
                    "requests_per_client": exp.key.requests_per_client,
                    "mq_entries": exp.key.mq_entries,
                    "head_sync_threshold": exp.key.head_sync_threshold,
                    "slow_client_count": exp.key.slow_client_count,
                    "slow_count_per_client": exp.key.slow_count_per_client,
                    "slow_client_send_pause_iters": (
                        exp.key.slow_client_send_pause_iters
                    ),
                    "cxl_extra_latency_ns": exp.key.cxl_extra_latency_ns,
                    "response_lane_count": exp.key.response_lane_count,
                    "clients_per_dma_lane": exp.key.clients_per_dma_lane,
                    "response_dma_threshold": exp.key.response_dma_threshold,
                    "prefetch_mode": exp.key.prefetch_mode,
                    "message_profile": (
                        "" if is_application_experiment(exp.key)
                        else exp.key.message_profile
                    ),
                    "app_profile": exp.key.app_profile,
                    "record_count": exp.key.record_count if exp.key.record_count > 0 else "",
                    "key_size": exp.key.key_size if exp.key.key_size > 0 else "",
                    "value_size": exp.key.value_size if exp.key.value_size > 0 else "",
                    "size_mode": exp.key.size_mode,
                    "key_dist": exp.key.key_dist,
                    "read_ratio": (
                        f"{exp.key.read_ratio:.6f}"
                        if is_application_experiment(exp.key) else ""
                    ),
                    "update_ratio": (
                        f"{exp.key.update_ratio:.6f}"
                        if is_application_experiment(exp.key) else ""
                    ),
                    "rmw_ratio": (
                        f"{exp.key.rmw_ratio:.6f}"
                        if is_application_experiment(exp.key) else ""
                    ),
                    "insert_ratio": (
                        f"{exp.key.insert_ratio:.6f}"
                        if is_application_experiment(exp.key) else ""
                    ),
                    "zipf_theta": (
                        format_zipf_field(exp.key.key_dist, exp.key.zipf_theta)
                        if is_application_experiment(exp.key) else ""
                    ),
                    "dataset_seed": (
                        str(exp.key.dataset_seed)
                        if is_application_experiment(exp.key) else ""
                    ),
                    "workload_seed": (
                        str(exp.key.workload_seed)
                        if is_application_experiment(exp.key) else ""
                    ),
                }
            )

    print(f"[matrix] total unique experiments: {len(experiments)}")
    print(f"[matrix] max required cores (server+clients): {max_required_cpus}")
    print(f"[matrix] start index: {args.start_index}")
    print(
        f"[matrix] end index: "
        f"{args.end_index if args.end_index != 0 else len(experiments)}"
    )
    print(f"[matrix] batch dir: {batch_dir}")

    experiments_csv = batch_dir / "experiments.csv"
    ticks_csv = batch_dir / "results_ticks.csv"
    server_ticks_csv = batch_dir / "results_server_ticks.csv"
    done_ids = set()
    if not args.force_rerun:
        done_ids = read_success_exp_ids(experiments_csv)

    exp_fields = [
        "exp_id",
        "source",
        "workload_kind",
        "request_size",
        "response_size",
        "request_min_size",
        "request_max_size",
        "response_min_size",
        "response_max_size",
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
        "app_profile",
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
        "checkpoint_dir",
        "num_cpus",
        "output_dir",
        "start_time",
        "end_time",
        "elapsed_sec",
        "gem5_rc",
        "test_cmd_exit",
        "tick_rows",
        "expected_rows",
        "server_tick_rows",
        "expected_server_rows",
        "status",
    ]
    tick_fields = [
        "exp_id",
        "workload_kind",
        "request_size",
        "response_size",
        "request_min_size",
        "request_max_size",
        "response_min_size",
        "response_max_size",
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
        "app_profile",
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
        "node_id",
        "req_index",
        "start_tick",
        "end_tick",
        "output_dir",
    ]
    server_tick_fields = [
        "exp_id",
        "workload_kind",
        "request_size",
        "response_size",
        "request_min_size",
        "request_max_size",
        "response_min_size",
        "response_max_size",
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
        "app_profile",
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
        "server_req_index",
        "poll_tick",
        "execute_tick",
        "response_tick",
        "output_dir",
    ]
    started_experiments = 0
    running_as_root = hasattr(os, "geteuid") and os.geteuid() == 0
    boot_requires_kvm = args.boot_cpu_type == "KVM"
    direct_kvm_access = os.access("/dev/kvm", os.R_OK | os.W_OK)
    sudo_n_available = sudo_nopass_available()
    kvm_cmd_prefix: List[str] = []
    checkpoint_cache: Dict[Tuple[str, int, int, int, int], Path] = {}
    checkpoint_failure_cache: Dict[Tuple[str, int, int, int, int], Tuple[str, int]] = {}
    kvm_signal_prefix: Optional[List[str]] = None
    lock_fp = None

    if not boot_requires_kvm:
        print(f"[matrix] boot CPU type is {args.boot_cpu_type}; /dev/kvm not required")
    elif direct_kvm_access:
        print("[matrix] /dev/kvm is directly accessible by current user")
    else:
        print("[matrix] /dev/kvm is not directly accessible by current user")
        if running_as_root:
            print("[matrix] current user is root; continuing without `sudo -n`")
        else:
            if not sudo_n_available:
                print("[fatal] /dev/kvm requires elevated access, but `sudo -n` is unavailable")
                return 2
            kvm_cmd_prefix = ["sudo", "-n"]
            kvm_signal_prefix = kvm_cmd_prefix
            print("[matrix] checkpoint builds will run under `sudo -n`")

    if not args.allow_concurrent_runs:
        lock_path = output_base / ".rpc_matrix.lock"
        lock_fp = acquire_matrix_lock(lock_path, batch_name)
        if lock_fp is None:
            return 2
        print(f"[matrix] acquired wrapper lock: {lock_path}")
    else:
        print("[matrix] concurrent wrapper runs allowed; skipping exclusive lock")

    try:
        if not args.skip_inject:
            inject_log = batch_dir / "inject.log"
            inject_cmd = ["bash", str(inject_script), str(disk_img)]
            if not args.dry_run:
                try:
                    rc = run_and_tee(inject_cmd, inject_log)
                except RuntimeError as exc:
                    print(f"[fatal] {exc}")
                    return 2
                if rc != 0:
                    print(f"[fatal] inject failed rc={rc}")
                    return rc
            else:
                print("[dry-run] skip inject execution")

        for idx, exp in enumerate(experiments, start=1):
            key = exp.key
            if idx < args.start_index:
                print(
                    f"[skip {idx}/{len(experiments)}] "
                    f"before --start-index={args.start_index}: {exp.exp_id}"
                )
                continue
            if args.end_index != 0 and idx > args.end_index:
                print(
                    f"[stop] reached --end-index={args.end_index}; "
                    "ending batch run"
                )
                break
            if exp.exp_id in done_ids:
                print(f"[skip {idx}/{len(experiments)}] already done: {exp.exp_id}")
                continue

            if started_experiments > 0 and args.inter_experiment_sleep_sec > 0:
                if args.dry_run:
                    print(
                        f"[dry-run] would sleep {args.inter_experiment_sleep_sec}s "
                        f"before starting next experiment: {exp.exp_id}"
                    )
                else:
                    print(
                        f"[matrix] sleeping {args.inter_experiment_sleep_sec}s before "
                        f"starting next experiment: {exp.exp_id}"
                    )
                    time.sleep(args.inter_experiment_sleep_sec)
            started_experiments += 1

            run_outdir = batch_dir / f"run_{idx:02d}_{exp.exp_id}"
            run_outdir.mkdir(parents=True, exist_ok=True)
            run_log = run_outdir / "gem5_run.log"
            required_cpus = key.clients + 1
            expected_requests = expected_total_requests(key)
            checkpoint_key = (
                args.boot_cpu_type,
                key.clients,
                key.response_lane_count,
                key.mq_entries,
                key.cxl_extra_latency_ns,
            )
            checkpoint_label = (
                f"boot_{args.boot_cpu_type.lower()}"
                f"_"
                f"clients_{key.clients}"
                f"_lanes_{key.response_lane_count}"
                f"_mq_{key.mq_entries}"
                f"_cxlp_{key.cxl_extra_latency_ns}ns"
            )
            if external_checkpoint_dir is not None:
                checkpoint_dir = external_checkpoint_dir
                checkpoint_failure = None
                ckpt_outdir = external_checkpoint_dir
            else:
                checkpoint_dir = checkpoint_cache.get(checkpoint_key)
                checkpoint_failure = checkpoint_failure_cache.get(checkpoint_key)
                ckpt_outdir = (
                    batch_dir / "checkpoints" / checkpoint_label /
                    "cxl_rpc_checkpoint"
                )

            if external_checkpoint_dir is None and checkpoint_dir is None:
                if checkpoint_failure is not None:
                    failed_status, failed_rc = checkpoint_failure
                    cached_status = f"{failed_status}_cached"
                    append_row(
                        experiments_csv,
                        {
                            "exp_id": exp.exp_id,
                            "source": exp.source,
                            **experiment_metadata_row(key),
                            "checkpoint_dir": str(ckpt_outdir),
                            "num_cpus": required_cpus,
                            "output_dir": str(run_outdir),
                            "start_time": "",
                            "end_time": "",
                            "elapsed_sec": "0.000",
                            "gem5_rc": failed_rc,
                            "test_cmd_exit": "",
                            "tick_rows": 0,
                            "expected_rows": expected_requests,
                            "server_tick_rows": 0,
                            "expected_server_rows": expected_requests,
                            "status": cached_status,
                        },
                        exp_fields,
                    )
                    print(
                        f"[done] {exp.exp_id} status={cached_status} "
                        f"gem5_rc={failed_rc} test_rc=None ticks=0/"
                        f"{expected_requests} elapsed=0.0s"
                    )
                    continue

                resolved = None if args.dry_run else resolve_checkpoint_dir(ckpt_outdir)
                if resolved is not None:
                    checkpoint_dir = resolved
                    checkpoint_cache[checkpoint_key] = checkpoint_dir
                    print(
                        f"[matrix] reuse checkpoint for {checkpoint_label}: "
                        f"{checkpoint_dir}"
                    )
                else:
                    ckpt_cmd = kvm_cmd_prefix + [
                        str(gem5_bin),
                        "-d",
                        str(ckpt_outdir),
                        str(save_ckpt_cfg),
                        "--disk",
                        str(disk_img),
                        "--boot_cpu_type",
                        args.boot_cpu_type,
                        "--num_cpus",
                        str(required_cpus),
                        "--rpc_client_count",
                        str(key.clients),
                        "--rpc_response_lane_count",
                        str(key.response_lane_count),
                        "--rpc_metadata_entries",
                        str(key.mq_entries),
                        "--cxl_extra_latency_ns",
                        str(key.cxl_extra_latency_ns),
                    ]
                    if args.copy_engine_channels > 0:
                        ckpt_cmd.extend(
                            [
                                "--copy_engine_channels",
                                str(args.copy_engine_channels),
                            ]
                        )
                    if args.checkpoint_handoff_deadline_sim_seconds > 0:
                        ckpt_cmd.extend(
                            [
                                "--handoff_deadline_sim_seconds",
                                str(args.checkpoint_handoff_deadline_sim_seconds),
                            ]
                        )
                    if args.dry_run:
                        checkpoint_dir = ckpt_outdir
                        print(
                            "[dry-run] would build checkpoint:",
                            " ".join(shlex.quote(c) for c in ckpt_cmd),
                        )
                    else:
                        ckpt_outdir.mkdir(parents=True, exist_ok=True)
                        ckpt_log = (
                            batch_dir / "checkpoints" / checkpoint_label /
                            "checkpoint_build.log"
                        )
                        try:
                            ckpt_rc = run_and_tee(
                                ckpt_cmd,
                                ckpt_log,
                                sudo_signal_prefix=kvm_signal_prefix,
                            )
                        except RuntimeError as exc:
                            print(f"[fatal] {exc}")
                            return 2
                        if ckpt_rc != 0:
                            checkpoint_failure_cache[checkpoint_key] = (
                                "checkpoint_failed",
                                ckpt_rc,
                            )
                            append_row(
                                experiments_csv,
                                {
                                    "exp_id": exp.exp_id,
                                    "source": exp.source,
                                    **experiment_metadata_row(key),
                                    "checkpoint_dir": str(ckpt_outdir),
                                    "num_cpus": required_cpus,
                                    "output_dir": str(run_outdir),
                                    "start_time": "",
                                    "end_time": "",
                                    "elapsed_sec": "0.000",
                                    "gem5_rc": ckpt_rc,
                                    "test_cmd_exit": "",
                                    "tick_rows": 0,
                                    "expected_rows": expected_requests,
                                    "server_tick_rows": 0,
                                    "expected_server_rows": expected_requests,
                                    "status": "checkpoint_failed",
                                },
                                exp_fields,
                            )
                            print(
                                f"[done] {exp.exp_id} status=checkpoint_failed "
                                f"gem5_rc={ckpt_rc} test_rc=None ticks=0/"
                                f"{expected_requests} elapsed=0.0s"
                            )
                            continue
                        resolved = resolve_checkpoint_dir(ckpt_outdir)
                        if resolved is None:
                            checkpoint_failure_cache[checkpoint_key] = (
                                "checkpoint_missing",
                                2,
                            )
                            append_row(
                                experiments_csv,
                                {
                                    "exp_id": exp.exp_id,
                                    "source": exp.source,
                                    **experiment_metadata_row(key),
                                    "checkpoint_dir": str(ckpt_outdir),
                                    "num_cpus": required_cpus,
                                    "output_dir": str(run_outdir),
                                    "start_time": "",
                                    "end_time": "",
                                    "elapsed_sec": "0.000",
                                    "gem5_rc": 2,
                                    "test_cmd_exit": "",
                                    "tick_rows": 0,
                                    "expected_rows": expected_requests,
                                    "server_tick_rows": 0,
                                    "expected_server_rows": expected_requests,
                                    "status": "checkpoint_missing",
                                },
                                exp_fields,
                            )
                            print(
                                f"[done] {exp.exp_id} status=checkpoint_missing "
                                f"gem5_rc=2 test_rc=None ticks=0/"
                                f"{expected_requests} elapsed=0.0s"
                            )
                            continue
                        checkpoint_dir = resolved

                    checkpoint_cache[checkpoint_key] = checkpoint_dir

            if is_application_experiment(key):
                server_args_parts = [
                    "--silent",
                    *app_profile_arg_list(key),
                    f"--mq-entries {key.mq_entries}",
                    f"--head-sync-threshold {key.head_sync_threshold}",
                    f"--response-dma-threshold {key.response_dma_threshold}",
                    f"--clients-per-dma-lane {key.clients_per_dma_lane}",
                    f"--prefetch-mode {key.prefetch_mode}",
                ]
                server_args = " ".join(server_args_parts)

                client_args_parts = [
                    f"--requests {key.requests_per_client}",
                    "--silent",
                    *app_profile_arg_list(key),
                    *app_workload_arg_list(key),
                ]
                client_args = " ".join(client_args_parts)
                test_cmd = (
                    f"CXL_RPC_CLIENT_COUNT={key.clients} "
                    f"CXL_RPC_CLIENT_TIMEOUT_SEC=0 "
                    f"CXL_RPC_SERVER_READY_TIMEOUT_SEC=0 "
                    f"CXL_RPC_PIN_CORES=1 "
                    f"CXL_RPC_SERVER_CORE=0 "
                    f"CXL_RPC_CLIENT_CORE_BASE=1 "
                    f"CXL_RPC_SERVER_ARGS={shlex.quote(server_args)} "
                    f"bash /home/test_code/run_rpc_server_clients.sh "
                    f"/home/test_code/rpc_mica_server /home/test_code/rpc_mica_client "
                    f"{client_args}"
                )
                workload_desc = (
                    f"kind=application, profile={key.app_profile}, "
                    f"rec={key.record_count}, key={key.key_size}, "
                    f"value={key.value_size}, rr={format_float_token(key.read_ratio)}, "
                    f"ur={format_float_token(key.update_ratio)}, "
                    f"rmw={format_float_token(key.rmw_ratio)}, "
                    f"ins={format_float_token(key.insert_ratio)}, "
                    f"dist={key.key_dist}"
                )
            else:
                server_args_parts = [
                    "--silent",
                    f"--request-size {key.request_size}",
                    f"--response-size {key.response_size}",
                    f"--mq-entries {key.mq_entries}",
                    f"--head-sync-threshold {key.head_sync_threshold}",
                    f"--response-dma-threshold {key.response_dma_threshold}",
                    f"--clients-per-dma-lane {key.clients_per_dma_lane}",
                    f"--prefetch-mode {key.prefetch_mode}",
                    f"--message-profile {key.message_profile}",
                ]
                if key.request_min_size > 0:
                    server_args_parts.append(f"--req-min-bytes {key.request_min_size}")
                if key.request_max_size > 0:
                    server_args_parts.append(f"--req-max-bytes {key.request_max_size}")
                if key.response_min_size > 0:
                    server_args_parts.append(f"--resp-min-bytes {key.response_min_size}")
                if key.response_max_size > 0:
                    server_args_parts.append(f"--resp-max-bytes {key.response_max_size}")
                server_args = " ".join(server_args_parts)
                test_cmd = (
                    f"CXL_RPC_CLIENT_COUNT={key.clients} "
                    f"CXL_RPC_CLIENT_TIMEOUT_SEC=0 "
                    f"CXL_RPC_SERVER_READY_TIMEOUT_SEC=0 "
                    f"CXL_RPC_PIN_CORES=1 "
                    f"CXL_RPC_SERVER_CORE=0 "
                    f"CXL_RPC_CLIENT_CORE_BASE=1 "
                    f"CXL_RPC_SERVER_ARGS={shlex.quote(server_args)} "
                    f"bash /home/test_code/run_rpc_server_clients.sh "
                    f"/home/test_code/rpc_server_example /home/test_code/rpc_client_example "
                    f"--requests {key.requests_per_client} "
                    f"--request-size {key.request_size} "
                    f"--response-size {key.response_size} "
                    f"--message-profile {key.message_profile} "
                    f"--silent"
                )
                if key.request_min_size > 0:
                    test_cmd += f" --req-min-bytes {key.request_min_size}"
                if key.request_max_size > 0:
                    test_cmd += f" --req-max-bytes {key.request_max_size}"
                if key.response_min_size > 0:
                    test_cmd += f" --resp-min-bytes {key.response_min_size}"
                if key.response_max_size > 0:
                    test_cmd += f" --resp-max-bytes {key.response_max_size}"
                if key.slow_client_count > 0:
                    test_cmd += (
                        f" --slow-client-count {key.slow_client_count} "
                        f"--slow-count-per-client {key.slow_count_per_client} "
                        f"--slow-client-send-pause-iters "
                        f"{key.slow_client_send_pause_iters}"
                    )
                workload_desc = (
                    f"kind=bare_rpc, req={key.request_size}"
                    f"[{key.request_min_size},{key.request_max_size}], "
                    f"resp={key.response_size}"
                    f"[{key.response_min_size},{key.response_max_size}], "
                    f"profile={key.message_profile}"
                )

            gem5_cmd = kvm_cmd_prefix + [
                str(gem5_bin),
                "-d",
                str(run_outdir),
                str(test_cfg),
                "--disk",
                str(disk_img),
                "--boot_cpu_type",
                args.boot_cpu_type,
                "--cpu_type",
                "TIMING",
                "--num_cpus",
                str(required_cpus),
                "--rpc_client_count",
                str(key.clients),
                "--rpc_response_lane_count",
                str(key.response_lane_count),
                "--rpc_metadata_entries",
                str(key.mq_entries),
                "--cxl_extra_latency_ns",
                str(key.cxl_extra_latency_ns),
                "--checkpoint",
                str(checkpoint_dir),
                "--test_cmd",
                test_cmd,
            ]
            if args.copy_engine_channels > 0:
                gem5_cmd.extend(
                    [
                        "--copy_engine_channels",
                        str(args.copy_engine_channels),
                    ]
                )

            start_ts = dt.datetime.now().isoformat(timespec="seconds")
            start_time = time.time()
            print(
                f"[run {idx}/{len(experiments)}] {exp.exp_id} "
                f"({workload_desc}, "
                f"clients={key.clients}, reqs/client={key.requests_per_client}, "
                f"mq={key.mq_entries}, hs={key.head_sync_threshold}, "
                f"slow={key.slow_client_count}, "
                f"slow_reqs={key.slow_count_per_client}, "
                f"slow_pause={key.slow_client_send_pause_iters}, "
                f"cxl+={key.cxl_extra_latency_ns}ns, lanes={key.response_lane_count}, "
                f"cpl={key.clients_per_dma_lane}, dmath={key.response_dma_threshold}, "
                f"pf={key.prefetch_mode}, cpus={required_cpus})"
            )

            if args.dry_run:
                gem5_rc = 0
                test_cmd_rc = 0
                client_rows: List[Dict[str, int]] = []
                server_rows: List[Dict[str, int]] = []
            else:
                try:
                    gem5_rc = run_and_tee(
                        gem5_cmd,
                        run_log,
                        sudo_signal_prefix=kvm_signal_prefix,
                    )
                except RuntimeError as exc:
                    print(f"[fatal] {exc}")
                    return 2
                test_cmd_rc, client_rows, server_rows = parse_run_results(
                    run_outdir
                )

            end_time = time.time()
            end_ts = dt.datetime.now().isoformat(timespec="seconds")
            elapsed = end_time - start_time

            expected_rows = expected_requests
            expected_server_rows = expected_requests
            if args.dry_run:
                status = "dry_run"
            else:
                status = "ok"
                if gem5_rc != 0:
                    status = "gem5_failed"
                elif test_cmd_rc is None:
                    status = "test_rc_missing"
                elif test_cmd_rc != 0:
                    status = "test_failed"
                elif len(client_rows) != expected_rows:
                    status = "incomplete_ticks"
                elif len(server_rows) != expected_server_rows:
                    status = "incomplete_server_ticks"

            append_row(
                experiments_csv,
                {
                    "exp_id": exp.exp_id,
                    "source": exp.source,
                    **experiment_metadata_row(key),
                    "checkpoint_dir": str(checkpoint_dir),
                    "num_cpus": required_cpus,
                    "output_dir": str(run_outdir),
                    "start_time": start_ts,
                    "end_time": end_ts,
                    "elapsed_sec": f"{elapsed:.3f}",
                    "gem5_rc": gem5_rc,
                    "test_cmd_exit": "" if test_cmd_rc is None else test_cmd_rc,
                    "tick_rows": len(client_rows),
                    "expected_rows": expected_rows,
                    "server_tick_rows": len(server_rows),
                    "expected_server_rows": expected_server_rows,
                    "status": status,
                },
                exp_fields,
            )

            for row in client_rows:
                append_row(
                    ticks_csv,
                    {
                        "exp_id": exp.exp_id,
                        **experiment_metadata_row(key),
                        "node_id": row["node_id"],
                        "req_index": row["req_index"],
                        "start_tick": row["start_tick"],
                        "end_tick": row["end_tick"],
                        "output_dir": str(run_outdir),
                    },
                    tick_fields,
                )

            for row in server_rows:
                append_row(
                    server_ticks_csv,
                    {
                        "exp_id": exp.exp_id,
                        **experiment_metadata_row(key),
                        "server_req_index": row["server_req_index"],
                        "poll_tick": row["poll_tick"],
                        "execute_tick": row["execute_tick"],
                        "response_tick": row["response_tick"],
                        "output_dir": str(run_outdir),
                    },
                    server_tick_fields,
                )

            print(
                f"[done] {exp.exp_id} status={status} "
                f"gem5_rc={gem5_rc} test_rc={test_cmd_rc} "
                f"ticks={len(client_rows)}/{expected_rows} "
                f"server_ticks={len(server_rows)}/{expected_server_rows} "
                f"elapsed={elapsed:.1f}s"
            )
    finally:
        release_matrix_lock(lock_fp)

    if not args.dry_run:
        summary_cmd = [
            sys.executable,
            str(summary_script),
            "--batch-dir",
            str(batch_dir),
        ]
        print("+", " ".join(shlex.quote(c) for c in summary_cmd), flush=True)
        summary_proc = subprocess.run(summary_cmd, check=False)
        if summary_proc.returncode != 0:
            print(f"[fatal] summary generation failed rc={summary_proc.returncode}")
            return summary_proc.returncode

    print("\n[matrix] finished")
    print(f"[matrix] plan: {plan_csv}")
    print(f"[matrix] experiments: {experiments_csv}")
    print(f"[matrix] ticks: {ticks_csv}")
    print(f"[matrix] server ticks: {server_ticks_csv}")
    if not args.dry_run:
        print(f"[matrix] client summary: {batch_dir / 'summary_client_latency.csv'}")
        print(f"[matrix] server summary: {batch_dir / 'summary_server_breakdown.csv'}")
        print(f"[matrix] throughput summary: {batch_dir / 'summary_throughput.csv'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
