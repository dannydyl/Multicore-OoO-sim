// MOSI agent. Mirrors project3/coherence/agents/MOSI_Agent.cpp.

#include "comparch/coherence/agent_mosi.hpp"

#include <stdexcept>
#include <string>

#include "comparch/coherence/coherence_cache.hpp"

namespace comparch::coherence {

namespace {

[[noreturn]] void bad_msg(const char* state, const char* side, MessageKind k) {
    throw std::runtime_error(
        std::string("MOSI: ") + state + " state shouldn't see " + side +
        " message: " + message_kind_str(k));
}

} // namespace

MosiAgent::MosiAgent(NodeId id, Cache* cache, BlockId block)
    : Agent(id, cache, block), state_(MosiState::I) {}

void MosiAgent::process_proc_request(const Message& req) {
    switch (state_) {
        case MosiState::I:  do_proc_I(req); break;
        case MosiState::S:  do_proc_S(req); break;
        case MosiState::M:  do_proc_M(req); break;
        case MosiState::O:  do_proc_O(req); break;
        case MosiState::IS:
        case MosiState::IM:
        case MosiState::SM:
        case MosiState::OM: do_proc_in_transient(req); break;
    }
}

void MosiAgent::process_ntwk_request(const Message& req) {
    switch (state_) {
        case MosiState::I:  bad_msg("I",  "ntwk", req.kind);
        case MosiState::S:  do_ntwk_S(req);  break;
        case MosiState::M:  do_ntwk_M(req);  break;
        case MosiState::O:  do_ntwk_O(req);  break;
        case MosiState::IS: do_ntwk_IS(req); break;
        case MosiState::IM: do_ntwk_IM(req); break;
        case MosiState::SM: do_ntwk_SM(req); break;
        case MosiState::OM: do_ntwk_OM(req); break;
    }
}

void MosiAgent::do_proc_I(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:
            send_GETS(req.block);
            state_ = MosiState::IS;
            ++cache_->stats().cache_misses;
            break;
        case MessageKind::STORE:
            send_GETM(req.block);
            state_ = MosiState::IM;
            ++cache_->stats().cache_misses;
            break;
        default: bad_msg("I", "proc", req.kind);
    }
}

void MosiAgent::do_proc_S(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:  send_DATA_proc(req.block); break;
        case MessageKind::STORE:
            send_GETM(req.block);
            state_ = MosiState::SM;
            break;
        default: bad_msg("S", "proc", req.kind);
    }
}

void MosiAgent::do_proc_M(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:
        case MessageKind::STORE:
            send_DATA_proc(req.block);
            state_ = MosiState::M;
            break;
        default: bad_msg("M", "proc", req.kind);
    }
}

void MosiAgent::do_proc_O(const Message& req) {
    switch (req.kind) {
        case MessageKind::LOAD:  send_DATA_proc(req.block); break;
        case MessageKind::STORE:
            send_GETM(req.block);
            state_ = MosiState::OM;
            break;
        default: bad_msg("O", "proc", req.kind);
    }
}

void MosiAgent::do_proc_in_transient(const Message& req) {
    (void)req;
    throw std::runtime_error(
        "MOSI: transient state should not see another proc request");
}

void MosiAgent::do_ntwk_S(const Message& req) {
    switch (req.kind) {
        case MessageKind::REQ_INVALID:
            send_INVACK(req.block);
            state_ = MosiState::I;
            break;
        default: bad_msg("S", "ntwk", req.kind);
    }
}

void MosiAgent::do_ntwk_M(const Message& req) {
    switch (req.kind) {
        case MessageKind::RECALL_GOTO_I:
            send_DATA_dir(req.block);
            state_ = MosiState::I;
            break;
        case MessageKind::RECALL_GOTO_S:
            send_DATA_dir(req.block);
            state_ = MosiState::O;
            break;
        default: bad_msg("M", "ntwk", req.kind);
    }
}

void MosiAgent::do_ntwk_O(const Message& req) {
    switch (req.kind) {
        case MessageKind::RECALL_GOTO_I:
            send_DATA_dir(req.block);
            state_ = MosiState::I;
            break;
        case MessageKind::RECALL_GOTO_S:
            send_DATA_dir(req.block);
            state_ = MosiState::O;
            break;
        default: bad_msg("O", "ntwk", req.kind);
    }
}

void MosiAgent::do_ntwk_IS(const Message& req) {
    switch (req.kind) {
        case MessageKind::DATA:
            send_DATA_proc(req.block);
            state_ = MosiState::S;
            break;
        default: bad_msg("IS", "ntwk", req.kind);
    }
}

void MosiAgent::do_ntwk_IM(const Message& req) {
    switch (req.kind) {
        case MessageKind::DATA:
            send_DATA_proc(req.block);
            state_ = MosiState::M;
            break;
        default: bad_msg("IM", "ntwk", req.kind);
    }
}

void MosiAgent::do_ntwk_SM(const Message& req) {
    switch (req.kind) {
        case MessageKind::REQ_INVALID:
            send_INVACK(req.block);
            state_ = MosiState::IM;
            break;
        case MessageKind::ACK:
            send_DATA_proc(req.block);
            state_ = MosiState::M;
            break;
        default: bad_msg("SM", "ntwk", req.kind);
    }
}

void MosiAgent::do_ntwk_OM(const Message& req) {
    switch (req.kind) {
        case MessageKind::RECALL_GOTO_S:
            send_DATA_dir(req.block);
            state_ = MosiState::OM;          // self-loop per project3
            break;
        case MessageKind::RECALL_GOTO_I:
            send_DATA_dir(req.block);
            state_ = MosiState::IM;
            break;
        case MessageKind::ACK:
            send_DATA_proc(req.block);
            state_ = MosiState::M;
            break;
        default: bad_msg("OM", "ntwk", req.kind);
    }
}

} // namespace comparch::coherence
