# LLS — A study guide

**Purpose.** This is a learning document. It walks through every concept you need to understand the Last-Level Shared (LLS) cache + hybrid coherence design that lives in this simulator after Phases 1 and 2, and it grounds each concept in actual code you can read and run. It assumes you know what an OoO core is and what a cache is, but otherwise defines every piece of jargon as it comes up.

If you want the architectural design rationale, read [10-lls-hybrid-coherence.md](10-lls-hybrid-coherence.md) first; it's a longer-form design note. This document is the *how-do-I-actually-reason-about-this* companion.

---

## 1. The starting picture: why we need any of this

A modern CPU has multiple cores. Each core runs its own instruction stream, but they all share one main memory. Two facts make life hard:

- **Caches.** Each core has private fast memory (caches) so it doesn't go to slow DRAM on every load. If two cores cache the same address, and one of them writes to it, the other core's cached copy is now wrong.
- **Memory latency.** Going off-chip to DRAM costs ~100 cycles. Pulling from a peer core's cache costs ~10–30 cycles. So even when caches *aren't* hot enough to be a correctness problem, *finding the data quickly* is an enormous performance lever.

**Cache coherence** is the protocol that keeps cached copies consistent. **Cache hierarchy organization** decides where each level of cache lives and who shares what. The two pieces are deeply linked, and our project's "LLS + hybrid coherence" change is about taking a cleaner-but-naive design (private L2 + dedicated directory node) and replacing it with something closer to what real chips do.

> **Jargon checkpoint.**
> - **Cache line / block.** The unit a cache moves around. Almost universally 64 bytes today. Same word: "line" = "block" = "cacheline."
> - **Tag, index, offset.** A cache looks up a line by splitting an address into three fields. Index picks which set, tag identifies which line within the set, offset is the byte within the line.
> - **Set associativity (assoc / N-way).** A *set* holds N lines. To install a new line, the cache picks one of the N to evict (LRU, etc.). Higher assoc = fewer conflict misses but more compare logic.
> - **Hit / miss.** Hit = the line is resident; miss = it isn't. Misses cost latency.
> - **MSHR (Miss Status Holding Register).** Tracks an outstanding miss while the cache waits for the fill from below. Bounds in-flight misses. See [src/cache/mshr.cpp](../src/cache/mshr.cpp).

---

## 2. The two coherence flavors, and why neither alone is enough

You only need to understand two ideas to read coherence papers:

### 2.1 Snooping

Every cache has a port on a shared bus. When core 0 misses on line `0xCAFE`, it **broadcasts** "anyone got 0xCAFE?" Every cache hears this. Whoever has it responds; if nobody does, memory does.

```
        ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐
        │ L1₀ │  │ L1₁ │  │ L1₂ │  │ L1₃ │      "anyone got 0xCAFE?"
        └──┬──┘  └──┬──┘  └──┬──┘  └──┬──┘
           │        │        │        │
        ═══╧════════╧════════╧════════╧═══ shared bus / ring
                    request seen by every cache
```

**The good.** When the data lives in a peer cache, the responder talks straight back to the requester — *one round trip* and you're done. Latency-optimal.

**The bad.** Every miss is *broadcast* to N caches. Bandwidth grows like O(N) per miss. The bus saturates. Doesn't scale much past ~16 cores in practice.

### 2.2 Directory

A separate structure (the *directory*) tracks, per cache line: *who currently caches it, in what state*. When core 0 misses, it sends a **unicast** message to the directory; the directory then forwards the request to whichever caches actually need it.

```
        ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐
        │ L1₀ │  │ L1₁ │  │ L1₂ │  │ L1₃ │
        └──┬──┘  └──┬──┘  └──┬──┘  └──┬──┘
           │        │        │        │
           └────────┴────┬───┴────────┘
                  ┌──────▼──────┐
                  │  Directory  │   "0xCAFE is in L1₂, state=M"
                  │  presence:  │
                  │  {2}, M     │
                  └─────────────┘
```

**The good.** Messages are unicast. Bandwidth scales with *actual* sharing, not core count. Works at hundreds of cores.

**The bad.** Extra hop: requester → directory → owner → requester. Three hops vs. snoop's one round-trip when the data is in a peer.

### 2.3 The honest summary

