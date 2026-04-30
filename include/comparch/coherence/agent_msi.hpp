#pragma once

// MSI cache agent. Mirrors project3/coherence/agents/MSI_Agent.{h,cpp}.
// Per-block state machine: I -> IS / IM (transient miss) -> S / M.
// SM is the silent-upgrade transient when a store hits an S-state
// block.

#include "comparch/coherence/agent.hpp"

namespace comparch::coherence {

enum class MsiState { I = 1, S, M, IS, SM, IM };

class MsiAgent : public Agent {
public:
    MsiAgent(NodeId id, Cache* cache, BlockId block);

    void process_proc_request(const Message& req) override;
    void process_ntwk_request(const Message& req) override;

    MsiState state() const { return state_; }

private:
    void do_proc_I(const Message& req);
    void do_proc_S(const Message& req);
    void do_proc_M(const Message& req);
    void do_proc_in_transient(const Message& req);
    void do_ntwk_S(const Message& req);
    void do_ntwk_M(const Message& req);
    void do_ntwk_IS(const Message& req);
    void do_ntwk_IM(const Message& req);
    void do_ntwk_SM(const Message& req);

    MsiState state_;
};

} // namespace comparch::coherence
