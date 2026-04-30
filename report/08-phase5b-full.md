# Phase 5B — Default mode: multi-core OoO + coherence

**Goal:** make the default invocation (no `--mode` flag) run the full
multi-core OoO simulator. N OoO cores, each with a private finite L1+L2,
all connected through the Phase 5A coherence ring + directory + agents.

After this phase the simulator engine is feature-complete: every
microarchitectural piece in the original plan runs together. What's
left is Phase 1 leftover (real workload tracing) and Phase 6 (polish,
plots, public release).

---

## What was built

| Piece | Where it lives now | Notes |
| --- | --- | --- |
| `CoherenceSink` interface | [include/comparch/cache/coherence_sink.hpp](../include/comparch/cache/coherence_sink.hpp) | Tiny abstract base. Lives in the cache module so the cache layer doesn't pull coherence headers. |
| `Cache::Config::coherence_sink` slot | [include/comparch/cache/cache.hpp](../include/comparch/cache/cache.hpp) | Wired into 8 miss/eviction call sites in `cache.cpp`. With sink null, behavior is byte-identical to Phase 4. |
| `Cache::mark_ready` / `coherence_invalidate` | [src/cache/cache.cpp](../src/cache/cache.cpp) | External MSHR completion + silent block drop on directory invalidation. |
| `kCoherenceSuspendedLatency` sentinel | [include/comparch/cache/coherence_sink.hpp](../include/comparch/cache/coherence_sink.hpp) | `unsigned int max` returned by `Cache::access` when a miss has been routed to the sink. Propagates through L1→L2 chains so L1's `issue()` parks the MSHR with `due_cycle = UINT64_MAX`. |
| `CpuPort` abstract type | [include/comparch/coherence/cpu_port.hpp](../include/comparch/coherence/cpu_port.hpp) | Replaces the `FiciCpu*` field in `coherence::Cache`. `FiciCpu` (5A) and `CoherenceAdapter` (5B) both inherit. |
| `CoherenceAdapter` | [src/coherence/coherence_adapter.cpp](../src/coherence/coherence_adapter.cpp) | The per-core shim. Implements `CoherenceSink` (L2 calls it on miss/evict) and `CpuPort` (the agent's `send_DATA_proc` writes into it). Owns the per-core `coherence::Cache` (the unbounded protocol-state map from Phase 5A) and holds non-owning pointers to L1+L2. |
| `MessageKind::DATA_WB` repurposed as WRITEBACK | [src/coherence/directory.cpp](../src/coherence/directory.cpp) | New shared `DirectoryController::handle_writeback` helper; each protocol's tick gets a leading branch. Dirty M/O/F evictions increment `memory_writes`; clean drops just clear presence. |
| Network second constructor | [include/comparch/coherence/network.hpp](../include/comparch/coherence/network.hpp), [src/coherence/network.cpp](../src/coherence/network.cpp) | Phase 5A constructor stays for `--mode coherence`. The 5B constructor takes pre-built `(CpuPort*, coherence::Cache*)` pairs and a `DirectoryController` and just stitches them into the ring. |
| Async OoO stores | [src/ooo/core.cpp](../src/ooo/core.cpp) | Phase 4's synchronous `l1d_->access(req)` for stores is replaced by `l1d_->issue(req)` + `peek/complete` polling on the LSU FU — same path as loads. Required because under coherence a store may need a network round-trip to acquire M-state. |
| `run_full_mode` driver | [src/full/full_mode.cpp](../src/full/full_mode.cpp) | Builds N OoO cores, N L1+L2 caches, N adapters, 1 directory, 1 Network. Per-cycle loop: tick all cores, then network, then tock. Global cycle cap as a backstop. |
| CLI: drop `"full"` from `--mode` validator | [src/common/cli.cpp](../src/common/cli.cpp) | Default invocation (no `--mode`) → `Mode::Full` → `run_full_mode`. `parse_mode("full")` still resolves internally for round-trip tests. |

What was **dropped**:

- The synchronous-store path in the OoO core. Under coherence it's
  fundamentally unsound (a store needs M-state, which may take a
  network round-trip). The async path subsumes both single-core and
  multi-core configurations; Phase 4 `--mode ooo` tests still pass
  with the async path because the `due_cycle` logic still drives
  completion in the absence of a coherence sink.

- The `Cache::issue` Op::Write rejection. Was a precondition designed
  to prevent merge fast-path from losing dirty-bit semantics; now
  writes simply skip the merge path and always allocate a fresh
  MSHR slot.

What was **deferred** to Phase 6 cleanup (acknowledged correctness gaps):

- **Store on S-state hit:** A store on a line resident in L1 in S-state
  hits the cache (`block_in() = true`) and completes synchronously
  without consulting the agent. Under coherence it should issue a
  GETM to upgrade S→M first. Phase 5B's tests don't exercise this
  pattern (synthetic shared-load and producer-consumer cases land
  blocks in M directly), so the gap doesn't surface in the suite —
  but a real workload doing read-then-write on shared data would hit
  it. Fix: route writes through a write-aware path that consults
  agent state, OR mark write-hits to non-M lines as forced misses.

- **RECALL_GOTO_S clean-bit propagation:** When the agent transitions
  M→O / E→F via RECALL_GOTO_S, the line stays resident in L1+L2 but
  its dirty bit doesn't get cleared. The next eviction will spuriously
  count it as a dirty writeback. Inflates `memory_writes` slightly;
  doesn't cause incorrect coherence behavior.

- **Adapter outbound queue under deep contention:** L2 misses in the
  same cycle from multiple LSU FUs are buffered in the adapter's
  `outbound_proc_` deque and drained one per cycle. Acceptable for
  Phase 5B but a real bottleneck-modeling pass would expose this as
  a configurable per-cycle dispatch limit.

---

## Architecture

```
Per core i (i ∈ [0, cores)):
   OooCore_i  ──► l1d_i  ──► l2_i  ──► CoherenceAdapter_i
                                            │
                                            ▼
                            coherence::Cache_i (state-table, agents)
                                            │
                                  Network (Phase 5A, RING)
                                            │
                                ┌───────────┴───────────┐
                                ▼                       ▼
                     DirectoryController         (other cores)
```

Per-cycle ordering (in `run_full_mode`'s main loop):

1. Tick every OoO core. The core advances its pipeline; on an L2 miss
   the adapter queues a LOAD/STORE Message into its outbound buffer.
2. Tick the Network. Each Node ticks: the adapter drains one queued
   message into `coh_cache->cpu_in_next`, the agent processes any
   message that landed in `cpu_in` last tock, ring movement happens.
3. Tock the Network. Half-cycle staging buffers shift forward.

Termination: every core reports done AND the Network has no in-flight
messages. A 5M-cycle global cap catches deadlock-shaped hangs.

---

## CLI changes

Before:
```
--mode {full,cache,predictor,ooo,coherence}
```
After:
```
--mode {cache,predictor,ooo,coherence}    # omit for full multi-core simulator
```

`Mode::Full` stays as the internal default (`CliArgs::mode`'s initializer).
`parse_mode("full")` continues to work for `to_string`/`parse_mode`
round-trip tests. Passing `--mode full` is now rejected with exit 1
and a clear error.

The default invocation:
```sh
./build/src/sim --config configs/baseline.json \
  --cores 4 \
  --trace-dir traces/myworkload/
```
runs the full system. `traces/myworkload/p0.champsimtrace` ...
`p3.champsimtrace` are the expected per-core ChampSim binary traces.

---

## Verification

Test count: 122 (Phase 5A) → 126 (Phase 5B). New tests:

- `test_cli_dispatch.cpp` (Phase 5A): 5 — `Mode::Full` is the default,
  `parse_mode("full")` still resolves, `--mode full` rejected, no-flag
  invocation reaches `Mode::Full`.
- `test_coherence_sink.cpp`: 4 — `Cache::access` returns suspended
  sentinel when a sink is wired; `mark_ready` flips MSHR ready;
  `coherence_invalidate` drops resident blocks; sink-wired evictions
  notify on both dirty and clean victims.
- `test_writeback.cpp`: 5 — one per protocol (MSI/MESI/MOSI/MOESIF
  + MI). Verify dirty-owner WRITEBACK increments `memory_writes`,
  clean-sharer WRITEBACK is silent, presence drops, state collapses
  to I when last sharer leaves.
- `test_full_mode.cpp`: 4 — 1-core ALU-only smoke; 2-core ALU-only;
  4-core private-loads under MESI; all 5 protocols run a 2-core
  private-load trace.

```
cmake --build build -j && ctest --test-dir build --output-on-failure
# 126 / 126 tests passed

cmake --preset ci && cmake --build build-ci -j && ctest --test-dir build-ci
# 126 / 126, clean under -Werror on Apple Clang

cmake --preset release && cmake --build build-release -j
# 126 / 126, -O3 build
```

Phase 5A's 16/16 proj3 parity combos still pass — the WRITEBACK
directory branch never fires under FICI traces (which never produce
evictions on the unbounded `coherence::Cache` state-table).

---

## What's next

The simulator engine is done. The remaining roadmap items are:

- **Phase 1 finish-out:** the real DynamoRIO-based tracer
  (`tools/tracer/drmem2champsim`), public trace fetcher
  (`scripts/fetch_traces.sh`), end-to-end validation on a pthreads
  matmul workload, optional Pin-based ChampSim-tracer wrapper. Until
  this lands, Phase 5B can only run synthetic and project-converted
  traces.
- **Phase 6 polish:** correctness gaps listed above (S→M store
  upgrade, dirty-bit propagation on RECALL_GOTO_S), an interesting
  "result" plot or two (IPC vs cache size, MPKI per predictor,
  scaling vs core count under MSI/MESI/MOSI/MOESIF), `docs/architecture.md`
  with timing diagrams, public-release decision.
