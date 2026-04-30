#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

#include "comparch/cache/cache.hpp"

using comparch::cache::Cache;
using comparch::cache::MemReq;
using comparch::cache::Op;
using comparch::cache::Replacement;
using comparch::cache::WritePolicy;

namespace {

Cache::Config small_l1() {
    Cache::Config c;
    c.c = 10;                         // 1 KB capacity
    c.b = 6;                          // 64 B blocks
    c.s = 1;                          // 2-way
    c.replacement  = Replacement::LRU_MIP;
    c.write_policy = WritePolicy::WBWA;
    c.hit_latency  = 2;
    return c;
}

} // namespace

TEST_CASE("L1 cold miss then hit on the same block", "[cache]") {
    Cache l1(small_l1(), "L1");

    auto m1 = l1.access({0x1000, Op::Read});
    REQUIRE(!m1.hit);
    REQUIRE(l1.stats().accesses == 1);
    REQUIRE(l1.stats().misses   == 1);

    auto m2 = l1.access({0x1000, Op::Read});
    REQUIRE(m2.hit);
    REQUIRE(l1.stats().accesses == 2);
    REQUIRE(l1.stats().hits     == 1);
    REQUIRE(l1.stats().misses   == 1);
}

TEST_CASE("L1 distinguishes blocks within the same set", "[cache]") {
    // 2-way, 8 sets => index from bits [6:9), block-aligned addresses with
    // identical low 9 bits but different tag bits map to the same set.
    Cache l1(small_l1(), "L1");

    const std::uint64_t a = 0x0000'2000ULL;        // index = (0x2000>>6) & 7 = 0
    const std::uint64_t b = 0x0000'2000ULL + 0x200ULL; // index = (0x2200>>6)&7 = 0
    const std::uint64_t c = 0x0000'2000ULL + 0x400ULL; // index = 0 too

    REQUIRE(l1.get_index(l1.get_block_addr(a)) ==
            l1.get_index(l1.get_block_addr(b)));
    REQUIRE(l1.get_index(l1.get_block_addr(b)) ==
            l1.get_index(l1.get_block_addr(c)));

    l1.access({a, Op::Read});
    l1.access({b, Op::Read});
    REQUIRE(l1.stats().misses == 2);
    REQUIRE(l1.access({a, Op::Read}).hit);
    REQUIRE(l1.access({b, Op::Read}).hit);
    REQUIRE(l1.stats().hits == 2);

    // Bringing in c evicts a (LRU) since the set is full.
    l1.access({c, Op::Read});
    REQUIRE(l1.stats().misses == 3);
    auto fa = l1.access({a, Op::Read});
    REQUIRE(!fa.hit);
    REQUIRE(l1.stats().misses == 4);
}

TEST_CASE("WBWA: write hit makes the block dirty; eviction triggers writeback",
          "[cache][wbwa]") {
    Cache l1(small_l1(), "L1");

    const std::uint64_t a = 0x0000'2000ULL;
    const std::uint64_t b = 0x0000'2000ULL + 0x200ULL;
    const std::uint64_t c = 0x0000'2000ULL + 0x400ULL;

    l1.access({a, Op::Write}); // miss + dirty insert
    l1.access({b, Op::Read});  // miss + clean insert; set now full
    REQUIRE(l1.stats().writes == 1);
    REQUIRE(l1.stats().reads  == 1);
    REQUIRE(l1.stats().misses == 2);
    REQUIRE(l1.stats().writebacks == 0);

    // Evict a (dirty) — writeback expected.
    l1.access({c, Op::Read});
    REQUIRE(l1.stats().misses     == 3);
    REQUIRE(l1.stats().writebacks == 1);
}

TEST_CASE("WBWA: clean eviction does not count as writeback",
          "[cache][wbwa]") {
    Cache l1(small_l1(), "L1");

    const std::uint64_t a = 0x0000'2000ULL;
    const std::uint64_t b = 0x0000'2000ULL + 0x200ULL;
    const std::uint64_t c = 0x0000'2000ULL + 0x400ULL;

    l1.access({a, Op::Read});
    l1.access({b, Op::Read});
    l1.access({c, Op::Read}); // evicts a (clean)

    REQUIRE(l1.stats().misses     == 3);
    REQUIRE(l1.stats().writebacks == 0);
}

TEST_CASE("Geometry helpers match project1's tag/index split",
          "[cache][geometry]") {
    Cache::Config c;
    c.c = 10; c.b = 6; c.s = 1; // 8 sets, 2 ways, 64 B blocks
    Cache l1(std::move(c), "L1");

    // index_bit = c - b - s = 3 -> 8 sets.
    // block_addr = addr >> 6.
    // index = block_addr & 0x7
    // tag   = block_addr >> 3
    const std::uint64_t addr = 0xDEAD'BEEFULL;
    const auto block_addr = l1.get_block_addr(addr);
    const auto idx        = l1.get_index(block_addr);
    const auto tag        = l1.get_tag(block_addr);

    REQUIRE(block_addr == (addr >> 6));
    REQUIRE(idx        == (block_addr & 0x7ULL));
    REQUIRE(tag        == (block_addr >> 3));
}

// ---------------------------------------------------------------------------
// Async / MSHR-aware path tests. Cover what the OoO LSU will do:
//   1. issue() returns an id, peek(id) reflects ready==false until enough
//      ticks have elapsed.
//   2. Two issues to the same in-flight block merge onto one slot.
//   3. MSHR-full causes issue() to return nullopt.
// ---------------------------------------------------------------------------

