#include <catch2/catch_test_macros.hpp>

#include "comparch/cache/cache.hpp"
#include "comparch/cache/main_memory.hpp"
#include "comparch/cache/prefetcher_hybrid.hpp"

using comparch::cache::Cache;
using comparch::cache::HybridPrefetcher;
using comparch::cache::MainMemory;
using comparch::cache::MemReq;
using comparch::cache::Op;
using comparch::cache::Replacement;
using comparch::cache::WritePolicy;

namespace {

struct Standalone {
    MainMemory mem;
    Cache      l2;

    static Cache::Config l2_cfg(MainMemory* mem_ptr) {
        Cache::Config c;
        c.c = 14; c.b = 6; c.s = 3; // 16 KB / 8-way / 32 sets
        c.replacement  = Replacement::LRU_MIP;
        c.write_policy = WritePolicy::WTWNA;
        c.hit_latency  = 10;
        c.main_memory  = mem_ptr;
        return c;
    }

    Standalone()
        : mem(MainMemory::Config{100}),
          l2(l2_cfg(&mem), "L2") {}
};

} // namespace

TEST_CASE("Hybrid: max_rows=0 reduces to +1 prefetcher",
          "[cache][prefetch][hybrid]") {
    Standalone s;
    HybridPrefetcher hyb(/*max_rows=*/0);

    hyb.on_miss(s.l2, nullptr, MemReq{0x0000ULL, Op::Read});
    REQUIRE(s.l2.stats().prefetches_issued == 1); // +1 for block 0 -> 1
}

TEST_CASE("Hybrid: novel source falls back to +1",
          "[cache][prefetch][hybrid]") {
    Standalone s;
    HybridPrefetcher hyb(/*max_rows=*/16);

    // No row exists for any source on the very first call -> +1 fallback.
    hyb.on_miss(s.l2, nullptr, MemReq{0x0000ULL, Op::Read});
    REQUIRE(s.l2.stats().prefetches_issued >= 1);
}

TEST_CASE("Hybrid: when a Markov prediction fires, +1 fallback is skipped",
          "[cache][prefetch][hybrid]") {
    Standalone s;
    HybridPrefetcher hyb(/*max_rows=*/16);

    const std::uint64_t a = 0x0000ULL; // block 0
    const std::uint64_t b = 0x4000ULL; // block 256

    // Train: pf_first(a) -> learn skipped on first; then b -> learns a->b.
    hyb.on_miss(s.l2, nullptr, MemReq{a, Op::Read});
    hyb.on_miss(s.l2, nullptr, MemReq{b, Op::Read});

    // Reset the issued counter by checking it now and comparing later.
    const auto before = s.l2.stats().prefetches_issued;

    // Third call (a): row source=a exists with prediction b. Neither b
    // (block 256) nor block 1 (the +1 successor of a) is resident, so the
    // hybrid's Markov branch issues exactly one prefetch (b) and the +1
    // fallback is suppressed.
    hyb.on_miss(s.l2, nullptr, MemReq{a, Op::Read});
    const auto delta = s.l2.stats().prefetches_issued - before;

    REQUIRE(delta == 1);             // exactly one prefetch, not two
    REQUIRE(s.l2.block_in(b));       // Markov target b was prefetched
}
