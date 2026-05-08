#!/usr/bin/env python3
"""Generate synthetic ChampSim traces for the validation sweep harness.

For each (pattern × size) combination listed in
sweep_matrix.SYNTH_TIERS / SYNTH_PATTERNS, calls
build-release/tools/gen_trace/gen_trace **once per core** (4 calls
total) with a per-core seed offset and a per-core addr_base offset, so
each `p<i>.champsimtrace` is a *distinct* stream over a *disjoint*
address range.

Why per-core distinct streams (not the previous one-file-symlinked-4x):
the old layout had every core executing the byte-identical instruction
stream over byte-identical addresses, which is unrealistic (real
multithreaded workloads diverge through branch outcomes, store mixes,
and per-thread data partitioning) and made the cross-protocol
invariance rule meaningless because the workload was effectively
fully-shared instead of private.

Idempotent: re-runs skip any (pattern, size) whose four per-core files
already exist as regular files (not symlinks). Pass --force to
regenerate.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from sweep_matrix import REPO_ROOT, DEFAULT_SWEEP_CONFIG, load_matrix

GEN_TRACE_BIN = REPO_ROOT / "build-release" / "tools" / "gen_trace" / "gen_trace"
TRACES_ROOT = REPO_ROOT / "traces" / "synth"

# gen_trace's default addr_base (see tools/gen_trace/gen_trace.hpp).
DEFAULT_ADDR_BASE = 0x1000_0000

# Per-core address offset. Each core's working set is at most:
#   sequential/stream: N records * 64 B/record       (long-tier = 6.4 GB)
#   random:            16 MB window (gen_trace.cpp:71)
#   loop:              loop_size * addr_stride       (default 4 KB)
# 1 TB stride is comfortably larger than any of those AND fits in the
# 64-bit address space with room for thousands of cores.
PER_CORE_ADDR_STRIDE = 1 << 40   # 1 TB

# How many cores to pre-generate traces for. Always 4 since baseline
# config and tests assume that, but kept as a constant for clarity.
NUM_CORES = 4


def trace_dir(pattern: str, size: str) -> Path:
    return TRACES_ROOT / f"{pattern}_{size}"


def already_done(d: Path) -> bool:
    for i in range(NUM_CORES):
        f = d / f"p{i}.champsimtrace"
        # Reject symlinks too: those are leftovers from the old
        # one-raw-file-symlinked-4x layout. We want regular files.
        if not f.exists() or f.is_symlink() or f.stat().st_size == 0:
            return False
    return True


def make_dir(pattern: str, size: str, records: int, seed_base: int) -> Path:
    d = trace_dir(pattern, size)
    d.mkdir(parents=True, exist_ok=True)

    # Drop legacy raw.champsimtrace and any symlinks from earlier layout.
    legacy_raw = d / "raw.champsimtrace"
    if legacy_raw.exists() or legacy_raw.is_symlink():
        legacy_raw.unlink()
    for i in range(NUM_CORES):
        out = d / f"p{i}.champsimtrace"
        if out.is_symlink() or out.exists():
            out.unlink()

    for i in range(NUM_CORES):
        out = d / f"p{i}.champsimtrace"
        addr_base = DEFAULT_ADDR_BASE + i * PER_CORE_ADDR_STRIDE
        cmd = [
            str(GEN_TRACE_BIN),
            "--out", str(out),
            "--records", str(records),
            "--pattern", pattern,
            "--seed", str(seed_base + i),
            "--addr-base", str(addr_base),
        ]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
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
            seed = seed_base + 1000 * pi + 10 * si
            print(f"gen {pattern}/{size}: {records:,} records × {NUM_CORES} cores "
                  f"seed_base={seed:#010x}")
            make_dir(pattern, size, records, seed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
