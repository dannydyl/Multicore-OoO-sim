#pragma once

#include <optional>
#include <string_view>

namespace comparch::cache {

enum class Replacement {
    LRU_MIP, // MRU insertion, LRU eviction
    LRU_LIP, // LRU insertion, LRU eviction
};

std::string_view       replacement_name(Replacement r);
std::optional<Replacement> parse_replacement(std::string_view s);

} // namespace comparch::cache
