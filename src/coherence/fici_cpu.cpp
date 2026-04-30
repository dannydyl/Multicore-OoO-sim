// FICI CPU. Mirrors project3/simulator/cpu.cpp:71-128 (tick/tock) and
// :20-69 (load_instrs).
//
// Block address is the demand-request address with the low
// `block_size_log2` bits cleared — `addr & ~((1<<bs)-1)`.

#include "comparch/coherence/fici_cpu.hpp"

#include "comparch/coherence/coherence_cache.hpp"
#include "comparch/trace.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace comparch::coherence {

std::vector<Instruction> load_proj3_trace(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw trace::TraceError("cannot open trace: " + path.string());
    }
    std::vector<Instruction> out;
    std::string line;
    std::size_t lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        if (line.empty()) continue;            // tolerate trailing blank lines
        std::istringstream iss(line);
        char        op  = 0;
        std::string addr_token;
        iss >> op >> addr_token;
        if (!iss || (op != 'r' && op != 'w') || addr_token.empty()) {
            throw trace::TraceError("bad trace line " + std::to_string(lineno) +
                                    " in " + path.string() + ": '" + line + "'");
        }
        // Accept "0x..." or bare hex.
        std::uint64_t addr = 0;
        try {
            addr = std::stoull(addr_token, nullptr, 16);
        } catch (const std::exception&) {
            throw trace::TraceError("bad trace line " + std::to_string(lineno) +
                                    " in " + path.string() +
                                    ": unparseable address '" + addr_token + "'");
        }
        out.push_back(Instruction{op, addr});
    }
    return out;
}

FiciCpu::FiciCpu(NodeId id,
                 const std::filesystem::path& trace_dir,
                 const Settings& s,
                 CoherenceStats& stats)
    : id_(id), settings_(s), stats_(stats) {
    instrs_     = load_proj3_trace(trace_dir / ("p" + std::to_string(id) + ".trace"));
    trace_done_ = instrs_.empty();
}

void FiciCpu::tick() {
    if (cache_in) {
        // Response received -> outstanding cleared. Cache deletes its own
        // copies; we own this one (Node moved it from network in tock).
        delete cache_in;
        cache_in    = nullptr;
        outstanding_ = false;
    }
    if (trace_done_ || outstanding_) return;

    const Instruction inst = instrs_[curip_];
    const BlockId block = inst.address &
        (~((static_cast<std::uint64_t>(1) << settings_.block_size_log2) - 1ULL));
    const MessageKind kind =
        (inst.action == 'r') ? MessageKind::LOAD : MessageKind::STORE;

    auto* req = new Message(id_, id_, block, kind, settings_);
    my_cache->cpu_in_next = req;
    outstanding_ = true;
    ++stats_.cache_accesses;

    ++curip_;
    if (curip_ == instrs_.size()) trace_done_ = true;
}

void FiciCpu::tock() {
    if (cache_in_next) {
        cache_in      = cache_in_next;
        cache_in_next = nullptr;
    }
}

bool FiciCpu::is_done() const {
    return trace_done_ && !outstanding_;
}

} // namespace comparch::coherence
