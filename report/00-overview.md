# Overview — what this project is

`Multicore-OoO-sim` is a from-scratch rebuild of three of my course
assignments into one cohesive multi-core, out-of-order, cache-coherent
computer-architecture simulator. It exists for two reasons:

1. **Personal portfolio.** As a single integrated piece, it reads on
   GitHub as one piece of engineering, not three homework dumps. Anyone
   landing on the repo sees one binary, one config format, one trace
   format, one test suite.
2. **Research scaffold.** Once the OoO core and coherence layer land,
   the simulator becomes a usable platform for my own micro-architecture
   experiments — branch-predictor variants, prefetcher ablations,
   coherence-protocol scaling studies — without me having to glue
   together other peoples' simulators every time.

## The architecture in one diagram

```
                 +-----------------------------+
                 |        Coherent Network     |   <- from project3 (RING / XBAR)
                 |  (directory + interconnect) |
                 +--+-------+-------+-------+--+
                    |       |       |       |
                  Core 0  Core 1  Core 2  Core N-1
                  +----+ +----+ +----+ +----+
                  |OoO | |OoO | |OoO | |OoO |   <- from project2 (Tomasulo + branch pred)
                  | L1 | | L1 | | L1 | | L1 |   <- from project1 (L1)
                  | L2 | | L2 | | L2 | | L2 |   <- from project1 (L2)
                  +----+ +----+ +----+ +----+
                            |
                          DRAM
```

Each core is a full out-of-order pipeline (fetch → branch predict →
dispatch → schedule → execute → writeback → retire), each core has a
real L1/L2 cache, and the L1s on different cores stay coherent through
a directory-based protocol.

## What's source projects vs. new

| Source project           | Kept                                           | Dropped                                                          |
| ------------------------ | ---------------------------------------------- | ---------------------------------------------------------------- |
| project1 (L1+L2 cache)   | geometry math, LRU/LIP/MIP, prefetchers (+1, Markov, Hybrid), AAT calc | global C-style arrays, course driver, course traces        |
| project2 (OoO + branch)  | branch predictors, ROB / dispatch / schedQ / FU pipeline | pre-baked icache_hit/dcache_hit in trace, course driver  |
| project3 (coherence)     | event loop, Network (RING/XBAR), Node, agents, directory | one-op-per-cycle CPU, course scaffolding files            |
| **Everywhere**           | —                                              | Docker scripts, validate_* targets, GT/course attribution        |

The course driver code is gone in every case — the simulator has a single
new `main.cpp` that wires everything together via JSON config and CLI
flags.

## Phased roadmap

The project ships in self-contained phases, each ending with a runnable,
testable thing.

| Phase | What lands                            | Status   | This report                           |
| ----- | ------------------------------------- | -------- | ------------------------------------- |
| 0     | Skeleton, build system, CLI, JSON config, logging, CI | ✅ done | [01-phase0-skeleton.md](01-phase0-skeleton.md) |
| 1     | ChampSim trace format, reader/writer, synthetic generator, tracer plan | partial  | [02-phase1-traces.md](02-phase1-traces.md)     |
| 2     | Cache subsystem (`--mode cache`)      | ✅ done | [03-phase2-cache.md](03-phase2-cache.md)       |
| 3     | Branch predictors (`--mode predictor`)| ✅ done | [04-phase3-predictor.md](04-phase3-predictor.md) |
| 4     | OoO core (`--mode ooo`, single-core full) | pending | —                                                |
| 5     | Multi-core + coherence (`--mode full` / `coherence`) | pending | —                                     |
| 6     | Polish, results plots, public release | pending  | —                                                |

Phase 1 is "partial" because the ChampSim reader/writer and the
synthetic `gen_trace` are done, but the DynamoRIO-based real-workload
tracer is deferred — Phase 3 only needs synthetic and project2-derived
traces, which we already have.

## What's runnable today

```bash
# Build
cmake --preset default
cmake --build --preset default

# Run cache mode on a fixture trace
./build/default/sim --mode cache \
    --config configs/baseline.json \
    --trace tests/cache/fixtures/proj1/short_gcc.trace

# Run predictor mode
./build/default/sim --mode predictor \
    --config configs/baseline.json \
    --trace tests/predictor/fixtures/proj2/branchsim.champsimtrace

# Test suite
ctest --preset default
```

62 tests, all green. Cache numbers match project1 reference output
bit-for-bit; predictor numbers match project2 reference output
bit-for-bit (see Phase 2 / Phase 3 reports for the regression
methodology).

## How to read the rest of this report

If you want a quick skim:

- **Read [00-overview.md](00-overview.md) (this file)** for the big picture.
- **Read [05-tools-and-libraries.md](05-tools-and-libraries.md)** if you
  want to understand the toolchain — what CMake / Catch2 / nlohmann/json
  / CLI11 / ChampSim / DynamoRIO / Pin actually do. Written for someone
  who knows architecture but hasn't worked with these particular tools.
- **Read the phase-N report** (e.g. `04-phase3-predictor.md`) for the
  details of one specific phase.
- **Read [code-review.md](code-review.md)** for the senior-level code
  review I did on top of Phases 0–3.

If you're picking up where Phase 3 left off and just want to start
Phase 4, the relevant section is "Recommended fixes, in priority order"
at the bottom of `code-review.md`.