| Property | Snoop | Directory |
| --- | --- | --- |
| Per-miss msgs | Broadcast (≈N) | Unicast (1–few) |
| Latency when peer has it | **Lowest** (1 round trip) | +1 hop through directory |
| Hardware cost | Snoop bandwidth | Directory storage |
| Scales to | ~16 cores | hundreds |

Real chips don't pick one. They **layer** them. That's "hybrid coherence." The simulator's old design was pure directory; our LLS work is the first step toward hybrid.

---

## 3. What "Last Level Shared" really means

There are four words to dissect.

- **Last level**: the cache closest to memory. When this cache misses, you go off-chip to DRAM. In our project, the L2 was the last level; with LLS, *the LLS is the new last level* and the per-core L2 disappears (or, in some real chips, sits between L1 and the LLS).
- **Shared** (vs. **private**): visible to *every core*, not partitioned per-core. A 1 MB shared LLS lets a single workload use all 1 MB. Four 256 KB private L2s only give one core 256 KB even if the others are idle.

> **Jargon checkpoint.** "**LLC**" (Last-Level Cache) is the industry name. Some chips have private LLCs (each core has its own L3) and some have shared. We use "**LLS**" to keep the "shared" assumption explicit, since the project moves *to* sharing as a deliberate design point.

### 3.1 Why share at all? Three reasons in order of importance

1. **Capacity sharing.** A workload running hot on one core can use the *whole* LLS. Private caches cap each core at its slice even if neighbors are idle.
2. **Producer-consumer locality.** When core 0 produces a value and core 1 consumes it, a shared cache means the consumer's L1 miss can be served from the LLS instead of memory. Without a shared cache, the value lives only in core 0's private L2 — core 1's L1 miss either snoops core 0 (bandwidth) or goes to memory (latency).
3. **A single, on-chip directory home.** Every off-chip miss has to go through the LLS anyway, so the LLS is the natural place to put the directory state. You get the directory "for free" in terms of pipeline placement. *This is the design lever the LLS+hybrid pattern actually exploits.*

### 3.2 Inclusion: a critical invariant

When you have an LLS, you have to decide what relationship it has with the L1s above it.

- **Inclusive.** Every line in any L1 is also in the LLS. Strong invariant; expensive (the L1 contents are duplicated in the LLS), but lets the LLS act as a **snoop filter**: if a line isn't in the LLS, no L1 has it, so don't bother broadcasting. Big bandwidth win. **This is what doc 10 picks for v0.**
- **Non-inclusive.** No relationship enforced. The LLS holds whatever fits; an L1 can have lines the LLS doesn't. More capacity-efficient, but you lose the snoop-filter shortcut and need a *separate* directory/snoop-filter to know who has what.
- **Exclusive.** A line in any L1 is *not* in the LLS. The LLS only holds *victims* (lines that were evicted from L1). Most aggressive use of capacity (no duplication). AMD Zen does this approximately.

**The modern trend** (Intel Skylake-X onward, AMD Zen, ARM CMN) is non-inclusive with a separate snoop filter, because per-core L2s have grown to 1 MB+ and inclusive duplication wastes too much LLS capacity. But for a 4-core teaching simulator with 256 KB L2s, **inclusive is the right v0** — fewer edge cases, snoop filter is essentially free.

