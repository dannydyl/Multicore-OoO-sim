# Tools and libraries — what we use and why

Quick guide to every external piece of software the project relies on.
Written for someone who knows architecture but hasn't worked deeply
with the C++ ecosystem or the DBI/tracing world. Each section says:
*what it is*, *why it's the choice we made*, *what it would feel like
to use it from scratch*.

---

## CMake (build system)

### What it is

A meta-build tool: you write one `CMakeLists.txt` describing your
project ("here are the source files, here are the dependencies") and
CMake generates the actual build files for whatever build system your
machine has — `make` on Linux, `ninja` if you have it, MSVC `.sln`
files on Windows, Xcode projects on macOS. The same `CMakeLists.txt`
works everywhere; you just tell CMake which generator you want.

CMake is the de facto standard for any modern C++ project that needs
to build on more than one platform. Alternatives like Bazel or Meson
exist; CMake won by inertia and ecosystem.

### Why we use it

- **Cross-platform.** Linux + macOS CI on every push, no hand-rolled
  Makefile-per-OS.
- **`FetchContent`.** Lets us pull external libraries directly from
  their git repos at configure time, with a known-good commit pinned.
  We don't need to vendor source code or rely on the user having
  package managers like `vcpkg` or `conan` set up. Three external
  libraries (CLI11, nlohmann/json, Catch2) all come down this way.
  See [cmake/Dependencies.cmake](../cmake/Dependencies.cmake).
- **Targets.** Each subsystem (`casim_cache`, `casim_predictor`,
  `casim_common`, etc.) is a CMake target with its own include paths
  and dependencies. The top-level `sim` binary just lists the targets
  it links against — no manual archive-flag wrangling.
- **Presets.** [CMakePresets.json](../CMakePresets.json) defines named
  configurations (`default`, `release`, `asan`, `ci`). One command:
  `cmake --preset default && cmake --build --preset default`. No
  `cd build && cmake -DCMAKE_BUILD_TYPE=... -G Ninja ..`.

### One-line summary
> "Tells your compiler what to compile, on whatever OS or compiler you
> happen to have."

---

## CLI11 (command-line argument parsing)

### What it is

A header-only C++ library for parsing command-line flags. You write:

```cpp
CLI::App app{"sim - my simulator"};
std::filesystem::path config_path;
app.add_option("--config", config_path, "JSON config")->required();
CLI11_PARSE(app, argc, argv);
```

…and you get parsed flags, error messages on bad input, auto-generated
`--help` output. Validation (`->check(CLI::PositiveNumber)`,
`->check(CLI::ExistingFile)`) is built in.

### Why we use it

- **No `argv` parsing by hand.** Project2's CLI used `getopt` with a
  growing `case` statement; project1's accepted no flags at all. Both
  approaches break the moment you add the 30-flag config sweep we'd
  need.
- **Header-only.** One `FetchContent` line, no separate library to
  manage.
- **Auto-generated `--help`.** Falls out for free.

CLI11 is used in [src/common/cli.cpp](../src/common/cli.cpp) (the main
sim CLI), [tools/gen_trace/main.cpp](../tools/gen_trace/main.cpp), and
[tools/proj2_to_champsim/main.cpp](../tools/proj2_to_champsim/main.cpp).

### One-line summary
> "Handles `--flag value` style argument parsing without you having
> to write the parser."

---

## nlohmann/json (JSON parsing)

### What it is

The de facto JSON library for modern C++. You write `nlohmann::json j
= /* parse from file */;` and operate on the result with `j["key"]`,
`j.at("key").get<int>()`, etc. It also supports custom types — define
`to_json` and `from_json` overloads for your struct and `j.get<MyStruct>()`
just works.

### Why we use it

- **JSON-driven configs.** [src/common/config.cpp](../src/common/config.cpp)
  defines `from_json` for every config struct (`SimConfig`,
  `CacheLevelConfig`, `PredictorConfig`, etc.). Loading a config is
  literally `j.get<SimConfig>()`.
- **Round-trippable.** Same struct → JSON → struct gives identical
  output. Makes it trivial to dump the merged config (after CLI
  overrides) for the user to inspect.
- **Header-only.** Same `FetchContent` story as the others.

The only mild downside: nlohmann/json is *slow* compared to RapidJSON
or simdjson. For config files that's irrelevant — we parse one tiny
JSON file at startup.

### One-line summary
> "Parses JSON files into C++ structs."

---

## Catch2 (unit testing framework)

### What it is

A C++ testing framework. You write tests like:

```cpp
TEST_CASE("SaturatingCounter walks the Smith state diagram", "[counter]") {
    SaturatingCounter c(2, 1);
    REQUIRE(c.is_weak());
    c.update(true);
    REQUIRE(c.is_taken());
}
```

