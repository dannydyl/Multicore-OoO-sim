// MOESIF agent. Mirrors project3/coherence/agents/MOESIF_Agent.cpp.

#include "comparch/coherence/agent_moesif.hpp"

#include <stdexcept>
#include <string>

#include "comparch/coherence/coherence_cache.hpp"

namespace comparch::coherence {

namespace {

[[noreturn]] void bad_msg(const char* state, const char* side, MessageKind k) {
    throw std::runtime_error(
        std::string("MOESIF: ") + state + " state shouldn't see " + side +
        " message: " + message_kind_str(k));
}

} // namespace

MoesifAgent::MoesifAgent(NodeId id, Cache* cache, BlockId block)
    : Agent(id, cache, block), state_(MoesifState::I) {}

void MoesifAgent::process_proc_request(const Message& req) {
    switch (state_) {
        case MoesifState::I:  do_proc_I(req); break;
        case MoesifState::S:  do_proc_S(req); break;
        case MoesifState::E:  do_proc_E(req); break;
        case MoesifState::M:  do_proc_M(req); break;
        case MoesifState::F:  do_proc_F(req); break;
        case MoesifState::O:  do_proc_O(req); break;
        case MoesifState::IS:
        case MoesifState::IM:
        case MoesifState::SM:
        case MoesifState::EM:
        case MoesifState::FM:
        case MoesifState::OM: do_proc_in_transient(req); break;
    }
}

void MoesifAgent::process_ntwk_request(const Message& req) {
    switch (state_) {
        case MoesifState::I:  bad_msg("I",  "ntwk", req.kind);
        case MoesifState::S:  do_ntwk_S(req);  break;
        case MoesifState::E:  do_ntwk_E(req);  break;
        case MoesifState::M:  do_ntwk_M(req);  break;
        case MoesifState::F:  do_ntwk_F(req);  break;
        case MoesifState::O:  do_ntwk_O(req);  break;
        case MoesifState::IS: do_ntwk_IS(req); break;
        case MoesifState::IM: do_ntwk_IM(req); break;
        case MoesifState::SM: do_ntwk_SM(req); break;
        case MoesifState::EM: do_ntwk_EM(req); break;
        case MoesifState::FM: do_ntwk_FM(req); break;
        case MoesifState::OM: do_ntwk_OM(req); break;
    }
}

void MoesifAgent::do_proc_I(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:
            send_GETS(req.block);
            state_ = MoesifState::IS;
            ++cache_->stats().cache_misses;
            break;
        case MessageKind::STORE:
            send_GETM(req.block);
            state_ = MoesifState::IM;
            ++cache_->stats().cache_misses;
            break;
        default: bad_msg("I", "proc", req.kind);
    }
}

void MoesifAgent::do_proc_S(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:
            send_DATA_proc(req.block);
            state_ = MoesifState::S;
            break;
        case MessageKind::STORE:
            send_GETM(req.block);
            state_ = MoesifState::SM;
            break;
        default: bad_msg("S", "proc", req.kind);
    }
}

void MoesifAgent::do_proc_E(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:
            send_DATA_proc(req.block);
            state_ = MoesifState::E;
            break;
        case MessageKind::STORE:
            send_GETX(req.block);
            state_ = MoesifState::EM;
            break;
        default: bad_msg("E", "proc", req.kind);
    }
}

void MoesifAgent::do_proc_M(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:
        case MessageKind::STORE:
            send_DATA_proc(req.block);
            state_ = MoesifState::M;
            break;
        default: bad_msg("M", "proc", req.kind);
    }
}

void MoesifAgent::do_proc_F(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:
            send_DATA_proc(req.block);
            state_ = MoesifState::F;
            break;
        case MessageKind::STORE:
            send_GETM(req.block);
            state_ = MoesifState::FM;
            break;
        default: bad_msg("F", "proc", req.kind);
    }
}

void MoesifAgent::do_proc_O(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:
            send_DATA_proc(req.block);
            state_ = MoesifState::O;
            break;
        case MessageKind::STORE:
            send_GETM(req.block);
            state_ = MoesifState::OM;
            break;
        default: bad_msg("O", "proc", req.kind);
    }
}

void MoesifAgent::do_proc_in_transient(const Message& req) {
    (void)req;
    throw std::runtime_error(
        "MOESIF: transient state should not see another proc request");
}

void MoesifAgent::do_ntwk_S(const Message& req) {
    switch (req.kind) {
        case MessageKind::REQ_INVALID:
            send_INVACK(req.block);
            state_ = MoesifState::I;
            break;
        default: bad_msg("S", "ntwk", req.kind);
    }
}

void MoesifAgent::do_ntwk_E(const Message& req) {
    switch (req.kind) {
        case MessageKind::RECALL_GOTO_I:
            send_DATA_dir(req.block);
            state_ = MoesifState::I;
            break;
        case MessageKind::RECALL_GOTO_S:
            send_DATA_dir(req.block);
            state_ = MoesifState::F;
            break;
        default: bad_msg("E", "ntwk", req.kind);
    }
}

