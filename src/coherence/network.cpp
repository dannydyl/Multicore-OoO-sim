// Network. Mirrors project3/simulator/network.cpp:5-41. Constructs N
// CPU nodes followed by 1 directory node; tick/tock walk all nodes
// once per cycle.

#include "comparch/coherence/network.hpp"

namespace comparch::coherence {

Network::Network(const Settings& s,
                 CoherenceStats& stats,
                 const std::filesystem::path& trace_dir,
                 AgentFactory agent_factory) {
    cpus_.reserve(s.num_procs);
    caches_.reserve(s.num_procs);
    nodes_.reserve(s.num_procs + 1);

    for (NodeId i = 0; i < s.num_procs; ++i) {
        cpus_.emplace_back(std::make_unique<FiciCpu>(i, trace_dir, s, stats));
        caches_.emplace_back(std::make_unique<Cache>(i, s, stats, agent_factory));
        cpus_.back()->my_cache  = caches_.back().get();
        caches_.back()->my_cpu  = cpus_.back().get();

        nodes_.emplace_back(std::make_unique<Node>(
            i, /*is_dir=*/false,
            cpus_.back().get(), caches_.back().get(),
            /*dir=*/nullptr,
            this));
        caches_.back()->my_node = nodes_.back().get();
    }

    auto dir_ctrl = std::make_unique<DirectoryController>(s.num_procs, s, stats);
    DirectoryController* dir_raw = dir_ctrl.get();
    nodes_.emplace_back(std::make_unique<Node>(
        s.num_procs, /*is_dir=*/true,
        nullptr, nullptr, std::move(dir_ctrl),
        this));
    dir_raw->my_node = nodes_.back().get();
}

Network::Network(const Settings& s,
                 CoherenceStats& stats,
                 std::vector<CpuNode> cpu_nodes,
                 std::unique_ptr<DirectoryController> dir) {
    (void)stats;   // not used directly here; the directory holds its own ref
    nodes_.reserve(cpu_nodes.size() + 1);

    for (NodeId i = 0; i < cpu_nodes.size(); ++i) {
        auto& cn = cpu_nodes[i];
        nodes_.emplace_back(std::make_unique<Node>(
            i, /*is_dir=*/false,
            cn.cpu, cn.cache,
            /*dir=*/nullptr,
            this));
        if (cn.cache) cn.cache->my_node = nodes_.back().get();
    }

    DirectoryController* dir_raw = dir.get();
    nodes_.emplace_back(std::make_unique<Node>(
        s.num_procs, /*is_dir=*/true,
        nullptr, nullptr, std::move(dir),
        this));
    if (dir_raw) dir_raw->my_node = nodes_.back().get();
}

bool Network::is_done() const {
    for (const auto& n : nodes_) {
        if (!n->is_done()) return false;
    }
    return true;
}

void Network::tick(Timestamp clock) {
    for (auto& n : nodes_) n->tick(clock);
}

void Network::tock() {
    for (auto& n : nodes_) n->tock();
}

} // namespace comparch::coherence