…and Catch2 discovers them, runs them, prints colorized pass/fail with
expression rewriting (`with expansion: 0 == 1` instead of just `failed`).
The `SECTION { ... }` mechanism lets you group related assertions in a
test case while resetting state between sections.

### Why we use it

- **Header-only and zero-config.** One `#include`, one
  `Catch2WithMain` link, you're done.
- **CTest integration.** `catch_discover_tests(target)` in
  `CMakeLists.txt` registers every Catch2 test case as an individual
  CTest target. `ctest --preset default` runs them all and reports
  pass/fail counts.
- **Sections.** Lets per-test-case setup happen once but assertions
  branch into multiple scenarios — useful for the predictor tests
  where we share a `cfg` across "all-taken", "alternating", "period-5"
  variants.

Alternative was Google Test (`gtest`); Catch2 wins on lower friction
for a single-engineer hobby project.

Tests live under [tests/](../tests/). At the end of Phase 3 there are
62 passing tests.

### One-line summary
> "Lets you write `REQUIRE(expression)` inside `TEST_CASE`s and
> reports which ones fail."

---

## ChampSim trace format

Already covered in detail in
[02-phase1-traces.md](02-phase1-traces.md). Key facts:

- **Binary format** — one fixed-size C struct per instruction record,
  packed, little-endian, optionally `xz`-compressed.
- **Fields**: `ip`, `is_branch`, `branch_taken`, fixed-size arrays of
  destination/source register IDs and memory addresses.
- **Two record-shape variants** — `input_instr` (the SPEC default,
  ~64 B) and `cloudsuite_instr` (for IPC-1 server traces, ~56 B).
- **De facto standard** for trace-driven academic comparch research.

