#!/usr/bin/env python3
"""Summarize CXL trace timing from global ticks.

Primary mode (RPC-aware):
- Anchor on RPC-engine remap event:
    "Request remapped to metadata queue ..."
- Use the remapped metadata slot address as the target.
- Measure:
    remap(write) -> first read request on that slot
    first read request -> first mem response / rsp send / bridge response

Fallback mode (legacy heuristic):
- Find latest address with write request followed by read request.
- Report the same downstream read-response stages.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


WRITE_RE = re.compile(
    r"^(\d+): .*cxl_rsp_port: recvTimingReq: (WriteClean|WriteReq) addr (0x[0-9a-fA-F]+)"
)
READ_REQ_RE = re.compile(
    r"^(\d+): .*cxl_rsp_port: recvTimingReq: (ReadCleanReq|ReadReq|ReadExReq) addr (0x[0-9a-fA-F]+)"
)
MEM_RESP_RE = re.compile(
    r"^(\d+): .*mem_req_port: recvTimingResp: (ReadResp|ReadExResp) addr (0x[0-9a-fA-F]+)"
)
RSP_SEND_RE = re.compile(
    r"^(\d+): .*cxl_rsp_port: trySend response addr (0x[0-9a-fA-F]+),"
)
BRIDGE_RESP_RE = re.compile(
    r"^(\d+): .*bridge\.mem_side_port: recvTimingResp: (ReadResp|ReadExResp) addr (0x[0-9a-fA-F]+), when tick(\d+)"
)
BRIDGE_CPU_READ_REQ_RE = re.compile(
    r"^(\d+): .*bridge\.cpu_side_port: recvTimingReq: (ReadCleanReq|ReadReq|ReadExReq) addr (0x[0-9a-fA-F]+)"
)
RPC_REMAP_RE = re.compile(
    r"^(\d+): .*Request remapped to metadata queue: client=(\d+) slot=(\d+) "
    r"slot_addr=(0x[0-9a-fA-F]+).*?phase=(\d+)\s+req_id=(\d+)"
)


@dataclass
class Candidate:
    addr: str
    write_req_tick: int
    read_req_tick: int
    mem_resp_tick: int | None
    rsp_send_tick: int | None
    bridge_when_tick: int | None
    read_cmd: str


@dataclass
class RpcRemap:
    tick: int
    client_id: int
    slot: int
    slot_addr: str
    phase: int
    req_id: int


def first_ge(values: list[int], lower: int) -> int | None:
    for v in values:
        if v >= lower:
            return v
    return None


def first_ge_pair(values: list[tuple[int, str]], lower: int) -> tuple[int, str] | None:
    for tick, text in values:
        if tick >= lower:
            return tick, text
    return None


def ns_from_ticks(ticks: int) -> float:
    # gem5 tick is 1 ps in this setup.
    return ticks / 1000.0


def print_delta(label: str, t0: int, t1: int) -> None:
    dt = t1 - t0
    print(f"{label}_ticks={dt}")
    print(f"{label}_ns={ns_from_ticks(dt):.3f}")


def summarize_legacy(
    writes: dict[str, list[int]],
    reads: dict[str, list[tuple[int, str]]],
    mem_resps: dict[str, list[int]],
    rsp_sends: dict[str, list[int]],
    bridge_whens: dict[str, list[int]],
) -> int:
    candidates: list[Candidate] = []
    for addr, wlist in writes.items():
        rlist = reads.get(addr, [])
        if not rlist:
            continue
        r_ticks = [t for t, _ in rlist]
        for w in wlist:
            read_req_tick = first_ge(r_ticks, w)
            if read_req_tick is None:
                continue
            read_cmd = next(cmd for t, cmd in rlist if t == read_req_tick)
            cand = Candidate(
                addr=addr,
                write_req_tick=w,
                read_req_tick=read_req_tick,
                mem_resp_tick=first_ge(mem_resps.get(addr, []), read_req_tick),
                rsp_send_tick=first_ge(rsp_sends.get(addr, []), read_req_tick),
                bridge_when_tick=first_ge(bridge_whens.get(addr, []), read_req_tick),
                read_cmd=read_cmd,
            )
            candidates.append(cand)

    if not candidates:
        print("trace_summary_status=no_write_read_pair_found")
        return 0

    best = max(candidates, key=lambda c: c.write_req_tick)

    print("trace_summary_status=ok")
    print("trace_summary_mode=legacy_latest_write_read")
    print(f"trace_selected_addr={best.addr}")
    print(f"trace_selected_read_cmd={best.read_cmd}")
    print(f"trace_write_req_tick={best.write_req_tick}")
    print(f"trace_read_req_tick={best.read_req_tick}")
    print_delta("trace_write_to_read_req", best.write_req_tick, best.read_req_tick)

    if best.mem_resp_tick is not None:
        print(f"trace_mem_resp_tick={best.mem_resp_tick}")
        print_delta(
            "trace_read_req_to_mem_resp",
            best.read_req_tick,
            best.mem_resp_tick,
        )

    if best.rsp_send_tick is not None:
        print(f"trace_rsp_send_tick={best.rsp_send_tick}")
        print_delta(
            "trace_read_req_to_rsp_send",
            best.read_req_tick,
            best.rsp_send_tick,
        )

    if best.bridge_when_tick is not None:
        print(f"trace_bridge_resp_tick={best.bridge_when_tick}")
        print_delta(
            "trace_read_req_to_bridge_resp",
            best.read_req_tick,
            best.bridge_when_tick,
        )

    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument(
        "--req-id",
        type=int,
        default=None,
        help="Optional RPC request id filter for remap-anchor mode.",
    )
    parser.add_argument(
        "--client-id",
        type=int,
        default=None,
        help="Optional RPC client id filter for remap-anchor mode.",
    )
    parser.add_argument(
        "--slot-addr",
        type=str,
        default=None,
        help="Optional metadata slot address filter (hex).",
    )
    parser.add_argument(
        "--select",
        choices=("latest", "earliest"),
        default="latest",
        help="When multiple RPC remaps match filter, choose latest/earliest.",
    )
    args = parser.parse_args()

    if not args.trace.exists():
        print(f"trace file not found: {args.trace}", file=sys.stderr)
        return 1

    writes: dict[str, list[int]] = {}
    reads: dict[str, list[tuple[int, str]]] = {}
    bridge_cpu_reads: dict[str, list[tuple[int, str]]] = {}
    mem_resps: dict[str, list[int]] = {}
    rsp_sends: dict[str, list[int]] = {}
    bridge_whens: dict[str, list[int]] = {}
    remaps: list[RpcRemap] = []

    with args.trace.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RPC_REMAP_RE.match(line)
            if m:
                remaps.append(
                    RpcRemap(
                        tick=int(m.group(1)),
                        client_id=int(m.group(2)),
                        slot=int(m.group(3)),
                        slot_addr=m.group(4).lower(),
                        phase=int(m.group(5)),
                        req_id=int(m.group(6)),
                    )
                )
                continue

            m = WRITE_RE.match(line)
            if m:
                tick = int(m.group(1))
                addr = m.group(3).lower()
                writes.setdefault(addr, []).append(tick)
                continue

            m = READ_REQ_RE.match(line)
            if m:
                tick = int(m.group(1))
                cmd = m.group(2)
                addr = m.group(3).lower()
                reads.setdefault(addr, []).append((tick, cmd))
                continue

            m = BRIDGE_CPU_READ_REQ_RE.match(line)
            if m:
                tick = int(m.group(1))
                cmd = m.group(2)
                addr = m.group(3).lower()
                bridge_cpu_reads.setdefault(addr, []).append((tick, cmd))
                continue

            m = MEM_RESP_RE.match(line)
            if m:
                tick = int(m.group(1))
                addr = m.group(3).lower()
                mem_resps.setdefault(addr, []).append(tick)
                continue

            m = RSP_SEND_RE.match(line)
            if m:
                tick = int(m.group(1))
                addr = m.group(2).lower()
                rsp_sends.setdefault(addr, []).append(tick)
                continue

            m = BRIDGE_RESP_RE.match(line)
            if m:
                addr = m.group(3).lower()
                when_tick = int(m.group(4))
                bridge_whens.setdefault(addr, []).append(when_tick)

    # Prefer RPC remap-anchor mode when remap events exist.
    if remaps:
        slot_filter = args.slot_addr.lower() if args.slot_addr else None
        filtered = [
            r for r in remaps
            if (args.req_id is None or r.req_id == args.req_id)
            and (args.client_id is None or r.client_id == args.client_id)
            and (slot_filter is None or r.slot_addr == slot_filter)
        ]
        if filtered:
            target = (
                max(filtered, key=lambda r: r.tick)
                if args.select == "latest"
                else min(filtered, key=lambda r: r.tick)
            )
            addr = target.slot_addr

            bridge_cpu_read = first_ge_pair(
                bridge_cpu_reads.get(addr, []), target.tick
            )
            cxl_read = first_ge_pair(reads.get(addr, []), target.tick)

            print("trace_summary_status=ok")
            print("trace_summary_mode=rpc_remap_anchor")
            print(f"trace_target_client_id={target.client_id}")
            print(f"trace_target_req_id={target.req_id}")
            print(f"trace_target_slot={target.slot}")
            print(f"trace_target_phase={target.phase}")
            print(f"trace_target_slot_addr={addr}")
            print(f"trace_remap_tick={target.tick}")

            if bridge_cpu_read is not None:
                bridge_cpu_tick, bridge_cpu_cmd = bridge_cpu_read
                print(f"trace_bridge_cpu_read_cmd={bridge_cpu_cmd}")
                print(f"trace_bridge_cpu_read_req_tick={bridge_cpu_tick}")
                print_delta(
                    "trace_remap_to_bridge_cpu_read_req",
                    target.tick,
                    bridge_cpu_tick,
                )

            if cxl_read is None:
                print("trace_summary_warning=no_cxl_read_after_remap_for_slot")
                return 0

            read_req_tick, read_cmd = cxl_read
            print(f"trace_selected_read_cmd={read_cmd}")
            print(f"trace_read_req_tick={read_req_tick}")
            print_delta("trace_remap_to_read_req", target.tick, read_req_tick)

            mem_resp_tick = first_ge(mem_resps.get(addr, []), read_req_tick)
            if mem_resp_tick is not None:
                print(f"trace_mem_resp_tick={mem_resp_tick}")
                print_delta("trace_read_req_to_mem_resp", read_req_tick, mem_resp_tick)

            rsp_send_tick = first_ge(rsp_sends.get(addr, []), read_req_tick)
            if rsp_send_tick is not None:
                print(f"trace_rsp_send_tick={rsp_send_tick}")
                print_delta("trace_read_req_to_rsp_send", read_req_tick, rsp_send_tick)

            bridge_resp_tick = first_ge(bridge_whens.get(addr, []), read_req_tick)
            if bridge_resp_tick is not None:
                print(f"trace_bridge_resp_tick={bridge_resp_tick}")
                print_delta(
                    "trace_read_req_to_bridge_resp",
                    read_req_tick,
                    bridge_resp_tick,
                )
                print_delta(
                    "trace_remap_to_bridge_resp",
                    target.tick,
                    bridge_resp_tick,
                )
            return 0

        # Remap exists but user filter matched nothing; keep a clear status.
        print("trace_summary_status=no_rpc_remap_match_for_filter")
        print(f"trace_rpc_remap_total={len(remaps)}")
        return 0

    return summarize_legacy(writes, reads, mem_resps, rsp_sends, bridge_whens)


if __name__ == "__main__":
    raise SystemExit(main())
