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

struct Hierarchy {
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
        c.c = 13; c.b = 6; c.s = 2; // 8 KB, 64 B blocks, 4-way -> 32 sets
        c.replacement  = Replacement::LRU_LIP;
        c.write_policy = WritePolicy::WTWNA;
        c.hit_latency  = 10;
        c.main_memory  = mem_ptr;
        return c;
    }

    Hierarchy()
        : mem(MainMemory::Config{100}),
          l2(l2_cfg(&mem), "L2"),
          l1(l1_cfg(&l2), "L1") {}
};

} // namespace

TEST_CASE("L1 read miss issues exactly one L2 read", "[cache][hierarchy]") {
    Hierarchy h;
    h.l1.access({0x1000, Op::Read});
    REQUIRE(h.l1.stats().misses == 1);
    REQUIRE(h.l2.stats().reads  == 1);
    REQUIRE(h.l2.stats().read_misses == 1);

    // Repeat: L1 hit; no new L2 access.
    h.l1.access({0x1000, Op::Read});
    REQUIRE(h.l1.stats().hits   == 1);
    REQUIRE(h.l2.stats().reads  == 1);
}

TEST_CASE("L1 write miss issues an L2 read (write-allocate)",
          "[cache][hierarchy]") {
    Hierarchy h;
    h.l1.access({0x1000, Op::Write});
    REQUIRE(h.l1.stats().misses     == 1);
    REQUIRE(h.l1.stats().writes     == 1);
    REQUIRE(h.l2.stats().reads      == 1); // fill is a read, not a write
    REQUIRE(h.l2.stats().writes     == 0);
}

TEST_CASE("L1 dirty eviction issues an L2 write (writeback)",
          "[cache][hierarchy]") {
    Hierarchy h;

    // Three same-set addresses for a 2-way L1 (assoc=2, 8 sets).
    const std::uint64_t a = 0x0000ULL;
    const std::uint64_t b = 0x0200ULL;
    const std::uint64_t c = 0x0400ULL;

    h.l1.access({a, Op::Write}); // miss + dirty insert
    h.l1.access({b, Op::Read});  // miss + clean insert
    h.l1.access({c, Op::Read});  // miss; evicts a (dirty) -> writeback to L2

    REQUIRE(h.l1.stats().writebacks == 1);
    REQUIRE(h.l2.stats().writes     == 1);
    REQUIRE(h.l2.stats().reads      == 3); // three L1 misses each read L2
}

TEST_CASE("L2 WTWNA write hit: splice-to-MRU only, no dirty update",
          "[cache][hierarchy][wtwna]") {
    Hierarchy h;

    // Force a block into L2 first via an L1 miss.
    h.l1.access({0x1000, Op::Read});
    REQUIRE(h.l2.stats().read_misses == 1);

    // Now issue a writeback at the same block (simulate via direct call).
    h.l2.access({0x1000, Op::Write});
    REQUIRE(h.l2.stats().writes      == 1);
    // Project1 doesn't classify L2 writes as hits/misses -> no change to
    // read_hits/read_misses.
    REQUIRE(h.l2.stats().read_misses == 1);
    REQUIRE(h.l2.stats().read_hits   == 0);

    // The block is still resident; a follow-up L2 read should hit.
    h.l2.access({0x1000, Op::Read});
    REQUIRE(h.l2.stats().read_hits   == 1);
}

TEST_CASE("L2 WTWNA write miss: do not allocate",
          "[cache][hierarchy][wtwna]") {
    Hierarchy h;

    h.l2.access({0xCAFE'1000ULL, Op::Write});
    REQUIRE(h.l2.stats().writes == 1);

    // Block was NOT allocated; subsequent read is still a miss.
    h.l2.access({0xCAFE'1000ULL, Op::Read});
    REQUIRE(h.l2.stats().read_misses == 1);
    REQUIRE(h.l2.stats().read_hits   == 0);
}

TEST_CASE("Main memory backs L2 misses", "[cache][hierarchy]") {
    Hierarchy h;
    h.l1.access({0x1000, Op::Read});
    h.l1.access({0x2000, Op::Read});
    REQUIRE(h.mem.stats().accesses == 2); // each L2 miss hits main memory once
}
