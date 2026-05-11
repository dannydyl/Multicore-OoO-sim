#pragma once

// Per-node coherence cache: an unbounded state table (block-keyed
// unordered_map of agent state) — no eviction, no LRU, no MSHR, no
// hit latency. Acts as the per-block protocol state machine; the
// finite cache::Cache is layered above it (one per core, wired
// through CoherenceAdapter).
//
// Mirrors project3/simulator/cache.h: holds two single-slot incoming
// buffers (cpu_in_next/cpu_in for the CPU side, ntwk_in_next/ntwk_in
// for the network side) plus a lazily-allocated map of per-block
// Agent instances. The protocol-specific agent factory is injected via
// the constructor so this class stays protocol-agnostic.

#include <functional>
#include <memory>
#include <unordered_map>

#include "comparch/coherence/agent.hpp"
#include "comparch/coherence/coherence_stats.hpp"
#include "comparch/coherence/cpu_port.hpp"
#include "comparch/coherence/message.hpp"
#include "comparch/coherence/settings.hpp"
#include "comparch/coherence/types.hpp"

namespace comparch::coherence {

class Node;

// Returns a freshly-constructed Agent for (block, this cache). Set by
// the factory at Network construction; null for the directory node.
using AgentFactory =
    std::function<std::unique_ptr<Agent>(NodeId id, Cache* cache, BlockId block)>;

class Cache {
public:
    Cache(NodeId id,
          const Settings& s,
          CoherenceStats& stats,
          AgentFactory factory);
    ~Cache();

    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;

    NodeId id() const { return id_; }

    // Messaging surface (single-slot incoming buffers per direction).
    Message* cpu_in_next  = nullptr;
    Message* cpu_in       = nullptr;
    Message* ntwk_in_next = nullptr;
    Message* ntwk_in      = nullptr;

    CpuPort* my_cpu  = nullptr;   // FiciCpu (5A) or CoherenceAdapter (5B)
    Node*    my_node = nullptr;

    void tick();
    void tock();

    // Lazy-allocate the per-block agent on first access.
    Agent* get_agent(BlockId block);

    // Read-only knob and stats access for agents.
    const Settings&        settings() const { return settings_; }
    CoherenceStats&        stats()          { return stats_; }
    const CoherenceStats&  stats()    const { return stats_; }

private:
    NodeId           id_;
    const Settings&  settings_;
    CoherenceStats&  stats_;
    AgentFactory     factory_;
    std::unordered_map<BlockId, std::unique_ptr<Agent>> agents_;
};

} // namespace comparch::coherence
