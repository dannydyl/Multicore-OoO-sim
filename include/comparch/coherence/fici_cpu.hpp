#pragma once

// Single-outstanding-request, FICI-style trace driver. Mirrors
// project3/simulator/cpu.h. Issues one memory request per cycle when
// not blocked on an outstanding response, then quiesces when the
// per-core trace is exhausted.
//
// Trace format (one record per line):
//     r 0x<hex-addr>   |   w 0x<hex-addr>
// Other characters / blank lines / comments are rejected — project3's
// fscanf("%c %lx") is strict about format.

#include <cstdint>
#include <filesystem>
#include <vector>

#include "comparch/coherence/coherence_stats.hpp"
#include "comparch/coherence/cpu_port.hpp"
#include "comparch/coherence/message.hpp"
#include "comparch/coherence/settings.hpp"
#include "comparch/coherence/types.hpp"

namespace comparch::coherence {

class Cache;

struct Instruction {
    char        action  = 'r';
    std::uint64_t address = 0;
};

class FiciCpu : public CpuPort {
public:
    // Loads `<trace_dir>/p<id>.trace` immediately, throws TraceError on
    // missing / unparseable input.
    FiciCpu(NodeId id,
            const std::filesystem::path& trace_dir,
            const Settings& s,
            CoherenceStats& stats);

    NodeId id() const { return id_; }

    Cache* my_cache = nullptr;        // attached by Node

    // cache_in_next is inherited from CpuPort. cache_in is the
    // already-arrived response; FiciCpu reads and clears it in tick().
    Message* cache_in      = nullptr; // shifted from *_next on tock

    void tick() override;
    void tock() override;
    bool is_done() const override;     // exhausted + no outstanding request

private:
    NodeId           id_;
    const Settings&  settings_;
    CoherenceStats&  stats_;

    std::vector<Instruction> instrs_;
    std::size_t curip_ = 0;
    bool   outstanding_ = false;
    bool   trace_done_  = false;
};

// Exposed for unit testing.
std::vector<Instruction> load_proj3_trace(const std::filesystem::path& path);

} // namespace comparch::coherence
