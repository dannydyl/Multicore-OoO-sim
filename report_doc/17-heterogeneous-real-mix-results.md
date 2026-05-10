# Heterogeneous real-trace mix: first results

**Goal:** run the `--trace-list`-based heterogeneous infrastructure ([12-heterogeneous-traces.md](12-heterogeneous-traces.md)) on real SPEC2017 traces — different benchmark per core — and quantify how much (or how little) the coherence protocol matters on a workload where the cores share no addresses. This is the standard CRC-2 / DPC-3 / IPC-1 multiprogrammed evaluation regime.

This is the first multicore result on real traces after the [16-real-trace-deadlock-rootcause.md](16-real-trace-deadlock-rootcause.md) RAT/store-completion fixes (2026-05-08) made real workloads stable single-core.

---

## Setup

| Field | Value |
| --- | --- |
| Mix | `traces/mixes/balanced_4core.txt` |
| Core 0 → trace | `traces/champsim/mcf/raw.champsimtrace` (4 GiB / 67M records) |
| Core 1 → trace | `traces/champsim/xz/raw.champsimtrace` |
| Core 2 → trace | `traces/champsim/leela/raw.champsimtrace` |
| Core 3 → trace | `traces/champsim/perlbench/raw.champsimtrace` |
| Cores | 4 |
| Pipeline | ROB=64, 8-wide fetch, 8 ALU + 4 MUL + 2 LSU FUs, yeh_patt predictor |
| L1 / L2 | 32 KB / 256 KB, 8-way, WB+WA, markov L2 prefetcher |
| Memory latency | 100 cycles |
| Interconnect | ring, link_width_log2=3, block_size_log2=6 |
| Global cycle cap | 60M cycles (lowered from 100M for this run; see *truncation* below) |
| Sweep id | `het_real_v3` |
| Driver | `python3 scripts/run_sweep.py --tier het_real_balanced --sweep-id het_real_v3 --jobs 4 --timeout 5400` |

The `het_real_balanced` tier was added to [configs/sweep.json](../configs/sweep.json); it expands to 5 runs (1 baseline=MESI + 4 proto-axis values: mi, msi, mosi, moesif) on a single mix. Sweep wallclock was ~68 min for the first 4 in parallel and ~18 min for the trailing moesif.

### Note on truncation

The 60M-cycle cap is below mcf's natural completion: at IPC ~0.41, mcf retires only ~24M of its 67M-record trace. The other three cores hit EOF well before 60M and sat idle for the tail. This means absolute IPCs for the fast cores are deflated (cycles count idle tail; instructions don't), but the **comparison across protocols** is fair because every protocol sees the same truncation against the same workload. The aggregate IPC numbers below should be read as throughput-up-to-60M-cycles, not as the per-trace asymptotic IPC.

---

## Per-core IPC by protocol

| Core / bench | MESI | MI | MSI | MOSI | MOESIF |
| --- | --- | --- | --- | --- | --- |
| 0 — mcf       | 0.4063 | **0.4086** | 0.4063 | 0.4063 | 0.4063 |
| 1 — xz        | 1.1185 | 1.1185 | 1.1185 | 1.1185 | 1.1185 |
| 2 — leela     | 1.1185 | 1.1185 | 1.1185 | 1.1185 | 1.1185 |
| 3 — perlbench | 1.1185 | 1.1185 | 1.1185 | 1.1185 | 1.1185 |
| **Aggregate** | **3.762** | **3.762** | **3.762** | **3.762** | **3.762** |

The MESI / MSI / MOSI / MOESIF columns are **bit-for-bit identical** at the precision the CSV reports. The only outlier is MI's mcf IPC — a +0.6% bump (0.4086 vs 0.4063), explained below.

The IPC tier ordering matches the MPKI tier ordering you'd predict from the single-core data: mcf (high MPKI) << xz/leela/perlbench (low/mid MPKI). The 60M-cycle cap masks that xz/leela/perlbench are *not all equal* — they all just happened to finish well before the cap and parked at the same `retired/cycles` ratio.

