#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "comparch/cache/cache.hpp"
#include "comparch/cache/main_memory.hpp"
#include "comparch/cache/prefetcher_plus_one.hpp"

using comparch::cache::Cache;
using comparch::cache::MainMemory;
using comparch::cache::MemReq;
using comparch::cache::Op;
using comparch::cache::PlusOnePrefetcher;
using comparch::cache::Replacement;
using comparch::cache::WritePolicy;

namespace {

// Project1-style hierarchy with WTWNA L2 + +1 prefetcher attached to L2.
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
        c.c = 13; c.b = 6; c.s = 2; // 8 KB / 4-way / 32 sets
        c.replacement  = Replacement::LRU_LIP;
        c.write_policy = WritePolicy::WTWNA;
        c.hit_latency  = 10;
        c.main_memory  = mem_ptr;
        c.prefetcher   = std::make_unique<PlusOnePrefetcher>();
        return c;
    }

    Hierarchy()
        : mem(MainMemory::Config{100}),
          l2(l2_cfg(&mem), "L2"),
          l1(l1_cfg(&l2), "L1") {
        l2.set_peer_above(&l1);
    }
};

} // namespace

TEST_CASE("+1 prefetch issues block+1 on a cold L2 miss",
          "[cache][prefetch][plus_one]") {
    Hierarchy h;

    // Cold miss on block 0 (addr 0x0000) -> +1 should prefetch block 1.
    h.l1.access({0x0000ULL, Op::Read});
    REQUIRE(h.l2.stats().prefetches_issued == 1);
}

TEST_CASE("+1 prefetched block becomes a demand hit later",
          "[cache][prefetch][plus_one]") {
    Hierarchy h;

    h.l1.access({0x0000ULL, Op::Read});             // demand miss -> prefetch block 1
    REQUIRE(h.l2.stats().prefetches_issued == 1);

    // Force the block-1 line out of L1 if it ever got there (it didn't —
    // only L2 received the prefetch). Then access block 1: L1 cold miss,
    // L2 hit on the prefetched block -> prefetch_hits++.
    h.l1.access({0x0040ULL, Op::Read});             // address of block 1
    REQUIRE(h.l2.stats().read_hits      >= 1);
    REQUIRE(h.l2.stats().prefetch_hits  == 1);
}

TEST_CASE("+1 does not prefetch a block already resident",
          "[cache][prefetch][plus_one]") {
    Hierarchy h;

    // Bring block 1 into L2 first via a normal demand miss.
    h.l1.access({0x0040ULL, Op::Read});  // demand block 1; +1 -> block 2
    const auto issued_after_first = h.l2.stats().prefetches_issued;
    REQUIRE(issued_after_first == 1);

    // Now demand block 0. +1 would target block 1 — already resident in
    // both L1 and L2 — so no new prefetch.
    h.l1.access({0x0000ULL, Op::Read});
    REQUIRE(h.l2.stats().prefetches_issued == issued_after_first);
}
