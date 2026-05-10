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

// Cache-hierarchy topology. PrivateL2 is the original Phase 5B design;
// SharedLls is the Phase 6 hybrid (per-core L1 + shared LLS with
// directory inside it).
enum class CacheMode { PrivateL2, SharedLls };
CacheMode parse_cache_mode(const std::string& s);
const char* cache_mode_label(CacheMode m);   // "private_l2" / "shared_lls"

// LLS inclusion policy. Only meaningful when cache_mode = SharedLls.
// Inclusive is the v0 baseline; non-inclusive is reserved for follow-up.
enum class Inclusion { Inclusive, NonInclusive };
Inclusion parse_inclusion(const std::string& s);
const char* inclusion_label(Inclusion p);

struct Settings {
    Protocol protocol      = Protocol::MSI;
    NodeId   num_procs     = 4;        // CPUs only; the directory adds one more node
    std::size_t mem_latency     = 100; // cycles
    std::size_t block_size_log2 = 6;   // 64-byte lines
    std::size_t link_width_log2 = 3;   // 8-byte links
    // Derived; computed from the two log2 fields at construction.
    std::size_t header_flits   = 0;    // 1 << (4 - link_width_log2)
    std::size_t payload_flits  = 0;    // 1 << (block_size_log2 - link_width_log2)
    // Cache topology + LLS inclusion. PrivateL2/Inclusive defaults keep
    // existing configs running unchanged. The shared_lls data path is
    // built in Phase 2+; selecting it before then will trigger an
    // explicit "not yet implemented" runtime error in full mode.
    CacheMode cache_mode = CacheMode::PrivateL2;
    Inclusion inclusion  = Inclusion::Inclusive;
    // LLS geometry, only meaningful when cache_mode == SharedLls.
    // lls_blocks = size_kb * 1024 / block_size; computed at to_settings()
    // time so the directory + LLS cache can pick it up directly.
    std::size_t lls_blocks      = 0;       // total LLS capacity in 64-byte blocks
    std::size_t lls_assoc       = 16;      // ways per set
    std::size_t lls_hit_latency = 10;      // cycles to satisfy an LLS hit
};

// Compute header_flits / payload_flits from the *_log2 fields.
void finalize_settings(Settings& s);

} // namespace comparch::coherence
