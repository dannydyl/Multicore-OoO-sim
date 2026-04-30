#pragma once

// Network. Mirrors project3/simulator/network.h. Owns N CPU nodes
// (IDs 0..N-1) plus one directory node (ID N), plus the per-CPU
// FiciCpu and Cache instances. Per-cycle tick/tock walks every node.
//
// The factory closure injected at construction is what makes a
// concrete coherence protocol "live" — Step 4 wires it to the MSI
// agent factory; Steps 5-8 do the same for the other protocols.

#include <filesystem>
#include <memory>
#include <vector>

#include "comparch/coherence/coherence_cache.hpp"
#include "comparch/coherence/coherence_stats.hpp"
#include "comparch/coherence/fici_cpu.hpp"
#include "comparch/coherence/node.hpp"
#include "comparch/coherence/settings.hpp"
#include "comparch/coherence/types.hpp"

namespace comparch::coherence {

class Network {
public:
    // Phase 5A constructor: builds N FiciCpu + N coherence::Cache nodes
    // from a per-core text trace directory.
    Network(const Settings& s,
            CoherenceStats& stats,
            const std::filesystem::path& trace_dir,
            AgentFactory agent_factory);

    // Phase 5B constructor: caller has already built the per-node
    // (CpuPort, coherence::Cache) pairs (e.g. CoherenceAdapter wrapping
    // an OoO core) and the DirectoryController. Network just stitches
    // them into the ring. Per-node objects must outlive the Network;
    // the Network owns only the Nodes themselves and the directory
    // controller it was given.
    struct CpuNode {
        CpuPort* cpu;
        Cache*   cache;
    };
    Network(const Settings& s,
            CoherenceStats& stats,
            std::vector<CpuNode> cpu_nodes,
            std::unique_ptr<DirectoryController> dir);

    ~Network() = default;

    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    std::uint64_t num_nodes() const { return nodes_.size(); }
    Node&         node(NodeId i)    { return *nodes_[i]; }

    bool is_done() const;
    void tick(Timestamp clock);
    void tock();

private:
    // Phase 5A path owns FiciCpu / coherence::Cache instances; Phase 5B
    // path leaves these empty (the caller owns them).
    std::vector<std::unique_ptr<FiciCpu>> cpus_;
    std::vector<std::unique_ptr<Cache>>   caches_;
    std::vector<std::unique_ptr<Node>>    nodes_;
};

} // namespace comparch::coherence
