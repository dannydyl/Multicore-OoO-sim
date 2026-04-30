// Synthetic OoO pipeline tests. Each test builds a tiny in-memory
// ChampSim trace via trace::Writer, hands it to an OooCore, and asserts
// analytically-computable IPC / cycle bounds.

#include <catch2/catch_test_macros.hpp>

#include <sstream>

#include "comparch/cache/cache.hpp"
#include "comparch/cache/main_memory.hpp"
#include "comparch/ooo/core.hpp"
#include "comparch/ooo/inst.hpp"
#include "comparch/predictor/predictor.hpp"
#include "comparch/trace.hpp"

using comparch::cache::Cache;
using comparch::cache::MainMemory;
using comparch::ooo::OooConfig;
using comparch::ooo::OooCore;
using comparch::ooo::Opcode;
using comparch::PredictorConfig;
using comparch::trace::Record;
using comparch::trace::Reader;
using comparch::trace::Variant;
using comparch::trace::Writer;

namespace {

// Build a Cache::Config with very fast latencies; latencies don't matter
// for ALU-only tests. The L1 has no downstream so misses cost nothing
// extra (downstream_latency == 0). For tests that exercise loads we
// attach an L2/DRAM hierarchy explicitly.
Cache::Config small_l1d() {
    Cache::Config c;
    c.c = 12;                          // 4 KB
    c.b = 6;                           // 64 B blocks
    c.s = 1;                           // 2-way
    c.replacement   = comparch::cache::Replacement::LRU_MIP;
    c.write_policy  = comparch::cache::WritePolicy::WBWA;
    c.hit_latency   = 2;
    c.mshr_entries  = 8;
    return c;
}

OooConfig narrow_4_4() {
    OooConfig c;
    c.fetch_width = 4;
    c.rob_entries = 32;
    c.schedq_entries_per_fu = 4;
    c.alu_fus = 4;
    c.mul_fus = 1;
    c.lsu_fus = 2;
    return c;
}

PredictorConfig always_taken_cfg() {
    PredictorConfig p;
    p.type = "always_taken";
    return p;
}

PredictorConfig hybrid_cfg() {
    PredictorConfig p;
    p.type = "hybrid";
    // Default knobs from PredictorConfig are project2's H=10 P=5 G=9 N=7,
    // tournament_index_bits=12, tournament_counter_bits=2, hybrid_init=2
    // (weakly perceptron). Suitable for any test that just needs the
    // hybrid to run without asserting the bookkeeping invariant.
    return p;
}

// Helpers to synthesize ChampSim records.
Record alu_record(std::uint64_t pc, std::uint8_t dest = 0,
                  std::uint8_t src1 = 0, std::uint8_t src2 = 0) {
    Record r{};
    r.ip = pc;
    if (dest) r.destination_registers[0] = dest;
    if (src1) r.source_registers[0]      = src1;
    if (src2) r.source_registers[1]      = src2;
    return r;
}

Record load_record(std::uint64_t pc, std::uint64_t addr,
                   std::uint8_t dest = 0, std::uint8_t src1 = 0) {
    Record r{};
    r.ip = pc;
    if (dest) r.destination_registers[0] = dest;
    if (src1) r.source_registers[0]      = src1;
    r.source_memory[0] = addr;
    return r;
}

Record branch_record(std::uint64_t pc, bool taken) {
    Record r{};
    r.ip           = pc;
    r.is_branch    = true;
    r.branch_taken = taken;
    return r;
}

// Materialize a vector<Record> into an in-memory ChampSim binary stream
// and return a unique_ptr<istream> over it (the OoO core takes a Reader,
// which holds an istream*).
std::unique_ptr<std::stringstream> records_to_stream(const std::vector<Record>& recs) {
    auto ss = std::make_unique<std::stringstream>(
        std::ios::in | std::ios::out | std::ios::binary);
    Writer w(*ss, Variant::Standard);
    for (const auto& r : recs) w.write(r);
    w.flush();
    return ss;
}

} // namespace

TEST_CASE("Empty trace: zero retired, finite cycles", "[ooo][empty]") {
    auto stream = records_to_stream({});
    Reader reader(*stream, Variant::Standard);

    MainMemory mem({100});
    Cache l2(small_l1d(), "L2");          // unused but constructed for hierarchy
    Cache l1d(small_l1d(), "L1d");
    auto pred = comparch::predictor::make(always_taken_cfg());

    OooCore core(narrow_4_4(), *pred, l1d, reader);
    core.run(/*cycle_cap=*/1000);

    REQUIRE(core.eof());
    REQUIRE(core.stats().instructions_fetched == 0);
    REQUIRE(core.stats().instructions_retired == 0);
}