void MoesifAgent::do_ntwk_M(const Message& req) {
    switch (req.kind) {
        case MessageKind::RECALL_GOTO_I:
            send_DATA_dir(req.block);
            state_ = MoesifState::I;
            break;
        case MessageKind::RECALL_GOTO_S:
            send_DATA_dir(req.block);
            state_ = MoesifState::O;
            break;
        default: bad_msg("M", "ntwk", req.kind);
    }
}

void MoesifAgent::do_ntwk_F(const Message& req) {
    switch (req.kind) {
        case MessageKind::RECALL_GOTO_I:
            send_DATA_dir(req.block);
            state_ = MoesifState::I;
            break;
        case MessageKind::RECALL_GOTO_S:
            send_DATA_dir(req.block);
            state_ = MoesifState::F;
            break;
        default: bad_msg("F", "ntwk", req.kind);
    }
}

void MoesifAgent::do_ntwk_O(const Message& req) {
    switch (req.kind) {
        case MessageKind::RECALL_GOTO_I:
            send_DATA_dir(req.block);
            state_ = MoesifState::I;
            break;
        case MessageKind::RECALL_GOTO_S:
            send_DATA_dir(req.block);
            state_ = MoesifState::O;
            break;
        default: bad_msg("O", "ntwk", req.kind);
    }
}

void MoesifAgent::do_ntwk_IS(const Message& req) {
    switch (req.kind) {
        case MessageKind::DATA:
            send_DATA_proc(req.block);
            state_ = MoesifState::S;
            break;
        case MessageKind::DATA_E:
            send_DATA_proc(req.block);
            state_ = MoesifState::E;
            break;
        default: bad_msg("IS", "ntwk", req.kind);
    }
}

void MoesifAgent::do_ntwk_IM(const Message& req) {
    switch (req.kind) {
        case MessageKind::DATA:
            send_DATA_proc(req.block);
            state_ = MoesifState::M;
            break;
        default: bad_msg("IM", "ntwk", req.kind);
    }
}

void MoesifAgent::do_ntwk_SM(const Message& req) {
    switch (req.kind) {
        case MessageKind::REQ_INVALID:
            send_INVACK(req.block);
            state_ = MoesifState::IM;
            break;
        case MessageKind::ACK:
            send_DATA_proc(req.block);
            state_ = MoesifState::M;
            break;
        // Eviction-induced presence loss: directory replies with DATA on
        // the memory-fetch path. Same shape as IM->M.
        case MessageKind::DATA:
            send_DATA_proc(req.block);
            state_ = MoesifState::M;
            break;
        default: bad_msg("SM", "ntwk", req.kind);
    }
}

void MoesifAgent::do_ntwk_EM(const Message& req) {
    switch (req.kind) {
        case MessageKind::RECALL_GOTO_I:
            send_DATA_dir(req.block);
            state_ = MoesifState::IM;
            break;
        case MessageKind::RECALL_GOTO_S:
            send_DATA_dir(req.block);
            state_ = MoesifState::FM;
            break;
        case MessageKind::ACK:
            send_DATA_proc(req.block);
            state_ = MoesifState::M;
            break;
        // Eviction-desync companion to dir's (I, GETX) recovery.
        case MessageKind::DATA:
            send_DATA_proc(req.block);
            state_ = MoesifState::M;
            break;
        default: bad_msg("EM", "ntwk", req.kind);
    }
}

void MoesifAgent::do_ntwk_FM(const Message& req) {
    switch (req.kind) {
        case MessageKind::RECALL_GOTO_I:
            send_DATA_dir(req.block);
            state_ = MoesifState::IM;
            break;
        case MessageKind::RECALL_GOTO_S:
            send_DATA_dir(req.block);
            state_ = MoesifState::FM;
            break;
        case MessageKind::ACK:
            send_DATA_proc(req.block);
            state_ = MoesifState::M;
            break;
        case MessageKind::DATA:
            send_DATA_proc(req.block);
            state_ = MoesifState::M;
            break;
        default: bad_msg("FM", "ntwk", req.kind);
    }
}

void MoesifAgent::do_ntwk_OM(const Message& req) {
    switch (req.kind) {
        case MessageKind::RECALL_GOTO_S:
            send_DATA_dir(req.block);
            state_ = MoesifState::OM;       // self-loop per project3
            break;
        case MessageKind::RECALL_GOTO_I:
            send_DATA_dir(req.block);
            state_ = MoesifState::IM;
            break;
        case MessageKind::ACK:
            send_DATA_proc(req.block);
            state_ = MoesifState::M;
            break;
        case MessageKind::DATA:
            send_DATA_proc(req.block);
            state_ = MoesifState::M;
            break;
        default: bad_msg("OM", "ntwk", req.kind);
    }
}

} // namespace comparch::coherence
