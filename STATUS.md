# Status

What's done, what's known to be missing, what's deferred. Replaces
the phase-numbered checklists that were in `../plan.md` and
`../PROGRESS.md`. Sister doc to [README.md](README.md), which describes
the architecture, and [RUNNING.md](RUNNING.md), which describes how
to drive the binary.

## Done — code in the tree, tested, working

- **Out-of-order pipeline.** Tomasulo-style core with fetch /
  dispatch / schedule / execute / retire, ROB, scheduling queue,
  RAT, configurable FUs (ALU / MUL / LSU). Async LSU through L1-D
  MSHR. Branch predictor at fetch, trained at retire.
- **Branch predictors.** always-taken, Yeh-Patt, perceptron, hybrid,
  tournament. Configurable per run.
- **Cache hierarchy.** Private L1 (WBWA) + private or shared L2.
  Configurable size / assoc / replacement (LRU-MIP / LRU-LIP) /
  prefetcher (+1, Markov, hybrid). MSHR-aware async path used by
  the OoO LSU.
- **Cache coherence.** Directory + ring interconnect with five
  protocols: MI, MSI, MESI, MOSI, MOESIF. Backed by 137+ unit and
  scenario tests across the protocol matrix.
- **Shared LLS.** `cache_mode=shared_lls` removes the per-core L2;
  L1 misses sink directly into the coherence adapter, and the
  directory consults a single shared last-level cache (LLS) as a
  data residency layer before going to memory. Non-inclusive
  non-exclusive (NINE) policy. The coherence mechanism is still
  pure directory — see "known limitations" for what this does and
  does not mean.
- **Multi-thread trace replay (CasimV2).** Per-thread `.casim` files
  in a 32-byte-header + tagged-64-byte-record format. Three record
  kinds: Instr, Sync (lock / barrier / atomic), Lifecycle
  (spawn / join). Readers auto-detect v2 vs legacy ChampSim format
  by magic bytes.
- **Sync coordinator.** Sim-global object that enforces
  happens-before across threads. Two-phase semantics:
  gate-check at fetch (LockAcquire / BarrierLeave) and
  retire-time signal advance (LockRelease / BarrierArrive /
  atomics flow through the pipeline as zero-dep pseudo-Insts
  that fire SyncCoordinator notification at retire). Lock
  contention shows real cycle cost in workloads with memory
  pressure.
- **Synthetic benchmark library.** `tools/casim_synth/` — fluent
  C++ API for hand-rolling multi-thread CasimV2 traces.
  Includes `lock_chain`, `lock_chain_mem`, `dot_product`,
  `producer_consumer` example programs.
- **Pipeline utilization breakdown.** Every run produces
  `report/<trace>_<proto>_c<N>/utilization.rpt` with:
  - Headline `useful retire cycles` / `fetch fired cycles`.
  - Backend (retire) stall breakdown that sums to 100%
    (ROB empty / head not ready).
  - Frontend (fetch) stall breakdown that sums to 100%
    (sync / dispq full / mispred recovery / post-EOF idle).
  - Per-FU utilization (ALU / MUL / LSU).
  - Plus a cross-core aggregate section for diff-friendly
    sweep comparison.

## Known limitations (documented, not bugs)

These are accurate descriptions of what the simulator does NOT
model. Calling these out so anyone reading numbers from this sim
knows what to discount.

- **Coherence is directory-only, no snooping.** All 5 protocols
  (MI/MSI/MESI/MOSI/MOESIF) use point-to-point unicast through a
  central DirectoryController. There is no broadcast / snoop
  primitive, no L1-to-L1 intervention path; every miss goes
  through the directory. On an S→M upgrade with K sharers the
  directory sends K REQ_INVALIDs and collects K INVACKs (O(N)
  network traffic per upgrade) — under a snooping protocol this
  would be a single broadcast (O(1) latency, bandwidth-bound).
  Numbers from this sim are honest directory-based numbers; they
  cannot be compared directly against snoop-based systems.
- **`cache_mode=shared_lls` is NOT hybrid coherence.** The label
  "hybrid" appears in [report_doc/10-lls-hybrid-coherence.md](report_doc/10-lls-hybrid-coherence.md)
  as the *design target* but the implementation is pure directory.
  What `shared_lls` actually does: removes the per-core L2 and
  routes L1 misses straight to the adapter; the directory then
  consults the LLS as a data residency cache before going to
  memory. The protocol mechanism (presence vector + unicast
  REQ_INVALID) is unchanged from `private_l2` mode. There is no
  L1-snoop layer, no peer-L1 intervention. The doc describes a
  two-layer system; the code implements a one-layer system with
  a faster data-fetch path.
- **LLS is a data residency cache, not a snoop filter.** It does
  not answer "would any L1 hit on a snoop?" Sharer tracking lives
  in the directory's presence vector (`kMaxSharers` array per
  block) — same mechanism in `private_l2` and `shared_lls` modes.
