# Study material

Concept-first explanations of the computer-architecture topics this simulator
implements. Each page follows the same template:

> **Concept** (what & why) → **Mechanics** (how it works, formulas / state
> diagrams) → **In this simulator** (which files implement it, with `src/...`
> links) → **Further reading** (papers, textbook chapters).

These pages are written in pedagogical order — foundations first, integration
last — and are being added one at a time. The roadmap below tracks what's
landed and what's queued.

## Roadmap

### A. Out-of-order execution

- [ ] OoO pipeline overview (F → D → R → Dispatch → Issue → Exec → WB → Commit)
- [ ] Tomasulo's algorithm — reservation stations, register renaming, CDB
- [ ] Reorder Buffer — in-order retirement, precise exceptions, RAT/ARF
- [ ] Load/Store Queue — disambiguation, store-to-load forwarding

### B. Branch prediction

- [ ] Fundamentals — BTB, gshare, accuracy/IPC impact
- [ ] Yeh-Patt two-level adaptive predictor
- [ ] Perceptron predictor
- [ ] Hybrid + tournament predictors

### C. Caches

- [ ] Cache fundamentals + AAT (with worked L1/L2 numbers)
- [ ] Replacement policies — LRU / LIP / MIP
- [ ] MSHRs and non-blocking caches
- [ ] Prefetchers — next-line, Markov, hybrid
- [ ] L1/L2 hierarchy — inclusion, write-back vs write-through, WBWA
- [ ] LLC / LLS / NINE — shared vs private last-level

### D. Cache coherence

- [ ] Fundamentals — the problem, SWMR invariant, broadcast vs directory
- [ ] MI and MSI — state diagrams, message types, races
- [ ] MESI — the Exclusive state and silent upgrades
- [ ] MOSI — the Owned state and dirty-sharing
- [ ] MOESIF — Forward state and family comparison
- [ ] Directory vs snoop — sharer vectors, snoop filters, cost models

### E. Memory consistency

- [ ] Consistency models — SC, TSO, PSO, weak ordering; fences

### F. Interconnect

- [ ] Ring topology — hop latency, ordering properties
- [ ] Message classes and deadlock avoidance — virtual channels

---

When you see a checked box above, the corresponding page is live and linked.
Until then, the [phase reports](../report/index.md) are the primary write-up
for each topic — they're development-journal style rather than concept-first,
but they cover the same ground with code paths and IPC numbers.
