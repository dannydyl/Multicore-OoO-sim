#include "comparch/cache/write_policy.hpp"

namespace comparch::cache {

std::string_view write_policy_name(WritePolicy w) {
    switch (w) {
        case WritePolicy::WBWA:  return "wbwa";
        case WritePolicy::WTWNA: return "wtwna";
    }
    return "?";
}

std::optional<WritePolicy> parse_write_policy(std::string_view s) {
    if (s == "writeback" || s == "wbwa")     return WritePolicy::WBWA;
    if (s == "writethrough" || s == "wtwna") return WritePolicy::WTWNA;
    return std::nullopt;
}

} // namespace comparch::cache
