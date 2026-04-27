#include "comparch/cli.hpp"

#include "comparch/version.hpp"

#include <string>
#include <vector>

#include <CLI/CLI.hpp>

namespace comparch {

CliParseResult parse_cli(int argc, char** argv) {
    CliParseResult r;
    CliArgs& a = r.args;

    CLI::App app{"sim - multi-core out-of-order cache-coherent CMP simulator"};
    app.set_version_flag("--version", std::string{"sim "} + kVersion);

    app.add_option("--config", a.config_path, "Machine config (JSON)")
        ->required()
        ->check(CLI::ExistingFile);

    app.add_option("--trace", a.trace, "Workload trace (canonical format)")
        ->check(CLI::ExistingFile);

    app.add_option("--out", a.out, "Write merged config JSON to this path");

    app.add_option("--cores", a.override_cores, "Override 'cores' from the config")
        ->check(CLI::PositiveNumber);

    std::string mode_str = std::string{to_string(a.mode)};
    app.add_option("--mode", mode_str, "Subsystem to run")
        ->check(CLI::IsMember(
            std::vector<std::string>{"full", "cache", "predictor", "ooo", "coherence"},
            CLI::ignore_case));

    std::string log_level_str = "info";
    app.add_option("--log-level", log_level_str, "Logging threshold")
        ->check(CLI::IsMember(
            std::vector<std::string>{"trace", "debug", "info", "warn", "error", "off"},
            CLI::ignore_case));

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        const int cli_code = app.exit(e);
        r.should_exit = true;
        r.exit_code = (cli_code == 0) ? 0 : 1;
        return r;
    }

    if (auto m = parse_mode(mode_str)) {
        a.mode = *m;
    }
    if (auto l = parse_log_level(log_level_str)) {
        a.log_level = *l;
    }

    return r;
}

void apply_overrides(SimConfig& cfg, const CliArgs& cli) {
    if (cli.override_cores) {
        cfg.cores = *cli.override_cores;
    }
}

} // namespace comparch
