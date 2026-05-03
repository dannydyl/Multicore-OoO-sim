#include "comparch/cli.hpp"

#include "comparch/version.hpp"

#include <cctype>
#include <iostream>
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

    app.add_option("--trace-dir", a.trace_dir,
                   "Per-core trace directory (project3 layout: <dir>/p<i>.trace). "
                   "Used by --mode coherence; mutually exclusive with --trace.")
        ->check(CLI::ExistingDirectory);

    app.add_option("--out", a.out, "Write merged config JSON to this path");

    app.add_option("--cores", a.override_cores, "Override 'cores' from the config")
        ->check(CLI::PositiveNumber);

    app.add_option("--protocol", a.protocol_override,
                   "Coherence protocol shorthand (overrides cfg.coherence.protocol). "
                   "Used by --mode coherence.")
        ->check(CLI::IsMember(
            std::vector<std::string>{"mi", "msi", "mesi", "mosi", "moesif"},
            CLI::ignore_case));

    app.add_option("--tag", a.tag,
                   "Suffix appended to the run-report folder name under report/ "
                   "(e.g. 'baseline' -> report/<trace>_<proto>_c<N>_baseline/).");

    // Default invocation (no --mode) runs the full multi-core OoO + coherence
    // simulator. Subsystem isolation modes (cache / predictor / ooo / coherence)
    // are opt-in for testing pieces in isolation. "full" is intentionally NOT
    // in the accepted list — passing it explicitly should produce a clear
    // error rather than silently equal the default.
    std::string mode_str;
    app.add_option("--mode", mode_str,
                   "Subsystem to run (omit for full multi-core simulator)")
        ->check(CLI::IsMember(
            std::vector<std::string>{"cache", "predictor", "ooo", "coherence"},
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

    if (!mode_str.empty()) {
        if (auto m = parse_mode(mode_str)) {
            a.mode = *m;
        }
    }
    if (auto l = parse_log_level(log_level_str)) {
        a.log_level = *l;
    }

    if (a.trace && a.trace_dir) {
        std::cerr << "--trace and --trace-dir are mutually exclusive\n";
        r.should_exit = true;
        r.exit_code = 1;
    }

    return r;
}

void apply_overrides(SimConfig& cfg, const CliArgs& cli) {
    if (cli.override_cores) {
        cfg.cores = *cli.override_cores;
    }
    if (cli.protocol_override) {
        std::string p = *cli.protocol_override;
        for (auto& c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        cfg.coherence.protocol = p;
    }
}

} // namespace comparch
