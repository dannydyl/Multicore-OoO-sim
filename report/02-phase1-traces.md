# Phase 1 — Trace format and tracer

**Goal:** decide on one trace format that the rest of the simulator
speaks, build the reader/writer for it, and lay out (but not yet
implement) the path to capturing real-workload traces from binaries.

This phase is partly a *decision* phase. Most of the work is in writing
down a trace strategy that lasts the rest of the project, not in
producing code.

---

## Why does a simulator need traces?

A simulator that takes "code" as input has two options:

1. **Execute the code itself.** Like `gem5` in syscall-emulation or
   full-system mode: hand the simulator a real binary, the simulator
   executes every instruction and reports timing. This is the gold
   standard for accuracy but requires implementing an entire ISA, system
   calls, virtual memory, the works. Months of work, and slow at
   runtime.
2. **Replay a recorded trace.** Pre-record a list of "instruction X had
   PC P, was a branch, taken=true, accessed memory address A" and just
   simulate the timing of that fixed list. Doesn't care what ISA the
   trace came from, doesn't need a syscall layer, runs ~100× faster.
   The downside: it's open-loop — the simulator can't change the
   program's behavior, so it can't model speculation that goes down a
   wrong path and recovers.

Trace-driven is the dominant approach in research-grade
microarchitectural simulators (ChampSim, zsim, Sniper, Scarab). It's
what `Multicore-OoO-sim` does too.

So we need:

- a **trace format** — what fields each instruction record carries.
- a **trace source** — where the records come from (recording from a
  real run vs. synthesizing them).

---

## The format decision: ChampSim binary

We adopted **ChampSim binary trace format** (`.champsimtrace.xz`) as
the canonical on-disk format for the entire simulator.

### What the format looks like

One record per instruction, fixed-size C struct, packed, little-endian:

| Field                      | Width    | Purpose                                            |
| -------------------------- | -------- | -------------------------------------------------- |
| `ip`                       | u64      | instruction pointer (PC)                           |
| `is_branch`                | u8       | 1 if this instruction is a branch                  |
| `branch_taken`             | u8       | 1 if branch was actually taken (else 0)            |
| `destination_registers[N]` | u8 × N   | up to N register IDs this instruction writes       |
| `source_registers[M]`      | u8 × M   | up to M register IDs this instruction reads        |
| `destination_memory[K]`    | u64 × K  | up to K data addresses this instruction stores to  |
| `source_memory[L]`         | u64 × L  | up to L data addresses this instruction loads from |

There are two record-shape variants in the wild:

- `input_instr` — N=2, M=4, K=2, L=4. ~64 bytes per record. Used by
  SPEC, CRC-2, DPC-3, and most of the academic corpus.
- `cloudsuite_instr` — N=4, M=4, K=1, L=2. ~56 bytes per record. Used
  by IPC-1 server traces.

Our reader/writer support both behind a config flag — see
[include/comparch/trace.hpp](../include/comparch/trace.hpp) for the
field constants and [src/common/trace.cpp](../src/common/trace.cpp) for
the I/O.

### What the format leaves out, and why we accept the cost

- **No opcode / instruction class.** The format doesn't say "this is an
  ALU op vs. a multiply vs. an FP op". Simulators that need this
  reconstruct it by binning PCs heuristically, or treat every non-mem,
  non-branch op as a generic ALU. ChampSim itself does the latter.
- **No memory access size.** Every load/store is assumed to be 1 cache
  block. Fine for cache modeling — caches don't model sub-block
  accesses anyway.
- **No untaken-branch fallthrough target.** Computable from the next
  record's PC.
- **No header, no checksum, fixed register counts.**

