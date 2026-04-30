#pragma once

// MI cache agent. Mirrors project3/coherence/agents/MI_Agent.{h,cpp}.
// Two stable states (I, M) plus the IM transient pending DATA.
// The simplest of the five protocols and the only one without a
// project3 reference output, so coverage relies on synthetic tests.

#include "comparch/coherence/agent.hpp"

namespace comparch::coherence {

enum class MiState { I = 1, M, IM };

class MiAgent : public Agent {
public:
    MiAgent(NodeId id, Cache* cache, BlockId block);

    void process_proc_request(const Message& req) override;
    void process_ntwk_request(const Message& req) override;

    MiState state() const { return state_; }

private:
    void do_proc_I(const Message& req);
    void do_proc_M(const Message& req);
    void do_proc_in_transient(const Message& req);
    void do_ntwk_M(const Message& req);
    void do_ntwk_IM(const Message& req);

    MiState state_;
};

} // namespace comparch::coherence
