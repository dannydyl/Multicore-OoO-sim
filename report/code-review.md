# Code review — Phases 0–3

A senior-engineer pass over the cache, predictor, trace I/O, config, and CLI
code as it stands at the end of Phase 3. Findings are tagged with severity:

| Tag       | Meaning                                                             |
| --------- | ------------------------------------------------------------------- |
| **BUG**   | Wrong output today. Should be fixed.                                |
| **LOGIC** | Behaves correctly but doesn't match how a real CPU would work.      |
| **PERF**  | Correct and fast enough today, but suboptimal for big experiments.  |
| **PHASE4**| Will block or complicate the OoO core port; mark now, fix later.    |
| **POLISH**| Minor robustness / ergonomics. Optional.                            |

I deliberately do not flag pure style nits.

---

## Summary

- **No correctness bugs found** that change a regression-test answer. The
  cache and predictor numbers match project1 / project2 reference output
  bit-for-bit.
- **Two PHASE4 architectural debts** that the OoO core port will trip on:
  the cache `access()` interface returns a synchronous result instead of
  a request handle, and prefetchers fire inside the demand miss path
  instead of after the miss is serviced. Both inherited from project1.
- **A few small polish items** in config error handling and main-loop
  dispatch that are worth cleaning up before more modes land.

I disagree with two of the original audit's "BUG" findings — see the
**rejected findings** section at the end.

---

## Cache subsystem