TEST_CASE("Independent ALU stream converges to fetch-width IPC",
          "[ooo][alu][ipc]") {
    // 200 independent ALU instructions: each writes a different dest
    // register, no source dependencies. With fetch=4 and 4 ALU FUs the
    // pipeline should hit very close to IPC=4 in steady state. Account
    // for fill + drain overhead by checking IPC > 3.5 and no_fire small.
    std::vector<Record> recs;
    constexpr int kN = 200;
    for (int i = 0; i < kN; ++i) {
        // Use destination registers cycling 1..31 (skip 0 = no operand
        // sentinel), no source operands -> trivially-ready dependencies.
        recs.push_back(alu_record(0x1000 + 4ULL * static_cast<unsigned>(i),
                                  /*dest=*/static_cast<std::uint8_t>(1 + (i % 31))));
    }
    auto stream = records_to_stream(recs);
    Reader reader(*stream, Variant::Standard);

    MainMemory mem({100});
    Cache l2(small_l1d(), "L2");
    Cache l1d(small_l1d(), "L1d");
    auto pred = comparch::predictor::make(always_taken_cfg());

    OooCore core(narrow_4_4(), *pred, l1d, reader);
    core.run(/*cycle_cap=*/2000);

    REQUIRE(core.eof());
    REQUIRE(core.stats().instructions_fetched == static_cast<std::uint64_t>(kN));
    REQUIRE(core.stats().instructions_retired == static_cast<std::uint64_t>(kN));
    // Steady state: IPC > 3.5 with these parameters.
    REQUIRE(core.stats().ipc() > 3.5);
}

TEST_CASE("Serial ALU dependency chain drops IPC to 1",
          "[ooo][alu][serial]") {
    // R1 = R1 + ... — every instruction reads and writes R1. The pipeline
    // must serialize on the RAW dependency. Even with 4 FUs and fetch=4,
    // IPC should sit near 1 (or below 2 — we just want to confirm
    // dependency tracking actually serializes things).
    std::vector<Record> recs;
    constexpr int kN = 80;
    for (int i = 0; i < kN; ++i) {
        recs.push_back(alu_record(0x2000 + 4ULL * static_cast<unsigned>(i),
                                  /*dest=*/1, /*src1=*/1));
    }
    auto stream = records_to_stream(recs);
    Reader reader(*stream, Variant::Standard);

    MainMemory mem({100});
    Cache l2(small_l1d(), "L2");
    Cache l1d(small_l1d(), "L1d");
    auto pred = comparch::predictor::make(always_taken_cfg());

    OooCore core(narrow_4_4(), *pred, l1d, reader);
    core.run(/*cycle_cap=*/4000);

    REQUIRE(core.eof());
    REQUIRE(core.stats().instructions_retired == static_cast<std::uint64_t>(kN));
    REQUIRE(core.stats().ipc() < 2.0);    // serialized
    REQUIRE(core.stats().ipc() > 0.3);    // but not stuck
}

TEST_CASE("Load chain bounded by L1-D latency", "[ooo][lsu]") {
    // Each load reads R2 and writes R2 -> next iteration depends. Cache
    // cold, then warm. IPC bounded by hit_latency between dependent loads.
    std::vector<Record> recs;
    constexpr int kN = 40;
    // All loads to the same block so they hit after the first miss.
    for (int i = 0; i < kN; ++i) {
        recs.push_back(load_record(0x3000 + 4ULL * static_cast<unsigned>(i),
                                   /*addr=*/0x40,
                                   /*dest=*/2, /*src1=*/2));
    }
    auto stream = records_to_stream(recs);
    Reader reader(*stream, Variant::Standard);

    MainMemory mem({100});
    Cache l2(small_l1d(), "L2");
    Cache l1d(small_l1d(), "L1d");
    auto pred = comparch::predictor::make(always_taken_cfg());

    OooCore core(narrow_4_4(), *pred, l1d, reader);
    core.run(/*cycle_cap=*/10000);

    REQUIRE(core.eof());
    REQUIRE(core.stats().instructions_retired == static_cast<std::uint64_t>(kN));
    // Serialized loads with hit_latency=2 -> IPC bounded near 0.5.
    REQUIRE(core.stats().ipc() < 0.6);
}

