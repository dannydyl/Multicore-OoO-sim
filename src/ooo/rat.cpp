#include "comparch/ooo/rat.hpp"

namespace comparch::ooo {

Rat::Rat() : entries_(kNumArchRegs) {
    // Project2 seeds each architectural register with a unique ready
    // tag (1..NUM_REGS) so that the first dispatch reads a defined
    // (tag, ready=true) pair. We mirror that: tag i+1 for register i.
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        entries_[i].tag   = static_cast<std::uint64_t>(i) + 1;
        entries_[i].ready = true;
    }
}

RatEntry Rat::read(std::int8_t addr) const {
    if (addr == kNoReg) {
        return RatEntry{};   // {tag=0, ready=true}
    }
    return entries_[static_cast<std::size_t>(addr)];
}

void Rat::write_use(std::int8_t addr, std::uint64_t tag) {
    if (addr == kNoReg) return;
    auto& e = entries_[static_cast<std::size_t>(addr)];
    e.tag   = tag;
    e.ready = false;
}

void Rat::mark_complete(std::int8_t addr, std::uint64_t tag) {
    if (addr == kNoReg) return;
    auto& e = entries_[static_cast<std::size_t>(addr)];
    if (e.tag == tag) {
        e.ready = true;
    }
}

void Rat::flush_to_ready() {
    for (auto& e : entries_) {
        e.ready = true;
    }
}

std::uint64_t Rat::allocate_tag() {
    return next_tag_++;
}

} // namespace comparch::ooo
