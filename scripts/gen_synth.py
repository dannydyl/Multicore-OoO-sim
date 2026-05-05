#!/usr/bin/env python3
"""Generate synthetic ChampSim traces for the validation sweep harness.

Calls build-release/tools/gen_trace/gen_trace once per (pattern × size)
combination listed in sweep_matrix.SYNTH_TIERS / SYNTH_PATTERNS, writes the
output to traces/synth/<pattern>_<size>/raw.champsimtrace, then symlinks
p0..p3.champsimtrace -> raw.champsimtrace so the directory works as a
homogeneous 4-core trace dir for the simulator.

Idempotent: re-runs skip any (pattern, size) whose raw file and four
symlinks already exist. Pass --force to regenerate.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from sweep_matrix import REPO_ROOT, DEFAULT_SWEEP_CONFIG, load_matrix

GEN_TRACE_BIN = REPO_ROOT / "build-release" / "tools" / "gen_trace" / "gen_trace"
TRACES_ROOT = REPO_ROOT / "traces" / "synth"


def trace_dir(pattern: str, size: str) -> Path:
    return TRACES_ROOT / f"{pattern}_{size}"


def already_done(d: Path) -> bool:
    raw = d / "raw.champsimtrace"
    if not raw.exists() or raw.stat().st_size == 0:
        return False
    for i in range(4):
        link = d / f"p{i}.champsimtrace"
        if not link.exists():
            return False
    return True


def make_dir(pattern: str, size: str, records: int, seed: int) -> Path:
    d = trace_dir(pattern, size)
    d.mkdir(parents=True, exist_ok=True)
    raw = d / "raw.champsimtrace"
    if not raw.exists() or raw.stat().st_size == 0:
        cmd = [
            str(GEN_TRACE_BIN),
            "--out", str(raw),
            "--records", str(records),
            "--pattern", pattern,
            "--seed", str(seed),
        ]
        subprocess.run(cmd, check=True)
    for i in range(4):
        link = d / f"p{i}.champsimtrace"
        if link.is_symlink() or link.exists():
            link.unlink()
        os.symlink("raw.champsimtrace", link)
    return d


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--tier", default="all",
                    help="Which tier's synth_sizes to generate (default: all).")
    ap.add_argument("--sweep-config", default=str(DEFAULT_SWEEP_CONFIG),
                    help=f"Path to sweep matrix JSON (default: {DEFAULT_SWEEP_CONFIG.relative_to(REPO_ROOT)}).")
    ap.add_argument("--force", action="store_true",
                    help="Regenerate even if traces already exist.")
    args = ap.parse_args(argv)

    if not GEN_TRACE_BIN.exists():
        print(f"ERROR: gen_trace not found at {GEN_TRACE_BIN}", file=sys.stderr)
        print("       cmake --preset release && cmake --build build-release -j",
              file=sys.stderr)
        return 1

    matrix = load_matrix(Path(args.sweep_config))
    if args.tier not in matrix.tiers:
        print(f"ERROR: tier '{args.tier}' not in {args.sweep_config}; "
              f"choices: {list(matrix.tiers)}", file=sys.stderr)
        return 1

    sizes = matrix.tiers[args.tier].get("synth_sizes", [])
    seed_base = 0xC0DEF00D

    for pi, pattern in enumerate(matrix.synth_patterns):
        for si, size in enumerate(sizes):
            d = trace_dir(pattern, size)
            if not args.force and already_done(d):
                print(f"skip {d.relative_to(REPO_ROOT)} (exists)")
                continue
            records = matrix.synth_tiers[size]["records"]
            seed = seed_base + 100 * pi + si
            print(f"gen {pattern}/{size}: {records:,} records seed={seed:#010x}")
            make_dir(pattern, size, records, seed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
