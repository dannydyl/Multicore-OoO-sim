// MSI agent. Mirrors project3/coherence/agents/MSI_Agent.cpp line-by-line.
// State transitions and stat-counter increments are pinned to the legacy
// behavior; the proj3 regression test diffs cycle counts and counters
// against ref_outs/MSI_core_<N>.out.

#include "comparch/coherence/agent_msi.hpp"

#include <stdexcept>
#include <string>

#include "comparch/coherence/coherence_cache.hpp"

namespace comparch::coherence {

namespace {

[[noreturn]] void bad_msg(const char* state, const char* side,
                          MessageKind k) {
    throw std::runtime_error(
        std::string("MSI: ") + state + " state shouldn't see " + side +
        " message: " + message_kind_str(k));
}

} // namespace

MsiAgent::MsiAgent(NodeId id, Cache* cache, BlockId block)
    : Agent(id, cache, block), state_(MsiState::I) {}

void MsiAgent::process_proc_request(const Message& req) {
    switch (state_) {
        case MsiState::I:  do_proc_I(req); break;
        case MsiState::S:  do_proc_S(req); break;
        case MsiState::M:  do_proc_M(req); break;
        case MsiState::IS:
        case MsiState::IM:
        case MsiState::SM: do_proc_in_transient(req); break;
    }
}

void MsiAgent::process_ntwk_request(const Message& req) {
    switch (state_) {
        case MsiState::I:  bad_msg("I",  "ntwk", req.kind);
        case MsiState::S:  do_ntwk_S(req);  break;
        case MsiState::M:  do_ntwk_M(req);  break;
        case MsiState::IS: do_ntwk_IS(req); break;
        case MsiState::IM: do_ntwk_IM(req); break;
        case MsiState::SM: do_ntwk_SM(req); break;
    }
}

void MsiAgent::do_proc_I(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:
            send_GETS(req.block);
            state_ = MsiState::IS;
            ++cache_->stats().cache_misses;
            break;
        case MessageKind::STORE:
            send_GETM(req.block);
            state_ = MsiState::IM;
            ++cache_->stats().cache_misses;
            break;
        default:
            bad_msg("I", "proc", req.kind);
    }
}

void MsiAgent::do_proc_S(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:
            send_DATA_proc(req.block);
            break;
        case MessageKind::STORE:
            send_GETM(req.block);
            state_ = MsiState::SM;
            break;
        default:
            bad_msg("S", "proc", req.kind);
    }
}

void MsiAgent::do_proc_M(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:
        case MessageKind::STORE:
            send_DATA_proc(req.block);
            break;
        default:
            bad_msg("M", "proc", req.kind);
    }
}

void MsiAgent::do_proc_in_transient(const Message& req) {
    // Project3 throws fatal_error here — only one outstanding request per
    // CPU is allowed, and a transient state means the CPU is parked.
    (void)req;
    throw std::runtime_error(
        "MSI: transient state should not see another proc request");
}

void MsiAgent::do_ntwk_S(const Message& req) {
    switch (req.kind) {
        case MessageKind::REQ_INVALID:
            send_INVACK(req.block);
            state_ = MsiState::I;
            break;
        default:
            bad_msg("S", "ntwk", req.kind);
    }
}

void MsiAgent::do_ntwk_M(const Message& req) {
    switch (req.kind) {
        case MessageKind::RECALL_GOTO_I:
            send_DATA_dir(req.block);
            state_ = MsiState::I;
            break;
        case MessageKind::RECALL_GOTO_S:
            send_DATA_dir(req.block);
            state_ = MsiState::S;
            break;
        default:
            bad_msg("M", "ntwk", req.kind);
    }
}

void MsiAgent::do_ntwk_IS(const Message& req) {
    switch (req.kind) {
        case MessageKind::DATA:
            send_DATA_proc(req.block);
            state_ = MsiState::S;
            break;
        default:
            bad_msg("IS", "ntwk", req.kind);
    }
}

void MsiAgent::do_ntwk_IM(const Message& req) {
    switch (req.kind) {
        case MessageKind::DATA:
            send_DATA_proc(req.block);
            state_ = MsiState::M;
            break;
        default:
            bad_msg("IM", "ntwk", req.kind);
    }
}

void MsiAgent::do_ntwk_SM(const Message& req) {
    switch (req.kind) {
        case MessageKind::REQ_INVALID:
            send_INVACK(req.block);
            state_ = MsiState::IM;
            break;
        case MessageKind::ACK:
            send_DATA_proc(req.block);
            state_ = MsiState::M;
            break;
        // Cache-side eviction can clear our line in S without notifying the
        // agent FSM (on_evict bypasses the agent). When the directory then
        // sees presence[us]=false at our subsequent GETM, it falls into the
        // memory-fetch path and answers with DATA instead of ACK. Treat it
        // like IM->M: accept the data, become M.
        case MessageKind::DATA:
            send_DATA_proc(req.block);
            state_ = MsiState::M;
            break;
        default:
            bad_msg("SM", "ntwk", req.kind);
    }
}

} // namespace comparch::coherence
