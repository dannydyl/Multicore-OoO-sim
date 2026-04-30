# Phase 5A — Cache coherence (`--mode coherence`)

**Goal:** port the multi-core, directory-based cache-coherence
subsystem from `project3_v1.1.0/` into the unified sim, drive it
with `--mode coherence`, and pin the resulting cycle counts and
event counters bit-for-bit against project3's own reference outputs.

This phase is the largest single subsystem (~3,000 LOC across the
network, node, directory, five protocol agents, and five protocol
directories) but mechanically the simplest in concept: most of the
work is a faithful structural port. The interesting decisions are
elsewhere — what to *not* port (XBAR, the `OLDNTWK` legacy network
path, the unused FICO/FOCO CPU types, MOESI), how to thread runtime
config without project3's globals, and how the unbounded coherence
state-table will graduate to the finite Phase 2 cache later.

Phase 5A is split off from the broader Phase 5 explicitly so that
parity can be locked before Phase 5B integrates this subsystem with
the Phase 4 OoO core for `--mode full`.

---

## What was ported

| project3 piece | Where it lives now | Notes |
| --- | --- | --- |
| `Network` orchestrator | [src/coherence/network.cpp](../src/coherence/network.cpp) | Owns N CPU nodes + 1 directory node; per-cycle tick/tock walk |
| `Node` (RING) | [src/coherence/node.cpp](../src/coherence/node.cpp) | Four message queues + per-port outgoing/incoming flit slots |
| `CPU` (FICI variant) | [src/coherence/fici_cpu.cpp](../src/coherence/fici_cpu.cpp) | Single-outstanding-request trace driver; `r 0xADDR` / `w 0xADDR` |
| `Cache` (state-table only) | [src/coherence/coherence_cache.cpp](../src/coherence/coherence_cache.cpp) | Lazy block-keyed Agent map; protocol-agnostic via injected factory |
| `Agent` base + helpers | [src/coherence/agent.cpp](../src/coherence/agent.cpp) | `send_GETS / GETM / GETX / INVACK / DATA_dir / DATA_proc` |
| `DirectoryController` (shared infra) | [src/coherence/directory.cpp](../src/coherence/directory.cpp) | `poll_queue / dequeue / cycle_queue / send_Request`, dispatch by protocol |
| MI agent + directory | [src/coherence/agent_mi.cpp](../src/coherence/agent_mi.cpp), [src/coherence/directory_mi.cpp](../src/coherence/directory_mi.cpp) | Smallest protocol; unit-tested only (no project3 reference output) |
| MSI agent + directory | [src/coherence/agent_msi.cpp](../src/coherence/agent_msi.cpp), [src/coherence/directory_msi.cpp](../src/coherence/directory_msi.cpp) | Bit-for-bit parity, 4 / 8 / 12 / 16 cores |
| MESI agent + directory | [src/coherence/agent_mesi.cpp](../src/coherence/agent_mesi.cpp), [src/coherence/directory_mesi.cpp](../src/coherence/directory_mesi.cpp) | Adds E + silent upgrades |
| MOSI agent + directory | [src/coherence/agent_mosi.cpp](../src/coherence/agent_mosi.cpp), [src/coherence/directory_mosi.cpp](../src/coherence/directory_mosi.cpp) | Adds O (Owner) and the M -> O recall |
| MOESIF agent + directory | [src/coherence/agent_moesif.cpp](../src/coherence/agent_moesif.cpp), [src/coherence/directory_moesif.cpp](../src/coherence/directory_moesif.cpp) | Largest; six stable states (I, S, E, M, O, F) |
| `Settings` (runtime knobs) | [include/comparch/coherence/settings.hpp](../include/comparch/coherence/settings.hpp) | Replaces project3's global `settings` struct; threaded by reference |
| `CoherenceStats` (counters + printer) | [src/coherence/coherence_stats.cpp](../src/coherence/coherence_stats.cpp) | Project3-compatible 7-line printf format pinned by unit test |
| Driver | [src/coherence/coherence_mode.cpp](../src/coherence/coherence_mode.cpp) | Validates inputs, prints banner, runs tick/tock until is_done |

What was **dropped**:

- **XBAR topology.** Experimental in project3 (no reference output). `--mode coherence` rejects `interconnect.topology=xbar` with a clear deferred-feature message until Phase 5B+.
- **`OLDNTWK` legacy path.** Project3's `node.cpp` carries a `#define OLDNTWK` block alongside the active flit-based network model; we ported only the active one.
- **FICO / FOCO CPU types.** Project3 stubs them out with `fatal_error`. Only FICI is ported.
- **MOESI (without F).** Project3 reserves `MOESI_PRO` in the enum but ships no Agent or Directory. Skipped.
- **MI external regression.** No reference output exists; coverage is synthetic (single-CPU re-access, two-CPU ping-pong, write-stream silent-upgrade check).

What was **deferred** (originally part of the Phase 5A plan, deliberately moved to 5B):

- **Wiring the finite Phase 2 [cache::Cache](../include/comparch/cache/cache.hpp) underneath the coherence layer.** The plan called for it; trying to land both at once made parity impossible (project3 has no eviction or hit latency, and matching cycle counts against an unbounded state-table is what locks the regression). Phase 5B re-introduces the finite cache as an underlying capacity / latency layer, with the coherence state map keeping its block-keyed structure on top.

---

## Architecture overview

```
                     +---------------------------+
                     |          Network          |
                     | (RING; one Directory node)|
                     +--+-----+-----+-----+--+---+
                        |     |     |     |  |
                      Node 0 ... Node N-1   Dir
                       |                     |
                  CPU + Cache             DirectoryController
                  (FiciCpu) (state-table)   (per-block dir entries +
                                             per-protocol *_tick)
```

