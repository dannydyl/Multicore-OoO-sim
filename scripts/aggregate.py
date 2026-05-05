#!/usr/bin/env python3
"""Aggregate per-run reports for a sweep into a summary.csv + summary.md.

Walks report/_sweep/<sweep_id>/logs/*.meta.json (one per run), reads the
referenced report.csv (per-core metrics) and report.rpt (system block,
deadlock status), applies sanity rules, and writes:

  report/_sweep/<sweep_id>/summary.csv
  report/_sweep/<sweep_id>/summary.md

Re-runnable mid-sweep (skips runs whose .csv/.rpt aren't there yet).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from sweep_matrix import REPO_ROOT, BASELINE_PATH
from sanity_rules import (
    CoreRow, SysStats, RunRecord, Violation,
    per_run_violations, cross_run_violations,
)


_KV_RE = re.compile(r"^\s*([A-Za-z][^:]*?)\s{2,}:\s*(.+?)\s*$")
_KV_RE_LOOSE = re.compile(r"^\s*([A-Za-z][A-Za-z0-9 _\-/]*?)\s*:\s*(.+?)\s*$")


def parse_report_csv(p: Path) -> list[CoreRow]:
    rows: list[CoreRow] = []
    with p.open() as f:
        header_line = f.readline().strip()
        if not header_line:
            return rows
        header = header_line.split(",")
        for line in f:
            line = line.strip()
            if not line:
                continue
            vals = line.split(",")
            d = dict(zip(header, vals))
            rows.append(CoreRow(
                core=int(d["core"]),
                cycles=int(d["cycles"]),
                instructions_retired=int(d["instructions_retired"]),
                instructions_fetched=int(d["instructions_fetched"]),
                ipc=float(d["ipc"]),
                cpi=float(d["cpi"]),
                branch_mispredictions=int(d["branch_mispredictions"]),
                mpki=float(d["mpki"]),
                l1_accesses=int(d["l1_accesses"]),
                l1_hits=int(d["l1_hits"]),
                l1_misses=int(d["l1_misses"]),
                l1_miss_rate=float(d["l1_miss_rate"]),
                l1_aat=float(d["l1_aat"]),
                l2_accesses=int(d["l2_accesses"]),
                l2_hits=int(d["l2_hits"]),
                l2_misses=int(d["l2_misses"]),
                l2_miss_rate=float(d["l2_miss_rate"]),
                l2_aat=float(d["l2_aat"]),
            ))
    return rows


def parse_report_rpt(p: Path) -> tuple[SysStats, bool]:
    """Pull the system-wide block + deadlock flag from report.rpt."""
    target = {
        "Cache accesses":           "cache_accesses",
        "Cache misses":             "cache_misses",
        "Silent upgrades":          "silent_upgrades",
        "Cache-to-cache transfers": "c2c_transfers",
        "Memory reads":             "memory_reads",
        "Memory writes":            "memory_writes",
    }
    out: dict[str, int] = {}
    deadlocked = False
    with p.open() as f:
        for line in f:
            m = _KV_RE_LOOSE.match(line)
            if not m:
                continue
            k, v = m.group(1).strip(), m.group(2).strip()
            if k == "Status" and "terminated" in v.lower():
                deadlocked = True
            if k in target:
                try:
                    out[target[k]] = int(v)
                except ValueError:
                    pass
    return (SysStats(
        cache_accesses=out.get("cache_accesses", 0),
        cache_misses=out.get("cache_misses", 0),
        silent_upgrades=out.get("silent_upgrades", 0),
        c2c_transfers=out.get("c2c_transfers", 0),
        memory_reads=out.get("memory_reads", 0),
        memory_writes=out.get("memory_writes", 0),
    ), deadlocked)


def collect_runs(sweep_id: str, baseline_cfg: dict) -> list[RunRecord]:
    sweep_root = REPO_ROOT / "report" / "_sweep" / sweep_id
    log_dir = sweep_root / "logs"
    if not log_dir.exists():
        return []

    records: list[RunRecord] = []
    for meta_path in sorted(log_dir.glob("*.meta.json")):
        try:
            meta = json.loads(meta_path.read_text())
        except json.JSONDecodeError:
            continue

        rd = Path(meta["report_dir"])
        csv = rd / "report.csv"
        rpt = rd / "report.rpt"
        if not csv.exists():
            cores_data: list[CoreRow] = []
        else:
            try:
                cores_data = parse_report_csv(csv)
            except Exception:
                cores_data = []

        sys_stats, deadlocked = (SysStats(), False)
        if rpt.exists():
            try:
                sys_stats, deadlocked = parse_report_rpt(rpt)
            except Exception:
                pass

        fetch_width = baseline_cfg["core"]["fetch_width"]
        cfg_path = Path(meta.get("config_path", ""))
        if cfg_path.exists():
            try:
                run_cfg = json.loads(cfg_path.read_text())
                fetch_width = run_cfg["core"]["fetch_width"]
            except Exception:
                pass

        records.append(RunRecord(
            run_id=meta["run_id"],
            trace_id=meta["trace_id"],
            protocol=meta["protocol"],
            cores=int(meta["cores"]),
            overrides=meta.get("overrides", {}),
            cores_data=cores_data,
            sys=sys_stats,
            deadlocked=deadlocked,
            exit_code=meta.get("exit_code"),
            wallclock_s=float(meta.get("wallclock_s", 0.0)),
            status=meta.get("status", "unknown"),
            fetch_width=int(fetch_width),
        ))
    return records


SUMMARY_HEADER = (
    "sweep_id,run_id,trace_id,protocol,cores,axis,value,exit_code,status,wallclock_s,"
    "mean_ipc,total_cycles,total_instructions,total_branch_mispredictions,"
    "l1_miss_rate_avg,l2_miss_rate_avg,"
    "sys_cache_accesses,sys_cache_misses,sys_silent_upgrades,sys_c2c_transfers,"
    "sys_memory_reads,sys_memory_writes,deadlocked\n"
)


def _axis_value(run_id: str) -> tuple[str, str]:
    if run_id == "baseline":
        return ("baseline", "")
    if run_id.startswith("hp_"):
        return ("handpicked", run_id)
    parts = run_id.split("_", 1)
    return (parts[0], parts[1] if len(parts) == 2 else "")


def _agg(records_for_run: RunRecord) -> dict:
    n = max(len(records_for_run.cores_data), 1)
    if records_for_run.cores_data:
        mean_ipc = sum(c.ipc for c in records_for_run.cores_data) / n
        tot_cycles = max(c.cycles for c in records_for_run.cores_data)
        tot_insts = sum(c.instructions_retired for c in records_for_run.cores_data)
        tot_brmp = sum(c.branch_mispredictions for c in records_for_run.cores_data)
        l1mr = sum(c.l1_miss_rate for c in records_for_run.cores_data) / n
        l2mr = sum(c.l2_miss_rate for c in records_for_run.cores_data) / n
    else:
        mean_ipc = tot_cycles = tot_insts = tot_brmp = l1mr = l2mr = 0
    return dict(mean_ipc=mean_ipc, tot_cycles=tot_cycles, tot_insts=tot_insts,
                tot_brmp=tot_brmp, l1mr=l1mr, l2mr=l2mr)


def write_summary_csv(records: list[RunRecord], sweep_id: str, out_path: Path) -> None:
    with out_path.open("w") as f:
        f.write(SUMMARY_HEADER)
        for r in records:
            a = _agg(r)
            axis, value = _axis_value(r.run_id)
            f.write(
                f"{sweep_id},{r.run_id},{r.trace_id},{r.protocol},{r.cores},"
                f"{axis},{value},{r.exit_code},{r.status},{r.wallclock_s:.2f},"
                f"{a['mean_ipc']:.5f},{a['tot_cycles']},{a['tot_insts']},{a['tot_brmp']},"
                f"{a['l1mr']:.5f},{a['l2mr']:.5f},"
                f"{r.sys.cache_accesses},{r.sys.cache_misses},{r.sys.silent_upgrades},"
                f"{r.sys.c2c_transfers},{r.sys.memory_reads},{r.sys.memory_writes},"
                f"{int(r.deadlocked)}\n"
            )


def write_summary_md(records: list[RunRecord],
                     violations: list[Violation],
                     sweep_id: str,
                     out_path: Path) -> None:
    errs  = [v for v in violations if v.severity == "error"]
    warns = [v for v in violations if v.severity == "warn"]
    infos = [v for v in violations if v.severity == "info"]

    lines: list[str] = []
    lines.append(f"# Sweep `{sweep_id}` — Validation Summary")
    lines.append("")
    lines.append(f"- Runs: {len(records)}")
    lines.append(f"- Errors: {len(errs)}")
    lines.append(f"- Warnings: {len(warns)}")
    lines.append(f"- Info: {len(infos)}")
    lines.append("")

    lines.append("## Caveats")
    lines.append("")
    lines.append("- **Coherence ABORT/INTERVENE knob is TODO.** MSI/MESI/MOSI/MOESIF may collapse to identical numbers on shared-address traces (synth/loop, synth/stream). Surfaced as `INFO: proto_invariance_shared`, not a violation. Re-run after the knob lands.")
    lines.append("- **Internal cycle cap** in `full_mode.cpp` ≠ Python `--timeout`. Sim may exit 0 with `Status: Simulation terminated`; that is flagged as `deadlocked=True` (rule `deadlock`).")
    lines.append("- **Trace EOF != predictable wallclock.** A long trace may finish quickly if loads are rare. If long-tier results look starved, regenerate synth traces with higher load/store rates.")
    lines.append("")

    if errs:
        lines.append("## Errors")
        lines.append("")
        for v in errs:
            lines.append(f"- **{v.rule}** [`{v.run_id}` / `{v.trace_id}`]: {v.detail}")
        lines.append("")
    if warns:
        lines.append("## Warnings")
        lines.append("")
        for v in warns:
            lines.append(f"- **{v.rule}** [`{v.run_id}` / `{v.trace_id}`]: {v.detail}")
        lines.append("")
    if infos:
        lines.append("## Info (expected today, no action needed)")
        lines.append("")
        for v in infos:
            lines.append(f"- **{v.rule}** [`{v.run_id}` / `{v.trace_id}`]: {v.detail}")
        lines.append("")

    by_trace: dict[str, list[RunRecord]] = {}
    for r in records:
        by_trace.setdefault(r.trace_id, []).append(r)

    lines.append("## Per-trace results")
    lines.append("")
    for trace_id in sorted(by_trace):
        lines.append(f"### `{trace_id}`")
        lines.append("")
        lines.append("| run_id | protocol | cores | mean_ipc | l1_miss_rate | l2_miss_rate | wall_s | status |")
        lines.append("|---|---|---|---|---|---|---|---|")
        for r in sorted(by_trace[trace_id], key=lambda x: x.run_id):
            a = _agg(r)
            lines.append(
                f"| `{r.run_id}` | {r.protocol} | {r.cores} | "
                f"{a['mean_ipc']:.4f} | {a['l1mr']:.4f} | {a['l2mr']:.4f} | "
                f"{r.wallclock_s:.1f} | {r.status} |"
            )
        lines.append("")

    out_path.write_text("\n".join(lines) + "\n")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sweep-id", required=True)
    args = ap.parse_args(argv)

    baseline_cfg = json.loads(BASELINE_PATH.read_text())
    records = collect_runs(args.sweep_id, baseline_cfg)
    if not records:
        print(f"no runs found under report/_sweep/{args.sweep_id}/logs/",
              file=sys.stderr)
        return 1

    violations: list[Violation] = []
    for r in records:
        violations.extend(per_run_violations(r))
    violations.extend(cross_run_violations(records))

    sweep_root = REPO_ROOT / "report" / "_sweep" / args.sweep_id
    csv_out = sweep_root / "summary.csv"
    md_out  = sweep_root / "summary.md"
    write_summary_csv(records, args.sweep_id, csv_out)
    write_summary_md(records, violations, args.sweep_id, md_out)
    errs  = sum(1 for v in violations if v.severity == "error")
    warns = sum(1 for v in violations if v.severity == "warn")
    infos = sum(1 for v in violations if v.severity == "info")
    print(f"wrote {csv_out.relative_to(REPO_ROOT)}")
    print(f"wrote {md_out.relative_to(REPO_ROOT)}")
    print(f"runs={len(records)} errors={errs} warnings={warns} info={infos}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
