# `log.rpt` Format — per-instruction execution trace

When you run with `LOG=1`, the simulator writes a per-instruction trace to
`report/<run>/log.rpt` for the **first 50 dynamic instructions per core**
(see `max_per_core` in
[include/comparch/ooo/trace_logger.hpp](../include/comparch/ooo/trace_logger.hpp#L38)).
This doc explains what's in it and how to read it.

```sh
LOG=1 ./build-release/src/sim --config configs/baseline.json \
                              --trace-list traces/mix_4core.txt \
                              --tag mix
# -> report/mix_4core_mesi_c4_mix/log.rpt
```

The header block at the top of the file (emitted by
[src/ooo/trace_logger.cpp](../src/ooo/trace_logger.cpp)) already gives the
quick reference. This doc is the longer version.

---

## Why only 50 instructions per core?

`log.rpt` is for **eyeballing the front-end behavior of the simulator** —
walking through the first few dozen retirements to confirm that fetch,
issue, retire, and the coherence path are doing what you expect. It is
**not** a per-run profile dump. A 100K-instruction trace at one line per
event would produce hundreds of thousands of lines per core, and the
interesting bugs (mispredicts on the first conditional branch, unexpected
L1 misses on the first reuse) all happen in the first handful of dynamic
instructions anyway.

The cap is hardcoded at 50 in
[full_mode.cpp:780](../src/full/full_mode.cpp#L780); change the third
argument to `TraceLogger` if you need a wider window.

---

## Line format

```
[c<core> cy=<cycle> dyn=<dyn>] <event> ...
```

| Field   | Meaning                                                          |
| ------- | ---------------------------------------------------------------- |
| `c<core>` | Core that emitted the line (`0..cores-1`).                     |
| `cy=`   | The core's cycle counter at the moment the event fired. Width-padded to 8 digits. |
| `dyn=`  | Dynamic-count index of this instruction in the core's input stream (1-based). Same instruction → same `dyn=` across `LSU` and `RETIRE` lines, so you can pair them. |

Two event kinds are emitted: **`LSU`** (load/store issued to L1) and
**`RETIRE`** (instruction commits from the ROB head).

---

## `LSU` events

```
[c<core> cy=<cycle> dyn=<dyn>] LSU      <op> pc=0x... addr=0x...  -> L1 <hit|miss>
```

| Field      | Meaning |
| ---------- | ------- |
| `<op>`     | `LOAD ` or `STORE` — what the LSU is performing. |
| `pc=`      | Program counter of the load/store instruction. |
| `addr=`    | The effective virtual address of the access. |
| `-> L1 …`  | Outcome of the L1 lookup at **issue time**. A miss here means the line wasn't in L1 — the access goes to L2/coherence and a fill arrives later. |

Emitted from
[src/ooo/core.cpp:395](../src/ooo/core.cpp#L395) when an LSU functional
unit picks up a load/store from the schedule queue.

> **Subtle point.** "L1 miss" here is the lookup result *at issue*; the
> line *will* be resident by the time the instruction retires (otherwise
> the retire would never fire). So a `cy=A LSU LOAD ... miss` followed by
> a `cy=B RETIRE LOAD` for the same `dyn=N` tells you the *observed
> memory latency* for that access is roughly `B - A` cycles.

---

## `RETIRE` events

```
[c<core> cy=<cycle> dyn=<dyn>] RETIRE   <op> pc=0x... [addr=0x...] [branch=T|N pred=T|N [*MISPRED*]]
```

Emitted when the ROB head is committed. In an out-of-order pipeline,
instructions execute in dataflow order but retire in **program order**, so
`RETIRE` lines appear in strictly increasing `dyn=` (per core).

`<op>` is one of:

| Opcode   | What it is                                                         |
| -------- | ------------------------------------------------------------------ |
| `ALU`    | Integer/logical op routed to an ALU functional unit (1-cycle by default). |
| `MUL`    | Multi-cycle multiply (3-cycle by default; see `core.mul_stages` in [configs/baseline.json](../configs/baseline.json)). |
| `LOAD`   | Memory load. Pairs with an earlier `LSU LOAD` line at the same `dyn=`. The `addr=` field is included. |
| `STORE`  | Memory store. Pairs with an earlier `LSU STORE` line at the same `dyn=`. |
| `BRANCH` | Conditional or unconditional branch. Carries branch metadata. |

Branch metadata fields (only present for `BRANCH`):

| Field       | Meaning |
| ----------- | ------- |
| `branch=T`  | Branch was actually taken at execute. `branch=N` = not taken. |
| `pred=T`    | Predictor said "taken" at fetch. `pred=N` = predictor said "not taken". |
| `*MISPRED*` | Present iff `branch != pred`. Marks the cycle on which fetch was unblocked from the wrong-path stall. |

Emitted from
[src/ooo/core.cpp:150,165](../src/ooo/core.cpp#L150-L172) — note there are
two call sites: one for the mispredict case (which then breaks out of the
retire loop because fetch was stalled) and one for normal retire.

---

## What "RETIRE ALU" means, conceptually

If you're new to OoO terminology: in this simulator the pipeline is
fetch → decode → rename → dispatch → schedule → execute → **retire**. A
"retire" event is the moment an instruction is removed from the ROB
(reorder buffer) and its effects become architecturally visible — the
point at which it counts toward `instructions_retired`. Until that
happens, an instruction's results are speculative.

So `RETIRE   ALU    pc=0x400000` means: *"the ALU op at PC `0x400000`
finished executing on an ALU functional unit, made its way to the head of
the reorder buffer, and just committed."*

---

## Example walkthrough

```text
[c2 cy=00000002 dyn=00004] LSU      LOAD  pc=0x40000c addr=0x100000c0  -> L1 miss
[c1 cy=00000004 dyn=00001] RETIRE   BRANCH pc=0x42176d  branch=T pred=N  *MISPRED*
[c2 cy=00000004 dyn=00001] RETIRE   ALU    pc=0x400000
[c2 cy=00000004 dyn=00002] RETIRE   ALU    pc=0x400004
```

Reading top to bottom:

1. **`c2 cy=2 dyn=4 LSU LOAD … miss`** — Core 2's 4th dynamic instruction
   is a load that issued to L1 on cycle 2 and missed. The line will be
   fetched from L2 (or further) and filled before this load retires.
2. **`c1 cy=4 dyn=1 RETIRE BRANCH … *MISPRED*`** — Core 1's very first
   instruction is a branch that retired on cycle 4. Predictor said
   not-taken, the actual outcome was taken, so this is a mispredict.
   Fetch on c1 was stalled until this resolved; from cycle 5 onward,
   c1 fetches from the correct path.
3. **`c2 cy=4 dyn=1 RETIRE ALU pc=0x400000`** — Core 2's first
   instruction (an ALU op) retired on cycle 4. (Cycles 0–3 covered
   fetch/decode/rename/issue/execute for it.)
4. **`c2 cy=4 dyn=2 RETIRE ALU pc=0x400004`** — Same cycle, c2's second
   instruction retired too — both fit in the retire-width budget for
   this cycle.

The fact that `dyn=4`'s `LSU` line appears *before* `dyn=1`'s `RETIRE`
line at the top is OoO at work: the load issued speculatively on cycle 2
while older ALU ops were still in the schedule queue. Retire happens in
order on cycles 4+.

---

## Cross-references

- [src/ooo/trace_logger.cpp](../src/ooo/trace_logger.cpp) — the formatter (authoritative).
- [src/ooo/core.cpp:130-176](../src/ooo/core.cpp#L130-L176) — retire loop and `on_retire` call sites.
- [src/ooo/core.cpp:395](../src/ooo/core.cpp#L395) — `on_lsu_issue` call site.
- [src/full/full_mode.cpp:715-786](../src/full/full_mode.cpp#L715-L786) — `LOG=1` env-var handling and logger construction.
- [docs/architecture.md](architecture.md) — pipeline overview.
- [RUNNING.md](../RUNNING.md) — how to invoke the simulator.
