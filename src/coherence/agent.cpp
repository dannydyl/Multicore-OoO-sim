#include "comparch/coherence/agent.hpp"

#include "comparch/coherence/coherence_cache.hpp"
#include "comparch/coherence/fici_cpu.hpp"
#include "comparch/coherence/node.hpp"

namespace comparch::coherence {

Agent::Agent(NodeId id, Cache* cache, BlockId block)
    : id_(id), cache_(cache), block_(block) {}

namespace {

// All network-bound agent helpers funnel here. Address the directory
// (settings.num_procs is its NodeId), allocate a Message with the
// per-Settings flit count, and push into the local node's egress queue.
Message* push_to_dir(Cache& cache, NodeId src, BlockId block,
                     MessageKind kind) {
    const auto& s = cache.settings();
    auto* req = new Message(src, s.num_procs, block, kind, s);
    cache.my_node->ntwk_out_next.push_back(req);
    return req;
}

} // namespace

void Agent::send_GETS(BlockId block) {
    push_to_dir(*cache_, id_, block, MessageKind::GETS);
}
void Agent::send_GETM(BlockId block) {
    push_to_dir(*cache_, id_, block, MessageKind::GETM);
}
void Agent::send_GETX(BlockId block) {
    push_to_dir(*cache_, id_, block, MessageKind::GETX);
}
void Agent::send_INVACK(BlockId block) {
    push_to_dir(*cache_, id_, block, MessageKind::INVACK);
}
void Agent::send_DATA_dir(BlockId block) {
    push_to_dir(*cache_, id_, block, MessageKind::DATA);
}

void Agent::send_DATA_proc(BlockId block) {
    // Project3 send_DATA_proc: short-circuit straight into the CPU's
    // cache_in_next slot. No network traversal — the data is already
    // resident in this cache.
    const auto& s = cache_->settings();
    auto* data = new Message(id_, id_, block, MessageKind::DATA, s);
    cache_->my_cpu->cache_in_next = data;
}

} // namespace comparch::coherence
