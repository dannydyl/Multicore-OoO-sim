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

And the standalone:

7. **[code-review.md](code-review.md)** — senior-level code review
   over Phases 0-3. Bugs (none found that affect output), logic /
   model-fidelity issues (some), and Phase 4 architectural debts
   (two important ones).

## Status snapshot

| Phase | Subject                                | Status      | Tests passing |
| ----- | -------------------------------------- | ----------- | ------------- |
| 0     | Skeleton & infrastructure              | ✅ done     | n/a           |
| 1     | Trace format & tracer                  | partial     | 5             |
| 2     | Cache (`--mode cache`)                 | ✅ done     | 41 (incl. 4 proj1 regressions) |
| 3     | Branch predictor (`--mode predictor`)  | ✅ done     | 11 (incl. 1 proj2 regression with 4 sections) |
| 4     | OoO core (`--mode ooo`)                | not started | —             |
| 5     | Multi-core + coherence                 | not started | —             |
| 6     | Polish & public release                | not started | —             |

Total tests: **62 / 62 passing** (April 2026).

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
