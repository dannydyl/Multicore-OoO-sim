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
    : id_(id),
      settings_(s),
      stats_(stats),
      // LLS sized from Settings. In private_l2 mode lls_blocks=0
      // and the cache is "disabled" -- every access misses with no
      // install, so the directory's data-response path collapses to
      // the legacy "always charge mem_latency" behavior.
      lls(s.cache_mode == CacheMode::SharedLls ? s.lls_blocks : 0,
          s.cache_mode == CacheMode::SharedLls ? s.lls_assoc  : 0) {}

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

void DirectoryController::send_Request(NodeId dest, BlockId tag,
                                       MessageKind kind, bool dirty) {
    auto* req = new Message(id_, dest, tag, kind, settings_);
    req->dirty = dirty;
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

    // Memory-write accounting: trust the source-side `dirty` flag on
    // the WB message. The directory's tracked state can lag a dirty
    // eviction (e.g. an unrelated transition moved the line out of
    // M/O/F before the WB drained), so relying on directory state
    // alone undercounts memory_writes. The flag came from the cache
    // line's own dirty bit at eviction time — authoritative.
    const bool source_dirty = request.dirty;

    // Whether the source held the line in M / O / F at the directory's
    // last observation. Still useful for the state-transition path
    // below (deciding whether remaining sharers collapse to S).
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

    if (source_dirty) {
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

void DirectoryController::schedule_data_response(BlockId block, NodeId target) {
    request_in_progress = true;
    target_node         = target;
    tag_to_send         = block;

    if (settings_.cache_mode != CacheMode::SharedLls) {
        // Private-L2 mode: directory has no LLS to consult. Behavior
        // matches the legacy code byte-for-byte: response in mem_latency
        // cycles, charge memory_reads at response time.
        response_time    = current_clock_ + settings_.mem_latency;
        pending_lls_miss = true;
        return;
    }

    ++stats_.lls_accesses;
    auto r = lls.access(block);
    if (r.hit) {
        ++stats_.lls_hits;
        response_time    = current_clock_ + settings_.lls_hit_latency;
        pending_lls_miss = false;
        return;
    }
    // LLS miss + install. Charge mem_latency and remember to count
    // memory_reads at response time. Capacity eviction (if any) under
    // inclusive policy must back-invalidate the victim's sharers so the
    // L1s can't keep a copy of a line the LLS no longer covers.
    ++stats_.lls_misses;
    response_time    = current_clock_ + settings_.mem_latency;
    pending_lls_miss = true;

    if (r.evicted) {
        ++stats_.lls_evictions;
        // v0 simplification: LLS evictions do NOT trigger back-invalidates
        // to L1 holders. The agents (MSI/MESI/MOSI/MOESIF) currently
        // don't accept REQ_INVALID in non-S states, and a strict
        // inclusive policy would need either a new "back-invalidate"
        // message kind or per-agent extensions to recognize it -- both
        // are out of scope for v0. The LLS therefore acts as a soft
        // residency cache: hits on resident blocks save mem_latency vs.
        // lls_hit_latency; evictions silently drop the LLS entry while
        // L1 copies persist. The directory protocol stays correct
        // because per-line state is held in the directory entry, not in
        // the LLS itself; the LLS only accelerates the data-response
        // path. Strict-inclusion back-invalidates land in a follow-up;
        // see report_doc/10 "non-inclusive baseline + back-invalidate
        // upgrade path" for the planned mechanism.
        (void)r.victim;
    }
}

} // namespace comparch::coherence
