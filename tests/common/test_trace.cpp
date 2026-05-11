#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

#include "comparch/trace.hpp"

using comparch::trace::AnyRecord;
using comparch::trace::FileHeader;
using comparch::trace::LifecycleKind;
using comparch::trace::LifecycleRecord;
using comparch::trace::Reader;
using comparch::trace::Record;
using comparch::trace::SyncKind;
using comparch::trace::SyncRecord;
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

// ---- CasimV2 ---------------------------------------------------------

TEST_CASE("CasimV2 record on-disk size is exactly 64 bytes", "[trace][v2]") {
    REQUIRE(comparch::trace::record_bytes(Variant::CasimV2) == 64);
}

TEST_CASE("CasimV2 instr/sync/lifecycle round-trip via next_any", "[trace][v2]") {
    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);

    Record  i1{}; i1.ip = 0x1000; i1.is_branch = true; i1.branch_taken = true;
    i1.source_memory = {0xCAFE, 0, 0, 0};
    SyncRecord s1{SyncKind::LockAcquire, /*obj*/0xABCD, /*seq*/7, /*extra*/0, /*ip*/0x2000, 0};
    Record  i2{}; i2.ip = 0x1004;
    SyncRecord s2{SyncKind::LockRelease, 0xABCD, 8, 0, 0x2008, 0};
    LifecycleRecord l1{LifecycleKind::ThreadSpawn, /*peer_tid*/3, /*ip*/0x3000};

    {
        Writer w(buf, Variant::CasimV2);
        FileHeader h; h.thread_id = 2; h.thread_count = 4; h.program_uid = 0xDEAD;
        w.write_header(h);
        w.write(i1);
        w.write(s1);
        w.write(i2);
        w.write(s2);
        w.write(l1);
    }

    // Header (32) + 5 * 64 = 352 bytes total.
    REQUIRE(buf.str().size() == 32 + 5 * 64);

    Reader r(buf, Variant::CasimV2);
    REQUIRE(r.header().thread_id    == 2);
    REQUIRE(r.header().thread_count == 4);
    REQUIRE(r.header().program_uid  == 0xDEADu);

    auto a = r.next_any();
    REQUIRE(std::holds_alternative<Record>(a));
    REQUIRE(std::get<Record>(a) == i1);

    a = r.next_any();
    REQUIRE(std::holds_alternative<SyncRecord>(a));
    REQUIRE(std::get<SyncRecord>(a) == s1);

    a = r.next_any();
    REQUIRE(std::holds_alternative<Record>(a));
    REQUIRE(std::get<Record>(a) == i2);

    a = r.next_any();
    REQUIRE(std::holds_alternative<SyncRecord>(a));
    REQUIRE(std::get<SyncRecord>(a) == s2);

    a = r.next_any();
    REQUIRE(std::holds_alternative<LifecycleRecord>(a));
    REQUIRE(std::get<LifecycleRecord>(a) == l1);

    a = r.next_any();
    REQUIRE(std::holds_alternative<std::monostate>(a));
}

TEST_CASE("CasimV2 next(Record&) skips non-instr records", "[trace][v2]") {
    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
    {
        Writer w(buf, Variant::CasimV2);
        FileHeader h; h.thread_id = 0; h.thread_count = 1;
        w.write_header(h);
        Record i1{}; i1.ip = 0xAA;
        Record i2{}; i2.ip = 0xBB;
        SyncRecord s{SyncKind::BarrierArrive, 0x100, 1, 4, 0, 0};
        LifecycleRecord l{LifecycleKind::ThreadSpawn, 1, 0};
        w.write(i1);
        w.write(s);
        w.write(l);
        w.write(i2);
    }
    Reader r(buf, Variant::CasimV2);
    Record got{};
    REQUIRE(r.next(got));
    REQUIRE(got.ip == 0xAA);
    REQUIRE(r.next(got));
    REQUIRE(got.ip == 0xBB);  // sync + lifecycle skipped
    REQUIRE_FALSE(r.next(got));
    REQUIRE(r.records_read() == 2);
}

