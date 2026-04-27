#include <catch2/catch_test_macros.hpp>

#include "comparch/cache/cache.hpp"

using comparch::cache::Cache;
using comparch::cache::Op;
using comparch::cache::Replacement;
using comparch::cache::WritePolicy;

namespace {

// 2-way, 8 sets, 64 B blocks. All test addresses below land in index 0.
Cache::Config make(Replacement r) {
    Cache::Config c;
    c.c = 10;
    c.b = 6;
    c.s = 1;
    c.replacement  = r;
    c.write_policy = WritePolicy::WBWA;
    c.hit_latency  = 2;
    return c;
}

constexpr std::uint64_t A = 0x0000ULL;        // block_addr = 0
constexpr std::uint64_t B = 0x0200ULL;        // block_addr = 8
constexpr std::uint64_t C = 0x0400ULL;        // block_addr = 16
constexpr std::uint64_t D = 0x0600ULL;        // block_addr = 24

} // namespace

TEST_CASE("MIP evicts the first-inserted block after thrashing the set",
          "[cache][replacement]") {
    Cache l1(make(Replacement::LRU_MIP), "L1");

    REQUIRE_FALSE(l1.access({A, Op::Read}).hit);
    REQUIRE_FALSE(l1.access({B, Op::Read}).hit);
    REQUIRE_FALSE(l1.access({C, Op::Read}).hit); // evicts A under MIP
    REQUIRE_FALSE(l1.access({D, Op::Read}).hit); // evicts B under MIP

    REQUIRE_FALSE(l1.access({A, Op::Read}).hit);
    REQUIRE_FALSE(l1.access({B, Op::Read}).hit);
}

TEST_CASE("LIP retains the original MRU block under thrashing",
          "[cache][replacement]") {
    Cache l1(make(Replacement::LRU_LIP), "L1");

    REQUIRE_FALSE(l1.access({A, Op::Read}).hit);
    REQUIRE_FALSE(l1.access({B, Op::Read}).hit);
    REQUIRE_FALSE(l1.access({C, Op::Read}).hit); // LIP: evicts B (LRU=back)
    REQUIRE_FALSE(l1.access({D, Op::Read}).hit); // LIP: evicts C (LRU=back)

    // Under LIP, A survives because it stayed at the front and never moved.
    REQUIRE(l1.access({A, Op::Read}).hit);
    // D is the most-recent insert; under LIP it sits at the back (LRU)
    // but is still in-cache.
    REQUIRE(l1.access({D, Op::Read}).hit);
    // B was evicted on the third miss.
    REQUIRE_FALSE(l1.access({B, Op::Read}).hit);
}

TEST_CASE("LIP-on-hit still promotes to MRU (splice-to-front)",
          "[cache][replacement]") {
    Cache l1(make(Replacement::LRU_LIP), "L1");

    // Fill the set: insertions go to LRU under LIP.
    l1.access({A, Op::Read}); // LIP: [A]
    l1.access({B, Op::Read}); // LIP: [A, B]

    // Touching B promotes it to MRU. Order becomes [B, A].
    REQUIRE(l1.access({B, Op::Read}).hit);

    // Now insert C: evicts the LRU, which is now A.
    REQUIRE_FALSE(l1.access({C, Op::Read}).hit);

    REQUIRE(l1.access({B, Op::Read}).hit);     // B still resident
    REQUIRE_FALSE(l1.access({A, Op::Read}).hit); // A was evicted
}
