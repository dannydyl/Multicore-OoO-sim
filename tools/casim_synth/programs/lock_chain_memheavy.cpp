// Variant of lock_chain whose critical section is full of
// cache-missing loads instead of ALU. The retire-time
// SyncCoordinator notification only meaningfully delays the
// next thread's acquire if the issuing thread's pipeline is
// actually slow to retire — which requires real memory stalls.
// ALU-only crit sections retire at ~steady-state IPC and the
// fetch-retire gap is bounded by dispq depth (8 instructions);
// memory-heavy crit sections push ROB occupancy way up and
// the gap can reach the full ROB depth (~64 instructions).
//
// Compare cycles vs synth_lock_chain on the same shape.

#include <filesystem>
#include <iostream>
#include <string>

#include "casim_synth.hpp"

int main(int argc, char** argv) {
    using namespace comparch::casim_synth;

    int threads          = 4;
    int pre              = 50;
    int loads_per_crit   = 64;     // each a cold miss into private region
    int post             = 50;
    std::string out_dir  = "lock_chain_mem_out";

    if (argc > 1) out_dir = argv[1];
    if (argc > 2) threads = std::stoi(argv[2]);
    if (argc > 3) loads_per_crit = std::stoi(argv[3]);

    Program prog("lock_chain_mem",
                 static_cast<std::uint32_t>(threads));
    auto m = prog.mutex();

    // Private region per thread for the critical-section loads —
    // each at a fresh stride so they miss into memory (no warm L1).
    constexpr std::uint64_t kPrivBase = 0x100000000ULL;
    constexpr std::uint64_t kStride   = 0x4000ULL;   // 16 KB per load

    for (int i = 0; i < threads; ++i) {
        const std::uint64_t base =
            kPrivBase + static_cast<std::uint64_t>(i) * 0x1000000ULL;
        auto th = prog.t(static_cast<std::uint32_t>(i));
        th.alus(static_cast<std::size_t>(pre));
        th.lock(m);
        for (int k = 0; k < loads_per_crit; ++k) {
            th.load(base + static_cast<std::uint64_t>(k) * kStride);
            th.alus(2);
        }
        th.unlock(m);
        th.alus(static_cast<std::size_t>(post));
    }

    prog.write(out_dir);
    std::cout << "lock_chain_mem: " << threads
              << " threads, " << loads_per_crit
              << " loads/crit, output -> "
              << std::filesystem::absolute(out_dir).string() << "\n";
    return 0;
}
