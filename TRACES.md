# Trace catalog

What every trace under [traces/](traces/) actually is, what its memory
footprint looks like, and what the simulator should produce on it. Read
this **before** you panic about a 100% miss rate or a zero writeback count.

> **TL;DR.** Many of these traces are intentionally pathological. A
> `loop_*` trace has 99.8% reuse and a `sequential_*` trace has 0% reuse,
> on purpose, so we can stress different parts of the cache and coherence
> hierarchies. If a number looks "wrong," check this doc first.

## Table of contents

1. [Quick reference](#1-quick-reference)
2. [How synth traces are built (and a sharing caveat)](#2-how-synth-traces-are-built-and-a-sharing-caveat)
3. [`traces/core_4/`](#3-tracescore_4)
4. [`traces/synth/loop_*`](#4-tracessynthloop_)
5. [`traces/synth/sequential_*`](#5-tracessynthsequential_)
6. [`traces/synth/stream_*`](#6-tracessynthstream_)
7. [`traces/synth/random_*`](#7-tracessynthrandom_)
8. [`traces/champsim/`](#8-traceschampsim)
9. [Expected sweep behavior cheat sheet](#9-expected-sweep-behavior-cheat-sheet)
10. [Known anomalies](#10-known-anomalies)

---

## 1. Quick reference

Numbers below are from `*_tiny` (small) instances; bigger sizes follow the
same patterns scaled up. Reuse measured by counting unique 64 B blocks vs
total memory operations.

| Trace                        | Mem ops | Unique 64 B blocks | Reuse  | Working-set status   | Use it for                          |
| ---------------------------- | ------: | -----------------: | -----: | -------------------- | ----------------------------------- |
| `traces/core_4/p<i>`         |     458 |                458 |  0.0 % | larger than L1       | Coherence regression (project3 fixture) |
| `traces/synth/loop_tiny`     | 34 114  |                 64 | 99.8 % | **fits in L1**       | Cache-friendly baseline; protocol diff via shared writes |
| `traces/synth/sequential_tiny` | 34 039 |             34 039 |  0.0 % | far larger than L1   | Stress capacity misses, prefetcher accuracy |
| `traces/synth/stream_tiny`   | 34 042 |             34 042 |  0.0 % | far larger than L1   | Worst-case streaming, prefetcher latency |
| `traces/synth/random_tiny`   | 34 382 |             32 270 |  6.1 % | far larger than L1   | Defeats prefetchers; the only synth trace where writebacks fire |
| `traces/champsim/<spec>`     |   ~10⁹ |              varies | varies | varies (real SPEC)   | "Real" benchmark numbers            |

> **What this means in architecture terms.** Reuse is what dictates
> miss rate, working-set size dictates whether reuse can be *captured*
> by the cache, and only writes-followed-by-evictions produce writebacks.
> Each trace flavor is engineered to push exactly one of those knobs to
> an extreme.

---

## 2. How synth traces are built

[scripts/gen_synth.py](scripts/gen_synth.py) calls
[build-release/tools/gen_trace](tools/gen_trace) **four times per
(pattern × size)** — once per core — with a distinct seed and a distinct
`--addr-base` for each core, producing four independent `.champsimtrace`
files with disjoint working sets:

```
traces/synth/sequential_tiny/
├── p0.champsimtrace     ← seed=S+0, addr_base=B+0  TB
├── p1.champsimtrace     ← seed=S+1, addr_base=B+1  TB
├── p2.champsimtrace     ← seed=S+2, addr_base=B+2  TB
└── p3.champsimtrace     ← seed=S+3, addr_base=B+3  TB
```

**Why per-core distinct streams.** An earlier version of `gen_synth.py`
generated a single `raw.champsimtrace` and symlinked it as `p0..p3`. All
four cores then executed the byte-identical instruction stream over the
byte-identical addresses, which made the workload *fully shared* (no
coherence protocol could differ from any other) and unrealistic — real
multithreaded workloads diverge through branch outcomes, store mixes,
and per-thread data partitioning. The current layout fixes that.

**Consequence for protocol comparisons:** all `synth/*` traces are now
**private** — no two cores share an address — so MI/MSI/MESI/MOSI/MOESIF
should produce nearly-identical IPC. Cross-protocol differences in
synth sweeps are statistical noise, not protocol effects.

If you need a workload that exercises shared-address coherence, build a
manifest that points two or more cores at the same trace file and run
with `--trace-list`:

```text
# traces/heterogeneous_4core.txt
traces/synth/loop_tiny/p0.champsimtrace
traces/synth/loop_tiny/p0.champsimtrace      ← intentional repeat: cores 0 and 1 share
traces/synth/random_tiny/p0.champsimtrace
traces/synth/sequential_tiny/p0.champsimtrace
```

```sh
make run TRACE=traces/heterogeneous_4core.txt TAG=mix
```

---

## 3. `traces/core_4/`

A 4-file fixture inherited from a Georgia Tech course's project 3
(`dirsim` reference). Each `p<i>.champsimtrace` is a tiny per-core trace
recorded for that assignment.

| Property               | Value                                        |
| ---------------------- | -------------------------------------------- |
| Files                  | `p0`, `p1`, `p2`, `p3` (4 distinct files)    |
| Size                   | 64 KB each, ~1 K instructions per core       |
| Mem ops per core       | ~458                                         |
| Unique 64 B blocks     | ~458 (every access touches a fresh block)    |
| Reuse                  | **0 %**                                      |
| Working set            | Larger than L1                               |

**What you should see on this trace:**

- L1 miss rate ≈ **100 %** (this is the workload's property, not a bug).
- L2 miss rate ≈ **100 %** (no temporal locality, so L2 doesn't help).
- IPC ≈ **0.008** (memory-bound; 100-cycle DRAM dominates).
- Coherence transitions: present but *limited* — the four cores have
  largely disjoint addresses, so directory traffic is sparse.
- Writebacks: small but non-zero on writes that hit (rare).

**Why it's still in the repo:** the bit-for-bit regression suite at
[tests/coherence/](tests/coherence/) pins it against the four reference
outputs (`MSI_core_4.out`, `MESI_core_4.out`, `MOSI_core_4.out`,
`MOESIF_core_4.out`). Don't delete it — `make test` will break.

---

## 4. `traces/synth/loop_*`

Tight loop over a small address window — per core. With per-core
distinct streams, each core has its own private 4 KB hot loop.

| Property                      | Value (`loop_tiny`, per core)         |
| ----------------------------- | ------------------------------------- |
| Mem ops                       | ~34 000                               |
| Unique 64 B blocks (per core) | **64** (a single L1 set's worth)      |
| Reuse                         | **99.8 %**                            |
| Working set (per core)        | **Fits trivially in L1** (4 KB ≪ 32 KB) |
| Sharing                       | **None** — each core's loop is in its own 1 TB-offset region |

**What you should see:**

- L1 miss rate ≈ **0.2 %** — only the cold misses fill the 64 blocks.
- L2 miss rate ≈ 1.0 (cold-fills go to memory; L2 doesn't help because
  L1 absorbs reuse).
- IPC ≈ **0.9–1.1**. Same across all 5 protocols (no sharing → coherence
  is a no-op).
- Writebacks = **0**. Working set fits → no evictions → no writebacks. ✓

**Use it to:** validate that the OoO + L1 happy-path works (high IPC,
low miss rate). Not useful for protocol comparison — sharing is needed
for that, and this trace has none by construction.

---

## 5. `traces/synth/sequential_*`

Linear address walk, monotonically increasing. Per-core distinct
addr_base offsets keep each core's walk in its own region.

| Property                      | Value (`sequential_tiny`, per core)   |
| ----------------------------- | ------------------------------------- |
| Mem ops                       | ~34 000                               |
| Unique 64 B blocks (per core) | ~34 000                               |
| Reuse                         | **0 %**                               |
| Working set (per core)        | ~2.2 MB — many ×L1 capacity           |
| Sharing                       | **None** — disjoint 1 TB-offset regions |

**What you should see:**

- L1 miss rate ≈ **100 %** (no reuse, capacity blown).
- L2 miss rate ≈ **100 %** (same reason).
- IPC ≈ **0.008** (memory-bound).
- L1 writebacks: **non-zero** (~30 K per core); L2 writebacks: a few
  hundred (only the dirty L1 evictions that fill an already-clean L2
  spot get re-dirtied).
- Same numbers across all 5 protocols (no sharing → coherence is a
  no-op).

**Use it to:** measure prefetcher accuracy and capacity-miss behavior.

---

## 6. `traces/synth/stream_*`

Like `sequential_*` but with stride > 1. Stresses pure streaming.

| Property                      | Value (`stream_tiny`, per core)       |
| ----------------------------- | ------------------------------------- |
| Mem ops                       | ~34 000                               |
| Unique 64 B blocks (per core) | ~34 000                               |
| Reuse                         | **0 %**                               |
| Working set (per core)        | Larger than L2                        |
| Sharing                       | **None**                              |

**What you should see:**

- L1 / L2 miss rate ≈ **100 %**.
- IPC ≈ **0.008**.
- L1 writebacks: **non-zero**, similar to sequential.
- Same numbers across all 5 protocols.

**Use it to:** worst-case bandwidth analysis, ring-link saturation tests.

---

## 7. `traces/synth/random_*`

Uniform-random address mix; some hits, mostly misses.

| Property                      | Value (`random_tiny`, per core)         |
| ----------------------------- | --------------------------------------- |
| Mem ops                       | ~34 000                                 |
| Unique 64 B blocks (per core) | ~32 000                                 |
| Reuse                         | **6.1 %**                               |
| Working set (per core)        | 16 MB random window (gen_trace.cpp:71)  |
| Sharing                       | **None** — per-core RNG, per-core base  |

**What you should see:**

- L1 miss rate ≈ **99.8 %**, L2 miss rate ≈ **98.5 %**.
- IPC ≈ **0.008** (still memory-bound, but a smidge of reuse).
- L1 writebacks: **non-zero** (~30 K per core).
- Prefetcher: useless here (random pattern defeats next-line and Markov).

**Use it to:** validate that writeback paths work, sanity-check prefetcher
accuracy.

---

## 8. `traces/champsim/`

Real SPEC-style ChampSim traces fetched on demand by
[scripts/fetch_traces.sh](scripts/fetch_traces.sh).

| Property               | Value                                        |
| ---------------------- | -------------------------------------------- |
| Files                  | One per benchmark, named by `bzip2`, `mcf`, etc. |
| Size                   | Hundreds of MB each (compressed)             |
| Reuse                  | Realistic — varies by benchmark              |
| Working set            | Realistic                                    |

**What you should see:**

- Behavior depends entirely on the benchmark. SPEC `mcf` is pointer-chasing
  (memory-bound, low IPC, lots of L2 misses); `bzip2` is computation-heavy
  with predictable access (high IPC, predictor matters).
- IPC range: anywhere from 0.3 (pointer chasing) to 2.5+ (compute-bound).
- Writebacks: realistic.

**Use it to:** report numbers to anyone who isn't running this simulator.

---

## 9. Expected sweep behavior cheat sheet

When you stare at `report/_sweep/<id>/summary.md`, the table below is what
each trace family **should** look like under MESI (the default):

| Trace family    | L1 miss rate | L2 miss rate | IPC band       | L1 writebacks | Protocol matters? |
| --------------- | ------------ | ------------ | -------------- | ------------- | ----------------- |
| `core_4`        | ~1.00        | ~1.00        | ~0.008         | small > 0     | yes (some sharing in fixture) |
| `loop_*`        | ~0.002       | ~1.00 (cold) | ~0.9–1.1       | **0** (fits in L1) | no (private per core) |
| `sequential_*`  | ~1.00        | ~1.00        | ~0.008         | ~30 K / core  | no (private per core) |
| `stream_*`      | ~1.00        | ~1.00        | ~0.008         | ~30 K / core  | no (private per core) |
| `random_*`      | ~1.00        | ~0.98        | ~0.008         | ~30 K / core  | no (private per core) |
| `champsim/*`    | varies       | varies       | 0.3–2.5        | varies        | varies            |

If a number is **two orders of magnitude** off this table, it's probably a
bug. If it's within a factor of 2, it's probably real workload variance.

---

## 10. Known anomalies and resolved bugs

### 10.1 [RESOLVED] `sequential_*` / `stream_*` showed zero writebacks

**Was:** all five protocols on `synth/sequential_tiny` and
`synth/stream_tiny` reported L1 writebacks = 0 despite 34 K
unique-block evictions on a 32 KB L1.

**Root cause:** the coherence adapter filled lines clean even when the
miss was caused by a store. The fill site at
[coherence_adapter.cpp](src/coherence/coherence_adapter.cpp) called
`cache_fill(..., 'R')` unconditionally, so dirty bits never got set on
store-miss fills, and evictions went silent.

**Fix:** [coherence_adapter.cpp + cache.cpp + ooo/core.cpp]. Threaded
`originating_op` through `MemReq` so when L1 forwards a store miss to
L2 (as `Op::Read` for write-allocate), L2 can still tell the coherence
sink that the fill is owed to a store. The adapter tracks pending
store misses in `pending_stores_` and fills L1 dirty when the response
arrives. Verified: `sequential_tiny` now produces ~30 K L1 writebacks
per core; project3 regression tests still pass bit-for-bit.

### 10.2 [RESOLVED] `proto_invariance_private` warnings on synth traces

**Was:** smoke sweeps fired warnings with 47-50% IPC spread between
protocols on `synth/sequential_*` and `synth/random_*` — and MI was
*faster* than MESI, which is paradoxical.

**Root cause:** `gen_synth.py` generated one `raw.champsimtrace` and
symlinked it as `p0..p3`, so all 4 cores executed byte-identical
streams over byte-identical addresses. The traces were fully-shared,
not private; on heavy sharing MI's "yank-exclusive" wins over MESI's
S→M upgrade chains.

**Fix:** [gen_synth.py]. Generates 4 distinct trace files per
(pattern × size) with per-core seed offsets and per-core 1 TB
`addr_base` strides. Synth traces are now genuinely private. Smoke
sweep IPC spread on private traces dropped from 47.8% to <1%; the
proto_invariance_private rule's tolerance was bumped from 1% to 5% to
absorb per-core RNG noise on tiny traces.

### 10.3 MI tail latency on shared workloads (now mostly moot)

On the old fully-shared synth layout, MI could take 200+ seconds vs.
~5 seconds for the other protocols, sometimes hitting the 30-min
Python wallclock timeout on `short`/`medium` tiers. With the new
per-core distinct streams MI runs in ~6 seconds across the smoke
tier — the network ping-pong pathology is gone for synth workloads.
For real shared-coherence stress (`tests/coherence/fixtures/proj3/`)
the MI tail-latency profile is unchanged. See
[report_doc/11-validation-bugs.md:328](report_doc/11-validation-bugs.md#L328)
for the full historical write-up.

### 10.4 [TODO] No shared-coherence synth trace family

Now that synth is fully private, sweeps don't exercise the
shared-line coherence path at all. The only way to stress
shared-line coherence today is the project3 fixture in
[tests/coherence/](tests/coherence/) or a hand-written
`--trace-list` manifest pointing two cores at the same trace file
(see [§2](#2-how-synth-traces-are-built)). A future
`gen_synth_shared.py` (or a `--shared` flag on the existing
generator) would close this gap.

---

## Cross-references

- [RUNNING.md](RUNNING.md) — how to run a simulation in the first place.
- [docs/trace-format.md](docs/trace-format.md) — binary layout of `.champsimtrace`.
- [docs/tracing.md](docs/tracing.md) — how the DynamoRIO tracer is supposed to work.
- [report_doc/11-validation-bugs.md](report_doc/11-validation-bugs.md) — the long-form bug log.
- [report_doc/13-log-mode-and-rpt-split.md](report_doc/13-log-mode-and-rpt-split.md) — the original 100%-miss-rate investigation that produced most of the numbers in this doc.
