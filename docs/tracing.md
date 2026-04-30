# Tracing — getting per-core traces into the simulator

Companion to [trace-format.md](trace-format.md). That doc specs the
**on-disk** ChampSim binary format. *This* doc is about **where the
traces come from** — i.e., how do you turn a real multithreaded
program into the per-core `p<i>.champsimtrace` files the simulator
expects?

The answer today is partially "we generate synthetic traces with
`tools/gen_trace`." The real-program path is the largest remaining
piece of unfinished work in the repo (Phase 1 leftover from
[../../plan.md](../../plan.md)).

---

## Status

| Piece | Status | Notes |
| ----- | -----: | ----- |
| ChampSim binary reader / writer | ✅ done | `src/common/trace.cpp`, `Variant::Standard` |
| Synthetic trace generator       | ✅ done | `tools/gen_trace/` — patterns: sequential / loop / stream / random |
| Per-core trace convention       | ✅ done | `<dir>/p<i>.champsimtrace` for core `i` |
| Real DBI tracer                 | ❌ missing | DynamoRIO `drmemtrace` client + converter |
| Public corpus fetcher           | ❌ missing | `scripts/fetch_traces.sh` for DPC-3 / IPC-1 |
| End-to-end matmul validation    | ❌ missing | own pthreads workload under `workloads/` |
| Pin-based ChampSim tracer       | ❌ missing | optional, for SPEC re-tracing on Pin platforms |

---

## How the simulator consumes traces today

Every multi-core mode is given a directory and looks for files by
filename convention:

```
<trace_dir>/
├── p0.champsimtrace      # core 0's instruction stream
├── p1.champsimtrace      # core 1's instruction stream
├── ...
└── p<N-1>.champsimtrace
```

The relevant code is small — see
[src/full/full_mode.cpp:148-156](../src/full/full_mode.cpp#L148-L156).
Each core gets its own `trace::Reader` bound to its file, and the OoO
core walks its trace top-to-bottom independently. There is **no
runtime workload distribution**; the simulator does not know or care
that the streams might have come from one program. It only knows the
filename → core mapping.

This mirrors:
- `project3_v1.1.0/`'s `traces/core_N/p<i>.trace` layout.
- ChampSim's standard multi-core convention
  (`champsim_trace_<workload>_p<i>.gz`).

---

## What "running a real program on N cores" requires

Five pieces, in order:

### 1. The program

A multithreaded source program with N threads doing real work — pthreads
matmul / strassen / parallel reduction, an OpenMP loop, or a SPEC /
PARSEC benchmark. The plan calls out a hand-written pthreads matmul
under `workloads/matmul/` as the "first real workload."

### 2. Dynamic Binary Instrumentation (DBI) capture

A tool that runs the program and records every executed instruction
per thread. Two viable choices:

- **DynamoRIO + `drmemtrace`** — recommended in
  [../../plan.md §3.1](../../plan.md). Cross-platform, actively
  maintained, captures instruction PCs + memory addresses + branch
  outcomes. The output is a per-thread directory of `.gz`-compressed
  binary records.
- **Pin + ChampSim's bundled tracer** (`tracer/champsim_tracer.cpp`).
  Linux + x86 only. Works but Pin's age is showing.

Both produce per-thread streams, each containing the actual
instruction trace for that thread of the running program.

### 3. The converter

DBI output is in the tool's native format (drmemtrace's records,
Pin's structs, etc.). Need to convert each per-thread stream into a
ChampSim binary record stream. Plan calls this
`tools/tracer/drmem2champsim/`.

The converter's job per record:
- Decode the captured instruction.
- Classify it: ALU / LOAD / STORE / BRANCH (matches our `Opcode` enum).
- Pack it into a 64-byte ChampSim `Record` (see [trace-format.md](trace-format.md)).
- Write to the target per-thread `.champsimtrace` file.

### 4. Thread → core mapping

Trivial when threads ≤ cores: thread `i` → core `i`, files named
`p<i>.champsimtrace`. Default convention; matches what the simulator
expects.

If threads > cores, you'd need a scheduler (round-robin, affinity,
gang scheduling, etc.) and the converter would emit one `.champsimtrace`
per *core*, not per *thread*. This is rarely needed for sim work.

### 5. Drop into the simulator

```sh
./build/src/sim --config configs/baseline.json --cores N \
  --trace-dir <output_dir>/
```

That's it. The simulator runs the captured workload exactly as it
would synthetic traces.

---

