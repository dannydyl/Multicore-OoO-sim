// Cache::tick / tock mirror project3/simulator/cache.cpp:17-46. Two
// single-slot incoming buffers (cpu and network) drained on tick;
// shifted from *_next to * on tock so the producer/consumer race
// stays half-cycle ordered.

#include "comparch/coherence/coherence_cache.hpp"

#include <utility>

namespace comparch::coherence {

Cache::Cache(NodeId id,
             const Settings& s,
             CoherenceStats& stats,
             AgentFactory factory)
    : id_(id), settings_(s), stats_(stats), factory_(std::move(factory)) {}

Cache::~Cache() {
    delete cpu_in_next;
    delete cpu_in;
    delete ntwk_in_next;
    delete ntwk_in;
}

void Cache::tick() {
    if (cpu_in) {
        Agent* a = get_agent(cpu_in->block);
        if (a) a->process_proc_request(*cpu_in);
        delete cpu_in;
        cpu_in = nullptr;
    }
    if (ntwk_in) {
        Agent* a = get_agent(ntwk_in->block);
        if (a) a->process_ntwk_request(*ntwk_in);
        // Notify the CPU-side hook so a CoherenceAdapter can drop the
        // line from its underlying finite cache on REQ_INVALID /
        // RECALL_GOTO_*. FiciCpu's default override is a no-op.
        if (my_cpu) my_cpu->on_ntwk_event(*ntwk_in);
        delete ntwk_in;
        ntwk_in = nullptr;
    }
}

void Cache::tock() {
    if (cpu_in_next) {
        cpu_in       = cpu_in_next;
        cpu_in_next  = nullptr;
    }
    if (ntwk_in_next) {
        ntwk_in      = ntwk_in_next;
        ntwk_in_next = nullptr;
    }
}

Agent* Cache::get_agent(BlockId block) {
    auto it = agents_.find(block);
    if (it != agents_.end()) return it->second.get();
    if (!factory_) return nullptr;
    auto agent = factory_(id_, this, block);
    Agent* raw = agent.get();
    agents_.emplace(block, std::move(agent));
    return raw;
}

} // namespace comparch::coherence
