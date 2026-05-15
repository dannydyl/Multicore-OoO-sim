# Tracing — getting traces into the simulator

Companion to [trace-format.md](trace-format.md), which specs the
on-disk binary formats (ChampSim v1 + CasimV2). *This* doc is about
**where the traces come from** — given a workload you want to study,
which path produces files the simulator can replay?

The sim accepts three trace input forms today:

| Form | Use when | Format | Driver flag |
| ---- | -------- | ------ | ----------- |
| Per-core ChampSim binary | You have an existing ChampSim corpus (DPC-3 / IPC-1, single-thread workloads sharded across cores) | `<dir>/p<i>.champsimtrace` | `--trace-dir DIR` |
| Manifest of mixed traces | You want heterogeneous workloads across cores (different traces per core) | line-per-file manifest | `--trace-list FILE` |
| Multi-thread program | You want one logical program with N threads sharing memory + sync | CasimV2 `.casim` per thread + manifest | `--program FILE` |

## Producing traces

### Option A: synthetic via `casim_synth` (works today)

The fastest way to get a meaningful multi-thread trace into the
sim. `tools/casim_synth/` is a small C++ library + four example
binaries (`synth_lock_chain`, `synth_lock_chain_mem`,
`synth_dot_product`, `synth_prodcon`). Each one constructs a
program description programmatically and writes `t<N>.casim` files
plus a `*.manifest` ready for `sim --program`.

Pros: instant feedback loop, deterministic, no external deps,
covers the sync subsystem's interesting cases. Cons: instruction
streams are synthesized (ALU filler between sync events), not
real program behavior — useful for studying cache coherence and
lock contention; less useful for studying frontend / branch
prediction in real code.

See `tools/casim_synth/programs/*.cpp` for templates. Adding a
new synthetic workload is one C++ file plus a CMakeLists entry.

### Option B: ChampSim corpus (works today)

If you have an existing ChampSim trace (e.g. one of the DPC-3
SPEC traces), drop it as `<dir>/p0.champsimtrace` and run with
`--trace-dir <dir> --cores 1`. For multi-core sweeps with
homogeneous workloads, copy the same trace to `p0..pN-1`. For
heterogeneous mixes, use `--trace-list` with a manifest.

This path doesn't produce inter-core coherence traffic (each core
runs an independent address space). It's the right starting point
for cache + branch-predictor sweeps; not the right one for
contention studies.

### Option C: DynamoRIO-based tracer (not implemented)

A DR client that captures per-thread CasimV2 traces from real
pthread programs. This is the highest-fidelity option and the
gap between "we have a sim" and "we have real-workload numbers
from real programs."

Status: **not implemented**. Sketched here so a future
implementation has a clear scope.

Required:
- DR 11+ on Linux x86_64 (DR on macOS arm64 is too patchy).
- A DR client (~800-1200 LOC C++) using `drmgr` + `drwrap`:
  - per-thread output file open in `dr_thread_init_event`
  - basic-block instrumentation that emits Instr records with
    IP, branch info, register IDs, memory addresses (via
    `drutil_insert_get_mem_addr`)
  - drwrap hooks on `pthread_mutex_lock`, `pthread_mutex_unlock`,
    `pthread_barrier_init`, `pthread_barrier_wait`,
    `pthread_create`, `pthread_join` that emit `SyncRecord` /
    `LifecycleRecord` entries
  - per-mutex / per-barrier sequence-number tracking with
    `dr_mutex`-protected atomic counters
- A Dockerfile that pins Ubuntu + DR + g++ + the target benchmark
  source (SPLASH-2 is the canonical starting point).
- A runner script that mounts a host volume for trace output and
  invokes `drrun -c client.so -- /path/to/benchmark`.

Validation path: run a small pthread program with two threads
contending on one mutex; confirm the produced `.casim` files
roundtrip cleanly through `sim --program` and that the
`utilization.rpt` shows a cascading sync-stall pattern matching
the lock-chain synth example.

### Option D: Intel Pin (not implemented, possible alternative to DR)

Pin offers similar dynamic-instrumentation capabilities to DR.
Some ChampSim corpora were originally captured with Pin tools.
We don't have a Pin client today; structurally it would mirror
the DR sketch above with Pin's `INS_InsertCall` /
`RTN_InsertCall` APIs.

### Option E: project2 → champsim converter (works today, niche)

`tools/proj2_to_champsim/` converts the textual project2 trace
format into ChampSim binary. Useful only if you have project2-
style traces from a course context.

## Why not just one path

Each option occupies a different point on the speed-vs-fidelity
curve:

- **casim_synth**: full control, full speed, low fidelity.
  Best for unit / property testing of the sim itself, and for
  controlled experiments where you want to isolate one variable.
- **ChampSim corpus**: real workload behavior at the frontend
  level, no inter-core coherence interaction. Best for
  uniprocessor or homogeneous-multicore sweeps.
- **DR client (future)**: real pthread programs, real coherence
  traffic, real cache pressure. Best for any result claim that
  appears in a paper — "speedup of X% on SPLASH-2 LU.b under
  protocol Y" — assuming the DR pipeline gets built.

## Trace file layout summary

```
report dir layout (output)        trace dir layout (input)
---------------------------       ------------------------
report/<run>/                     ChampSim per-core:
  report.rpt                        <dir>/
  config.rpt                          p0.champsimtrace
  stats.rpt                           p1.champsimtrace
  coherence.rpt                       ...
  utilization.rpt                     pN-1.champsimtrace
  report.csv
                                   Trace-list manifest:
                                     <file>.txt
                                     ├─ /path/to/trace0
                                     ├─ /path/to/trace1
                                     └─ ...
                                       (one path per line; '#' comments)

                                   CasimV2 program:
                                     <dir>/
                                       prog.manifest
                                       ├─ program: name
                                       ├─ threads: N
                                       ├─ t0: t0.casim
                                       ├─ t1: t1.casim
                                       └─ ...
                                       t0.casim
                                       t1.casim
                                       ...
```

## Known limitations

The non-trace-format limitations live in [STATUS.md](../STATUS.md).
Trace-related ones:

- **No condition-variable capture path**. Even when the DR client
  lands, the sim doesn't model cond-wait pair-matching, so the
  client can either skip cond-var emission or emit records that
  pass the gate trivially.
- **No I-stream capture in casim_synth**. The synthetic programs
  emit a sequence of "filler" ALU records with rotating dest
  registers between sync events. Realistic frontend behavior
  (branch prediction, I-cache pressure) is not modeled.
- **Trace size**. Real SPLASH-2 LU.b runs ~1B instructions per
  thread. At 64 bytes per CasimV2 record that's ~64GB per
  thread uncompressed. Future work: in-trace bracket
  primitives (`trace_begin` / `trace_end`) so the client only
  captures the region of interest, plus on-the-fly gzip.
