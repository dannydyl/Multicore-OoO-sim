# Phase 3 — Branch predictor (`--mode predictor`)

**Goal:** lift the four direction predictors from project2 into a
polymorphic `BranchPredictor` interface, drive them from a ChampSim
trace via `--mode predictor`, and pin the resulting accuracy numbers
against project2's own reference output.

This phase is the smallest in lines of code (~600 LOC of algorithm
plus ~400 LOC of plumbing) but the most algorithmically dense. Each of
the four predictors is a textbook microarchitecture algorithm; the
work is in re-expressing them clean enough that they can be both
re-read as an explanatory artifact and reused unchanged by Phase 4's
OoO core.

---

## What was ported

| project2 piece                       | Where it lives now                                                          | Notes                                                                                                                  |
| ------------------------------------ | --------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| `Counter` saturating counter         | [include/comparch/predictor/saturating_counter.hpp](../include/comparch/predictor/saturating_counter.hpp) | Header-only, modern-C++. Replaces project2's C-bindings counter; same Smith-counter semantics                            |
| Always-Taken predictor               | [src/predictor/always_taken.cpp](../src/predictor/always_taken.cpp)         | Trivial baseline                                                                                                       |
| Yeh-Patt two-level adaptive          | [src/predictor/yeh_patt.cpp](../src/predictor/yeh_patt.cpp)                  | History table + pattern table; FIFO-based pipelining stripped out                                                       |
| Perceptron                           | [src/predictor/perceptron.cpp](../src/predictor/perceptron.cpp)              | Per-PC weight vectors, global history register, online training with `theta = floor(1.93 G + 14)`                      |
| Hybrid (tournament)                  | [src/predictor/hybrid.cpp](../src/predictor/hybrid.cpp)                       | Composes Yeh-Patt + Perceptron, picks per-PC via a 4-bit tournament selector table                                     |
| `BranchPredictor` interface          | [include/comparch/predictor/predictor.hpp](../include/comparch/predictor/predictor.hpp) | Replaces project2's function-pointer table with a virtual class                                                          |
| Stats / accuracy                     | [src/predictor/predictor_mode.cpp](../src/predictor/predictor_mode.cpp)      | Single Stats struct, MPKI / accuracy / mispredict counts                                                                 |
| Project2 trace converter             | [tools/proj2_to_champsim/](../tools/proj2_to_champsim/)                       | One-shot: reads the project2 11-field text format, emits ChampSim binary; used to build the regression fixture          |

What was **dropped**: project2's `RUN_BRANCHSIM` / `RUN_BRANCHSIM_PIPELINED`
modes (the standalone branch-only and 4-stage-pipelined drivers), the
per-predictor FIFOs (`YP_FIFO`, `PCT_fifo`, `TNMT_fifo`) used to delay
training across pipeline stages. `--mode predictor` predicts and
trains synchronously per branch; Phase 4 will re-introduce delay where
the OoO pipeline actually needs it.

---

## Algorithm intuition (skim if you already know these)

### Always-Taken
Predict every branch will be taken. Useful as a baseline floor —
non-trivial predictors should beat ~55-65% on real workloads.

### Yeh-Patt two-level adaptive
Two tables:

- **History Table (HT)** — `2^H` entries. HT[i] holds a P-bit shift
  register recording recent taken/not-taken outcomes for every PC that
  hashes to slot i. Indexed by the low H bits of `(PC >> 2)`.
- **Pattern Table (PT)** — `2^P` entries, each a 2-bit saturating
  counter. Indexed by the P-bit history pulled from HT.

