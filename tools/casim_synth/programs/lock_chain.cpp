// Simplest synthetic multi-thread program: every thread takes and
// releases the same mutex once, in source order. Used to exercise
// the SyncCoordinator's LockAcquire/LockRelease handshake under
// strict serialization.
//
// Tuning notes:
//   - alus(N) per "section" determines how much work happens between
//     sync events. The pre-section is intentionally short so threads
//     race to the lock.
//   - The critical section is large so contended threads visibly
//     stall in the sim's report (look at thread N's `cycles`).

#include <filesystem>
#include <iostream>
#include <string>

#include "casim_synth.hpp"

int main(int argc, char** argv) {
    using namespace comparch::casim_synth;

    int threads = 4;
    int pre     = 200;
    int crit    = 800;
    int post    = 200;
    std::string out_dir = "lock_chain_out";

    if (argc > 1) out_dir = argv[1];
    if (argc > 2) threads = std::stoi(argv[2]);

    Program prog("lock_chain", static_cast<std::uint32_t>(threads));
    auto m = prog.mutex();

    for (int i = 0; i < threads; ++i) {
        prog.t(static_cast<std::uint32_t>(i))
            .alus(static_cast<std::size_t>(pre))
            .lock(m)
            .alus(static_cast<std::size_t>(crit))
            .unlock(m)
            .alus(static_cast<std::size_t>(post));
    }

    prog.write(out_dir);
    std::cout << "lock_chain: wrote " << threads
              << " threads to " << std::filesystem::absolute(out_dir).string()
              << "; run with: sim --config <cfg> --cores " << threads
              << " --program " << out_dir << "/lock_chain.manifest\n";
    return 0;
}