Per-cycle ordering matches project3 verbatim — Network::tick walks every
node which ticks its local engine (CPU + Cache, or Directory) then
processes the network egress/ingress queues; Network::tock then drains
the half-cycle staging buffers (`*_next` -> `*`) and decrements
in-flight flit counters. The 4-queue model
(`ntwk_out_next` / `ntwk_out_queue` / `ntwk_in_queue` / `ntwk_in_next`)
plus per-port `outgoing_packets[]` / `incoming_packets[]` arrays drives
both flit timing and port contention. An off-by-one anywhere in here
would skew cycle counts across a 130k-cycle run; each piece was ported
line-by-line and diffed against the legacy.

The Settings struct holds `protocol`, `num_procs`, `mem_latency`,
`block_size_log2`, `link_width_log2` (and the derived `header_flits` /
`payload_flits` from `finalize_settings`). It's threaded by reference
into Network -> Node -> Cache / Directory / Agent. CoherenceStats is
the same — passed by reference, mutated in place — replacing
project3's global `Simulator *sim` singleton.

---

## Verification — bit-for-bit against project3

The regression test ([tests/coherence/test_proj3_regression.cpp](../tests/coherence/test_proj3_regression.cpp))
runs `run_coherence_mode` against each fixture and diffs captured
stdout against the reference output, modulo the `Trace Directory:`
banner line (which echoes whatever path the user passes — the
fixtures encode `traces/core_4` as a relative literal because that's
what `dirsim` was originally invoked with).

### Pinned matrix

```
          MSI    MESI   MOSI   MOESIF
core_4    PIN    PIN    PIN    PIN
core_8    PIN    PIN    PIN    PIN
core_12   PIN    PIN    PIN    PIN
core_16   PIN    PIN    PIN    PIN
```

16 / 16 combos bit-for-bit identical to `dirsim` reference outputs.
Each fixture diffs a banner + 7 stat lines:
```
Cycles
Cache Misses           <count> misses
Cache Accesses         <count> accesses
Silent Upgrades        <count> upgrades
$-to-$ Transfers       <count> transfers
Memory Reads           <count> reads
Memory Writes          <count> writes
```

### Synthetic unit tests

- [test_coherence_stats](../tests/coherence/test_coherence_stats.cpp) — pin the project3-compatible 7-line printf format byte-for-byte (catches a single trailing-space drift before it can break parity).
- [test_message](../tests/coherence/test_network_basic.cpp) — Message ctor adds payload flits exactly for DATA / DATA_E / DATA_F / DATA_WB.
- [test_fici_cpu](../tests/coherence/test_fici_cpu.cpp) — `r 0xADDR` / `w 0xADDR` parsing; rejects malformed lines, empty file = empty trace.
- [test_network_basic](../tests/coherence/test_network_basic.cpp) — empty traces -> immediate `is_done`; tick / tock safe on no-op networks.
- [test_mi](../tests/coherence/test_mi.cpp) — three MI scenarios verifying miss-on-first-access, $-to-$ on ping-pong, and the no-silent-upgrades invariant.

Test count after Phase 5A: 88 (Phase 4 baseline) -> 108. Run with:

```
cmake --preset default && cmake --build build -j
ctest --test-dir build --output-on-failure
```

CI matrix (default + ci [-Werror] + release [-O3]) all green on macOS Clang.

---

## Key decisions

- **All five protocols ported in this phase.** Same MO as Phase 3
  (which shipped four predictors at once). The factory + interface
  + tests are equally valuable whether one or five protocols are
  wired through them.
- **RING only; XBAR rejected with a clear "deferred" message.**
  Project3's XBAR has no graded reference outputs, so it's code
  without a parity check. Plug it back in if Phase 5B+ needs it.
- **No globals.** Project3 leans heavily on `settings` and
  `sim` singletons. The port threads `const Settings&` and
  `CoherenceStats&` everywhere — uglier-looking constructors, but
  the unit tests can instantiate one Network per case and read
  stats off it without polluting global state.
- **Unbounded `CoherenceCache` (this phase) vs. finite
  `cache::Cache` (Phase 5B).** Plan originally called for layering
  coherence on top of Phase 2's finite cache from day one. In
  practice, project3's reference outputs assume infinite capacity
  and zero hit latency; matching those with a finite cache requires
  hand-tuned over-sized configs that defeat the purpose. Decoupled:
  Phase 5A nails parity against the unbounded state table that the
  legacy regressions assume; Phase 5B re-introduces the finite
  cache as an underlying capacity layer when wiring to the OoO
  core (where it actually matters for IPC).
- **Manual `new`/`delete` on `Message*` queues.** Mirrors project3
  for review-ability. The Message lifetime is deliberately
  protocol-shaped (an INVACK from the directory hops the ring,
  is consumed by an agent, deleted by Cache::tick), and dropping
  in `unique_ptr` would obscure the diff against the legacy
  state machines that the regression test pins.

---

## What's next — Phase 5B

Phase 5B wraps the verified Phase 5A subsystem around the Phase 4
OoO core for `--mode full`:

1. Replace each Node's FiciCpu with a Phase 4 `OooCore`, wired to
   a per-core finite L1-D from `cache::Cache`.
2. Add a coherence-aware path that observes Cache fills /
   evictions / MSHR completions and forwards the events to the
   per-block coherence state.
3. Split the per-Node configuration so each node's L1 / L2 size,
   prefetcher, predictor, OoO width are independently tunable
   from JSON.
4. Cross-validate against project3 again where it makes sense
   (capacity-matched configs) and add new "real workload"
   scenarios (pthreads matmul) where the OoO + coherence
   interaction is the actual subject.