TEST_CASE("open_auto detects CasimV2 vs Standard by magic", "[trace][v2]") {
    // Write one of each into temp files and check the auto detector.
    auto v2_path = std::filesystem::temp_directory_path() / "comparch_auto_v2.casim";
    auto std_path = std::filesystem::temp_directory_path() / "comparch_auto_std.champsimtrace";

    {
        Writer w(v2_path, Variant::CasimV2);
        FileHeader h; h.thread_id = 5; h.thread_count = 8;
        w.write_header(h);
        Record r{}; r.ip = 0x77;
        w.write(r);
    }
    {
        Writer w(std_path, Variant::Standard);
        Record r{}; r.ip = 0x88;
        w.write(r);
    }

    {
        auto r = Reader::open_auto(v2_path);
        REQUIRE(r.variant() == Variant::CasimV2);
        REQUIRE(r.header().thread_id == 5);
        Record got{};
        REQUIRE(r.next(got));
        REQUIRE(got.ip == 0x77);
    }
    {
        auto r = Reader::open_auto(std_path);
        REQUIRE(r.variant() == Variant::Standard);
        Record got{};
        REQUIRE(r.next(got));
        REQUIRE(got.ip == 0x88);
    }

    std::filesystem::remove(v2_path);
    std::filesystem::remove(std_path);
}

TEST_CASE("CasimV2 writer rejects record before header", "[trace][v2]") {
    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
    Writer w(buf, Variant::CasimV2);
    Record r{};
    REQUIRE_THROWS_AS(w.write(r), TraceError);
}

TEST_CASE("Standard reader rejects next_any() and Standard writer rejects header",
          "[trace][v2]") {
    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
    {
        Writer w(buf, Variant::Standard);
        Record r{}; r.ip = 1;
        w.write(r);
    }
    Reader r(buf, Variant::Standard);
    REQUIRE_THROWS_AS(r.next_any(), TraceError);

    std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
    Writer w(out, Variant::Standard);
    FileHeader h;
    REQUIRE_THROWS_AS(w.write_header(h), TraceError);
}

// A SyncSink test double that the test can flip between "approve"
// and "reject", with a counter for how many times each method ran.
class TestSink : public comparch::trace::SyncSink {
public:
    bool                                approve = true;
    std::size_t                         consume_calls = 0;
    std::size_t                         lifecycle_calls = 0;
    std::vector<comparch::trace::SyncRecord>      seen_sync;
    std::vector<comparch::trace::LifecycleRecord> seen_life;

    bool try_consume_sync(std::uint32_t /*tid*/,
                          const comparch::trace::SyncRecord& s) override {
        ++consume_calls;
        if (approve) {
            seen_sync.push_back(s);
            return true;
        }
        return false;
    }
    void notify_retire(std::uint32_t /*tid*/,
                       const comparch::trace::SyncRecord& /*s*/) override {
        // Not exercised by the trace-layer tests; retire-time
        // delivery is OoO-stage-specific. Stub is enough.
    }
    void on_lifecycle(std::uint32_t /*tid*/,
                      const comparch::trace::LifecycleRecord& l) override {
        ++lifecycle_calls;
        seen_life.push_back(l);
    }
};

TEST_CASE("Reader+SyncSink: approve immediately drops sync, instr stream flows",
          "[trace][v2][sync]") {
    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
    {
        Writer w(buf, Variant::CasimV2);
        FileHeader h; h.thread_id = 0; h.thread_count = 1;
        w.write_header(h);
        Record i1{}; i1.ip = 0x1;
        Record i2{}; i2.ip = 0x2;
        w.write(i1);
        w.write(SyncRecord{SyncKind::LockAcquire, 0xABC, 0, 0, 0, 0});
        w.write(LifecycleRecord{LifecycleKind::ThreadSpawn, 1, 0});
        w.write(i2);
    }

    Reader r(buf, Variant::CasimV2);
    TestSink sink;
    r.set_sync_sink(&sink, /*tid=*/0);

    Record got{};
    REQUIRE(r.next(got));
    REQUIRE(got.ip == 0x1);

    // Sync + lifecycle are auto-consumed; second next() returns i2.
    REQUIRE(r.next(got));
    REQUIRE(got.ip == 0x2);
    REQUIRE_FALSE(r.blocked());

    REQUIRE_FALSE(r.next(got));   // EOF, not stalled
    REQUIRE_FALSE(r.blocked());

    REQUIRE(sink.consume_calls == 1);
    REQUIRE(sink.lifecycle_calls == 1);
    REQUIRE(sink.seen_sync.front().sync_object_addr == 0xABCu);
}

