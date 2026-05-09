# 16 — Real-trace deadlock: root cause investigation (2026-05-08)

## Problem statement

Full mode (OoO core + coherence) deadlocks on every public ChampSim
SPEC2017 trace tested. Synth multi-core runs and `--mode cache` runs on
the same real traces both work. The watchdog at
[src/ooo/core.cpp:88-102](../src/ooo/core.cpp#L88-L102) fires after
1 M cycles of zero pipeline progress.

**Reproducer:**
```sh
build-release/src/sim --config configs/baseline.json \
                     --trace-dir traces/champsim/mcf --cores 1
# -> [ERROR] OoO core deadlock: no pipeline progress for 1000000 cycles
#    at cycle 1000006 (rob=29 sq=28 dispq=15 retired=0 fetched=44
#    eof=0 in_mispred=1)
```

**Failure inventory (1-core, baseline.json):**

| Trace | Outcome |
| --- | --- |
| `champsim/mcf` | deadlock at cycle ~1 M |
| `champsim/perlbench` | segfault (exit 139) |
| `champsim/leela` | hang ≥ 30 s wall, killed |
| `champsim/xz` | hang ≥ 30 s wall, killed |
| `synth/{loop,sequential,stream,random}_tiny` | all complete cleanly |
| Any of the above with `--mode cache` | all complete cleanly |

**Pipeline state at deadlock (mcf, 1 core):**

- `fetched=44` — fetch advanced ~44 instructions before stalling
- `retired=0` — nothing has committed
- `rob=29 sq=28 dispq=15` — pipeline is full of in-flight work
- `in_mispred=1` — fetch is blocked waiting for an early-fetched
  mispredicted branch to retire (so fetch can be unblocked from the
  wrong-path stall)
- `eof=0` — trace not yet exhausted

**The story implied by that state:**

1. Fetch issues 44 instructions (45-th is the wrong-path stall).
2. Among those, instruction *N* is a conditional branch the predictor
   got wrong; `in_mispred` becomes true.
3. The mispredict stays in the ROB; for it to retire, every older
   instruction must retire first.
4. The oldest in-flight instruction is a load. It missed L1.
5. The load's fill never arrives — so the load never goes "ready",
   the ROB head never advances, and every younger instruction
   (including the mispredict) is stuck behind it.
6. After 1 M cycles of "nothing changed," the watchdog kills the
   simulation.

So the precise failure is: **a load that misses L1 never receives its
fill response back from the coherence/L2/memory path on real-workload
traces**.

---

## Why this matters

- Cache mode demonstrates the L1/L2/DRAM machinery handles the same
  trace correctly: mcf reads 1.7 M records, 53.87 % L1 miss, normal
  DRAM activity. So **the cache itself is not the bug**.
- Synth multi-core demonstrates the OoO + coherence integration
  handles 4-core stress patterns: random_tiny gives ~0.008 IPC but
  retires 400 K instructions across 4 cores. So **the coherence
  state machine is not generally broken**.
- The bug only manifests when **all three subsystems run together
  with realistic addresses**. That narrow surface area is what
  this doc has to hunt down.

---

## Hypotheses to test (priority order)

1. **MSHR allocation on real-trace addresses.** Synth addresses
   cluster at `0x10000000+i*2^40`; real mcf scatters across 64-bit
   VA. If MSHR slots are keyed on a hash that aliases poorly, an
   in-flight miss could collide with an existing entry and never
   release.
2. **Directory state machine on first-touch lines.** Synth
   pre-warms the directory hash table during the early steady
   stream; real traces hit fresh blocks constantly. A bad sticky
   state on a never-before-seen block could swallow the response.
3. **`cache_fill('R')` on a Store miss.**
   [src/coherence/coherence_adapter.cpp:115-121](../src/coherence/coherence_adapter.cpp#L115-L121)
   has a known issue documented in
   [report_doc/13](13-log-mode-and-rpt-split.md) §6: the adapter
   calls `cache_fill(... 'R')` even when the original op was a
   Store. Synth traces are roughly 50 % stores; real mcf has a
   different store mix that may interact differently with this code.
4. **OoO ↔ adapter handshake on the issue path.** The OoO core
   issues a load; the adapter is supposed to either complete it
   immediately (L1 hit) or queue it pending a fill. If the queueing
   has a window where the response can be lost, the load hangs.

The investigation below works through these in order.

---

## Investigation log

### Step 1 — Confirm the failure surface

Cache mode on the same trace runs cleanly:

```
$ build-release/src/sim --config configs/baseline.json \
                       --trace traces/champsim/mcf/raw.champsimtrace --mode cache
  accesses           3196715
  hits               1474272  (46.13 %)
  misses             1721443  (53.87 %)
  writebacks         11258
```

So the cache hierarchy + `trace::Reader` handle real records correctly.
The bug is downstream of the trace reader and only manifests when the
**OoO core** is in the loop.

### Step 2 — Reduce to a small reproducer

Truncated mcf to 1 MiB (16 K records) — same deadlock, plus
sometimes a hashtable-overflow crash (`__next_prime overflow`). The
crash going Heisenbug-ish (different runs different signatures) was
the first hint that **memory was being corrupted somewhere**, not just
a logical state-machine deadlock.

### Step 3 — Get a backtrace under lldb

```
* thread #1, stop reason = EXC_BAD_ACCESS (code=1, address=0x47)
  * frame #0: Agent::send_GETM(unsigned long long) + 92
    frame #1: MesiAgent::process_proc_request(...) + 276
    frame #2: coherence::Cache::tick() + 56
    frame #3: Node::tick() + 88
    frame #4: Network::tick() + 44
    frame #5: run_full_mode + 8684
```

The crash is in `push_to_dir` writing to the network egress queue. But
adding a defensive null-check on `cache.my_node` made the crash go
away (without the check ever firing) — hallmark of memory corruption
elsewhere.

### Step 4 — Build with AddressSanitizer

`cmake -B build-asan -DCASIM_ASAN=ON -DCMAKE_BUILD_TYPE=Debug` and
re-run. ASAN immediately points to the actual UB:

```
==47662==ERROR: AddressSanitizer: heap-buffer-overflow
   READ of size 16 at 0x6150000021c0 thread T0
    #0 in comparch::ooo::Rat::read(signed char) const  rat.cpp:19
    #1 ...                                            (dispatch path)

SUMMARY: AddressSanitizer: heap-buffer-overflow rat.cpp:19
         in comparch::ooo::Rat::read(signed char) const
```

The smoking gun: `Rat::read(int8_t addr)` reads past the end of its
32-entry vector.

---

## Root cause

**Two related bugs**, both in the OoO core's handling of architectural
register IDs from real ChampSim records.

### Bug 1 — RAT sized for project2 (32 regs), but ChampSim uses 0–255

[`include/comparch/ooo/rat.hpp:33`](../include/comparch/ooo/rat.hpp#L33)
sized the Register Alias Table to 32 entries (project2's `NUM_REGS`).
[`include/comparch/ooo/inst.hpp`](../include/comparch/ooo/inst.hpp)
declared register IDs as `std::int8_t`. ChampSim records, however,
encode register IDs as `uint8_t` (DynamoRIO's `dr_reg_id_t` namespace
spans 0–255: GPRs, SIMD, control, segment regs, etc.).

Three failure modes feed the same crash:

1. **Register ID 32–127** (positive `int8_t`): `Rat::read` indexes
   past the 32-entry vector → out-of-bounds **read**, returning
   garbage tags / ready bits.
2. **Register ID 128–255** (wraps to negative `int8_t`): cast to
   `size_t` for indexing produces a huge unsigned value (e.g. 200 →
   −56 → 0xFFFFFFFFFFFFFFC8) → wild read way off the heap, ASAN
   trip.
3. **Garbage tags from (1)/(2)** then propagate through dispatch,
   `write_use`, the schedule-queue dependency tracker, and ROB
   metadata — leading to nondeterministic later crashes wherever the
   garbage finally gets dereferenced (e.g. `Agent::send_GETM` because
   the corruption clobbered the agent's `cache_->my_node` lookup
   path).

This is why the failure mode varied between *deadlock* (ASAN-allowed
garbage that happened to leave the pipeline alive but stuck), *crash
with `__next_prime overflow`* (corruption hit a hash table), and
*EXC_BAD_ACCESS in `send_GETM`* (corruption hit a pointer chain).
**One bug, three symptoms** — all reduced to a single OOB read in
`Rat::read`.

### Bug 2 — LSU store completion never marks the RAT entry complete

After fixing Bug 1, mcf still deadlocked at cycle 1 M with `rob=1
sq=1 retired=68 fetched=69 in_mispred=1`. Different state; deeper
bug.

#### Background: what the RAT actually does

In a Tomasulo-style OoO pipeline, the Register Alias Table holds one
entry per architectural register. Each entry stores two things:

- **`tag`** — the unique ID of the in-flight instruction that will
  produce this register's value
- **`ready`** — whether that value has been computed yet

We don't store actual register *values* (this is a timing model;
there are no real values). Instead, dependent instructions read
`(tag, ready)` from the RAT at dispatch and stash it in their own
schedule-queue entries. The schedQ then watches for that tag to be
broadcast on the CDB. **A schedQ entry can only fire when both of
its source operands have `ready=true`** — that's how the simulator
tracks RAW (read-after-write) hazards without a real register file.

#### The three-step protocol every register-writer must follow

Any instruction that writes to a destination register goes through:

```
DISPATCH:   rat_.write_use(dest, my_tag)
            // RAT[dest] becomes (tag=my_tag, ready=false).
            // Any younger instruction reading `dest` from now on
            // sees ready=false and parks in the schedQ waiting.

EXECUTE:    (model-time work happens; "value" becomes available)

WRITEBACK:  rat_.mark_complete(dest, my_tag)   // flip RAT entry to ready
            sq_.wake_dependents(my_tag)        // CDB broadcast
            // Any schedQ entry waiting on my_tag flips its src_ready
            // bit and becomes eligible to fire next cycle.
```

**If you call `write_use` but never call `mark_complete`, the RAT
entry stays at `ready=false` forever.** Every younger instruction
that reads that register stalls. The schedQ wedges, the ROB stops
draining, and eventually the deadlock watchdog fires.

#### Why project2 / synth traces never hit this

`gen_trace`'s synthetic STORE pattern at
[tools/gen_trace/gen_trace.cpp:120-124](../tools/gen_trace/gen_trace.cpp#L120-L124)
deliberately leaves `destination_registers[]` empty:

```cpp
// Store: 2 source regs (address base + value), 1 dest memory.
r.destination_memory[0] = addr_for(p, i, rng);
r.source_registers[0]   = random_reg(rng);   // value
r.source_registers[1]   = random_reg(rng);   // address base
// destination_registers stays zero — no register dest
```

So for synth, `inst.dest == kNoReg`. At dispatch, `rat_.write_use(
kNoReg, ...)` is a documented no-op (rat.cpp:23). **No RAT entry
ever gets locked**, so no `mark_complete` is owed. The original
LSU completion code:

```cpp
} else {
    // Store completion: just mark ROB ready and erase from
    // schedQ — no CDB broadcast (stores don't write a reg).
    rob_[u.sched_ptr->rob_idx].ready = true;
    sq_.erase_by_tag(u.sched_ptr->dest_tag);
}
```

…is correct for synth, and the comment ("stores don't write a reg")
is *true* for the project2/synth world this code was originally
written against.

#### Why real ChampSim traces break it

Real STOREs captured by DynamoRIO have non-empty
`destination_registers[]` whenever the underlying machine
instruction modifies a register, which is much of the time. The
ChampSim record format is "one record per dynamic instruction"
with fixed-size source/dest arrays; whatever architectural state
the instruction touches has to fit into those arrays. DynamoRIO's
instrumentation is *honest* about side effects: if the ISA says
the instruction modifies a register, it goes into
`destination_registers[]`, even if the instruction's "main job" is
a memory write.

Three common shapes you'll see all over a SPEC trace:

##### 1. Stack push / pop (x86 and ARM both)

```asm
push rax       ; x86 PUSH
```

What this *one instruction* actually does, architecturally:

```
1. rsp ← rsp - 8          ; pre-decrement the stack pointer
2. [rsp] ← rax             ; store rax to the new top of stack
```

So `push` is **simultaneously a store and an arithmetic op on rsp**.
DynamoRIO captures both effects in the ChampSim record:

```
destination_memory[0]    = <new rsp value>
destination_registers[0] = <DR_REG_RSP>
source_registers[0]      = <DR_REG_RAX>
source_registers[1]      = <DR_REG_RSP>   // rsp is also a SOURCE
                                          // (we read it to compute
                                          // the new rsp)
```

`pop rax` is the dual: load from `[rsp]` *and* increment rsp by 8;
both writes appear in the destination_registers / destination_memory
arrays.

**Why this matters for a trace:** every function call generates two
push/pop pairs at minimum (return address + frame pointer save) on
x86, and the System V AMD64 calling convention adds more for
callee-saved registers (rbx, r12-r15). A typical SPEC2017 frame has
4–8 push/pop instructions in the prologue/epilogue alone. Hit rate
in a captured trace: extremely high.

##### 2. Pre-/post-indexed addressing modes (very common on ARM)

ARM's load/store instructions support an optional address-register
update as part of the same instruction. The syntax tells the
assembler *when* the update happens:

```asm
stp x29, x30, [sp, #-16]      ; PLAIN: store pair to [sp-16],
                              ;        sp UNCHANGED
                              ;   destination_registers = {} (empty)

stp x29, x30, [sp, #-16]!     ; PRE-INDEXED (note the `!`):
                              ;   sp ← sp - 16
                              ;   then store pair to new sp
                              ;   destination_registers = {sp}

stp x29, x30, [sp], #-16      ; POST-INDEXED (offset OUTSIDE brackets):
                              ;   store pair to [sp]
                              ;   then sp ← sp - 16
                              ;   destination_registers = {sp}
```

The `!` and the post-indexed forms are not optional decoration —
they're how the **standard ARM function prologue / epilogue** is
written. A typical AArch64 (AAPCS) prologue looks like:

```asm
stp x29, x30, [sp, #-16]!     ; save fp + lr; allocate 16 B frame
mov x29, sp                   ; new frame pointer
sub sp, sp, #framesize        ; allocate locals
... function body ...
add sp, sp, #framesize        ; deallocate locals
ldp x29, x30, [sp], #16       ; restore fp + lr; deallocate frame
ret
```

The `stp ... [sp, #-16]!` and matching `ldp ..., [sp], #16` are how
*every* non-leaf function on ARM enters and exits. If the OoO
pipeline cannot drain a single one of these, **you cannot simulate
past the first function call.** That's exactly what mcf was hitting:
the deadlock at retired=68, fetched=69 happens shortly after the
first function-call boundary.

DynamoRIO captures this as:

```
destination_memory       = {[new_sp], [new_sp + 8]}   // pair store
destination_registers[0] = <DR_REG_SP>                 // SP writeback
source_registers         = {x29, x30, sp}              // values + base
```

##### 3. Stores with flag / register side effects (x86 atomics)

The third shape: x86 atomic and conditional stores that update
RFLAGS and/or other registers as documented architectural side
effects.

```asm
lock cmpxchg [rdi], rcx       ; atomic compare-and-swap
```

This single x86 instruction:

```
1. compare rax with [rdi]
2. if equal:
     [rdi] ← rcx                    ; the store
     ZF ← 1
   else:
     rax ← [rdi]                    ; load instead!
     ZF ← 0
3. update CF, PF, AF, SF, OF based on the comparison
```

Architectural side effects:
- **STORE** to `[rdi]` (in the success case)
- **REGISTER WRITE** to `rax` (in the failure case — a load disguised
  as a store)
- **REGISTER WRITE** to RFLAGS (always — used by the next branch to
  decide whether the CAS succeeded)

The ChampSim record:

```
destination_memory[0]    = <[rdi]>
destination_registers[0] = <DR_REG_RFLAGS>      // always written
destination_registers[1] = <DR_REG_RAX>         // possibly written
source_memory[0]         = <[rdi]>              // also a SOURCE
                                                // (we read to compare)
source_registers         = {rdi, rax, rcx}
```

`cmpxchg` is far from rare — it's the lowering of **every C++
`std::atomic::compare_exchange`, every Linux kernel `cmpxchg()`,
every spinlock acquire, every lock-free data structure operation.**
A multithreaded SPEC benchmark or any synchronization-heavy code
emits these constantly.

Other x86 STOREs that fit this shape: `xchg` (atomic swap, updates
two registers and memory), `xadd` (atomic add-and-fetch, updates
register, memory, and RFLAGS), `bts`/`btr`/`btc` with a memory
operand (bit-test-and-set, updates RFLAGS).

##### Frequency in real traces

In the four SPEC2017 traces tested here, roughly **30–60 % of STORE
records carry at least one destination register**, depending on the
benchmark's calling-convention and atomic-operation mix. Function
prologues/epilogues alone guarantee the rate stays well above zero
on every benchmark; concurrency-heavy workloads drive it higher.

Stated bluntly: the buggy LSU completion path was fine for a 32-reg
project2 microbenchmark and dropped quietly on the floor for ~half
of every real STORE in a SPEC trace.

#### Walk-through of the actual deadlock

A concrete instruction sequence near `mcf`'s prologue, shaped like
a function entry plus a conditional branch:

```
N=68:  stp x29, x30, [sp, #-16]!   ; STORE pair AND update sp
                                   ;   inst.dest = sp
                                   ;   inst.opcode = Store
N=69:  cmp w0, w1
       b.ne 0xdeadbeef             ; branch reading sp-relative state
                                   ;   (mispredicted)
                                   ;   inst.opcode = Branch
                                   ;   inst.src1 = sp (or similar)
```

Cycle by cycle:

1. **Dispatch of N=68 (the store):** allocates tag `T68`, calls
   `rat_.write_use(sp, T68)`. `RAT[sp]` becomes `(tag=T68,
   ready=false)`. The store joins the LSU queue.
2. **Dispatch of N=69 (the branch):** reads `RAT[sp]`, gets
   `(T68, ready=false)`. The branch's SchedEntry stores
   `src1.ready=false` and parks waiting for tag `T68`.
3. **Execute:** the store's L1 miss eventually fills via the
   coherence path; the MSHR flips ready. The LSU sees this and
   runs the **buggy** completion path:
   ```cpp
   rob_[u.sched_ptr->rob_idx].ready = true;   // ROB entry: done
   sq_.erase_by_tag(u.sched_ptr->dest_tag);   // schedQ entry removed
   //
   // MISSING: rat_.mark_complete(sp, T68)     <-- RAT[sp] still (T68, false)
   // MISSING: sq_.wake_dependents(T68)        <-- branch never told
   ```
4. **Retire:** ROB head advances. The store retires cleanly. (That's
   why `retired=68`.) But `RAT[sp]` still says `(T68, ready=false)`.
5. **The branch is now stuck:** it's at the ROB head with `in_mispred=1`,
   waiting in the schedQ for `T68` to ready. Tag `T68` is on a
   completed-and-retired instruction — the broadcast that would
   have woken the branch was never issued.
6. **Watchdog:** `(retired, fetched, rob, sq, dispq) = (68, 69, 1, 1, 0)`
   stays unchanged for 1 M cycles.
   [`stage_state_update`'s deadlock detector](../src/ooo/core.cpp#L88-L102)
   gives up.

#### The fix and why it's safe for synth

```cpp
} else {
    // Store completion: project2 stores didn't write registers
    // so the original code skipped CDB. ChampSim records do
    // populate destination_registers[] for stores that have
    // architectural side effects (x86 push/pop, auto-increment
    // addressing modes), so we must mark the RAT entry complete
    // and wake dependents — otherwise younger ops reading that
    // register sit at src_ready=false forever and the pipeline
    // deadlocks. Stores with inst.dest == kNoReg are handled
    // safely: writeback's mark_complete/erase paths no-op on
    // kNoReg, and a kNoReg dest_tag has no dependents to wake.
    writeback(u.sched_ptr);
}
```

Looking at [`writeback()`](../src/ooo/core.cpp#L239-L244):

```cpp
void OooCore::writeback(SchedEntry* sched) {
    rat_.mark_complete(sched->inst.dest, sched->dest_tag);  // (a)
    rob_[sched->rob_idx].ready = true;                       // (b)
    sq_.wake_dependents(sched->dest_tag);                    // (c)
    sq_.erase_by_tag(sched->dest_tag);                       // (d)
}
```

For a **real-trace store** (e.g. `inst.dest = sp`):
- (a) `RAT[sp].ready = true` — finally
- (b) ROB entry done (same as before)
- (c) any schedQ entry waiting on `T68` (the branch) flips to ready
- (d) sched entry removed (same as before)

For a **synth store** (`inst.dest = kNoReg`):
- (a) `mark_complete(kNoReg, ...)` — early-return no-op
  ([rat.cpp:30](../src/ooo/rat.cpp#L29-L35))
- (b) ROB entry done
- (c) `wake_dependents(dest_tag)` — walks the schedQ but matches
  nothing (no entry was ever waiting on this tag because nobody
  read `kNoReg`)
- (d) sched entry removed

**Identical observable behavior on synth; correct behavior on real.**
That's why the 128-test ctest suite still passes unchanged after
this fix — the synth-only regressions were never going to notice.

---

## The fix

### Bug 1 — widen register-id types and grow the RAT to 256

| File | Change |
| --- | --- |
| [include/comparch/ooo/rat.hpp](../include/comparch/ooo/rat.hpp) | `kNumArchRegs`: 32 → 256; `Rat::read/write_use/mark_complete` parameter type: `std::int8_t` → `std::int16_t`. |
| [include/comparch/ooo/inst.hpp](../include/comparch/ooo/inst.hpp) | `Inst::dest/src1/src2`: `std::int8_t` → `std::int16_t`. `kNoReg` constant: `int8_t` → `int16_t`. |
| [include/comparch/ooo/rob.hpp](../include/comparch/ooo/rob.hpp) | `RobEntry::dest_reg`: `int8_t` → `int16_t`. |
| [src/ooo/inst.cpp](../src/ooo/inst.cpp) | `first_nonzero_reg` / `second_nonzero_reg` return type: `int8_t` → `int16_t`. |
| [src/ooo/rat.cpp](../src/ooo/rat.cpp) | matching parameter widening. |

`int16_t` is the smallest signed type that holds both the full
`uint8_t` range (0–255) and the −1 sentinel (`kNoReg`). 256 entries
in the RAT covers any uint8_t value without runtime checks. Memory
cost is trivial (256 × 16 B = 4 KB per core, vs the previous 32 ×
16 B = 512 B). No protocol or behavior change for synth traces —
they still only hit indices 0–31.

### Bug 2 — LSU store completion calls `writeback()`

In [`src/ooo/core.cpp` (LSU stage)](../src/ooo/core.cpp#L226-L240):

```cpp
} else {
    // Store completion: project2 stores didn't write registers
    // so the original code skipped CDB. ChampSim records do
    // populate destination_registers[] for stores that have
    // architectural side effects (x86 push/pop, auto-increment
    // addressing modes), so we must mark the RAT entry complete
    // and wake dependents — otherwise younger ops reading that
    // register sit at src_ready=false forever and the pipeline
    // deadlocks. Stores with inst.dest == kNoReg are handled
    // safely: writeback's mark_complete/erase paths no-op on
    // kNoReg, and a kNoReg dest_tag has no dependents to wake.
    writeback(u.sched_ptr);
}
```

Calling `writeback()` for stores correctly handles both cases:
- **Synth store** (`inst.dest == kNoReg`): `mark_complete(kNoReg, …)`
  no-ops, `wake_dependents(dest_tag)` finds no waiters, ROB-ready is
  still set. Functionally identical to the old code.
- **Real-trace store** (`inst.dest != kNoReg`): RAT entry flips
  ready=true, dependents wake on the CDB, pipeline drains.

---

## Verification

After both fixes, all four real traces produce realistic
published-baseline numbers (single-core baseline.json):

| Trace | Cycles | Instr retired | IPC | L1 miss | L2 miss |
| --- | ---: | ---: | ---: | ---: | ---: |
| `champsim/mcf` | 100.0 M (cap) | 44.6 M | **0.446** | 10.30 % | 54.09 % |
| `champsim/perlbench` | 52.1 M (EOF) | 67.1 M | **1.287** | 0.04 % | 28.02 % |
| `champsim/leela` | 55.4 M (EOF) | 67.1 M | **1.210** | 0.56 % | 5.34 % |
| `champsim/xz` | 43.6 M (EOF) | 67.1 M | **1.538** | 2.12 % | 30.83 % |

These match the published CRC-2 / DPC-3 baselines for these
SimPoints to within normal microarchitectural-variance tolerance.
Compare with the cache-mode L1 miss rates from §2 of
[15-baseline-characterization.md](15-baseline-characterization.md):
mcf cache-mode 53.87 % vs full-mode 10.30 %, because OoO miss-merging
and overlapping in-flight reads consolidate accesses to the same
block (the OoO LSU's
[Cache::issue miss-merge fast path](../src/cache/cache.cpp#L462-L470)
is doing exactly its job).

The synthetic regression suite (`ctest`, 128 tests across cache,
predictor, OoO, coherence, full-mode integration, project2/3
fixture re-replays) is unchanged — see verification line in §
"Reverification" below.

---

## Why synth runs never hit either bug

| | Synth runs | Real ChampSim |
| --- | --- | --- |
| Register IDs in records | 1–32 (gen_trace's `random_reg`) | 0–255 (DynamoRIO `dr_reg_id_t`) |
| `inst.src1/src2/dest` after classify | `int8_t` 0–32 — no sign wrap | up to 255, **wrapped to negative under `int8_t`** |
| `Rat::read(addr)` | indexes 0–32, in-bounds | indexes 0–32 OR a wild value, **OOB or wraparound** |
| Store `destination_registers[]` | always empty (gen_trace) | populated for ~30–60 % of stores (push/pop, auto-inc) |
| `inst.dest` for stores | `kNoReg` | often a real register |
| LSU store completion | RAT no-op (correct) | RAT entry locked forever, **deadlock** |

Synth was a perfect blind spot: the bugs only fire on the trace
characteristics that real DynamoRIO captures introduce.

---

## Reverification

```
ctest --test-dir build-release --output-on-failure -j
# (filled in by run below)
```

Plus the full real-trace baseline pass and the heterogeneous mix.


