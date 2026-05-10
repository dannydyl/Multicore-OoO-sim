// MI directory. Mirrors project3/coherence/directory/MI_Directory.cpp.

#include <stdexcept>

#include "comparch/coherence/directory.hpp"

namespace comparch::coherence {

void DirectoryController::MI_tick() {
    Message* request = nullptr;
    DirEntry* entry  = nullptr;

    if (!request_in_progress) {
        if ((request = poll_queue()) != nullptr) {
            entry = get_entry(request->block);

            if (handle_writeback(entry, *request)) {
                // already dequeued / cycled; fall through to mem-response.
            } else if (entry->state == DirState::I && request->kind == MessageKind::GETM) {
                entry->state  = DirState::M;
                entry->presence[request->src] = true;
                ++entry->active_sharers;
                schedule_data_response(request->block, request->src);
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
            } else if (entry->state == DirState::MM &&
                       request->kind == MessageKind::GETM) {
                cycle_queue();
            } else {
                throw std::runtime_error("MI dir: invalid (state, message) pair");
            }
        }
    }

    if (request_in_progress && current_clock_ >= response_time) {
        // pending_lls_miss is set by schedule_data_response: true when
        // the response went off-chip to memory, false when satisfied
        // by an LLS hit. Charge memory_reads only on the off-chip path.
        if (pending_lls_miss) ++stats_.memory_reads;
        send_Request(target_node, tag_to_send, MessageKind::DATA);
        request_in_progress = false;
    }
}

} // namespace comparch::coherence