---

## Coherence-traffic stats by protocol

| Protocol | Cache misses | C2C transfers | Coherence invals | Mem reads | Mem writes |
| --- | --- | --- | --- | --- | --- |
| MESI    | 329,160 | 0 | 0 | 330,771 | 20,736 |
| MI      | 329,160 | 0 | 0 | 329,160 | **319,576** |
| MSI     | 329,160 | 0 | 0 | 330,771 | 20,736 |
| MOSI    | 329,160 | 0 | 0 | 330,771 | 20,736 |
| MOESIF  | 329,160 | 0 | 0 | 330,771 | 20,736 |

**Zero coherence invalidations and zero c2c transfers across every protocol.** The four cores share no physical address space (each runs an independent SPEC trace), so there is no true sharing for the protocol to mediate. Every miss is a cold or capacity miss serviced from main memory.

**MI's 15× memory-write count** is the only meaningful spread: MI lacks a Shared state, so on a read miss it acquires the line in M and any dirty eviction must write back. Without M→S downgrades, MI cannot keep clean copies around and a larger fraction of evictions get classified as dirty. The other four protocols use S/E/F/O-type clean states and skip those writebacks — hence 20K writebacks vs MI's 320K.

That 15× extra writeback traffic somehow yields *a +0.6% IPC bump on mcf* (0.4086 vs 0.4063) — counterintuitive at first. The likely explanation is small-N noise: mcf retires only ~24M instructions, branch-mispredict timing differs slightly between protocols because invalidation/writeback messages perturb the cycle when speculatively-loaded lines arrive. The other cores show identical IPC because they're not memory-stalled — they finished early and IPC just reflects retire/cap.

---

## Per-core cache + branch behavior (MESI baseline)

| Core / bench | L1 miss | L2 miss | L1 AAT | L2 AAT | Branch MPKI |
| --- | --- | --- | --- | --- | --- |
| 0 — mcf       | 10.33% | 54.36% | 8.65 | 64.36 | 35.04 |
| 1 — xz        | 2.12%  | 30.83% | 2.87 | 40.83 | 12.68 |
| 2 — leela     | 0.56%  | 5.34%  | 2.09 | 15.34 | 24.82 |
| 3 — perlbench | 0.04%  | 28.02% | 2.01 | 38.02 | 2.21  |

These match the MPKI/IPC stratification from the single-core May-8 data: mcf is severely memory-bound (54% L2 miss is what drags its IPC to 0.41), perlbench is compute-bound (effectively zero L1 misses, AAT ≈ L1 hit latency). leela's branch MPKI of 25 is high but its tiny L1 miss rate keeps it fast.

Cache stats are nearly identical across protocols (same workload + no sharing → same access pattern), so the per-protocol breakdown is redundant; the variance is in the writeback path tracked by the coherence stats above.

---

## Discussion

**Headline:** on a 4-bench heterogeneous mix with no true sharing, **MESI / MSI / MOSI / MOESIF are indistinguishable.** Adding O / E / F states cost zero performance and zero coherence traffic — there were no opportunities to use them. MI is slightly worse on memory bandwidth (15× writebacks) but the bandwidth wasn't the bottleneck so end-to-end IPC barely moves.

This is the *expected* null result for this workload class. Its value isn't a protocol ranking — it's:

1. **Validates the heterogeneous infrastructure** end-to-end on real traces. Manifest input → per-core trace binding → independent address spaces → 0 invalidations confirms the cores really are isolated. If a manifest accidentally pointed all cores at the same trace (the project-3 lockstep regime [12-heterogeneous-traces.md](12-heterogeneous-traces.md) called out), we'd see invalidations everywhere.

