# LLS — Last-Level Shared cache with hybrid (snoop + directory) coherence

**Status:** background reading / design notes for the upcoming Phase 6.
This is **not** an implementation plan — it's the mental model the
implementation will build on. If a sentence here ever contradicts the
code, the code wins.

> **What we have today.** Every core owns a private L1 *and* a private
> L2; coherence between cores is handled entirely by a directory
> protocol on a ring NoC. There is no cache shared across cores — the
> "last level" before main memory is still per-core.
>
> **Where we're going.** A more realistic chip has a **shared
> last-level cache** (LLC, or in this doc *LLS* — "last level shared")
> that sits between every core's private L2 and main memory. Coherence
> at L1 will switch to **snooping** (cheap, fast for small core
> counts), and coherence at the shared L2 will use a **directory**
> built into the LLS itself (scalable, doesn't broadcast). The two
> share the same ring NoC. This is what real Intel client/server
> chips, AMD CCX clusters, and ARM CMN meshes look like in spirit.

---

## 1. Refresher — the two coherence flavors

### 1.1 Snooping

Every cache **listens** ("snoops") on a shared bus. When a core wants a
line it doesn't have, it *broadcasts* a request and every other cache
checks: *do I have this line? if so, in what state?*

- If **someone has the line dirty (M)** they respond with the data and
  hand off ownership.
- If **someone has it clean (S/E)** they may also respond, depending
  on protocol.
- If **nobody has it** the broadcast falls through to memory.

```
        ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐
        │ L1₀ │  │ L1₁ │  │ L1₂ │  │ L1₃ │      "anyone got 0xCAFE?"
        └──┬──┘  └──┬──┘  └──┬──┘  └──┬──┘
           │        │        │        │
        ═══╧════════╧════════╧════════╧════ shared bus / ring
                          ↑
            request is *seen by every cache*
```

**Pros**
- Conceptually simple; one request, one round trip, done.
- Very low latency when the data is in another cache nearby —
  responder talks straight back to requester, no third party.

**Cons**
- Every request is **seen by everyone**. Bandwidth grows like O(N) per
  miss, so as cores multiply the bus saturates.
- Doesn't scale past ~16–32 cores in practice.

### 1.2 Directory

A **directory** is a separate structure that tracks, for each cache
line in memory, which caches currently hold a copy and in what state.
Instead of broadcasting, the requester sends a point-to-point message
to the directory; the directory then forwards the request **only** to
the relevant caches.

```
        ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐
        │ L1₀ │  │ L1₁ │  │ L1₂ │  │ L1₃ │
        └──┬──┘  └──┬──┘  └──┬──┘  └──┬──┘
           │        │        │        │
           └────────┴────┬───┴────────┘
                         │  unicast
                  ┌──────▼──────┐
                  │  Directory  │   "0xCAFE is in L1₂, state=M"
                  │   (sharer   │
                  │    bits)    │
                  └─────────────┘
```

For each line the directory keeps something like:
```
line 0xCAFE : state=Modified, owner=core 2, sharers={2}
line 0xBEEF : state=Shared,   sharers={0, 1, 3}
```

**Pros**
- Messages are **unicast**, not broadcast. Scales to hundreds of
  cores.
- Bandwidth grows with *actual sharing*, not with core count.

**Cons**
- Extra hop: requester → directory → owner → requester (3 hops, vs.
  1 round trip on a snoop bus when the data is local).
- Directory itself costs memory (sharer bits per line × number of
  lines).

### 1.3 The honest summary

| Property | Snooping | Directory |
| --- | --- | --- |
| Per-miss messages | Broadcast (≈N) | Unicast (1–few) |
| Best for | Small N, low-latency intervention | Large N, scalability |
| Latency when data is in a peer cache | **Lowest** (1 round trip) | +1 hop through directory |
| Hardware cost | Snoop bandwidth | Directory storage |
| Real-world cutoff | Up to ~16 cores | Anything bigger |

Real chips don't usually pick one. They **layer** them — which is what
LLS gives us.

---

## 2. What "Last-Level Shared" means

### 2.1 The phrase, decoded

- **Last level**: the cache closest to main memory. When this cache
  misses, you go off-chip to DRAM.
- **Shared**: visible to and accessible by **every core** on the chip
  — as opposed to L1/L2, which are typically private to each core.

Most industry literature calls this the **LLC** (Last-Level Cache).
In this project we'll often write **LLS** to keep "shared" in the
name, since some papers use "LLC" even for private last-level caches
(it's confusing — different docs disagree on the naming).

### 2.2 Why have a shared cache at all?

Three reasons, in order of importance:

1. **Capacity sharing.** A workload running on one core can use the
   *whole* shared cache; with private caches each core is capped at
   its own size. Big sequential workloads benefit a lot.
2. **Producer-consumer locality.** When core 0 produces a value and
   core 1 consumes it, a shared cache means the consumer doesn't have
   to go to memory — the data is already on-chip.
3. **A single, on-chip directory home.** Every off-chip miss has to go
   through the LLS anyway, so the LLS naturally becomes the place
   where the directory lives. You get the directory "for free" in
   terms of pipeline placement.

Reason 3 is the one that matters for *this* project.

### 2.3 Inclusion policies (one paragraph)

A shared cache can be **inclusive** (every line in any L1/L2 must also
be in the LLS), **non-inclusive** (no enforced relationship, but the
LLS isn't actively kept disjoint), or **exclusive** (a line in any L1
is *not* in the LLS — the LLS holds only victims). Inclusive is the
easiest target for hybrid coherence because **a snoop only needs to
ask the LLS** to know whether a line might be cached on-chip — if the
LLS doesn't have it, no L1 has it. We will start there.

---

## 3. The hybrid design we're targeting

### 3.1 The picture

```
   ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐
   │  Core 0  │  │  Core 1  │  │  Core 2  │  │  Core 3  │
   │ ┌──────┐ │  │ ┌──────┐ │  │ ┌──────┐ │  │ ┌──────┐ │
   │ │  L1  │ │  │ │  L1  │ │  │ │  L1  │ │  │ │  L1  │ │     ← snooping
   │ └──┬───┘ │  │ └──┬───┘ │  │ └──┬───┘ │  │ └──┬───┘ │
   └────┼─────┘  └────┼─────┘  └────┼─────┘  └────┼─────┘
        │             │             │             │
   ═════╪═════════════╪═════════════╪═════════════╪═════ Ring NoC
        │             │             │             │
        └─────────────┴──────┬──────┴─────────────┘
                             ▼
                  ┌──────────────────────┐
                  │   LLS  (shared L2)   │           ← directory
                  │   ┌──────────────┐   │
                  │   │  directory   │   │
                  │   │ entries      │   │
                  │   └──────────────┘   │
                  └──────────┬───────────┘
                             ▼
                          Memory
```

**Two coherence layers, one network:**

- **Layer 1 — between L1s (snoop layer).** When core *i* misses in its
  L1 and another L1 might have the line, we broadcast a snoop on the
  ring. Whichever L1 has it responds directly (intervention).
- **Layer 2 — at the LLS (directory layer).** The LLS keeps a
  directory entry per cached line: who has a copy, in what state.
  When the snoop layer can't satisfy a request, the LLS uses its
  directory to either return the line itself or fetch from memory.

### 3.2 What happens on an L1 miss (the canonical walk)

Suppose core 0 issues a load that misses in its L1.

1. **Core 0's L1 broadcasts a snoop request** on the ring (e.g. `GETS
   0xCAFE`). The request travels around the ring; each L1 along the
   way checks its tags.
2. Two outcomes:
   - **Some other L1 has the line.** That L1 responds directly with
     the data (`DATA 0xCAFE → core 0`), updating its own state per
     the protocol (M → O or S, etc.). This is **cache-to-cache
     transfer / intervention**. It's *fast* because it skipped the
     LLS entirely.
   - **Nobody has it (or the protocol decided to consult the
     directory anyway).** The request continues on to the LLS.
3. **At the LLS:**
   - Look up the line in the LLS array.
     - **LLS hit** → LLS returns the line; the directory records
       that core 0 now shares (or owns) the line.
     - **LLS miss** → LLS fetches from memory, installs in its
       array, returns to core 0, and the directory records core 0
       as the (so far only) holder.
4. Core 0's L1 fills the line and the load retires.

### 3.3 Why is this hybrid?

The two layers solve **different problems**:

- The **snoop layer** is great when several cores share the same line
  in their L1s (producer-consumer, locks, shared structures). The
  snoop finds the data in one round trip — no detour through a third
  party. **Latency-optimized.**
- The **directory layer** is great for the long tail — lines that
  aren't actively shared. A snoop finds nothing; rather than burden
  every L1 with another broadcast on the next miss, the directory
  remembers the truth ("nobody has this; go to memory") and answers
  authoritatively. **Bandwidth-optimized.**

The catch: snoop traffic is still O(N) per miss broadcast. So this
hybrid works **at moderate core counts** (4–16-ish), exactly where
this simulator lives. Above that, you start adding **snoop filters**
(small directories whose only job is to suppress snoops that nobody
will answer), which is conceptually the next step but out of scope.

### 3.4 What changes versus today

| Aspect | Today (Phase 5B) | After LLS |
| --- | --- | --- |
| L1 | Private per core | Same — private per core |
| L2 | **Private** per core (currently dead-on-the-stats path) | **Shared** across all cores; this is the LLS |
| Directory location | A separate `DirectoryController` on the ring | Embedded in the LLS — every line has a directory entry alongside its tag |
| L1 miss path | L1 → coherence adapter → directory → memory or peer | L1 → snoop on ring → either peer L1 (intervention) or LLS (directory) |
| Network traffic | All directory unicast | Mix of broadcast (snoop) and unicast (directory) |
| Stats | Per-core L2 hit/miss rate, on a per-core basis | LLS stats become a single shared number — easier to reason about |

### 3.5 Why the ring is fine for this

A ring is a degenerate NoC topology — every node has exactly two
neighbors, and a message ripples around either clockwise or
counter-clockwise. Two reasons it works for our hybrid:

- **Snoops are naturally amenable to rings.** A broadcast on a ring
  is just "forward this packet around the loop until it comes back".
  Every L1 on the way sees it. No router decisions needed.
- **Directory unicasts are also fine.** The ring delivers
  point-to-point messages; the cost is the average hop count
  (≈N/4 hops on a bidirectional ring). For 4–16 cores this is small.

Bigger chips graduate to **2D meshes** with proper routers, but the
coherence *concepts* don't change — only the network mechanics.

---

## 4. How real CPUs do this

### 4.1 Intel — distributed LLC on a bidirectional ring

From Sandy Bridge onward, Intel client/server chips put the LLC in
**slices**, one slice per core. The slices and the cores hang off a
**bidirectional ring** (clockwise + counter-clockwise, so the average
hop count is halved). A line's home slice is determined by hashing
its address; that slice owns the directory entry for the line.

A small piece of logic per slice called the **CBo** ("cache box" /
caching agent) is responsible for:

- Routing requests from a core to the right slice.
- Generating snoops to other cores when the protocol requires.
- Maintaining the directory state for lines homed in this slice.

Intel's protocol is called **MESIF** — MESI with an **F (Forward)**
state that nominates exactly one sharer to respond to read requests
when multiple have the line, avoiding redundant responses.

Intel exposes coherence behavior via knobs called **snoop modes**:

- *Source Snoop / Early Snoop* — close to pure snooping; the
  requester broadcasts and the home agent collects responses.
- *Home Snoop* — close to pure directory; the home agent decides who
  to snoop based on directory state.
- *COD (Cluster-on-Die) / SNC* — partition the chip into clusters;
  inside a cluster behave one way, across clusters another.

These are real-world embodiments of the hybrid spectrum. Different
workloads prefer different points on it.

### 4.2 AMD — CCX clusters and Infinity Fabric

AMD's Zen chips group cores into **CCX** (core complex) clusters of
4–8 cores. Each CCX has a private L3 acting as that cluster's LLS.
Coherence **inside** a CCX is fast (snoop-like across the cluster's
internal ring/mesh). Coherence **between** CCXes goes through
**Infinity Fabric**, AMD's chip interconnect, with directory-like
filtering. Protocol is a MOESI variant.

The pattern is the same: snoop locally, direct globally.

### 4.3 ARM — CoreLink CMN and DSU

ARM's server interconnect (CMN-600 / 700) is a **mesh** of routers
with a distributed system-level cache and a separate directory called
the **system-level snoop filter**. Cache clusters (DSU — DynamIQ
Shared Unit) handle a small group of cores with a shared L3 inside
the cluster.

### 4.4 What to take away

Every modern many-core chip uses some flavor of "fast nearby, scalable
far away". They differ in:
- Where the directory lives (separate, or in LLS).
- How clusters are sized.
- What protocol family (MESI / MOESI / MESIF).

Our project's design — L1-snoop + LLS-directory on a ring — is a
**plausible, well-known point** in this space. It's not a toy; it's
just the simplest version of what's out there.

---

## 5. Concepts worth bookmarking before we implement

- **Inclusion property.** If LLS is inclusive of L1, snoops can be
  *filtered* by the LLS itself — if the line isn't in the LLS, no L1
  has it, so don't bother snooping. Massive bandwidth win.
- **Snoop filter / sparse directory.** A separate small directory
  whose job is to answer "does anyone have this line?" without doing
  a full broadcast. The next step beyond plain hybrid.
- **False sharing.** Two cores write to *different* variables that
  happen to live in the same cache line. Coherence treats them as
  conflicting — the line ping-pongs. Not unique to LLS, but more
  visible in shared caches. Worth measuring.
- **Directory storage cost.** Roughly `#LLS_lines × (presence_bits +
  state_bits)`. With sharer bits = O(N) for N cores, this scales
  linearly. Above ~64 cores you switch to *limited-pointer* or
  *coarse-vector* directories.
- **Silent upgrades.** A core that has an Exclusive (E) copy can
  promote it to Modified (M) silently — no message, no traffic. MESI
  earns this; MSI doesn't. Counted in our existing
  `CoherenceStats::silent_upgrades`.
- **Intervention** (a.k.a. cache-to-cache transfer, our existing
  `c2c_transfers` counter). Owner supplies data instead of memory. In
  the LLS world, intervention happens between L1s via snoop;
  non-intervention misses go to LLS.

---

## 6. What this means for our simulator (high-level only)

> Implementation details belong in a Phase 6 design doc, not here.
> This section just sketches *what surfaces will need to change*, so
> the picture above isn't floating in the abstract.

- **L2 stops being per-core** in full mode. There's one shared LLS
  (eventually banked, but a single bank is fine v0).
- **The directory moves into the LLS.** The current
  `DirectoryController` becomes part of the LLS module rather than a
  standalone node on the ring.
- **A new "snoop layer" sits between L1s and the LLS.** L1 misses
  first try the snoop, then fall through to the LLS. The protocol
  state machines extend to handle "snoop hit at peer" as a distinct
  outcome.
- **Stats consolidate.** Today we report L2 hit/miss per core (4 numbers
  for a 4-core run). With a shared LLS, there's a single
  cache-system-wide hit rate — easier to compare across configs and
  the natural quantity to plot in sweeps.
- **Reuse, not rewrite.** The protocol *engines* (MSI / MESI / MOSI /
  MOESIF agents) are largely about state transitions on `GETS` /
  `GETM` / `INV` / `DATA` messages — not about who lives where. The
  refactor is mostly about *plumbing*: where the directory state
  table sits, who issues snoops, how the network distinguishes
  broadcast from unicast.

---

## 7. Suggested further reading

- Hennessy & Patterson, *Computer Architecture: A Quantitative
  Approach*, Ch. 5 (multiprocessors and memory consistency).
- Marty, M. R. — *Cache Coherence Techniques for Multicore
  Processors* (PhD thesis, Wisconsin, 2008): exhaustive treatment of
  hierarchical and hybrid coherence in CMPs.
- CMU 15-418 lecture on snooping-based coherence — the most
  approachable visual treatment.
- Intel's *Uncore Performance Monitoring Reference* — shows what the
  CBo / ring / LLC slice actually do in shipping silicon.

---

## Sources

- [A Novel Hybrid Cache Coherence with Global Snooping for Many-core Architectures (ACM TODAES)](https://dl.acm.org/doi/fullHtml/10.1145/3462775)
- [Cache Coherence I — University of Maryland (CMSC 411)](https://www.cs.umd.edu/~meesh/411/CA-online/chapter/cache-coherence-i/index.html)
- [Snooping-Based Cache Coherence — CMU 15-418/15-618](https://www.cs.cmu.edu/afs/cs/academic/class/15418-s21/www/lectures/11_cachecoherence1.pdf)
- [Lecture 18: Snooping vs. Directory Based Coherency — UC Berkeley](https://people.eecs.berkeley.edu/~pattrsn/252F96/Lecture18.pdf)
- [Practical Cache Coherence — Yizhou Shan](http://lastweek.io/notes/cache_coherence/)
- [NUMA Deep Dive Part 3: Cache Coherency](http://www.staroceans.org/cache_coherency.htm)
- [Cache Coherence Techniques for Multicore Processors — Marty PhD thesis (Wisconsin)](https://research.cs.wisc.edu/multifacet/theses/michael_marty_phd.pdf)
- [A hybrid NoC design for cache coherence optimization for chip multiprocessors](https://www.semanticscholar.org/paper/A-hybrid-NoC-design-for-cache-coherence-for-chip-Zhao-Jang/9e8c9be6693d5273bbe8f459505c0681d126629f)
- [Split Private and Shared L2 Cache Architecture for Snooping-based CMP — IEEE](https://ieeexplore.ieee.org/document/4276497/)
- [Cache Coherence — Wikipedia](https://en.wikipedia.org/wiki/Cache_coherence)