These limitations are tracked in
[ChampSim issue #382](https://github.com/ChampSim/ChampSim/issues/382).
A v2 format with opcodes and explicit sizes is planned upstream; when
it lands we'll follow.

### Why this format and not something fancier

Because **interoperability with the existing trace corpus** is worth
more than richer fields. Adopting ChampSim format means we get, for
free:

- **DPC-3** — SPEC CPU2006/CPU2017 traces (1B skip / 200M warm / 500M
  sim) hosted at Stony Brook. Gold-standard SPEC traces for cache /
  prefetcher work.
- **CRC-2** — same SPEC mix, used for the cache-replacement
  championship.
- **IPC-1** — server / front-end-heavy traces (Cassandra, Drupal,
  Kafka, MySQL) used for the instruction-prefetching championship.
- **CVP-1 → ChampSim** — Qualcomm's 135 small + 2013 large traces,
  ported by Feliu et al. with the corrected converter from the
  [IISWC 2023 *Rebasing Microarchitectural Research with Industry Traces*](https://webs.um.es/aros/papers/pdfs/jfeliu-iiswc23.pdf)
  paper.

If a reviewer or collaborator asks "how does your simulator do on SPEC
2017 600.perlbench_s?", we can point at the DPC-3 trace and run it.

---

## Trace sources: where records come from

Two paths, both producing the same on-disk format.

### Path A — DynamoRIO `drmemtrace` (primary, planned)

#### What is DynamoRIO?

**Dynamic binary instrumentation (DBI)** tools sit between an
unmodified compiled binary and the CPU it runs on. Instead of
modifying source code or recompiling, they intercept every basic
block of machine code at runtime, optionally insert extra
instrumentation, and let the program continue. From the outside it
looks like the program is just running; from the inside, the DBI tool
can record every load, store, and branch as it happens.

The two big DBI frameworks in this space:

- **DynamoRIO** — BSD-style license, fully open source, Linux + macOS +
  Windows, x86-64 + AArch64. Actively developed at Google. Ships with
  a built-in trace collector called `drmemtrace` that records exactly
  the kind of (pc, opcode, mem_addr, branch_outcome) stream we need.
- **Intel Pin** — proprietary EULA but free binary download, primarily
  Linux + Windows, x86 only. The legacy choice; ChampSim's official
  tracer is a Pintool. Less actively developed than DynamoRIO.

We're going with DynamoRIO as the primary because it's open source,
cross-platform, and `drmemtrace` is the maintained way to do this kind
of recording. The plan:

```bash
# 1. Run an unmodified binary under DynamoRIO with the drcachesim
#    client. This produces a drmemtrace.*.zip file containing the
#    trace in DynamoRIO's native format.
drrun -t drcachesim -offline -- ./my_program args

# 2. Convert drmemtrace -> ChampSim binary using our own converter
#    (tools/tracer/drmem2champsim, planned).
tools/tracer/drmem2champsim out.champsimtrace drmemtrace.*.zip

# 3. Simulate.
sim --config configs/baseline.json --trace out.champsimtrace.xz
```

For multi-core / coherence workloads we'd trace pthreads programs (a
matmul, producer-consumer, linked-list walker, etc.) and split the
per-thread streams into per-core trace files at conversion time.

**Status:** the `drmem2champsim` post-processor and the
`docs/tracing.md` setup guide are deferred. Phase 3 didn't need real
workload traces — synthetic and project2-derived traces were enough.
We'll build out Path A when Phase 5 (multi-core) actually needs it.

### Path B — Pin-based ChampSim tracer (compatibility, planned)

The DPC-3 / CRC-2 / IPC-1 trace corpora were originally generated by
the upstream ChampSim Pintool (`tracer/champsim_tracer.cpp` in the
ChampSim repo). For platforms where Pin installs more cleanly than
DynamoRIO, or for SPEC re-tracing using binaries we already have, we'll
vendor or wrap that exact Pintool.

We're not writing it from scratch. The plan is to keep it under
`tools/tracer/pin_champsim/` either as a git submodule, a pinned
download, or a small `cmake` glue layer that builds the upstream
source.

### Path C — synthetic generation (done)

Both paths above need real binaries and a working DBI install.
Sometimes you just want a deterministic small trace for a unit test or
a CI smoke check. That's what `tools/gen_trace` does.

`gen_trace` synthesizes ChampSim records directly with configurable
patterns:

```bash
# Build
cmake --build --preset default --target gen_trace

# Generate 5000 records, ~20% of which are branches, in a "loop" pattern.
./build/default/tools/gen_trace/gen_trace \
    --out /tmp/synth.champsimtrace \
    --records 5000 \
    --pattern loop \
    --branch-rate 0.2
```

Patterns: `sequential`, `loop`, `stream`, `random`. Used in cache and
predictor unit tests where a real corpus would be overkill.

See [tools/gen_trace/](../tools/gen_trace/).

---

## What we built in Phase 1

### The trace I/O library
[src/common/trace.cpp](../src/common/trace.cpp), [include/comparch/trace.hpp](../include/comparch/trace.hpp)

- `trace::Reader` — opens a `.champsimtrace`, exposes `next(Record&)`
  iterator-style.
- `trace::Writer` — opens an output, exposes `write(Record)` plus
  `flush()`.
- `Record` struct — every field listed in the format table above.
- `Variant` enum — picks `input_instr` (default) or `cloudsuite_instr`.

Endianness is `static_assert`-checked at compile time
([trace.cpp:12](../src/common/trace.cpp#L12)) — the format is
little-endian and we don't byte-swap on big-endian hosts. If anyone
ever runs this on a PowerPC, the build will fail loudly rather than
silently corrupt traces.

Round-trip tested in [tests/common/test_trace.cpp](../tests/common/test_trace.cpp).

### The synthetic generator
[tools/gen_trace/](../tools/gen_trace/)

CLI tool plus a library (`casim_gen_trace`) so that test code can
generate a trace inline without shelling out to a binary.

### The format spec doc
[docs/trace-format.md](../docs/trace-format.md)

Plain-text spec of the binary layout, both record variants, and the
`.trace.meta.json` sidecar we'll ship next to traces (recording the
record-variant choice, source workload, warmup/sim split — out-of-band
metadata that doesn't break ChampSim compatibility).

### The proj2 → ChampSim converter
[tools/proj2_to_champsim/](../tools/proj2_to_champsim/)

A one-shot tool that converts project2's 11-field text trace format
into our canonical ChampSim binary, used by Phase 3 to cross-validate
the predictors against project2's reference numbers. Built in Phase 1
because the converter belongs to the trace toolchain, not to the
predictor subsystem.

---

## What's still pending

- `tools/tracer/drmem2champsim` (Path A converter)
- `docs/tracing.md` (DynamoRIO install + setup walkthrough)
- `tools/tracer/pin_champsim/` (Path B compatibility tracer)
- `scripts/fetch_traces.sh` (downloader for representative DPC-3 / IPC-1
  traces)

None of these block Phase 4 (single-core OoO) — that phase will run
fine on the synthetic traces and the project2-derived fixture from
Phase 3. They become necessary when Phase 5 (multi-core coherence)
needs realistic multi-threaded workloads.

---

## Why this matters for downstream phases

Phase 2 (`--mode cache`) reads `.champsimtrace` files and pulls
addresses out of `source_memory[]` / `destination_memory[]`.

Phase 3 (`--mode predictor`) reads the same files and pulls
`is_branch` / `branch_taken` / `ip`.

Phase 4 (`--mode ooo`) will read the same files and use everything —
the OoO core needs PCs for the I-cache, register IDs for ROB
dependencies, and memory addresses for the LSU.

Phase 5 will use per-core trace streams, but the on-disk record format
stays identical.

One format, four consumers. That's the point of doing the format work
upfront.
