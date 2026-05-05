#!/usr/bin/env bash
# Fetch a curated subset of public ChampSim traces (~1.5 GB total) and prepare
# them for the validation sweep harness.
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
# URLs reflect the canonical ChampSim trace hosting at hpca23.cse.tamu.edu/
# champsim-traces/ as of 2025. If they have moved, edit the TRACES array
# below; the rest of the script does not need to change.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEST_ROOT="${REPO_ROOT}/traces/champsim"
mkdir -p "${DEST_ROOT}"

# Format per row: bench_label|download_URL|compression(gz|xz)
TRACES=(
  "mcf|https://hpca23.cse.tamu.edu/champsim-traces/speccpu/605.mcf_s-665B.champsimtrace.xz|xz"
  "perlbench|https://hpca23.cse.tamu.edu/champsim-traces/speccpu/600.perlbench_s-210B.champsimtrace.xz|xz"
  "server|https://hpca23.cse.tamu.edu/champsim-traces/speccpu/623.xalancbmk_s-700B.champsimtrace.xz|xz"
)

decompress() {
  local algo="$1" infile="$2" outfile="$3"
  case "$algo" in
    gz) gunzip -c "$infile" > "$outfile" ;;
    xz) xz -dc "$infile" > "$outfile" ;;
    *)  echo "ERR: unknown compression '$algo'" >&2; return 1 ;;
  esac
}

filesize() {
  if stat -f%z "$1" >/dev/null 2>&1; then
    stat -f%z "$1"
  else
    stat -c%s "$1"
  fi
}

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
      continue
    fi
    echo "  decompress -> ${raw#${REPO_ROOT}/}"
    if ! decompress "$algo" "$archive" "$raw"; then
      echo "WARN: decompress failed for ${name}" >&2
      rm -f "$archive" "$raw"
      continue
    fi
    rm -f "$archive"
  fi

  size=$(filesize "$raw")
  if (( size % 64 != 0 )); then
    echo "WARN: ${raw} size ${size} not divisible by 64 (corrupt?); skipping symlinks" >&2
    continue
  fi
  echo "  ${name}: ${size} bytes ($((size/64)) records)"

  for i in 0 1 2 3; do
    link="${dest}/p${i}.champsimtrace"
    rm -f "$link"
    ln -s "raw.champsimtrace" "$link"
  done
done

echo "fetch_traces.sh: done"
