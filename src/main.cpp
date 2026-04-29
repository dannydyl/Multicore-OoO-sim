#include <fstream>
#include <iostream>

#include "comparch/cache/cache_mode.hpp"
#include "comparch/cli.hpp"
#include "comparch/config.hpp"
#include "comparch/log.hpp"
#include "comparch/ooo/ooo_mode.hpp"
#include "comparch/predictor/predictor_mode.hpp"
#include "comparch/trace.hpp"
#include "comparch/version.hpp"

int main(int argc, char** argv) {
    auto parsed = comparch::parse_cli(argc, argv);
    if (parsed.should_exit) {
        return parsed.exit_code;
    }
    const auto& cli = parsed.args;
    comparch::set_log_level(cli.log_level);

    LOG_INFO("sim " << comparch::kVersion << " (mode=" << comparch::to_string(cli.mode) << ")");

    comparch::SimConfig cfg;
    try {
        cfg = comparch::load_config(cli.config_path);
    } catch (const comparch::ConfigError& e) {
        LOG_ERROR(e.what());
        return 2;
    }
    comparch::apply_overrides(cfg, cli);

    if (cli.mode == comparch::Mode::Cache) {
        try {
            return comparch::cache::run_cache_mode(cfg, cli);
        } catch (const comparch::ConfigError& e) {
            LOG_ERROR(e.what());
            return 2;
        } catch (const comparch::trace::TraceError& e) {
            LOG_ERROR("trace: " << e.what());
            return 4;
        }
    }

    if (cli.mode == comparch::Mode::Predictor) {
        try {
            return comparch::predictor::run_predictor_mode(cfg, cli);
        } catch (const comparch::ConfigError& e) {
            LOG_ERROR(e.what());
            return 2;
        } catch (const comparch::trace::TraceError& e) {
            LOG_ERROR("trace: " << e.what());
            return 4;
        }
    }

    if (cli.mode == comparch::Mode::Ooo) {
        try {
            return comparch::ooo::run_ooo_mode(cfg, cli);
        } catch (const comparch::ConfigError& e) {
            LOG_ERROR(e.what());
            return 2;
        } catch (const comparch::trace::TraceError& e) {
            LOG_ERROR("trace: " << e.what());
            return 4;
        }
    }

    // Coherence and Full modes are accepted by the CLI but have no driver
    // yet (Phase 5). Returning 0 here would silently lie to scripts that
    // treat exit code as truth — fail loudly instead, but still emit the
    // merged config to --out for users who pass --mode full just to render
    // the resolved config.
    if (cli.out) {
        std::ofstream out(*cli.out);
        if (!out) {
            LOG_ERROR("cannot open --out file for writing: " << *cli.out);
            return 3;
        }
        out << comparch::dump_config(cfg) << '\n';
        LOG_INFO("wrote merged config to " << *cli.out);
    }

    LOG_ERROR(comparch::to_string(cli.mode)
              << ": driver not implemented yet (Phase 5)");
    return 5;
}
