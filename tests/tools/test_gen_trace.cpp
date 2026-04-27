#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "comparch/trace.hpp"
#include "gen_trace.hpp"

using comparch::gen_trace::generate_records;
using comparch::gen_trace::GenParams;
using comparch::gen_trace::Pattern;
using comparch::trace::Reader;
using comparch::trace::Record;
using comparch::trace::Variant;
using comparch::trace::Writer;

namespace {

std::size_t generate_to_buffer(const GenParams& p, std::stringstream& buf) {
    Writer w(buf, Variant::Standard);
    generate_records(p, w);
    return w.records_written();
}

} // namespace

TEST_CASE("gen_trace produces the requested record count", "[gen_trace]") {
    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
    GenParams p;
    p.records = 4096;
    p.pattern = Pattern::Sequential;
    REQUIRE(generate_to_buffer(p, buf) == 4096);

    Reader r(buf, Variant::Standard);
    Record got{};
    std::size_t n = 0;
    while (r.next(got)) ++n;
    REQUIRE(n == 4096);
}

TEST_CASE("gen_trace branch/load/store rates land within tolerance",
          "[gen_trace]") {
    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
    GenParams p;
    p.records     = 50000;
    p.pattern     = Pattern::Sequential;
    p.branch_rate = 0.20;
    p.load_rate   = 0.40;
    p.store_rate  = 0.10;
    p.seed        = 0xABCD'EF01ULL;
    generate_to_buffer(p, buf);

    Reader r(buf, Variant::Standard);
    Record rec{};
    std::size_t branches = 0, loads = 0, stores = 0, alus = 0;
    while (r.next(rec)) {
        if (rec.is_branch) {
            ++branches;
        } else if (rec.source_memory[0] != 0) {
            ++loads;
        } else if (rec.destination_memory[0] != 0) {
            ++stores;
        } else {
            ++alus;
        }
    }
    REQUIRE(branches + loads + stores + alus == p.records);

    const double br = static_cast<double>(branches) / p.records;
    const double lr = static_cast<double>(loads)    / p.records;
    const double sr = static_cast<double>(stores)   / p.records;

    REQUIRE(std::abs(br - 0.20)      < 0.02);
    REQUIRE(std::abs(lr - 0.80*0.40) < 0.02);
    REQUIRE(std::abs(sr - 0.80*0.10) < 0.02);
}

TEST_CASE("gen_trace stream pattern produces strided source addresses",
          "[gen_trace]") {
    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
    GenParams p;
    p.records      = 4096;
    p.pattern      = Pattern::Stream;
    p.branch_rate  = 0.0;
    p.load_rate    = 1.0;
    p.store_rate   = 0.0;
    p.addr_base    = 0x1000'0000ULL;
    p.addr_stride  = 64;
    p.seed         = 0x1ULL;
    generate_to_buffer(p, buf);

    Reader r(buf, Variant::Standard);
    Record rec{};
    std::size_t i = 0;
    while (r.next(rec)) {
        REQUIRE(rec.source_memory[0] == p.addr_base + i * p.addr_stride);
        ++i;
    }
    REQUIRE(i == p.records);
}

TEST_CASE("gen_trace loop pattern cycles PCs", "[gen_trace]") {
    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
    GenParams p;
    p.records   = 1000;
    p.pattern   = Pattern::Loop;
    p.pc_base   = 0x40'0000ULL;
    p.pc_stride = 4;
    p.loop_size = 16;
    p.seed      = 0x42ULL;
    generate_to_buffer(p, buf);

    Reader r(buf, Variant::Standard);
    Record rec{};
    std::size_t i = 0;
    while (r.next(rec)) {
        const std::uint64_t expect_pc =
            p.pc_base + (i % p.loop_size) * p.pc_stride;
        REQUIRE(rec.ip == expect_pc);
        ++i;
    }
    REQUIRE(i == p.records);
}

TEST_CASE("gen_trace round-trips through a file", "[gen_trace]") {
    auto tmp = std::filesystem::temp_directory_path()
             / "comparch_gen_trace_roundtrip.champsimtrace";

    GenParams p;
    p.records = 2048;
    p.seed    = 0x9999ULL;

    {
        Writer w(tmp, Variant::Standard);
        generate_records(p, w);
    }
    REQUIRE(std::filesystem::file_size(tmp) == p.records * 64);

    {
        Reader r(tmp, Variant::Standard);
        Record rec{};
        std::size_t n = 0;
        while (r.next(rec)) ++n;
        REQUIRE(n == p.records);
    }
    std::filesystem::remove(tmp);
}
