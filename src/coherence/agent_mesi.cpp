// MESI agent. Mirrors project3/coherence/agents/MESI_Agent.cpp.

#include "comparch/coherence/agent_mesi.hpp"

#include <stdexcept>
#include <string>

#include "comparch/coherence/coherence_cache.hpp"

namespace comparch::coherence {

namespace {

[[noreturn]] void bad_msg(const char* state, const char* side, MessageKind k) {
    throw std::runtime_error(
        std::string("MESI: ") + state + " state shouldn't see " + side +
        " message: " + message_kind_str(k));
}

} // namespace

MesiAgent::MesiAgent(NodeId id, Cache* cache, BlockId block)
    : Agent(id, cache, block), state_(MesiState::I) {}

void MesiAgent::process_proc_request(const Message& req) {
    switch (state_) {
        case MesiState::I:  do_proc_I(req); break;
        case MesiState::S:  do_proc_S(req); break;
        case MesiState::E:  do_proc_E(req); break;
        case MesiState::M:  do_proc_M(req); break;
        case MesiState::IS:
        case MesiState::IM:
        case MesiState::SM:
        case MesiState::EM: do_proc_in_transient(req); break;
    }
}

void MesiAgent::process_ntwk_request(const Message& req) {
    switch (state_) {
        case MesiState::I:  bad_msg("I",  "ntwk", req.kind);
        case MesiState::S:  do_ntwk_S(req);  break;
        case MesiState::E:  do_ntwk_E(req);  break;
        case MesiState::M:  do_ntwk_M(req);  break;
        case MesiState::IS: do_ntwk_IS(req); break;
        case MesiState::IM: do_ntwk_IM(req); break;
        case MesiState::SM: do_ntwk_SM(req); break;
        case MesiState::EM: do_ntwk_EM(req); break;
    }
}

void MesiAgent::do_proc_I(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:
            send_GETS(req.block);
            state_ = MesiState::IS;
            ++cache_->stats().cache_misses;
            break;
        case MessageKind::STORE:
            send_GETM(req.block);
            state_ = MesiState::IM;
            ++cache_->stats().cache_misses;
            break;
        default: bad_msg("I", "proc", req.kind);
    }
}

void MesiAgent::do_proc_S(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:  send_DATA_proc(req.block); break;
        case MessageKind::STORE:
            send_GETM(req.block);
            state_ = MesiState::SM;
            break;
        default: bad_msg("S", "proc", req.kind);
    }
}

void MesiAgent::do_proc_E(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:  send_DATA_proc(req.block); break;
        case MessageKind::STORE:
            send_GETX(req.block);            // silent-upgrade signal
            state_ = MesiState::EM;
            break;
        default: bad_msg("E", "proc", req.kind);
    }
}

void MesiAgent::do_proc_M(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:
        case MessageKind::STORE: send_DATA_proc(req.block); break;
        default: bad_msg("M", "proc", req.kind);
    }
}

void MesiAgent::do_proc_in_transient(const Message& req) {
    (void)req;
    throw std::runtime_error(
        "MESI: transient state should not see another proc request");
}

void MesiAgent::do_ntwk_S(const Message& req) {
    switch (req.kind) {
        case MessageKind::REQ_INVALID:
            send_INVACK(req.block);
            state_ = MesiState::I;
            break;
        default: bad_msg("S", "ntwk", req.kind);
    }
}

void MesiAgent::do_ntwk_E(const Message& req) {
    switch (req.kind) {
        case MessageKind::RECALL_GOTO_I:
            send_DATA_dir(req.block);
            state_ = MesiState::I;
            break;
        case MessageKind::RECALL_GOTO_S:
            send_DATA_dir(req.block);
            state_ = MesiState::S;
            break;
        default: bad_msg("E", "ntwk", req.kind);
    }
}

void MesiAgent::do_ntwk_M(const Message& req) {
    switch (req.kind) {
        case MessageKind::RECALL_GOTO_I:
            send_DATA_dir(req.block);
            state_ = MesiState::I;
            break;
        case MessageKind::RECALL_GOTO_S:
            send_DATA_dir(req.block);
            state_ = MesiState::S;
            break;
        default: bad_msg("M", "ntwk", req.kind);
    }
}

void MesiAgent::do_ntwk_IS(const Message& req) {
    switch (req.kind) {
        case MessageKind::DATA:
            send_DATA_proc(req.block);
            state_ = MesiState::S;
            break;
        case MessageKind::DATA_E:
            send_DATA_proc(req.block);
            state_ = MesiState::E;
            break;
        default: bad_msg("IS", "ntwk", req.kind);
    }
}

void MesiAgent::do_ntwk_IM(const Message& req) {
    switch (req.kind) {
        case MessageKind::DATA:
            send_DATA_proc(req.block);
            state_ = MesiState::M;
            break;
        default: bad_msg("IM", "ntwk", req.kind);
    }
}

void MesiAgent::do_ntwk_SM(const Message& req) {
    switch (req.kind) {
        case MessageKind::REQ_INVALID:
            send_INVACK(req.block);
            state_ = MesiState::IM;
            break;
        case MessageKind::ACK:
            send_DATA_proc(req.block);
            state_ = MesiState::M;
            break;
        // Eviction can clear our line in S without driving the agent FSM,
        // so the directory may see presence[us]=false at our GETM and reply
        // with DATA (memory-fetch path) instead of ACK. Mirror IM->M.
        case MessageKind::DATA:
            send_DATA_proc(req.block);
            state_ = MesiState::M;
            break;
        default: bad_msg("SM", "ntwk", req.kind);
    }
}

void MesiAgent::do_ntwk_EM(const Message& req) {
    switch (req.kind) {
        case MessageKind::RECALL_GOTO_I:
            send_DATA_dir(req.block);
            state_ = MesiState::IM;
            break;
        case MessageKind::RECALL_GOTO_S:
            send_DATA_dir(req.block);
            state_ = MesiState::SM;
            break;
        case MessageKind::ACK:
            send_DATA_proc(req.block);
            state_ = MesiState::M;
            break;
        // Eviction-desync companion to the directory's (I, GETX) handler:
        // we issued a silent-upgrade GETX from a desynced E-state, the
        // directory had no record of us, fetched from memory, and replied
        // with DATA. Accept it and transition to M.
        case MessageKind::DATA:
            send_DATA_proc(req.block);
            state_ = MesiState::M;
            break;
        default: bad_msg("EM", "ntwk", req.kind);
    }
}

} // namespace comparch::coherence
