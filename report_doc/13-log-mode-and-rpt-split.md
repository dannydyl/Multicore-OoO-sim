# LOG=1 trace mode and multi-file report split

**Status:** done.
**Files touched:** [src/full/full_mode.cpp](../src/full/full_mode.cpp), [src/ooo/core.cpp](../src/ooo/core.cpp), [include/comparch/ooo/core.hpp](../include/comparch/ooo/core.hpp), [src/ooo/CMakeLists.txt](../src/ooo/CMakeLists.txt). New: [include/comparch/ooo/trace_logger.hpp](../include/comparch/ooo/trace_logger.hpp), [src/ooo/trace_logger.cpp](../src/ooo/trace_logger.cpp).
**Tests:** 128 / 128 passing on `build-release` and `build-werror-rel`.

---

## 1. Why this was needed

Two complaints, one investigation, then two changes:

1. *"Cache hit rate looks suspiciously low on some traces."* I audited the cache + coherence + OoO integration and the answer turned out to be **the workload, not a bug** (see §2). But to make this easy for anyone to verify on their own runs, I added a per-instruction execution log so the user can read off exactly what the cache does on the early access stream.
2. *"Split `report.rpt` into more focused files with deeper academic-style stats."* The previous report.rpt was one ~80-line dump with header / config / per-core / system glued together. That made diffing two runs noisy and hid the metrics worth tracking (AMAT, MPKI, occupancy %, branch accuracy, …). The split below mirrors how gem5 / ChampSim / ZSim organize their per-run output.

---

## 2. Code review — is the simulator under-counting hits?

**No.** I traced the cache → coherence path end-to-end and the bookkeeping is correct.