## Why this matters for what the sim measures

Until step 5 is real, the simulator is exercising **independent
synthetic streams**, not actual cooperating threads. Some consequences
visible in [report/09-config-sweep.md](../report/09-config-sweep.md):

- **Coherence protocols only differentiate when traces actually share
  addresses.** With 4 cores on disjoint synthetic ranges, MI / MSI /
  MESI / MOSI / MOESIF give identical numbers (no sharing → no
  coherence work). Real programs have producer-consumer patterns,
  shared structures, locks — that's where MESI's E-state and
  MOESIF's F-state earn their cycle savings.
- **Scaling curves reflect the simulator's bottlenecks, not the
  program's.** Sweep 1's "aggregate IPC saturates at ~0.04 by 4
  cores" is a real microarchitectural finding (single ring, single
  directory, single DRAM). But on a real workload, you'd *also* see
  the program's own contention overhead (lock waits, false sharing,
  synchronization cost).
- **Stat counters wired only against synthetic patterns may be
  incomplete.** `Memory Writes = 0` across every config in 5B is
  partially because [CoherenceAdapter::tick](../src/coherence/coherence_adapter.cpp)
  fills L1 with `rw='R'` regardless of the original op. A real
  store-heavy workload would expose this faster than synthetic
  store-heavy traces happen to.

---

## TODO checklist

Tracked under Phase 1 leftover. Pick these up after the simulator
engine settles.

- [ ] **Documenting the DynamoRIO setup.** Install steps for macOS
      (drmemtrace works on macOS-arm64 with caveats) and Linux. A
      "hello world" capture: trace `int main() { puts("hi"); }`,
      pipe through the converter, simulate. Probably 200 lines of
      docs, mostly pasted from upstream.
- [ ] **`tools/tracer/drmem2champsim/`** — the converter. C++ tool
      that reads drmemtrace's per-thread output and emits ChampSim
      binary. Probably 500–800 LOC; the bulk is opcode classification.
- [ ] **`workloads/matmul/`** — own pthreads matmul, ~50 LOC. The
      validation target: capture 4-thread matmul, simulate on 4
      cores, observe coherence traffic across the shared output
      matrix.
- [ ] **`scripts/fetch_traces.sh`** — pull a few ~10 MB traces from
      DPC-3 / IPC-1 corpora into `traces/` for sanity runs without
      DBI overhead. Useful for benchmarking the simulator itself.
- [ ] **Optional: vendor Pin-based ChampSim tracer** under
      `tools/tracer/pin_champsim/`. Pin is Linux/x86-only but is
      what most published trace corpora were captured with.
- [ ] **Once the pipeline works:** rerun the
      [config sweep](../report/09-config-sweep.md) on real
      multi-threaded workloads. Coherence protocols should show
      *real* differentiation, not just the synthetic sharing-only
      result from Sweep 3.

---

## Learning resources

- **DynamoRIO drmemtrace docs:**
  https://dynamorio.org/page_drcachesim.html
  — what records look like, how to write a custom client.
- **ChampSim itself:** https://github.com/ChampSim/ChampSim
  — the trace format's reference implementation.
- **Public ChampSim trace corpora (already captured, drop-in usable):**
  - DPC-3 (data-prefetcher comp): https://hpca23.cse.tamu.edu/champsim-traces/
  - CRC-2 (cache replacement comp): same hosting.
  - IPC-1 (instruction-prefetcher comp): same hosting.
- **A friendly intro to DBI** for context:
  https://www.usenix.org/legacy/event/usenix05/tech/general/full_papers/luk/luk_html/
  ("Pin: Building Customized Program Analysis Tools with Dynamic
  Instrumentation," PLDI 2005). Older but the concepts haven't moved.

---

## Notes for future-me

The simulator engine being feature-complete is the easy part —
generating *meaningful* traces is the long pole on every academic
sim project. Don't underestimate how much time the DBI plumbing
takes. A reasonable order:

1. Get a single-threaded "hello world" trace working end-to-end on
   your own machine first. Just one core, one short trace, one
   simulator run that exits 0. That validates the toolchain.
2. Then a 2-thread pthreads program. Verify both threads' traces
   are captured separately. Verify the simulator runs both. Compare
   `coherence_invals` count to "is there real sharing?"
3. Then a real matmul or stencil — something with predictable
   sharing patterns. This is where you start collecting interesting
   results plots.
4. Public corpus traces last — they're a known-good baseline but
   uninstructive about your particular workload.
