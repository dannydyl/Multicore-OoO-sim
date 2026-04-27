#include <catch2/catch_test_macros.hpp>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include "comparch/cache/cache.hpp"
#include "comparch/cache/main_memory.hpp"
#include "comparch/cache/prefetcher_hybrid.hpp"
#include "comparch/cache/prefetcher_markov.hpp"
#include "comparch/cache/prefetcher_plus_one.hpp"

using comparch::cache::Cache;
using comparch::cache::HybridPrefetcher;
using comparch::cache::MainMemory;
using comparch::cache::MarkovPrefetcher;
using comparch::cache::MemReq;
using comparch::cache::Op;
using comparch::cache::PlusOnePrefetcher;
using comparch::cache::Prefetcher;
using comparch::cache::Replacement;
using comparch::cache::WritePolicy;

namespace {

// project1 defaults: L1 (C,B,S)=(10,6,1) MIP WBWA, L2 (15,6,3) LIP WTWNA.
struct Proj1Hierarchy {
    MainMemory mem;
    Cache      l2;
    Cache      l1;

    static Cache::Config l1_cfg(Cache* l2_ptr) {
        Cache::Config c;
        c.c = 10; c.b = 6; c.s = 1;
        c.replacement  = Replacement::LRU_MIP;
        c.write_policy = WritePolicy::WBWA;
        c.next_level   = l2_ptr;
        return c;
    }
    static Cache::Config l2_cfg(MainMemory* mem_ptr,
                                std::unique_ptr<Prefetcher> pf) {
        Cache::Config c;
        c.c = 15; c.b = 6; c.s = 3;
        c.replacement  = Replacement::LRU_LIP;
        c.write_policy = WritePolicy::WTWNA;
        c.main_memory  = mem_ptr;
        c.prefetcher   = std::move(pf);
        return c;
    }

    explicit Proj1Hierarchy(std::unique_ptr<Prefetcher> pf)
        : mem(MainMemory::Config{100}),
          l2(l2_cfg(&mem, std::move(pf)), "L2"),
          l1(l1_cfg(&l2), "L1") {
        l2.set_peer_above(&l1);
    }
};

// Drive the hierarchy from a project1 ascii trace (R/W 0x... lines).
void drive_trace(const std::filesystem::path& path, Cache& l1) {
    std::ifstream in(path);
    REQUIRE(in.good());

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        char rw = line[0];
        std::uint64_t addr = 0;
        if (std::sscanf(line.c_str() + 2, "%" SCNx64, &addr) != 1) continue;
        const Op op = (rw == 'W') ? Op::Write : Op::Read;
        l1.access({addr, op, 0});
    }
}

// Parse project1's print_statistics output. Returns map of label -> int.
std::map<std::string, std::uint64_t>
parse_proj1_out(const std::filesystem::path& path) {
    std::ifstream in(path);
    REQUIRE(in.good());
    std::map<std::string, std::uint64_t> out;
    std::string line;
    while (std::getline(in, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        // Trim leading space from value.
        size_t i = 0;
        while (i < val.size() && std::isspace(static_cast<unsigned char>(val[i]))) ++i;
        val = val.substr(i);
        // Skip non-integer fields (ratios, AAT, settings).
        try {
            const std::uint64_t n = std::stoull(val);
            out[key] = n;
        } catch (...) {
            // ignore
        }
    }
    return out;
}

void check_counters(const std::map<std::string, std::uint64_t>& exp,
                    const Cache& l1, const Cache& l2) {
    REQUIRE(exp.at("Reads")              == l1.stats().reads);
    REQUIRE(exp.at("Writes")             == l1.stats().writes);

    REQUIRE(exp.at("L1 accesses")        == l1.stats().accesses);
    REQUIRE(exp.at("L1 hits")            == l1.stats().hits);
    REQUIRE(exp.at("L1 misses")          == l1.stats().misses);
    REQUIRE(exp.at("Write-backs from L1") == l1.stats().writebacks);

    REQUIRE(exp.at("L2 reads")           == l2.stats().reads);
    REQUIRE(exp.at("L2 writes")          == l2.stats().writes);
    REQUIRE(exp.at("L2 read hits")       == l2.stats().read_hits);
    REQUIRE(exp.at("L2 read misses")     == l2.stats().read_misses);

    REQUIRE(exp.at("L2 prefetches issued") == l2.stats().prefetches_issued);
    REQUIRE(exp.at("L2 prefetch hits")    == l2.stats().prefetch_hits);
    REQUIRE(exp.at("L2 prefetch misses")  == l2.stats().prefetch_misses);
}

const std::filesystem::path kFixtureDir =
    std::filesystem::path(PROJ1_FIXTURE_DIR);

} // namespace

TEST_CASE("proj1 regression: short_gcc.trace, default config (no prefetch)",
          "[cache][proj1]") {
    Proj1Hierarchy h(/*pf=*/nullptr);
    drive_trace(kFixtureDir / "short_gcc.trace", h.l1);
    auto exp = parse_proj1_out(kFixtureDir / "short_gcc_default.out");
    check_counters(exp, h.l1, h.l2);
}

TEST_CASE("proj1 regression: short_gcc.trace, +1 prefetch",
          "[cache][proj1]") {
    Proj1Hierarchy h(std::make_unique<PlusOnePrefetcher>());
    drive_trace(kFixtureDir / "short_gcc.trace", h.l1);
    auto exp = parse_proj1_out(kFixtureDir / "short_gcc_plus1.out");
    check_counters(exp, h.l1, h.l2);
}

TEST_CASE("proj1 regression: short_gcc.trace, Markov prefetch (256 rows)",
          "[cache][proj1]") {
    Proj1Hierarchy h(std::make_unique<MarkovPrefetcher>(256));
    drive_trace(kFixtureDir / "short_gcc.trace", h.l1);
    auto exp = parse_proj1_out(kFixtureDir / "short_gcc_markov.out");
    check_counters(exp, h.l1, h.l2);
}

TEST_CASE("proj1 regression: short_gcc.trace, Hybrid prefetch (256 rows)",
          "[cache][proj1]") {
    Proj1Hierarchy h(std::make_unique<HybridPrefetcher>(256));
    drive_trace(kFixtureDir / "short_gcc.trace", h.l1);
    auto exp = parse_proj1_out(kFixtureDir / "short_gcc_hybrid.out");
    check_counters(exp, h.l1, h.l2);
}
