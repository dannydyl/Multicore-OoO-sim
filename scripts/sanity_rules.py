"""Per-run and cross-run validation rules.

Per-run rules check internal invariants the sim should always satisfy
(retired ≤ fetched, IPC bounds, deadlock, etc.). Cross-run rules check
expected relationships across configs (protocol invariance on private
addresses, prefetcher reduces stream miss rate, etc.).

Severities:
  error -> sim is producing nonsense; harness flags loudly
  warn  -> unexpected but possible (e.g. ROB regression on tiny trace);
           inspect manually
  info  -> known-expected today, surfaced for transparency (e.g. shared-
           address protocol-invariance pending the ABORT/INTERVENE knob)
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass
class CoreRow:
    core: int
    cycles: int
    instructions_retired: int
    instructions_fetched: int
    ipc: float
    cpi: float
    branch_mispredictions: int
    mpki: float
    l1_accesses: int
    l1_hits: int
    l1_misses: int
    l1_miss_rate: float
    l1_aat: float
    l2_accesses: int
    l2_hits: int
    l2_misses: int
    l2_miss_rate: float
    l2_aat: float


@dataclass
class SysStats:
    cache_accesses: int = 0
    cache_misses: int = 0
    silent_upgrades: int = 0
    c2c_transfers: int = 0
    memory_reads: int = 0
    memory_writes: int = 0


@dataclass
class RunRecord:
    run_id: str
    trace_id: str
    protocol: str
    cores: int
    overrides: dict[str, Any]
    cores_data: list[CoreRow]
    sys: SysStats
    deadlocked: bool
    exit_code: int | None
    wallclock_s: float
    status: str
    fetch_width: int


@dataclass
class Violation:
    run_id: str
    trace_id: str
    rule: str
    detail: str
    severity: str   # "error" | "warn" | "info"


def _avg_ipc(rec: RunRecord) -> float:
    if not rec.cores_data:
        return 0.0
    return sum(c.ipc for c in rec.cores_data) / len(rec.cores_data)


def _avg_l1_miss_rate(rec: RunRecord) -> float:
    if not rec.cores_data:
        return 0.0
    return sum(c.l1_miss_rate for c in rec.cores_data) / len(rec.cores_data)


def per_run_violations(rec: RunRecord) -> list[Violation]:
    vs: list[Violation] = []

    if rec.status != "ok":
        vs.append(Violation(rec.run_id, rec.trace_id, "exit_status",
                            f"status={rec.status} exit={rec.exit_code}", "error"))
    if rec.deadlocked:
        vs.append(Violation(rec.run_id, rec.trace_id, "deadlock",
                            "report.rpt 'Status' = 'Simulation terminated'", "error"))

    for c in rec.cores_data:
        if c.instructions_retired > c.instructions_fetched:
            vs.append(Violation(rec.run_id, rec.trace_id, "retired_gt_fetched",
                                f"core {c.core}: {c.instructions_retired} > {c.instructions_fetched}",
                                "error"))
        if not (0.0 <= c.ipc <= rec.fetch_width + 1e-6):
            vs.append(Violation(rec.run_id, rec.trace_id, "ipc_out_of_range",
                                f"core {c.core}: ipc={c.ipc:.3f} fetch_width={rec.fetch_width}",
                                "error"))
        if c.l1_misses > c.l1_accesses:
            vs.append(Violation(rec.run_id, rec.trace_id, "l1_misses_gt_accesses",
                                f"core {c.core}: l1_misses={c.l1_misses} l1_accesses={c.l1_accesses}",
                                "error"))
        if c.l1_misses > 0:
            ratio = abs(c.l2_accesses - c.l1_misses) / c.l1_misses
            if ratio > 0.10:
                vs.append(Violation(rec.run_id, rec.trace_id, "l2_l1_miss_mismatch",
                                    f"core {c.core}: l2_acc={c.l2_accesses} l1_miss={c.l1_misses} ratio={ratio:.2%}",
                                    "warn"))
    return vs


def cross_run_violations(records: list[RunRecord]) -> list[Violation]:
    vs: list[Violation] = []

    by_trace: dict[str, list[RunRecord]] = {}
    for r in records:
        by_trace.setdefault(r.trace_id, []).append(r)

    for trace_id, runs in by_trace.items():
        baselines = [r for r in runs if r.run_id == "baseline" and r.status == "ok"]
        if not baselines:
            continue
        base = baselines[0]
        base_ipc = _avg_ipc(base)
        base_miss = _avg_l1_miss_rate(base)

        is_private = trace_id.startswith("synth/sequential_") or trace_id.startswith("synth/random_")
        is_shared = trace_id.startswith("synth/loop_") or trace_id.startswith("synth/stream_")

        proto_runs = [r for r in runs if r.status == "ok" and (r.run_id == "baseline" or r.run_id.startswith("proto_"))]
        proto_ipcs = {r.run_id: _avg_ipc(r) for r in proto_runs}
        if proto_ipcs:
            ipcs = list(proto_ipcs.values())
            spread = (max(ipcs) - min(ipcs)) / max(max(ipcs), 1e-9)
            if is_private and spread > 0.01:
                vs.append(Violation("(cross)", trace_id, "proto_invariance_private",
                                    f"protocol IPC spread {spread:.2%} > 1% on private-address trace; values={ {k: round(v,4) for k,v in proto_ipcs.items()} }",
                                    "warn"))
            if is_shared and spread < 0.001:
                vs.append(Violation("(cross)", trace_id, "proto_invariance_shared",
                                    f"protocols collapse to identical IPC on shared-address trace (ABORT/INTERVENE knob TODO); values={ {k: round(v,4) for k,v in proto_ipcs.items()} }",
                                    "info"))

        if trace_id.startswith("synth/stream_") and base_miss > 0:
            for r in runs:
                if r.run_id.startswith("pf_") and r.status == "ok":
                    delta = (base_miss - _avg_l1_miss_rate(r)) / max(base_miss, 1e-9)
                    if delta < 0.10:
                        vs.append(Violation(r.run_id, trace_id, "prefetcher_no_help_stream",
                                            f"L1 miss-rate reduction {delta:.2%} < 10% on stream trace; "
                                            f"baseline={base_miss:.3f} run={_avg_l1_miss_rate(r):.3f}",
                                            "warn"))

        for r in runs:
            if r.run_id == "rob_128" and r.status == "ok" and base_ipc > 0:
                delta = (_avg_ipc(r) - base_ipc) / base_ipc
                if delta < -0.05:
                    vs.append(Violation(r.run_id, trace_id, "rob128_regression",
                                        f"rob_128 IPC {_avg_ipc(r):.3f} regressed >5% from baseline {base_ipc:.3f}",
                                        "warn"))

    return vs
