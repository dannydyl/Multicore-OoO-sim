// MESI directory. Mirrors project3/coherence/directory/MESI_Directory.cpp.

#include <stdexcept>

#include "comparch/coherence/directory.hpp"

namespace comparch::coherence {

void DirectoryController::MESI_tick() {
    Message* request = nullptr;
    DirEntry* entry  = nullptr;

    if (!request_in_progress) {
        if ((request = poll_queue()) != nullptr) {
            entry = get_entry(request->block);

            if (handle_writeback(entry, *request)) {
                // already dequeued / cycled; fall through to mem-response.
            } else if (entry->state == DirState::I && request->kind == MessageKind::GETM) {
                request_in_progress = true;
                tag_to_send   = request->block;
                target_node   = request->src;
                response_time = current_clock_ + settings_.mem_latency;
                entry->state  = DirState::M;
                entry->presence[request->src] = true;
                ++entry->active_sharers;
                dequeue();
            } else if (entry->state == DirState::I && request->kind == MessageKind::GETS) {
                request_in_progress = true;
                tag_to_send   = request->block;
                target_node   = request->src;
                response_time = current_clock_ + settings_.mem_latency;
                entry->state  = DirState::E;
                send_E = true;
                entry->presence[request->src] = true;
                ++entry->active_sharers;
                dequeue();
            } else if (entry->state == DirState::M && request->kind == MessageKind::GETM) {
                tag_to_send = request->block;
                target_node = static_cast<NodeId>(-1);
                for (NodeId i = 0; i < settings_.num_procs; ++i) {
                    if (entry->presence[i]) target_node = i;
                }
                send_Request(target_node, tag_to_send, MessageKind::RECALL_GOTO_I);
                entry->state                 = DirState::MM;
                entry->req_node_in_transient = request->src;
                dequeue();
            } else if (entry->state == DirState::M && request->kind == MessageKind::GETS) {
                tag_to_send = request->block;
                target_node = static_cast<NodeId>(-1);
                for (NodeId i = 0; i < settings_.num_procs; ++i) {
                    if (entry->presence[i]) target_node = i;
                }
                send_Request(target_node, tag_to_send, MessageKind::RECALL_GOTO_S);
                entry->state                 = DirState::MS;
                entry->req_node_in_transient = request->src;
                dequeue();
            } else if (entry->state == DirState::M && request->kind == MessageKind::GETX) {
                tag_to_send = request->block;
                target_node = static_cast<NodeId>(-1);
                for (NodeId i = 0; i < settings_.num_procs; ++i) {
                    if (entry->presence[i]) target_node = i;
                }
                send_Request(target_node, tag_to_send, MessageKind::RECALL_GOTO_I);
                entry->state                 = DirState::MM;
                entry->req_node_in_transient = request->src;
                dequeue();
            } else if (entry->state == DirState::MM && request->kind == MessageKind::DATA) {
                tag_to_send = request->block;
                target_node = entry->req_node_in_transient;
                entry->presence[request->src] = false;
                --entry->active_sharers;
                ++stats_.c2c_transfers;
                send_Request(target_node, tag_to_send, MessageKind::DATA);
                entry->state                 = DirState::M;
                entry->presence[target_node] = true;
                ++entry->active_sharers;
                dequeue();
            } else if (entry->state == DirState::MS && request->kind == MessageKind::DATA) {
                tag_to_send = request->block;
                target_node = entry->req_node_in_transient;
                ++stats_.c2c_transfers;
                ++stats_.memory_writes;
                send_Request(target_node, tag_to_send, MessageKind::DATA);
                entry->state                 = DirState::S;
                entry->presence[target_node] = true;
                ++entry->active_sharers;
                dequeue();
            } else if (entry->state == DirState::E && request->kind == MessageKind::GETM) {
                tag_to_send = request->block;
                target_node = static_cast<NodeId>(-1);
                for (NodeId i = 0; i < settings_.num_procs; ++i) {
                    if (entry->presence[i]) target_node = i;
                }
                send_Request(target_node, tag_to_send, MessageKind::RECALL_GOTO_I);
                entry->state                 = DirState::EM;
                entry->req_node_in_transient = request->src;
                dequeue();
            } else if (entry->state == DirState::E && request->kind == MessageKind::GETS) {
                tag_to_send = request->block;
                target_node = static_cast<NodeId>(-1);
                for (NodeId i = 0; i < settings_.num_procs; ++i) {
                    if (entry->presence[i]) target_node = i;
                }
                send_Request(target_node, tag_to_send, MessageKind::RECALL_GOTO_S);
                entry->state                 = DirState::ES;
                entry->req_node_in_transient = request->src;
                dequeue();
            } else if (entry->state == DirState::E && request->kind == MessageKind::GETX) {
                tag_to_send = request->block;
                target_node = request->src;
                ++stats_.silent_upgrades;
                send_Request(target_node, tag_to_send, MessageKind::ACK);
                entry->state = DirState::M;
                dequeue();
            } else if (entry->state == DirState::ES && request->kind == MessageKind::DATA) {
                tag_to_send = request->block;
                target_node = entry->req_node_in_transient;
                ++stats_.c2c_transfers;
                send_Request(target_node, tag_to_send, MessageKind::DATA);
                entry->state                 = DirState::S;
                entry->presence[target_node] = true;
                ++entry->active_sharers;
                dequeue();
            } else if (entry->state == DirState::EM && request->kind == MessageKind::DATA) {
                tag_to_send = request->block;
                target_node = entry->req_node_in_transient;
                entry->presence[request->src] = false;
                --entry->active_sharers;
                ++stats_.c2c_transfers;
                send_Request(target_node, tag_to_send, MessageKind::DATA);
                entry->state                 = DirState::M;
                entry->presence[target_node] = true;
                ++entry->active_sharers;
                dequeue();
            } else if (entry->state == DirState::S && request->kind == MessageKind::GETM) {
                entry->inv_ack_waiting = 0;
                for (NodeId i = 0; i < settings_.num_procs; ++i) {
                    if (entry->presence[i] && i != request->src) {
                        tag_to_send = request->block;
                        target_node = i;
                        ++entry->inv_ack_waiting;
                        send_Request(i, tag_to_send, MessageKind::REQ_INVALID);
                    }
                }
                entry->state                 = DirState::SM;
                entry->req_node_in_transient = request->src;
                dequeue();
            } else if (entry->state == DirState::S && request->kind == MessageKind::GETS) {
                request_in_progress = true;
                tag_to_send   = request->block;
                target_node   = request->src;
                response_time = current_clock_ + settings_.mem_latency;
                entry->state  = DirState::S;
                entry->presence[request->src] = true;
                ++entry->active_sharers;
                dequeue();
            } else if (entry->state == DirState::S && request->kind == MessageKind::GETX) {
                entry->inv_ack_waiting = 0;
                for (NodeId i = 0; i < settings_.num_procs; ++i) {
                    if (entry->presence[i] && i != request->src) {
                        tag_to_send = request->block;
                        target_node = i;
                        ++entry->inv_ack_waiting;
                        send_Request(i, tag_to_send, MessageKind::REQ_INVALID);
                    }
                }
                entry->state                 = DirState::SM;
                entry->req_node_in_transient = request->src;
                dequeue();
            } else if (entry->state == DirState::SM && request->kind == MessageKind::INVACK) {
                if (entry->inv_ack_waiting >= 1) {
                    entry->presence[request->src] = false;
                    --entry->inv_ack_waiting;
                    --entry->active_sharers;
                } else {
                    throw std::runtime_error("MESI dir: too many INVACKs");
                }
                if (entry->inv_ack_waiting == 0) {
                    if (entry->presence[entry->req_node_in_transient]) {
                        tag_to_send = request->block;
                        target_node = entry->req_node_in_transient;
                        send_Request(target_node, tag_to_send, MessageKind::ACK);
                        entry->state = DirState::M;
                    } else {
                        request_in_progress = true;
                        tag_to_send   = request->block;
                        target_node   = entry->req_node_in_transient;
                        response_time = current_clock_ + settings_.mem_latency;
                        entry->presence[entry->req_node_in_transient] = true;
                        ++entry->active_sharers;
                        entry->state = DirState::M;
                    }
                }
                dequeue();
            } else if ((entry->state == DirState::SM || entry->state == DirState::MM ||
                        entry->state == DirState::MS || entry->state == DirState::EM ||
                        entry->state == DirState::ES) &&
                       (request->kind == MessageKind::GETM ||
                        request->kind == MessageKind::GETS ||
                        request->kind == MessageKind::GETX)) {
                cycle_queue();
            } else {
                throw std::runtime_error("MESI dir: invalid (state, message) pair");
            }
        }
    }

    if (request_in_progress && current_clock_ >= response_time) {
        ++stats_.memory_reads;
        if (send_E) {
            send_Request(target_node, tag_to_send, MessageKind::DATA_E);
            send_E = false;
        } else {
            send_Request(target_node, tag_to_send, MessageKind::DATA);
        }
        request_in_progress = false;
    }
}

} // namespace comparch::coherence
