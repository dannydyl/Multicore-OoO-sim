#include "comparch/coherence/settings.hpp"

#include "comparch/config.hpp"

namespace comparch::coherence {

Protocol parse_protocol(const std::string& s) {
    if (s == "mi")     return Protocol::MI;
    if (s == "msi")    return Protocol::MSI;
    if (s == "mesi")   return Protocol::MESI;
    if (s == "mosi")   return Protocol::MOSI;
    if (s == "moesif") return Protocol::MOESIF;
    throw ConfigError("unknown coherence protocol: '" + s + "'");
}

const char* protocol_label(Protocol p) {
    switch (p) {
        case Protocol::MI:     return "MI_PRO";
        case Protocol::MSI:    return "MSI_PRO";
        case Protocol::MESI:   return "MESI_PRO";
        case Protocol::MOSI:   return "MOSI_PRO";
        case Protocol::MOESIF: return "MOESIF_PRO";
    }
    return "?";
}

void finalize_settings(Settings& s) {
    // Match project3/simulator/settings.h: header is 16 bytes
    // (1 << 4) -> 1 << (4 - link_width); payload is one block.
    s.header_flits  = static_cast<std::size_t>(1) << (4 - s.link_width_log2);
    s.payload_flits = static_cast<std::size_t>(1) << (s.block_size_log2 - s.link_width_log2);
}

} // namespace comparch::coherence
