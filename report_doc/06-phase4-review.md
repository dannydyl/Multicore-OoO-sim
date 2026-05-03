# Code review — Phase 4 (cache MSHR + OoO pipeline)

A second senior-engineer pass over the codebase, scoped to the work
landed after [code-review.md](code-review.md): Phase 4A (cache MSHR /
non-blocking-access API) and Phase 4B (single-core OoO pipeline +
`--mode ooo`). The earlier review's tags carry over:

| Tag       | Meaning                                                             |
| --------- | ------------------------------------------------------------------- |
| **BUG**   | Wrong output today. Should be fixed.                                |
| **LOGIC** | Behaves correctly but doesn't match how a real CPU would work.      |
| **PERF**  | Correct and fast enough today, but suboptimal for big experiments.  |
| **PHASE5**| Will block or complicate the multi-core port; mark now, fix later.  |
| **POLISH**| Minor robustness / ergonomics. Optional.                            |

The review fanned out across four parallel passes (cache, predictor, OoO
core, common infrastructure). Findings were merged, verified against
source one more time, and the bug-class items were fixed in this same
sitting.

---

## Summary

- **Four BUG findings** that all live in the seam between subsystems
  rather than inside any one piece. The OoO core, the MSHR API, and the
  hybrid predictor each work in isolation, but the way they interact
  exposed contracts that Phase 3 had no occasion to stress-test. All
  four are fixed; the changes don't move any pinned regression number.
- **One MED finding** elevated to a fix on the same change: the cache
  miss-merge fast-path silently dropped Write semantics. Not reachable
  from any current caller (loads use `issue()`, stores use synchronous
  `access()`), but a latent invariant that's cheaper to lock down now
  than to rediscover during Phase 5.
- **One PHASE5 architectural concern** documented but deferred: the
  cache hierarchy is "non-blocking" only at L1 from the caller's
  perspective. L2's MSHR never sees in-flight L1 misses, because
  `Cache::issue()` recurses synchronously through `next_level->access()`
  rather than through `next_level->issue()`. A coherence agent in Phase 5
  needs each level to hold its own miss state. This deserves a real
  redesign rather than a patch.
- **One LOGIC item left as-is by user choice:** the predictors update
  GHR / HT at retire time rather than at predict time, so wrong-path
  fetches see stale history and there's no rollback on flush. This is
  acceptable for now (the OoO core only ever has one mispredicted
  branch in flight, and `in_mispred_` blocks fetch until retire flushes
  the pipeline), but it does mean prediction quality on a real OoO
  trace will sit below what a speculative-history predictor could
  achieve. The PHASE4 marker on `BranchPredictor` already calls this
  out as the place a speculative-checkpoint API would land.
- **Two MED docs-drift items fixed:** the `Cloudsuite` variant and `.xz`
  decompression were both written up as "we support both behind a
  config flag" while the implementation only carried `Standard` /
  uncompressed. Updated to match reality.

---

## BUG fixes

### BUG — Hybrid predictor crashed `--mode ooo` once two branches were in flight
[src/predictor/hybrid.cpp](../src/predictor/hybrid.cpp),
[tests/ooo/test_ooo_basic.cpp](../tests/ooo/test_ooo_basic.cpp)
(test "Hybrid predictor handles many branches in flight simultaneously")

#### What was wrong

The Phase-3 hybrid stashed each branch's two sub-predictions in three
scalar member variables:

```cpp
bool last_yp_  = false;
bool last_pct_ = false;
bool pending_update_ = false;   // debug guard
```

The contract was "exactly one branch in flight between predict and
update." That worked perfectly for `--mode predictor`, where the driver
calls predict-then-update for every branch sequentially. It is the
wrong contract for an OoO pipeline. The OoO core fetches up to
`fetch_width` instructions per cycle and only retires them several
cycles later — so the moment a second branch is fetched while the first
is still in dispatch / sched / ROB, hybrid's `predict()` is called
again. With the old code:

- **Debug builds** (asserts active): `assert(!pending_update_)` fires on
  the second predict.
- **Release builds:** the second predict overwrites `last_yp_` and
  `last_pct_`. When the first branch finally retires, its `update()`
  trains the tournament selector against the *second* branch's
  sub-predictions. Stats are corrupt and the selector drifts.

