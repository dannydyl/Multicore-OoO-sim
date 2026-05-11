// LlsCache unit tests: hit/miss/LRU/eviction at the class level.
// Directory-side integration (LLS consulted by schedule_data_response)
// is exercised by the scenario tests in tests/coherence/.

#include <catch2/catch_test_macros.hpp>

#include "comparch/coherence/lls_cache.hpp"

using comparch::coherence::BlockId;
using comparch::coherence::LlsCache;
using comparch::coherence::LlsLookup;

TEST_CASE("LlsCache: cold access misses, no eviction") {
    LlsCache c{/*size_blocks=*/8, /*assoc=*/2};   // 4 sets x 2 ways
    auto r = c.access(BlockId{42});
    REQUIRE_FALSE(r.hit);
    REQUIRE_FALSE(r.evicted);
}

TEST_CASE("LlsCache: second access hits") {
    LlsCache c{8, 2};
    c.access(42);
    auto r = c.access(42);
    REQUIRE(r.hit);
    REQUIRE_FALSE(r.evicted);
}

TEST_CASE("LlsCache: same set, beyond assoc, evicts LRU") {
    // num_sets = 4. Blocks 0, 4, 8, 12 all map to set 0.
    LlsCache c{8, 2};
    c.access(0);   // set 0 LRU = [0]
    c.access(4);   // set 0 LRU = [4, 0]
    auto r = c.access(8);   // forces eviction of 0
    REQUIRE_FALSE(r.hit);
    REQUIRE(r.evicted);
    REQUIRE(r.victim == 0);
    REQUIRE_FALSE(c.contains(0));
    REQUIRE(c.contains(4));
    REQUIRE(c.contains(8));
}

TEST_CASE("LlsCache: hit promotes to MRU") {
    LlsCache c{8, 2};
    c.access(0);
    c.access(4);   // LRU = [4, 0]
    c.access(0);   // touch 0 -> LRU = [0, 4]
    auto r = c.access(8);   // should evict 4, not 0
    REQUIRE(r.evicted);
    REQUIRE(r.victim == 4);
    REQUIRE(c.contains(0));
    REQUIRE_FALSE(c.contains(4));
}

TEST_CASE("LlsCache: contains() does not perturb LRU") {
    LlsCache c{8, 2};
    c.access(0);
    c.access(4);   // LRU = [4, 0]
    (void)c.contains(0);   // must not promote 0
    auto r = c.access(8);
    REQUIRE(r.evicted);
    REQUIRE(r.victim == 0);   // 0 is still the LRU end
}

TEST_CASE("LlsCache: invalidate removes block, no-op on miss") {
    LlsCache c{8, 2};
    c.access(0);
    c.invalidate(0);
    REQUIRE_FALSE(c.contains(0));
    c.invalidate(99);   // not present; should not throw
    REQUIRE_FALSE(c.contains(99));
}

TEST_CASE("LlsCache: disabled cache (size=0) is a permanent miss") {
    LlsCache c{0, 0};
    auto r = c.access(7);
    REQUIRE_FALSE(r.hit);
    REQUIRE_FALSE(r.evicted);
    REQUIRE_FALSE(c.contains(7));
}

TEST_CASE("LlsCache: rejects size_blocks not divisible by assoc") {
    REQUIRE_THROWS(LlsCache{/*size_blocks=*/9, /*assoc=*/2});
}

TEST_CASE("LlsCache: independent sets do not interfere") {
    LlsCache c{4, 2};   // 2 sets x 2 ways. set(0)={0,2}, set(1)={1,3}
    c.access(0);
    c.access(2);   // set 0 full
    c.access(1);   // set 1 cold miss
    c.access(3);   // set 1 fills
    // Set 0 should still hold 0 and 2; set 1 holds 1 and 3.
    REQUIRE(c.contains(0));
    REQUIRE(c.contains(2));
    REQUIRE(c.contains(1));
    REQUIRE(c.contains(3));
    // Now another set-0 access should evict from set 0, not set 1.
    auto r = c.access(4);
    REQUIRE(r.evicted);
    // 4 maps to set 0; victim must be a set-0 member (0 or 2).
    REQUIRE((r.victim == 0 || r.victim == 2));
}
