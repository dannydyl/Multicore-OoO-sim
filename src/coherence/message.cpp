#include "comparch/coherence/message.hpp"

namespace comparch::coherence {

const char* message_kind_str(MessageKind k) {
    switch (k) {
        case MessageKind::NOP:           return "NOP";
        case MessageKind::LOAD:          return "LOAD";
        case MessageKind::STORE:         return "STORE";
        case MessageKind::GETS:          return "GETS";
        case MessageKind::GETM:          return "GETM";
        case MessageKind::DATA:          return "DATA";
        case MessageKind::DATA_WB:       return "DATA_WB";
        case MessageKind::REQ_INVALID:   return "REQ_INVALID";
        case MessageKind::INVACK:        return "INVACK";
        case MessageKind::ACK:           return "ACK";
        case MessageKind::DATA_E:        return "DATA_E";
        case MessageKind::GETX:          return "GETX";
        case MessageKind::DATA_F:        return "DATA_F";
        case MessageKind::RECALL_GOTO_I: return "RECALL_GOTO_I";
        case MessageKind::RECALL_GOTO_S: return "RECALL_GOTO_S";
        case MessageKind::REQ_MESSAGE_NUM: return "REQ_MESSAGE_NUM";
    }
    return "?";
}

Message::Message(NodeId src_, NodeId dst_, BlockId block_, MessageKind kind_,
                 const Settings& s)
    : src(src_), dst(dst_), block(block_), kind(kind_), req_time(0),
      flits(static_cast<int>(s.header_flits)), tof(0) {
    // Project3 Request::Request adds payload flits for data-bearing messages.
    if (kind_ == MessageKind::DATA || kind_ == MessageKind::DATA_E ||
        kind_ == MessageKind::DATA_F || kind_ == MessageKind::DATA_WB) {
        flits += static_cast<int>(s.payload_flits);
    }
}

} // namespace comparch::coherence
