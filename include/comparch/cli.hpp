#pragma once

#include <filesystem>
#include <optional>

#include "comparch/config.hpp"
#include "comparch/log.hpp"

namespace comparch {

struct CliArgs {
    std::filesystem::path config_path;
    std::optional<std::filesystem::path> trace;
    Mode mode = Mode::Full;
    std::optional<std::filesystem::path> out;
    std::optional<int> override_cores;
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
