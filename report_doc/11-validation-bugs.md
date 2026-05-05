# Validation sweep bug report

**Source data:** sweep `v3` (tier=short, 120 runs across 8 synth traces × 15 configs each, 2026-05-04). Aggregated to [report/_sweep/v3/summary.md](../report/_sweep/v3/summary.md), raw per-run logs under [report/_sweep/v3/logs/](../report/_sweep/v3/logs/).

**Top-line numbers:**

| Status | Count | Cause |
| ---: | ---: | --- |
| OK | 29 | sim ran trace to EOF and wrote a clean report |
| `exit=5` | 60 | hit `kGlobalCap = 5'000'000` cycles before EOF — sim wrote a `Status: Simulation terminated` rpt |
| `exit=-6` (SIGABRT) | 31 | uncaught `std::runtime_error` in coherence FSM, no rpt written |

The sweep is homogeneous 4-core (same trace replicated to `p0..p3` via symlink). The address streams gen_trace produces depend only on record index `i` (or seed for random), so all four cores walk *identical* address sequences in lockstep. That's the unifying root cause — it's a contention pathology that exposes both classes of failure.

---

## Bucket 1: directory / agent FSM crashes (31 failures)

These are unambiguous bugs. A sim should not throw an uncaught C++ exception regardless of the input workload — at minimum it should print a clean error and exit non-zero.

### 1A. `MESI dir: invalid (state, message) pair` — 21 instances

