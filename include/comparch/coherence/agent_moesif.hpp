#pragma once

// MOESIF cache agent. Mirrors project3/coherence/agents/MOESIF_Agent.{h,cpp}.
// Six stable states: I, S, E, M, O, F (Forwarder = the read-shared
// dirty-block forwarder, distinct from Owner).
//
// Transient sets: IS / IM (cold miss), SM (silent upgrade from S),
// EM (E -> M via GETX), OM (O -> M), FM (F -> M).

#include "comparch/coherence/agent.hpp"

namespace comparch::coherence {

enum class MoesifState {
    I = 1, S, E, M, O, F,
    IS, IM, SM, EM, FM, OM
};

class MoesifAgent : public Agent {
public:
    MoesifAgent(NodeId id, Cache* cache, BlockId block);

    void process_proc_request(const Message& req) override;
    void process_ntwk_request(const Message& req) override;

    MoesifState state() const { return state_; }

private:
    void do_proc_I(const Message& req);
    void do_proc_S(const Message& req);
    void do_proc_E(const Message& req);
    void do_proc_M(const Message& req);
    void do_proc_F(const Message& req);
    void do_proc_O(const Message& req);
    void do_proc_in_transient(const Message& req);
    void do_ntwk_S(const Message& req);
    void do_ntwk_E(const Message& req);
    void do_ntwk_M(const Message& req);
    void do_ntwk_F(const Message& req);
    void do_ntwk_O(const Message& req);
    void do_ntwk_IS(const Message& req);
    void do_ntwk_IM(const Message& req);
    void do_ntwk_SM(const Message& req);
    void do_ntwk_EM(const Message& req);
    void do_ntwk_FM(const Message& req);
    void do_ntwk_OM(const Message& req);

    MoesifState state_;
};

} // namespace comparch::coherence
