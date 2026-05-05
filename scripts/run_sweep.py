#!/usr/bin/env python3
"""Drive the validation sweep across the matrix in sweep_matrix.py.

For each (trace, run_id) pair selected by --tier (and optional filters):
  1. Clone configs/baseline.json, apply the run's overrides, write to
     report/_sweep/<sweep_id>/configs/<run_id>.json.
  2. Invoke build-release/src/sim with --config <that> --trace-dir <trace_dir>
     --tag <sweep_id>__<run_id>. The sim writes its own per-run report to
     report/<basename(trace_dir)>_<proto>_c<cores>_<sweep_id>__<run_id>/.
  3. Capture stdout/stderr/exit/wallclock to
     report/_sweep/<sweep_id>/logs/<run_id>__<safe_trace_id>.{out,err,meta.json}.
  4. Append a row to report/_sweep/<sweep_id>/progress.tsv (live).

Parallelism: ThreadPoolExecutor; each sim is its own subprocess so threads
are just waiting on OS-level wait(). Default --jobs = nproc/2.

Resume: --resume skips runs whose report.csv already has the expected rows.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from sweep_matrix import (
    REPO_ROOT, BASELINE_PATH, DEFAULT_SWEEP_CONFIG,
    RunSpec, expand, load_matrix,
)

SIM_BIN = REPO_ROOT / "build-release" / "src" / "sim"


@dataclass
class RunResult:
    run_id: str
    trace_id: str
    status: str           # "ok" | "fail" | "timeout" | "skip" | "error"
    exit_code: int | None
    wallclock_s: float
    report_dir: Path
    protocol: str
    cores: int


def apply_overrides(cfg: dict, overrides: dict[str, Any]) -> dict:
    out = json.loads(json.dumps(cfg))
    for dotted, val in overrides.items():
        parts = dotted.split(".")
        cur = out
        for p in parts[:-1]:
            cur = cur[p]
        cur[parts[-1]] = val
    return out


def materialize_config(cfg: dict, out_path: Path) -> Path:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(cfg, indent=2) + "\n")
    return out_path


def predicted_run_dir(spec: RunSpec, sweep_id: str) -> Path:
    base = spec.trace_dir.name
    tag = f"{sweep_id}__{spec.run_id}"
    return REPO_ROOT / "report" / f"{base}_{spec.protocol}_c{spec.cores}_{tag}"


def run_already_done(spec: RunSpec, sweep_id: str) -> bool:
    csv = predicted_run_dir(spec, sweep_id) / "report.csv"
    if not csv.exists():
        return False
    try:
        with csv.open() as f:
            non_empty = sum(1 for line in f if line.strip())
        return non_empty >= spec.cores + 1
    except OSError:
        return False


def safe_trace_token(trace_id: str) -> str:
    return trace_id.replace("/", "_")


def run_one(
    spec: RunSpec,
    sweep_id: str,
    baseline_cfg: dict,
    sweep_root: Path,
    timeout_s: int,
) -> RunResult:
    cfg = apply_overrides(baseline_cfg, spec.overrides_dict)
    cfg_path = sweep_root / "configs" / f"{spec.run_id}__{safe_trace_token(spec.trace_id)}.json"
    materialize_config(cfg, cfg_path)

    log_dir = sweep_root / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    stem = f"{spec.run_id}__{safe_trace_token(spec.trace_id)}"
    out_log = log_dir / f"{stem}.out"
    err_log = log_dir / f"{stem}.err"
    meta_path = log_dir / f"{stem}.meta.json"

    tag = f"{sweep_id}__{spec.run_id}"
    cmd = [
        str(SIM_BIN),
        "--config", str(cfg_path),
        "--trace-dir", str(spec.trace_dir),
        "--tag", tag,
    ]

    rd = predicted_run_dir(spec, sweep_id)
    start = time.time()
    status: str
    exit_code: int | None
    try:
        with out_log.open("w") as ofd, err_log.open("w") as efd:
            proc = subprocess.run(cmd, stdout=ofd, stderr=efd, timeout=timeout_s)
        exit_code = proc.returncode
        status = "ok" if exit_code == 0 else "fail"
    except subprocess.TimeoutExpired:
        status = "timeout"
        exit_code = None
    except FileNotFoundError as e:
        status = "error"
        exit_code = None
        err_log.write_text(str(e))
    wall = time.time() - start

    meta = {
        "run_id": spec.run_id,
        "trace_id": spec.trace_id,
        "trace_dir": str(spec.trace_dir),
        "config_path": str(cfg_path),
        "overrides": spec.overrides_dict,
        "protocol": spec.protocol,
        "cores": spec.cores,
        "tag": tag,
        "report_dir": str(rd),
        "command": cmd,
        "status": status,
        "exit_code": exit_code,
        "wallclock_s": wall,
        "timeout_s": timeout_s,
        "sweep_id": sweep_id,
    }
    meta_path.write_text(json.dumps(meta, indent=2) + "\n")

    return RunResult(spec.run_id, spec.trace_id, status, exit_code, wall, rd,
                     spec.protocol, spec.cores)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier", required=True,
                    help="Tier name from sweep config (typically smoke/short/medium/long/all).")
    ap.add_argument("--sweep-id", required=True,
                    help="Identifier baked into --tag and the per-sweep report dir.")
    ap.add_argument("--sweep-config", default=str(DEFAULT_SWEEP_CONFIG),
                    help=f"Path to sweep matrix JSON (default: {DEFAULT_SWEEP_CONFIG.relative_to(REPO_ROOT)}).")
    ap.add_argument("--jobs", type=int,
                    default=max(1, (os.cpu_count() or 4) // 2))
    ap.add_argument("--timeout", type=int, default=1800,
                    help="Per-run wallclock cap in seconds.")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--resume", action="store_true",
                    help="Skip runs whose report.csv already has the expected rows.")
    ap.add_argument("--only-axes", default="",
                    help="Comma-separated axis names to restrict (e.g. 'proto,rob').")
    ap.add_argument("--only-traces", default="",
                    help="Comma-separated trace_ids to restrict (e.g. 'synth/loop_tiny,champsim/mcf').")
    ap.add_argument("--strict-traces", action="store_true",
                    help="Fail if any trace dir is missing instead of skipping it with a warning.")
    args = ap.parse_args(argv)
    matrix = load_matrix(Path(args.sweep_config))
    if args.tier not in matrix.tiers:
        print(f"ERROR: tier '{args.tier}' not in {args.sweep_config}; "
              f"choices: {list(matrix.tiers)}", file=sys.stderr)
        return 1

    if not SIM_BIN.exists():
        print(f"ERROR: sim binary not found at {SIM_BIN}", file=sys.stderr)
        print("       cmake --preset release && cmake --build build-release -j",
              file=sys.stderr)
        return 2

    baseline_cfg = json.loads(BASELINE_PATH.read_text())

    only_axes = [a for a in args.only_axes.split(",") if a]
    only_traces = [t for t in args.only_traces.split(",") if t]
    specs = expand(args.tier,
                   matrix=matrix,
                   only_axes=only_axes or None,
                   only_traces=only_traces or None)

    def trace_dir_ready(d: Path) -> bool:
        return d.is_dir() and (d / "p0.champsimtrace").exists()

    missing = sorted({s.trace_id for s in specs if not trace_dir_ready(s.trace_dir)})
    if missing:
        if args.strict_traces:
            print("ERROR: trace dirs missing (--strict-traces set):", file=sys.stderr)
            for m in missing:
                print(f"   - {m}  ({REPO_ROOT / 'traces' / m})", file=sys.stderr)
            print("       run scripts/gen_synth.py and/or scripts/fetch_traces.sh first.",
                  file=sys.stderr)
            return 3
        print(f"WARN: skipping {len(missing)} missing trace dir(s):", file=sys.stderr)
        for m in missing:
            print(f"   - {m}", file=sys.stderr)
        print("      generate with scripts/gen_synth.py --tier <T> and/or "
              "scripts/fetch_traces.sh; or pass --strict-traces to fail instead.",
              file=sys.stderr)
        missing_set = set(missing)
        specs = [s for s in specs if s.trace_id not in missing_set]
        if not specs:
            print("ERROR: no traces available; nothing to run.", file=sys.stderr)
            return 3

    sweep_root = REPO_ROOT / "report" / "_sweep" / args.sweep_id
    sweep_root.mkdir(parents=True, exist_ok=True)
    progress_path = sweep_root / "progress.tsv"
    progress_lock = threading.Lock()
    if not progress_path.exists():
        progress_path.write_text(
            "ts\trun_id\ttrace_id\tprotocol\tcores\tstatus\texit\twall_s\treport_dir\n"
        )

    if args.resume:
        before = len(specs)
        specs = [s for s in specs if not run_already_done(s, args.sweep_id)]
        print(f"resume: {before - len(specs)} already done, {len(specs)} remaining")

    print(f"sweep '{args.sweep_id}' tier={args.tier} runs={len(specs)} "
          f"jobs={args.jobs} timeout={args.timeout}s")
    if args.dry_run:
        for s in specs:
            print(f"  would run: {s.run_id:<32} trace={s.trace_id:<32} "
                  f"proto={s.protocol} cores={s.cores}")
        return 0

    failures = 0
    completed = 0
    total = len(specs)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {
            ex.submit(run_one, s, args.sweep_id, baseline_cfg, sweep_root,
                      args.timeout): s
            for s in specs
        }
        for fut in concurrent.futures.as_completed(futs):
            s = futs[fut]
            try:
                r = fut.result()
            except Exception as e:
                r = RunResult(s.run_id, s.trace_id, "error", -1, 0.0,
                              predicted_run_dir(s, args.sweep_id),
                              s.protocol, s.cores)
                print(f"  ! exception in {s.run_id}/{s.trace_id}: {e}",
                      file=sys.stderr)
            completed += 1
            if r.status != "ok":
                failures += 1
            with progress_lock, progress_path.open("a") as fp:
                fp.write(f"{int(time.time())}\t{r.run_id}\t{r.trace_id}\t"
                         f"{r.protocol}\t{r.cores}\t{r.status}\t{r.exit_code}\t"
                         f"{r.wallclock_s:.2f}\t{r.report_dir}\n")
            mark = "OK" if r.status == "ok" else r.status.upper()
            print(f"  [{completed:>4}/{total}] {mark:<7} {r.run_id:<32} "
                  f"{r.trace_id:<32} exit={r.exit_code} wall={r.wallclock_s:.1f}s")

    print(f"sweep done: {completed} runs, {failures} failures")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
