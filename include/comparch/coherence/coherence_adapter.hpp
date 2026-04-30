#pragma once

// Per-core shim that splices a finite cache::Cache (Phase 2) into the
// coherence ring (Phase 5A). One adapter per core; the L2 cache holds
// it as `cfg.coherence_sink`, the ring node holds it as `CpuPort*`.
//
// Responsibilities:
//   - on_miss   (CoherenceSink, fired by L2 on a tag miss): translate
//     into a LOAD/STORE Message, push into coh_cache->cpu_in_next, the
//     agent will GETS/GETM to the directory.
//   - on_evict  (CoherenceSink, fired by L1 or L2 on LRU eviction):
//     push a DATA_WB message to the directory so it can drop presence
//     and (for dirty M/O/F holders) write to memory.
//   - tick      (CpuPort, fired by Node::tick): if the agent dropped a
//     DATA into cache_in_next, treat it as the response to whatever
//     request is outstanding for that block — fill L2, fill L1, mark
//     all matching MSHR entries ready.
//   - on_ntwk_event (CpuPort, fired by coherence::Cache after the
//     agent processed a network request): on REQ_INVALID /
//     RECALL_GOTO_I, drop the line from L1 and L2; on RECALL_GOTO_S,
//     leave resident but clear the dirty bit.

#include <deque>
#include <memory>
#include <vector>

#include "comparch/cache/cache.hpp"
#include "comparch/cache/coherence_sink.hpp"
#include "comparch/coherence/coherence_cache.hpp"
#include "comparch/coherence/coherence_stats.hpp"
#include "comparch/coherence/cpu_port.hpp"
#include "comparch/coherence/message.hpp"
#include "comparch/coherence/settings.hpp"
#include "comparch/coherence/types.hpp"

namespace comparch::coherence {

class CoherenceAdapter : public cache::CoherenceSink, public CpuPort {
public:
    // l1d and l2d are non-owning. The caller (run_full_mode) outlives
    // the adapter and is responsible for wiring `l2d.cfg.coherence_sink`
    // to point here AFTER construction.
    CoherenceAdapter(NodeId id,
                     const Settings& s,
                     CoherenceStats& stats,
                     AgentFactory factory,
                     cache::Cache& l1d,
                     cache::Cache& l2d);
    ~CoherenceAdapter();

    NodeId id() const { return id_; }

    // The wrapped Phase 5A coherence cache (per-block agent state map).
    // Owned by the adapter; passed to the Network as the `cache` half
    // of CpuNode in run_full_mode.
    Cache* coh_cache() { return coh_cache_.get(); }

    // === cache::CoherenceSink ===
    void on_miss(std::uint64_t block_addr, cache::Op op) override;
    void on_evict(std::uint64_t block_addr, bool dirty) override;

    // === CpuPort ===
    void tick() override;
    void tock() override;
    bool is_done() const override;
    void on_ntwk_event(const Message& req) override;

private:
    NodeId           id_;
    const Settings&  settings_;
    cache::Cache*    l1d_;     // non-owning
    cache::Cache*    l2d_;     // non-owning
    std::unique_ptr<Cache> coh_cache_;

    // Pending outgoing-LOAD/STORE messages. Multiple LSU FUs can miss
    // in the same cycle but coh_cache->cpu_in_next can only hold one
    // per tick — drain at most one per cycle from this queue. Owns the
    // pointers (deletes any leftover at destruction).
    std::deque<Message*> outbound_proc_;
};

} // namespace comparch::coherence
