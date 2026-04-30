#pragma once

// MESI cache agent. Mirrors project3/coherence/agents/MESI_Agent.{h,cpp}.
// Adds the E state to MSI: Exclusive (clean, single copy) silently
// upgrades to M via GETX without going through the directory's
// invalidation broadcast.

#include "comparch/coherence/agent.hpp"

namespace comparch::coherence {

enum class MesiState { I = 1, S, E, M, IS, IM, SM, EM };

class MesiAgent : public Agent {
public:
    MesiAgent(NodeId id, Cache* cache, BlockId block);

    void process_proc_request(const Message& req) override;
    void process_ntwk_request(const Message& req) override;

    MesiState state() const { return state_; }

private:
    void do_proc_I(const Message& req);
    void do_proc_S(const Message& req);
    void do_proc_E(const Message& req);
    void do_proc_M(const Message& req);
    void do_proc_in_transient(const Message& req);
    void do_ntwk_S(const Message& req);
    void do_ntwk_E(const Message& req);
    void do_ntwk_M(const Message& req);
    void do_ntwk_IS(const Message& req);
    void do_ntwk_IM(const Message& req);
    void do_ntwk_SM(const Message& req);
    void do_ntwk_EM(const Message& req);

    MesiState state_;
};

} // namespace comparch::coherence
