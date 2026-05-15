# Architecture

`Multicore-OoO-sim` is a multi-core, out-of-order, cache-coherent CMP simulator
in modern C++20. Each core models a Tomasulo-style out-of-order pipeline with
a pluggable branch predictor and a private L1/L2 cache hierarchy. Cores share
a directory over a ring interconnect, with five interchangeable coherence
protocols (MI, MSI, MESI, MOSI, MOESIF). One binary, one JSON config, one
trace format — runs ChampSim v1 traces and the project's CasimV2 multi-thread
extension.

## System block diagram

```
   ┌─ Core 0 ──┐  ┌─ Core 1 ──┐  ...  ┌─ Core N-1 ─┐
   │ Fetch     │  │           │       │            │
   │ Decode    │  │           │       │            │
   │ Rename    │  │           │       │            │
   │ ROB / RS  │  │           │       │            │
   │ Exec / LSU│  │           │       │            │
   │ L1 D + I  │  │           │       │            │
   │ L2 priv.  │  │           │       │            │
   └─────┬─────┘  └─────┬─────┘       └─────┬──────┘
         │              │                    │
         └──────── Ring interconnect ────────┘
                          │
                  ┌───── Directory ─────┐
                  │   (sharer vectors)  │
                  └──────────┬──────────┘
                             │
                           ┌─┴─┐
                           │DRAM│
                           └───┘
```

Each core is a full OoO pipeline (fetch → decode → rename → dispatch → schedule
→ execute → writeback → retire). Each core has private L1 (I + D) and L2 caches.
The L1s on different cores stay coherent through a directory-based protocol;
the directory and the ring live in `src/coherence/`.

---

## Per-module breakdown

### Fetch / decode / rename