The Phase-3 code-review actually flagged this exact pattern as a LOGIC
risk ("Hybrid caches `last_yp_` / `last_pct_` between predict and
update") and proposed only documenting the contract — which was the
right call for the time, because Phase 4 hadn't yet wired hybrid into
an OoO pipeline. Now it has.

The bug didn't surface in the Phase-4B test suite because every
`tests/ooo/*` case used `always_taken` (which has no per-branch state).
But `configs/baseline.json` defaults to `yeh_patt` with hybrid knobs
primed; any user choosing `hybrid` for `--mode ooo` would have hit the
assert immediately.

#### What changed

The single-slot scalars are gone. In their place: an
`std::unordered_map<std::uint64_t, Pending> pending_;` keyed by
`Branch::inst_num`. Every `predict()` inserts an entry; the matching
`update()` looks it up and erases it. `inst_num` is the OoO core's
monotonically-increasing dynamic-instruction count, so collisions are
impossible across the simulation.

The debug-guard semantics that `pending_update_` provided are kept as
two asserts:

```cpp
auto [it, inserted] = pending_.emplace(b.inst_num, Pending{yp, pct});
assert(inserted && "Hybrid::predict: duplicate inst_num in flight");
```
```cpp
auto it = pending_.find(b.inst_num);
assert(it != pending_.end() &&
       "Hybrid::update called without a matching predict()");
```

Same misuse-detection, now correctly scoped to "this specific branch"
rather than "any branch."

The Phase-3 cross-validation regression test
([tests/predictor/test_proj2_regression.cpp](../tests/predictor/test_proj2_regression.cpp))
still passes bit-for-bit, which is the strong evidence that the
refactor is observationally equivalent for sequential predict-update
pairs.

#### How it's tested

A new test runs 200 alternating-direction branches through the OoO
core with `narrow_4_4()` (fetch=4, rob=32, lsu=2) — guaranteeing
multiple branches simultaneously in flight. It asserts the run
completes (the old code would assert immediately) and the retire count
matches the trace. Mispredict counts aren't pinned, since the goal is
to prove the bookkeeping works, not to enshrine a specific accuracy
under one knob set.

### BUG — `Cache::issue()` mutated cache state on a request that returned `nullopt`
[src/cache/cache.cpp:406-453](../src/cache/cache.cpp#L406-L453),
[tests/cache/test_cache_basic.cpp](../tests/cache/test_cache_basic.cpp)
(test "Cache::issue does not mutate cache state when MSHR is full")

#### What was wrong

Old `Cache::issue()`:

```cpp
const AccessResult result = access(req);          // <- mutates state
const std::uint64_t due_cycle = now_ + result.latency;
const std::uint64_t id = next_id_++;
if (mshr_.allocate(...) == nullptr) {
    --next_id_;                                   // theatre
    return std::nullopt;
}
return id;
```

`access()` is the synchronous workhorse — it walks the set, splices the
LRU list, fires the prefetcher, and chains writebacks to the next
level. The MSHR-full check ran *after* all of that. So when the table
was full and `issue()` returned `nullopt`, the caller saw "request
rejected, please stall" but the cache had already promoted blocks,
fired prefetches, and possibly written back a dirty victim to L2.

This was particularly nasty because it interacted with
[BUG #3 below](#bug--ooo-lsu-busy-looped-on-an-mshr-full-stall) — a
single stalled cycle in the OoO core could re-trigger the same `access()`
side effects multiple times.

#### What changed

Reorder so the capacity check runs first. The merge fast-path stays at
the top (merging is always safe and doesn't allocate a new slot); if no
merge candidate exists, check `mshr_.full()` *before* calling
`access()`:

```cpp
// (merge scan — unchanged)

if (mshr_.full()) return std::nullopt;            // <- moved up

const AccessResult result = access(req);
const std::uint64_t due_cycle = now_ + result.latency;
const std::uint64_t id = next_id_++;
(void)mshr_.allocate(id, block_addr, req.op, req.pc, due_cycle, result);
return id;
```

We pre-checked capacity, so `allocate()` cannot return nullptr — the
`(void)` cast documents the intentional ignore.

#### How it's tested

A new unit test fills a 1-slot MSHR with one read, snapshots `stats()`,
then attempts a second `issue()` to a different block. It asserts the
return is `nullopt` *and* that:

- `stats().accesses` is unchanged
- `stats().misses` is unchanged
- The second block isn't resident (`!l1.block_in(0x2000)`)

With the old code all three checks fail.

### BUG — OoO LSU busy-looped on an MSHR-full stall
[src/ooo/core.cpp:259-345](../src/ooo/core.cpp#L259-L345),
[tests/ooo/test_ooo_basic.cpp](../tests/ooo/test_ooo_basic.cpp)
(test "OoO LSU stalls cleanly when MSHR fills")

#### What was wrong

Old structure of `stage_schedule`'s LSU loop:

```cpp
for (std::size_t i = 0; i < lsu_avail; ++i) {     // outer
    SchedEntry* oldest = ...;                     // pick oldest ready load/store
    if (!oldest) break;

    for (auto& u : lsu_) {                        // inner: find a free FU
        if (u.busy) continue;
        u.busy = true; u.is_load = ...; u.sched_ptr = oldest;
        if (u.is_load) {
            auto id = l1d_->issue(req);
            if (!id) {
                u.busy = false; u.sched_ptr = nullptr;
                break;                            // <- inner break only
            }
            u.mshr_id = *id;
        } else { ... }
        oldest->busy = true;                      // <- only set on success
        ++num_fires;
        break;
    }
}
```

When `issue()` returned `nullopt` the inner loop unwound the FU state,
broke out, and let the outer loop continue. But because `oldest->busy`
hadn't been set, the outer loop's next iteration *re-found the same
load*, re-allocated an FU, called `issue()` again against the
still-full MSHR, and unwound — burning up to `lsu_avail` wasted
attempts per cycle. Worse, every retry walked the MSHR table looking
for a merge-eligible block ([cache.cpp:413](../src/cache/cache.cpp#L413));
a transient merge candidate could nondeterministically succeed mid-loop.

This is the bug that combined badly with the previous one: each wasted
retry, in the old `Cache::issue()`, *also* mutated cache state.

#### What changed

Two changes. First, a pre-flight check at the top of each outer-loop
iteration — if the candidate is a load and the MSHR is full, stall
this cycle entirely:

```cpp
if (oldest->inst.opcode == Opcode::Load && l1d_->mshr().full()) {
    mshr_stalled = true;
    break;
}
```

Second, on the (now defensive) `issue()`-returned-nullopt path: also
break the *outer* loop, and reset `u.is_load` along with `u.busy` and
`u.sched_ptr` so the FU's state is fully rewound:

```cpp
if (!id) {
    u.busy = false; u.is_load = false; u.sched_ptr = nullptr;
    mshr_stalled = true;
    break;          // inner
}
// ...
if (mshr_stalled) break;     // outer
```

The pre-flight check makes the second path unreachable in practice —
`Cache::issue()` for a Read can only return `nullopt` when the MSHR is
full — but it stays as defense-in-depth, since a future cache subclass
might fail issue for some other reason.

#### How it's tested

A new test sends 64 independent loads (distinct block addresses, no
merging possible) through an OoO core whose L1-D has only 2 MSHR
entries and a 50-cycle hit latency. The MSHR will be saturated
continuously. The test asserts the simulator terminates within a
generous cycle cap and that all 64 loads retire. With the old code,
the simulator either burns cycles in the busy loop (slowing things
down without actually deadlocking) or — under the right merge timing —
silently fails to maintain the load order.

### BUG — `--mode coherence` and the default `--mode full` silently exited 0
[src/main.cpp:32-94](../src/main.cpp#L32-L94)

#### What was wrong

The CLI ([src/common/cli.cpp:34](../src/common/cli.cpp#L34)) accepts
five mode strings: `cache`, `predictor`, `ooo`, `coherence`, `full`.
`main()` had explicit dispatch for the first three. The other two fell
through to a config-dump branch that printed the merged config to
stdout (or `--out`), logged "no driver yet; exiting", and returned
`0`. CI scripts treating exit code as truth would have thought the run
succeeded; a user typing the wrong mode wouldn't see the error.

#### What changed

The fall-through path now logs an explicit "driver not implemented yet
(Phase 5)" error and returns `5`. The merged-config dump still happens
when `--out` is given, since there's a legitimate use case for "render
the resolved config without running anything" — but stdout output is
gone, and no run is reported as successful unless one actually
happened.

```cpp
LOG_ERROR(comparch::to_string(cli.mode)
          << ": driver not implemented yet (Phase 5)");
return 5;
```

No test covers this — main entry-point dispatch is awkward to assert
on without a process-level test harness. Manual verification:
`./build/sim --mode coherence --config configs/baseline.json` now
returns `5` and prints the expected error.

---

## MED fixes folded into the same change

### MED — Miss-merge fast-path could lose Write semantics (latent)
[src/cache/cache.cpp:413-422](../src/cache/cache.cpp#L413-L422),
[tests/cache/test_cache_basic.cpp](../tests/cache/test_cache_basic.cpp)
(test "Cache::issue rejects Write requests")

The merge fast-path skips the `access()` call and inherits the
primary's `AccessResult`. If a Write secondary merged onto a Read
primary, the dirty-bit mutation that `access()` would normally apply
under WBWA never happened. Currently unreachable — the OoO core only
calls `issue()` for loads ([core.cpp:305](../src/ooo/core.cpp#L305));
stores use synchronous `access()` directly — but it's a quiet
invariant that would surface during Phase 5 if any caller starts
treating `issue()` as a generic non-blocking memory port.

The fix is a runtime precondition: `Cache::issue()` throws
`std::invalid_argument` on `Op::Write`. A short comment in
[cache.cpp](../src/cache/cache.cpp) explains the precondition tied to
the merge semantics. The earlier code-review's pattern of preferring
`throw` over `assert` (so the check survives `-DNDEBUG`) carries
through.

A small test exercises the throw on `l1.issue({0x1000, Op::Write})`.

### MED — Documentation drift for `Cloudsuite` variant and `.xz` codec
[docs/trace-format.md](../docs/trace-format.md),
[report/02-phase1-traces.md](02-phase1-traces.md)

The trace-format spec already had an "Implementation status" callout
saying these are unimplemented, but two body paragraphs still claimed
"we support both behind a config flag" and one example used a
`.champsimtrace.xz` filename. The body claims now match reality: only
`input_instr` (Standard) is wired; `Cloudsuite` is documented for
future-compatibility but not reachable from `Variant`; only
uncompressed `.champsimtrace` streams are read today.

---

## Deferred — flagged but not fixed in this pass

### PHASE5 — `Cache::issue()` recurses through `next_level->access()`, not `next_level->issue()`
[src/cache/cache.cpp:432-433](../src/cache/cache.cpp#L432-L433)

When an L1 miss services through L2, the L1 `issue()` calls
`access()`, which calls `next_level->access()`, which (synchronously)
returns a latency. L1 stamps its MSHR slot with the combined latency
and we're done — but L2's MSHR was never told anything happened. So
L2 *appears empty* even when there are L1 misses being filled through
it. Today this matches every assertion in the test suite (which only
inspects stats), but in Phase 5 a coherence agent inspecting "what's
in flight at L2?" gets zeros.

The right fix is a real redesign: each cache level holds its own MSHR
slot and ticks together, so the directory / coherence agent sees a
consistent snapshot. That's a Phase-5-scope change, not a Phase-4
patch. Marked here so it doesn't get re-flagged in three weeks.

### LOGIC — Speculative history not maintained (perceptron, yeh_patt)
[src/predictor/perceptron.cpp:64-79](../src/predictor/perceptron.cpp#L64-L79),
[src/predictor/yeh_patt.cpp:72](../src/predictor/yeh_patt.cpp#L72)

Both predictors update their history register at `update()` (retire)
time. So every branch fetched between B1's predict and B1's retire
sees a stale history register, and there's no rollback on
mispredict-flush.

This is a deliberate carry-over from Phase 3's commit-time-only model.
Two reasons it's acceptable today: (a) the OoO core's `in_mispred_`
flag bounds wrong-path fetch to a single mispredicted branch, so the
"polluted history" window is small; (b) the simulator's purpose is to
study cache and OoO behavior, not to chase the last 1% of branch
prediction accuracy. The PHASE4 marker on `BranchPredictor` already
documents that a speculative-checkpoint API is the place this would
land.

### MED — No project2-cross-validation regression test for the OoO core
The predictor module has
[tests/predictor/test_proj2_regression.cpp](../tests/predictor/test_proj2_regression.cpp)
pinning bit-for-bit against project2's `proj2sim`. Phase 4B has no
equivalent despite `core.cpp:1-12` claiming "everything outside those
two integration points mirrors project2's behavior verbatim." With
`tools/proj2_to_champsim` already built, the cost of a small canned
trace + project2 reference + Catch2 assertion is low. Tracking as a
follow-up; the four bug-class fixes were the priority for this pass.

### MED — Possible 1-cycle skew in load latency vs project2
[src/ooo/core.cpp:67-69](../src/ooo/core.cpp#L67-L69)

`Cache::tick()` runs at end-of-cycle; the next cycle's `stage_exec`
runs *before* `tick()`, so a load issued at cycle N with `latency=1`
appears ready at exec of cycle N+2, not N+1. Whether this matches
project2's `left_cycles` semantics needs verification. A pinned-IPC
test on a deterministic load chain would catch a drift either way.

### Other LOW polish
A long tail of project1-leftover snake_case (`tag_meta_t`, `set_t`,
`MAX_MKV_rows`, `LRU_list`), the 130-line `Cache::access()` that could
be split into per-write-policy helpers, and minor coverage gaps for
MUL FU exec, store-then-load disambiguation, multiple-mispredict-in-a-row
behavior, and the asan CMake preset in CI. Each is filed mentally; none
was urgent enough to bundle into this pass.

---

## False positives investigated and dropped

The four parallel review passes initially flagged several items that on
verification turned out to be fine. Listing them so they don't get
re-flagged later:

- **Yeh-Patt mask-on-write vs project2's mask-on-read.** Observationally
  equivalent for predictions and updates; in-memory shift register
  values diverge by a high-bit mask, but predictions always use the
  masked low P bits. Confirmed by the bit-for-bit project2 regression.
- **Perceptron `theta = floor(1.93 G + 14)`.** Identical formula and
  integer truncation; for G=9 both give 31.
- **Hybrid tournament index width / counter MSB selector / init
  mapping.** Confirmed line-by-line against `branchsim.cpp:372-515`.
- **MUL stage-shift order in `stage_exec`.** Completes stage[2] FIRST,
  then shifts. No double-fire, no skip.
- **Wrong-path fetch unbounded.** `in_mispred_` bounds it to a single
  mispredict at a time; `flush_to_ready()` at retire is defense-in-depth.
- **Stores skip the CDB.** Matches project2 verbatim.
- **Trace struct packing.** The on-disk wire format is built field-by-field
  with explicit endian helpers; the C++ `Record` struct's natural
  alignment never reaches the disk. No `__attribute__((packed))` needed.
- **WBWA writeback target.** Uses victim's address, not new block's.
  Correct.
- **`proj2_to_champsim` converter.** Endian-clean, branch-classification
  matches project2's `OPCODE_BRANCH==6`, register-zero convention
  preserved as ChampSim's "unused" sentinel.

---

## Summary of changes

| Area                          | Files                                                                                                                                                | Lines                              |
| ----------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------- |
| Hybrid per-branch state       | [src/predictor/hybrid.cpp](../src/predictor/hybrid.cpp)                                                                                              | refactor (~30 changed, no net add) |
| Cache `issue()` reorder       | [src/cache/cache.cpp](../src/cache/cache.cpp)                                                                                                        | ~15 lines                          |
| Cache `issue()` Write reject  | [src/cache/cache.cpp](../src/cache/cache.cpp)                                                                                                        | ~10 lines                          |
| LSU MSHR-full pre-flight      | [src/ooo/core.cpp](../src/ooo/core.cpp)                                                                                                              | ~12 lines                          |
| Mode dispatch for unimplemented| [src/main.cpp](../src/main.cpp)                                                                                                                      | ~5 lines                           |
| New regression tests          | [tests/cache/test_cache_basic.cpp](../tests/cache/test_cache_basic.cpp), [tests/ooo/test_ooo_basic.cpp](../tests/ooo/test_ooo_basic.cpp)             | ~100 lines, +4 cases               |
| Docs drift                    | [docs/trace-format.md](../docs/trace-format.md), [02-phase1-traces.md](02-phase1-traces.md)                                                           | ~10 lines                          |

**Test count: 82 → 86 passing.** Phase-3 cross-validation against
`proj2sim` still bit-for-bit. No pinned regression number moves.