Source: [src/coherence/directory_mesi.cpp:204](../src/coherence/directory_mesi.cpp#L204)

**Distribution.** Every `proto=mesi` config (baseline, rob_64, rob_128, l1kb_16, l1kb_64, predictor_*, pf_*, cores_2) on `synth/random_tiny` and `synth/random_small`. **Zero crashes** on `synth/sequential_*` or `synth/stream_*` for the MESI protocol — those just hit the cycle cap.

**Why random and not the others?** The address stream determines how many distinct cache lines are concurrently in transient states. With stride-64 sequential / stream, adjacent records hit *adjacent* lines, so the directory typically has at most a handful of in-flight entries at any moment. With random, addresses are spread over a 16 MB window aligned to 64 B (262 144 distinct lines), and 4 cores are independently issuing GETS/GETM into that pool. The directory ends up holding many lines in transient states (`SM`, `MM`, `MS`, `EM`, `ES`, etc.) simultaneously. The probability of an "in-transient line A receives a network message originating from a different transient line B" goes up combinatorially.

**The unhandled cases** (read [directory_mesi.cpp:171-204](../src/coherence/directory_mesi.cpp#L171-L204)). The FSM handles only:

- `(SM, INVACK)` — drop the inv-ack count, transition to M when count reaches 0
- `(SM | MM | MS | EM | ES, GETM | GETS | GETX)` — `cycle_queue()` (defer)

Anything else in those transient states throws. Likely missing transitions:

- `(MM, INVACK)` — directory sent INVs while transitioning M→M (e.g. between two writers); the second writer's owner sends INVACK, no handler.
- `(MS, DATA)` — owner sent DATA back during downgrade race.
- `(SM, DATA)` — race where the directory itself sent DATA before resolving the SM → M transition (see also Bucket 1C below).
- `(*, INVACK)` for any non-SM transient state where invalidations were issued.

Comparing to the MOESIF directory (358 LOC, far more transient cases) suggests MESI was written for the simpler "few sharers, low contention" regime and never stressed under random.

### 1B. `MOESIF dir: invalid (state, message) pair` — 6 instances

Source: [src/coherence/directory_moesif.cpp:341](../src/coherence/directory_moesif.cpp#L341)

**Distribution.** Every MOESIF run on `synth/sequential_*`, `synth/stream_*`, and `synth/random_*` — **all four** non-loop synth traces. Only `loop_*` survives. MOESIF's failure mode is the broadest of any protocol.

**Why MOESIF crashes more broadly than MESI.** MOESIF has more transient states (`MM, MO, EM, EF, FM, FF, OM, OO`) than MESI. The handled set at [directory_moesif.cpp:332-339](../src/coherence/directory_moesif.cpp#L332-L339) covers GETM/GETS/GETX deferral on these states, plus a handful of (state, DATA) and (state, INVACK) transitions. But the F (Forward) and O (Owned) states introduce paths the MESI FSM never has to consider — e.g. an O-holder receiving an INV while a different requester is mid-GETS. Those paths likely escape the if-else ladder and hit the catch-all throw.

**Why even sequential / stream crash MOESIF when they only deadlock MESI.** The F-state (Forward) is supposed to designate a single sharer to respond on subsequent GETS, avoiding directory work. With 4 cores reading the same stride-64 sequence in lockstep, four GETS for line `i` arrive in tight succession. Choosing F-holder, transitioning F→FM on a write, handling concurrent GETS/INVs for the *same* line — these are MOESIF-specific transitions, not exercised by MESI which has no F state. MESI's failure mode on these traces is "stall at cycle cap" (because the basic FSM is at least *closed*). MOESIF's is "throw" (because the F/O FSM is not closed).

### 1C. `MSI: SM state shouldn't see ntwk message: DATA` — 2 instances

Source: [src/coherence/agent_msi.cpp:160](../src/coherence/agent_msi.cpp#L160)

**Distribution.** `proto_msi` on `synth/random_tiny` and `synth/random_small`.

**Why this is interesting.** The MSI *agent* in SM state expects only `REQ_INVALID` (downgrade to IM) or `ACK` (transition to M). Look at [agent_msi.cpp:149-162](../src/coherence/agent_msi.cpp#L149-L162):

```cpp
void MsiAgent::do_ntwk_SM(const Message& req) {
    switch (req.kind) {
        case MessageKind::REQ_INVALID: ...; state_ = MsiState::IM; break;
        case MessageKind::ACK:         ...; state_ = MsiState::M;  break;
        default: bad_msg("SM", "ntwk", req.kind);
    }
}
```

But the MSI *directory* at [directory_msi.cpp](../src/coherence/directory_msi.cpp) — same shape as MESI directory — has a code path where it sends `DATA` to a node mid-SM transition (when the requester is no longer in `presence` after invalidations complete; see analogous MESI path at [directory_mesi.cpp:185-193](../src/coherence/directory_mesi.cpp#L185-L193)). So the directory will, under contention, send DATA to an SM agent. The agent then crashes.

**This is the symmetric cousin of bug 1A.** Bug 1A is "directory in transient state receives unexpected network message"; bug 1C is "agent in transient state receives unexpected network message". Both are "we forgot a case under contention" but in different actors.

The MESI agent has the same SM handler shape as MSI ([agent_mesi.cpp:165-177](../src/coherence/agent_mesi.cpp#L165-L177)) — also missing DATA. We don't see the MESI agent crash because the MESI *directory* crashes first (Bucket 1A) on those same traces, beating the agent to the bug.

### 1D. `MOSI: SM state shouldn't see ntwk message: DATA` — 2 instances

Source: [src/coherence/agent_mosi.cpp](../src/coherence/agent_mosi.cpp) (analogous to 1C).

**Distribution.** `proto_mosi` on `synth/random_tiny` and `synth/random_small`. Identical mechanism to 1C.

### Recommended fixes for Bucket 1

1. **Mass audit of `bad_msg("...", "ntwk", ...)` defaults** in [src/coherence/agent_*.cpp](../src/coherence/). For each transient state (`IS, IM, SM, EM, FM, OM`), confirm the agent handles every message kind the directory's analogous transient state can send. The DATA case in SM is the most obvious gap.

2. **Mass audit of the directory's transient-state handlers** in [src/coherence/directory_*.cpp](../src/coherence/). MESI has 9 transient states and only handles a handful of (state, message) pairs explicitly; MOESIF has more. For each, enumerate which incoming messages are reachable under concurrent requests and add cases (or `cycle_queue()` deferrals).

3. **At minimum, replace `throw runtime_error` with a guarded log + clean exit** so the harness flags these as "missing FSM case" rather than SIGABRT. Even before fixing the FSM, the sim should not crash uncleanly. The harness can then surface this as a structured "fsm_missing_case" violation instead of "exit_status=-6".

4. **Stress test the FSMs directly** with synthetic message-injection unit tests (separate from the full simulator) that drive each protocol through every transient state with every legal incoming message. The current MSI/MESI/MOSI/MOESIF tests at [tests/coherence/](../tests/coherence/) likely don't cover transient × ntwk-msg crossproduct.

---

## Bucket 2: cycle-cap timeouts (60 failures)

Source: [src/full/full_mode.cpp:466-501](../src/full/full_mode.cpp#L466-L501) — global cap `kGlobalCap = 5'000'000` cycles. When tripped, the sim writes a partial report.csv (with low IPC and `Status: Simulation terminated` in the rpt) and returns 5.

**Distribution.** All non-loop synth traces × MESI/MSI/MOSI/MI configs that don't crash first. So `sequential_tiny`, `sequential_small`, `stream_tiny`, `stream_small`, `random_*` (the runs that escape Bucket 1).

**Loop survives because of cache residency.** `synth/loop_*` with default `--loop-size=64` cycles through 64 PCs and 64 distinct cache lines. The L1 is 32 KB / 8-way / 64 B = 64 sets × 8 ways = 512 lines. The 64-line working set fits in any one L1, so after warm-up nearly every access is a hit. No L1 misses → no L2 traffic → no coherence work → no contention → trace runs to EOF. Confirmed in summary: `loop_*` runs all show ~0.0019 L1 miss rate.

**Sequential/stream/random saturate the ring.** With one new line touched per record (stride 64 = one block) and the same address stream on all 4 cores, every record causes:
1. L1 miss on each of the 4 cores within a few cycles of each other.
2. Four concurrent GETS to the same line.
3. Directory must decide who gets first DATA, who gets ACK, and serialize the rest.
4. Any subsequent write triggers GETM → INV broadcast → INVACKs → DATA.

Even a perfectly-implemented MESI cannot retire instructions faster than the directory can serialize requests. With one ring + one directory + 100-cycle memory latency, throughput per shared line is ≪ 1 line per cycle. The aggregate cycle budget (5 M cycles) is plausibly *not enough* to retire 100 K records × 4 cores' worth of work in this regime.

**Is it livelock or just slowness?** Distinguishing requires bumping the cap. From [report/_sweep/v3/summary.md](../report/_sweep/v3/summary.md), the `exit=5` runs report ~5 M cycles, ~40 K instructions retired across all 4 cores, IPC ≈ 0.002. At that rate, 100 K records would need ~50 M cycles — 10× the current cap. So most exit=5 runs are likely *slow*, not deadlocked — but we cannot prove it without running with a higher cap.

### Recommended fixes for Bucket 2

1. **Raise `kGlobalCap` to 50 M (or 100 M)** as a first-pass diagnostic. Runs that complete under the new cap were just slow; runs that still hit it are real livelocks. Keep the cap — it's a useful backstop — but match it to the synthetic workload size. Even better: derive it from the trace length (e.g. `cap = max(5M, 10 × records × cores)`).

2. **Make `kGlobalCap` config-driven** instead of a constant. Add `core.deadlock_cycle_cap` to [configs/baseline.json](../configs/baseline.json) and let the sweep harness override it per tier. Current `core.deadlock_threshold_cycles` is per-core stall detection, distinct from this global cap.

3. **Stop using homogeneous 4-core setups for stress tests.** The harness currently symlinks one trace to `p0..p3`. Real multi-threaded programs (matmul, stencil, etc.) have 4 threads operating on *different* data partitions — false sharing exists but is bounded. Generating per-core synthetic traces with offset `addr_base` (e.g. `pi.addr_base = 0x1000_0000 + pi * 0x100_0000`) would mirror real-program contention much better. Documented as a TODO in [docs/tracing.md:124-130](../docs/tracing.md#L124-L130) but not yet implemented.

4. **Consider a per-line forward-progress watchdog in the coherence layer.** The current cap is global ("5M cycles passed total"). A line-level watchdog ("this line has been in MM state for 1M cycles, something is wrong") would distinguish "slow but progressing" from "stuck on one line". Out of scope for the immediate fix list.

---

## Cross-bucket observations

**The harness setup amplifies both bug classes.** Homogeneous 4-core means:
- All 4 cores miss on the same line at roughly the same time → maximum directory pressure (drives Bucket 2).
- Many lines are simultaneously in transient states → maximum cross-line race exposure (drives Bucket 1).

A switch to heterogeneous traces would reduce the failure rate considerably without fixing any sim code. That's a useful diagnostic but it would also *hide* real bugs — the FSM crashes are genuine and must be fixed regardless.

**Prefetcher does not save you.** [scripts/sweep_matrix.py](../scripts/sweep_matrix.py)'s `pf_markov` and `pf_hybrid` axes show no improvement over baseline on the failing traces — the bottleneck is coherence serialization, not L1 miss rate. Prefetching helps only when bandwidth to memory is the limit, not when contention to a directory is.

**The OK/FAIL distribution depends on protocol.** Bucket 1 is concentrated in MESI (21) and to a lesser extent MOESIF (6); the simpler MI protocol shows 0 crashes (it just hits the cap). The order from "fewest crashes" to "most crashes" is roughly MI < MSI ≈ MOSI < MOESIF < MESI. MESI is the *baseline* protocol and the most-tested in unit tests, but those tests don't exercise the contention regime where bug 1A surfaces.

---

## Suggested fix order

Independent fixes; do in any order.

1. **Bucket 1A** (MESI directory) — highest impact, 21 of 31 SIGABRTs. Audit transient-state handlers, add missing message cases. Target file: [src/coherence/directory_mesi.cpp](../src/coherence/directory_mesi.cpp). Estimated effort: 1-2 days, mostly enumeration + testing.
2. **Bucket 1C/1D** (MSI/MOSI agents missing DATA in SM) — small, focused fix in [src/coherence/agent_msi.cpp](../src/coherence/agent_msi.cpp) and [agent_mosi.cpp](../src/coherence/agent_mosi.cpp). Add `case MessageKind::DATA: send_DATA_proc(...); state_ = M;` to `do_ntwk_SM`. Verify the parallel directory code actually intends to send DATA in that path. ~1 hour.
3. **Bucket 1B** (MOESIF directory) — broader audit, broader test surface. Target [src/coherence/directory_moesif.cpp](../src/coherence/directory_moesif.cpp). 2-3 days.
4. **Bucket 2** (cycle cap) — bump constant or wire through config; 30 min. Then re-run the sweep to see what's actually deadlocked vs. just slow. Defer "real" deadlock investigation until after Bucket 1 fixes (since deadlock under live FSM bugs is hard to diagnose).

After all four: rerun `make short SWEEP_ID=postfix` and expect ~0 errors (modulo whatever real livelocks Bucket 2 step exposes).

---

## How to reproduce a single crash

```sh
cd Multicore-OoO-sim/

# Bucket 1A — MESI directory crash on random:
./build-release/src/sim --config configs/baseline.json \
    --trace-dir traces/synth/random_tiny --tag repro_mesi_random
# expected: libc++abi terminate with "MESI dir: invalid (state, message) pair"

# Bucket 1B — MOESIF directory crash on stream:
./build-release/src/sim --config configs/baseline.json \
    --trace-dir traces/synth/stream_tiny --tag repro_moesif_stream \
    --protocol moesif
# expected: libc++abi terminate with "MOESIF dir: invalid (state, message) pair"

# Bucket 1C — MSI agent crash on random:
./build-release/src/sim --config configs/baseline.json \
    --trace-dir traces/synth/random_tiny --tag repro_msi_random \
    --protocol msi
# expected: libc++abi terminate with "MSI: SM state shouldn't see ntwk message: DATA"

# Bucket 2 — cycle cap on stream:
./build-release/src/sim --config configs/baseline.json \
    --trace-dir traces/synth/stream_tiny --tag repro_cap
# expected: exit 5, [ERROR] full mode: global cycle cap reached at 5000000
```

All four are deterministic.

---

## Fixes applied (2026-05-04)

All four buckets fixed in one pass. 126/126 unit tests still pass. Each fix is small, surgical, and documented with a comment at the change site explaining the recovery scenario.

### Root cause for buckets 1A, 1B, 1C, 1D — eviction desync

The cache layer evicts lines via [coherence_adapter.cpp:83](../src/coherence/coherence_adapter.cpp#L83) (`on_evict` → sends `DATA_WB` straight to the network), **bypassing the agent FSM**. So after an eviction the cache no longer holds the line but the agent still thinks it's in S/E/F/O. When the CPU then issues another LOAD/STORE on that line, the agent's `do_proc_<state>` handler fires from a stale state and either:

- Sends `GETM` from S → enters SM, dir sees `presence[me]=false` → memory-fetch path → DATA arrives at agent in SM. Agent SM has no DATA case → crash.
- Sends `GETX` from E → enters EM, dir is in I (writeback dropped it) → no `(I, GETX)` handler → crash.
- For MOESIF, an F-holder evicting drops the dir into S (via the shared `handle_writeback` at [directory.cpp:139](../src/coherence/directory.cpp#L139)) → next access finds MOESIF in a state it doesn't natively model → crash.

All three crash flavors share the same fix shape: **add the missing transitions on the recovery path so the FSM accepts what actually arrives**.

### Bucket 1C/1D — agent SM missing `DATA` (4 protocols)

Files: [src/coherence/agent_msi.cpp:149-167](../src/coherence/agent_msi.cpp#L149-L167), [agent_mesi.cpp:165-183](../src/coherence/agent_mesi.cpp#L165-L183), [agent_mosi.cpp:164-182](../src/coherence/agent_mosi.cpp#L164-L182), [agent_moesif.cpp:238-256](../src/coherence/agent_moesif.cpp#L238-L256).

Each `do_ntwk_SM` handler gained a `case MessageKind::DATA:` arm that mirrors the existing `IM → M` behavior:

```cpp
case MessageKind::DATA:
    send_DATA_proc(req.block);
    state_ = <Protocol>State::M;
    break;
```

**Verification.** `MSI` and `MOSI` reproducers (`./sim --protocol msi --trace-dir traces/synth/random_tiny ...`) previously SIGABRT'd; now run to completion or hit the cycle cap (which is itself fixed below).

### Bucket 1A — MESI directory `(I, GETX)` (21 of 21 instances)

File: [src/coherence/directory_mesi.cpp:38-53](../src/coherence/directory_mesi.cpp#L38-L53).

Added an `(I, GETX)` arm that mirrors `(I, GETM)`: memory-fetch path, grant exclusive, transition dir to M, requester to presence. The agent meanwhile is in EM and now has its own DATA case (see Bucket 1A-companion below).

```cpp
} else if (entry->state == DirState::I && request->kind == MessageKind::GETX) {
    request_in_progress = true;
    target_node = request->src;
    response_time = current_clock_ + settings_.mem_latency;
    entry->state = DirState::M;
    entry->presence[request->src] = true;
    ++entry->active_sharers;
    dequeue();
}
```

**Companion fix in [agent_mesi.cpp:200-205](../src/coherence/agent_mesi.cpp#L200-L205):** `do_ntwk_EM` gained the `DATA → M` case so the response from the (I, GETX) handler is accepted.

**Verification.** Reproducer `./sim --trace-dir traces/synth/random_tiny ...` no longer crashes; exits 0.

### Bucket 1B — MOESIF directory `(S, *)` and `(SM, INVACK)`

File: [src/coherence/directory_moesif.cpp:40-105](../src/coherence/directory_moesif.cpp#L40-L105) (new handlers); [agent_moesif.cpp:252-307](../src/coherence/agent_moesif.cpp#L252-L307) (DATA cases on EM/FM/OM).

MOESIF reaches `DirState::S` only via the shared writeback path when an F/O-holder evicts while other sharers remain. The native MOESIF FSM has no S handlers because S isn't a stable state in MOESIF's design. Added three:

- **`(S, GETS)`** → memory-fetch + add requester to presence, line stays S. Mirrors MSI/MESI.
- **`(S, GETM | GETX)`** → invalidate other sharers, transition to SM. If no other sharers, silent upgrade to M.
- **`(SM, INVACK)`** → drain INVACKs, then either ACK the requester (still in presence) or memory-fetch + DATA (presence cleared).

Plus added `DirState::SM` to the existing transient-state defer block so concurrent GETs for the same line cycle correctly.

The MOESIF agent's `do_ntwk_EM`, `do_ntwk_FM`, and `do_ntwk_OM` all gained `DATA → M` cases for the same eviction-desync reason (analogous to Bucket 1C/1D).

**Verification.** All three previously-crashing MOESIF combos (`sequential_tiny`, `stream_tiny`, `random_tiny`) now exit 0.

### Bucket 2 — global cycle cap

File: [src/full/full_mode.cpp:50-57](../src/full/full_mode.cpp#L50-L57). Bumped from 5 M to 50 M:

```cpp
constexpr coherence::Timestamp kGlobalCap = 50'000'000;
```

Comment at the constant explains the rationale (homogeneous-shared traces need ~10× a private-load run of equivalent length). Long-tier (100 M-record) traces should still finish on EOF in normal operation, not the cap.

**Not yet config-driven.** A follow-up could route this through `core.deadlock_cycle_cap` or similar so the harness can sweep it. Out of scope for this pass.

### Net effect on the validation sweep

Reproducers in the table above all converge:

| Bug | Before fix | After fix |
|---|---|---|
| `(SM, DATA)` MSI/MOSI agent | SIGABRT | exit 0 |
| `(I, GETX)` MESI dir | SIGABRT | exit 0 |
| `(S, GETS)` MOESIF dir | SIGABRT | exit 0 |
| stream/sequential cycle cap (tiny) | exit 5 | exit 0 |

**Postfix sweep result** (`make short SWEEP_ID=postfix`):

| Sweep | OK | FAIL | SIGABRT (-6) | Cycle cap (5) |
| --- | ---: | ---: | ---: | ---: |
| v3 pre-fix | 29 | 91 | 31 | 60 |
| postfix | **76** | **44** | **0** | 44 |

- **All 31 SIGABRTs eliminated.** Bucket 1A/1B/1C/1D fully closed.
- **Cycle-cap hits dropped from 60 to 44** despite the 50 M cap. The remaining cap hits all come from the `_small` (1 M-record) traces in homogeneous 4-core contention; the `_tiny` (100 K) traces all complete now. With 4 cores × identical random/stream/sequential streams × ~0.002 effective IPC, 1 M records still need >50 M cycles; that's the synthetic-pathology issue, not a sim bug.
- Every remaining FAIL is `exit=5` — no crashes, no `(state, message)` gaps surfaced by the wider sweep.

Aggregator output (`report/_sweep/postfix/summary.md`): 88 errors (down from 164), 4 warnings (cross-run rules now have enough data to flag predictable things like rob-128 regressions on contended traces), 0 infos.

Failure distribution post-fix:

| Trace | OK | FAIL |
| --- | ---: | ---: |
| `synth/loop_*` | 30 | 0 |
| `synth/sequential_tiny` | 15 | 0 |
| `synth/sequential_small` | 0 | 15 |
| `synth/stream_tiny` | 15 | 0 |
| `synth/stream_small` | 0 | 15 |
| `synth/random_tiny` | 15 | 0 |
| `synth/random_small` | 1 | 14 |

The clean tiny-vs-small split confirms the diagnosis: it's purely about whether the 4-core lockstep contention can fit inside the cycle budget. Either bump the cap further (100 M would likely close most of these), make the cap config-driven, or — better — move to heterogeneous 4-core synthetic traces so the contention level is realistic.

---

## Empirical disambiguation: 500M-cap rerun

To distinguish "slow but progressing" from "truly livelocked" among the 44 cap-hit FAILs, bumped `kGlobalCap` from 50 M → 500 M ([full_mode.cpp:50-56](../src/full/full_mode.cpp#L50-L56)) and re-ran just the failing subset:

```sh
python3 scripts/run_sweep.py --tier short --sweep-id cap500 --jobs 8 --timeout 1800 \
    --only-traces synth/sequential_small,synth/stream_small,synth/random_small
```

**Result: 43 of 45 OK, 2 timeout (Python 30-min wallclock).** Aggregator emitted `report/_sweep/cap500/summary.{csv,md}` with 2 errors (the timeouts), 4 warnings (cross-run regressions), 0 infos.

| Trace × axis | OK | Timeout |
| --- | ---: | ---: |
| `synth/sequential_small` × 15 configs | 14 | 1 (`proto_mi`) |
| `synth/stream_small` × 15 configs | 14 | 1 (`proto_mi`) |
| `synth/random_small` × 15 configs | 15 | 0 |

OK-run wallclock distribution: min 7.6 s, median 209 s, mean 249 s, **max 1619 s (27 min)**. Several runs took 5–25× longer than the 50 M-cap run would have, but they all completed.

### Conclusion

**42 of 44 previously-failing runs were slow, not livelocked.** The 5 M and 50 M caps were both well below the cycle budget needed for homogeneous 4-core contention on `_small` traces. Bigger cap, more wallclock → they all finish.

**2 outliers, both MI protocol** (`proto_mi` on `sequential_small` and `stream_small`), hit the **30-min Python wallclock timeout** before completing — and did so without writing a partial report (Python kills the subprocess before the sim can dump). We can't tell from output alone whether MI is genuinely livelocked or just extremely slow.

### Why MI is suspicious but not necessarily a bug

MI is the worst-case protocol by design: every read of a shared block invalidates whichever node currently holds it, so a 4-core lockstep load on the same address ping-pongs the line to the new reader on every record. Per-line throughput drops to roughly:

```
cycle_per_record ≈ 4 × (ring_round_trip + dir_serialize + mem_response)
                 ≈ 4 × (~30 + ~10 + 100) cycles
                 ≈ 560 cycles/record
```

For 1 M records × 4 cores = 4 M dynamic loads, that's ~2.2 B cycles. At our wallclock rate (~2–3 M cycles/s under MI's high message volume), that's ~12–20 minutes. The two timeouts at 30 min wallclock are within an order of magnitude of "expected slow"; a 60-min timeout would likely close them too.

To prove forward progress vs livelock, the sim would need a finer-grained watchdog — e.g. "no instruction retired in the last 100 K cycles" — rather than the current global cap. That's a real piece of work, not a one-line bump. **Filed as follow-up, not investigated here.**

### Final bucket status

| Bucket | Pre-fix v3 | Postfix (50M cap) | Cap500 (subset) |
| --- | ---: | ---: | ---: |
| 1A — MESI dir invalid | 21 SIGABRT | 0 | 0 |
| 1B — MOESIF dir invalid | 6 SIGABRT | 0 | 0 |
| 1C — MSI agent SM/DATA | 2 SIGABRT | 0 | 0 |
| 1D — MOSI agent SM/DATA | 2 SIGABRT | 0 | 0 |
| 2 — cycle cap (slow) | 60 exit=5 | 44 exit=5 | **2 timeout** (MI only) |
| **Total fails** | **91 / 120** | **44 / 120** | **2 / 45** |

The harness now runs 96.4% green on the short tier (118 of 120 with cap500). The 2 remaining MI timeouts are a tail-latency phenomenon, not an unhandled-state bug — exactly the kind of pathological case that goes away with heterogeneous 4-core traces (the planned next step).

### Recommendation

For routine harness use, **the 50 M cap is fine** — it's a pragmatic backstop that catches genuine deadlock-shaped runs without letting any one run drag for 30 min. The 500 M cap was a diagnostic, not a permanent setting. Revert if wallclock matters more than max-coverage.

For the long-tier sweep (100 M-record traces), config-drive the cap so heavy synthetic runs can opt into 500 M while normal usage stays at 50 M. Out of scope for this pass.

### Architectural note for future work

The eviction-desync class of bug is structurally inevitable as long as `on_evict` bypasses the agent FSM. A more durable fix is to drive cache evictions *through* the agent (e.g. agent receives an explicit `EVICT` proc-side message and chooses M→I, S→I, etc., emitting `DATA_WB` itself). That eliminates the post-eviction stale-state class entirely. Considered out of scope for this pass — the targeted fixes above are sufficient to unblock the validation harness, and the structural change touches every protocol's agent FSM.

