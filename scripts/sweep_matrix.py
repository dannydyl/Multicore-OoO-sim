"""Sweep matrix loader.

Loads tiers / axes / hand-picked combos / synth-tier sizes from a JSON file
(default: configs/sweep.json). Provides `RunSpec` and `expand()` for the
driver scripts. No I/O beyond reading the JSON file.

Run identity is (trace_id, run_id). The simulator's report folder name is
predictable from those plus the resolved protocol/cores:

    report/<basename(trace_dir)>_<proto>_c<cores>_<sweep_id>__<run_id>/
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

REPO_ROOT = Path(__file__).resolve().parent.parent
BASELINE_PATH = REPO_ROOT / "configs" / "baseline.json"
DEFAULT_SWEEP_CONFIG = REPO_ROOT / "configs" / "sweep.json"


@dataclass(frozen=True)
class RunSpec:
    run_id: str
    trace_id: str
    trace_dir: Path
    overrides: tuple[tuple[str, Any], ...]
    protocol: str
    cores: int

    @property
    def overrides_dict(self) -> dict[str, Any]:
        return dict(self.overrides)


@dataclass
class SweepMatrix:
    synth_tiers: dict[str, dict[str, Any]]
    synth_patterns: list[str]
    champsim_benches: list[str]
    axes: dict[str, list[tuple[str, dict[str, Any]]]]
    handpicked: list[tuple[str, dict[str, Any]]]
    tiers: dict[str, dict[str, Any]]
    source_path: Path = field(default_factory=lambda: DEFAULT_SWEEP_CONFIG)


def load_matrix(path: Path | None = None) -> SweepMatrix:
    """Read the sweep JSON and return a SweepMatrix. Path defaults to configs/sweep.json."""
    p = Path(path) if path else DEFAULT_SWEEP_CONFIG
    if not p.exists():
        raise FileNotFoundError(f"sweep config not found: {p}")
    raw = json.loads(p.read_text())

    def _normalize_value_list(rows: Any) -> list[tuple[str, dict[str, Any]]]:
        out: list[tuple[str, dict[str, Any]]] = []
        for row in rows:
            if not (isinstance(row, list) and len(row) == 2):
                raise ValueError(f"axis row must be [label, overrides_dict]: got {row!r}")
            label, overrides = row
            if not isinstance(label, str) or not isinstance(overrides, dict):
                raise ValueError(f"axis row malformed in {p}: {row!r}")
            out.append((label, overrides))
        return out

    axes_raw = raw.get("axes", {})
    axes = {axis: _normalize_value_list(values) for axis, values in axes_raw.items()}
    handpicked = _normalize_value_list(raw.get("handpicked", []))

    return SweepMatrix(
        synth_tiers=raw.get("synth_tiers", {}),
        synth_patterns=list(raw.get("synth_patterns", [])),
        champsim_benches=list(raw.get("champsim_benches", [])),
        axes=axes,
        handpicked=handpicked,
        tiers=raw.get("tiers", {}),
        source_path=p,
    )


def _trace_id_to_dir(trace_id: str, repo_root: Path) -> Path:
    return repo_root / "traces" / trace_id


def _resolve_proto_cores(overrides: dict[str, Any]) -> tuple[str, int]:
    proto = str(overrides.get("coherence.protocol", "mesi"))
    cores = int(overrides.get("cores", 4))
    return proto, cores


def trace_ids_for_tier(tier_label: str, matrix: SweepMatrix) -> list[str]:
    if tier_label not in matrix.tiers:
        raise ValueError(f"unknown tier '{tier_label}'; choices: {list(matrix.tiers)}")
    spec = matrix.tiers[tier_label]
    ids: list[str] = []
    for pat in matrix.synth_patterns:
        for size in spec.get("synth_sizes", []):
            ids.append(f"synth/{pat}_{size}")
    for bench in spec.get("champsim", []):
        ids.append(f"champsim/{bench}")
    return ids


def expand(
    tier: str,
    *,
    matrix: SweepMatrix | None = None,
    only_axes: Iterable[str] | None = None,
    only_traces: Iterable[str] | None = None,
    repo_root: Path = REPO_ROOT,
) -> list[RunSpec]:
    if matrix is None:
        matrix = load_matrix()
    if tier not in matrix.tiers:
        raise ValueError(f"unknown tier '{tier}'; choices: {list(matrix.tiers)}")
    spec = matrix.tiers[tier]
    only_axes_set = set(only_axes) if only_axes else None
    only_traces_set = set(only_traces) if only_traces else None

    trace_ids = trace_ids_for_tier(tier, matrix)
    if only_traces_set is not None:
        trace_ids = [t for t in trace_ids if t in only_traces_set]

    selected_axes = [a for a in spec.get("axes", [])
                     if (only_axes_set is None or a in only_axes_set)]
    use_handpicked = spec.get("handpicked", False) and (
        only_axes_set is None or "handpicked" in only_axes_set
    )

    runs: list[RunSpec] = []
    for trace_id in trace_ids:
        trace_dir = _trace_id_to_dir(trace_id, repo_root)
        proto, cores = _resolve_proto_cores({})
        runs.append(RunSpec(
            run_id="baseline",
            trace_id=trace_id,
            trace_dir=trace_dir,
            overrides=tuple(),
            protocol=proto,
            cores=cores,
        ))
        for axis in selected_axes:
            for value_label, overrides in matrix.axes.get(axis, []):
                proto, cores = _resolve_proto_cores(overrides)
                runs.append(RunSpec(
                    run_id=f"{axis}_{value_label}",
                    trace_id=trace_id,
                    trace_dir=trace_dir,
                    overrides=tuple(sorted(overrides.items())),
                    protocol=proto,
                    cores=cores,
                ))
        if use_handpicked:
            for run_id, overrides in matrix.handpicked:
                proto, cores = _resolve_proto_cores(overrides)
                runs.append(RunSpec(
                    run_id=run_id,
                    trace_id=trace_id,
                    trace_dir=trace_dir,
                    overrides=tuple(sorted(overrides.items())),
                    protocol=proto,
                    cores=cores,
                ))
    return runs
