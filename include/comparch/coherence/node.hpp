#pragma once

// Node. Mirrors project3/simulator/node.h. Each Node is either a CPU
// node (FiciCpu + Cache) or a directory node (DirectoryController).
//
// The four message queues (ntwk_out_next, ntwk_out_queue, ntwk_in_next,
// ntwk_in_queue) and the per-port outgoing/incoming packet arrays are
// the timing-critical piece — flit counts, port contention, and tof
// decrements drive the cycle counter that the parity test pins.

#include <cstdint>
#include <list>
#include <memory>
#include <vector>

#include "comparch/coherence/coherence_cache.hpp"
#include "comparch/coherence/coherence_stats.hpp"
#include "comparch/coherence/cpu_port.hpp"
#include "comparch/coherence/directory.hpp"
#include "comparch/coherence/message.hpp"
#include "comparch/coherence/settings.hpp"
#include "comparch/coherence/types.hpp"

namespace comparch::coherence {

class Network;

class Node {
public:
    // Directory node: pass `is_dir = true`, all other slots null.
    // CPU node: pass `is_dir = false` plus cpu/cache pointers (Network owns
    // the CPU and Cache; Node holds non-owning pointers).
    Node(NodeId id, bool is_dir,
         CpuPort* cpu, Cache* cache,
         std::unique_ptr<DirectoryController> dir,
         Network* ntwk);
    ~Node();

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    NodeId id() const { return id_; }
    bool   is_dir() const { return is_dir_; }
    CpuPort*             cpu()   { return cpu_; }
    Cache*               cache() { return cache_; }
    DirectoryController* dir()   { return dir_.get(); }

    // Four queues drive ring-network message movement (see project3 node.h
    // comments for the half-cycle ordering contract).
    std::list<Message*> ntwk_out_next;
    std::list<Message*> ntwk_out_queue;
    std::list<Message*> ntwk_in_queue;
    std::list<Message*> ntwk_in_next;

    // Per-port outgoing and incoming "in-flight" slots, one per ring port.
    // Size is fixed at 2 for RING. Each slot holds a non-owning pointer
    // to a Message that's currently traversing this hop (tof > 0).
    std::vector<Message*> outgoing_packets;
    std::vector<Message*> incoming_packets;
    std::size_t num_ports = 0;

    bool is_done();
    void tick(Timestamp clock);
    void tock();

private:
    NodeId           id_;
    bool             is_dir_;
    CpuPort*         cpu_;     // nullable; not owned. FiciCpu (Phase 5A) or
                               // CoherenceAdapter (Phase 5B).
    Cache*           cache_;   // nullable; not owned
    std::unique_ptr<DirectoryController> dir_;   // owned
    Network*         ntwk_;
};

} // namespace comparch::coherence
