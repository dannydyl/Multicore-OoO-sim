# Multicore-OoO-sim

A multi-core, out-of-order, cache-coherent CMP simulator written in modern C++20.
Each core models a Tomasulo-style out-of-order pipeline with a real branch
predictor and a private L1/L2 cache hierarchy. Cores are connected through a
configurable interconnect with a directory-based coherence protocol.

```
                 +-----------------------------+
                 |        Coherent Network     |   (RING / XBAR)
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

This is a personal project under active development. The implementation is
being assembled incrementally; the current state is **Phase 0 (skeleton only)**.

## Status

- [x] Phase 0 — skeleton, build system, CLI, JSON config
- [~] Phase 1 — canonical trace format and DynamoRIO-based tracer (reader/writer + synthetic generator landed; tracer pending)
- [x] Phase 2 — L1/L2 cache subsystem
- [x] Phase 3 — branch predictors
- [x] Phase 4 — single-core out-of-order pipeline
- [x] Phase 5A — multi-core + cache coherence (`--mode coherence`; MI/MSI/MESI/MOSI/MOESIF on a RING)
- [x] Phase 5B — full multi-core OoO + coherence (the default mode; OoO cores + private L1+L2 + ring + directory)
- [ ] Phase 6 — polish, results, plots

## Build

Requires a C++20 compiler and CMake 3.21 or newer.

```sh
cmake --preset default
cmake --build --preset default -j
ctest --preset default
```

Or without presets:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Available presets: `default` (Debug), `release` (-O3), `asan` (AddressSanitizer),
`ci` (Debug + -Werror, the configuration CI runs).

The first configure downloads dependencies (nlohmann/json, CLI11, Catch2) into
`build/_deps/`. Subsequent configures are fast.

### Build options

| Option            | Default | Effect                                       |
| ----------------- | ------- | -------------------------------------------- |
| `CASIM_FAST`      | OFF     | `-O2`                                        |
| `CASIM_FASTER`    | OFF     | `-O3`                                        |
| `CASIM_ASAN`      | OFF     | AddressSanitizer (`-fsanitize=address -g`)   |
| `CASIM_PROFILE`   | OFF     | `gprof` profiling (`-pg`)                    |
| `CASIM_BUILD_TESTS` | ON    | Build unit tests                             |
| `CASIM_WERROR`    | OFF     | Treat warnings as errors (CI sets this on)   |

Pass with `-D`, e.g. `cmake -S . -B build-asan -DCASIM_ASAN=ON`.

## Usage

```sh
./build/src/sim --config configs/baseline.json
./build/src/sim --config configs/baseline.json --cores 8 --out merged.json
./build/src/sim --help
./build/src/sim --version
```

### CLI flags

| Flag             | Required | Description                                                |
| ---------------- | -------- | ---------------------------------------------------------- |
| `--config FILE`  | yes      | Machine config (JSON)                                      |
| `--trace FILE`   | no       | Workload trace (canonical format; ignored in Phase 0)      |
| `--mode MODE`    | no       | One of `full,cache,predictor,ooo,coherence`. Default `full` |
| `--cores N`      | no       | Override `cores` from the config                           |
| `--out FILE`     | no       | Write merged config JSON here instead of stdout            |
| `--log-level L`  | no       | One of `trace,debug,info,warn,error,off`. Default `info`   |
| `--version`      | no       | Print version and exit                                     |
| `--help`         | no       | Print help and exit                                        |

### Exit codes

| Code | Meaning                                  |
| ---- | ---------------------------------------- |
| 0    | Success                                  |
| 1    | CLI parse error / usage problem          |
| 2    | Config error (bad JSON, missing fields)  |
| 3    | I/O error (file unreadable, etc.)        |

## Repository layout

```
include/comparch/   public headers
src/                per-subsystem static libraries + main.cpp
tools/tracer/       DynamoRIO client for trace generation (Phase 1)
configs/            JSON machine configs
workloads/          small example programs we trace ourselves
traces/             canonical traces (small ones checked in for tests)
tests/              Catch2 unit + integration tests
docs/               architecture, trace format, benchmarks
cmake/              CMake helpers
```

## Development

```sh
clang-format -i $(find src include tests -name '*.hpp' -o -name '*.cpp')
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
```

CI runs the same configure/build/test sequence on Linux and macOS for both Debug
and Release. See [`.github/workflows/ci.yml`](.github/workflows/ci.yml).

## License

[MIT](LICENSE).
