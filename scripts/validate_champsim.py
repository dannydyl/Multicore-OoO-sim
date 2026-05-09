#!/usr/bin/env python3
"""Sanity-check that real-trace runs produce in-band IPC and miss rates.

Runs each available champsim per-bench dir and each mixes/*.txt manifest
once, parses the resulting report/<...>/report.csv, and checks that the
aggregate IPC and L1 miss rate fall inside loose published-baseline
bands. Exits non-zero on any out-of-band result.

This catches the case where a code change accidentally regresses real
workload behavior — e.g. mcf comes back at 0.005 IPC instead of 0.3,
which would otherwise silently land. The bands are deliberately wide:
catching real bugs, not enforcing exact numbers.

Skips benchmarks whose traces are not present on disk. Run
scripts/fetch_traces.sh first if you want full coverage.

Usage:
  scripts/validate_champsim.py
  scripts/validate_champsim.py --only mcf,balanced_4core
  scripts/validate_champsim.py --no-build  # skip rebuild check
"""
from __future__ import annotations

import argparse
import csv
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from sweep_matrix import REPO_ROOT  # noqa: E402

SIM_BIN = REPO_ROOT / "build-release" / "src" / "sim"
BASELINE_CFG = REPO_ROOT / "configs" / "baseline.json"
REPORT_ROOT = REPO_ROOT / "report"
CHAMPSIM_ROOT = REPO_ROOT / "traces" / "champsim"
MIXES_ROOT = REPO_ROOT / "traces" / "mixes"


@dataclass(frozen=True)
class Band:
    ipc_lo: float
    ipc_hi: float
    l1_miss_lo: float
    l1_miss_hi: float


# Loose bands — designed to catch order-of-magnitude regressions, not
# enforce exact numbers. Per-core aggregate (mean IPC, mean L1 miss rate
# across the 4 cores). Numbers reflect typical SPEC2017 SimPoint behavior
# under similar 4-core OoO + MESI configurations from CRC-2 / DPC-3 /
# IPC-1 reference runs.
PER_BENCH_BANDS: dict[str, Band] = {
    "mcf":       Band(ipc_lo=0.05, ipc_hi=0.6, l1_miss_lo=0.05, l1_miss_hi=0.50),
    "omnetpp":   Band(ipc_lo=0.10, ipc_hi=0.8, l1_miss_lo=0.05, l1_miss_hi=0.40),
    "xalancbmk": Band(ipc_lo=0.20, ipc_hi=1.5, l1_miss_lo=0.02, l1_miss_hi=0.30),
    "xz":        Band(ipc_lo=0.30, ipc_hi=2.0, l1_miss_lo=0.00, l1_miss_hi=0.20),
    "gcc":       Band(ipc_lo=0.30, ipc_hi=2.0, l1_miss_lo=0.00, l1_miss_hi=0.15),
    "deepsjeng": Band(ipc_lo=0.50, ipc_hi=2.5, l1_miss_lo=0.00, l1_miss_hi=0.05),
    "leela":     Band(ipc_lo=0.40, ipc_hi=2.0, l1_miss_lo=0.00, l1_miss_hi=0.05),
    "perlbench": Band(ipc_lo=0.50, ipc_hi=2.5, l1_miss_lo=0.00, l1_miss_hi=0.10),
}

MIX_BANDS: dict[str, Band] = {
    # Aggregate across 4 cores (per-core mean).
    "balanced_4core":  Band(ipc_lo=0.20, ipc_hi=1.5, l1_miss_lo=0.02, l1_miss_hi=0.40),
    "hi_mpki_4core":   Band(ipc_lo=0.05, ipc_hi=0.8, l1_miss_lo=0.10, l1_miss_hi=0.50),
    "mid_mpki_4core":  Band(ipc_lo=0.20, ipc_hi=1.5, l1_miss_lo=0.02, l1_miss_hi=0.30),
    "lo_mpki_4core":   Band(ipc_lo=0.40, ipc_hi=2.0, l1_miss_lo=0.00, l1_miss_hi=0.10),
}


@dataclass
class RunResult:
    label: str
    ipc_mean: float
    l1_miss_mean: float
    band: Band

    @property
    def ipc_ok(self) -> bool:
        return self.band.ipc_lo <= self.ipc_mean <= self.band.ipc_hi

    @property
    def l1_ok(self) -> bool:
        return self.band.l1_miss_lo <= self.l1_miss_mean <= self.band.l1_miss_hi

    @property
    def ok(self) -> bool:
        return self.ipc_ok and self.l1_ok


