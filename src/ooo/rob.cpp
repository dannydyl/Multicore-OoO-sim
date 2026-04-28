#include "comparch/ooo/rob.hpp"

namespace comparch::ooo {

Rob::Rob(std::size_t capacity)
    : entries_(capacity), capacity_(capacity) {}

std::size_t Rob::allocate(const RobEntry& e) {
    const std::size_t idx = tail_;
    entries_[idx] = e;
    tail_ = (tail_ + 1) % capacity_;
    ++count_;
    return idx;
}

void Rob::retire_head() {
    head_ = (head_ + 1) % capacity_;
    --count_;
}

} // namespace comparch::ooo
