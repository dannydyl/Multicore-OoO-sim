# Phase 0 — Skeleton & infrastructure

**Goal:** stand up enough scaffolding that future phases land into a
real codebase, not a pile of scripts. No simulation behavior yet. Each
piece exists to remove a future bottleneck.

## What landed

| Piece                  | Where it lives                                         | What it does                                                                                                                                                                                                                                                |
| ---------------------- | ------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Repo layout            | `src/{common,cache,predictor,ooo,coherence,core}/`, `include/comparch/`, `tools/`, `tests/`, `configs/`, `traces/`, `workloads/` | One directory per subsystem. New code goes into the right place by name; nobody has to ask "where does this go?"                                                                                                                                            |
| Build system           | [CMakeLists.txt](../CMakeLists.txt), [CMakePresets.json](../CMakePresets.json), [cmake/Dependencies.cmake](../cmake/Dependencies.cmake) | CMake 3.20+ project, C++20 across the tree. Presets define `default`, `release`, `asan`, `ci`. Three external deps fetched via FetchContent: nlohmann/json (config), CLI11 (CLI parsing), Catch2 (tests). One-command build everywhere.                     |
| Logging                | [include/comparch/log.hpp](../include/comparch/log.hpp), [src/common/log.cpp](../src/common/log.cpp) | `LOG_INFO`, `LOG_WARN`, `LOG_ERROR` macros with run-time level filtering. Cheap; nothing fancy.                                                                                                                                                             |
| Stats aggregator stub  | [include/comparch/stats.hpp](../include/comparch/stats.hpp) | Placeholder. Each subsystem has its own stats struct today; Phase 6 unifies them.                                                                                                                                                                           |
| CLI parser             | [src/common/cli.cpp](../src/common/cli.cpp), [include/comparch/cli.hpp](../include/comparch/cli.hpp) | `--config <path>`, `--trace <path>`, `--mode <full\|cache\|predictor\|ooo\|coherence>`, `--out`, `--cores`, `--log`. Powered by CLI11.                                                                                                                       |
| JSON config            | [src/common/config.cpp](../src/common/config.cpp), [include/comparch/config.hpp](../include/comparch/config.hpp), [configs/baseline.json](../configs/baseline.json) | One `SimConfig` struct populated from a JSON file. Sub-structs for `interconnect`, `memory`, `core`, `l1`, `l2`, `predictor`, `coherence`. Powered by nlohmann/json. Round-trippable: load → modify → dump.                                                  |
| Catch2 unit tests      | [tests/](../tests/) , [tests/CMakeLists.txt](../tests/CMakeLists.txt)        | Per-subsystem executables (`test_config`, `test_trace`, `test_cache`, `test_predictor`, etc). Discovered automatically by CTest via `catch_discover_tests()`.                                                                                                |
| GitHub Actions CI      | [.github/workflows/ci.yml](../.github/workflows/ci.yml) | Builds + runs the test suite on Linux and macOS on every push. Catches "works on my machine" regressions before they land.                                                                                                                                  |

## Why this matters

The shape of the repo is now stable. Adding a new mode means dropping
one source file into `src/<subsystem>/`, one test file into
`tests/<subsystem>/`, and one config block into `baseline.json`. No
rearchitecting needed when each subsequent phase lands.

## Key design choices

### CMake + presets, not Make
Project1/2 used hand-rolled Makefiles. CMake gives:

- One generator output (`make`, `ninja`, IDE projects, MSVC) without
  changing source files.
- `FetchContent` so we don't vendor third-party libraries; they're
  cloned and built as part of the tree.
- Presets that turn `cmake --preset default` into one command for both
  configure-and-build, vs. the multi-step `mkdir build && cd build &&
  cmake -DCMAKE_BUILD_TYPE=Debug -G Ninja ..` dance.

See [05-tools-and-libraries.md](05-tools-and-libraries.md) for what
CMake actually is and why it's the de facto standard for C++.

### JSON config, not CLI flags

Project2's CLI accepted ~12 flags. Project1 hard-coded constants. The
unified sim needs ~30 knobs across cache geometry, replacement policies,
prefetchers, branch predictors, ROB size, coherence protocol, etc. Two
problems with stuffing all of those into the CLI:

1. **Reproducibility.** A JSON file is the experiment. Check it into
   git, share it with collaborators, sweep it with a script. CLI args
   leave no audit trail.
2. **Sweeping.** Want to compare 20 cache sizes? Generate 20 JSON files
   programmatically, run them in a loop. With CLI args you'd be
   building bash strings; with JSON you generate machine-readable
   manifests.

CLI flags still exist for one-off tweaks (`--cores 8` overrides the
JSON), but they live alongside the config, not instead of it.

### One binary, mode-dispatched

`sim --mode cache` and `sim --mode predictor` are the same binary. Two
arguments for shipping one executable instead of `sim_cache`,
`sim_predictor`, `sim_full`:

- All subsystems link together. No risk of a config struct or trace
  reader subtly drifting between mode-specific binaries.
- The mode flag is the user-visible artifact of the "isolation"
  decision in plan.md §4.2: every subsystem can be tested in isolation
  while sharing all the surrounding code paths.

### Catch2 + CTest, not gtest

Catch2 was picked for one reason: header-only, single FetchContent
include, no extra steps. gtest works fine but pulls in absl/protobuf
transitively in some configurations. For a single-engineer hobby
project, Catch2's lower friction wins.

## What's deliberately not in Phase 0

- **Stats aggregator implementation.** Each subsystem has its own
  ad-hoc stats today (`CacheStats`, `Stats` in `predictor_mode.cpp`,
  etc.). Phase 6 collapses them into one aggregator with optional JSON
  output. Premature now.
- **Architecture documentation.** Phase 6 ships a `docs/architecture.md`
  with timing diagrams. The plan.md file is enough scaffolding for
  Phases 0–5.
- **Public visibility.** Repo stays private until Phase 6 polish.

## Verification

```bash
cmake --preset default && cmake --build --preset default
ctest --preset default
```

Phase 0 ended with: empty subsystems but a working build, a CLI that
parses, a JSON loader, a logging utility, and a CI pipeline. From this
point onward every phase landed against a stable foundation rather than
having to invent its own.

## Files of note

- [CMakeLists.txt](../CMakeLists.txt) — top-level build config.
- [CMakePresets.json](../CMakePresets.json) — predefined build configurations.
- [configs/baseline.json](../configs/baseline.json) — the canonical
  example config; every other config is a delta from this one.
- [include/comparch/config.hpp](../include/comparch/config.hpp) — the
  `SimConfig` struct definition. Source of truth for what knobs exist.
- [src/main.cpp](../src/main.cpp) — the one entry point. Mode dispatch
  lives here.