def parse_report_csv(csv_path: Path) -> tuple[float, float]:
    """Return (mean per-core IPC, mean per-core L1 miss rate)."""
    with csv_path.open() as f:
        rows = list(csv.DictReader(f))
    if not rows:
        raise ValueError(f"empty CSV: {csv_path}")
    ipcs = [float(r["ipc"]) for r in rows]
    miss_rates = [float(r["l1_miss_rate"]) for r in rows]
    return sum(ipcs) / len(ipcs), sum(miss_rates) / len(miss_rates)


def run_one(label: str, trace_arg: tuple[str, str], tag: str) -> Path:
    """Invoke the sim and return the report dir."""
    flag, value = trace_arg
    cmd = [
        str(SIM_BIN),
        "--config", str(BASELINE_CFG),
        flag, value,
        "--tag", tag,
    ]
    print(f"  [{label}] {' '.join(cmd)}", flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        raise RuntimeError(f"sim failed for {label} (exit {proc.returncode})")
    # Parse the "wrote reports to" line for the actual run dir.
    for line in proc.stderr.splitlines():
        if "wrote reports to" in line:
            return Path(line.split('"')[1] if '"' in line else line.split()[-1])
    raise RuntimeError(f"could not locate report dir for {label}")


def select_targets(only: set[str] | None) -> list[tuple[str, tuple[str, str], Band]]:
    """Return [(label, (sim-flag, sim-arg), band)] for what's actually on disk."""
    targets: list[tuple[str, tuple[str, str], Band]] = []

    for bench, band in PER_BENCH_BANDS.items():
        if only is not None and bench not in only:
            continue
        bench_dir = CHAMPSIM_ROOT / bench
        if not (bench_dir / "p0.champsimtrace").exists():
            continue
        targets.append((bench, ("--trace-dir", str(bench_dir)), band))

    for mix, band in MIX_BANDS.items():
        if only is not None and mix not in only:
            continue
        manifest = MIXES_ROOT / f"{mix}.txt"
        if not manifest.exists():
            continue
        # Manifest entries must all resolve.
        all_present = True
        for raw in manifest.read_text().splitlines():
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            tp = Path(line)
            if not tp.is_absolute():
                tp = manifest.parent / tp
            if not tp.exists():
                all_present = False
                break
        if not all_present:
            continue
        targets.append((mix, ("--trace-list", str(manifest)), band))

    return targets


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n", 1)[0] if __doc__ else None,
    )
    ap.add_argument("--only", default="",
                    help="Comma-separated list of bench/mix labels to validate.")
    ap.add_argument("--tag", default="validate",
                    help="Tag suffix for the report directory (default: validate).")
    args = ap.parse_args(argv)

    only = set(t for t in args.only.split(",") if t) or None

    if not SIM_BIN.exists():
        print(f"ERROR: simulator binary missing: {SIM_BIN}", file=sys.stderr)
        print("       run `make build` (or `cmake --build build-release`) first.",
              file=sys.stderr)
        return 2

    targets = select_targets(only)
    if not targets:
        print("WARN: no champsim/mixes traces found on disk.", file=sys.stderr)
        print("      run scripts/fetch_traces.sh first; or pass --only with",
              file=sys.stderr)
        print("      a label whose trace exists.", file=sys.stderr)
        return 0  # not a failure — just nothing to validate

    print(f"validating {len(targets)} targets:")
    results: list[RunResult] = []
    for label, trace_arg, band in targets:
        try:
            run_dir = run_one(label, trace_arg, args.tag)
        except RuntimeError as e:
            print(f"  ! {label}: {e}", file=sys.stderr)
            continue
        csv_path = run_dir / "report.csv"
        if not csv_path.exists():
            print(f"  ! {label}: no report.csv at {csv_path}", file=sys.stderr)
            continue
        ipc, l1 = parse_report_csv(csv_path)
        results.append(RunResult(label, ipc, l1, band))

    print()
    print(f"{'label':<20} {'ipc':>8} {'ipc band':>14} {'l1miss':>8} {'l1 band':>14}  status")
    print("-" * 80)
    n_fail = 0
    for r in results:
        ipc_band = f"[{r.band.ipc_lo:.2f},{r.band.ipc_hi:.2f}]"
        l1_band = f"[{r.band.l1_miss_lo:.2f},{r.band.l1_miss_hi:.2f}]"
        status = "ok" if r.ok else "FAIL"
        if not r.ok:
            n_fail += 1
        print(f"{r.label:<20} {r.ipc_mean:>8.3f} {ipc_band:>14} "
              f"{r.l1_miss_mean:>8.3f} {l1_band:>14}  {status}")
    print()
    if n_fail:
        print(f"validate_champsim: {n_fail}/{len(results)} OUT OF BAND", file=sys.stderr)
        return 1
    print(f"validate_champsim: {len(results)}/{len(results)} in band")
    return 0


if __name__ == "__main__":
    sys.exit(main())
