#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "comparch/cache/cache.hpp"
#include "comparch/cache/main_memory.hpp"
#include "comparch/cache/prefetcher_markov.hpp"

using comparch::cache::Cache;
using comparch::cache::MainMemory;
using comparch::cache::MarkovPrefetcher;
using comparch::cache::MemReq;
using comparch::cache::Op;
using comparch::cache::Replacement;
using comparch::cache::WritePolicy;

namespace {

// A standalone L2-style cache (no peer, plenty of capacity) that we drive
// directly. Lets us isolate the prefetcher's decision logic from cache
// eviction dynamics.
struct Standalone {
    MainMemory mem;
    Cache      l2;

    static Cache::Config l2_cfg(MainMemory* mem_ptr) {
        Cache::Config c;
        c.c = 14; c.b = 6; c.s = 3; // 16 KB, 64 B, 8-way -> 32 sets
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

TEST_CASE("Markov: trained A->B prefetches B when A misses again",
          "[cache][prefetch][markov]") {
    Standalone s;
    MarkovPrefetcher mkv(/*max_rows=*/16);

    const std::uint64_t a = 0x0000ULL;
    const std::uint64_t b = 0x4000ULL;

    // First miss seeds prev (no prefetch, no learning).
    mkv.on_miss(s.l2, /*peer=*/nullptr, MemReq{a, Op::Read});

    // Second miss: prev=a, missed=b -> learn row{source=a, dest=b}.
    mkv.on_miss(s.l2, /*peer=*/nullptr, MemReq{b, Op::Read});
    REQUIRE(s.l2.stats().prefetches_issued == 0); // no prediction yet for b

    // Now revisit a as a fresh miss. Table predicts b. b is not in the
    // standalone cache (we never serviced a real demand for b through
    // l2.access), so the prefetcher should issue.
    mkv.on_miss(s.l2, /*peer=*/nullptr, MemReq{a, Op::Read});
    REQUIRE(s.l2.stats().prefetches_issued == 1);
}

TEST_CASE("Markov: no prediction when source has no row",
          "[cache][prefetch][markov]") {
    Standalone s;
    MarkovPrefetcher mkv(/*max_rows=*/16);

    const std::uint64_t a = 0x0000ULL;
    const std::uint64_t b = 0x4000ULL;
    const std::uint64_t c = 0x8000ULL;

    mkv.on_miss(s.l2, nullptr, MemReq{a, Op::Read});
    mkv.on_miss(s.l2, nullptr, MemReq{b, Op::Read});
    REQUIRE(s.l2.stats().prefetches_issued == 0);

    // c has no row in the table -> no prefetch issued. Just a learn step.
    mkv.on_miss(s.l2, nullptr, MemReq{c, Op::Read});
    REQUIRE(s.l2.stats().prefetches_issued == 0);
}

TEST_CASE("Markov: skip prefetch when MFU target equals the missed block",
          "[cache][prefetch][markov]") {
    Standalone s;
    MarkovPrefetcher mkv(/*max_rows=*/16);

    const std::uint64_t a = 0x0000ULL;

    // Self-loop A -> A. Train it.
    mkv.on_miss(s.l2, nullptr, MemReq{a, Op::Read});
    mkv.on_miss(s.l2, nullptr, MemReq{a, Op::Read});

    // Now miss A again: table says A's MFU is A, but the prefetcher
    // explicitly skips MFU == missed.
    mkv.on_miss(s.l2, nullptr, MemReq{a, Op::Read});
    REQUIRE(s.l2.stats().prefetches_issued == 0);
}

TEST_CASE("Markov with max_rows=0 issues no prefetches",
          "[cache][prefetch][markov]") {
    Standalone s;
    MarkovPrefetcher mkv(/*max_rows=*/0);

    for (int i = 0; i < 64; ++i) {
        mkv.on_miss(s.l2, nullptr,
                    MemReq{0x4000ULL * static_cast<std::uint64_t>(i),
                           Op::Read});
    }
    REQUIRE(s.l2.stats().prefetches_issued == 0);
}
