# proj3 regression fixtures

Copies of the deterministic per-core memory traces and reference outputs
from `project3_v1.1.0/`. The `test_coherence` regression cases diff
captured stdout against `ref_outs/<P>_core_<N>.out` byte-for-byte;
keep these files in sync if the legacy project ever changes.

## Files

- `traces/core_{4,8,12,16}/p<i>.trace` — per-core memory traces
  (`r 0xADDR` / `w 0xADDR` lines). Same files as
  `project3_v1.1.0/traces/`.
- `ref_outs/<PROTO>_core_<N>.out` — captured `dirsim` reference output
  for `(PROTO, N) ∈ {MSI, MESI, MOSI, MOESIF} × {4, 8, 12, 16}`.
  MI has no fixture: project3 ships no MI reference output, so MI
  parity is covered by synthetic unit tests only.

## Regenerating from the legacy source

```sh
cd ../../../../../project3_v1.1.0
make
for proto in MSI MESI MOSI MOESIF; do
  for n in 4 8 12 16; do
    ./dirsim -p $proto -n $n -t traces/core_$n > ref_outs/${proto}_core_${n}.out
  done
done
```

The fixtures here were copied from project3_v1.1.0 verbatim — no editing.
