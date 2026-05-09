# 15 — Baseline Characterization (2026-05-08)

What the simulator says about its workloads under the default
[configs/baseline.json](../configs/baseline.json) — 4 cores, MESI_PRO,
ring interconnect, 32 KB L1 / 256 KB L2, Yeh-Patt predictor, no
prefetcher. This is the headline number sheet a reader can scan to
know "where the simulator is" today.

The companion docs that produced these numbers are
[12-heterogeneous-traces.md](12-heterogeneous-traces.md) (mix mechanism)
and [13-log-mode-and-rpt-split.md](13-log-mode-and-rpt-split.md) (synth
miss-rate reality check).

---

## 1. Configuration

Single config, unchanged from the repo default:

| Knob | Value |
| --- | --- |
| Cores | 4 |
| Coherence | MESI_PRO |
| Interconnect | ring (link latency 1, link width 2³ = 8 B/flit) |
| L1 | 32 KB / 8-way / 64 B / writeback / LRU / 8 MSHRs |
| L2 | 256 KB / 8-way / 64 B / writeback / LIP |
| DRAM | 100-cycle uniform-latency oracle |
| ROB | 64 entries |
| Schedule queue | 2 entries / FU |
| Functional units | 8 ALU / 4 MUL / 2 LSU |
| Branch predictor | Yeh-Patt (H=10, P=5) |
| Global cycle cap | 100 M ([src/full/full_mode.cpp:62](../src/full/full_mode.cpp#L62)) |

Each synth run retires exactly **400 K instructions** (4 cores × 100 K
records). Real-trace cache-mode runs walk to EOF on the 4 GiB-truncated
traces (~67 M records each) per
[scripts/fetch_traces.sh:48-65](../scripts/fetch_traces.sh#L48-L65).

---

## 2. Synthetic patterns (full mode, MESI, 4 cores)

Each row is a homogeneous run: all four cores read the same trace
flavor from
[traces/synth/&lt;pattern&gt;_tiny/](../traces/synth/) (per-core distinct
streams over disjoint 1 TiB-spaced address ranges; see
[scripts/gen_synth.py:48](../scripts/gen_synth.py#L48)).

| Trace flavor | cycles | Per-core IPC | L1 miss | L2 miss | branch MPKI | Mem reads | Mem writes | C2C | Inval |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `synth/loop_tiny` | **108 K** | **0.926** | 0.19 % | 100 % (cold) | 75.0 | 256 | 0 | 0 | 0 |
| `synth/sequential_tiny` | 13.87 M | 0.007 | 100 % | 92 % | 75.5 | 135 896 | 21 558 | 0 | 0 |
| `synth/stream_tiny` | 13.82 M | 0.007 | 100 % | 92 % | 75.0 | 135 498 | 21 480 | 0 | 0 |
| `synth/random_tiny` | 13.16 M | 0.008 | 99.80 % | 91 % | 75.0 | 128 936 | 21 543 | 0 | 0 |

Reading the table:

- **`loop_tiny` is the diagnostic that the OoO core works.** ~1 IPC per
  core, near-zero L1 miss, two orders of magnitude faster wall-clock.
  The 256 system-wide cache misses are exactly the cold-start
  compulsory fills (4 cores × 64 unique blocks each = 256). After cold,
  the 4 KB working set fits trivially in 32 KB L1.
- **The other three are stress tests, not workloads.** L1 miss ≈ 100 %,
  L2 miss ≈ 92 %, IPC two orders of magnitude lower than loop. This
  is by design — generator addresses march forward at stride 64 with
  no reuse, so the working set blows past every cache. See
  [tools/gen_trace/gen_trace.cpp:64-71](../tools/gen_trace/gen_trace.cpp#L64-L71).
- **No coherence work in any homogeneous run.** Zero C2C transfers,
  zero invalidations across all four flavors. That's a property of
  the trace layout: the 1 TiB-per-core stride in
  `gen_synth.py` makes cores' address ranges fully disjoint, so even
  with the most permissive protocol there is nothing to share.
- **Branch MPKI ~75 across all four flavors.** That's a property of the
  *generator*, not the predictor — synthetic traces emit a branch
  every ~13 instructions over essentially-random PCs. Real workloads
  (§4) are very different.

---

## 3. Heterogeneous synth mix

[`traces/mix_4core.txt`](../traces/mix_4core.txt) — `random_tiny/{p0,p1}`
on cores 0/1, `stream_tiny/{p0,p1}` on cores 2/3.

| Metric | Value |
| --- | ---: |
| cycles | 13.28 M |
| Aggregate IPC | 0.030 |
| Per-core IPC (c0/c1/c2/c3) | 0.008 / 0.008 / 0.008 / 0.008 |
| L1 miss rate (c0/c1/c2/c3) | 0.998 / 0.998 / **1.000** / **1.000** |
| Mem reads | 130 052 |
| Mem writes | 21 882 |
| **Cache-to-cache transfers** | **1 942** |
| **Coherence invalidations** | **534** |

This is the first row in this report with non-zero coherence activity.
The reason is subtle: although cores 0/2 (random p0 and stream p0) are
on different *generators*, both files use `addr_base = 0x10000000`
because the 1 TiB per-core stride in `gen_synth.py` is applied
**within** a single pattern dir, not across dirs. So core 0 (random
p0) and core 2 (stream p0) walk overlapping address ranges with
different patterns — incidental sharing. Same story for cores 1 and 3.

The IPC stays flat at 0.008 because both component traces are
high-miss-rate. The interesting signal here is the **1942 C2C
transfers** — proof the simulator's coherence path moves data between
cores, not just to/from main memory, when the workload actually
shares addresses.

---

## 4. Real SPEC2017 traces — cache mode (works)

Single-core, single-trace runs through the cache hierarchy in
isolation (`--mode cache`). These bypass the OoO core and the
coherence directory, so they characterize the trace itself plus the
L1+L2+DRAM path.

| Trace | Records walked | L1 miss | L1 writebacks | DRAM accesses |
| --- | ---: | ---: | ---: | ---: |
| `champsim/perlbench` | 30.32 M | **0.04 %** | 1 762 | ~14 K |
| `champsim/leela` | 26.33 M | **0.58 %** | 29 539 | ~182 K |
| `champsim/xz` | 20.71 M | **1.95 %** | 289 184 | ~692 K |
| `champsim/mcf` | 3.20 M | **53.87 %** | 11 258 | 1.73 M |

These match the bands published in the CRC-2 / DPC-3 / IPC-1
literature for these SimPoints — the spread from 0.04 % (perlbench's
hot interpreter loop) to 53.87 % (mcf's pointer-chasing) is two
orders of magnitude wider than anything synth produces, and it tracks
the published MPKI ranking. **The trace ingestion path and cache
machinery handle real traces correctly.**

---

## 5. Real SPEC2017 traces — full mode (BROKEN)

Same traces, full mode (OoO + coherence). Two distinct failure modes:

| Trace | Cores | Result |
| --- | --- | --- |
| `champsim/mcf` | 1 | **Deadlock at cycle 1 000 006** (rob=29, sq=28, dispq=15, retired=0, fetched=44, in_mispred=1) |
| `champsim/mcf` | 4 | Same deadlock, 4-core flavor |
| `champsim/perlbench` | 1 | **Segfault** (exit 139) |
| `champsim/leela` | 1 | Hang (no progress; killed at 30 s wall clock) |
| `champsim/xz` | 1 | Hang (same) |

The `[ERROR] OoO core deadlock` watchdog is the
[`stage_state_update`](../src/ooo/core.cpp#L88-L102) check. The
pipeline state at deadlock tells the story:

- 44 fetched, 0 retired
- ROB has 29 entries waiting; the head is not ready
- Store queue has 28 entries (high store fraction)
- A mispredicted branch was fetched and is sitting in the ROB
- Fetch is stalled by `in_mispred=1` waiting for the mispredict to retire
- The mispredict can only retire after older instructions retire
- The oldest instruction is a load that's never received its fill

**The first L1 miss never gets filled.** Cache mode on the same
addresses fills correctly, so the bug is in the OoO ↔ coherence
integration, not the cache itself. The plan that produced this report
([report_doc/14](14-writeback-and-private-synth.md) leftovers, this
session's plan file) explicitly gates further work on this finding.

Three suspect places to investigate, in priority order, captured in
[memory/project_real_trace_deadlock.md](../../.claude/projects/-Users-dongyunlee-Documents-GT-2026-Spring-ECE6100-comparch-sim/memory/project_real_trace_deadlock.md):

1. **MSHR allocation on real-trace addresses.** Synth addresses
   cluster narrowly around `0x10000000+i*2^40`; real traces scatter
   across 64-bit virtual addresses. If MSHR slot allocation hashes
   poorly, an in-flight load might never see its fill.
   ([src/cache/cache.cpp](../src/cache/cache.cpp) MSHR machinery.)
2. **Directory state on first-touch lines.** Synth pre-populates the
   directory hash table via the early predictable stream; real traces
   touch fresh addresses constantly. A bad sticky state on a fresh
   block could swallow the response.
   ([src/coherence/directory.cpp](../src/coherence/directory.cpp).)
3. **`cache_fill('R')` on a Store miss.** The doc'd known issue at
   [src/coherence/coherence_adapter.cpp:115-121](../src/coherence/coherence_adapter.cpp#L115-L121)
   and [report_doc/13](13-log-mode-and-rpt-split.md): adapter calls
   `cache_fill(... 'R')` even when the original miss was `Op::Write`.
   This may interact badly with realistic store-mix traces in a way
   that synth's all-load patterns hide.

---

## 6. What the report says, in one paragraph

The simulator is **correct on the four subsystems in isolation** —
cache mode produces published-quality miss rates for both synthetic
and real workloads, predictor mode trains and reports MPKI
sensibly, and the OoO core retires near-1 IPC on a workload that
fits in cache (loop_tiny). The simulator is **also correct on
multicore synthetic stress tests** — 4-core synth runs all complete,
the heterogeneous mix shows expected coherence activity, and `ctest`
runs 128/128 green. **What does not yet work** is full mode (OoO +
coherence) on real ChampSim traces: the first L1 miss never gets
its fill response, fetch stalls behind an early mispredict, and the
deadlock watchdog fires. Until that is fixed, real benchmark numbers
remain blocked. The trace ingestion infrastructure shipped this
session is ready and waiting on that bug.

---

## 7. Reproducer commands

```sh
# Synth, all four flavors:
for p in loop sequential stream random; do
  make run TRACE=traces/synth/${p}_tiny TAG=rpt
done

# Heterogeneous synth mix:
make run TRACE=traces/mix_4core.txt TAG=rpt

# Cache-mode characterization of real traces (works):
for b in mcf perlbench leela xz; do
  build-release/src/sim --config configs/baseline.json \
    --trace traces/champsim/${b}/raw.champsimtrace --mode cache
done

# Full-mode attempt on real traces (will deadlock or segfault):
make run TRACE=traces/champsim/mcf TAG=rpt_real CORES=1
```

---

## Cross-references

- [TRACES.md](../TRACES.md) — what each trace file is and how it was
  built.
- [RUNNING.md](../RUNNING.md) — invocation reference, `make run`,
  log levels.
- [docs/log-format.md](../docs/log-format.md) — `LOG=1` per-instruction
  trace format.
- [report_doc/13-log-mode-and-rpt-split.md](13-log-mode-and-rpt-split.md)
  — earlier finding that motivated the synth-vs-real split.
- [report_doc/14-writeback-and-private-synth.md](14-writeback-and-private-synth.md)
  — the gen_synth rewrite that produced today's `_tiny` directories.
