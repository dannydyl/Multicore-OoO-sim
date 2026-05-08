#pragma once

// TraceLogger — per-instruction execution trace for the first N
// instructions of every core. Activated by the LOG=1 environment
// variable (or --log-trace CLI flag). Output goes to log.rpt in the
// run directory; one line per event, prefixed by core id and cycle.
//
// Two event kinds are emitted:
//   - LSU issue: when a load/store enters the L1 MSHR. Includes the
//     hit/miss outcome the cache reported synchronously during issue,
//     so the user can see how the cache behaves on the early access
//     stream.
//   - Retire:    when an instruction commits at the head of the ROB.
//     Includes opcode, PC, mem_addr (for loads/stores), and branch
//     outcome (predicted vs. actual).
//
// Each core has a fixed budget (default 50). Once the budget runs out
// for a core, further events from that core are dropped silently.
// The budget gate is gated by `dyn_count <= max_dyn_`, so events for
// the same instruction (issue + retire) both make it through as long
// as the instruction is in the first N of its core.

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <vector>

namespace comparch::ooo {

class TraceLogger {
public:
    // `out` is the stream the logger writes to (the full-mode driver
    // owns it as an ofstream pointing at log.rpt). `cores` is the
    // number of cores we'll be receiving events from; used only for
    // the optional active-cores summary in the header. `max_per_core`
    // sets the dyn_count cutoff.
    TraceLogger(std::ostream& out, std::size_t cores,
                std::size_t max_per_core = 50);

    // Emit a one-time banner at the top of log.rpt explaining the
    // format. Caller invokes this exactly once after construction.
    void write_header(const std::string& trace_label,
                      const std::string& proto_label);

    // Cheap predicate the OoO core can check before allocating a
    // formatted line. Returns true iff dyn_count is within the
    // logging window for this core.
    bool active(int core_id, std::uint64_t dyn_count) const;

    // LSU issue event. `hit` is the hit/miss outcome reported by
    // Cache::access() (synchronous, recorded inside Cache::issue()).
    void on_lsu_issue(int core_id, std::uint64_t cycle,
                      std::uint64_t dyn_count, std::uint64_t pc,
                      std::uint64_t addr, bool is_load, bool hit);

    // Retire event. `mem_addr` is 0 for non-memory instructions.
    // Branch fields are unused unless `is_branch=true`.
    void on_retire(int core_id, std::uint64_t cycle,
                   std::uint64_t dyn_count, std::uint64_t pc,
                   const char* opcode, std::uint64_t mem_addr,
                   bool is_branch, bool taken,
                   bool predicted_taken, bool mispredict);

private:
    std::ostream& out_;
    std::size_t   max_per_core_;
};

} // namespace comparch::ooo
