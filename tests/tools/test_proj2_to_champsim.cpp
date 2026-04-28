#include <catch2/catch_test_macros.hpp>

#include <sstream>

#include "comparch/trace.hpp"
#include "proj2_to_champsim.hpp"

using namespace comparch;

TEST_CASE("proj2_to_champsim parses a branch line", "[proj2_to_champsim]") {
    // Format: pc opcode dest src1 src2 mem br_taken br_target ic dc dyn
    // pc and br_target are hex; addresses are hex; the rest are decimal.
    const std::string_view line =
        "400123 6 -1 5 6 0 1 400200 1 1 42";

    trace::Record rec;
    const auto r = proj2::parse_line(line, rec);
    REQUIRE(r.ok);
    REQUIRE(rec.ip == 0x400123);
    REQUIRE(rec.is_branch);
    REQUIRE(rec.branch_taken);
    // src1=5, src2=6 → bytes 5, 6; dest=-1 → 0; remaining slots stay 0.
    REQUIRE(rec.source_registers[0] == 5);
    REQUIRE(rec.source_registers[1] == 6);
    REQUIRE(rec.destination_registers[0] == 0);
    // No memory addresses for branches.
    for (auto a : rec.source_memory)      REQUIRE(a == 0);
    for (auto a : rec.destination_memory) REQUIRE(a == 0);
}

TEST_CASE("proj2_to_champsim places load addr in source_memory", "[proj2_to_champsim]") {
    const std::string_view line =
        "400000 4 7 1 -1 deadbeef 0 0 1 1 100";
    trace::Record rec;
    REQUIRE(proj2::parse_line(line, rec).ok);
    REQUIRE_FALSE(rec.is_branch);
    REQUIRE(rec.source_memory[0] == 0xdeadbeef);
    REQUIRE(rec.destination_memory[0] == 0);
    REQUIRE(rec.destination_registers[0] == 7);
}

TEST_CASE("proj2_to_champsim places store addr in destination_memory", "[proj2_to_champsim]") {
    const std::string_view line =
        "400000 5 -1 1 2 cafe1234 0 0 1 1 100";
    trace::Record rec;
    REQUIRE(proj2::parse_line(line, rec).ok);
    REQUIRE_FALSE(rec.is_branch);
    REQUIRE(rec.destination_memory[0] == 0xcafe1234);
    REQUIRE(rec.source_memory[0] == 0);
}

TEST_CASE("proj2_to_champsim rejects malformed lines", "[proj2_to_champsim]") {
    trace::Record rec;
    SECTION("blank line") {
        const auto r = proj2::parse_line("", rec);
        REQUIRE_FALSE(r.ok);
    }
    SECTION("too few fields") {
        const auto r = proj2::parse_line("400000 6 -1", rec);
        REQUIRE_FALSE(r.ok);
    }
    SECTION("opcode out of range") {
        const auto r = proj2::parse_line("400000 9 -1 1 2 0 0 0 1 1 100", rec);
        REQUIRE_FALSE(r.ok);
    }
}

TEST_CASE("proj2_to_champsim convert() round-trips through trace::Reader", "[proj2_to_champsim]") {
    const std::string text =
        "400000 2 7 1 2 0 0 0 1 1 0\n"     // ALU, no mem, no branch
        "400004 6 -1 7 -1 0 1 400100 1 1 1\n"   // taken branch
        "400008 4 8 1 -1 1000 0 0 1 1 2\n";  // load

    std::istringstream is(text);
    std::ostringstream os(std::ios::binary);
    {
        trace::Writer w(os, trace::Variant::Standard);
        const auto n = proj2::convert(is, w);
        REQUIRE(n == 3);
    }

    std::istringstream rs(os.str(), std::ios::binary);
    trace::Reader r(rs, trace::Variant::Standard);
    trace::Record rec;

    REQUIRE(r.next(rec));
    REQUIRE(rec.ip == 0x400000);
    REQUIRE_FALSE(rec.is_branch);

    REQUIRE(r.next(rec));
    REQUIRE(rec.ip == 0x400004);
    REQUIRE(rec.is_branch);
    REQUIRE(rec.branch_taken);

    REQUIRE(r.next(rec));
    REQUIRE(rec.ip == 0x400008);
    REQUIRE(rec.source_memory[0] == 0x1000);

    REQUIRE_FALSE(r.next(rec));
}
