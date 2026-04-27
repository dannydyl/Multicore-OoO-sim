#include <iostream>

#include "comparch/config.hpp"
#include "comparch/log.hpp"
#include "comparch/version.hpp"

int main() {
    LOG_INFO("sim " << comparch::kVersion);

    try {
        auto cfg = comparch::load_config("configs/baseline.json");
        std::cout << comparch::dump_config(cfg) << '\n';
    } catch (const comparch::ConfigError& e) {
        LOG_ERROR(e.what());
        return 2;
    }

    LOG_INFO("phase 0: nothing simulated; exiting");
    return 0;
}
