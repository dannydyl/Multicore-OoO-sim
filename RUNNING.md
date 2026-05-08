# Running the Multicore OoO Simulator

A step-by-step guide for running this simulator end to end: how to build it,
how to feed it a workload, where every output file lands on disk, and what the
numbers in those files mean.

The text in **"What this means in architecture terms"** boxes is background
on what the simulator is actually modeling. If you're already comfortable
with Tomasulo, MESI, ROBs, etc., skim those and focus on commands and file
paths.

## Table of contents

1. [The 30-second version](#1-the-30-second-version)
2. [What this simulator models](#2-what-this-simulator-models)
3. [Prerequisites](#3-prerequisites)
4. [Building the simulator](#4-building-the-simulator)
5. [Inputs: configs and traces](#5-inputs-configs-and-traces)
6. [Running a single simulation](#6-running-a-single-simulation)
7. [Where output files go](#7-where-output-files-go)
8. [Reading `report.rpt` like an architect](#8-reading-reportrpt-like-an-architect)
9. [Sweeps: running many configs at once](#9-sweeps-running-many-configs-at-once)
10. [Cleanup](#10-cleanup)
11. [Troubleshooting](#11-troubleshooting)
12. [File-path cheat sheet](#12-file-path-cheat-sheet)

---

## 1. The 30-second version

```sh
make build                              # compile (into build-release/)
make run TRACE=traces/core_4            # run one simulation
ls report/core_4_mesi_c4/               # report.rpt is the human report
```

For a full validation sweep:

```sh
make smoke               # tiny end-to-end: synth traces + sweep + summary
```

If `make smoke` finishes without errors and `report/_sweep/smoke/summary.md`
says `Errors: 0`, the simulator is working on your machine.

---

## 2. What this simulator models

This is a **chip-multiprocessor (CMP) simulator**: several out-of-order cores
sharing memory through a directory-based cache coherence protocol over a
ring interconnect.

```
                +-----------------------------+
                |        Coherent Network     |   (ring)
                |  (directory + interconnect) |
                +--+-------+-------+-------+--+
                   |       |       |       |
                 Core 0  Core 1  Core 2  Core N-1
                 +----+ +----+ +----+ +----+
                 |OoO | |OoO | |OoO | |OoO |
                 | L1 | | L1 | | L1 | | L1 |
                 | L2 | | L2 | | L2 | | L2 |
                 +----+ +----+ +----+ +----+
                              |
                            DRAM
```

> **What this means in architecture terms.** Each core is a Tomasulo-style
> superscalar pipeline. The simulator models five stages:
> **fetch → dispatch (decode+rename) → schedule (issue) → execute → retire (state update)**.
> Results broadcast on the CDB at the end of execute — there is no
> dedicated writeback stage. Stores don't write back to a register file at
> all; they sit in the LSU queue and drain to memory. A real **branch
> predictor** (Yeh-Patt or perceptron) drives speculation; a **reorder
> buffer (ROB)** holds in-flight instructions so they can commit in program
> order even though they execute out of order. Each core has private **L1**
> and **L2** caches. When several cores read or write the same memory
> block, a **coherence protocol** (MI / MSI / MESI / MOSI / MOESIF) keeps
> their views consistent. Misses that escape L2 hit a simple **DRAM model**
> with a fixed latency.

You drive the simulator with a **trace**: a recorded stream of instructions
per core (one file per core). The simulator replays each core's trace
through its pipeline, accounting for every cycle a structural hazard, cache
miss, branch mispredict, or coherence stall costs you.

---

## 3. Prerequisites

| Thing                     | Why                                       |
| ------------------------- | ----------------------------------------- |
| C++20 compiler            | Builds the simulator                      |
| CMake ≥ 3.21              | Build system                              |
| Python 3                  | Sweep harness, synthetic trace generator  |
| `bash` + `curl` (macOS/Linux) | `scripts/fetch_traces.sh` for ChampSim traces |

The first CMake configure downloads three small dependencies (nlohmann/json,
CLI11, Catch2) into `build-release/_deps/`.

---

## 4. Building the simulator

The repo's `Makefile` wraps CMake so you don't need to remember the flags.

```sh
make build               # default: Debug-ish, skip if binaries already exist
make build FAST=1        # release-style: -O2 (much faster simulations)
```

Under the hood `make build` runs:

```sh
cmake -S . -B build-release [-DCASIM_FAST=ON]
cmake --build build-release -j
```

The two binaries you'll actually use end up at:

| Binary                                | Purpose                                  |
| ------------------------------------- | ---------------------------------------- |
| [build-release/src/sim](build-release/src/sim)            | The simulator itself      |
| [build-release/tools/gen_trace/gen_trace](build-release/tools/gen_trace/gen_trace) | Synthetic-trace generator (used by `make traces`) |

> **Tip.** Use `FAST=1` for any real sweep. Debug builds are 5–10× slower and
> long-tier sweeps will take all night.

For finer-grained CMake options (AddressSanitizer, profiling, treat-warnings-as-errors)
see the [README's build options table](README.md#build-options).

---

## 5. Inputs: configs and traces

A simulator run needs **two** things:

1. **A machine config** — a JSON file describing the chip
   (cores, cache geometries, FU counts, predictor type, coherence protocol, ...).
2. **Per-core traces** — one trace file per core, in
   [ChampSim binary trace format](docs/trace-format.md).

### 5.1 The machine config

The default lives at [configs/baseline.json](configs/baseline.json). Highlights:

```jsonc
{
  "cores": 4,                              // number of cores
  "core": {
    "fetch_width": 4,                      // instructions fetched per cycle
    "rob_entries": 96,                     // reorder buffer size
    "schedq_entries_per_fu": 2,            // scheduler entries per FU
    "alu_fus": 3, "mul_fus": 2, "lsu_fus": 2,
    "predictor": { "type": "yeh_patt", "history_bits": 10, "pattern_bits": 5 }
  },
  "l1": { "size_kb": 32,  "assoc": 8, "replacement": "lru", "hit_latency": 2  },
  "l2": { "size_kb": 256, "assoc": 8, "replacement": "lip", "hit_latency": 10 },
  "memory":      { "latency": 100 },
  "interconnect":{ "topology": "ring", "link_latency": 1 },
  "coherence":   { "protocol": "mesi" }
}
```

> **What this means in architecture terms.** Every line is a knob the
> hardware designer would turn. `rob_entries` caps how far ahead of the
> commit point you can look — bigger ROB = more in-flight instructions =
> more memory-level parallelism, but more area and a longer rename pipeline.
> `assoc` is set associativity (conflict-miss tradeoff). `latency` for
> memory is the DRAM round-trip in cycles you've all seen on slides as
> "~100 cycles." `protocol` picks which states the coherence FSM has
> (MI, MSI, MESI, MOSI, MOESIF — each adds states that reduce write-upgrade
> traffic in different sharing patterns).

You can override any single field on the command line without editing JSON:

```sh
./build-release/src/sim \
    --config configs/baseline.json \
    --cores 8                       \
    --protocol moesif
```

### 5.2 The traces

Three sources of trace data live under [traces/](traces/):

| Path                       | What it is                                                    |
| -------------------------- | ------------------------------------------------------------- |
| [traces/core_4/](traces/core_4/)     | A 4-core project3 coherence-regression fixture (committed in repo). |
| [traces/synth/](traces/synth/)       | Synthetic patterns (random / sequential / stream / loop) generated by `make gen-synth`. |
| [traces/champsim/](traces/champsim/) | Real SPEC-style traces, fetched on demand by `make fetch-traces`. |

> **Read [TRACES.md](TRACES.md) before reasoning about any number in
> a report.** It documents each trace's reuse %, working-set size,
> expected miss rate, and known measurement anomalies. A 100% L1 miss
> rate on `traces/core_4/` is a *property of the workload*, not a
> simulator bug — knowing that up front saves an afternoon of debugging.

Generate the synth + champsim trace bundle for a given size tier with:

```sh
make traces TIER=smoke      # ~1 s,  tiny
make traces TIER=short      # ~30 s, small
make traces TIER=medium     # ~5 min
make traces TIER=long       # ~20 min, 100 M-instruction synth
```

> **What this means in architecture terms.** A trace is the input *workload*.
> Different patterns stress different parts of the memory hierarchy:
> `sequential` is friendly to spatial-locality predictors; `stream` blows
> through cache like an iota loop; `random` defeats prefetching;
> `loop` exercises the branch predictor and stresses temporal locality.
> Mixing trace flavors across cores (see `--trace-list` below) lets you
> study how heterogeneous workloads interact through coherence.

---

## 6. Running a single simulation

The simulator binary takes a config and a trace source, and prints a summary
to stdout while writing detailed report files to disk. The Makefile wraps
this into a one-liner so you don't have to type the full CLI.

### 6.1 The easy way: `make run`

```sh
make run TRACE=traces/core_4
```

That's it. Defaults to [configs/baseline.json](configs/baseline.json), picks
up `cores` from the trace directory automatically, and writes reports to
`report/core_4_mesi_c4/`.

Common variations:

```sh
# Tag the run so re-runs don't overwrite each other
make run TRACE=traces/core_4 TAG=baseline-v1

# Try a different coherence protocol on the same trace
make run TRACE=traces/core_4 TAG=mosi PROTOCOL=mosi

# Use a custom config
make run TRACE=traces/core_4 CONFIG=configs/my_tweak.json

# Lowercase variable names work too
make run trace=traces/synth/random_small tag=v1
```

What `make run` actually does:

1. Builds [build-release/src/sim](build-release/src/sim) if it's missing.
2. Inspects `TRACE`:
   - If it's a **directory**, passes `--trace-dir` and auto-counts
     `p*.champsimtrace` files inside to set `--cores`.
   - If it's a **file**, passes `--trace-list` (treats it as a manifest).
3. Forwards `TAG` / `PROTOCOL` / `CORES` / `CONFIG` to the simulator if set.
4. Echoes the full command before executing so you can copy it for debugging.

### 6.2 The raw command

If you need to pass flags `make run` doesn't expose, run the binary directly:

```sh
./build-release/src/sim \
    --config configs/baseline.json \
    --trace-dir traces/core_4
```

This:

1. Parses `configs/baseline.json` into an in-memory `SimConfig`.
2. Opens `traces/core_4/p0.champsimtrace`, `p1.champsimtrace`, ... one per core.
3. Builds 4 OoO cores, each with private L1+L2, all hanging off a ring
   directory running MESI.
4. Ticks the global clock until every core hits trace EOF and the network
   drains.
5. Prints a short overview to stdout and writes the full reports under
   `report/<run-name>/` (path rules in [§7](#7-where-output-files-go)).

### 6.3 The CLI flags you'll actually use

| Flag             | Purpose                                                                 |
| ---------------- | ----------------------------------------------------------------------- |
| `--config FILE`  | Required. Machine config JSON.                                          |
| `--trace-dir D`  | Per-core directory; expects `D/p0.champsimtrace`, `D/p1.champsimtrace`, … |
| `--trace-list F` | Manifest file: one trace path per line. Lets you mix workloads across cores. |
| `--cores N`      | Override `cores` from the config.                                       |
| `--protocol P`   | One of `mi msi mesi mosi moesif`. Overrides `cfg.coherence.protocol`.   |
| `--tag NAME`     | Suffix appended to the report directory name (see §7).                  |
| `--mode M`       | Run a subsystem in isolation: `cache`, `predictor`, `ooo`, `coherence`. Default = full multicore. |
| `--log-level L`  | `trace debug info warn error off`. Default `info`.                      |
| `--out FILE`     | Dump the merged config to JSON (sanity check) and exit.                 |

### 6.4 Heterogeneous traces (mixing workloads)

A common experiment: run *different* workloads on different cores to see how
they interact through the coherence directory. Make a manifest:

```text
# traces/mix_4core.txt — one path per line, blank/'#' ignored.
# Relative paths are resolved against THIS FILE'S directory (traces/),
# not your shell's cwd. Absolute paths are also fine.
synth/random_tiny/p0.champsimtrace
synth/random_tiny/p1.champsimtrace
synth/stream_tiny/p0.champsimtrace
synth/stream_tiny/p1.champsimtrace
```

Then, from the repo root:

```sh
./build-release/src/sim --config configs/baseline.json \
                        --trace-list traces/mix_4core.txt
```

or via the wrapper:

```sh
make run TRACE=traces/mix_4core.txt TAG=mix
```

Manifest entry count must equal `cores`. The resolution rule
(`manifest_dir / entry`) is at
[src/full/full_mode.cpp:147](src/full/full_mode.cpp#L147) — keep it in mind
if you put the manifest somewhere other than `traces/` or write entries
relative to the repo root and they fail to open.

### 6.5 Subsystem modes

When you only want to study one piece in isolation:

| `--mode`     | What it runs                                                         |
| ------------ | -------------------------------------------------------------------- |
| `cache`      | Just the L1/L2 cache hierarchy on a single trace.                    |
| `predictor`  | Just the branch predictor.                                           |
| `ooo`        | Single-core OoO pipeline (no coherence, no other cores).             |
| `coherence`  | Multicore + coherence + caches, but with a *trivial* core (no OoO).  |
| (omitted)    | **Full multicore OoO + coherence.** This is what you want by default. |

> **What this means in architecture terms.** This is the same factoring you
> see in textbook chapters: cache hierarchy first, predictor second, OoO
> pipeline third, coherence fourth. The full mode glues them all together
> into the chip.

---

## 7. Where output files go

This is the part that's easy to get wrong. There are **three** different
destinations depending on what you're running.

### 7.1 Single-run output (what you get from `./build-release/src/sim ...`)

Every full-mode run writes a folder under [report/](report/) named:

```
report/<trace-stem>_<protocol>_c<cores>[_<tag>]/
```

where:

- `<trace-stem>` = the **last component** of `--trace-dir` (or the
  **filename without extension** of `--trace-list`)
- `<protocol>`   = `mi` / `msi` / `mesi` / `mosi` / `moesif`
- `<cores>`      = the value of `cores` after CLI overrides
- `<tag>`        = the value of `--tag` (omitted if absent)

**Concrete example.** This command:

```sh
./build-release/src/sim --config configs/baseline.json \
                        --trace-dir traces/core_4 \
                        --tag baseline
```

writes to:

```
report/core_4_mesi_c4_baseline/
├── report.rpt        ← human-readable run report (the one you read first)
├── config.rpt        ← just the configuration section
├── stats.rpt         ← detailed per-core/cache/predictor stats
├── coherence.rpt     ← coherence FSM transition counts and traffic
└── report.csv        ← machine-readable per-core row data
```

If you set the `LOG=1` environment variable, you also get:

```
report/<...>/log.rpt  ← per-instruction commit trace (first 50 dyn instructions per core)
```

So:

```sh
LOG=1 ./build-release/src/sim --config configs/baseline.json --trace-dir traces/core_4
```

writes a `log.rpt` alongside the other reports. The file format (LSU
issue events, RETIRE events, branch metadata) is documented in
[docs/log-format.md](docs/log-format.md), and a header block at the top
of `log.rpt` itself summarizes the same. Note `LOG=1` is an env var, not
a CLI flag; `--log-level` only affects what prints to stderr and is
unrelated to `log.rpt`.

### 7.2 The source of those file names

The output directory is computed in
[src/full/full_mode.cpp:705](src/full/full_mode.cpp#L705) (`build_run_dir_pre`),
and the per-file writes happen at
[src/full/full_mode.cpp:870-883](src/full/full_mode.cpp#L870-L883). Read those
~20 lines if a path looks wrong; they're authoritative.

### 7.3 Sweep output (what you get from `make smoke / short / medium / long`)

A *sweep* runs many configs against many traces in parallel. It produces
**two** kinds of artifacts:

#### (a) One per-run folder per (config × trace) combination

These live at the same `report/<trace-stem>_<protocol>_c<cores>_<tag>/` paths
as in §7.1. Each contains its own `report.rpt`, `config.rpt`, `stats.rpt`,
`coherence.rpt`, `report.csv`. The sweep tags them with the variant name
(e.g. `baseline`, `cores_2`, `cap500`).

#### (b) One sweep-wide aggregation folder

```
report/_sweep/<SWEEP_ID>/
├── summary.md          ← human-readable: violation list + caveats. Read first.
├── summary.csv         ← machine-readable: one row per run, all metrics.
├── progress.tsv        ← live progress log (status of every run as it ran)
├── configs/            ← every per-run config JSON the sweep generated
│   ├── baseline__synth_random_small.json
│   └── ...
└── logs/               ← stdout/stderr of every run + meta JSON
    ├── baseline__synth_random_small.out
    ├── baseline__synth_random_small.err
    └── baseline__synth_random_small.meta.json
```

`<SWEEP_ID>` defaults to the tier name (`smoke`, `short`, …) but you can
override it: `make short SWEEP_ID=v3` writes to `report/_sweep/v3/`.

> **What this means in architecture terms.** A sweep is a *design space
> exploration*: vary one hardware parameter at a time, hold workload
> constant, see which knob actually changes IPC. `summary.md` is where
> you'll first notice that your fancier coherence protocol shaved 3% off
> cycles, or that nothing changed because all the traces hit in L1 anyway.

---

## 8. Reading `report.rpt` like an architect

Here's an actual run report header (from `report/core_4_mesi_c4_baseline/report.rpt`):

```
 Multicore OoO Simulator -- Run Report
================================================================================
Trace                     : traces/core_4
Cores                     : 4
Protocol                  : MESI_PRO
Status                    : Simulation complete
Total cycles              : 122273
```

Then a **Configuration** section (echo of the merged config) and a **Per-core
results** section, one block per core:

```
[ Core 0 ]
  Pipeline
    cycles                    : 122274
    instructions retired      : 1000
    instructions fetched      : 1000
    IPC                       : 0.008
```

What to look at and what it tells you:

| Metric                  | Architecture meaning                                                                 |
| ----------------------- | ------------------------------------------------------------------------------------ |
| **IPC**                 | Instructions Per Cycle. Single most important number. The achievable peak is `min(fetch_width, dispatch_width, total FU count, retire_width)`; in this sim `fetch_width` caps both fetch and dispatch, and retire is unbounded, so the practical ceiling is `min(fetch_width, alu_fus + mul_fus + lsu_fus)`. |
| **CPI**                 | 1 / IPC. Easier to reason about as a sum of stalls.                                  |
| **MPKI**                | Branch Mispredictions Per Kilo-Instructions. Predictor quality.                      |
| **L1 / L2 miss rate**   | Fraction of accesses that escaped each level. Drives AAT.                            |
| **L1 / L2 AAT**         | Average Access Time = `hit_latency + miss_rate × miss_penalty`.                      |
| **coherence transitions** | Per-state FSM event counts. Tells you whether MESI's E-state actually saved upgrades vs MSI. |
| **Network traffic**     | Per-link byte counts on the ring. Stresses the interconnect-bandwidth budget.        |

> **What this means in architecture terms.** The achievable peak IPC is
> bounded by the narrowest pipeline stage and by FU availability, i.e.
> `min(fetch_width, dispatch_width, sum of FU counts, retire_width)` — not
> just by `fetch_width` alone. When measured IPC sits well below that
> ceiling, *something* is stalling the pipeline. Walk down the report:
> high MPKI → predictor; high L1 miss rate but low L2 → working set blew L1;
> high L2 miss rate → memory-bound; low miss rates everywhere but still low
> IPC → check the FU mix (instruction-type imbalance against your `alu_fus`
> / `mul_fus` / `lsu_fus`) or ROB size (in-flight cap throttling MLP).
> Coherence transitions matter when several cores write the same line:
> you'll see lots of M→I transitions (write invalidations) and the `c2c`
> transfer counts go up.

If you'd rather slurp the data into a spreadsheet, every number in
`report.rpt` is also a column in `report.csv` (and aggregated across all
runs in `report/_sweep/<SWEEP_ID>/summary.csv`).

---

## 9. Sweeps: running many configs at once

Sweeps are driven by [configs/sweep.json](configs/sweep.json), which defines
**tiers** (`smoke`, `short`, `medium`, `long`) and **axes** (which knobs to
vary). The Makefile chains them:

```sh
make smoke               # ~1 min,  tiny synth, proto axis only
make short               # ~10 min, tiny+small + champsim, all axes
make medium              # ~1 hour
make long                # overnight, 100M synth
```

Each tier shortcut runs four phases: `build → traces → sweep → aggregate`.
You can also invoke them separately:

```sh
make build FAST=1
make traces TIER=short
make sweep  TIER=short SWEEP_ID=experiment-1 JOBS=4 TIMEOUT=900
make aggregate          SWEEP_ID=experiment-1
```

`JOBS` controls parallelism; `TIMEOUT` is the per-run wallclock budget
(seconds). To preview which runs *would* execute without actually running:

```sh
make dry-run TIER=short
```

The output of any sweep ends up under `report/_sweep/<SWEEP_ID>/` (see §7.3).

> **What this means in architecture terms.** Sweeps are how you turn a
> simulator into evidence for an argument. *"MESI saves 8% over MSI on this
> workload set."* — that's a sweep, plus a `summary.csv` lookup, plus an
> honest paragraph about which workloads contributed.

---

## 10. Cleanup

The Makefile is **scoped** by `SWEEP_ID` to make it hard to nuke the wrong
thing. Four targets:

| Command                       | Removes                                                                                |
| ----------------------------- | -------------------------------------------------------------------------------------- |
| `make clean SWEEP_ID=<id>`    | `report/_sweep/<id>/` plus any per-run dir matching `report/*_<id>__*`                  |
| `make clean-reports`          | **Everything** under `report/` — sweep aggregations, sweep per-run dirs, *and* manual single-run dirs |
| `make clean-all`              | Alias for `clean-reports`                                                              |
| `make clean-traces`           | `traces/synth/` and `traces/champsim/` (regen via `make traces TIER=…`; keeps `traces/core_4/`) |

```sh
make clean SWEEP_ID=v3      # remove one sweep's artifacts (scoped, safe)
make clean-reports          # remove every report (manual + sweep)
make clean-all              # same as clean-reports
make clean-traces           # remove generated trace data (not reports)
```

`make clean` refuses to run without an explicit `SWEEP_ID` (or with
`SWEEP_ID=all`) — that's deliberate, to prevent thumb-fumble disasters
during long sweeps. Use `clean-reports` (or its alias `clean-all`) when
you really do want a clean slate under `report/`.

### 10.1 What gets cleaned

`clean-reports` deletes every entry directly under `report/` (it preserves
the `report/` directory itself, so subsequent runs can still write into
it). Concretely:

| Folder                                                | Removed by `clean-reports` / `clean-all`? |
| ----------------------------------------------------- | ----------------------------------------- |
| `report/_sweep/<id>/` *(sweep aggregation)*           | yes                                       |
| `report/loop_small_mesi_c2_v3__cores_2/` *(sweep run)*| yes                                       |
| `report/core_4_mesi_c4_baseline/` *(manual run)*      | yes                                       |

### 10.2 Recipes

```sh
# Remove just one sweep (other sweeps and manual runs untouched)
make clean SWEEP_ID=v3

# Full reset of all reports
make clean-reports

# Also reclaim the trace data (forces a re-fetch / re-gen next time)
make clean-traces
```

---

## 11. Troubleshooting

| Symptom                                                          | What's likely wrong                                                                                  |
| ---------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------- |
| `default mode requires --trace-dir DIR or --trace-list FILE`     | You passed `--trace` (single-trace flag); full mode is per-core. Use `--trace-dir` or `--trace-list`. |
| `interconnect.topology=xbar is not supported`                    | Only `ring` is implemented. Edit the config back to `ring`.                                          |
| `Status: Simulation terminated` in `report.rpt`                  | Hit the global cycle cap (`kGlobalCap` in `full_mode.cpp`); usually a coherence deadlock. The sweep's `summary.md` will flag it as `deadlock`. |
| Sweep run shows `exit -6` in `summary.md`                        | Hit the Python sweep `TIMEOUT` (wallclock, not cycles). Either raise `TIMEOUT=` or rebuild with `FAST=1`. |
| `IPC` looks too low (~0.008) on tiny traces                      | Synth traces have very low retire rates relative to memory latency. Generate larger traces or use champsim. |
| Per-run dir has no `report.rpt`                                  | Earlier crash before reports were written. Check `report/_sweep/<id>/logs/*.err` for that run.       |
| `clang: command not found` (macOS)                               | Install Xcode CLT: `xcode-select --install`.                                                         |

---

## 12. File-path cheat sheet

| What                          | Where                                                              |
| ----------------------------- | ------------------------------------------------------------------ |
| Default config                | [configs/baseline.json](configs/baseline.json)                     |
| Sweep matrix                  | [configs/sweep.json](configs/sweep.json)                           |
| Sim binary                    | `build-release/src/sim`                                            |
| Synth-trace generator         | `build-release/tools/gen_trace/gen_trace`                          |
| Committed smoke trace         | [traces/core_4/](traces/core_4/) (`p0.champsimtrace` … `p3.champsimtrace`) |
| Generated synth traces        | `traces/synth/<pattern>_<size>/p<i>.champsimtrace`                 |
| Fetched ChampSim traces       | `traces/champsim/`                                                 |
| Single-run reports (full mode) | `report/<trace-stem>_<protocol>_c<cores>[_<tag>]/report.rpt` (+ `config.rpt`, `stats.rpt`, `coherence.rpt`, `report.csv`, optional `log.rpt`) |
| Sweep aggregation             | `report/_sweep/<SWEEP_ID>/summary.md` + `summary.csv` + `progress.tsv` |
| Per-run sweep configs         | `report/_sweep/<SWEEP_ID>/configs/<variant>__<workload>.json`      |
| Per-run sweep stdout/stderr   | `report/_sweep/<SWEEP_ID>/logs/<variant>__<workload>.{out,err,meta.json}` |

That's the whole pipeline: **config + traces in, `report.rpt` and friends out.**
