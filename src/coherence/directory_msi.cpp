// MSI directory. Mirrors project3/coherence/directory/MSI_Directory.cpp
// line-by-line. Stat-counter increments and the request_in_progress /
// response_time handshake stay at the same code positions as the legacy.

#include <stdexcept>

#include "comparch/coherence/directory.hpp"

namespace comparch::coherence {

void DirectoryController::MSI_tick() {
    Message* request = nullptr;
    DirEntry* entry  = nullptr;

    if (!request_in_progress) {
        if ((request = poll_queue()) != nullptr) {
            entry = get_entry(request->block);

            if (handle_writeback(entry, *request)) {
                // already dequeued / cycled; fall through to mem-response.
            } else if (entry->state == DirState::I && request->kind == MessageKind::GETM) {
                request_in_progress = true;
                response_time = current_clock_ + settings_.mem_latency;
                entry->state  = DirState::M;
                entry->dirty  = true;
                entry->block_id = request->block;

                if (!entry->presence[request->src]) {
                    entry->presence[request->src] = true;
                    ++entry->active_sharers;
                }
                target_node = request->src;
                tag_to_send = request->block;
                dequeue();
            } else if (entry->state == DirState::I && request->kind == MessageKind::GETS) {
                request_in_progress = true;
                response_time = current_clock_ + settings_.mem_latency;
                entry->state  = DirState::S;
                entry->dirty  = false;
                entry->block_id = request->block;

                if (!entry->presence[request->src]) {
                    entry->presence[request->src] = true;
                    ++entry->active_sharers;
                }
                target_node = request->src;
                tag_to_send = request->block;
                dequeue();
            } else if (entry->state == DirState::M && request->kind == MessageKind::GETM) {
                NodeId old_owner = 0;
                for (std::size_t i = 0; i < kMaxSharers; ++i) {
                    if (entry->presence[i]) { old_owner = static_cast<NodeId>(i); break; }
                }
                entry->req_node_in_transient = request->src;
                send_Request(old_owner, request->block, MessageKind::RECALL_GOTO_I);
                tag_to_send = request->block;
                entry->state = DirState::MM;
                dequeue();
            } else if (entry->state == DirState::M && request->kind == MessageKind::GETS) {
                NodeId old_owner = 0;
                for (std::size_t i = 0; i < kMaxSharers; ++i) {
                    if (entry->presence[i]) { old_owner = static_cast<NodeId>(i); break; }
                }
                entry->req_node_in_transient = request->src;
                send_Request(old_owner, request->block, MessageKind::RECALL_GOTO_S);
                tag_to_send = request->block;
                entry->state = DirState::MS;
                dequeue();
            } else if (entry->state == DirState::MM && request->kind == MessageKind::DATA) {
                NodeId requester = entry->req_node_in_transient;
                // Drop the previous M-state owner's presence (now I).
                for (std::size_t i = 0; i < kMaxSharers; ++i) {
                    if (entry->presence[i]) {
                        entry->presence[i] = false;
                        --entry->active_sharers;
                        break;
                    }
                }
                send_Request(requester, request->block, MessageKind::DATA);
                entry->presence[requester] = true;
                ++entry->active_sharers;
                ++stats_.c2c_transfers;
                entry->dirty = true;
                entry->state = DirState::M;
                dequeue();
            } else if (entry->state == DirState::MS && request->kind == MessageKind::DATA) {
                ++stats_.c2c_transfers;
                send_Request(entry->req_node_in_transient, request->block, MessageKind::DATA);
                entry->presence[entry->req_node_in_transient] = true;
                ++entry->active_sharers;
                // M -> S: must write-back to memory.
                ++stats_.memory_writes;
                entry->state = DirState::S;
                entry->dirty = false;
                dequeue();
            } else if (entry->state == DirState::S && request->kind == MessageKind::GETM) {
                const NodeId requester = request->src;
                const bool requester_has_copy = entry->presence[request->src];
                int invalidates_sent = 0;
                for (std::size_t i = 0; i < kMaxSharers; ++i) {
                    if (entry->presence[i] && static_cast<NodeId>(i) != requester) {
                        send_Request(static_cast<NodeId>(i), entry->block_id,
                                     MessageKind::REQ_INVALID);
                        ++invalidates_sent;
                    }
                }
                if (invalidates_sent == 0) {
                    send_Request(requester, entry->block_id, MessageKind::ACK);
                    entry->dirty = true;
                    entry->state = DirState::M;
                } else {
                    entry->req_node_in_transient = requester;
                    entry->inv_ack_waiting       = invalidates_sent;
                    entry->getM_upgrade          = requester_has_copy;
                    entry->state                 = DirState::SM;
                }
                dequeue();
            } else if (entry->state == DirState::S && request->kind == MessageKind::GETS) {
                request_in_progress = true;
                response_time = current_clock_ + settings_.mem_latency;
                entry->state  = DirState::S;
                if (!entry->presence[request->src]) {
                    entry->presence[request->src] = true;
                    ++entry->active_sharers;
                }
                target_node = request->src;
                tag_to_send = request->block;
                dequeue();
            } else if (entry->state == DirState::SM && request->kind == MessageKind::INVACK) {
                entry->presence[request->src] = false;
                --entry->active_sharers;
                --entry->inv_ack_waiting;
                if (entry->inv_ack_waiting == 0) {
                    if (entry->getM_upgrade) {
                        send_Request(entry->req_node_in_transient,
                                     entry->block_id, MessageKind::ACK);
                        entry->dirty = true;
                        entry->state = DirState::M;
                    } else {
                        request_in_progress = true;
                        response_time = current_clock_ + settings_.mem_latency;
                        target_node   = entry->req_node_in_transient;
                        tag_to_send   = entry->block_id;
                        entry->presence[entry->req_node_in_transient] = true;
                        ++entry->active_sharers;
                        entry->state = DirState::M;
                        entry->dirty = true;
                    }
                }
                dequeue();
            } else if ((entry->state == DirState::SM || entry->state == DirState::MM ||
                        entry->state == DirState::MS) &&
                       (request->kind == MessageKind::GETM ||
                        request->kind == MessageKind::GETS)) {
                cycle_queue();
            } else {
                throw std::runtime_error(
                    "MSI dir: invalid (state, message) pair");
            }
        }
    }

    if (request_in_progress && current_clock_ >= response_time) {
        ++stats_.memory_reads;
        send_Request(target_node, tag_to_send, MessageKind::DATA);
        request_in_progress = false;
    }
}

} // namespace comparch::coherence