> **Why we deferred strict inclusion.** Inclusion's hard part isn't the read path (it's just "did the line get installed in the LLS too?") — it's the **eviction path**: if the LLS evicts a line, every L1 that has it must also drop its copy ("back-invalidate"). That requires a new message kind plus per-agent recognition. Phase 2c v0 keeps the LLS but skips back-invalidates; the directory protocol is unaffected because per-line *protocol state* lives in the directory entry, not in the LLS itself. See the comment in [src/coherence/directory.cpp:`schedule_data_response`](../src/coherence/directory.cpp).

---

## 4. The protocol state-name jungle

If you've read about coherence, you've seen MSI, MESI, MOSI, MESIF, MOESI, MOESIF, MI… Almost all of them are the same idea with different optimizations. Each letter is a *cache-line state*.

| Letter | Meaning | Property |
| --- | --- | --- |
| **M** — Modified | I have the only copy and it's dirty (memory is stale). | Must writeback on eviction. |
| **O** — Owned | Multiple copies exist; I'm the *owner* and my copy is dirty (memory is stale). | Must writeback on eviction. Other holders are read-only. |
| **E** — Exclusive | I have the only copy and it's clean (matches memory). | Can silently upgrade to M. No writeback needed on eviction. |
| **S** — Shared | I have a clean copy; others may also. | No writeback. Can't write without first asking. |
| **I** — Invalid | I don't have the line. | Treated as "not present." |
| **F** — Forward | (Intel MESIF) Like S, but *I'm* nominated to respond to other cores' read requests. | Avoids redundant simultaneous responses from many sharers. |

Now the protocols:

- **MI** (toy). Only M and I — there's no way for two cores to share a line. Every read by a different core *invalidates* the previous holder. Educational only; bandwidth-disastrous on real workloads. (See our [Phase 2c result](../report/balanced_4core_mi_c4_het_real_v3__proto_mi/coherence.rpt) — MI does 15× more memory writes than MESI on a heterogeneous mix.)
- **MSI**. M, S, I. The classic minimal protocol. Reads can be S; writes must be M (and require invalidating other S holders).
- **MESI**. MSI + the **E** state: when a single core reads a line and nobody else has it, you grant E (clean exclusive) instead of S, so a subsequent write doesn't need to broadcast invalidates. *Silent upgrade.*
- **MOSI**. MSI + **O**: lets one cache hold a dirty copy with others sharing clean copies, avoiding the immediate writeback when an M-holder is asked for a read share.
- **MESIF**. Intel's flavor: MESI + **F** to avoid all-N caches racing to respond to a read.
- **MOESIF**. MESI + MOSI + MESIF combined. Most general; Intel and AMD both use variants.

> **Jargon checkpoint.**
> - **Silent upgrade**: changing state without sending any coherence message (E→M is the canonical example). Stat counter: `CoherenceStats::silent_upgrades`.
> - **Cache-to-cache transfer / intervention**: another core's cache supplies the data instead of memory. Cheaper than memory; this is *the* big latency win. Stat counter: `c2c_transfers`.

The state machines are big and not the point of *this* document. To read one, look at [src/coherence/agent_mesi.cpp](../src/coherence/agent_mesi.cpp): each method handles "what does my cache do when a CPU LOAD/STORE arrives" or "what does my cache do when a network message arrives," indexed by current state. The directory has a parallel state machine in [src/coherence/directory_mesi.cpp](../src/coherence/directory_mesi.cpp).

---

## 5. The hardware-cost picture

Three resources scale differently between snoop and directory:

| Resource | Snoop | Directory |
| --- | --- | --- |
| Bus / ring **bandwidth** | Linear in N (every miss broadcasts) | Logarithmic-ish (only relevant caches messaged) |
| **Storage** | Almost none (each cache has its own state) | Per-line directory entry (presence vector + state). For N cores and L lines, ~N·L bits. |
| **Latency** when data is in a peer | 1 round-trip | 3 hops (req → dir → peer → req) |

The hybrid trick is to use snoop **only** when it'll save those 2 extra hops, and directory **otherwise**. To decide which to use, you need to know whether the line *might be in a peer L1*. The inclusive LLS gives you that for free: if the LLS has the line, broadcast a snoop; if it doesn't, no L1 can possibly have it (by inclusion), so go straight to the directory.

This is exactly what real chips do, just with much more elaborate snoop filters once core counts grow.

---

## 6. What we built — Phases 1 and 2 in code-grounded detail

The implementation is layered so each phase produces a runnable simulator. Right now (after Phase 2) the simulator runs in two modes, picked by `coherence.cache_mode` in your config JSON.

### 6.1 The config knob

[configs/baseline.json](../configs/baseline.json) currently has:

```json
{
  "l1": { "size_kb": 32, "assoc": 8, "hit_latency": 2, ... },
  "l2": { "size_kb": 256, "assoc": 8, "hit_latency": 10, ... },
  "coherence": { "protocol": "mesi" }
}
```

You can now opt into shared LLS by setting:

```json
{
  "l1": { "size_kb": 32, "assoc": 8, "hit_latency": 2, ... },
  "lls": { "size_kb": 1024, "assoc": 16, "hit_latency": 10, ... },
  "coherence": {
    "protocol": "mesi",
    "cache_mode": "shared_lls",
    "inclusion": "inclusive"
  }
}
```

When `cache_mode = "shared_lls"`, the per-core L2 is *not built* and the L1 sinks straight into the coherence adapter. See [src/full/full_mode.cpp:819-846](../src/full/full_mode.cpp#L819-L846).

### 6.2 The data path, side by side

**private_l2 mode** (the original):

```
core → L1 → L2 → CoherenceAdapter → coherence::Cache → Agent → ring → DirectoryNode → Memory
                  ^^^^^^^^^^^^^^^^^                                    ^^^^^^^^^^^^^
                  L2 filtered misses                                   sole gatekeeper
```

**shared_lls mode** (after Phase 2):

```
core → L1 → CoherenceAdapter → coherence::Cache → Agent → ring → DirectoryNode (with LLS inside) → Memory
                                                                  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
                                                                  on miss, check LLS first; hit = fast,
                                                                  miss = mem_latency + install
```

The change is structural in two places:

1. **Per-core stack** ([src/full/full_mode.cpp:819-846](../src/full/full_mode.cpp#L819-L846)): no L2 in shared mode. L1's `next_level` is `nullptr` and L1's `coherence_sink` is the adapter directly.
2. **Directory node**: now owns a `LlsCache` ([include/comparch/coherence/lls_cache.hpp](../include/comparch/coherence/lls_cache.hpp)) that gets consulted by [`schedule_data_response`](../src/coherence/directory.cpp) on every memory-bound request.

### 6.3 The LLS class itself

`LlsCache` is purpose-built — *not* a `cache::Cache`. The real `cache::Cache` is a complex pipeline-aware thing with MSHRs, prefetchers, write policies. The LLS just answers two questions:

- "Is block X resident?" → `contains(block)` (no LRU update)
- "Install block X, what's the LRU victim if any?" → `access(block)` (returns hit/miss + evicted-victim id)

That's it. ~80 lines of code, no MSHRs, no async. Why? Because in our design the LLS is queried by the directory in response to coherence messages — it's not on a per-cycle pipeline path. This matches how real chips work: the LLS slice's "data array" is logically separate from the protocol agent.

The LLS also supports a **disabled mode** (size = 0). When `cache_mode = "private_l2"`, the directory still constructs an `LlsCache` but with size 0 — every access misses, no install. This lets the directory's miss-path code branch on `lls.access(...)` without an extra null check, and keeps the private_l2 path byte-equivalent to the old code (verified by all 137 tests still passing).

### 6.4 The directory's new helper: `schedule_data_response()`

This is where the LLS plugs into the protocol logic. Before Phase 2, every "fetch from memory" site in each protocol's tick function inlined this:

```cpp
request_in_progress = true;
response_time = current_clock_ + settings_.mem_latency;
target_node = ...;
tag_to_send = ...;
```

After Phase 2, all 18 of those sites (across MI/MSI/MESI/MOSI/MOESIF) collapse to one call:

```cpp
schedule_data_response(block, requester);
```

The helper ([src/coherence/directory.cpp](../src/coherence/directory.cpp)) does:
- LLS access. If hit: response in `lls_hit_latency` cycles, no `memory_reads` counted.
- LLS miss: response in `mem_latency` cycles, install in LLS, set `pending_lls_miss = true`.
- Response handler in each protocol: `if (pending_lls_miss) ++stats_.memory_reads;`.

This is a tiny refactor with a big leverage. In private_l2 mode the LLS is disabled → every "access" misses → response = `mem_latency`, `memory_reads++` → identical to legacy behavior. In shared_lls mode the LLS catches reuse and changes response latency.

### 6.5 What we deferred (the v0 simplifications, in plain English)

Two things we *didn't* implement that strictly we should have:

1. **Back-invalidates on LLS eviction.** Under inclusive policy, when the LLS evicts a line, the L1 holders are supposed to also drop it. Doing this requires a new message kind ("back-invalidate, no response needed") because existing `REQ_INVALID` is only handled by S-state agents (it's part of the S→M upgrade path, not a generic "drop everything" message). For v0, we skip it and document that the LLS is "advisory" — it tracks residency for hit-rate measurement, but L1 copies persist regardless. The protocol's correctness is unaffected because per-line *protocol state* lives in the directory entry, not the LLS.

2. **The actual snoop layer.** Phase 2 is *just shared-LLS-with-directory* — there's no broadcast snoop on the ring yet. The "hybrid" part is Phase 3 (broadcast routing) + Phase 4 (agents recognizing peer responses). Phase 2 already gives us the LLS-as-capacity-pool benefit; Phase 3+4 give us the snoop-as-latency-shortcut benefit.

---

## 7. Real-world parallels (so the model isn't floating in abstract space)

What we built is a *plausible, simplified version* of how every modern multicore actually does this. Quick sketch:

- **Intel (Sandy Bridge → Skylake-X → Sapphire Rapids).** Distributed LLC sliced one slice per core, all on a *bidirectional ring* (clockwise + counter-clockwise, halves average hop count). A small block called the **CBo** (Caching Box / Caching Agent) per slice owns the directory state for lines homed to that slice. Hashing the address picks the home slice. Protocol is **MESIF**. Pre-Skylake-X, LLC was inclusive; post-Skylake-X switched to non-inclusive with a separate snoop filter. The hybrid spectrum is exposed via "snoop modes" (Source Snoop, Home Snoop, COD/SNC).
- **AMD Zen.** Cores grouped into 4–8-core **CCXs** (core complexes), each with its own L3 acting as that cluster's LLS. Coherence inside a CCX is fast (snoop-like across the cluster's small ring/mesh). Coherence between CCXs goes through **Infinity Fabric** with directory-like filtering. Protocol is a MOESI variant. L3 is mostly-exclusive (holds L2 victims).
- **ARM (CMN-600/700, DSU).** The big-system interconnect is a *mesh* of routers with a distributed system-level cache plus a separate **system-level snoop filter**. Cluster-level (DSU = DynamIQ Shared Unit) has its own shared L3 across a small core group.

Our project's "L1-snoop + LLS-directory on a ring" is the simplest plausible point in this space. Same concepts, simpler mechanics.

---

## 8. Experiments to run yourself (the actually-fun part)

This is what makes the implementation worth having. The simulator already runs both modes; you can produce side-by-side numbers right now without writing any code.

### 8.1 Confirm the LLS does anything at all

Run the same workload under private_l2 and shared_lls. The "loop" trace has reuse and should show the LLS catching most misses; the "sequential" trace should show ~0% LLS hits.

```bash
# Workload with reuse → expect LLS hit rate >50%
./build-release/src/sim --config /tmp/lls_smoke.json \
    --trace-dir traces/loop_64 --tag exp_loop_lls

# Sequential streaming → expect LLS hit rate ≈ 0% (every line touched once)
./build-release/src/sim --config /tmp/lls_smoke.json \
    --trace-dir traces/synth/sequential_tiny --tag exp_seq_lls
```

Then `cat report/loop_64_mesi_c4_exp_loop_lls/coherence.rpt` and look at the "Shared LLS" section. Verified during Phase 2: 73.8% hit rate on loop_64, 0% on sequential.

> **Why this is interesting.** The LLS hit rate isn't a property of the LLS — it's a property of the **workload's reuse distance**. Workloads whose hot working set fits in the LLS see big wins; streaming workloads see nothing.

### 8.2 Capacity-share: same total bytes, private vs. shared

Run with 4×256 KB private L2 (1 MB total) vs. 1×1 MB shared LLS. Same protocol (MESI), same workload, same trace.

The interesting question is *not* "which is better on average" — it's **which workloads each one wins on**:
- Workloads where one core hot-runs while others idle → shared wins (uses the full 1 MB)
- Workloads where all cores have similar working sets that fit in 256 KB each → roughly equal
- Workloads where caches *interfere* (e.g., one core thrashes another's hot lines from the shared cache) → private can actually win

This is a textbook tradeoff and you can produce real numbers for it.

### 8.3 Protocol × cache_mode matrix

Under shared_lls, do the protocols still look identical on the heterogeneous-mix workload (the "headline result" from [report_doc/17](17-heterogeneous-real-mix-results.md))? Or does the LLS change the relative ordering?

Hypothesis (the design doc supports it): on a no-sharing heterogeneous mix, the LLS doesn't *change* protocol behavior — it just changes capacity. So protocols should still be near-identical IPC, but with a different absolute number from the LLS hit rate. Worth testing.

### 8.4 LLS size sweep

Vary `lls.size_kb` and watch hit rate climb until it saturates (working set fits) then plateau. This is the *Mattson stack* curve, a classic performance-engineering exercise.

---

## 9. What's next (a preview of Phases 3 and 4)

Phase 2 gives us the **shared-LLS capacity pool** but no snoop layer. The directory still mediates every request. Phases 3 and 4 add the snoop fast-path:

- **Phase 3** introduces broadcast message kinds (`SNOOP_GETS`, `SNOOP_GETM`, `DATA_SNOOP`, `SNOOP_NACK`) and teaches the ring node how to fan a broadcast out to all peers. New tests at the network level only — no agent changes yet.
- **Phase 4** extends each protocol's agent to **recognize peer responses**. When core 0's L1 misses, it broadcasts a snoop; if core 1 has the line in M/E/F, it replies with `DATA_SNOOP` directly. Core 0 fills its L1 from the peer instead of waiting for the directory. This is the **intervention** path — the latency-win the hybrid pattern was designed to give.

These two phases together turn "shared LLS with directory" into "shared LLS with hybrid (snoop + directory)" — the actual Phase 6 picture from doc 10.

---

## 10. Glossary (everything alphabetized for re-reference)

- **Agent**: per-cache, per-protocol state machine that decides what to do on processor and network events. See [src/coherence/agent.hpp](../include/comparch/coherence/agent.hpp).
- **AMAT** (Average Memory Access Time): `hit_latency + miss_rate × miss_penalty`. Hennessy & Patterson Ch. 2. Used in our reports.
- **Back-invalidate**: under inclusive policy, when the LLS evicts a line, the directory tells the L1 holders to also drop it. Not implemented in v0.
- **CBo (Caching Box / Caching Agent)**: per-slice unit on Intel ring chips that owns the directory entries for lines homed to that slice.
- **CCX (Core Complex)**: AMD's 4–8-core cluster with its own L3.
- **C2C / Cache-to-cache transfer / Intervention**: peer cache supplies the data instead of memory. The headline latency optimization in coherence protocols. Stat: `c2c_transfers`.
- **CCB / Caching agent**: see CBo.
- **CMN / DSU**: ARM's mesh-interconnect / cluster-shared-L3 IP blocks.
- **Coherence**: keeping cached copies of the same line consistent across cores.
- **DirectoryController**: holds per-line state (presence vector, dir state). One per chip in our project. See [include/comparch/coherence/directory.hpp](../include/comparch/coherence/directory.hpp).
- **Directory entry**: state for one line: which sharers have copies, in what protocol state, dirty bit. Sized ~N bits per line for an N-core chip (presence vector) plus a few bits of state.
- **DRAM**: off-chip main memory. Our model: 100-cycle access. Real: 200–400 ns ≈ 600+ cycles at 3 GHz.
- **Eviction**: removing a line from a cache to make room. If the line was dirty, you owe a writeback.
- **Exclusive (E) state**: clean, sole holder. Can silently upgrade to M.
- **F (Forward) state**: like S, but I'm nominated to respond to GETS broadcasts. Avoids redundant peer responses.
- **GETS / GETM / GETX**: protocol messages a cache sends when it needs a line for read (GETS), write (GETM), or write-after-promote (GETX in some flavors).
- **Hit / miss**: whether the looked-up line is resident in this cache.
- **Home / home node / home slice**: the directory location responsible for a given address. In Intel chips, picked by hashing the address across LLC slices.
- **Inclusion (inclusive / non-inclusive / exclusive)**: relationship between a cache and the level above it. See §3.2.
- **Infinity Fabric**: AMD's chip-level interconnect connecting CCXs and IO dies.
- **Intervention**: see C2C.
- **Invalidation / REQ_INVALID**: directory message telling a sharer to drop its copy.
- **Invalid (I) state**: I don't have the line.
- **LLC (Last-Level Cache)**: industry name; may be private or shared depending on chip.
- **LLS (Last-Level Shared)**: this project's name for an explicitly-shared LLC.
- **LRU (Least Recently Used)**: eviction policy that kicks the longest-untouched line in a set.
- **M (Modified) state**: dirty, sole holder.
- **MESI**: M+E+S+I. Adds silent-upgrade via E.
- **MESIF**: MESI+F. Intel's flavor.
- **MI**: M+I. No sharing supported. Educational only.
- **Miss penalty**: cycles cost when a cache miss happens. Our L1: 10 (= L2 hit) or 100 (= mem). LLS-aware: 10 (LLS hit) or 100 (LLS miss).
- **MOESI / MOESIF**: M+O+E+S+I (+F). Most general.
- **MOSI**: M+O+S+I. Owned state.
- **MSHR (Miss Status Holding Register)**: tracks an outstanding miss while waiting for fill. Bounds in-flight misses per cache.
- **MSI**: M+S+I. Minimal protocol.
- **NoC (Network on Chip)**: the on-chip interconnect carrying coherence messages. Our project: ring. Real chips: ring (Intel) or mesh (Intel newer, ARM CMN, AMD).
- **O (Owned) state**: dirty, but other clean copies exist (other cores can read; only owner can write or supply data on snoops).
- **Presence vector / sharer bits**: per-line bit-vector in the directory: which cores currently cache this line.
- **Private cache**: only one core sees it.
- **Protocol** (in this project): MI, MSI, MESI, MOSI, MOESIF.
- **Recall (RECALL_GOTO_I, RECALL_GOTO_S)**: directory tells an M/E/O holder to give up its line (fully on `_GOTO_I`, downgrade to S on `_GOTO_S`). M-holder responds with DATA_WB.
- **Ring (NoC topology)**: every node has two neighbors; messages traverse hop-by-hop. Project topology.
- **S (Shared) state**: clean, others may also have it.
- **Shared cache**: visible to multiple cores.
- **Silent upgrade**: state change with no message (E → M). Saves a round-trip.
- **Snoop**: broadcast a request to all caches simultaneously.
- **Snoop filter / sparse directory**: small directory whose only job is to suppress snoops that nobody will answer. Replaces the snoop-filter-by-inclusion when the LLS becomes non-inclusive.
- **Sweep**: in this project, running the simulator across a matrix of (config × workload) combinations. See [scripts/run_sweep.py](../scripts/run_sweep.py).
- **Unicast**: one-to-one message. Directory mode is all unicasts; snoop mode is broadcasts.
- **Writeback (DATA_WB)**: dirty cache line being sent back to memory (or LLS, or peer) on eviction or recall.
- **WBWA / WTWNA**: cache write policies. Write-Back Write-Allocate (the modern default; project's L1/L2) vs. Write-Through No-Allocate.

---

## 11. Suggested reading order if you want to go deeper

1. **CMU 15-418 lectures on coherence** — clearest visual treatment of snoop and directory I've found.
2. **Hennessy & Patterson, *Computer Architecture: A Quantitative Approach*, Ch. 5** — multiprocessors and memory consistency. Slow but thorough.
3. **Marty's PhD thesis (Wisconsin, 2008): *Cache Coherence Techniques for Multicore Processors*** — exhaustive treatment of hierarchical and hybrid coherence in CMPs. Reading this is the closest thing to a coherence-design boot camp.
4. **Intel Uncore Performance Monitoring Reference** — what the CBo / ring / LLC slice actually do in shipping silicon. Heavy but real.
5. The Wikipedia pages for "**Cache coherence**" and "**MESI protocol**" are unusually good entry points.

---

## Quick-reference: code map for the LLS implementation

| What | Where |
| --- | --- |
| Config schema | [include/comparch/config.hpp](../include/comparch/config.hpp) (`CoherenceConfig`, `SimConfig.lls`) |
| Settings + parse helpers | [include/comparch/coherence/settings.hpp](../include/comparch/coherence/settings.hpp), [src/coherence/settings.cpp](../src/coherence/settings.cpp) |
| LLS cache class | [include/comparch/coherence/lls_cache.hpp](../include/comparch/coherence/lls_cache.hpp), [src/coherence/lls_cache.cpp](../src/coherence/lls_cache.cpp) |
| LLS unit tests | [tests/coherence/test_lls_cache.cpp](../tests/coherence/test_lls_cache.cpp) |
| Directory wiring | [include/comparch/coherence/directory.hpp](../include/comparch/coherence/directory.hpp), [src/coherence/directory.cpp](../src/coherence/directory.cpp) (search for `schedule_data_response`) |
| Per-protocol tick refactor | `src/coherence/directory_{mi,msi,mesi,mosi,moesif}.cpp` |
| Adapter (nullable L2) | [include/comparch/coherence/coherence_adapter.hpp](../include/comparch/coherence/coherence_adapter.hpp), [src/coherence/coherence_adapter.cpp](../src/coherence/coherence_adapter.cpp) |
| full_mode wiring | [src/full/full_mode.cpp](../src/full/full_mode.cpp) (`shared_lls` boolean threaded through `CoreStack` construction; `l2_stats_or_empty()` helper) |
| Stats fields | [include/comparch/coherence/coherence_stats.hpp](../include/comparch/coherence/coherence_stats.hpp) (`lls_*` fields) |
| Report section | [src/full/full_mode.cpp](../src/full/full_mode.cpp) (`write_coherence`, "Shared LLS" block) |
