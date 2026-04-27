#include "comparch/log.hpp"
#include "comparch/version.hpp"

int main() {
    LOG_INFO("sim " << comparch::kVersion << " (phase 0 stub)");
    LOG_DEBUG("debug-level message (suppressed by default threshold)");
    return 0;
}
