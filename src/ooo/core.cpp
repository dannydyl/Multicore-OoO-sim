// OooCore — single-core out-of-order pipeline implementation.
// ===========================================================
// Direct port of project2's procsim.cpp (the per-stage logic at lines
// 300-1000), refactored as a class and re-pointed at:
//
//   - the Phase-3 BranchPredictor (predict at fetch, update at retire)
//   - the Phase-4 Cache MSHR API for L1-D loads
//
// Everything outside those two integration points mirrors project2's
// behavior verbatim, including the reverse-stage cycle order and the
// memory-disambiguation rules in stage_schedule.

#include "comparch/ooo/core.hpp"

#include <algorithm>

#include "comparch/cache/mem_req.hpp"
#include "comparch/cache/mshr.hpp"

namespace comparch::ooo {

OooCore::OooCore(OooConfig cfg,
                 predictor::BranchPredictor& pred,
                 cache::Cache& l1d,
                 trace::Reader& trace)
    : cfg_(cfg),
      pred_(&pred),
      l1d_(&l1d),
      trace_(&trace),
      sq_(cfg.schedq_entries_per_fu * (cfg.alu_fus + cfg.mul_fus + cfg.lsu_fus)),
      rob_(cfg.rob_entries),
      alu_(cfg.alu_fus),
      mul_(cfg.mul_fus),
      lsu_(cfg.lsu_fus),
      schedq_capacity_(cfg.schedq_entries_per_fu *
                       (cfg.alu_fus + cfg.mul_fus + cfg.lsu_fus)) {}

bool OooCore::tick() {
    // Stages run in REVERSE program order. retire first so this cycle
    // sees freshly-completed instructions; fetch last so newly-fetched
    // ones don't try to dispatch in the same cycle they arrive.
    bool mispred_retired = false;
    const std::uint64_t retired_this_cycle = stage_state_update(mispred_retired);

    if (mispred_retired) {
        ++stats_.branch_mispredictions;
        // After a mispredicted retire, project2 skips the rest of the
        // cycle's stages (procsim.cpp:957-963). The mispredicted-fetch
        // flag is cleared inside stage_state_update().
    } else {
        stage_exec();
        stage_schedule();
        stage_dispatch();
        stage_fetch();
    }

    // Per-cycle bookkeeping for utilization stats and termination.
    stats_.dispq_max  = std::max<std::uint64_t>(stats_.dispq_max,  dispq_.size());
    stats_.schedq_max = std::max<std::uint64_t>(stats_.schedq_max, sq_.size());
    stats_.rob_max    = std::max<std::uint64_t>(stats_.rob_max,    rob_.size());
    stats_.dispq_avg_sum  += static_cast<double>(dispq_.size());
    stats_.schedq_avg_sum += static_cast<double>(sq_.size());
    stats_.rob_avg_sum    += static_cast<double>(rob_.size());
    ++stats_.cycles;

    // Tick the cache at end-of-cycle so MSHR readiness is observable on
    // the *next* cycle's exec stage. This matches project2's implicit
    // model where left_cycles decrements between exec calls.
    l1d_->tick();

    (void)retired_this_cycle;

    // The pipeline is "done" when the trace is exhausted AND nothing is
    // left in any of the queues.
    return !(eof_ && dispq_.empty() && rob_.empty() && sq_.empty());
}

void OooCore::run(std::uint64_t cycle_cap) {
    while (tick()) {
        if (cycle_cap != 0 && stats_.cycles >= cycle_cap) break;
    }
}

// ---------------------------------------------------------------------------
// stage_state_update — retire (procsim.cpp:300-333).
// ---------------------------------------------------------------------------
std::uint64_t OooCore::stage_state_update(bool& mispred_retired_out) {
    mispred_retired_out = false;
    std::uint64_t retired = 0;

    while (!rob_.empty() && rob_.head_entry().ready) {
        const auto& head = rob_.head_entry();

        if (head.inst.opcode == Opcode::Branch) {
            // Train the predictor on the actual outcome. The wrong-path
            // branches that were fetched between this branch's mispredict
            // and its retire never reach this point — they're sitting
            // un-fetched because in_mispred_ blocked fetch, so they
            // can't pollute predictor state.
            const predictor::Branch b{
                .ip       = head.inst.pc,
                .taken    = head.inst.branch_taken,
                .inst_num = head.inst.dyn_count,
            };
            pred_->update(b, head.inst.predicted_taken);
            ++stats_.num_branch_instructions;

            if (head.inst.mispredict) {
                rat_.flush_to_ready();
                mispred_retired_out = true;
                in_mispred_ = false;          // un-block fetch next cycle
                rob_.retire_head();
                ++retired;
                ++stats_.instructions_retired;
                break;
            }
        }

        rob_.retire_head();
        ++retired;
        ++stats_.instructions_retired;
    }

    return retired;
}

// ---------------------------------------------------------------------------
// stage_exec — advance FUs and complete instructions (procsim.cpp:340-493).
// ---------------------------------------------------------------------------
void OooCore::stage_exec() {
    // ----- ALU (1 stage) -----
    for (auto& u : alu_) {
        if (u.busy) {
            writeback(u.sched_ptr);
            u.busy      = false;
            u.sched_ptr = nullptr;
        }
    }

    // ----- MUL (3-stage pipelined) -----
    // First: complete anything in stage 3.
    for (auto& u : mul_) {
        if (u.busy_stage[2]) {
            writeback(u.sched_at_stage[2]);
            u.busy_stage[2]    = false;
            u.sched_at_stage[2] = nullptr;
        }
    }
    // Then: shift stage1->stage2->stage3, freeing stage1.
    for (auto& u : mul_) {
        u.busy_stage[2]      = u.busy_stage[1];
        u.sched_at_stage[2]  = u.sched_at_stage[1];
        u.busy_stage[1]      = u.busy_stage[0];
        u.sched_at_stage[1]  = u.sched_at_stage[0];
        u.busy_stage[0]      = false;
        u.sched_at_stage[0]  = nullptr;
    }

    // ----- LSU -----
    // Stores complete in 1 cycle (project2 frees them immediately on
    // exec). Loads poll the L1-D MSHR; complete when ready.
    for (auto& u : lsu_) {
        if (!u.busy) continue;

        if (!u.is_load) {
            // Store completion: project2 only sets ROB ready and removes
            // from schedQ — no CDB broadcast (stores don't write a reg).
            rob_[u.sched_ptr->rob_idx].ready = true;
            sq_.erase_by_tag(u.sched_ptr->dest_tag);
            u.busy      = false;
            u.sched_ptr = nullptr;
            continue;
        }

        // Load: poll MSHR.
        const cache::MSHREntry* mshr = l1d_->peek(u.mshr_id);
        if (mshr == nullptr || !mshr->ready) continue;   // wait

        writeback(u.sched_ptr);
        l1d_->complete(u.mshr_id);
        u.busy      = false;
        u.sched_ptr = nullptr;
        u.mshr_id   = 0;
    }
}

void OooCore::writeback(SchedEntry* sched) {
    rat_.mark_complete(sched->inst.dest, sched->dest_tag);
    rob_[sched->rob_idx].ready = true;
    sq_.wake_dependents(sched->dest_tag);
    sq_.erase_by_tag(sched->dest_tag);
}

// ---------------------------------------------------------------------------
// stage_schedule — fire ready instructions into FUs (procsim.cpp:503-660).
// ---------------------------------------------------------------------------
void OooCore::stage_schedule() {
    int num_fires = 0;

    // Helper: pick the oldest ready entry of a given opcode predicate.
    auto pick_oldest = [&](auto pred) -> SchedEntry* {
        SchedEntry* oldest = nullptr;
        for (auto& e : sq_.entries()) {
            if (e.busy) continue;
            if (!pred(e)) continue;
            if (!e.src1.ready || !e.src2.ready) continue;
            if (oldest == nullptr || e.inst.dyn_count < oldest->inst.dyn_count) {
                oldest = &e;
            }
        }
        return oldest;
    };

    // ----- ALU (handles ADD/ALU + BRANCH, per project2 line 536) -----
    const std::size_t alu_avail = free_alu_count(alu_);
    for (std::size_t i = 0; i < alu_avail; ++i) {
        auto* oldest = pick_oldest([](const SchedEntry& e) {
            return e.inst.opcode == Opcode::Alu ||
                   e.inst.opcode == Opcode::Branch;
        });
        if (!oldest) break;

        for (auto& u : alu_) {
            if (!u.busy) {
                u.busy      = true;
                u.sched_ptr = oldest;
                oldest->busy = true;
                ++num_fires;
                break;
            }
        }
    }

    // ----- MUL -----
    const std::size_t mul_avail = free_mul_count(mul_);
    for (std::size_t i = 0; i < mul_avail; ++i) {
        auto* oldest = pick_oldest([](const SchedEntry& e) {
            return e.inst.opcode == Opcode::Mul;
        });
        if (!oldest) break;

        for (auto& u : mul_) {
            if (!u.busy_stage[0]) {
                u.busy_stage[0]     = true;
                u.sched_at_stage[0] = oldest;
                oldest->busy        = true;
                ++num_fires;
                break;
            }
        }
    }

    // ----- LSU with memory disambiguation -----
    // Conservative ordering (project2 lines 596-620):
    //   - A load may fire only if no older STORE is in the schedQ.
    //   - A store may fire only if no older LOAD or STORE is in the
    //     schedQ.
    // Plus: at most one store fires per cycle (project2 line 623).
    bool store_fired = false;
    const std::size_t lsu_avail = free_lsu_count(lsu_);
    for (std::size_t i = 0; i < lsu_avail; ++i) {
        SchedEntry* oldest = nullptr;
        for (auto& e : sq_.entries()) {
            if (e.busy) continue;
            if (e.inst.opcode != Opcode::Load && e.inst.opcode != Opcode::Store) continue;
            if (!e.src1.ready || !e.src2.ready) continue;

            // Memory disambiguation walk.
            bool ambiguous = false;
            for (auto& other : sq_.entries()) {
                if (&other == &e) continue;
                if (other.inst.dyn_count >= e.inst.dyn_count) continue;
                if (e.inst.opcode == Opcode::Load) {
                    if (other.inst.opcode == Opcode::Store) ambiguous = true;
                } else { // Store
                    if (other.inst.opcode == Opcode::Store ||
                        other.inst.opcode == Opcode::Load) ambiguous = true;
                }
                if (ambiguous) break;
            }
            if (ambiguous) continue;

            if (oldest == nullptr || e.inst.dyn_count < oldest->inst.dyn_count) {
                oldest = &e;
            }
        }

        if (!oldest) break;
        if (oldest->inst.opcode == Opcode::Store && store_fired) break;

        for (auto& u : lsu_) {
            if (u.busy) continue;
            u.busy      = true;
            u.is_load   = (oldest->inst.opcode == Opcode::Load);
            u.sched_ptr = oldest;

            if (u.is_load) {
                // Issue the load to L1-D. The MSHR's due_cycle reflects
                // the real round-trip latency; the load's exec polling
                // loop completes when peek(mshr_id)->ready flips.
                cache::MemReq req{};
                req.addr = oldest->inst.mem_addr;
                req.op   = cache::Op::Read;
                req.pc   = oldest->inst.pc;
                auto id = l1d_->issue(req);
                if (!id) {
                    // MSHR full: stall this cycle. Project2 has no MSHR
                    // pressure model so we degrade gracefully — undo the
                    // FU allocation and try again next cycle.
                    u.busy = false;
                    u.sched_ptr = nullptr;
                    break;
                }
                u.mshr_id = *id;
            } else {
                // Store: synchronous write through the cache (state
                // mutations + writeback bookkeeping happen now).
                cache::MemReq req{};
                req.addr = oldest->inst.mem_addr;
                req.op   = cache::Op::Write;
                req.pc   = oldest->inst.pc;
                (void)l1d_->access(req);
                store_fired = true;
            }

            oldest->busy = true;
            ++num_fires;
            break;
        }
    }

    if (num_fires == 0) ++stats_.no_fire_cycles;
}

// ---------------------------------------------------------------------------
// stage_dispatch — pull from dispq into schedQ + ROB (procsim.cpp:671-795).
// ---------------------------------------------------------------------------
void OooCore::stage_dispatch() {
    bool stopped_on_rob = false;
    std::size_t inst_dispatched = 0;

    while (!dispq_.empty()) {
        if (inst_dispatched >= cfg_.fetch_width) break;
        if (sq_.size() >= schedq_capacity_) break;
        if (rob_.full()) { stopped_on_rob = true; break; }

        Inst inst = dispq_.front();
        dispq_.pop_front();

        // Build schedQ entry. Source operands read their current
        // (tag, ready) from the RAT; destination gets a fresh tag.
        SchedEntry se{};
        se.inst     = inst;
        se.dest_tag = rat_.allocate_tag();

        const RatEntry r1 = rat_.read(inst.src1);
        se.src1 = SrcOperand{ r1.tag, r1.ready };
        const RatEntry r2 = rat_.read(inst.src2);
        se.src2 = SrcOperand{ r2.tag, r2.ready };

        // ROB entry mirrors the sched entry's metadata.
        RobEntry re{};
        re.dest_reg = inst.dest;
        re.dest_tag = se.dest_tag;
        re.inst     = inst;
        re.ready    = false;
        const std::size_t rob_idx = rob_.allocate(re);
        se.rob_idx = rob_idx;

        // Mark the destination architectural register as in-flight in
        // the RAT before pushing — younger dispatches in this same
        // cycle's pass should see the new tag.
        rat_.write_use(inst.dest, se.dest_tag);

        sq_.push_back(se);
        ++inst_dispatched;
    }

    if (stopped_on_rob) ++stats_.rob_no_dispatch_cycles;
}

// ---------------------------------------------------------------------------
// stage_fetch — read trace records, predict, push to dispq.
// ---------------------------------------------------------------------------
void OooCore::stage_fetch() {
    if (in_mispred_ || eof_) {
        // project2's DRIVER_READ_MISPRED path: do nothing this cycle
        // (the mispredicted branch itself was already pushed last
        // time). Wait for retire to flush.
        return;
    }

    for (std::size_t i = 0; i < cfg_.fetch_width; ++i) {
        trace::Record rec{};
        if (!trace_->next(rec)) {
            eof_ = true;
            return;
        }

        ++dyn_count_;
        Inst inst = classify(rec, dyn_count_);

        if (inst.opcode == Opcode::Branch) {
            const predictor::Branch b{
                .ip       = inst.pc,
                .taken    = inst.branch_taken,
                .inst_num = inst.dyn_count,
            };
            inst.predicted_taken = pred_->predict(b);
            inst.mispredict      = (inst.predicted_taken != inst.branch_taken);
        }

        dispq_.push_back(inst);
        ++stats_.instructions_fetched;

        if (inst.mispredict) {
            // Push the mispredicted branch itself, then stop fetching
            // until retire flushes the pipeline.
            in_mispred_ = true;
            return;
        }
    }
}

} // namespace comparch::ooo
