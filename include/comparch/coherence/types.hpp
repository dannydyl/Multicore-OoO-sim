#pragma once

// Coherence-mode type aliases. Mirrors project3_v1.1.0/simulator/types.h
// (NodeId / BlockId / Timestamp), but kept inside the coherence namespace
// so the rest of the unified sim is unaffected.

#include <cstdint>

namespace comparch::coherence {

using NodeId    = std::uint64_t;
using BlockId   = std::uint64_t;     // (64 - block_size_log2)-bit block tag
using Timestamp = std::uint64_t;

} // namespace comparch::coherence
