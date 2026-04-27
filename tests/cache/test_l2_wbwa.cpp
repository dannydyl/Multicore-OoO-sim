#include <catch2/catch_test_macros.hpp>

#include "comparch/cache/cache.hpp"
#include "comparch/cache/main_memory.hpp"

using comparch::cache::Cache;
using comparch::cache::MainMemory;
using comparch::cache::MemReq;
using comparch::cache::Op;
using comparch::cache::Replacement;
using comparch::cache::WritePolicy;

namespace {

// Hierarchy with a clearly larger L2 than L1 so capacity isn't the variable
// under test. L1: 1 KB / 64 B / 2-way (8 sets). L2: 4 KB / 64 B / 4-way
// (16 sets). Project1 only had WTWNA at L2; this exercises the additive
// WBWA path.
struct WbwaHierarchy {
    MainMemory mem;
    Cache      l2;
    Cache      l1;

    static Cache::Config l1_cfg(Cache* l2_ptr) {
        Cache::Config c;
        c.c = 10; c.b = 6; c.s = 1;
        c.replacement  = Replacement::LRU_MIP;
        c.write_policy = WritePolicy::WBWA;
        c.hit_latency  = 2;
        c.next_level   = l2_ptr;
        return c;
    }
    static Cache::Config l2_cfg(MainMemory* mem_ptr) {
        Cache::Config c;
        c.c = 12; c.b = 6; c.s = 2; // 4 KB, 64 B blocks, 4-way -> 16 sets
        c.replacement  = Replacement::LRU_MIP;
        c.write_policy = WritePolicy::WBWA;
        c.hit_latency  = 10;
        c.main_memory  = mem_ptr;
        return c;
    }

    WbwaHierarchy()
        : mem(MainMemory::Config{100}),
          l2(l2_cfg(&mem), "L2"),
          l1(l1_cfg(&l2), "L1") {}
};

} // namespace

TEST_CASE("L2-WBWA: L1 writeback hits L2 and marks the line dirty",
          "[cache][l2_wbwa]") {
    WbwaHierarchy h;

    // Three same-L1-set addresses; L2 has plenty of capacity per set, so
    // these don't collide at L2.
    const std::uint64_t a = 0x0000ULL;
    const std::uint64_t b = 0x0200ULL;
    const std::uint64_t c = 0x0400ULL;

    h.l1.access({a, Op::Write});
    h.l1.access({b, Op::Read});
    h.l1.access({c, Op::Read}); // evicts a (dirty); L2 still holds a -> hit

    REQUIRE(h.l1.stats().writebacks == 1);
    REQUIRE(h.l2.stats().writes     == 1);
    REQUIRE(h.l2.stats().hits       >= 1); // the writeback hit
}

TEST_CASE("L2-WBWA: dirty eviction at L2 reaches DRAM as a write",
          "[cache][l2_wbwa]") {
    WbwaHierarchy h;

    // L2 is 4-way per set; pick 5 addresses that all hash to L2 set 0 to
    // overflow it. addr_step = (block_size << index_bit) = 64 << 4 = 1024.
    const std::uint64_t step = 1024;
    const std::uint64_t base = 0x10000ULL;
    const std::uint64_t addrs[5] = {
        base + 0 * step,
        base + 1 * step,
        base + 2 * step,
        base + 3 * step,
        base + 4 * step,
    };

    // Sanity: all five share the same L2 set.
    for (int i = 1; i < 5; ++i) {
        REQUIRE(h.l2.get_index(h.l2.get_block_addr(addrs[0])) ==
                h.l2.get_index(h.l2.get_block_addr(addrs[i])));
    }

    // Drive five distinct L1 dirty evictions to the same L2 set. The 5th
    // forces an L2 dirty eviction (since L2 set holds 4) -> DRAM write.
    auto dirty_then_evict = [&](std::uint64_t addr) {
        h.l1.access({addr, Op::Write});
        h.l1.access({addr + 0x0001'0000ULL, Op::Read});
        h.l1.access({addr + 0x0002'0000ULL, Op::Read});
    };
    for (auto a : addrs) dirty_then_evict(a);

    REQUIRE(h.l1.stats().writebacks >= 5);
    REQUIRE(h.l2.stats().writes     >= 5);
    REQUIRE(h.l2.stats().writebacks >= 1);
    REQUIRE(h.mem.stats().writes    >= 1);
}

TEST_CASE("L2-WBWA accounting: hits/misses tracked, read_hits/read_misses zero",
          "[cache][l2_wbwa]") {
    WbwaHierarchy h;

    h.l1.access({0x1000, Op::Read});      // L1 miss -> L2 miss
    REQUIRE(h.l2.stats().misses      == 1);
    REQUIRE(h.l2.stats().read_misses == 0); // WBWA does not touch read_*
    REQUIRE(h.l2.stats().read_hits   == 0);

    h.l1.access({0x1000, Op::Read});      // L1 hit; no L2 traffic
    REQUIRE(h.l2.stats().accesses    == 1);

    // Force an L1 eviction so the next L1 read misses again. L2 retains the
    // line because it has plenty of capacity (4-way, distinct sets).
    const std::uint64_t b = 0x1000ULL + 0x200ULL;
    const std::uint64_t c = 0x1000ULL + 0x400ULL;
    h.l1.access({b, Op::Read});
    h.l1.access({c, Op::Read});
    h.l1.access({0x1000, Op::Read});      // L1 miss -> L2 hit

    REQUIRE(h.l2.stats().hits        >= 1);
    REQUIRE(h.l2.stats().read_hits   == 0); // still zero in WBWA mode
}
