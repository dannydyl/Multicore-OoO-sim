---
title: Home
hide:
  - navigation
---

# Multicore OoO Simulator

A multi-core, out-of-order, cache-coherent CMP simulator written in modern C++20.
Each core models a Tomasulo-style out-of-order pipeline with a real branch
predictor and a private L1/L2 cache hierarchy. Cores are connected through a
ring interconnect with a directory-based coherence protocol.

```
                 +-----------------------------+
                 |        Coherent Network     |   (RING)
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

This is a personal project, continuously evolving. The core engine
(OoO + cache + coherence + multi-thread sync) is complete and tested
against published baselines on real SPEC2017 traces.

---

## Where to go next

<div class="grid cards" markdown>

-   :material-sitemap: **[Architecture](overview/architecture.md)**

    The system block diagram, per-module breakdown, and what's modeled vs. abstracted.

-   :material-play: **[Running the simulator](usage/running.md)**

    Build, configure, point it at a trace, and read the output.

-   :material-book-open-page-variant: **[Phase reports](report/index.md)**

    A development journal documenting every phase — design, implementation, bugs, validation.

-   :material-school: **[Study material](study/index.md)**

    Concept-first explanations: Tomasulo, ROB, branch prediction, cache coherence (MSI → MOESIF),
    memory consistency, interconnect.

</div>

---

## At a glance

| Aspect            | What this sim has                                                       |
| ----------------- | ----------------------------------------------------------------------- |
| Cores             | Configurable N. Tomasulo-style OoO. ROB + reservation stations + LSU.   |
| Branch predictor  | Pluggable: always-taken, Yeh-Patt, perceptron, hybrid, tournament.      |
| Caches            | Private L1 + L2 per core. Non-blocking, MSHR-backed. LRU/LIP/MIP.       |
| Prefetcher        | Pluggable: next-line (+1), Markov, hybrid.                              |
| Coherence         | Directory-based. Five protocols: MI, MSI, MESI, MOSI, MOESIF.           |
| Interconnect      | Ring. Request / response / snoop message classes.                       |
| Memory            | Single-channel DRAM with fixed latency (a known simplification).        |
| Traces            | ChampSim v1 binary + CasimV2 multi-thread extension.                    |
| Validation        | Bit-for-bit regression against three Georgia Tech course projects.      |
| Tests             | 187 Catch2 tests, CI on Linux x86_64 and macOS arm64.                   |

See [Status](overview/status.md) for the honest current state — what works, what's
known-limited, what's deferred.
