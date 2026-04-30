#pragma once

// Runtime knobs threaded through Network / Node / Cache / Agent /
// Directory by reference, replacing project3's global `settings` /
// `sim` singletons.

#include <cstddef>
#include <string>

#include "comparch/coherence/types.hpp"

namespace comparch::coherence {

enum class Protocol { MI, MSI, MESI, MOSI, MOESIF };

// Lower-case string -> Protocol. Throws ConfigError on bad input.
Protocol parse_protocol(const std::string& s);
const char* protocol_label(Protocol p);   // "MSI_PRO", "MESI_PRO", ...

struct Settings {
    Protocol protocol      = Protocol::MSI;
    NodeId   num_procs     = 4;        // CPUs only; the directory adds one more node
    std::size_t mem_latency     = 100; // cycles
    std::size_t block_size_log2 = 6;   // 64-byte lines
    std::size_t link_width_log2 = 3;   // 8-byte links
    // Derived; computed from the two log2 fields at construction.
    std::size_t header_flits   = 0;    // 1 << (4 - link_width_log2)
    std::size_t payload_flits  = 0;    // 1 << (block_size_log2 - link_width_log2)
};

// Compute header_flits / payload_flits from the *_log2 fields.
void finalize_settings(Settings& s);

} // namespace comparch::coherence