TEST_CASE("Cache::issue / tick / peek / complete drive a single miss", "[cache][mshr]") {
    auto cc = small_l1();
    cc.hit_latency  = 2;
    cc.mshr_entries = 4;
    Cache l1(std::move(cc), "L1");

    // First touch on 0x1000 misses; with no next_level, downstream latency
    // is 0, so total latency == hit_latency == 2.
    auto id = l1.issue({0x1000, Op::Read});
    REQUIRE(id.has_value());

    const auto* e = l1.peek(*id);
    REQUIRE(e != nullptr);
    REQUIRE_FALSE(e->ready);
    REQUIRE(e->due_cycle == 2);

    l1.tick();                                     // now_ = 1
    REQUIRE_FALSE(l1.peek(*id)->ready);
    l1.tick();                                     // now_ = 2; ready flips
    REQUIRE(l1.peek(*id)->ready);
    REQUIRE(l1.peek(*id)->result.hit == false);    // cold miss

    l1.complete(*id);
    REQUIRE(l1.peek(*id) == nullptr);
}

TEST_CASE("Cache::issue merges concurrent requests to the same block",
          "[cache][mshr][merge]") {
    auto cc = small_l1();
    cc.hit_latency  = 2;
    cc.mshr_entries = 4;
    Cache l1(std::move(cc), "L1");

    auto id1 = l1.issue({0x1000, Op::Read});
    auto id2 = l1.issue({0x1000, Op::Read});  // same block -> merge
    REQUIRE(id1.has_value());
    REQUIRE(id2.has_value());
    REQUIRE(*id1 != *id2);

    // Only one MSHR slot occupied, both ids point at it.
    REQUIRE(l1.mshr().occupancy() == 1);
    REQUIRE(l1.peek(*id1) == l1.peek(*id2));

    // Stats show only ONE miss — the secondary is not a separate access.
    REQUIRE(l1.stats().accesses == 1);
    REQUIRE(l1.stats().misses   == 1);
}

TEST_CASE("Cache::issue returns nullopt when MSHR is full",
          "[cache][mshr][stall]") {
    auto cc = small_l1();
    cc.hit_latency  = 100;   // misses linger long enough to fill the table
    cc.mshr_entries = 2;
    Cache l1(std::move(cc), "L1");

    auto a = l1.issue({0x1000, Op::Read});
    auto b = l1.issue({0x2000, Op::Read});   // different blocks -> two slots
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(l1.mshr().full());

    // Third request to a third block has nowhere to go.
    auto c = l1.issue({0x3000, Op::Read});
    REQUIRE_FALSE(c.has_value());

    // Releasing one slot opens room again.
    l1.complete(*a);
    auto d = l1.issue({0x3000, Op::Read});
    REQUIRE(d.has_value());
}

TEST_CASE("Cache::issue does not mutate cache state when MSHR is full",
          "[cache][mshr][stall]") {
    // Regression for the side-effect-leak bug: previously issue() called
    // access() before checking MSHR capacity, so a request that ultimately
    // returned nullopt had still shifted LRU, fired prefetchers, and bumped
    // hit/miss stats. Now the capacity check runs first.
    auto cc = small_l1();
    cc.hit_latency  = 100;
    cc.mshr_entries = 1;
    Cache l1(std::move(cc), "L1");

    auto a = l1.issue({0x1000, Op::Read});       // fills the only slot
    REQUIRE(a.has_value());
    REQUIRE(l1.mshr().full());

    const auto stats_before = l1.stats();

    auto b = l1.issue({0x2000, Op::Read});       // must stall
    REQUIRE_FALSE(b.has_value());

    // No new access registered, no LRU mutation, block 0x2000 not resident.
    REQUIRE(l1.stats().accesses == stats_before.accesses);
    REQUIRE(l1.stats().misses   == stats_before.misses);
    REQUIRE_FALSE(l1.block_in(0x2000));
}

TEST_CASE("Cache::issue accepts Write but never merges them",
          "[cache][mshr][precondition]") {
    // Phase 5B: writes go through the async issue/peek/complete path
    // (the OoO core's stage_schedule LSU section uses it for both
    // loads and stores under coherence). The merge fast-path is still
    // disabled for writes — a Write piggybacking on a Read primary
    // would inherit the primary's clean AccessResult and skip the
    // dirty-bit mutation. Verify writes always allocate fresh slots.
    Cache l1(small_l1(), "L1");
    auto a = l1.issue({0x1000, Op::Read});
    auto b = l1.issue({0x1000, Op::Write});
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(*a != *b);
}

TEST_CASE("Cache::access yields the same hit/miss as Cache::issue + peek",
          "[cache][mshr][equiv]") {
    // Two caches with identical config; one drives via access(), the other
    // through issue/tick/peek/complete. Hit/miss ordering must match.
    auto cc1 = small_l1();
    auto cc2 = small_l1();
    Cache l1a(std::move(cc1), "L1a");
    Cache l1b(std::move(cc2), "L1b");

    const std::uint64_t addrs[] = {0x1000, 0x1000, 0x2000, 0x1000, 0x3000};
    for (auto a : addrs) {
        const auto sync = l1a.access({a, Op::Read});
        auto id = l1b.issue({a, Op::Read});
        REQUIRE(id.has_value());
        // Spin tick until ready (mimics what the OoO core's poll would do).
        while (!l1b.peek(*id)->ready) l1b.tick();
        const auto async_e = l1b.peek(*id);
        REQUIRE(async_e->result.hit     == sync.hit);
        REQUIRE(async_e->result.latency == sync.latency);
        l1b.complete(*id);
    }

    REQUIRE(l1a.stats().hits    == l1b.stats().hits);
    REQUIRE(l1a.stats().misses  == l1b.stats().misses);
    REQUIRE(l1a.stats().accesses == l1b.stats().accesses);
}
