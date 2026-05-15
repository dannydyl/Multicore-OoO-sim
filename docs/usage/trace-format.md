# Trace Format

This simulator reads **ChampSim binary traces** — the de facto interchange
format for academic trace-driven out-of-order / cache / branch-prediction
research. Adopting it directly buys interop with the DPC-3, CRC-2, IPC-1,
ML-DPC, and CVP-1→ChampSim trace corpora; we don't invent a new format.

The format's known shortcomings are tracked upstream in
[ChampSim issue #382](https://github.com/ChampSim/ChampSim/issues/382). When a
v2 lands and the public corpora migrate, we follow.

> **Implementation status.** The current code only reads/writes the
> `input_instr` (Standard) variant and only handles **uncompressed**
> on-disk streams. `cloudsuite_instr`, transparent `.xz` (de)compression,
> and the `.meta.json` sidecar are specced below but not yet implemented.
> They land later when results figures need them.

## On-disk

```
foo.champsimtrace.xz       xz-compressed stream of fixed-size records
foo.champsimtrace          uncompressed stream of fixed-size records (tests)
foo.champsimtrace.meta.json optional sidecar (ours, ignored by ChampSim itself)
```

Records are packed (no padding inside the struct), little-endian, written
back-to-back with no header, no length prefix, no checksum. End of file =
end of trace.

## Record variants

Two record shapes exist in the wild. The simulator currently implements
only `input_instr` (Standard); `cloudsuite_instr` is specced here for
future-compatibility but not yet wired up — see the implementation-status
note above.

### `input_instr` (standard, used by DPC-3 / CRC-2 / IPC-1)

```c
#define NUM_INSTR_DESTINATIONS 2
#define NUM_INSTR_SOURCES      4

struct input_instr {
    uint64_t ip;                // instruction pointer
    uint8_t  is_branch;
    uint8_t  branch_taken;
    uint8_t  destination_registers[NUM_INSTR_DESTINATIONS];
    uint8_t  source_registers[NUM_INSTR_SOURCES];
    uint64_t destination_memory[NUM_INSTR_DESTINATIONS];
    uint64_t source_memory[NUM_INSTR_SOURCES];
} __attribute__((packed));        // 64 bytes
```

### `cloudsuite_instr` (server traces)

```c
#define NUM_INSTR_DESTINATIONS_SPARC 4
#define NUM_INSTR_SOURCES_SPARC      4

struct cloudsuite_instr {
    uint64_t ip;
    uint8_t  is_branch;
    uint8_t  branch_taken;
    uint8_t  destination_registers[NUM_INSTR_DESTINATIONS_SPARC];
    uint8_t  source_registers[NUM_INSTR_SOURCES_SPARC];
    uint64_t destination_memory[1];
    uint64_t source_memory[2];
} __attribute__((packed));        // 56 bytes
```

## Field semantics

| Field                    | Meaning                                                            |
| ------------------------ | ------------------------------------------------------------------ |
| `ip`                     | Architectural PC of this instruction.                              |
| `is_branch`              | 1 if this instruction is a branch (cond/uncond/call/ret/indirect). |
| `branch_taken`           | 1 if the branch resolved taken; 0 otherwise. Undefined if not a branch. |
| `destination_registers`  | Architectural register numbers written. `0` = slot unused.         |
| `source_registers`       | Architectural register numbers read. `0` = slot unused.            |
| `destination_memory`     | Virtual store addresses. `0` = slot unused.                        |
| `source_memory`          | Virtual load addresses. `0` = slot unused.                         |

Conventions and consequences:

- **No opcode / instruction class.** We classify on the simulator side: any
  record with `source_memory[0] != 0` is a load; with `destination_memory[0] != 0`
  is a store; with `is_branch` is a branch; everything else is a generic ALU
  op. Multi-cycle latency for MUL/FP is recovered by PC-binned heuristics in
  Phase 4 — this matches what every ChampSim-format consumer does.
- **No memory access size.** We assume each access touches one cache block.
  Consequences for misaligned / multi-block accesses are the same as for
  ChampSim itself.
- **No fall-through target for not-taken branches.** Computable from the next
  record's `ip` if needed.
- **Register `0`** is reserved as "slot unused", not "architectural reg 0".
  Tracers that emit traces must remap any real reg-0 read/write to a different
  number (or omit it).

## Sidecar metadata (optional, ours)

We optionally write a small JSON sidecar next to each trace to record provenance:

```json
{
  "format": "champsim",
  "variant": "input_instr",
  "endianness": "little",
  "record_bytes": 64,
  "record_count": 500000000,
  "source_tracer": "dynamorio-drmemtrace + drmem2champsim",
  "source_workload": "spec2017/lbm/ref",
  "warmup_records": 200000000,
  "sim_records": 500000000,
  "generated_utc": "2026-04-27T15:00:00Z"
}
```

The sidecar is ours; ChampSim itself ignores it. It's purely so we can
reproduce a result a year later without grepping shell history.

## What about the original three projects' formats?

Project1 (`R/W 0x...`), project2 (11-field text per line with embedded cache
hits), and project3 (per-core `r/w 0x...`) are all dropped. None of them ship
in the unified simulator. Anything we want to keep from those traces gets
re-emitted into ChampSim format by a one-shot conversion script if and when
needed; the simulator itself only reads ChampSim binary.
