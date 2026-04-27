#pragma once

#include <optional>
#include <string_view>

namespace comparch::cache {

enum class WritePolicy {
    WBWA,  // Write back, write-allocate (project1 L1 default)
    WTWNA, // Write through, write-no-allocate (project1 L2 default)
};

std::string_view       write_policy_name(WritePolicy w);
std::optional<WritePolicy> parse_write_policy(std::string_view s);

} // namespace comparch::cache
