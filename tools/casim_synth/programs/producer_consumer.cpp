// Classic producer / consumer over a shared queue. Two threads:
//   - producer (t0): produces N items, each item is a store followed
//     by lock/unlock around incrementing a shared counter.
//   - consumer (t1): consumes N items, each item is lock/unlock
//     around reading + decrementing the counter, then a load.
//
// Locks are taken in alternation (one per item) so the trace ends up
// with 2N total lock pairs on the same mutex, giving a long chain
// of seq numbers for the SyncCoordinator to enforce.

#include <filesystem>
#include <iostream>
#include <string>

#include "casim_synth.hpp"

int main(int argc, char** argv) {
    using namespace comparch::casim_synth;

    int n = 64;                          // items
    std::string out_dir = "prodcon_out";

    if (argc > 1) out_dir = argv[1];
    if (argc > 2) n = std::stoi(argv[2]);

    Program prog("prodcon", /*threads=*/2);
    auto q_lock = prog.mutex();

    constexpr std::uint64_t kQueueAddr   = 0x40000000ULL;
    constexpr std::uint64_t kCounterAddr = 0x40000040ULL;

    // We alternate t(0) and t(1) calls in source order so the lock
    // grant sequence is producer, consumer, producer, consumer, ...
    // matching how a real prodcon program serializes on the queue.
    for (int i = 0; i < n; ++i) {
        prog.t(0)
            .alus(20)
            .store(kQueueAddr + static_cast<std::uint64_t>(i & 7) * 64ULL)
            .lock(q_lock)
            .load(kCounterAddr)
            .alus(2)
            .store(kCounterAddr)
            .unlock(q_lock);

        prog.t(1)
            .alus(15)
            .lock(q_lock)
            .load(kCounterAddr)
            .alus(2)
            .store(kCounterAddr)
            .unlock(q_lock)
            .load(kQueueAddr + static_cast<std::uint64_t>(i & 7) * 64ULL);
    }

    prog.write(out_dir);
    std::cout << "prodcon: wrote 2 threads, " << n << " items each, to "
              << std::filesystem::absolute(out_dir).string()
              << "; run with: sim --config <cfg> --cores 2 --program "
              << out_dir << "/prodcon.manifest\n";
    return 0;
}
