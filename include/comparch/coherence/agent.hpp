#pragma once

// Per-block coherence agent base class. Mirrors
// project3/simulator/agent.h. Each protocol (MI / MSI / MESI / MOSI /
// MOESIF) provides a concrete subclass that implements
// process_proc_request and process_ntwk_request.
//
// The agent does NOT own the Message it processes — Cache::tick deletes
// the message after dispatch (matching project3). Outgoing messages
// are pushed into Node::ntwk_out_next via the Cache reference.

#include "comparch/coherence/message.hpp"
#include "comparch/coherence/types.hpp"

namespace comparch::coherence {

class Cache;

class Agent {
public:
    Agent(NodeId id, Cache* cache, BlockId block);
    virtual ~Agent() = default;

    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    virtual void process_proc_request(const Message& req) = 0;
    virtual void process_ntwk_request(const Message& req) = 0;

protected:
    // Network-side helpers — push a Message into the local node's
    // ntwk_out_next queue, addressed to the directory (id = num_procs).
    void send_GETS(BlockId block);
    void send_GETM(BlockId block);
    void send_GETX(BlockId block);
    void send_INVACK(BlockId block);
    void send_DATA_dir(BlockId block);
    // CPU-side helper — short-circuits the local cache->cpu hand-off
    // (no network traversal, matching project3's send_DATA_proc).
    void send_DATA_proc(BlockId block);

    NodeId   id_;
    Cache*   cache_;
    BlockId  block_;
};

} // namespace comparch::coherence