Repository: [github.com/ChampSim/ChampSim](https://github.com/ChampSim/ChampSim).
Methodology paper: [arXiv:2210.14324](https://arxiv.org/pdf/2210.14324).

Why we adopted it: free interoperability with the existing public
trace corpora (DPC-3, CRC-2, IPC-1, CVP-1).

### One-line summary
> "The on-disk format every research-grade trace-driven simulator in
> this niche reads."

---

## DynamoRIO (planned tracer, Path A)

### What it is

A **dynamic binary instrumentation (DBI)** framework. Lets you run an
unmodified compiled binary while intercepting every basic block of
machine code at runtime. You can read, modify, or count every load,
store, and branch as it happens — without touching source code, without
recompiling, without a debugger.

DynamoRIO ships with a built-in trace collector called `drmemtrace`
that records exactly the (PC, opcode, mem_addr, branch_outcome) stream
that microarchitectural simulators need. Run the program once under
DynamoRIO, get a multi-GB trace file out, replay it through the
simulator forever after.

License: BSD. Cross-platform: Linux, macOS, Windows. Architectures: x86,
x86-64, AArch64. Actively maintained at Google.

Repository: [github.com/DynamoRIO/dynamorio](https://github.com/DynamoRIO/dynamorio).
Trace format spec: [drcachesim format](https://dynamorio.org/sec_drcachesim_format.html).

### How DBI works (intuition)

When you launch a program normally, the OS loads the binary's machine
code into memory and the CPU executes it directly. When you launch
the same program through `drrun`, DynamoRIO inserts itself between the
program and the CPU:

1. DynamoRIO copies a basic block of the program's machine code into
   its own "code cache".
2. It modifies the copy — for `drmemtrace`, that means inserting a
   small bit of instrumentation that records every load, store, or
   branch.
3. The modified copy executes on the CPU. The program never knows.
4. When control flow leaves the basic block, DynamoRIO catches it and
   instruments the next block.

The entire program runs through this filter. The cost is ~5-50× slower
than native execution, but the recorded stream is faithful to a real
binary on a real ISA.

### Why we want it (and why it's deferred)

For Phase 5 (multi-core coherence) and Phase 6 (publishable results),
we need real workload traces — SPEC, multithreaded micro-benchmarks,
maybe a subset of a real application. Synthetic traces stop being
convincing.

DynamoRIO `drmemtrace` is the modern, open-source way to get those
traces. The plan (described in
[02-phase1-traces.md](02-phase1-traces.md) §"Path A") is:

```bash
drrun -t drcachesim -offline -- ./my_program args
tools/tracer/drmem2champsim out.champsimtrace drmemtrace.*.zip
```

A small post-processor (`drmem2champsim`, not yet written) converts
DynamoRIO's native trace into our canonical ChampSim binary format.

It's deferred because Phase 3 didn't need it: synthetic + project2-derived
traces were enough for predictor cross-validation.

### One-line summary
> "Lets you record a (PC, mem-addr, branch-outcome) trace from any
> compiled binary without modifying it."

---

## Intel Pin (compatibility tracer, Path B)

### What it is

The other big DBI framework. Older than DynamoRIO, originally from
Intel Research, still widely used in academic comparch work because
it's been the default for so long.

License: proprietary EULA, free binary download. Cross-platform: Linux
+ Windows, primarily x86. Less actively developed than DynamoRIO.

ChampSim's official tracer is a Pintool — the file
`tracer/champsim_tracer.cpp` in the [ChampSim repo](https://github.com/ChampSim/ChampSim)
is what produced the original DPC-3 / CRC-2 trace corpora. If we want
to re-trace SPEC binaries we already have, the simplest path is to
run that exact Pintool, not to re-implement the tracing logic.

### Why we plan to vendor it

For two reasons:

1. **Some platforms install Pin more cleanly than DynamoRIO.** A
   teaching-machine where Pin is already set up is a better path than
   bringing up DynamoRIO from scratch.
2. **The DPC-3 / CRC-2 corpora were generated by this exact tool.**
   Bit-for-bit reproducibility of public trace files means using the
   same tracer that produced them.

We won't reimplement the Pintool — we'll vendor (git-submodule or
pinned tarball) the upstream `champsim_tracer.cpp` under
[tools/tracer/pin_champsim/](../tools/tracer/) and add a thin CMake
glue to build it on Pin-friendly platforms.

Status: deferred alongside the DynamoRIO path.

### One-line summary
> "The legacy DBI framework used to generate the public ChampSim trace
> corpora. We'll vendor its tracer for compatibility."

---

## Public trace corpora

These are the trace collections we plan to plug in once the
DynamoRIO/Pin paths are wired. We don't have to host any of them; they
live on third-party servers and we just pull representative ones into
`traces/` for local runs.

| Corpus     | What                                                    | Host                                                                                       |
| ---------- | ------------------------------------------------------- | ------------------------------------------------------------------------------------------ |
| **DPC-3**  | SPEC CPU2006 + CPU2017 memory-intensive subset           | [dpc3.compas.cs.stonybrook.edu](https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/) |
| **CRC-2**  | Same SPEC mix, used for the cache replacement championship | (linked from CRC-2 page)                                                                  |
| **IPC-1**  | Server / front-end-heavy: Cassandra, Drupal, Kafka, MySQL | [research.ece.ncsu.edu/ipc/rules-criteria](https://research.ece.ncsu.edu/ipc/rules-criteria/) |
| **CVP-1 → ChampSim** | Qualcomm's 135 small + 2013 large traces, ported by Feliu et al. | [zenodo:14045186](https://zenodo.org/records/14045186), [Feliu IISWC 2023](https://webs.um.es/aros/papers/pdfs/jfeliu-iiswc23.pdf) |

If a reviewer ever asks "but how does your simulator do on real
workloads?", these are the answer.

---

## GitHub Actions (CI)

### What it is

Free CI/CD service that runs jobs in a fresh VM every time you push.
Configuration lives in a YAML file under `.github/workflows/`.

### Why we use it

[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) is set up to
build the project and run the test suite on Linux and macOS for every
push. Catches "works on my machine" regressions — anything that
compiles only because my local toolchain has weird headers will fail
in CI immediately.

Free for public repos, generous quota for private. Costs nothing to
keep running.

### One-line summary
> "A robot that builds and tests your project on every push so you
> notice breakage before merging."

---

## Quick reference: which tool covers which need

| Need                                  | Tool                             | Where                                                             |
| ------------------------------------- | -------------------------------- | ----------------------------------------------------------------- |
| Compile across Linux/macOS            | CMake + presets                  | [CMakeLists.txt](../CMakeLists.txt), [CMakePresets.json](../CMakePresets.json) |
| Parse `--flag` arguments              | CLI11                            | [src/common/cli.cpp](../src/common/cli.cpp)                       |
| Load JSON config files                | nlohmann/json                    | [src/common/config.cpp](../src/common/config.cpp)                 |
| Write unit tests                      | Catch2 (+ CTest discovery)       | [tests/](../tests/)                                                |
| On-disk trace format                  | ChampSim binary spec             | [docs/trace-format.md](../docs/trace-format.md)                   |
| Trace from a real binary (Linux/macOS, modern) | DynamoRIO `drmemtrace` + `drmem2champsim` | (Phase 5; not yet written)                          |
| Trace from a real binary (Linux, legacy) | Intel Pin + ChampSim Pintool   | (Phase 5; not yet vendored)                                       |
| Synthesize a small trace for tests    | `tools/gen_trace`                | [tools/gen_trace/](../tools/gen_trace/)                           |
| Convert project2 text trace → ChampSim | `tools/proj2_to_champsim`        | [tools/proj2_to_champsim/](../tools/proj2_to_champsim/)           |
| Run tests on every push               | GitHub Actions                   | [.github/workflows/ci.yml](../.github/workflows/ci.yml)            |

If you're trying to answer "how do I do X with this codebase?", look
up X in the left column.
