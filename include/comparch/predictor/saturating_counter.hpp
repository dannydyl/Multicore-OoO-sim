#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace comparch::predictor {

// A textbook N-bit saturating up/down counter, the building block of essentially
// every direction predictor in this subsystem.
//
// State semantics (for an N-bit counter, valid range [0, 2^N - 1]):
//   - "taken" if value >= 2^(N-1)         (top half of the range)
//   - "weakly taken"     if value == 2^(N-1)
//   - "weakly not taken" if value == 2^(N-1) - 1
//   - "strongly taken"   if value == 2^N - 1   (saturated upward)
//   - "strongly not taken" if value == 0       (saturated downward)
//
// update(taken) moves the counter one step toward the matching saturation end:
// taken increments, not-taken decrements; both clamp at the boundary rather
// than wrapping. This is the same Smith counter behavior as project2's Counter,
// just rewritten to be header-only and to avoid the project2 reset() trick that
// relied on uint64_t underflow.
class SaturatingCounter {
public:
    SaturatingCounter(int bits, unsigned init)
        : bits_(bits), value_(init) {
        // Runtime-checked precondition (vs. assert) so the bound survives
        // -DNDEBUG. The factory layers already validate user-driven sizes;
        // these throws only fire on internal misuse.
        if (bits_ < 1 || bits_ > 31) {
            throw std::invalid_argument(
                "SaturatingCounter: bits must be in [1, 31] (got "
                + std::to_string(bits_) + ")");
        }
        if (value_ > max()) {
            throw std::invalid_argument(
                "SaturatingCounter: init " + std::to_string(value_)
                + " exceeds max " + std::to_string(max())
                + " for " + std::to_string(bits_) + "-bit counter");
        }
    }

    // The widest representable value, 2^bits - 1.
    unsigned max() const { return (1u << bits_) - 1u; }

    // Numeric counter value (0 .. max()).
    unsigned value() const { return value_; }

    // Number of bits this counter occupies.
    int bits() const { return bits_; }

    // True if the counter is in the upper half of its range (>= 2^(N-1)).
    bool is_taken() const { return value_ >= (1u << (bits_ - 1)); }

    // True if the counter is in either weakly-taken or weakly-not-taken state,
    // i.e. one transition away from flipping its prediction.
    bool is_weak() const {
        const unsigned weak_taken = 1u << (bits_ - 1);
        return value_ == weak_taken || value_ == weak_taken - 1u;
    }

    // Saturating up/down step. Note: the only place this differs from a plain
    // increment/decrement is at the clamps — we never wrap.
    void update(bool taken) {
        if (taken && value_ < max()) {
            ++value_;
        } else if (!taken && value_ > 0) {
            --value_;
        }
    }

    // Reset to weakly-taken (taken = true) or weakly-not-taken (false). Useful
    // for initializing tournament tables without hardcoding the magic value.
    void reset(bool weakly_taken) {
        const unsigned weak_taken = 1u << (bits_ - 1);
        value_ = weakly_taken ? weak_taken : (weak_taken - 1u);
    }

private:
    int bits_;
    unsigned value_;
};

} // namespace comparch::predictor
