#include "comparch/ooo/trace_logger.hpp"

#include <iomanip>
#include <ios>
#include <sstream>
#include <string>

namespace comparch::ooo {

TraceLogger::TraceLogger(std::ostream& out, std::size_t /*cores*/,
                         std::size_t max_per_core)
    : out_(out), max_per_core_(max_per_core) {}

void TraceLogger::write_header(const std::string& trace_label,
                               const std::string& proto_label) {
    out_ << "# Multicore OoO Simulator -- per-core execution trace\n"
         << "# trace    : " << trace_label << '\n'
         << "# protocol : " << proto_label << '\n'
         << "# window   : first " << max_per_core_
         << " dynamic instructions per core (dyn_count <= "
         << max_per_core_ << ")\n"
         << "# format   : [c<core> cy=<cycle> dyn=<dyn>] <event> ...\n"
         << "#\n"
         << "# Events:\n"
         << "#   LSU      <op> pc=0x... addr=0x...  -> L1 <hit|miss>\n"
         << "#   RETIRE   <op> pc=0x... [addr=0x...] [branch info]\n"
         << "#\n"
         << "# Notes:\n"
         << "#   - LSU events show the cache outcome at issue time. A miss\n"
         << "#     here means the line was not resident; the line will be\n"
         << "#     filled by the time the load/store retires (under the\n"
         << "#     coherence path, that fill happens via DATA arrival).\n"
         << "#   - LSU and RETIRE for the same dyn=N appear separately, so\n"
         << "#     subtracting their cycles gives the observed memory\n"
         << "#     latency for that access.\n"
         << "#\n";
}

bool TraceLogger::active(int /*core_id*/, std::uint64_t dyn_count) const {
    return dyn_count > 0 && dyn_count <= max_per_core_;
}

namespace {

// Print "[c<core> cy=<cycle> dyn=<dyn>]" with stable widths so columns
// line up in the file. Width 8 on cycle covers up to 99M cycles, which
// is well past the 50-instruction window we ever look at.
std::string prefix(int core_id, std::uint64_t cycle, std::uint64_t dyn) {
    std::ostringstream oss;
    oss << "[c" << core_id
        << " cy=" << std::setw(8) << std::setfill('0') << cycle
        << " dyn=" << std::setw(5) << std::setfill('0') << dyn
        << "] ";
    return oss.str();
}

std::string hex(std::uint64_t v) {
    std::ostringstream oss;
    oss << "0x" << std::hex << v;
    return oss.str();
}

} // namespace

void TraceLogger::on_lsu_issue(int core_id, std::uint64_t cycle,
                               std::uint64_t dyn_count, std::uint64_t pc,
                               std::uint64_t addr, bool is_load, bool hit) {
    if (!active(core_id, dyn_count)) return;
    out_ << prefix(core_id, cycle, dyn_count)
         << "LSU      " << (is_load ? "LOAD " : "STORE")
         << " pc=" << hex(pc)
         << " addr=" << hex(addr)
         << "  -> L1 " << (hit ? "hit"  : "miss")
         << '\n';
}

void TraceLogger::on_retire(int core_id, std::uint64_t cycle,
                            std::uint64_t dyn_count, std::uint64_t pc,
                            const char* opcode, std::uint64_t mem_addr,
                            bool is_branch, bool taken,
                            bool predicted_taken, bool mispredict) {
    if (!active(core_id, dyn_count)) return;
    out_ << prefix(core_id, cycle, dyn_count)
         << "RETIRE   ";
    // Pad opcode to 6 chars so the columns line up with LSU events.
    {
        std::ostringstream op;
        op << opcode;
        std::string s = op.str();
        if (s.size() < 6) s.append(6 - s.size(), ' ');
        out_ << s;
    }
    out_ << " pc=" << hex(pc);
    if (mem_addr != 0) {
        out_ << " addr=" << hex(mem_addr);
    }
    if (is_branch) {
        out_ << "  branch=" << (taken ? "T" : "N")
             << " pred="    << (predicted_taken ? "T" : "N");
        if (mispredict) out_ << "  *MISPRED*";
    }
    out_ << '\n';
}

} // namespace comparch::ooo