TEST_CASE("Branch with always_taken predicts correctly when taken",
          "[ooo][branch]") {
    // Stream of taken branches (always_taken predictor agrees) plus some
    // ALU filler. No mispredictions should be reported.
    std::vector<Record> recs;
    constexpr int kN = 50;
    for (int i = 0; i < kN; ++i) {
        recs.push_back(branch_record(0x4000 + 4ULL * static_cast<unsigned>(i),
                                     /*taken=*/true));
    }
    auto stream = records_to_stream(recs);
    Reader reader(*stream, Variant::Standard);

    MainMemory mem({100});
    Cache l2(small_l1d(), "L2");
    Cache l1d(small_l1d(), "L1d");
    auto pred = comparch::predictor::make(always_taken_cfg());

    OooCore core(narrow_4_4(), *pred, l1d, reader);
    core.run(/*cycle_cap=*/2000);

    REQUIRE(core.eof());
    REQUIRE(core.stats().num_branch_instructions == static_cast<std::uint64_t>(kN));
    REQUIRE(core.stats().branch_mispredictions == 0);
    REQUIRE(core.stats().instructions_retired   == static_cast<std::uint64_t>(kN));
}

TEST_CASE("Branch with always_taken records mispredict on not-taken",
          "[ooo][branch][mispred]") {
    // Single not-taken branch. Always-taken predictor mispredicts ->
    // exactly one mispred event reported. The branch still retires.
    std::vector<Record> recs;
    recs.push_back(branch_record(0x5000, /*taken=*/false));
    auto stream = records_to_stream(recs);
    Reader reader(*stream, Variant::Standard);

    MainMemory mem({100});
    Cache l2(small_l1d(), "L2");
    Cache l1d(small_l1d(), "L1d");
    auto pred = comparch::predictor::make(always_taken_cfg());

    OooCore core(narrow_4_4(), *pred, l1d, reader);
    core.run(/*cycle_cap=*/2000);

    REQUIRE(core.eof());
    REQUIRE(core.stats().num_branch_instructions == 1);
    REQUIRE(core.stats().branch_mispredictions   == 1);
    REQUIRE(core.stats().instructions_retired    == 1);
}

TEST_CASE("Hybrid predictor handles many branches in flight simultaneously",
          "[ooo][branch][hybrid][regression]") {
    // Regression for the predict()-twice-without-update assertion bug:
    // the OoO core fetches up to fetch_width branches per cycle, all
    // before any of them retire. The old hybrid kept single-slot
    // last_yp_/last_pct_/pending_update_ and would either assert (debug)
    // or train the tournament selector against the wrong outcome
    // (release). The fix moved this state into a per-inst_num map.
    //
    // Stream: alternating taken / not-taken at distinct PCs to keep the
    // branch outcome non-trivial. Several branches will be in fetch /
    // dispatch / sched / rob simultaneously.
    std::vector<Record> recs;
    constexpr int kN = 200;
    for (int i = 0; i < kN; ++i) {
        recs.push_back(branch_record(0x6000 + 4ULL * static_cast<unsigned>(i),
                                     /*taken=*/(i % 2) == 0));
    }
    auto stream = records_to_stream(recs);
    Reader reader(*stream, Variant::Standard);

    MainMemory mem({100});
    Cache l2(small_l1d(), "L2");
    Cache l1d(small_l1d(), "L1d");
    auto pred = comparch::predictor::make(hybrid_cfg());

    OooCore core(narrow_4_4(), *pred, l1d, reader);
    core.run(/*cycle_cap=*/20000);

    REQUIRE(core.eof());
    REQUIRE(core.stats().num_branch_instructions == static_cast<std::uint64_t>(kN));
    REQUIRE(core.stats().instructions_retired   == static_cast<std::uint64_t>(kN));
    // We don't pin a specific mispredict count — both sub-predictors are
    // training in parallel, and the alternating pattern is deliberately
    // hard. The point is that the run completed without tripping the
    // bookkeeping assert and produced a sane retire count.
}

TEST_CASE("Deadlock watchdog fires when pipeline stops making progress",
          "[ooo][watchdog]") {
    // A single load that misses L1 and L2 to DRAM. While the MSHR is
    // outstanding nothing gets fetched, nothing retires, and the queue
    // sizes don't move — that's a long stretch of constant signature.
    // With the watchdog threshold set well below the DRAM latency the
    // pipeline must trip before the load completes.
    std::vector<Record> recs;
    recs.push_back(load_record(/*pc=*/0x9000, /*addr=*/0x80,
                               /*dest=*/2, /*src1=*/0));
    auto stream = records_to_stream(recs);
    Reader reader(*stream, Variant::Standard);

    auto memcfg = comparch::cache::MainMemory::Config{500};   // long DRAM stall
    MainMemory mem(memcfg);

    auto cc2 = small_l1d();
    cc2.main_memory = &mem;                       // wire L2 -> DRAM
    Cache l2(std::move(cc2), "L2");

    auto cc1 = small_l1d();
    cc1.next_level = &l2;
    Cache l1d(std::move(cc1), "L1d");
    l2.set_peer_above(&l1d);

    auto cfg = narrow_4_4();
    cfg.deadlock_threshold_cycles = 30;

    auto pred = comparch::predictor::make(always_taken_cfg());
    OooCore core(cfg, *pred, l1d, reader);
    core.run(/*cycle_cap=*/2000);

    REQUIRE(core.stats().deadlocked);
    REQUIRE(core.stats().instructions_retired == 0);
    REQUIRE(core.stats().stall_cycles_at_abort >= 30);
}

