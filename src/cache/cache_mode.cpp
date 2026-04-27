#include "comparch/cache/cache_mode.hpp"

#include <bit>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

#include "comparch/log.hpp"
#include "comparch/trace.hpp"

namespace comparch::cache {

namespace {

unsigned log2_pow2(int value, const char* what) {
    if (value <= 0 || std::popcount(static_cast<unsigned>(value)) != 1) {
        std::ostringstream oss;
        oss << what << " must be a positive power of two (got " << value << ")";
        throw ConfigError(oss.str());
    }
    return static_cast<unsigned>(std::countr_zero(static_cast<unsigned>(value)));
}

void print_level(std::ostream& os, const std::string& name,
                 const CacheStats& s, bool wbwa_style) {
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

Cache::Config to_cache_config(const CacheLevelConfig& level) {
    Cache::Config cc;
    const unsigned size_log2  = log2_pow2(level.size_kb * 1024, "size_kb*1024");
    const unsigned block_log2 = log2_pow2(level.block_size, "block_size");
    const unsigned assoc_log2 = log2_pow2(level.assoc,      "assoc");

    cc.c = size_log2;
    cc.b = block_log2;
    cc.s = assoc_log2;

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

MainMemory::Config to_memory_config(const MemoryConfig& mem) {
    MainMemory::Config m;
    m.latency = static_cast<unsigned>(mem.latency);
    return m;
}

int run_cache_mode(const SimConfig& cfg, const CliArgs& cli) {
    if (!cli.trace) {
        LOG_ERROR("--mode cache requires --trace");
        return 2;
    }

    MainMemory mem(to_memory_config(cfg.memory));

    auto l2_cc = to_cache_config(cfg.l2);
    l2_cc.main_memory = &mem;
    Cache l2(l2_cc, "L2");

    auto l1_cc = to_cache_config(cfg.l1);
    l1_cc.next_level = &l2;
    Cache l1(l1_cc, "L1");

    LOG_INFO("cache mode: L1 " << cfg.l1.size_kb << "KB / "
             << cfg.l1.assoc  << "-way / " << cfg.l1.write_policy
             << " / " << cfg.l1.replacement
             << " | L2 " << cfg.l2.size_kb << "KB / "
             << cfg.l2.assoc << "-way / " << cfg.l2.write_policy
             << " / " << cfg.l2.replacement);

    trace::Reader reader(*cli.trace, trace::Variant::Standard);
    trace::Record rec{};
    std::size_t records = 0;
    while (reader.next(rec)) {
        ++records;
        for (auto a : rec.source_memory) {
            if (a != 0) l1.access({a, Op::Read, rec.ip});
        }
        for (auto a : rec.destination_memory) {
            if (a != 0) l1.access({a, Op::Write, rec.ip});
        }
    }

    LOG_INFO("walked " << records << " records");

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