[src/ooo/core.cpp](https://github.com/dannydyl/Multicore-OoO-sim/blob/main/src/ooo/core.cpp),
[src/ooo/inst.cpp](https://github.com/dannydyl/Multicore-OoO-sim/blob/main/src/ooo/inst.cpp),
[src/ooo/rat.cpp](https://github.com/dannydyl/Multicore-OoO-sim/blob/main/src/ooo/rat.cpp).

The front-end pulls instructions from the trace reader, decodes them into the
internal `inst` representation, and renames their source/destination registers
through the **Register Alias Table (RAT)**. Fetch and decode widths, ROB size,
RS size, and number of functional units are all config knobs.

### Out-of-order backend (ROB + reservation stations + LSU)

[src/ooo/rob.cpp](https://github.com/dannydyl/Multicore-OoO-sim/blob/main/src/ooo/rob.cpp),
[src/ooo/schedq.cpp](https://github.com/dannydyl/Multicore-OoO-sim/blob/main/src/ooo/schedq.cpp).

A Tomasulo-style scheduler with a unified scheduling queue (RS) and a separate
Reorder Buffer for in-order retirement. Operands are tracked by ROB tag; when
a producer broadcasts on the Common Data Bus, dependent entries in the RS
capture the value and become ready to issue. Loads and stores flow through
the LSU; stores wait until commit to write the cache.

Once instructions retire from the ROB head, their architectural state is final
— this is what gives the simulator precise IPC numbers and lets it speak
honestly about branch-mispredict and memory-stall behavior.

> **Study material:**
> [Tomasulo's algorithm](../study/index.md),
> [Reorder Buffer](../study/index.md),
> [LSQ and store-to-load forwarding](../study/index.md)
> *(written progressively — see the [study index](../study/index.md) for current status)*.

### Branch predictor

[src/predictor/](https://github.com/dannydyl/Multicore-OoO-sim/tree/main/src/predictor) —
pluggable through [`factory.cpp`](https://github.com/dannydyl/Multicore-OoO-sim/blob/main/src/predictor/factory.cpp).

Five predictors available, selected from JSON config:

| Predictor      | File                  | Notes                                         |
| -------------- | --------------------- | --------------------------------------------- |
| always-taken   | `always_taken.cpp`    | Baseline / sanity check.                      |
| Yeh-Patt       | `yeh_patt.cpp`        | Two-level adaptive (BHR + PHT).               |
| perceptron     | `perceptron.cpp`      | Long-history capture via linear classifier.   |
| hybrid         | `hybrid.cpp`          | Combines two component predictors.            |
| tournament     | (via `hybrid.cpp`)    | Per-branch chooser between components.        |

Predictor-only mode (`--mode predictor`) was bit-for-bit validated against the
project2 reference output. See
[Phase 3 — Branch predictor](../report/04-phase3-predictor.md) for the regression
methodology.

### L1 / L2 caches

[src/cache/](https://github.com/dannydyl/Multicore-OoO-sim/tree/main/src/cache).

Private L1 and L2 per core. Both are non-blocking, MSHR-backed
([`mshr.cpp`](https://github.com/dannydyl/Multicore-OoO-sim/blob/main/src/cache/mshr.cpp)),
with pluggable replacement
([`replacement.cpp`](https://github.com/dannydyl/Multicore-OoO-sim/blob/main/src/cache/replacement.cpp):
LRU / LIP / MIP), write policy
([`write_policy.cpp`](https://github.com/dannydyl/Multicore-OoO-sim/blob/main/src/cache/write_policy.cpp):
WBWA), and prefetcher
(`prefetcher_plus_one.cpp` / `prefetcher_markov.cpp` / `prefetcher_hybrid.cpp`).

Cache geometry — sets, ways, block size, latency — is fully configurable per
level. The cache-only mode (`--mode cache`) is bit-for-bit validated against
project1's reference output. See
[Phase 2 — Cache subsystem](../report/03-phase2-cache.md) for AAT methodology
and reference numbers.

### Coherence agents

[src/coherence/agent_*.cpp](https://github.com/dannydyl/Multicore-OoO-sim/tree/main/src/coherence) —
one agent per protocol, dispatched through
[`factory.cpp`](https://github.com/dannydyl/Multicore-OoO-sim/blob/main/src/coherence/factory.cpp).

A coherence agent sits between each core's L1 and the ring. It tracks the
per-line protocol state (M / E / S / I / O / F depending on protocol), issues
requests, snoops responses, and updates state on transitions. Five protocols:

| Protocol | Agent file          | States                              |
| -------- | ------------------- | ----------------------------------- |
| MI       | `agent_mi.cpp`      | Modified, Invalid                   |
| MSI      | `agent_msi.cpp`     | M, S, I                             |
| MESI     | `agent_mesi.cpp`    | M, E, S, I                          |
| MOSI     | `agent_mosi.cpp`    | M, O, S, I                          |
| MOESIF   | `agent_moesif.cpp`  | M, O, E, S, I, F                    |

The coherence layer is bit-for-bit validated across all 16 protocol × topology
combos against project3. See
[Phase 5A — Cache coherence](../report/07-phase5a-coherence.md).

### Directory

[src/coherence/directory.cpp](https://github.com/dannydyl/Multicore-OoO-sim/blob/main/src/coherence/directory.cpp)
plus protocol-specific `directory_*.cpp` files.

Central directory with per-line **sharer vectors**. Serves requests from
agents, tracks owner / sharers, generates invalidations and downgrades.
Each protocol family has its own directory implementation because the state
machine is protocol-specific (e.g. MOESIF tracks the Forward responder
explicitly).

### Ring interconnect

[src/coherence/network.cpp](https://github.com/dannydyl/Multicore-OoO-sim/blob/main/src/coherence/network.cpp),
[src/coherence/message.cpp](https://github.com/dannydyl/Multicore-OoO-sim/blob/main/src/coherence/message.cpp),
[src/coherence/node.cpp](https://github.com/dannydyl/Multicore-OoO-sim/blob/main/src/coherence/node.cpp).

Ring topology with separate message classes (request, response, snoop) to
avoid protocol deadlock. Per-hop latency configurable; ring contention is
modeled — agents stall when the ring slot is occupied.

### DRAM

[src/cache/main_memory.cpp](https://github.com/dannydyl/Multicore-OoO-sim/blob/main/src/cache/main_memory.cpp).

A single shared channel with a fixed access latency. **This is the largest
deliberate simplification in the sim** — there's no rank / bank queueing,
no row-buffer modeling, no controller scheduling. See "What's modeled vs.
abstracted" below.

---

## Operating modes

`--mode` selects which subsystem the binary exercises. Default is `full`.

| Mode        | Exercises                                                  | Validated against                                     |
| ----------- | ---------------------------------------------------------- | ----------------------------------------------------- |
| `cache`     | L1 + L2 + MSHR + replacement + prefetcher                  | project1 reference output, bit-for-bit                |
| `predictor` | Branch predictor only (one predictor per trace)            | project2 reference output, bit-for-bit                |
| `ooo`       | Single-core OoO pipeline (fetch → retire) + private caches | Internal tests; see [Phase 4 review](../report/06-phase4-review.md) |
| `coherence` | Multi-core caches + directory + ring (no OoO timing)       | project3 reference output, bit-for-bit (16/16 combos) |
| `full`      | Everything: N OoO cores + coherence + ring + DRAM          | Internal regressions; SPEC2017 IPC matches published baselines |

Modes are gateways into the same code paths — they exist so each subsystem can
be regressed independently against its course-project reference, and so a
failing test can be localized.

---

## Configuration surface

The JSON config in
[configs/baseline.json](https://github.com/dannydyl/Multicore-OoO-sim/blob/main/configs/baseline.json)
is the single source of truth. CLI flags override individual fields. Major
knobs:

| Knob                       | Why you'd tune it                                                  |
| -------------------------- | ------------------------------------------------------------------ |
| `cores`                    | Scale studies. IPC saturates at 4 cores in the baseline (see [config sweep](../report/09-config-sweep.md)). |
| `coherence.protocol`       | Compare MI / MSI / MESI / MOSI / MOESIF on the same workload.      |
| `cache.l1.size`, `assoc`, `block_size` | AAT and miss-rate studies; capacity vs. associativity tradeoff. |
| `cache.l2.size`, etc.      | Same for L2; inclusion behavior.                                   |
| `cache.replacement`        | `LRU` / `LIP` / `MIP` — replacement-policy ablations.              |
| `cache.prefetcher`         | `none` / `plus_one` / `markov` / `hybrid` — coverage vs. accuracy. |
| `dram.latency`             | Sensitivity studies; how memory-bound is the workload?             |
| `ooo.rob_size`, `rs_size`, `width` | OoO sizing tradeoffs; instruction-window vs. IPC.          |
| `predictor.type`           | Compare always-taken / Yeh-Patt / perceptron / hybrid / tournament. |

See [Running](../usage/running.md) for the full CLI and config schema.

---

## What's modeled vs. what's abstracted

**Modeled:**

- OoO pipeline timing (fetch / decode / rename / dispatch / issue / exec / writeback / retire).
- ROB-bounded instruction window and in-order retirement (so the IPC numbers are real).
- Non-blocking caches with MSHR-induced stalls and secondary-miss combining.
- Coherence state machines including **races** (e.g. concurrent upgrades, eviction during snoop).
- Directory serialization and the contention it implies.
- Ring contention and per-hop latency.
- Multi-thread traces (CasimV2): per-core trace streams with sync/lifecycle records.

**Abstracted:**

- DRAM as a fixed-latency channel — no rank/bank queueing, no row-buffer locality, no controller scheduling.
- No TLB, no page-walk modeling, no virtual memory translation cost.
- No OS effects (context switches, page faults, interrupts).
- No frequency scaling, voltage, or power modeling.
- Functional execution: instructions are timed but not actually computed — values come from the trace.

These limits are stated honestly in [Status](status.md) so the IPC numbers are
read in context.

---

## Code-to-concept map

| Concept                       | Source                                                             | Phase report                                                              |
| ----------------------------- | ------------------------------------------------------------------ | ------------------------------------------------------------------------- |
| Trace reader + formats        | `tools/gen_trace/`, `src/coherence/fici_cpu.cpp`                   | [Phase 1 — Traces](../report/02-phase1-traces.md)                         |
| L1/L2 cache + MSHR            | `src/cache/`                                                       | [Phase 2 — Cache](../report/03-phase2-cache.md)                           |
| Branch prediction             | `src/predictor/`                                                   | [Phase 3 — Predictor](../report/04-phase3-predictor.md)                   |
| OoO pipeline (ROB / RS / LSU) | `src/ooo/`                                                         | [Phase 4 review](../report/06-phase4-review.md)                           |
| Coherence agents + directory  | `src/coherence/agent_*.cpp`, `directory_*.cpp`                     | [Phase 5A — Coherence](../report/07-phase5a-coherence.md)                 |
| Ring + message classes        | `src/coherence/network.cpp`, `message.cpp`, `node.cpp`             | [Phase 5B — Full integration](../report/08-phase5b-full.md)               |
| Multi-core integration        | `src/full/`, `src/main.cpp`                                        | [Phase 5B](../report/08-phase5b-full.md), [Config sweep](../report/09-config-sweep.md) |
| LLS shared cache + NINE       | `src/coherence/lls_cache.cpp`                                      | [LLS + hybrid coherence](../report/10-lls-hybrid-coherence.md), [LLS study guide](../report/18-lls-study-guide.md) |

---

## Where to go next

- **[How to run it](../usage/running.md)** — build, configs, CLI, trace formats.
- **[How it was built](../report/00-overview.md)** — phase-by-phase development journal.
- **[Concepts explained](../study/index.md)** — concept-first study material on Tomasulo, ROB, coherence protocols, predictors, AAT, consistency, and the ring.