| What I checked | Result |
| --- | --- |
| `Cache::access()` increments `accesses` exactly once per call | ✓ |
| `Cache::issue()` calls `access()` once on a fresh allocate; merge fast-path skips `access()` entirely so secondaries are not double-counted as misses | ✓ ([cache.cpp:437-485](../src/cache/cache.cpp#L437-L485)) |
| L1 miss → L2 access → coherence sink path doesn't lose the fill: adapter calls `cache_fill(L2)` and `cache_fill(L1)` on DATA arrival, and `mark_block_ready` flips every MSHR slot tied to that block | ✓ ([coherence_adapter.cpp:108-130](../src/coherence/coherence_adapter.cpp#L108-L130)) |
| Tag/index used to fill matches what `access()` looks up later (block-index form, idempotent through `block_in()`) | ✓ |

So a 100% miss rate on `traces/core_4/p0` looked alarming, but it's the trace's own property. I dumped the trace and counted unique 64 B blocks against memory-op count:

| Trace | mem ops | unique 64 B blocks | reuse |
| --- | ---: | ---: | ---: |
| `traces/core_4/p0.champsimtrace` | 458 | 458 | **0.0 %** |
| `synth/sequential_tiny` | 34,039 | 34,039 | 0.0 % |
| `synth/stream_tiny` | 34,042 | 34,042 | 0.0 % |
| `synth/random_tiny` | 34,382 | 32,270 | 6.1 % |
| `synth/loop_tiny` | 34,114 | 64 | **99.8 %** |

`loop_*` is the only family with temporal locality by construction, and on it we observe `L1 miss rate = 0.0019` — exactly what the validation report ([10-validation-bugs.md](11-validation-bugs.md), line ~101) predicts. So the headline number is reporting reality.

### Two model-fidelity items I noticed (neither affects hit rate)

- **OoO under-counts mem traffic vs. cache mode.** [src/ooo/inst.cpp:50-61](../src/ooo/inst.cpp#L50-L61) takes only the *first* non-zero entry of `source_memory[]` / `destination_memory[]`. A ChampSim record can encode 4 sources / 2 destinations (e.g. `rep movs`-shaped instructions). `--mode cache` iterates all of them; OoO collapses to one. Doesn't change miss *rate*, but means OoO accesses `≤` cache-mode accesses for the same trace.
- **Coherence-managed write-allocate fills clean.** [coherence_adapter.cpp:115-121](../src/coherence/coherence_adapter.cpp#L115-L121) calls `cache_fill(... 'R')` even when the original miss was a `Op::Write`. The comment promises a follow-up `Op::Write` to set dirty, but no follow-up exists in the codebase — so stores via the coherence path may not mark the line dirty, which can under-count later writebacks. Acknowledged in the comment but unresolved.

Filed for later — not blocking.

---

## 3. `LOG=1` execution-trace mode

### 3.1 What it does

When the simulator runs full mode and the environment variable `LOG` is set to anything truthy (`1`, `on`, `true`, `yes`, or any non-empty value other than `0`/`off`/`false`/`no`), it writes a `log.rpt` file to the run directory containing a per-instruction execution trace for each core's first 50 dynamic instructions.

```bash
# Default — no log.rpt:
./build-release/src/sim --config configs/baseline.json --trace-dir traces/core_4

# Logged:
LOG=1 ./build-release/src/sim --config configs/baseline.json --trace-dir traces/core_4
#                                                 -> report/.../log.rpt
```

### 3.2 What's in the file

Two event kinds, both prefixed with `[c<core> cy=<cycle> dyn=<dyn>]`:

- **`LSU      <op> pc=… addr=…  -> L1 <hit|miss>`** — fired when a load or store enters the L1 MSHR. The hit/miss outcome is the synchronous result `Cache::access()` reported when `Cache::issue()` allocated the slot, captured via `MSHR::peek(id)->result.hit`. So the user sees how the cache responded *at issue time* on the early access stream.
- **`RETIRE   <op> pc=… [addr=…] [branch info]`** — fired when an instruction commits at the head of the ROB. Includes branch outcome and mispredict flag for branch instructions, and the memory address for loads/stores.

Subtracting an instruction's `LSU` cycle from its `RETIRE` cycle gives the observed memory latency for that access, which is exactly what the user wanted: "how the cache is acting" on early instructions.

### 3.3 Sample (truncated)

```
# Multicore OoO Simulator -- per-core execution trace
# trace    : traces/core_4
# protocol : MESI_PRO
# window   : first 50 dynamic instructions per core (dyn_count <= 50)
# format   : [c<core> cy=<cycle> dyn=<dyn>] <event> ...
…
[c0 cy=00000002 dyn=00003] LSU      LOAD  pc=0x400008 addr=0x10000080  -> L1 miss
[c2 cy=00000002 dyn=00002] LSU      LOAD  pc=0x400004 addr=0x10000040  -> L1 miss
[c1 cy=00000004 dyn=00001] RETIRE   BRANCH pc=0x400000  branch=T pred=N  *MISPRED*
[c0 cy=00000223 dyn=00003] RETIRE   LOAD   pc=0x400008 addr=0x10000080
[c0 cy=00000644 dyn=00006] RETIRE   BRANCH pc=0x400014  branch=T pred=N  *MISPRED*
```

The dyn=00003 load above issued at cycle 2 and retired at cycle 223 → 221 cycles of memory latency, consistent with a 100-cycle DRAM round-trip plus ring + directory hops.

### 3.4 Implementation

**[include/comparch/ooo/trace_logger.hpp](../include/comparch/ooo/trace_logger.hpp)** declares a small `TraceLogger` class:

```cpp
class TraceLogger {
public:
    TraceLogger(std::ostream& out, std::size_t cores, std::size_t max_per_core = 50);

    void write_header(const std::string& trace_label, const std::string& proto_label);
    bool active(int core_id, std::uint64_t dyn_count) const;
    void on_lsu_issue(int core_id, std::uint64_t cycle, std::uint64_t dyn_count,
                      std::uint64_t pc, std::uint64_t addr, bool is_load, bool hit);
    void on_retire(int core_id, std::uint64_t cycle, std::uint64_t dyn_count,
                   std::uint64_t pc, const char* opcode, std::uint64_t mem_addr,
                   bool is_branch, bool taken, bool predicted_taken, bool mispredict);
};
```

The budget is a simple `dyn_count <= max_per_core_` check inside `active()`, so the same instruction's issue *and* retire both make it through as long as it's in the first 50 of its core. No per-core counter needed.

**`OooCore`** ([include/comparch/ooo/core.hpp](../include/comparch/ooo/core.hpp)) gained one accessor:

```cpp
void set_trace_logger(TraceLogger* logger, int core_id);
```

Default is null; existing test sites and `--mode ooo` are unaffected.

**Hook sites** in [src/ooo/core.cpp](../src/ooo/core.cpp):

- `stage_state_update` — both retire paths (mispredicted-branch retire + regular retire) call `trace_logger_->on_retire(...)` if the logger is set.
- `stage_schedule` — after `l1d_->issue(req)` succeeds, peek the just-allocated MSHR slot and call `trace_logger_->on_lsu_issue(...)` with `mshr->result.hit`.

**Driver** ([src/full/full_mode.cpp](../src/full/full_mode.cpp:480-520)):

- `log_trace_enabled()` reads `getenv("LOG")` with a permissive parse.
- The run directory is resolved up-front (so `log.rpt` can be opened *before* tick() runs) via the new `build_run_dir_pre()` helper.
- If LOG is on, an `std::ofstream log_stream` and a `std::unique_ptr<TraceLogger>` are constructed; the latter is wired into every `OooCore` via `set_trace_logger`.
- Both objects outlive the simulation loop.

The hot path overhead when LOG is off is one null-pointer compare per retire and per LSU issue. When LOG is on, formatting cost is bounded by `cores × 50 × 2 events` ≈ a few hundred lines.

---

## 4. Multi-file report split

### 4.1 What changed

Before, the run directory had:

```
report/<trace>_<proto>_c<N>_<tag>/
├── report.rpt   # everything: header + config + per-core + system
└── report.csv   # per-core scalars (sweeper input)
```

After:

```
report/<trace>_<proto>_c<N>_<tag>/
├── report.rpt        # short overview: aggregate IPC + per-core 6-line summary
├── config.rpt        # full configuration dump (no sim numbers)
├── stats.rpt         # detailed per-core: pipeline / branch / L1 / L2 / pipeline resources / stalls
├── coherence.rpt     # protocol counters + derived ratios (C2C/miss, system miss rate, total invals)
├── log.rpt           # only if LOG=1: per-instruction execution trace (first 50 per core)
└── report.csv        # unchanged — sweep tooling stays compatible
```

Stdout shows `report.rpt`'s short overview only — easier to grep across many runs.

### 4.2 Stats added (industry / academic standard)

Each metric below was either already computed but not exposed, or trivially derivable from the existing `OooStats` / `CacheStats` / `CoherenceStats` packs.

**Pipeline throughput**
- `cycles`, `instructions fetched`, `instructions retired`, `IPC`, `CPI`
- `deadlock state`: `clean` or `DEADLOCKED@<cycle>` from the per-core watchdog.

**Branch predictor**
- `branches`, `mispredictions`, **`branch accuracy %`**, **`mispredict rate %`**
- **`branches/kinst`** and `MPKI (mispred)` — the standard branch-density and branch-MPKI numbers in any architecture paper.

**Cache (per level)**
- `accesses`, `reads / writes`, `hits`, `misses`
- **`hit rate %`**, **`miss rate %`**
- **`APKI`** (accesses per kilo-instruction) and **`MPKI`** (misses per kilo-instruction) — the two pressure metrics gem5/ChampSim sweep papers report side-by-side
- **`AMAT`** — average memory access time (Hennessy & Patterson Ch. 2, two-level form `hit_lat + miss_rate × AMAT_next_level`)
- For L2: **`local miss rate`** (L2 misses / L2 accesses) and **`global miss rate`** (L2 misses / L1 accesses). The two-rate distinction is standard in cache-hierarchy sections of the architecture textbooks.
- `writebacks`, `coherence invals` (broken out from the system-wide pack)
- Prefetcher accounting (`issued / hits / misses`) shown only when a prefetcher is configured.

**Pipeline resources**
- `ROB / SchedQ / DispQ` rows reporting `avg / max / capacity / occupancy %` — gives a one-shot picture of how full each structure was during the run.

**Stalls**
- `no-fire cycles` — fraction of cycles with zero issues from `stage_schedule`.
- `ROB-full dispatch stalls` — fraction of cycles where dispatch was blocked because the ROB was full. Together these are the textbook "front-end vs. back-end stall" classification, scaled to a single core.

**Coherence (`coherence.rpt`)**
- `Cache accesses`, `Cache misses`, `Silent upgrades`, `C2C transfers`, `Memory reads`, `Memory writes` — the existing system-wide counters.
- **`C2C / miss`** — fraction of misses satisfied by a peer cache. A protocol-comparison metric.
- **`System miss rate`** — `cache_misses / cache_accesses` aggregated.
- **`Coherence invalidations`** — sum of L1+L2 `coherence_invals` across cores.

`config.rpt` is exactly the existing `write_config()` output, factored out so config sweeps can diff it without churning stats.

### 4.3 Implementation notes

- I split the old monolithic `write_rpt()` into 4 section writers (`write_overview`, `write_config_only`, `write_stats`, `write_coherence_file`), one per output file, plus a small generic `write_to(name, fn)` helper in `run_full_mode` that opens and writes each one.
- `CoreMetrics` ([src/full/full_mode.cpp:191-260](../src/full/full_mode.cpp#L191-L260)) was extended to carry the derived rates / percentages so both `stats.rpt` and `report.csv` use identical numbers.
- `report.csv` schema is unchanged — every header column from before still appears in the same order. The sweep aggregator (`scripts/aggregate.py`) will keep working without changes.

---

## 5. Test surfaces

| Test layer | Result |
| --- | --- |
| `ctest --test-dir build-release` | 128 / 128 passing |
| `ctest --test-dir build-werror-rel` | 128 / 128 passing |
| `--mode cache` regression (project1 fixtures) | unchanged; cache code untouched |
| `--mode coherence` regression (project3 fixtures × proto × cores) | unchanged; coherence code untouched |
| `tests/full/test_full_mode.cpp` (5 cases) | passing — the test that asserted `"instructions retired"` in stdout still finds it (the label is preserved in the new short overview) |
| Sanity run: `core_4` × MESI | 4 cores × 1 K instr, 100 % L1 miss as expected (workload property) |
| Sanity run: `synth/loop_tiny` × MESI | 4 cores × 100 K instr, **L1 miss rate = 0.0019** — confirms hit rate works when the trace has reuse |

---

## 6. Out of scope, surfaced for later

1. The `(Op::Write)` coherence-fill issue noted in §2 — fix would be to plumb the original op through to `cache_fill`, or to issue a synthetic dirty-marking pass in the adapter. Not addressed here.
2. The OoO multi-mem record collapse noted in §2 — would require expanding one trace record into multiple in-flight memory ops, with attendant LSU / MSHR pressure changes. Different design discussion.
3. Per-core breakdown of coherence counters (today they're system-wide). The agent FSMs already track a few of these per-block; surfacing them per core is a small refactor in `CoherenceStats`.
4. Optionally make `LOG_WINDOW` (currently 50) configurable via env var or `--log-window=N` if 50 turns out to be too small for some debugging workflow.
