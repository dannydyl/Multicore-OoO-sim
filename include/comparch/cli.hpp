#pragma once

#include <filesystem>
#include <optional>

#include "comparch/config.hpp"
#include "comparch/log.hpp"

namespace comparch {

struct CliArgs {
    std::filesystem::path config_path;
    std::optional<std::filesystem::path> trace;
    // Per-core trace directory (project3 layout: <dir>/p<i>.trace). Used by
    // --mode coherence; mutually exclusive with --trace.
    std::optional<std::filesystem::path> trace_dir;
    Mode mode = Mode::Full;
    std::optional<std::filesystem::path> out;
    std::optional<int> override_cores;
    // Coherence-mode shorthand: overrides cfg.coherence.protocol.
    std::optional<std::string> protocol_override;
    LogLevel log_level = LogLevel::Info;
    bool show_version = false;
};

struct CliParseResult {
    int exit_code = 0;
    bool should_exit = false;
    CliArgs args;
};

CliParseResult parse_cli(int argc, char** argv);

void apply_overrides(SimConfig& cfg, const CliArgs& cli);

} // namespace comparch
