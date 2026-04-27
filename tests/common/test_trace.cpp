#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

#include "comparch/trace.hpp"

using comparch::trace::Reader;
using comparch::trace::Record;
using comparch::trace::TraceError;
using comparch::trace::Variant;
using comparch::trace::Writer;

namespace {

Record make_record(std::mt19937_64& rng) {
    Record r;
    r.ip = rng();
    r.is_branch    = (rng() & 1) != 0;
    r.branch_taken = r.is_branch && ((rng() & 1) != 0);
    for (auto& v : r.destination_registers) v = static_cast<std::uint8_t>(rng() & 0xFF);
    for (auto& v : r.source_registers)      v = static_cast<std::uint8_t>(rng() & 0xFF);
    for (auto& v : r.destination_memory)    v = rng();
    for (auto& v : r.source_memory)         v = rng();
    return r;
}

} // namespace

TEST_CASE("Standard record on-disk size is exactly 64 bytes", "[trace]") {
    REQUIRE(comparch::trace::record_bytes(Variant::Standard) == 64);

    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
    Writer w(buf, Variant::Standard);
    Record r{};
    r.ip = 0xDEADBEEFCAFE0001ULL;
    w.write(r);
    w.flush();

    REQUIRE(buf.str().size() == 64);
}

TEST_CASE("Round-trip preserves a single record", "[trace]") {
    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);

    Record orig{};
    orig.ip = 0x1122334455667788ULL;
    orig.is_branch = true;
    orig.branch_taken = false;
    orig.destination_registers = {7, 0};
    orig.source_registers      = {1, 2, 3, 0};
    orig.destination_memory    = {0xAAAA'BBBB'CCCC'DDDDULL, 0};
    orig.source_memory         = {0x1, 0x2, 0x3, 0x4};

    {
        Writer w(buf, Variant::Standard);
        w.write(orig);
    }

    Reader r(buf, Variant::Standard);
    Record got{};
    REQUIRE(r.next(got));
    REQUIRE(got == orig);

    Record extra{};
    REQUIRE_FALSE(r.next(extra));
    REQUIRE(r.records_read() == 1);
}

TEST_CASE("Round-trip preserves a 1024-record stream", "[trace]") {
    std::mt19937_64 rng(0xC0DEF00D);
    std::vector<Record> records;
    records.reserve(1024);
    for (int i = 0; i < 1024; ++i) records.push_back(make_record(rng));

    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
    {
        Writer w(buf, Variant::Standard);
        for (const auto& r : records) w.write(r);
        REQUIRE(w.records_written() == records.size());
    }

    REQUIRE(buf.str().size() == records.size() * 64);

    Reader r(buf, Variant::Standard);
    for (std::size_t i = 0; i < records.size(); ++i) {
        Record got{};
        REQUIRE(r.next(got));
        REQUIRE(got == records[i]);
    }
    Record extra{};
    REQUIRE_FALSE(r.next(extra));
    REQUIRE(r.records_read() == records.size());
}

TEST_CASE("Reader detects truncated records", "[trace]") {
    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
    {
        Writer w(buf, Variant::Standard);
        Record r{};
        r.ip = 0x42;
        w.write(r);
        w.write(r);
    }

    auto s = buf.str();
    s.resize(64 + 30);
    std::stringstream truncated(s, std::ios::in | std::ios::binary);

    Reader r(truncated, Variant::Standard);
    Record got{};
    REQUIRE(r.next(got));
    REQUIRE(got.ip == 0x42);
    REQUIRE_THROWS_AS(r.next(got), TraceError);
}

TEST_CASE("Empty stream yields zero records cleanly", "[trace]") {
    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
    Reader r(buf, Variant::Standard);
    Record got{};
    REQUIRE_FALSE(r.next(got));
    REQUIRE(r.records_read() == 0);
}

TEST_CASE("Round-trip through a real file", "[trace]") {
    auto tmp = std::filesystem::temp_directory_path()
             / "comparch_trace_roundtrip.champsimtrace";

    std::mt19937_64 rng(0xABCD'1234'EF56'7890ULL);
    std::vector<Record> records;
    records.reserve(64);
    for (int i = 0; i < 64; ++i) records.push_back(make_record(rng));

    {
        Writer w(tmp, Variant::Standard);
        for (const auto& r : records) w.write(r);
    }

    REQUIRE(std::filesystem::file_size(tmp) == records.size() * 64);

    {
        Reader r(tmp, Variant::Standard);
        for (const auto& expected : records) {
            Record got{};
            REQUIRE(r.next(got));
            REQUIRE(got == expected);
        }
        Record extra{};
        REQUIRE_FALSE(r.next(extra));
    }

    std::filesystem::remove(tmp);
}

TEST_CASE("Opening a missing file throws TraceError", "[trace]") {
    REQUIRE_THROWS_AS(
        Reader("/nonexistent/path/should/not/exist.champsimtrace", Variant::Standard),
        TraceError);
}
