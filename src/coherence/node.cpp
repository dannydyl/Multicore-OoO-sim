// Node. Mirrors project3/simulator/node.cpp:54-230 (the active branch
// without the OLDNTWK macro). Ring routing computes the shortest hop;
// each port can carry one in-flight Message at a time and stalls on
// contention. The half-cycle ordering is preserved — *_next drained
// to * in tock; tof decrement happens in tock; arrivals dispatched in
// tick.

#include "comparch/coherence/node.hpp"

#include "comparch/coherence/network.hpp"

namespace comparch::coherence {

Node::Node(NodeId id, bool is_dir,
           CpuPort* cpu, Cache* cache,
           std::unique_ptr<DirectoryController> dir,
           Network* ntwk)
    : id_(id), is_dir_(is_dir), cpu_(cpu), cache_(cache),
      dir_(std::move(dir)), ntwk_(ntwk) {
    // RING_TOP only in Phase 5A — XBAR rejected at coherence_mode entry.
    num_ports = 2;
    outgoing_packets.assign(num_ports, nullptr);
    incoming_packets.assign(num_ports, nullptr);
}

Node::~Node() {
    for (auto* m : ntwk_out_next)  delete m;
    for (auto* m : ntwk_out_queue) delete m;
    for (auto* m : ntwk_in_next)   delete m;
    for (auto* m : ntwk_in_queue)  delete m;
    // outgoing_packets / incoming_packets aliases live in ntwk_*_queue or
    // are otherwise tracked elsewhere; we don't double-free here.
}

bool Node::is_done() {
    return cpu_ ? cpu_->is_done() : true;
}

void Node::tick(Timestamp clock) {
    // 1) Tick the local engine (CPU + Cache, or the Directory).
    if (!is_dir_) {
        cpu_->tick();
        cache_->tick();
    } else {
        dir_->tick(clock);
    }

    // 2) Drain ntwk_out_queue onto the ring, respecting per-port contention.
    if (!ntwk_out_queue.empty()) {
        int sent = 0;
        for (auto it = ntwk_out_queue.begin(); it != ntwk_out_queue.end(); ++it) {
            Message* r = *it;
            // Ring routing: pick the shorter direction. Project3 ties
            // distance > N/2 -> go left (port 0); else right (port 1).
            const std::uint64_t N = ntwk_->num_nodes();
            const std::uint64_t distance = (r->dst + N - id_) % N;
            const NodeId target =
                (((distance > N / 2) ? id_ - 1 : id_ + 1) + N) % N;
            const std::size_t out_port = (distance > N / 2) ? 0 : 1;
            const std::size_t rcv_port = 1 - out_port;

            if (!outgoing_packets[out_port] &&
                !ntwk_->node(target).incoming_packets[rcv_port]) {
                outgoing_packets[out_port] = r;
                ntwk_->node(target).incoming_packets[rcv_port] = r;
                r->tof = r->flits;
                ++sent;
            } else {
                break; // ring stalls; preserve per-cycle FIFO order.
            }
        }
        for (int i = 0; i < sent; ++i) ntwk_out_queue.pop_front();
    }

    // 3) Pull arrived flits off incoming ports.
    for (std::size_t i = 0; i < num_ports; ++i) {
        Message* r = incoming_packets[i];
        if (!r) continue;
        if (r->tof <= 0) {
            if (r->dst == id_) {
                ntwk_in_queue.push_back(r);    // arrived
            } else {
                ntwk_out_queue.push_back(r);   // forward another hop
            }
            incoming_packets[i] = nullptr;
        }
    }

    // 4) Hand a message to the local agent at most once per cycle.
    for (std::size_t i = 0; i < num_ports; ++i) {
        if (ntwk_in_queue.empty()) break;
        if (is_dir_) {
            Message* r = ntwk_in_queue.front();
            ntwk_in_queue.pop_front();
            dir_->request_next.push_back(r);
        } else {
            if (cache_->ntwk_in_next == nullptr) {
                Message* r = ntwk_in_queue.front();
                ntwk_in_queue.pop_front();
                cache_->ntwk_in_next = r;
            } else {
                break; // cache can only digest one message per cycle.
            }
        }
    }
}

void Node::tock() {
    if (!is_dir_) {
        cpu_->tock();
        cache_->tock();
    } else {
        dir_->tock();
    }

    // Drain ntwk_out_next into ntwk_out_queue.
    ntwk_out_queue.splice(ntwk_out_queue.end(), ntwk_out_next);

    // Decrement tof on every outgoing packet; clear when done.
    for (std::size_t i = 0; i < num_ports; ++i) {
        Message* r = outgoing_packets[i];
        if (!r) continue;
        --r->tof;
        if (r->tof <= 0) outgoing_packets[i] = nullptr;
    }

    // Drain ntwk_in_next into ntwk_in_queue.
    ntwk_in_queue.splice(ntwk_in_queue.end(), ntwk_in_next);
}

} // namespace comparch::coherence
