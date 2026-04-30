#pragma once

// Network message. Mirrors project3/simulator/request.h plus
// project3/coherence/messages.h, merged into one struct.
//
// Lifetime: messages flow through std::list<Message*> queues with manual
// new/delete in the legacy code; we keep that pattern for parity since
// the agents will route a message into multiple queues during a
// fan-out and the lifetime depends on protocol logic.

#include "comparch/coherence/settings.hpp"
#include "comparch/coherence/types.hpp"

namespace comparch::coherence {

enum class MessageKind {
    NOP = 0,
    LOAD,
    STORE,
    GETS,
    GETM,
    DATA,
    DATA_WB,        // unused in project3 but reserved
    REQ_INVALID,
    INVACK,
    ACK,
    DATA_E,
    GETX,
    DATA_F,
    RECALL_GOTO_I,
    RECALL_GOTO_S,
    REQ_MESSAGE_NUM // sentinel
};

const char* message_kind_str(MessageKind k);

struct Message {
    NodeId      src   = 0;
    NodeId      dst   = 0;
    BlockId     block = 0;
    MessageKind kind  = MessageKind::NOP;
    Timestamp   req_time = 0;
    int         flits = 0;
    int         tof   = 0;          // time-of-flight remaining (cycles)

    Message() = default;

    // Project3 Request::Request — flit count derived from Settings.
    Message(NodeId src_, NodeId dst_, BlockId block_, MessageKind kind_,
            const Settings& s);
};

} // namespace comparch::coherence