TEST_CASE("Deadlock watchdog disabled (threshold=0) lets pipeline run",
          "[ooo][watchdog]") {
    // Same pathological-but-legal scenario as above, but with the
    // watchdog disabled. The load eventually completes and retires.
    std::vector<Record> recs;
    recs.push_back(load_record(/*pc=*/0x9000, /*addr=*/0x80,
                               /*dest=*/2, /*src1=*/0));
    auto stream = records_to_stream(recs);
    Reader reader(*stream, Variant::Standard);

    auto memcfg = comparch::cache::MainMemory::Config{100};
    MainMemory mem(memcfg);

    auto cc2 = small_l1d();
    cc2.main_memory = &mem;
    Cache l2(std::move(cc2), "L2");

    auto cc1 = small_l1d();
    cc1.next_level = &l2;
    Cache l1d(std::move(cc1), "L1d");
    l2.set_peer_above(&l1d);

    auto cfg = narrow_4_4();
    cfg.deadlock_threshold_cycles = 0;            // disabled

    auto pred = comparch::predictor::make(always_taken_cfg());
    OooCore core(cfg, *pred, l1d, reader);
    core.run(/*cycle_cap=*/2000);

    REQUIRE_FALSE(core.stats().deadlocked);
    REQUIRE(core.stats().instructions_retired == 1);
    REQUIRE(core.eof());
}

TEST_CASE("OoO LSU stalls cleanly when MSHR fills",
          "[ooo][lsu][mshr][regression]") {
    // Regression for the LSU busy-loop bug: previously, when issue()
    // returned nullopt the inner FU loop reset u.busy/u.sched_ptr but
    // never set oldest->busy, so the outer lsu_avail loop re-found the
    // same load and retried against the still-full MSHR — burning up to
    // lsu_avail wasted attempts per cycle. Worse, an MSHR with a
    // merge-eligible block could nondeterministically succeed mid-loop.
    //
    // Build a long stream of independent loads to distinct blocks (no
    // load-load merges possible) with a tiny MSHR. The cache *must*
    // saturate, the simulator *must* still make progress, and the retire
    // count *must* match the trace.
    std::vector<Record> recs;
    constexpr int kN = 64;
    for (int i = 0; i < kN; ++i) {
        // Stride by a full cache block so each load is to a distinct
        // block_addr; no merging.
        recs.push_back(load_record(0x7000 + 4ULL * static_cast<unsigned>(i),
                                   /*addr=*/0x10'0000ULL +
                                            static_cast<std::uint64_t>(i) * 64ULL,
                                   /*dest=*/static_cast<std::uint8_t>(1 + (i % 31)),
                                   /*src1=*/0));
    }
    auto stream = records_to_stream(recs);
    Reader reader(*stream, Variant::Standard);

    auto cc1 = small_l1d();
    cc1.mshr_entries = 2;        // tiny MSHR forces sustained stalls
    cc1.hit_latency  = 50;       // long enough to keep the table busy
    auto cc2 = small_l1d();
    auto memcfg = comparch::cache::MainMemory::Config{200};

    MainMemory mem(memcfg);
    Cache l2(std::move(cc2), "L2");
    auto cc1_owned = std::move(cc1);
    cc1_owned.next_level = &l2;
    Cache l1d(std::move(cc1_owned), "L1d");

    auto pred = comparch::predictor::make(always_taken_cfg());
    OooCore core(narrow_4_4(), *pred, l1d, reader);
    core.run(/*cycle_cap=*/200000);

    REQUIRE(core.eof());
    REQUIRE(core.stats().instructions_retired == static_cast<std::uint64_t>(kN));
    // Loads serialized through a 2-entry MSHR with hit_latency=50 must be
    // far below fetch-width IPC. Sanity bound, not a hand-tuned number.
    REQUIRE(core.stats().ipc() < 1.0);
}
