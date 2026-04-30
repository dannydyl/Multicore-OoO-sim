// DirectoryController. Mirrors project3/coherence/directory/Directory_Controller.cpp.
// Per-protocol *_tick() methods live in directory_<protocol>.cpp and are
// filled in by Steps 4-8.

#include "comparch/coherence/directory.hpp"

#include "comparch/coherence/node.hpp"
#include "comparch/log.hpp"

namespace comparch::coherence {

const char* dir_state_str(DirState s) {
    switch (s) {
        case DirState::M:  return "M";
        case DirState::O:  return "O";
        case DirState::S:  return "S";
        case DirState::I:  return "I";
        case DirState::F:  return "F";
        case DirState::FS: return "FS";
        case DirState::FM: return "FM";
        case DirState::SM: return "SM";
        case DirState::MO: return "MO";
        case DirState::SS: return "SS";
        case DirState::MS: return "MS";
        case DirState::MM: return "MM";
        case DirState::OM: return "OM";
        case DirState::E:  return "E";
        case DirState::ES: return "ES";
        case DirState::EM: return "EM";
        case DirState::OO: return "OO";
        case DirState::EF: return "EF";
        case DirState::FF: return "FF";
        case DirState::NUM_DIR_STATE: return "NUM_DIR_STATE";
    }
    return "?";
}

DirectoryController::DirectoryController(NodeId id,
                                         const Settings& s,
                                         CoherenceStats& stats)
    : id_(id), settings_(s), stats_(stats) {}

DirectoryController::~DirectoryController() {
    for (auto& kv : directory_) delete kv.second;
    for (auto* m : request_next)  delete m;
    for (auto* m : request_queue) delete m;
}

void DirectoryController::tick(Timestamp clock) {
    current_clock_ = clock;
    switch (settings_.protocol) {
        case Protocol::MI:     MI_tick();     break;
        case Protocol::MSI:    MSI_tick();    break;
        case Protocol::MESI:   MESI_tick();   break;
        case Protocol::MOSI:   MOSI_tick();   break;
        case Protocol::MOESIF: MOESIF_tick(); break;
    }
}

void DirectoryController::tock() {
    request_queue.splice(request_queue.end(), request_next);
}

DirEntry* DirectoryController::get_entry(BlockId block) {
    auto it = directory_.find(block);
    if (it != directory_.end()) return it->second;
    auto* e = new DirEntry();
    e->block_id = block;
    directory_.emplace(block, e);
    return e;
}

Message* DirectoryController::poll_queue() {
    return request_queue.empty() ? nullptr : request_queue.front();
}

void DirectoryController::dequeue() {
    if (request_queue.empty()) return;
    Message* front = request_queue.front();
    request_queue.pop_front();
    delete front;
}

void DirectoryController::cycle_queue() {
    if (request_queue.empty()) return;
    Message* head = request_queue.front();
    request_queue.pop_front();
    request_next.push_back(head);
}

void DirectoryController::send_Request(NodeId dest, BlockId tag, MessageKind kind) {
    auto* req = new Message(id_, dest, tag, kind, settings_);
    my_node->ntwk_out_next.push_back(req);
}

bool DirectoryController::handle_writeback(DirEntry* entry,
                                           const Message& request) {
    if (request.kind != MessageKind::DATA_WB) return false;

    // Cycle the message in any in-flight transient — wait for the
    // protocol round to settle before processing the eviction.
    if (entry->state != DirState::I &&
        entry->state != DirState::S &&
        entry->state != DirState::E &&
        entry->state != DirState::M &&
        entry->state != DirState::O &&
        entry->state != DirState::F) {
        cycle_queue();
        return true;
    }

    // The directory's own state tells us whether the dropping node was
    // the dirty owner: M / O / F. (Project3's MOESIF reuses 'F' for the
    // forwarder; in MESI it's a clean Exclusive holder.)
    const bool was_dirty_holder =
        (entry->state == DirState::M || entry->state == DirState::O ||
         entry->state == DirState::F) &&
        entry->presence[request.src] &&
        request.src == entry->o_f_id /* O/F holder */;
    const bool was_m_holder =
        entry->state == DirState::M && entry->presence[request.src];

    if (entry->presence[request.src]) {
        entry->presence[request.src] = false;
        if (entry->active_sharers > 0) --entry->active_sharers;
    }

    if (was_dirty_holder || was_m_holder) {
        ++stats_.memory_writes;
        entry->dirty = false;
    }

    if (entry->active_sharers == 0) {
        entry->state = DirState::I;
        entry->dirty = false;
    } else if (was_m_holder || was_dirty_holder) {
        // If the M/O/F holder dropped out but other S-state sharers
        // remain, the line is now clean-shared.
        entry->state = DirState::S;
        entry->o_f_id = static_cast<NodeId>(-1);
    }

    dequeue();
    return true;
}

// All five per-protocol *_tick() implementations live in
// directory_<protocol>.cpp now (Steps 4-8 complete).

} // namespace comparch::coherence
