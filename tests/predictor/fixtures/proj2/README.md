# Project2 cross-validation fixture

`branchsim.champsimtrace` is a small (8000-record, 512 KB) ChampSim binary
trace built from a deterministic project2 text trace. It exists so the
test_proj2_regression suite can pin the new sim's branch-prediction counts
against the reference numbers produced by project2's own `proj2sim -x`
binary on the same input.

## Files

- `branchsim.champsimtrace` — the binary trace consumed by the test.
- `branchsim.expected.txt` — per-predictor (correct, mispredicted) counts
  captured from project2 with default parameters (H=10 P=5 G=9 N=7 T=2).
- `make_trace.py` — regenerates the project2 text trace.

## Regenerating

If the trace ever needs to be rebuilt (e.g. the converter changes), do:

```bash
python3 tests/predictor/fixtures/proj2/make_trace.py > /tmp/branchsim.trace

# Build project2 (one-time, requires only g++ and make):
make -C project2_v2.1.0_all proj2sim

# Capture reference numbers; update branchsim.expected.txt accordingly.
for b in 0 1 2 3; do
    project2_v2.1.0_all/proj2sim -x -i /tmp/branchsim.trace \
        -b $b -H 10 -P 5 -G 9 -N 7 -T 2
done

# Convert to ChampSim binary; this is what the test consumes.
build/tools/proj2_to_champsim/proj2_to_champsim \
    --in /tmp/branchsim.trace \
    --out tests/predictor/fixtures/proj2/branchsim.champsimtrace
```

The trace is deterministic, so re-running `make_trace.py` produces a
byte-identical text file every time.
