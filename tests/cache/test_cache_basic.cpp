#include <catch2/catch_test_macros.hpp>

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
