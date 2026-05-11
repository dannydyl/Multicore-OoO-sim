#pragma once

// OooCore — single-core out-of-order pipeline.
// =============================================
// Reverse-stage cycle: retire -> exec -> schedule -> dispatch -> fetch.
// Drives a real branch predictor and a real L1-D cache via
// Cache::issue / peek / complete (MSHR-aware async path).
//
// Known limitations (documented, not bugs):
//   - Idealized I-cache (every fetch is 1 cycle). An I-cache MSHR
//     could be added later.
//   - Predictor uses synchronous predict(at-fetch) / update(at-retire).
//     No speculative checkpoint — predict() is read-only across all
//     four predictors, so wrong-path branches never reach retire and
//     never train the predictor.
//   - Stores complete via the same MSHR path as loads under
//     coherence; no separate store buffer / TSO model.
//   - MUL opcode is only emitted for CasimV2 records with the
//     is_mul hint set. ChampSim traces (no opcode class) never
//     produce MUL, so the MUL FUs go unused under ChampSim-only
//     workloads.

#include <cstddef>
#include <cstdint>
#include <list>
#include <tuple>
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

class TraceLogger;

struct OooConfig {
    std::size_t fetch_width             = 4;
    std::size_t rob_entries             = 96;
    std::size_t schedq_entries_per_fu   = 2;
    std::size_t dispq_capacity          = 32;
    std::size_t alu_fus                 = 3;
    std::size_t mul_fus                 = 2;
    std::size_t lsu_fus                 = 2;
    // mul_stages, alu_stages, lsu_stages are fixed at 3 / 1 / 1 to match
    // project2's pinned constants. We don't expose them as knobs yet.

    // Watchdog: abort tick() if pipeline progress (retired / fetched /
    // rob+sq+dispq sizes) is unchanged for this many consecutive cycles.
    // 0 disables the watchdog.
    std::uint64_t deadlock_threshold_cycles = 100000;
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
    bool          deadlocked              = false;
    std::uint64_t stall_cycles_at_abort   = 0;

    // ---- Utilization breakdown ---------------------------------
    // Headline: useful_retire_cycles / cycles is the fraction of
    // wall-clock cycles where at least one instruction committed.
    // The fetch-* and retire-* counters split the remaining "no
    // progress" cycles into causes so the user can see whether
    // the pipeline is sync-bound, memory-bound, frontend-bound, etc.
    //
    // None of these are mutually exclusive with no_fire_cycles —
    // they slice the same cycles by a different axis.
    std::uint64_t useful_retire_cycles    = 0;  // \\geq 1 retire this cycle
    // Fetch-side stall reasons (counted exactly once per cycle
    // when stage_fetch fails to add any instruction to dispq):
    std::uint64_t fetch_stall_sync        = 0;  // SyncCoordinator rejected
    std::uint64_t fetch_stall_dispq_full  = 0;  // dispq at capacity
    std::uint64_t fetch_stall_mispred     = 0;  // in_mispred recovery
    std::uint64_t fetch_stall_eof         = 0;  // trace exhausted
    // Retire-side stall reasons (counted exactly once per cycle
    // when stage_state_update retires 0 instructions):
    std::uint64_t retire_stall_rob_empty  = 0;  // nothing to retire
    std::uint64_t retire_stall_head_busy  = 0;  // head not ready (memory etc.)
    // Per-FU "busy this cycle" sums. A cycle counted once per
    // busy FU, so alu_busy_sum / (cycles * alu_fus) is per-ALU util.
    std::uint64_t alu_busy_sum            = 0;
    std::uint64_t mul_busy_sum            = 0;
    std::uint64_t lsu_busy_sum            = 0;

    double ipc()        const { return cycles ? static_cast<double>(instructions_retired) / static_cast<double>(cycles) : 0.0; }
    double dispq_avg()  const { return cycles ? dispq_avg_sum  / static_cast<double>(cycles) : 0.0; }
    double schedq_avg() const { return cycles ? schedq_avg_sum / static_cast<double>(cycles) : 0.0; }
    double rob_avg()    const { return cycles ? rob_avg_sum    / static_cast<double>(cycles) : 0.0; }
    double useful_pct() const { return cycles ? 100.0 * static_cast<double>(useful_retire_cycles) / static_cast<double>(cycles) : 0.0; }
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

    // LOG=1 wiring. Default is no logger; full_mode attaches one
    // when LOG is enabled. core_id is used only as a label in the
    // output; the core itself doesn't otherwise need to know its ID.
    void set_trace_logger(TraceLogger* logger, int core_id) {
        trace_logger_ = logger;
        core_id_      = core_id;
    }

    // Software TID this core is currently running. Stamped onto
    // every MemReq the LSU issues. Defaults to 0; full_mode sets it
    // = core_id at construction (1:1 fixed mapping). When
    // ThreadScheduler lands, it will call this on context switch.
    void set_active_tid(std::uint32_t tid) { active_tid_ = tid; }
    std::uint32_t active_tid() const { return active_tid_; }

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

    // Deadlock watchdog. last_progress_sig_ snapshots the per-cycle
    // progress vector (retired, fetched, rob_size, sq_size, dispq_size);
    // if it stays equal for cfg_.deadlock_threshold_cycles cycles in a
    // row, tick() aborts and sets stats_.deadlocked.
    using ProgressSig = std::tuple<std::uint64_t, std::uint64_t,
                                   std::size_t, std::size_t, std::size_t>;
    ProgressSig   last_progress_sig_{};
    bool          last_progress_sig_valid_ = false;
    std::uint64_t stall_cycles_            = 0;

    OooStats stats_;

    // LOG=1 hooks. Both default to "off"; full_mode wires them up
    // post-construction via set_trace_logger().
    TraceLogger*  trace_logger_ = nullptr;
    int           core_id_      = 0;
    std::uint32_t active_tid_   = 0;
};

} // namespace comparch::ooo