TEST_CASE("Reader+SyncSink: reject stalls fetch; later approve unblocks",
          "[trace][v2][sync]") {
    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
    {
        Writer w(buf, Variant::CasimV2);
        FileHeader h; h.thread_id = 0; h.thread_count = 1;
        w.write_header(h);
        Record i1{}; i1.ip = 0x10;
        Record i2{}; i2.ip = 0x20;
        w.write(i1);
        w.write(SyncRecord{SyncKind::LockAcquire, 0xABC, 5, 0, 0, 0});
        w.write(i2);
    }

    Reader r(buf, Variant::CasimV2);
    TestSink sink;
    sink.approve = false;
    r.set_sync_sink(&sink, /*tid=*/0);

    Record got{};
    REQUIRE(r.next(got));
    REQUIRE(got.ip == 0x10);
    REQUIRE_FALSE(r.blocked());

    // Hits the LockAcquire — sink rejects, reader stalls.
    REQUIRE_FALSE(r.next(got));
    REQUIRE(r.blocked());
    REQUIRE(sink.consume_calls == 1);

    // Re-poll while still rejecting: blocked() stays true, no advance.
    REQUIRE_FALSE(r.next(got));
    REQUIRE(r.blocked());
    REQUIRE(sink.consume_calls == 2);

    // Sink approves; next() consumes the sync and returns i2.
    sink.approve = true;
    REQUIRE(r.next(got));
    REQUIRE(got.ip == 0x20);
    REQUIRE_FALSE(r.blocked());
    REQUIRE(sink.consume_calls == 3);

    REQUIRE_FALSE(r.next(got));   // EOF
    REQUIRE_FALSE(r.blocked());
}

TEST_CASE("CasimV2 is_mul flag survives round-trip", "[trace][v2][a4]") {
    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
    {
        Writer w(buf, Variant::CasimV2);
        FileHeader h; h.thread_id = 0; h.thread_count = 1;
        w.write_header(h);
        Record m{}; m.ip = 0xAA; m.is_mul = true;
        Record a{}; a.ip = 0xBB;                       // plain ALU
        Record b{}; b.ip = 0xCC; b.is_branch = true;   // branch (mul=false)
        w.write(m);
        w.write(a);
        w.write(b);
    }
    Reader r(buf, Variant::CasimV2);
    Record got{};

    REQUIRE(r.next(got));
    REQUIRE(got.ip == 0xAA);
    REQUIRE(got.is_mul == true);
    REQUIRE(got.is_branch == false);

    REQUIRE(r.next(got));
    REQUIRE(got.ip == 0xBB);
    REQUIRE(got.is_mul == false);

    REQUIRE(r.next(got));
    REQUIRE(got.ip == 0xCC);
    REQUIRE(got.is_mul == false);
    REQUIRE(got.is_branch == true);
}

TEST_CASE("CasimV2 1024-record stream round-trips via next_any", "[trace][v2]") {
    std::mt19937_64 rng(0xFEEDFACEDEADBEEFULL);
    std::vector<Record> records;
    records.reserve(1024);
    for (int i = 0; i < 1024; ++i) records.push_back(make_record(rng));

    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
    {
        Writer w(buf, Variant::CasimV2);
        FileHeader h; h.thread_id = 0; h.thread_count = 1;
        w.write_header(h);
        for (const auto& r : records) w.write(r);
    }
    REQUIRE(buf.str().size() == 32 + records.size() * 64);

    Reader r(buf, Variant::CasimV2);
    for (std::size_t i = 0; i < records.size(); ++i) {
        auto a = r.next_any();
        REQUIRE(std::holds_alternative<Record>(a));
        REQUIRE(std::get<Record>(a) == records[i]);
    }
    auto eof = r.next_any();
    REQUIRE(std::holds_alternative<std::monostate>(eof));
}
