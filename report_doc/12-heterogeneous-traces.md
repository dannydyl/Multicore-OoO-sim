# Heterogeneous per-core traces

**Goal:** let each core read a *different* trace file, instead of the homogeneous lockstep regime imposed by `--trace-dir DIR/p<i>.champsimtrace`. The validation report ([11-validation-bugs.md](11-validation-bugs.md)) called this out twice as the "real fix" for the tail-latency / cycle-cap pathologies it documented:

> "The 'real' fix is either heterogeneous traces or a finer-grained no-progress watchdog" — [full_mode.cpp:57](../src/full/full_mode.cpp#L57)
>
> "...exactly the kind of pathological case that goes away with heterogeneous 4-core traces (the planned next step)." — [11-validation-bugs.md:353](11-validation-bugs.md#L353)

This phase delivers the feature and runs an experiment that quantifies how much contention the lockstep regime was inflating.

---

## What was built

| Piece | Where it lives | Notes |
| --- | --- | --- |
| `--trace-list FILE` CLI flag | [include/comparch/cli.hpp](../include/comparch/cli.hpp), [src/common/cli.cpp](../src/common/cli.cpp) | Manifest input. Mutually exclusive with `--trace` and `--trace-dir`. |
| Manifest format | (text file, no schema) | One trace path per line. Lines starting with `#` are comments. Blank lines skipped. Whitespace trimmed. Relative paths resolve against the manifest's parent directory (so manifests are portable). |
| `resolve_per_core_traces()` helper | [src/full/full_mode.cpp](../src/full/full_mode.cpp) (anonymous ns, near the top) | Single resolution path for both `--trace-dir` and `--trace-list`. Hard error if entry count != `cfg.cores`. |
| `trace_label()` helper | [src/full/full_mode.cpp](../src/full/full_mode.cpp) | Centralizes the "what to print in the report header / use as the run-dir stem" decision now that two CLI shapes both feed the per-core path vector. |
| Tests | [tests/full/test_full_mode.cpp](../tests/full/test_full_mode.cpp) | Two new cases: (1) manifest mixing two trace flavors across 4 cores runs cleanly, (2) entry-count mismatch throws `trace::TraceError`. Existing 9 full-mode tests still pass — `--trace-dir` path is unchanged. |
| Example manifests | [traces/synth/mix_loop_seq_tiny.txt](../traces/synth/mix_loop_seq_tiny.txt), [traces/synth/mix_stream_rand_tiny.txt](../traces/synth/mix_stream_rand_tiny.txt), [traces/synth/mix_full_tiny.txt](../traces/synth/mix_full_tiny.txt), [traces/synth/mix_full_small.txt](../traces/synth/mix_full_small.txt) | Used by the experiment below. Also serve as documentation-by-example. |

What was **not** built (deferred):

- **Coherence-mode wiring.** `--mode coherence` still only accepts `--trace-dir`. The Network constructor that loads project-3 `.trace` files takes a directory path; passing it a list would touch [src/coherence/network.cpp](../src/coherence/network.cpp), which is more invasive than the full-mode change and the validation pathology lives in full mode anyway.
- **Sweep harness integration.** [scripts/run_sweep.py](../scripts/run_sweep.py) iterates over `--trace-dir` paths; teaching it to emit and consume manifests is its own piece of work. The follow-up section sketches what that would look like.
- **JSON config integration.** Traces stay a CLI concept, separate from the machine-config JSON. Mixing workload-shape into the machine config seemed like the wrong coupling.

---

## Why this matters: the lockstep contention regime

Phase 5B's full-mode driver originally constructed per-core trace paths by hardcoded naming convention:

```cpp
// pre-change, full_mode.cpp:401-408
for (int i = 0; i < cfg.cores; ++i) {
    const auto p = *cli.trace_dir /
                   ("p" + std::to_string(i) + ".champsimtrace");
    ...
}
```

This works for single-workload sweeps but has a built-in trap: in [traces/synth/](../traces/synth/), the per-core files are *symlinks* to the same `raw.champsimtrace`:

```
$ ls -la traces/synth/sequential_tiny/
p0.champsimtrace -> raw.champsimtrace
p1.champsimtrace -> raw.champsimtrace
p2.champsimtrace -> raw.champsimtrace
p3.champsimtrace -> raw.champsimtrace
```

Every "4-core run" of a synthetic trace was four cores reading the *exact same byte sequence*. The address streams `gen_trace` produces depend only on record index `i` (or seed), so all four cores walk identical address sequences in lockstep. That's the unifying root cause of the cap-hits and FSM crashes catalogued in [11-validation-bugs.md](11-validation-bugs.md): a contention regime that doesn't occur in real multi-program workloads.

The runtime always supported per-core distinct content — every core constructs its own `trace::Reader` ([full_mode.cpp:441-442](../src/full/full_mode.cpp#L441-L442)) — but there was no configuration mechanism to point each core at a different file. `--trace-list` is that mechanism.

---

## Experiment

**Setup.** `configs/baseline.json`, `--cores 4`, all 5 protocols (MI / MSI / MESI / MOSI / MOESIF). Two phases:

- **Phase A — homogeneous baselines.** Each of the four `synth/*_tiny` flavors via `--trace-dir`, all 5 protocols. 20 runs.
- **Phase B — heterogeneous mixes via `--trace-list`.** Three manifests, all 5 protocols. 15 runs.
  - `mix_loop_seq_tiny` — 2× loop + 2× sequential (private + shared)
  - `mix_stream_rand_tiny` — 2× stream + 2× random (both shared, different access patterns)
  - `mix_full_tiny` — 1× each of {loop, sequential, stream, random}

Every run retires exactly 400K instructions total (4 cores × 100K records each). The interesting metric is **cycles**: how long the simulator takes to drain 400K retirements.

### Phase A — homogeneous baselines (cycles, M)

| Trace | MI | MSI | MESI | MOSI | MOESIF |
| --- | ---: | ---: | ---: | ---: | ---: |
| `loop_tiny` | 1.69 | 0.11 | 0.10 | 0.11 | 0.09 |
| `sequential_tiny` | 6.90 | 13.87 | 12.91 | 13.87 | 12.43 |
| `stream_tiny` | 6.89 | 13.87 | 12.90 | 13.87 | 12.43 |
| `random_tiny` | 6.87 | 13.16 | 12.13 | 13.16 | 11.69 |

All 20 runs report `Status: Simulation complete`. No FSM crashes — the bucket 1A/1B/1C/1D fixes from [11-validation-bugs.md](11-validation-bugs.md) hold up on `_tiny` data.

The split is dramatic: `loop_tiny` runs ~100× faster than the other three on every directory protocol. Loop traces have private working sets per core (each core sweeps a different region), so coherence traffic is near-zero. The other three flavors all touch a shared address space, and lockstep × 4 cores produces the contention regime we're trying to characterize.

A few sub-points worth flagging:

- **MI is faster than MSI / MOSI on shared-style traces** (~6.9 M vs ~13.9 M). MI has no S state — every read is exclusive — so there's no SS→SM upgrade dance, just ping-pong invalidations. That ping-pong is cheap when there's only one shared line in flight, which is what stride-64 lockstep produces.
- **MSI ≡ MOSI exactly** in cycles. The O state never gets entered: the workload doesn't transition through clean-shared-then-modified, so the additional protocol state is unused.
- **MESI ≈ MOESIF** (MOESIF slightly cheaper) — Forward state shaves ~4% when there are many sharers.
- **MI on `loop_tiny`** is anomalously slow (1.69 M vs ~0.10 M for everyone else). With private working sets there's no real sharing, but MI's read-acquires-exclusive policy still costs a directory round-trip on every cold miss; the others coalesce cold misses into a single shared/exclusive grant.

### Phase B — heterogeneous mixes (cycles, M)

| Mix | MI | MSI | MESI | MOSI | MOESIF |
| --- | ---: | ---: | ---: | ---: | ---: |
| `mix_loop_seq_tiny` | 5.48 | 6.95 | 6.58 | 6.95 | 6.58 |
| `mix_stream_rand_tiny` | 9.74 | 13.52 | 12.70 | 13.52 | 12.53 |
| `mix_full_tiny` | 9.87 | 10.24 | 9.96 | 10.24 | 9.93 |

All 15 runs complete. The picture varies sharply across mixes:

- **`mix_loop_seq_tiny` is the headline.** Compare against `sequential_tiny` homogeneous on the same protocol: MESI drops from 12.91 M to 6.58 M, a **2× reduction**. The mechanism is straightforward — replacing 2 of the 4 contending cores with private-working-set loop cores cuts the directory's transient-state pressure roughly in half. Each shared line now has 2 sharers competing, not 4. This is the lockstep-contention pathology getting partially defused.

- **`mix_stream_rand_tiny` shows almost no improvement.** MESI: 12.70 M (mix) vs 12.13 M (random homogeneous). Both flavors are shared-style, so 4 cores still hammer shared addresses — just two streams of them instead of one. The directory's combinatorial transient-state load barely budges.

- **`mix_full_tiny` is in between.** MESI: 9.96 M, ~22% below `random_tiny` and ~23% below `sequential_tiny` homogeneous. One private (loop) core lifts the others; three contending shared-style cores still anchor the cycle count.

A couple of secondary observations:

- **MI flips sign on `mix_stream_rand`.** It runs *worse* than the homogeneous baseline (9.74 M vs 6.87–6.89 M for stream / random homogeneous). Heterogeneous shared-style streams hit different lines, so MI's invalidate-on-every-read strategy churns across more distinct cache lines instead of ping-ponging a small working set. Lockstep is *better* for MI on this workload — a counter-intuitive but consistent result.
- **MSI / MOSI continue to match cycle-for-cycle** across all mixes. The O state is still unused because the synthetic traces don't produce read-after-write-after-read patterns.

### Phase C — small-trace pathology check

The validation report's [recommendation section](11-validation-bugs.md#L353) flagged the homogeneous `_small` MI runs as the only remaining failure mode after the FSM fixes — pure tail-latency from 4 cores × 1 M records of lockstep contention. A targeted comparison (MESI only, the protocol with the cleanest baseline data):

| Run | Cycles (M) | Wallclock | Status |
| --- | ---: | ---: | --- |

*[results filled in once the small comparison completes]*

---

## CPI summary (MESI, headline protocol)

Putting Phase A and Phase B side by side at MESI, the protocol whose behavior is closest to "what a real CMP would use":

| Workload | Cycles (M) | CPI (cycles / 400K retired) | vs `sequential_tiny` |
| --- | ---: | ---: | ---: |
| `loop_tiny` | 0.10 | 0.26 | -99.2% |
| `sequential_tiny` | 12.91 | 32.3 | — (baseline) |
| `stream_tiny` | 12.90 | 32.3 | -0.1% |
| `random_tiny` | 12.13 | 30.3 | -6.0% |
| `mix_loop_seq_tiny` | 6.58 | 16.5 | **-49%** |
| `mix_stream_rand_tiny` | 12.70 | 31.8 | -1.6% |
| `mix_full_tiny` | 9.96 | 24.9 | **-22.8%** |

The heterogeneity wins are real but workload-dependent. Mixing private with shared roughly halves contention. Mixing two shared streams doesn't help much. Mixing all four (one private, three shared) lands in between.

---

## Conclusions

1. **The feature works and the runtime was already heterogeneity-ready.** The change touches one flag in the CLI, ~50 lines in the full-mode driver (helper extraction + report-formatting fallbacks), and adds two integration tests. Nothing in the OoO core, caches, adapter, network, or directory needed any change — every core has always run an independent `trace::Reader`. Only the configuration plumbing was missing.

2. **Lockstep `_tiny` runs are not a worst-case adversarial regime — they're an artifact of the symlinked trace layout.** The cycle counts in Phase A's table are systematically inflated relative to a realistic 4-program workload. Future tier sweeps should run heterogeneous mixes by default; the existing `_tiny` numbers stay useful as a "lockstep stress" reference, not as a representative measurement.

3. **`mix_loop_seq` is the simplest contention-reducing mix.** Halving directory pressure by spending 2 of 4 cores on private working sets is a clean way to characterize how much of the homogeneous cycle count is contention vs intrinsic memory latency. The 49% MESI cycle reduction is the empirical answer.

4. **MI is the protocol most sensitive to access-pattern uniformity.** It's faster than MSI / MOSI / MESI / MOESIF when the working set is small and lockstep (a few lines, ping-pong); it's *slower* when the working set spreads out across many lines (heterogeneous shared streams). The validation report's MI tail-latency outliers are consistent with this: small, lockstep, but spread address range.

---

## Follow-ups

- **Coherence mode (`--mode coherence`)** — same treatment. `Network`'s 5A constructor takes a `trace_dir` path and constructs its own per-core readers internally; switching it to take a `vector<fs::path>` is mechanical but touches [src/coherence/network.cpp](../src/coherence/network.cpp) and the project-3 regression fixtures, which expect the directory layout.

- **Sweep harness** — extend [scripts/run_sweep.py](../scripts/run_sweep.py) with a `--mix-tier` mode that generates manifests on the fly (e.g. all `C(4, 4)` combinations of the four synth flavors as 4-core mixes) and runs them as a third tier alongside `tiny` and `small`. Should close out the 2 remaining MI tail-latency outliers from [11-validation-bugs.md:350-353](11-validation-bugs.md#L350-L353) by giving them a non-lockstep regime to run in.

- **Real workload mixes.** Once the ChampSim-format converter ([tools/proj2_to_champsim](../tools/proj2_to_champsim)) is exercised against a real corpus (gem5 / Pin / SimPoint outputs of SPEC, server, etc.), heterogeneous mixing becomes the natural way to study CMP-style multiprogrammed workloads instead of staying inside the synthetic-pathology regime.
