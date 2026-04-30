#pragma once

// Directory controller. Mirrors
// project3/coherence/directory/Directory_Controller.h.
//
// Phase 5A: structural port — request queues, get_entry, poll/dequeue/
// cycle_queue/send_Request, and a tick() that dispatches by protocol.
// The per-protocol *_tick() methods are filled in incrementally
// (Step 4 = MSI, Step 5 = MI, Step 6 = MESI, Step 7 = MOSI, Step 8 =
// MOESIF). The "send_E / send_F" handshake and the directory-entry
// presence vectors are kept here verbatim because every protocol's
// tick depends on them.

#include <cstdint>
#include <list>
#include <unordered_map>

#include "comparch/coherence/coherence_stats.hpp"
#include "comparch/coherence/message.hpp"
#include "comparch/coherence/settings.hpp"
#include "comparch/coherence/types.hpp"

namespace comparch::coherence {

class Node;

// Project3 directory states (project3/coherence/directory/Directory_Controller.h).
// Exact names + ordinal positions are preserved so any per-protocol switch
// statement can be ported without translation.
enum class DirState {
    M = 0, O, S, I, F,
    FS, FM, SM, MO, SS, MS, MM, OM,
    E, ES, EM, OO, EF, FF,
    NUM_DIR_STATE
};

constexpr std::size_t kMaxSharers = 16;

struct DirEntry {
    BlockId block_id = 0;
    bool dirty = false;
    bool presence[kMaxSharers] = {};
    DirState state = DirState::I;
    std::uint32_t active_sharers = 0;
    NodeId req_node_in_transient = 0;
    NodeId o_f_id = 0;
    std::int32_t inv_ack_waiting = 0;
    bool getM_upgrade = false;
};

class DirectoryController {
public:
    DirectoryController(NodeId id, const Settings& s, CoherenceStats& stats);
    ~DirectoryController();

    DirectoryController(const DirectoryController&) = delete;
    DirectoryController& operator=(const DirectoryController&) = delete;

    NodeId id() const { return id_; }

    Node* my_node = nullptr;

    std::list<Message*> request_next;
    std::list<Message*> request_queue;

    // Per-protocol cross-cycle handshake state — preserved verbatim
    // from project3 because the per-protocol ticks set/read these.
    bool      send_E = false;
    bool      send_F = false;
    BlockId   tag_to_send = 0;
    NodeId    target_node = 0;
    bool      request_in_progress = false;
    Timestamp response_time = 0;

    void tick(Timestamp clock);
    void tock();

    DirEntry* get_entry(BlockId block);
    Message*  poll_queue();
    void      dequeue();
    void      cycle_queue();
    void      send_Request(NodeId dest, BlockId tag, MessageKind kind);

    // Phase 5B: an L1+L2 in a real CMP can self-evict. The
    // CoherenceAdapter sends DATA_WB (= "WRITEBACK") on any eviction;
    // this helper folds in protocol-agnostic bookkeeping (memory_writes
    // for dirty drops, presence-bit clear, state collapse to I when the
    // last sharer leaves). Returns true if the message was consumed
    // (dequeued or cycled). All protocol ticks call this first.
    bool handle_writeback(DirEntry* entry, const Message& request);

    // Per-protocol implementations (Steps 4-8 fill these in). Each reads
    // current_clock_ for memory-latency comparisons.
    void MI_tick();
    void MSI_tick();
    void MESI_tick();
    void MOSI_tick();
    void MOESIF_tick();

    Timestamp current_clock() const { return current_clock_; }

    const Settings&       settings() const { return settings_; }
    CoherenceStats&       stats()          { return stats_; }
    const CoherenceStats& stats()    const { return stats_; }

private:
    NodeId            id_;
    const Settings&   settings_;
    CoherenceStats&   stats_;
    Timestamp         current_clock_ = 0;
    std::unordered_map<BlockId, DirEntry*> directory_;
};

const char* dir_state_str(DirState s);

} // namespace comparch::coherence
