// MI agent. Mirrors project3/coherence/agents/MI_Agent.cpp.

#include "comparch/coherence/agent_mi.hpp"

#include <stdexcept>
#include <string>

#include "comparch/coherence/coherence_cache.hpp"

namespace comparch::coherence {

namespace {

[[noreturn]] void bad_msg(const char* state, const char* side, MessageKind k) {
    throw std::runtime_error(
        std::string("MI: ") + state + " state shouldn't see " + side +
        " message: " + message_kind_str(k));
}

} // namespace

MiAgent::MiAgent(NodeId id, Cache* cache, BlockId block)
    : Agent(id, cache, block), state_(MiState::I) {}

void MiAgent::process_proc_request(const Message& req) {
    switch (state_) {
        case MiState::I:  do_proc_I(req); break;
        case MiState::M:  do_proc_M(req); break;
        case MiState::IM: do_proc_in_transient(req); break;
    }
}

void MiAgent::process_ntwk_request(const Message& req) {
    switch (state_) {
        case MiState::I:  bad_msg("I", "ntwk", req.kind);
        case MiState::M:  do_ntwk_M(req); break;
        case MiState::IM: do_ntwk_IM(req); break;
    }
}

void MiAgent::do_proc_I(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:
        case MessageKind::STORE:
            send_GETM(req.block);
            state_ = MiState::IM;
            ++cache_->stats().cache_misses;
            break;
        default:
            bad_msg("I", "proc", req.kind);
    }
}

void MiAgent::do_proc_M(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:
        case MessageKind::STORE:
            send_DATA_proc(req.block);
            break;
        default:
            bad_msg("M", "proc", req.kind);
    }
}

void MiAgent::do_proc_in_transient(const Message& req) {
    (void)req;
    throw std::runtime_error(
        "MI: transient state should not see another proc request");
}

void MiAgent::do_ntwk_M(const Message& req) {
    switch (req.kind) {
        case MessageKind::RECALL_GOTO_I:
            send_DATA_dir(req.block);
            state_ = MiState::I;
            break;
        default:
            bad_msg("M", "ntwk", req.kind);
    }
}

void MiAgent::do_ntwk_IM(const Message& req) {
    switch (req.kind) {
        case MessageKind::DATA:
            send_DATA_proc(req.block);
            state_ = MiState::M;
            break;
        default:
            bad_msg("IM", "ntwk", req.kind);
    }
}

} // namespace comparch::coherence