### LOGIC — WTWNA L2 write-hit doesn't claim the prefetch
[src/cache/cache.cpp:333-341](../src/cache/cache.cpp#L333-L341)

The WBWA hit path (line 289-294) and the WTWNA read-hit path (line 351-353)
both clear `block.prefetched` and bump `prefetch_hits`. The WTWNA write-hit
path does neither. A prefetched line that gets written to before any read
demand will not be counted toward `prefetch_hits`, even though the prefetch
clearly paid off (the block was needed).

This **matches project1** verbatim — that's why the regression tests pass —
so it isn't strictly a bug. But it's a quirk worth documenting in the code,
because someone reading it will eventually wonder why the three hit paths
do three different things.

### PHASE4 — `access()` returns a synchronous boolean+latency, no request handle
[include/comparch/cache/cache.hpp:58](../include/comparch/cache/cache.hpp#L58),
[include/comparch/cache/mem_req.hpp](../include/comparch/cache/mem_req.hpp)

`AccessResult { bool hit; unsigned latency; }` is fine for `--mode cache`
where one access is one transaction. The OoO core needs more:

- multiple in-flight misses concurrently (MSHRs)
- the ability to ask "is request X done yet?" without re-issuing it
- a way to attach the consumer (load instruction, prefetcher) to the result
  so writeback / fill events can complete the right transaction

When Phase 4 lands, `access()` will need to either return a `RequestHandle`
or take a callback. Today's interface should grow a comment marking the
limit so a future me doesn't accidentally lean on it.

### PHASE4 — prefetchers fire synchronously inside the demand miss path
[src/cache/cache.cpp:320-323, 375-377](../src/cache/cache.cpp#L320-L323),
[src/cache/prefetcher_markov.cpp:140-185](../src/cache/prefetcher_markov.cpp#L140-L185)

`Cache::access()` calls `prefetcher->on_miss()` immediately when it detects
a miss. The prefetcher then calls `cache.issue_prefetch()`, which fills the
block at zero latency. So the prefetched line is in the cache before the
demand miss has even returned data.

Real cores fire prefetchers from the miss-resolution event so that the
prefetch is competing with the demand request for the same downstream
bandwidth. Project1 didn't model that distinction either. Phase 4 will need
to push prefetchers into the OoO event loop.

### POLISH — writebacks treated as instant
[src/cache/cache.cpp:168-179](../src/cache/cache.cpp#L168-L179)

`++stats_.writebacks` is bumped and the dirty line is immediately re-issued
as a `Write` to the next level. No latency, no write-buffer queueing.
Project1 didn't model this either. Phase 4's memory model is the right
place to add a write buffer.

### PERF — linear set scans per access (nit-tier)
[src/cache/cache.cpp:285-301, 347-358](../src/cache/cache.cpp#L285-L301)

Every `access()` walks the set's `std::list<tag_meta_t>` to find a tag
match. For 8-way sets that's 8 pointer chases — fine for today's traces.
At 16-way and millions of accesses you'll feel it. A per-set tag → iterator
hashmap would make hits O(1).

Not worth doing now.

---

## Predictor subsystem

### POLISH — `assert(value_ <= max())` in SaturatingCounter
[include/comparch/predictor/saturating_counter.hpp:30-32](../include/comparch/predictor/saturating_counter.hpp#L30-L32)

`assert()` disappears in release builds. If a future caller hands in a
bad `init` the constructor would silently start the counter in a broken
state. The factories already validate ranges, so today this is only
reachable through an internal mistake — but a `throw std::invalid_argument`
would survive `-DNDEBUG`. Tiny robustness win.

### PERF — Perceptron weights stored as `vector<vector<int>>`
[src/predictor/perceptron.cpp:55-59](../src/predictor/perceptron.cpp#L55-L59)

Each row is a separate heap allocation, so the inner loop in `dot_product()`
walks an array of pointers instead of contiguous memory. With default sizes
(128 perceptrons × 10 ints) the impact is negligible. With a larger N or G
you'd want a single flat `std::vector<int>` indexed as `idx*(G+1) + i`.

Not blocking; flag if it shows up in a profile.

### LOGIC — Hybrid caches `last_yp_` / `last_pct_` between predict and update
[src/predictor/hybrid.cpp:75-101](../src/predictor/hybrid.cpp#L75-L101)

Hybrid stores the two sub-predictor outputs in member variables in
`predict()` and re-reads them in `update()`. That works because the
contract is "predict, then update, then move to next branch", but it
isn't documented or asserted. The day someone calls `predict()` twice
without an intervening `update()` the cache will be wrong silently.

Either document the contract loud and clear in the comment, or store the
predictions in a small per-call return struct. I lean toward the comment;
restructuring the interface for one class isn't worth it.

### POLISH — Predictor stats live inside `predictor_mode.cpp`, not on the predictor
[src/predictor/predictor_mode.cpp:24-30](../src/predictor/predictor_mode.cpp#L24-L30)

The `Stats` struct is defined locally. Phase 4 will want the OoO core to
read the same accuracy / MPKI numbers, so this struct will move into a
header. Easy to do whenever the second consumer shows up.

---

## Common code (config, trace, CLI)

### POLISH — `from_json()` requires every field on every config struct
[src/common/config.cpp:31-55](../src/common/config.cpp#L31-L55)

`InterconnectConfig`, `MemoryConfig`, base `PredictorConfig` fields, and
the early `CacheLevelConfig` fields are read with `j.at(...).get_to(...)`,
which throws if the key is missing. We've started using the
`if (j.contains(...))` pattern for new optional fields (`n_markov_rows`,
`perceptron_*`, `hybrid_*`). The older required fields will trip up
anyone trying to write a minimal config.

Reasonable cleanup: change every `from_json` to check `contains()` and
fall back to the struct's default. Keeps `baseline.json` valid while
making partial configs viable.

### POLISH — Mode dispatch in `main.cpp` will scale poorly
[src/main.cpp:30-52](../src/main.cpp#L30-L52)

Each new `--mode` adds another `if (cli.mode == ...)` block with the same
try/catch shell. By Phase 5 there will be five copies. A `std::map<Mode,
std::function<int(...)>>` (or a tiny dispatch table) collapses this.

Not urgent; do it whenever the third mode lands.

### Trace I/O looks clean
[src/common/trace.cpp](../src/common/trace.cpp)

Endianness is `static_assert`-guarded, partial reads throw with a
descriptive message, the writer flushes on destruction. No issues to flag.

---

## Rejected findings from the original audit

The first audit pass produced a few "BUG" calls I disagree with. Listing
them so future-me doesn't get spooked into "fixing" working code.

### Not a bug — "Markov learns even when the prefetch hit"

`MarkovPrefetcher::on_miss()` is only called from `Cache::access()` when
there is a **demand miss**. If a Markov-issued prefetch successfully landed
before the next access, that access becomes a hit and `on_miss()` never
fires. So `learn()` is always training on real cold misses. The Markov
table grows correctly.

### Not a bug — "Hybrid issues two prefetches per miss"

Looking at [src/cache/prefetcher_hybrid.cpp:38-47](../src/cache/prefetcher_hybrid.cpp#L38-L47):
the `if (has_prediction) { ... } else if (!found_row) { plus_one_.on_miss(...); }`
chain is a strict either/or. Markov and +1 never both fire on the same
miss. The original audit walked back the claim itself; copying the
conclusion here for completeness.

---

## Recommended fixes, in priority order

1. **Comment the PHASE4 limits** in [cache.hpp:58](../include/comparch/cache/cache.hpp#L58)
   and [predictor.hpp](../include/comparch/predictor/predictor.hpp). One sentence each.
   Cheap insurance against re-tripping on the same issues during the OoO port.
2. **Loosen `from_json` to use `contains()` everywhere** so partial JSONs
   give clear errors instead of nlohmann exception text.
3. **Document Hybrid's predict/update ordering contract** in
   [hybrid.cpp](../src/predictor/hybrid.cpp).
4. **Convert the `assert` in SaturatingCounter** to a runtime throw — tiny.
5. Everything else is fine to defer.

Total effort for items 1-4: under an hour.