So the predictor effectively asks: *"the last time this PC had history
pattern X, was it taken?"* — and saturating counters smooth out noise.
Captures any pattern with period ≤ 2^P. Reference:
[Yeh & Patt, MICRO-24, 1991](https://dl.acm.org/doi/10.5555/123456).

### Perceptron
For each PC slot (`2^N` of them), keep a vector of `G+1` signed
integer weights. Inputs to the perceptron are the bias (constant +1)
plus the global history register (G bits, encoded as +1 / -1 per
position). Output = dot product. Sign of output is the prediction.

Train online: when wrong, or when the output magnitude is below a
confidence threshold `theta = floor(1.93 G + 14)`, nudge each weight
toward agreement with the actual outcome and clamp to `[-theta,
theta]`. Captures **linearly separable** patterns over G history bits
— including a lot of long-range correlations Yeh-Patt's bounded
history can't see. Reference:
[Jiménez & Lin, HPCA-7, 2001](https://dl.acm.org/doi/10.5555/123457).

### Hybrid (tournament)
Run Yeh-Patt and Perceptron in parallel on every branch. Use a
per-PC tournament selector — 4-bit saturating counters in a
`2^TI`-entry table — to pick which one's prediction to trust.

Train the selector only when the two sub-predictors disagree:

- Yeh-Patt right, Perceptron wrong → counter decrements (more YP)
- Perceptron right, Yeh-Patt wrong → counter increments (more PCT)
- Agree → no movement (no signal)

Both sub-predictors keep training on every branch regardless of which
was selected. Reference:
[McFarling, DEC WRL TN-36, 1993](https://www.hpl.hp.com/techreports/Compaq-DEC/WRL-TN-36.pdf).

---

## The interface
[include/comparch/predictor/predictor.hpp](../include/comparch/predictor/predictor.hpp)

```cpp
namespace comparch::predictor {

struct Branch {
    std::uint64_t ip       = 0;
    bool          taken    = false;   // ground truth — for update only
    std::uint64_t inst_num = 0;       // dynamic instruction count (debug)
};

class BranchPredictor {
public:
    virtual ~BranchPredictor() = default;
    virtual bool predict(const Branch& b)             = 0;
    virtual void update(const Branch& b, bool prediction) = 0;
    virtual std::string_view name() const             = 0;
};

std::unique_ptr<BranchPredictor> make(const PredictorConfig&);

} // namespace
```

The lifecycle is **predict, then update, then move to next branch**.
Phase 4 will need to keep predicted state alive until a branch
resolves at retire — easy to add, but the v1 interface doesn't force
it on `--mode predictor` callers.

---

## Configuration

[configs/baseline.json](../configs/baseline.json):

```jsonc
"predictor": {
  "type": "yeh_patt",
  "history_bits": 10,         // Yeh-Patt H
  "pattern_bits": 5,          // Yeh-Patt P
  "perceptron_history_bits": 9,   // Perceptron G
  "perceptron_index_bits": 7,     // Perceptron N
  "hybrid_init": 2,           // Tournament initial state, 0..3
  "tournament_index_bits": 12,    // Hybrid selector table size = 1 << this
  "tournament_counter_bits": 4    // Hybrid selector counter width
}
```

The block sits at the top level of the config (alongside `l1` / `l2`
/ `memory`) — that's what `--mode predictor` reads. The same struct
also lives nested inside `core.predictor`, where Phase 4's per-core
OoO pipeline will read it; for now Phase 4 hasn't shipped so the
nested copy is a placeholder.

Defaults match project2's defaults: H=10 P=5 G=9 N=7 T=2.

---

## Verification: dual regression strategy

Phase 3 pins predictor accuracy two ways. Both are required to pass
before the phase ships.

### 1. Synthetic patterns with analytically-derivable accuracy
[tests/predictor/test_predictors.cpp](../tests/predictor/test_predictors.cpp)

Each predictor is fed a deterministic synthetic outcome stream (all
taken, alternating, period-N loop, etc.) where the expected accuracy
can be derived by hand from how the algorithm works. For example:

- Yeh-Patt on all-taken with `H=10 P=5`: exactly 6 mispredictions
  total, because the history register fills one bit at a time and
  each transitional history pattern indexes a fresh PT slot. Test
  asserts `correct == 1000 - 6` exactly.
- Perceptron on alternating: weight on `h_1` becomes strongly
  negative, accuracy converges to >97%.

These tests catch coarse algorithm bugs (off-by-one, wrong table
indexing, wrong update rule) without needing any external reference.

### 2. Cross-validation against project2's `proj2sim` binary
[tests/predictor/test_proj2_regression.cpp](../tests/predictor/test_proj2_regression.cpp)

The strongest signal is matching project2's own output bit-for-bit.
The recipe:

1. Generate a deterministic 8000-record project2-format text trace
   with [tests/predictor/fixtures/proj2/make_trace.py](../tests/predictor/fixtures/proj2/make_trace.py).
   It mixes four branch PCs: one always-taken, one always-not-taken,
   one alternating, one period-5 (TTTTN).
2. Build project2 natively (no Docker — its Makefile builds clean):
   `make -C ../project2_v2.1.0_all proj2sim`.
3. Run project2 against the text trace with each of the four
   predictors and the default parameters; capture the output.
4. Convert the same text trace into ChampSim binary using
   `tools/proj2_to_champsim`. Save under
   [tests/predictor/fixtures/proj2/branchsim.champsimtrace](../tests/predictor/fixtures/proj2/branchsim.champsimtrace).
5. Save the captured numbers in
   [tests/predictor/fixtures/proj2/branchsim.expected.txt](../tests/predictor/fixtures/proj2/branchsim.expected.txt).
6. The Catch2 regression test loads the binary trace, runs each of
   the four predictors, and asserts the (correct, mispredicted) pair
   matches `expected.txt` exactly.

The numbers we're pinned against:

| Predictor    | Correct | Mispredicted | Accuracy |
| ------------ | ------- | ------------ | -------- |
| always_taken | 2300    | 1700         | 57.50%   |
| yeh_patt     | 3784    | 216          | 94.60%   |
| perceptron   | 3587    | 413          | 89.67%   |
| hybrid       | 3782    | 218          | 94.55%   |

(out of 4000 branches in the fixture trace.)

These match project2's `proj2sim -x` exactly, with no tolerance —
all four algorithms are deterministic and no float math drifts.

The fixture's `make_trace.py` and `README.md` are checked in too, so
if the trace ever needs regenerating, the recipe is self-contained.

---

## Running it manually

```bash
# Build
cmake --build --preset default --target sim

# Run with the default (yeh_patt) predictor on the regression fixture
./build/default/sim --mode predictor \
    --config configs/baseline.json \
    --trace tests/predictor/fixtures/proj2/branchsim.champsimtrace
```

Output:

```
[INFO] sim 0.0.1 (mode=predictor)
[INFO] predictor mode: yeh_patt (history=10 pattern=5)
[INFO] walked 8000 records (4000 branches)
==== branch predictor stats ====
  predictor          yeh_patt
  records            8000
  branches           4000  (50.00 %)
  correct            3784  (94.60 %)
  mispredicted       216  (5.40 %)
  MPKI               27.000
```

Switch predictor by editing the JSON or by writing a one-line override
file:

```bash
jq '.predictor.type = "perceptron"' configs/baseline.json > /tmp/p.json
./build/default/sim --mode predictor --config /tmp/p.json \
    --trace tests/predictor/fixtures/proj2/branchsim.champsimtrace
```

---

## Code commentary policy (this phase)

Defaulting to "no comments" felt wrong here — the predictor algorithms
are textbook pieces a future me (or anyone landing on the GitHub repo)
will want to re-read for understanding, not just to fix bugs. So this
phase is the one big exception:

- Every predictor source file has a top-of-file paragraph describing
  the algorithm in prose, plus the canonical paper citation.
- Inside `predict()` and `update()`, every logical step has a one-line
  comment explaining the math (`// fold low-order PC bits with global
  history to index the perceptron table`, etc.).
- `saturating_counter.hpp` documents the wrap/clamp semantics on the
  class itself.
- Hardcoded constants in algorithm code carry the name they have in
  the original paper or in project2 (e.g. `// theta = floor(1.93*G +
  14)` from Jiménez 2001).

The bar was: *would a reader who knows C++ but not branch prediction
learn something from this comment?*

Other files (config plumbing, CMake, mode dispatch, tests) follow the
project's normal sparse-comment style.

---

## What's next

The four predictor classes are designed so Phase 4 can drop them into
the OoO core's fetch stage essentially unchanged. The one thing that
will need to grow is the `BranchPredictor` interface: speculative
predictions need to be checkpointed at predict-time and either
committed or rolled back at branch resolve. The plan is to add
something like:

```cpp
virtual Checkpoint predict_speculative(const Branch&) = 0;
virtual void resolve(Checkpoint, bool actual)         = 0;
```

without breaking the v1 `predict()` / `update()` calls — Phase 4 will
get the heavier API; `--mode predictor` keeps using the simpler one.

Until then, `--mode predictor` is the second canonical regression
target alongside `--mode cache`. Phase 4 changes that touch shared
plumbing get cross-checked against both modes.
