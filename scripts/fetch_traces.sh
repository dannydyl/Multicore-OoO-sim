#!/usr/bin/env bash
# Fetch a curated subset of public ChampSim traces, decompress (capped at
# MAX_TRACE_BYTES per file), and prepare them for the validation sweep
# harness. With the default 4 GiB cap, the active 4-trace set lands in
# ~16 GB of disk; the full 8-bench corpus in ~32 GB.
#
# SPEC2017 "_s" traces decompress to 50-100+ GB uncompressed. The
# simulator's 100M global cycle cap consumes far less than that per run,
# so we truncate at decompress time to keep disk usage bounded.
#
# Output layout, one dir per benchmark, all four core symlinks pointing at the
# same decompressed trace (homogeneous 4-core):
#
#   traces/champsim/<bench>/raw.champsimtrace
#   traces/champsim/<bench>/p0.champsimtrace -> raw.champsimtrace
#   traces/champsim/<bench>/p1.champsimtrace -> raw.champsimtrace
#   traces/champsim/<bench>/p2.champsimtrace -> raw.champsimtrace
#   traces/champsim/<bench>/p3.champsimtrace -> raw.champsimtrace
#
# Idempotent: skips downloads + decompression when raw file already exists.
# Tolerant: if a download URL fails, prints a warning and moves on so the
# sweep can still run on whatever did succeed (plus the synthetic tier).
#
# URLs point at the Stony Brook DPC-3 mirror at
# https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/ -- this
# is the live successor to the original Texas A&M hpca23 hosting, which
# was decommissioned after HPCA-2023. If the mirror moves, edit the
# TRACES array below; the rest of the script does not need to change.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEST_ROOT="${REPO_ROOT}/traces/champsim"
mkdir -p "${DEST_ROOT}"

# Format per row: bench_label|download_URL|compression(gz|xz)
#
# Eight SPEC2017 SimPoint traces, chosen to span the MPKI spectrum used in
# the CRC-2 / DPC-3 / IPC-1 evaluation literature:
#   high-MPKI (memory-bound)   : mcf, omnetpp, xalancbmk
#   mid-MPKI                   : xz
#   low/mid-MPKI               : gcc
#   low-MPKI (compute-bound)   : deepsjeng, leela, perlbench
#
# If a download URL 404s (the corpus host occasionally shuffles SimPoint
# identifiers), edit the offending row and rerun. The summary at the end
# of this script reports which traces failed.
# Active set: the 4 traces needed for traces/mixes/balanced_4core.txt.
# With the default 4 GiB MAX_TRACE_BYTES cap, total disk = ~16 GB.
# Uncomment the rest of the catalog (below) when you want full sweep
# coverage (adds ~16 GB to total).
TRACES=(
  "mcf|https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/605.mcf_s-665B.champsimtrace.xz|xz"
  "xz|https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/657.xz_s-3167B.champsimtrace.xz|xz"
  "leela|https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/641.leela_s-862B.champsimtrace.xz|xz"
  "perlbench|https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/600.perlbench_s-210B.champsimtrace.xz|xz"

  # Uncomment for full 8-bench coverage (additional ~3 GB compressed,
  # ~15-25 GB decompressed). Required for the hi/mid/lo MPKI mixes
  # and for sweep.json's full champsim_benches list.
  #"omnetpp|https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/620.omnetpp_s-141B.champsimtrace.xz|xz"
  #"xalancbmk|https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/623.xalancbmk_s-700B.champsimtrace.xz|xz"
  #"gcc|https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/602.gcc_s-734B.champsimtrace.xz|xz"
  #"deepsjeng|https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/631.deepsjeng_s-928B.champsimtrace.xz|xz"
)

# Cap each decompressed trace at MAX_TRACE_BYTES. SPEC2017 _s traces
# decompress to 50-100+ GB each, far more than we need: the simulator's
# 100M global cycle cap (full_mode.cpp:62) consumes at most ~100M records
# per core (~6.4 GB) on the highest-IPC workload before terminating. A
# 4 GiB cap is enough for every realistic run and keeps total disk to
# ~16 GB for 4 traces / ~32 GB for the full 8-bench corpus.
#
# Cap must be a multiple of 64 (the ChampSim record size). 4 GiB satisfies
# this. Override via `MAX_TRACE_BYTES=<N> make fetch-traces`.
MAX_TRACE_BYTES="${MAX_TRACE_BYTES:-4294967296}"

decompress() {
  # head -c closes the pipe early once it has enough bytes, which makes
  # the upstream xz/gunzip exit with SIGPIPE (status 141). That's
  # expected behavior, not an error. Disable pipefail/errexit locally
  # so the SIGPIPE doesn't crash the script; verify the result via the
  # output file size check at the call site.
  local algo="$1" infile="$2" outfile="$3"
  set +o pipefail +e
  case "$algo" in
    gz) gunzip -c "$infile" | head -c "${MAX_TRACE_BYTES}" > "$outfile" ;;
    xz) xz -dc "$infile"   | head -c "${MAX_TRACE_BYTES}" > "$outfile" ;;
    *)  echo "ERR: unknown compression '$algo'" >&2; set -eo pipefail; return 1 ;;
  esac
  set -eo pipefail
  return 0
}

filesize() {
  if stat -f%z "$1" >/dev/null 2>&1; then
    stat -f%z "$1"
  else
    stat -c%s "$1"
  fi
}

SUCCEEDED=()
FAILED=()

for entry in "${TRACES[@]}"; do
  IFS='|' read -r name url algo <<< "$entry"
  dest="${DEST_ROOT}/${name}"
  mkdir -p "$dest"
  raw="${dest}/raw.champsimtrace"

  if [[ -f "$raw" && -s "$raw" ]]; then
    echo "skip ${name}: ${raw#${REPO_ROOT}/} exists ($(filesize "$raw") bytes)"
  else
    archive="${dest}/raw.champsimtrace.${algo}"
    echo "download ${name}"
    echo "  ${url} -> ${archive#${REPO_ROOT}/}"
    if ! curl -fL --retry 3 --connect-timeout 30 -o "$archive" "$url"; then
      echo "WARN: download failed for ${name}; harness will skip this trace" >&2
      rm -f "$archive"
      FAILED+=("${name}")
      continue
    fi
    echo "  decompress -> ${raw#${REPO_ROOT}/}"
    if ! decompress "$algo" "$archive" "$raw"; then
      echo "WARN: decompress failed for ${name}" >&2
      rm -f "$archive" "$raw"
      FAILED+=("${name}")
      continue
    fi
    rm -f "$archive"
  fi

  size=$(filesize "$raw")
  if (( size % 64 != 0 )); then
    echo "WARN: ${raw} size ${size} not divisible by 64 (corrupt?); skipping symlinks" >&2
    FAILED+=("${name}")
    continue
  fi
  echo "  ${name}: ${size} bytes ($((size/64)) records)"

  for i in 0 1 2 3; do
    link="${dest}/p${i}.champsimtrace"
    rm -f "$link"
    ln -s "raw.champsimtrace" "$link"
  done
  SUCCEEDED+=("${name}")
done

total=${#TRACES[@]}
ok=${#SUCCEEDED[@]}
echo
echo "fetch_traces.sh: ${ok}/${total} traces ready"
if (( ${#FAILED[@]} > 0 )); then
  echo "  missing: ${FAILED[*]}"
  echo "  (edit the TRACES array above with corrected URLs and rerun;"
  echo "   the sweep harness will skip missing benches and continue)"
fi