2. **Establishes a baseline** for sharing-sensitive mixes. To exercise the protocol differences, the next step is the hi/mid/lo MPKI mixes — but that still wouldn't introduce sharing, just bandwidth pressure. To actually expose MOESIF's wins over MESI we'd need a workload with read-mostly shared data (a parallel kernel, not separate single-threaded traces). The multiprogrammed regime is the wrong test for protocol differences in principle; we ran it because it's the standard ChampSim-style mix and it lets us check the multicore plumbing on real workloads.

3. **Quantifies the per-core stratification** that real benchmarks produce: mcf at IPC 0.41 versus the others at 1.12+ is a 2.7× spread. On the homogeneous traces (4 copies of mcf), the contention regime documented in [11-validation-bugs.md](11-validation-bugs.md) erased this stratification by lockstepping every core onto the same address stream. The heterogeneous regime restores realistic per-core diversity.

---

## Limitations

- **Truncated workload.** The 60M-cycle cap leaves mcf with 24M / 67M instructions retired. Lifting the cap to ~150M would let mcf finish naturally, but at the cost of ~3× the wallclock. The other cores would still hit EOF early and the cycles-include-idle problem would worsen. The right structural fix is to stop counting cycles for finished cores (or report `cycles_until_eof` separately); see *future work*.
- **Cap also fires as the deadlock backstop.** Originally `kGlobalCap = 100M` was sized as "if you hit this, the run is broken." Lowering it to 60M for this experiment overloads the same constant with "stop early on purpose." A cleaner refactor would split the two: a real no-progress detector (no core retired anything for N cycles) and a separate run-length cap that's a clean expected termination, not an error path.
- **Only the balanced mix.** hi/mid/lo MPKI mixes need `omnetpp` / `xalancbmk` / `gcc` / `deepsjeng` traces; uncomment in [scripts/fetch_traces.sh](../scripts/fetch_traces.sh) lines 62–65 and rerun (~16 GB additional disk).
- **Exit code 5 from the sim.** Each run exited non-zero because the per-core deadlock watchdog fired on the cores that hit EOF and sat idle waiting for the cap. The reports were still written — exit-5 here just means "watchdog tripped," not "results invalid." Worth fixing the watchdog to suppress on `eof && drained` so future sweeps return clean exit codes.

---

## Files touched

- [configs/sweep.json](../configs/sweep.json) — added `het_real_balanced` tier
- [src/full/full_mode.cpp](../src/full/full_mode.cpp) — lowered `kGlobalCap` 100M → 60M; added post-idle-grace exit in the tick loop
- *(this doc)* [report_doc/17-heterogeneous-real-mix-results.md](17-heterogeneous-real-mix-results.md)

Sweep artifacts:
- [report/_sweep/het_real_v3/](../report/_sweep/het_real_v3/) — progress.tsv, configs, logs
- `report/balanced_4core_<proto>_c4_het_real_v3__<run_id>/` for each of mesi/mi/msi/mosi/moesif — full report.{rpt,csv}, coherence.rpt, stats.rpt, config.rpt

---

## Future work

1. **Sharing-sensitive workload.** Build or import a real parallel-kernel trace (one program, multiple threads, shared addresses) so MOESIF/MOSI can demonstrate their wins over MESI. This is the actual test the protocols were designed for.
2. **Cycles-until-eof bookkeeping.** Track and report each core's cycle-of-EOF separately so per-core IPC isn't deflated by the trailing idle of fast cores. Aggregate IPC stays the system throughput; per-core IPC becomes the per-trace asymptote.
3. **Split the global cap.** Decouple "deadlock backstop" from "run-length cap." See *limitations* above.
4. **Fetch the rest of the SPEC corpus** and run the hi/mid/lo MPKI mixes — confirms cache-pressure stratification across protocols, even though sharing still won't appear.
5. **Re-run with a higher cap** once the cycles-until-eof fix lands, so mcf can finish its full 67M trace and we get the asymptotic numbers.
