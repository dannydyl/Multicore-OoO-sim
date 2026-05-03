# Report — `Multicore-OoO-sim`

Phase-by-phase write-up of `Multicore-OoO-sim` from inception through
the end of Phase 3 (April 2026). Written for a reader who knows
computer architecture but hasn't necessarily worked with the C++
ecosystem (CMake, Catch2, CLI11, nlohmann/json) or with the
trace-driven simulator world (ChampSim, DynamoRIO, Pin).

## Reading order

If you're new to the project, start here:

1. **[00-overview.md](00-overview.md)** — what this project is, the
   architecture diagram, the phased roadmap, the runnable status.
2. **[05-tools-and-libraries.md](05-tools-and-libraries.md)** —
   one-page-per-tool tour of every external piece the codebase
   depends on. Read this before the phase docs if any of "CMake",
   "ChampSim", "DynamoRIO" feels unfamiliar.

Then the phase reports, in order:

3. **[01-phase0-skeleton.md](01-phase0-skeleton.md)** — Phase 0:
   skeleton, build system, JSON config, CLI, logging, CI. No simulator
   behavior yet, but the foundation everything else lands on.
4. **[02-phase1-traces.md](02-phase1-traces.md)** — Phase 1: trace
   format decision (ChampSim binary), reader/writer, synthetic
   generator, tracer plan. Includes a deeper explainer of why traces
   matter, what dynamic binary instrumentation is, and the public
   academic trace corpora.
5. **[03-phase2-cache.md](03-phase2-cache.md)** — Phase 2: cache
   subsystem (`--mode cache`). Geometry, replacement policies (LRU,
   LIP, MIP), write policies (WBWA, WTWNA), prefetchers (+1, Markov,
   Hybrid). Pinned bit-for-bit against project1 reference output.
6. **[04-phase3-predictor.md](04-phase3-predictor.md)** — Phase 3:
   branch predictors (`--mode predictor`). Always-Taken, Yeh-Patt
   two-level adaptive, Perceptron, Hybrid tournament. Pinned
   bit-for-bit against project2's `proj2sim` binary.

And the standalone reviews:

7. **[code-review.md](code-review.md)** — senior-level code review
   over Phases 0-3. Bugs (none found that affect output), logic /
   model-fidelity issues (some), and Phase 4 architectural debts
   (two important ones).
8. **[06-phase4-review.md](06-phase4-review.md)** — second review pass
   covering Phase 4A (cache MSHR) and Phase 4B (OoO core). Four
   bug-class findings that all live in the seam between subsystems —
   hybrid predictor under multi-branch-in-flight, `Cache::issue()`
   side-effect leak on MSHR-full, OoO LSU busy-loop on the same stall,
   and `--mode coherence` silent exit. All fixed in the same pass with
   tests pinning each regression.
9. **[07-phase5a-coherence.md](07-phase5a-coherence.md)** — Phase 5A:
   `--mode coherence`. Five protocols (MI / MSI / MESI / MOSI /
   MOESIF), RING network, directory, FICI per-core trace driver.
   Pinned bit-for-bit against project3's `dirsim` reference outputs
   for `(MSI / MESI / MOSI / MOESIF) × (4 / 8 / 12 / 16 cores)` —
   16 / 16 combos.
10. **[08-phase5b-full.md](08-phase5b-full.md)** — Phase 5B: default
    invocation runs the full multi-core OoO + coherence simulator. N
    OoO cores, each with private finite L1+L2, all connected through
    the Phase 5A ring + directory + agents. The simulator engine is
    feature-complete after this phase.
11. **[09-config-sweep.md](09-config-sweep.md)** — post-5B
    configuration sweep: ~30 configs across cores / protocols / cache
    geometry / DRAM latency / OoO width / predictor. No deadlocks.
    Headline finding: aggregate IPC saturates at ~0.042 by 4 cores —
    the single ring + single directory + single DRAM channel is the
    bottleneck.

## Status snapshot

| Phase | Subject                                | Status      | Tests passing |
| ----- | -------------------------------------- | ----------- | ------------- |
| 0     | Skeleton & infrastructure              | ✅ done     | n/a           |
| 1     | Trace format & tracer                  | partial     | 5             |
| 2     | Cache (`--mode cache`)                 | ✅ done     | 41 (incl. 4 proj1 regressions) |
| 3     | Branch predictor (`--mode predictor`)  | ✅ done     | 11 (incl. 1 proj2 regression with 4 sections) |
| 4     | OoO core (`--mode ooo`)                | ✅ done     | 8 (basic) + 4 from cache/predictor regressions covering Phase-4 fixes |
| 5A    | Coherence (`--mode coherence`)         | ✅ done     | 20 (4 banner + 5 fici + 5 network/message + 3 MI synthetic + 4 proj3 regression cases × multi-core fan-out) |
| 5B    | Multi-core OoO + coherence (default)   | ✅ done     | 18 (5 CLI dispatch + 4 coherence-sink + 5 WRITEBACK directory + 4 full-mode integration) |
| 6     | Polish & public release                | not started | —             |

Total tests: **126 / 126 passing**.

## Build / run cheat sheet

```bash
# Configure + build
cmake --preset default
cmake --build --preset default

# Run cache mode
./build/default/sim --mode cache \
    --config configs/baseline.json \
    --trace tests/cache/fixtures/proj1/short_gcc.trace

# Run predictor mode
./build/default/sim --mode predictor \
    --config configs/baseline.json \
    --trace tests/predictor/fixtures/proj2/branchsim.champsimtrace

# Run all tests
ctest --preset default
```
