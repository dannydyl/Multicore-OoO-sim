#pragma once

// OooCore — single-core out-of-order pipeline.
// =============================================
// Reverse-stage cycle: retire -> exec -> schedule -> dispatch -> fetch.
// Direct port of project2's procsim.cpp do_cycle (lines 948-1000), now
// driving a real predictor (Phase 3 BranchPredictor) and a real L1-D
// cache via Cache::issue / peek / complete (Phase 4 MSHR API).
//
// Phase 4 simplifications relative to project2:
//   - Idealized I-cache (every fetch is 1 cycle). project2's
//     `misses_enabled=false` mode worked the same way; an I-cache MSHR
//     can be plugged in later.
//   - Predictor uses synchronous predict(at-fetch) / update(at-retire).
//     No speculative checkpoint (predict() is read-only across all four
//     predictors, so wrong-path branches simply never reach retire and
//     never train the predictor).
//   - Stores complete via Cache::access() (synchronous round-trip);
//     no store buffer.
//   - MUL opcode never produced by the ChampSim classifier; MUL FUs
//     sit unused unless a PC-bin heuristic is added later.

#include <cstddef>
#include <cstdint>
#include <list>
#include <vector>

#include "comparch/cache/cache.hpp"
#include "comparch/ooo/fu.hpp"
#include "comparch/ooo/inst.hpp"
#include "comparch/ooo/rat.hpp"
#include "comparch/ooo/rob.hpp"
#include "comparch/ooo/schedq.hpp"
#include "comparch/predictor/predictor.hpp"
#include "comparch/trace.hpp"

namespace comparch::ooo {

struct OooConfig {
    std::size_t fetch_width             = 4;
    std::size_t rob_entries             = 96;
    std::size_t schedq_entries_per_fu   = 2;
    std::size_t alu_fus                 = 3;
    std::size_t mul_fus                 = 2;
    std::size_t lsu_fus                 = 2;
    // mul_stages, alu_stages, lsu_stages are fixed at 3 / 1 / 1 to match
    // project2's pinned constants. We don't expose them as knobs yet.
};

struct OooStats {
    std::uint64_t cycles                  = 0;
    std::uint64_t instructions_fetched    = 0;
    std::uint64_t instructions_retired    = 0;
    std::uint64_t num_branch_instructions = 0;
    std::uint64_t branch_mispredictions   = 0;
    std::uint64_t no_fire_cycles          = 0;
    std::uint64_t rob_no_dispatch_cycles  = 0;
    std::uint64_t dispq_max               = 0;
    std::uint64_t schedq_max              = 0;
    std::uint64_t rob_max                 = 0;
    double        dispq_avg_sum           = 0.0;
    double        schedq_avg_sum          = 0.0;
    double        rob_avg_sum             = 0.0;

    double ipc()        const { return cycles ? static_cast<double>(instructions_retired) / static_cast<double>(cycles) : 0.0; }
    double dispq_avg()  const { return cycles ? dispq_avg_sum  / static_cast<double>(cycles) : 0.0; }
    double schedq_avg() const { return cycles ? schedq_avg_sum / static_cast<double>(cycles) : 0.0; }
    double rob_avg()    const { return cycles ? rob_avg_sum    / static_cast<double>(cycles) : 0.0; }
};

class OooCore {
public:
    OooCore(OooConfig cfg,
            predictor::BranchPredictor& pred,
            cache::Cache& l1d,
            trace::Reader& trace);

    // One pipeline cycle. Returns true while there is still work to do
    // (trace not yet exhausted, or instructions still in flight). When
    // it returns false the simulator can stop ticking this core.
    bool tick();

    // Convenience: tick until the pipeline drains. Bails after `cycle_cap`
    // to avoid spinning on a deadlock during development. cycle_cap=0
    // means no cap.
    void run(std::uint64_t cycle_cap = 0);

    const OooStats& stats() const { return stats_; }

    // Test introspection.
    std::size_t dispq_size() const { return dispq_.size(); }
    std::size_t schedq_size() const { return sq_.size(); }
    std::size_t rob_size()    const { return rob_.size(); }
    bool        eof()         const { return eof_; }
    bool        in_mispred()  const { return in_mispred_; }

private:
    // Stages — all return void except retire, which reports a mispredict
    // event so the do-cycle wrapper can stop the rest of the cycle's
    // stages (matching project2's procsim_do_cycle).
    std::uint64_t stage_state_update(bool& mispred_retired_out);
    void          stage_exec();
    void          stage_schedule();
    void          stage_dispatch();
    void          stage_fetch();

    // Helper: when an FU finishes an instruction, broadcast on the CDB
    // (wake schedQ dependents, mark RAT + ROB ready, erase from schedQ).
    void writeback(SchedEntry* sched);

    // Inputs.
    OooConfig                   cfg_;
    predictor::BranchPredictor* pred_;
    cache::Cache*               l1d_;
    trace::Reader*              trace_;

    // Pipeline state.
    std::list<Inst>     dispq_;        // dispatch queue (project2's dispatchQueue)
    SchedQ              sq_;           // scheduling queue (project2's scheduleQueue)
    Rob                 rob_;
    Rat                 rat_;
    std::vector<AluUnit> alu_;
    std::vector<MulUnit> mul_;
    std::vector<LsuUnit> lsu_;
    std::size_t          schedq_capacity_;

    // Fetch + global state.
    bool          eof_         = false;   // trace exhausted
    bool          in_mispred_  = false;   // mispred fetched, awaiting retire
    std::uint64_t dyn_count_   = 0;       // monotonic instruction sequence number

    OooStats stats_;
};

} // namespace comparch::ooo
