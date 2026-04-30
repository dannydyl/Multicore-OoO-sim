#pragma once

// MOSI cache agent. Mirrors project3/coherence/agents/MOSI_Agent.{h,cpp}.
// Adds the O (Owner) state: dirty shared, the owner forwards on read
// requests instead of writing back. M->O on RECALL_GOTO_S; O->M via OM
// on a store. No silent upgrades.

#include "comparch/coherence/agent.hpp"

namespace comparch::coherence {

enum class MosiState { I = 1, S, M, O, IS, SM, IM, OM };

class MosiAgent : public Agent {
public:
    MosiAgent(NodeId id, Cache* cache, BlockId block);

    void process_proc_request(const Message& req) override;
    void process_ntwk_request(const Message& req) override;

    MosiState state() const { return state_; }

private:
    void do_proc_I(const Message& req);
    void do_proc_S(const Message& req);
    void do_proc_M(const Message& req);
    void do_proc_O(const Message& req);
    void do_proc_in_transient(const Message& req);
    void do_ntwk_S(const Message& req);
    void do_ntwk_M(const Message& req);
    void do_ntwk_O(const Message& req);
    void do_ntwk_IS(const Message& req);
    void do_ntwk_IM(const Message& req);
    void do_ntwk_SM(const Message& req);
    void do_ntwk_OM(const Message& req);

    MosiState state_;
};

} // namespace comparch::coherence
