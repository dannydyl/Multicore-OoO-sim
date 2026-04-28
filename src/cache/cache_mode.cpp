// cache_mode
// ==========
// Driver for `comparch-sim --mode cache`. Builds an L1 / L2 / DRAM
// hierarchy from the JSON config, walks a ChampSim trace as a stream
// of memory accesses, and prints per-level stats at the end.
//
// This mode is intentionally narrow: it ignores PCs, branches, register
// IDs, and core counts. Just memory addresses through the cache. Used
// to validate the cache port against project1 and to explore how
// geometry / replacement / prefetcher choices change miss ratios.

#include "comparch/cache/cache_mode.hpp"

#include <bit>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

#include "comparch/cache/prefetcher.hpp"
#include "comparch/cache/prefetcher_hybrid.hpp"
#include "comparch/cache/prefetcher_markov.hpp"
#include "comparch/cache/prefetcher_plus_one.hpp"
#include "comparch/log.hpp"
#include "comparch/trace.hpp"

namespace comparch::cache {

namespace {

// Convert a positive power-of-two integer into its log2 (e.g. 64 -> 6,
// 32768 -> 15). The Cache class works in log2 (C, B, S) form because that's
// what the geometry math (tag/index splitting via shifts) actually needs.
//
// Validation:
//   - value > 0
//   - exactly one bit set (popcount == 1)
// Throws ConfigError otherwise so the caller can surface a useful message.
unsigned log2_pow2(int value, const char* what) {
    if (value <= 0 || std::popcount(static_cast<unsigned>(value)) != 1) {
        std::ostringstream oss;
        oss << what << " must be a positive power of two (got " << value << ")";
        throw ConfigError(oss.str());
    }
    return static_cast<unsigned>(std::countr_zero(static_cast<unsigned>(value)));
}

// Format one cache level's stats for the user. The `wbwa_style` flag picks
// between project1-L1-style accounting (hits / misses / writebacks) and
// project1-L2-style accounting (read_hits / read_misses), matching the
// underlying Cache::access bookkeeping.
void print_level(std::ostream& os, const std::string& name,
                 const CacheStats& s, bool wbwa_style) {
    // Tiny inline percentage helper so we don't divide by zero on an
    // empty run.
    const auto pct = [](std::uint64_t num, std::uint64_t den) {
        return den == 0 ? 0.0 : 100.0 * static_cast<double>(num)
                                       / static_cast<double>(den);
    };

    os << name << ":\n";
    os << "  accesses           " << s.accesses << '\n';
    os << "  reads              " << s.reads    << '\n';
    os << "  writes             " << s.writes   << '\n';
    if (wbwa_style) {
        os << "  hits               " << s.hits   << "  ("
           << std::fixed << std::setprecision(2)
           << pct(s.hits, s.accesses) << " %)\n";
        os << "  misses             " << s.misses << "  ("
           << std::fixed << std::setprecision(2)
           << pct(s.misses, s.accesses) << " %)\n";
        os << "  writebacks         " << s.writebacks << '\n';
    } else {
        os << "  read_hits          " << s.read_hits << "  ("
           << std::fixed << std::setprecision(2)
           << pct(s.read_hits, s.reads) << " %)\n";
        os << "  read_misses        " << s.read_misses << "  ("
           << std::fixed << std::setprecision(2)
           << pct(s.read_misses, s.reads) << " %)\n";
    }
    os << "  prefetches_issued  " << s.prefetches_issued << '\n';
    os << "  prefetch_hits      " << s.prefetch_hits     << '\n';
    os << "  prefetch_misses    " << s.prefetch_misses   << '\n';
}

} // namespace

// Translate one JSON-driven cache level (size_kb, block_size, assoc, ...)
// into the runtime Cache::Config (c, b, s, ...) the Cache class expects.
//
// The JSON exposes human-friendly numbers (e.g. size_kb=32, assoc=8); the
// Cache class wants log2 of those. log2_pow2 enforces the power-of-two
// constraint because the address-decomposition shifts assume it.
Cache::Config to_cache_config(const CacheLevelConfig& level) {
    Cache::Config cc;
    const unsigned size_log2  = log2_pow2(level.size_kb * 1024, "size_kb*1024");
    const unsigned block_log2 = log2_pow2(level.block_size, "block_size");
    const unsigned assoc_log2 = log2_pow2(level.assoc,      "assoc");

    cc.c = size_log2;
    cc.b = block_log2;
    cc.s = assoc_log2;

    // String -> enum conversions. parse_* return optional<>, so we can
    // surface a clean ConfigError instead of crashing on bad input.
    if (auto r = parse_replacement(level.replacement)) {
        cc.replacement = *r;
    } else {
        throw ConfigError("unknown replacement policy: " + level.replacement);
    }
    if (auto w = parse_write_policy(level.write_policy)) {
        cc.write_policy = *w;
    } else {
        throw ConfigError("unknown write policy: " + level.write_policy);
    }
    cc.hit_latency = static_cast<unsigned>(level.hit_latency);
    return cc;
}

// Translate the JSON memory section (latency in cycles) into the
// MainMemory runtime config. Trivial today — placeholder for swapping in
// a real DRAM model later.
MainMemory::Config to_memory_config(const MemoryConfig& mem) {
    MainMemory::Config m;
    m.latency = static_cast<unsigned>(mem.latency);
    return m;
}

namespace {

// Factory: build the right Prefetcher subclass from the config's "prefetcher"
// string. Returning nullptr means "no prefetcher attached"; the Cache
// then skips its on_miss hook entirely.
std::unique_ptr<Prefetcher> make_prefetcher(const CacheLevelConfig& level) {
    const auto& name = level.prefetcher;
    if (name == "none")     return nullptr;
    if (name == "plus_one") return std::make_unique<PlusOnePrefetcher>();
    if (name == "markov") {
        return std::make_unique<MarkovPrefetcher>(
            static_cast<unsigned>(level.n_markov_rows));
    }
    if (name == "hybrid") {
        return std::make_unique<HybridPrefetcher>(
            static_cast<unsigned>(level.n_markov_rows));
    }
    throw ConfigError("unknown prefetcher: " + name);
}

} // namespace

// Main driver for --mode cache.
//
// Steps:
//   1. Validate that --trace was supplied (cache mode is meaningless
//      without an access stream).
//   2. Build the hierarchy from the bottom up so each level's downstream
//      pointer is set at construction:
//          DRAM (MainMemory)
//             ^- L2 (Cache, points to mem, owns the prefetcher)
//                  ^- L1 (Cache, points to L2)
//   3. After both caches exist, patch L2.peer_above = &L1. The peer
//      pointer is what prefetchers consult to avoid re-prefetching a
//      block that's already in L1. We can't do this at construction
//      because L1 doesn't exist yet when L2 is built (chicken-and-egg).
//   4. Open the ChampSim trace and walk every record. For each record,
//      issue a Read for every non-zero source_memory entry and a Write
//      for every non-zero destination_memory entry. The cache model
//      handles latency / eviction / propagation internally.
//   5. Print L1, L2, DRAM stats.
//
// Return codes:
//   0 = success
//   2 = bad config / missing --trace
//   4 = trace error
int run_cache_mode(const SimConfig& cfg, const CliArgs& cli) {
    if (!cli.trace) {
        LOG_ERROR("--mode cache requires --trace");
        return 2;
    }

    // ---- Stage 1: build the hierarchy bottom-up.
    MainMemory mem(to_memory_config(cfg.memory));

    auto l2_cc = to_cache_config(cfg.l2);
    l2_cc.main_memory = &mem;
    l2_cc.prefetcher  = make_prefetcher(cfg.l2);
    // peer_above is patched after L1 is constructed; set below.
    Cache l2(std::move(l2_cc), "L2");

    auto l1_cc = to_cache_config(cfg.l1);
    l1_cc.next_level = &l2;
    Cache l1(std::move(l1_cc), "L1");

    // Resolve the L1<->L2 circular reference: L1.next_level=&L2 was set
    // at construction; we set L2.peer_above=&L1 here so the L2 prefetcher
    // can ask "is this block already in L1?" before issuing a prefetch.
    l2.set_peer_above(&l1);

    LOG_INFO("cache mode: L1 " << cfg.l1.size_kb << "KB / "
             << cfg.l1.assoc  << "-way / " << cfg.l1.write_policy
             << " / " << cfg.l1.replacement
             << " | L2 " << cfg.l2.size_kb << "KB / "
             << cfg.l2.assoc << "-way / " << cfg.l2.write_policy
             << " / " << cfg.l2.replacement);

    // ---- Stage 2: walk the trace and drive accesses.
    trace::Reader reader(*cli.trace, trace::Variant::Standard);
    trace::Record rec{};
    std::size_t records = 0;
    while (reader.next(rec)) {
        ++records;
        // Each record encodes up to 4 loads (source_memory) and 2 stores
        // (destination_memory). Slot value 0 means "unused".
        for (auto a : rec.source_memory) {
            if (a != 0) l1.access({a, Op::Read, rec.ip});
        }
        for (auto a : rec.destination_memory) {
            if (a != 0) l1.access({a, Op::Write, rec.ip});
        }
    }

    LOG_INFO("walked " << records << " records");

    // ---- Stage 3: print stats. The wbwa_style flag picks the right
    // counter set based on what the level was configured with.
    std::ostream& out = std::cout;
    out << "==== cache stats ====\n";
    print_level(out, "L1",   l1.stats(),  cfg.l1.write_policy != "writethrough"
                                       && cfg.l1.write_policy != "wtwna");
    print_level(out, "L2",   l2.stats(),  cfg.l2.write_policy != "writethrough"
                                       && cfg.l2.write_policy != "wtwna");
    out << "DRAM:\n"
        << "  accesses           " << mem.stats().accesses << '\n'
        << "  reads              " << mem.stats().reads    << '\n'
        << "  writes             " << mem.stats().writes   << '\n';

    return 0;
}

} // namespace comparch::cache
