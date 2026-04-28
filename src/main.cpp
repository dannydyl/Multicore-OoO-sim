#include <fstream>
#include <iostream>

#include "comparch/cache/cache_mode.hpp"
#include "comparch/cli.hpp"
#include "comparch/config.hpp"
#include "comparch/log.hpp"
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

    if (cli.trace) {
        LOG_INFO("trace path: " << *cli.trace);
    }

    const std::string dumped = comparch::dump_config(cfg);

    if (cli.out) {
        std::ofstream out(*cli.out);
        if (!out) {
            LOG_ERROR("cannot open --out file for writing: " << *cli.out);
            return 3;
        }
        out << dumped << '\n';
        LOG_INFO("wrote merged config to " << *cli.out);
    } else {
        std::cout << dumped << '\n';
    }

    LOG_INFO(comparch::to_string(cli.mode) << ": no driver yet; exiting");
    return 0;
}
