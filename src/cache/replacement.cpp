#include "comparch/cache/replacement.hpp"

namespace comparch::cache {

std::string_view replacement_name(Replacement r) {
    switch (r) {
        case Replacement::LRU_MIP: return "lru_mip";
        case Replacement::LRU_LIP: return "lru_lip";
    }
    return "?";
}

std::optional<Replacement> parse_replacement(std::string_view s) {
    if (s == "lru" || s == "mip" || s == "lru_mip") return Replacement::LRU_MIP;
    if (s == "lip" || s == "lru_lip")               return Replacement::LRU_LIP;
    return std::nullopt;
}

} // namespace comparch::cache