- **Interconnect topology: ring only.** `interconnect.topology=xbar`
  is rejected at config-load. XBAR is not implemented.
- **Memory model: SC by default.** No TSO store buffer. The LSU
  completes stores once they reach M state; we don't model
  write-after-write reordering that would surface under TSO.
- **DRAM: fixed-latency.** `memory.latency` cycles per access,
  unbanked. No Ramulator-style bank-conflict / row-buffer model.
- **Strict-inclusive LLS not implemented.** `inclusion=inclusive`
  on `shared_lls` is rejected at config-load. The working path is
  non-inclusive non-exclusive (NINE) — LLS evictions do not
  back-invalidate L1 holders. Strict inclusive would need a new
  back-invalidate message kind plus per-agent handling for
  non-S states.
- **Condition variables stubbed.** `SyncKind::CondWait`,
  `CondSignal`, `CondBroadcast` are accepted but always pass at
  gate time. Pair-matching with spurious-wakeup semantics is
  not implemented. SPLASH-2 / PARSEC programs that use cond vars
  will not see contention from this primitive.
- **No thread scheduler.** `--program` requires `cores == threads`
  (1:1 thread-to-core). Migration / preemption / oversubscription
  are not modeled.
- **No I-cache.** Fetch is idealized: every fetch is one cycle.
  An I-cache MSHR could be added; it isn't today.
- **ChampSim records don't carry an opcode class.** MUL FUs are
  unused under pure-ChampSim traces. CasimV2 records can set the
  `is_mul` opcode hint (used by `casim_synth`); future DR-based
  tracers will need to populate this.

## Future work

Listed in rough priority order. None of these is blocking the
sim from producing meaningful research results today; they're
genuine extensions, not unfinished baseline work.

- **Actual hybrid L1-snoop + LLS-directory coherence.** Build
  out the design in [report_doc/10-lls-hybrid-coherence.md](report_doc/10-lls-hybrid-coherence.md)
  for real. Scope, roughly:
    - Network: a broadcast primitive (ring already has the
      topology for it — one packet rippling clockwise touches every
      node).
    - New message kinds: snoop GETS/GETM that propagate around the
      ring, INTERVENE_DATA for peer-L1 responses.
    - Each agent (MI/MSI/MESI/MOSI/MOESIF): a snoop-receiver path
      that decides intervene-or-pass-along based on local state.
    - Stats: snoop hits (intervention) vs directory hits (LLS or
      memory) — a workload's `intervention rate` becomes a real
      metric instead of being implicit in `c2c_transfers`.
    - Tests: shared-load workload should show ≫0 interventions
      under hybrid, 0 under current directory-only.
  This is substantial — probably a week of focused work — and the
  numbers it would produce are genuinely different from what the
  current sim reports, so any paper-style result comparing snoop
  vs directory has to wait for this.
- **DR-based tracer.** A DynamoRIO client to capture per-thread
  CasimV2 traces from real pthread programs (SPLASH-2, PARSEC).
  Sketched in [docs/tracing.md](docs/tracing.md). Requires a
  Linux/Docker workflow. Untested locally because DR doesn't
  install cleanly on macOS arm64.
- **Strict-inclusive LLS.** Implement the back-invalidate path
  so the `inclusion=inclusive` knob is honest.
- **Condition variable semantics.** Pair-matching with spurious
  wakeups. Needed before running SPLASH-2 / PARSEC workloads
  that lean on cond vars.
- **3-level hybrid cache mode.** L1 + per-core L2 + shared LLS.
  Today's modes are either L1 + per-core L2 (private_l2) or
  L1 + shared LLS (shared_lls); the 3-level combination is a
  new wiring path, not just a config knob.
- **TLB / virtual memory.** All addresses are physical today.
  TLB shootdown is not modeled.

## Test coverage

187 tests, all passing on macOS arm64 (Apple Silicon) and Linux
x86_64. Breakdown:

| Suite | Count | Covers |
| ----- | ----: | ------ |
| `test_cache`             |  ~35 | Cache geometry, MSHR, coherence sink hooks, dirty-bit propagation |
| `test_coherence`         |  ~55 | All 5 protocols, directory writeback, network, LLS, stats |
| `test_predictor`         |  ~25 | Each predictor type + accuracy on canned traces |
| `test_ooo`               |  ~25 | Pipeline correctness, sync e2e, utilization invariants |
| `test_trace`             |  ~20 | ChampSim v1 + CasimV2 round-trip, sync sink, autodetect |
| `test_full`              |  ~15 | End-to-end `run_full_mode` + report-file presence |
| `test_program_manifest`  |   9 | Manifest parser (every failure mode + happy path) |
| `test_sync_coordinator`  |  10 | Lock / barrier / atomic ordering at the coordinator level |

Run `ctest` in the `build/` directory after a CMake build.
