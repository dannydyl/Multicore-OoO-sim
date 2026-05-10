// CoherenceAdapter — see coherence_adapter.hpp for the contract.
//
// Address conventions:
//   - cache::Cache works in "block index" units: byte_addr >> b.
//   - coherence::Message::block holds the byte address with the low
//     b bits cleared (project3 convention). The two differ by a
//     `block_size_log2` shift; we convert at every boundary.

#include "comparch/coherence/coherence_adapter.hpp"

#include <utility>

#include "comparch/coherence/node.hpp"

namespace comparch::coherence {

namespace {

// byte_addr (low bits cleared) -> cache::Cache "block index" form.
inline std::uint64_t to_cache_block(std::uint64_t byte_block_addr,
                                    std::size_t block_size_log2) {
    return byte_block_addr >> block_size_log2;
}

// cache::Cache "block index" -> byte address with low bits cleared.
inline std::uint64_t to_byte_block(std::uint64_t cache_block_addr,
                                   std::size_t block_size_log2) {
    return cache_block_addr << block_size_log2;
}

// Mark all MSHR entries for `block_addr` (cache::Cache form) ready.
// MSHR merging means there can be multiple ids on the same block;
// they all wake up together when the fill arrives.
void mark_block_ready(cache::Cache& c, std::uint64_t block_addr) {
    for (const auto& e : c.mshr().entries()) {
        if (e.valid && e.block_addr == block_addr) {
            c.mark_ready(e.id);
            for (auto sec : e.merged_ids) c.mark_ready(sec);
        }
    }
}

void cache_fill(cache::Cache& c, std::uint64_t block_addr, char rw) {
    if (c.block_in(block_addr << c.cfg().b)) return;       // already resident
    const auto tag = c.get_tag(block_addr);
    const auto idx = c.get_index(block_addr);
    c.insert_new_block(rw, tag, idx, block_addr, /*is_prefetch=*/false);
}

} // namespace

CoherenceAdapter::CoherenceAdapter(NodeId id,
                                   const Settings& s,
                                   CoherenceStats& stats,
                                   AgentFactory factory,
                                   cache::Cache& l1d,
                                   cache::Cache* l2d)
    : id_(id), settings_(s), l1d_(&l1d), l2d_(l2d),
      coh_cache_(std::make_unique<Cache>(id, s, stats, std::move(factory))) {
    coh_cache_->my_cpu = this;
}

CoherenceAdapter::~CoherenceAdapter() {
    for (auto* m : outbound_proc_) delete m;
    delete cache_in_next;
    cache_in_next = nullptr;
}

void CoherenceAdapter::on_miss(std::uint64_t block_addr, cache::Op op) {
    // block_addr is the cache::Cache block-index form. Convert to the
    // byte-cleared form the coherence layer expects.
    const auto byte_block = to_byte_block(block_addr, settings_.block_size_log2);
    const auto kind       = (op == cache::Op::Write) ? MessageKind::STORE
                                                     : MessageKind::LOAD;
    // Remember store misses so the fill response can mark L1 dirty.
    // MSHR merging means a load and a store on the same block coalesce
    // into one outstanding request; if any of the merged ops is a
    // store, the resulting line owes a writeback once it gets evicted.
    if (op == cache::Op::Write) {
        pending_stores_.insert(byte_block);
    }
    // Buffer pending requests. coh_cache->cpu_in_next is a single slot
    // and the OoO core's multi-LSU configuration may issue several
    // misses in the same cycle. tick() drains at most one per cycle
    // into the agent's mailbox — preserving the per-block agent's
    // single-message-per-cycle contract.
    outbound_proc_.push_back(new Message(id_, id_, byte_block, kind, settings_));
}

void CoherenceAdapter::on_evict(std::uint64_t block_addr, bool dirty) {
    // Cache reports block_addr in the shifted form. The directory's
    // handle_writeback distinguishes dirty (memory_writes++) vs clean
    // by the directory's own tracked state, not by the message kind,
    // so we use DATA_WB regardless. The `dirty` flag is reflected in
    // local stats only.
    (void)dirty;
    const auto byte_block = to_byte_block(block_addr, settings_.block_size_log2);
    auto* msg = new Message(id_, settings_.num_procs, byte_block,
                            MessageKind::DATA_WB, settings_);
    coh_cache_->my_node->ntwk_out_next.push_back(msg);
}

void CoherenceAdapter::tick() {
    // Drain one outbound LOAD/STORE per cycle into the agent's
    // single-slot mailbox. Anything still queued waits for the next
    // cycle.
    if (!outbound_proc_.empty() && coh_cache_->cpu_in_next == nullptr) {
        coh_cache_->cpu_in_next = outbound_proc_.front();
        outbound_proc_.pop_front();
    }

    // The agent put a DATA into cache_in_next when it served a load /
    // store completion. Treat it as the fill response: insert into
    // L2 + L1, wake the matching MSHR entries.
    if (cache_in_next) {
        const auto byte_block = cache_in_next->block;
        const auto cache_block =
            to_cache_block(byte_block, settings_.block_size_log2);

        // Recover the original op from pending_stores_: any block whose
        // miss was caused (in whole or in part) by a STORE fills L1
        // dirty so insert_new_block sets dirty=true on the new line.
        // Without this, store misses fill clean, the dirty bit is never
        // set, and evictions go silent (writebacks=0 even on workloads
        // that should produce many). L2 stays clean — write-allocate
        // dirty is L1-only; L2 dirties later when L1 writes back to it.
        const bool was_store = pending_stores_.erase(byte_block) > 0;
        if (l2d_) cache_fill(*l2d_, cache_block, /*rw=*/'R');
        cache_fill(*l1d_, cache_block, /*rw=*/was_store ? 'W' : 'R');

        // Wake every L1 (and L2) MSHR entry that was parked on this
        // block. Phase 5B's L2 has no MSHR yet, but mark_block_ready is
        // a no-op for empty MSHR tables. In shared_lls mode there is
        // no L2.
        mark_block_ready(*l1d_, cache_block);
        if (l2d_) mark_block_ready(*l2d_, cache_block);

        delete cache_in_next;
        cache_in_next = nullptr;
    }
}

void CoherenceAdapter::tock() {
    // No staging needed — the agent writes directly into cache_in_next
    // and we consume in tick() the same cycle. Kept for CpuPort symmetry.
}

bool CoherenceAdapter::is_done() const {
    return cache_in_next == nullptr && outbound_proc_.empty();
}

void CoherenceAdapter::on_ntwk_event(const Message& req) {
    const auto cache_block =
        to_cache_block(req.block, settings_.block_size_log2);
    switch (req.kind) {
        case MessageKind::REQ_INVALID:
        case MessageKind::RECALL_GOTO_I:
            l1d_->coherence_invalidate(cache_block);
            if (l2d_) l2d_->coherence_invalidate(cache_block);
            break;
        case MessageKind::RECALL_GOTO_S:
            // Downgrade-on-remote-read. RECALL_GOTO_S is overloaded
            // across protocols; the destination state determines
            // whether this core is still responsible for writeback:
            //   MSI    M->S        : clean (dir wrote back to memory)
            //   MESI   M->S, E->S  : clean
            //   MOSI   M->O, O->O  : owner stays dirty
            //   MOESIF E->F, F->F  : clean
            //   MOESIF M->O, O->O  : owner stays dirty
            // The adapter can't tell clean-dest from owner-dest from
            // the message alone, so we leave the cache's dirty bit
            // untouched. Correctness is preserved either way: on a
            // later clean eviction, on_evict ignores the cache-local
            // dirty flag and the directory decides memory_writes
            // from its own state (handle_writeback in directory.cpp),
            // so a stale dirty bit cannot cause a phantom memory
            // writeback. The only artifact is the per-cache
            // stats_.writebacks counter, which over-counts dirty
            // evictions for MSI/MESI M->S downgrades.
            break;
        default:
            break;
    }
}

} // namespace comparch::coherence
