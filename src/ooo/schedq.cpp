#include "comparch/ooo/schedq.hpp"

namespace comparch::ooo {

SchedQ::SchedQ(std::size_t capacity) : capacity_(capacity) {}

SchedEntry* SchedQ::push_back(const SchedEntry& e) {
    entries_.push_back(e);
    return &entries_.back();
}

bool SchedQ::erase_by_tag(std::uint64_t dest_tag) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->dest_tag == dest_tag) {
            entries_.erase(it);
            return true;
        }
    }
    return false;
}

void SchedQ::wake_dependents(std::uint64_t result_tag) {
    for (auto& e : entries_) {
        if (e.src1.tag == result_tag) e.src1.ready = true;
        if (e.src2.tag == result_tag) e.src2.ready = true;
    }
}

} // namespace comparch::ooo
