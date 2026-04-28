# Phase 2 — Cache subsystem (`--mode cache`)

**Goal:** port project1 (the L1+L2 cache assignment) into a polymorphic
`Cache` class, drive it from a ChampSim trace via `--mode cache`, and
pin the resulting numbers against project1's reference output.

This phase touches the most code of any phase to date. By the end of it
the simulator can read a real (or real-ish) memory trace and produce
hit/miss numbers that match a known-good reference within a single
miss.

---

## What was ported

| project1 piece                              | Where it lives now                                      | What changed                                                                       |
| ------------------------------------------- | ------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| Geometry math (C, B, S → tag/index/offset)  | [cache.cpp:71-88](../src/cache/cache.cpp#L71-L88)        | Same shifts and masks; lifted into the Cache class                                  |
| Set as MRU-front linked list                | [cache.hpp:29-31](../include/comparch/cache/cache.hpp#L29-L31) | `std::list<tag_meta_t>`, MRU at head                                                |
| LRU replacement                             | [cache.cpp:285-301](../src/cache/cache.cpp#L285-L301)    | "splice to front on hit" handles LRU implicitly                                      |
| LIP replacement                             | [cache.cpp:191-195](../src/cache/cache.cpp#L191-L195)    | Branch in `insert_new_block`: LIP inserts at LRU position, MIP at MRU                |
| WBWA write policy (project1 L1)             | [cache.cpp:155-196](../src/cache/cache.cpp#L155-L196)    | Allocate-on-miss, dirty on write, writeback chain on dirty eviction                 |
| WTWNA write policy (project1 L2)            | [cache.cpp:199-222, 333-341](../src/cache/cache.cpp#L199-L222) | Read-allocate only; writes pass through to next level                              |
| L1 → L2 → DRAM chain                        | [cache_mode.cpp:182-198](../src/cache/cache_mode.cpp#L182-L198) | Hierarchy built bottom-up; L1.next_level=&L2, L2.main_memory=&mem, L2.peer_above=&L1 |
| +1 (next-line) prefetcher                   | [prefetcher_plus_one.cpp](../src/cache/prefetcher_plus_one.cpp) | Trivially issues `block_addr + 1` on every miss                                     |
| Markov prefetcher                           | [prefetcher_markov.cpp](../src/cache/prefetcher_markov.cpp) | Per-source correlation table, MFU-by-count prediction                                |
| Hybrid prefetcher                           | [prefetcher_hybrid.cpp](../src/cache/prefetcher_hybrid.cpp) | Markov first; falls back to +1 when Markov has no row for this source               |
| Stats (hits/misses/writebacks/prefetches)   | [cache_stats.hpp](../include/comparch/cache/cache_stats.hpp) | Single struct per Cache; printer in `cache_mode.cpp`                                |

What was **dropped**: the global-array layout (`L1_row[]`, `L2_row[]`,
`sim_access`), `cachesim_driver.cpp`, `Makefile`, `run.sh`, course
trace files, all `validate_*` Makefile targets, all
`6290docker-*.{sh,bat}` scripts.

---

## Cache geometry, in one paragraph

For non-architecture readers: a cache is parameterized by three
log2-of-power-of-two numbers, `(C, B, S)`:

- `C` — log2 of total capacity in bytes. C=15 means 32 KB.
- `B` — log2 of block (cache-line) size. B=6 means 64-byte lines.
- `S` — log2 of associativity (ways per set). S=3 means 8-way.

From those, a 64-bit byte address splits into three fields:

```
|<------------- byte address (64 bits) ----------------->|
|        tag        |    index    |     block offset     |
                    ^-- (C-B-S) bits   ^-- B bits
```

The block offset is the byte position inside a line and we drop it.
The index picks which set the address maps to (there are `2^(C-B-S)`
sets). The tag is everything left over and identifies the unique block
inside the set.

The `Cache` class holds those numbers and `2^(C-B-S)` sets, each set a
`std::list<tag_meta_t>` of up to `2^S` blocks.

---

## Replacement policies

Three options, all behind the same enum:

- **LRU** (default, "MIP" insert) — Least Recently Used. New fills go
  to the MRU slot; eviction takes the LRU. Standard.
- **LIP** — LRU Insertion Policy. New fills go to the **LRU** slot.
  This sounds backwards, but it's the trick from
  [Qureshi et al., ISCA 2007](https://users.ece.utexas.edu/~patt/07.MICRO/papers/Qureshi.pdf):
  on a one-shot scan that touches a block once and never returns,
  inserting at LRU means the block leaves quickly without polluting the
  rest of the set. Hot blocks get hit again before they reach LRU and
  get promoted.
- **MIP** — MRU Insertion Policy. The "normal" thing — equivalent to
  vanilla LRU. Named in project1 to contrast with LIP.

Implementation difference is exactly two lines in
[cache.cpp:191-195](../src/cache/cache.cpp#L191-L195) — `push_back` vs.
`push_front`. That's it.

---

## Write policies

- **WBWA** (Write-Back, Write-Allocate) — used at L1 by default. Hits
  are normal; on a write hit, mark the block dirty. On a miss (read
  *or* write), allocate the block locally; write-miss inserts mark the
  newly-fetched block dirty. When a dirty block is evicted, push it
  downstream.
- **WTWNA** (Write-Through, Write-No-Allocate) — used at L2 by default.
  Write hits update; write misses do nothing locally and pass straight
  to the next level. Reads behave normally. Blocks are never dirty,
  so no writebacks.

The class implements both. Same `Cache` class, picked by the config
field `write_policy`.

---

## Prefetchers

A `Prefetcher` is a small object hooked into the Cache via
`cache.set_prefetcher()`. Its one method, `on_miss()`, is called every
time the Cache experiences a demand miss. It can decide to
`cache.issue_prefetch(addr)`, which inserts the line tagged as a
prefetched fill.

Three implementations:

### +1 (next-line) prefetcher
[prefetcher_plus_one.cpp](../src/cache/prefetcher_plus_one.cpp)

On any miss, fetch the *next* block too. Models the simplest possible
spatial prefetcher. Works great on streaming code, useless on irregular
access patterns.

### Markov prefetcher
[prefetcher_markov.cpp](../src/cache/prefetcher_markov.cpp)

Maintains a small table indexed by the most recent miss address (the
"source"). Each source row holds up to 4 destination block addresses,
each with an access counter. On miss, look up the source row for the
current miss; predict the most-frequently-used destination as the next
line to prefetch. Then update the table: add the current miss as a
destination of the previous miss's row.

This is **history-based correlation prefetching** — it learns
miss-to-miss patterns ("after I miss X, I usually miss Y next"). Works
on linked-list-style traversals where +1 fails.

### Hybrid prefetcher
[prefetcher_hybrid.cpp](../src/cache/prefetcher_hybrid.cpp)

If Markov has a learned destination for this source, use Markov.
Otherwise (cold source, no row yet) fall back to +1. Exactly one of
the two fires per miss — never both.

Gets the best-of-both: cold-start coverage from +1, learned-pattern
coverage from Markov.

---

## How it all hooks together

```
            ChampSim trace  (loads + stores per instruction)
                  |
                  v
         +--------+---------+
         |  cache_mode.cpp  |  (the --mode cache driver)
         +--------+---------+
                  |
                  v
              L1 Cache  (WBWA, MIP)
              latency=2
                  |  miss
                  v
              L2 Cache  (WTWNA, LIP, Markov prefetcher)
              latency=10
                  |  miss
                  v
              MainMemory
              latency=100
```

`cache_mode.cpp` walks every record in the trace, issues a `Read` for
every non-zero `source_memory[]` entry and a `Write` for every non-zero
`destination_memory[]` entry, lets the cache hierarchy figure out where
each access lands.

Built bottom-up so each level has its downstream pointer set at
construction. The `L1 ↔ L2` peer pointer (used by L2's prefetcher to
ask "is this block already in L1?") is patched after both exist.

---

## Configuration

Cache settings live in [configs/baseline.json](../configs/baseline.json):

```jsonc
"l1": {
  "size_kb": 32, "block_size": 64, "assoc": 8,
  "replacement": "lru", "write_policy": "writeback",
  "prefetcher": "none",
  "hit_latency": 2
},
"l2": {
  "size_kb": 256, "block_size": 64, "assoc": 8,
  "replacement": "lip", "write_policy": "writeback",
  "prefetcher": "markov",
  "hit_latency": 10,
  "n_markov_rows": 64
}
```

Geometry fields (`size_kb`, `block_size`, `assoc`) must be powers of
two; `cache_mode.cpp:to_cache_config()` converts them to log2 form for
the Cache class.

---

## Verification: pinning project1 numbers

The big regression target for this phase is "match project1's numbers
on equivalent inputs". Project1 ships with `short_gcc.trace` (a small
gcc memory trace in project1's `R/W 0x...` ASCII format) plus
known-good output files for several configurations.

Pinned in [test_proj1_regression.cpp](../tests/cache/test_proj1_regression.cpp)
against fixtures under [tests/cache/fixtures/proj1/](../tests/cache/fixtures/proj1/):

- `short_gcc_default.out` — no prefetcher
- `short_gcc_plus1.out` — +1 prefetcher at L2
- `short_gcc_markov.out` — Markov prefetcher at L2 (256 rows)
- `short_gcc_hybrid.out` — Hybrid prefetcher at L2 (256 rows)

Each test loads the trace, runs the configured hierarchy, parses the
expected `.out` file, and asserts every counter matches exactly:
hits, misses, writebacks, prefetches_issued, prefetch_hits,
prefetch_misses, L2 read_hits, L2 read_misses, DRAM accesses.

```
$ ctest --preset default -R proj1
        Start  44: proj1 regression: short_gcc.trace, default config (no prefetch)
 1/4 Test #44: proj1 regression: short_gcc.trace, default config (no prefetch)   Passed
 2/4 Test #45: proj1 regression: short_gcc.trace, +1 prefetch                    Passed
 3/4 Test #46: proj1 regression: short_gcc.trace, Markov prefetch (256 rows)     Passed
 4/4 Test #47: proj1 regression: short_gcc.trace, Hybrid prefetch (256 rows)     Passed
```

Bit-for-bit faithful to project1.

---

## Running it manually

```bash
# Build
cmake --build --preset default --target sim

# Run cache mode on the project1 fixture
./build/default/sim --mode cache \
    --config configs/baseline.json \
    --trace tests/cache/fixtures/proj1/short_gcc.trace
```

Expected output looks like:

```
==== cache stats ====
L1:
  accesses           XXXX
  reads              XXXX
  writes             XXXX
  hits               XXXX  (95.42 %)
  misses             XXXX  (4.58 %)
  writebacks         XXXX
  prefetches_issued  0
  prefetch_hits      0
  prefetch_misses    0
L2:
  accesses           XXXX
  ...
DRAM:
  accesses           XXXX
  reads              XXXX
  writes             XXXX
```

---

## Trade-offs and known limits

These are pure project1-fidelity choices. They show up in
[code-review.md](code-review.md) tagged **PHASE4**.

- **Prefetchers fire synchronously inside the demand miss path.** Real
  cores defer the prefetch to after the miss is resolved so the
  prefetch competes with the demand for downstream bandwidth. Project1
  doesn't model that, and we match project1.
- **Writebacks are instant.** No write buffer, no queuing. The
  `++stats_.writebacks` happens, the dirty data goes downstream, no
  cycles charged.
- **`Cache::access()` returns a synchronous `(hit, latency)` pair.**
  No request handle, no MSHRs. Fine for trace-driven cache mode where
  there's only ever one request in flight at a time. The OoO core in
  Phase 4 will need a richer interface — flagged in the review.

---

## What's next

Phase 3 (branch predictors) reuses the trace I/O and the mode-dispatch
plumbing from Phase 2 but doesn't touch any cache code. Phase 4 (OoO
core) is where the cache gets a second consumer — the OoO LSU will
issue `cache.access()` calls per load/store, and at that point the
synchronous interface becomes a problem.

For now, `--mode cache` is the canonical regression target every
future phase will cross-check against. If a Phase 4 change accidentally
breaks cache-only behavior, the four `proj1 regression` tests will
catch it immediately.
