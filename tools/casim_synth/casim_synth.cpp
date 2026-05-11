#include "casim_synth.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

namespace comparch::casim_synth {

namespace fs = std::filesystem;

// ---- ThreadHandle ----------------------------------------------------

ThreadHandle& ThreadHandle::alus(std::size_t n) {
    prog_->emit_alu_block(tid_, n);
    return *this;
}
ThreadHandle& ThreadHandle::load(std::uint64_t addr) {
    prog_->emit_load(tid_, addr);
    return *this;
}
ThreadHandle& ThreadHandle::store(std::uint64_t addr) {
    prog_->emit_store(tid_, addr);
    return *this;
}
ThreadHandle& ThreadHandle::branch(bool taken) {
    prog_->emit_branch(tid_, taken);
    return *this;
}
ThreadHandle& ThreadHandle::lock(MutexId m) {
    prog_->emit_lock(tid_, m);
    return *this;
}
ThreadHandle& ThreadHandle::unlock(MutexId m) {
    prog_->emit_unlock(tid_, m);
    return *this;
}
ThreadHandle& ThreadHandle::barrier(BarrierId b) {
    prog_->emit_barrier(tid_, b);
    return *this;
}

// ---- Program ---------------------------------------------------------

Program::Program(std::string name, std::uint32_t threads, std::uint64_t program_uid)
    : name_(std::move(name)),
      program_uid_(program_uid),
      threads_(threads) {
    if (threads == 0) {
        throw std::invalid_argument("Program: thread count must be > 0");
    }
    // Stagger per-thread PC base so an OoO log can tell at a glance
    // which thread an instruction belongs to.
    for (std::uint32_t i = 0; i < threads; ++i) {
        threads_[i].next_pc = 0x10000ULL + 0x1000000ULL * i;
    }
}

Program::~Program() = default;

MutexId Program::mutex() {
    const auto id = next_mutex_id_;
    next_mutex_id_ += 0x40;       // stride so addresses don't collide
    next_lock_seq_[id] = 0;
    return id;
}

BarrierId Program::barrier(std::uint32_t expected_participants) {
    if (expected_participants == 0) {
        throw std::invalid_argument(
            "Program::barrier: expected_participants must be > 0");
    }
    if (expected_participants > thread_count()) {
        throw std::invalid_argument(
            "Program::barrier: expected_participants exceeds thread count");
    }
    const auto id = next_barrier_id_;
    next_barrier_id_ += 0x40;
    BarrierState s;
    s.expected = expected_participants;
    barriers_[id] = s;
    return id;
}

ThreadHandle Program::t(std::uint32_t tid) {
    check_tid(tid);
    if (written_) {
        throw std::logic_error("Program::t: cannot mutate after write()");
    }
    return ThreadHandle(*this, tid);
}

void Program::check_tid(std::uint32_t tid) const {
    if (tid >= thread_count()) {
        throw std::out_of_range(
            "Program: tid=" + std::to_string(tid) +
            " >= threads=" + std::to_string(thread_count()));
    }
}

std::size_t Program::record_count(std::uint32_t tid) const {
    check_tid(tid);
    return threads_[tid].records.size();
}

std::uint32_t Program::thread_count() const {
    return static_cast<std::uint32_t>(threads_.size());
}

// ---- Emit helpers ----------------------------------------------------

void Program::emit_alu_block(std::uint32_t tid, std::size_t n) {
    check_tid(tid);
    auto& st = threads_[tid];
    st.records.reserve(st.records.size() + n);
    for (std::size_t i = 0; i < n; ++i) {
        trace::Record r{};
        r.ip = st.next_pc;
        st.next_pc += 4;
        // Give each ALU a dst reg dependency that rotates through
        // a 31-register pool. This avoids accidental zero-dependency
        // streams where the OoO core decodes everything as parallel.
        r.destination_registers[0] =
            static_cast<std::uint8_t>(1 + (i % 31));
        st.records.emplace_back(r);
    }
}

void Program::emit_load(std::uint32_t tid, std::uint64_t addr) {
    check_tid(tid);
    auto& st = threads_[tid];
    trace::Record r{};
    r.ip = st.next_pc;
    st.next_pc += 4;
    r.source_memory[0] = addr;
    r.destination_registers[0] = 1;
    st.records.emplace_back(r);
}

void Program::emit_store(std::uint32_t tid, std::uint64_t addr) {
    check_tid(tid);
    auto& st = threads_[tid];
    trace::Record r{};
    r.ip = st.next_pc;
    st.next_pc += 4;
    r.destination_memory[0] = addr;
    r.source_registers[0] = 1;
    st.records.emplace_back(r);
}

void Program::emit_branch(std::uint32_t tid, bool taken) {
    check_tid(tid);
    auto& st = threads_[tid];
    trace::Record r{};
    r.ip = st.next_pc;
    st.next_pc += 4;
    r.is_branch    = true;
    r.branch_taken = taken;
    st.records.emplace_back(r);
}

void Program::emit_lock(std::uint32_t tid, MutexId m) {
    check_tid(tid);
    auto it = next_lock_seq_.find(m);
    if (it == next_lock_seq_.end()) {
        throw std::logic_error("Program::lock: unknown MutexId (call Program::mutex() first)");
    }
    const auto seq = it->second++;
    // Real hardware acquires a lock via load + CAS on the lock word.
    // Emit those memory ops so the coherence layer sees the line
    // bouncing between cores — that's where the *timing* cost of
    // lock contention comes from in the sim. The SyncRecord that
    // follows still drives correctness (happens-before ordering).
    emit_load(tid, m);
    emit_store(tid, m);
    auto& st = threads_[tid];
    trace::SyncRecord s{trace::SyncKind::LockAcquire, m, seq, 0,
                        /*ip=*/st.next_pc, 0};
    st.next_pc += 4;
    st.records.emplace_back(s);
    st.held_locks.emplace_back(m, seq);
}

void Program::emit_unlock(std::uint32_t tid, MutexId m) {
    check_tid(tid);
    auto& st = threads_[tid];
    if (st.held_locks.empty() || st.held_locks.back().first != m) {
        throw std::logic_error(
            "Program::unlock: no matching prior lock on this thread for mutex");
    }
    const auto seq = st.held_locks.back().second;
    st.held_locks.pop_back();
    // Release: store-0 to lock word. Forces M→S transitions on any
    // future readers (a contending core's load will pull a fresh
    // line via coherence).
    emit_store(tid, m);
    trace::SyncRecord s{trace::SyncKind::LockRelease, m, seq, 0,
                        /*ip=*/st.next_pc, 0};
    st.next_pc += 4;
    st.records.emplace_back(s);
}

void Program::emit_barrier(std::uint32_t tid, BarrierId b) {
    check_tid(tid);
    auto it = barriers_.find(b);
    if (it == barriers_.end()) {
        throw std::logic_error(
            "Program::barrier: unknown BarrierId (call Program::barrier() first)");
    }
    auto& bs = it->second;
    const auto iter = bs.iter;
    ++bs.arrived_this_iter;
    if (bs.arrived_this_iter >= bs.expected) {
        bs.arrived_this_iter = 0;
        ++bs.iter;
    }
    auto& st = threads_[tid];
    trace::SyncRecord arrive{trace::SyncKind::BarrierArrive, b, iter,
                             bs.expected, /*ip=*/st.next_pc, 0};
    st.next_pc += 4;
    st.records.emplace_back(arrive);
    trace::SyncRecord leave{trace::SyncKind::BarrierLeave, b, iter, 0,
                            /*ip=*/st.next_pc, 0};
    st.next_pc += 4;
    st.records.emplace_back(leave);
}

// ---- Write -----------------------------------------------------------

void Program::write(const fs::path& out_dir) {
    if (written_) {
        throw std::logic_error("Program::write: already written");
    }
    fs::create_directories(out_dir);

    // One .casim per thread.
    std::vector<fs::path> tpaths;
    tpaths.reserve(threads_.size());
    for (std::uint32_t i = 0; i < thread_count(); ++i) {
        const auto p = out_dir / ("t" + std::to_string(i) + ".casim");
        trace::Writer w(p, trace::Variant::CasimV2);
        trace::FileHeader h;
        h.thread_id    = i;
        h.thread_count = thread_count();
        h.program_uid  = program_uid_;
        w.write_header(h);
        for (const auto& rec : threads_[i].records) {
            std::visit([&](const auto& r) { w.write(r); }, rec);
        }
        w.flush();
        tpaths.push_back(p);
    }

    // program.manifest. Use the name passed in the constructor.
    const auto mpath = out_dir / (name_ + ".manifest");
    {
        std::ofstream mf(mpath);
        if (!mf) {
            throw std::runtime_error(
                "casim_synth: cannot open manifest for writing: " + mpath.string());
        }
        mf << "# generated by casim_synth\n";
        mf << "program: " << name_ << "\n";
        mf << "threads: " << thread_count() << "\n";
        mf << "program_uid: " << program_uid_ << "\n";
        for (std::uint32_t i = 0; i < thread_count(); ++i) {
            mf << "t" << i << ": "
               << tpaths[i].filename().string() << "\n";
        }
    }
    written_ = true;
}

} // namespace comparch::casim_synth
