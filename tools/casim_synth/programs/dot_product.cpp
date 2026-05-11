// SPLASH-2-style dot product. N threads each:
//   1. Compute local sum across a private slice (alus + loads in a loop)
//   2. Barrier — all threads finish local work
//   3. Lock the result accumulator, add local sum, unlock
//   4. Barrier — all threads have published their partial sum
//
// Mixes barriers (fan-in stall on all participants) with a serialized
// accumulator (lock contention). Closer to a realistic parallel
// benchmark than lock_chain.

#include <filesystem>
#include <iostream>
#include <string>

#include "casim_synth.hpp"

int main(int argc, char** argv) {
    using namespace comparch::casim_synth;

    int threads        = 4;
    int slice_len      = 1024;     // per-thread local work
    std::string out_dir = "dot_product_out";

    if (argc > 1) out_dir = argv[1];
    if (argc > 2) threads = std::stoi(argv[2]);

    Program prog("dot_product", static_cast<std::uint32_t>(threads));
    auto sum_lock  = prog.mutex();
    auto local_b   = prog.barrier(static_cast<std::uint32_t>(threads));
    auto pub_b     = prog.barrier(static_cast<std::uint32_t>(threads));

    constexpr std::uint64_t kSliceBase = 0x40000000ULL;
    constexpr std::uint64_t kAccumAddr = 0x80000000ULL;

    for (int i = 0; i < threads; ++i) {
        auto t = prog.t(static_cast<std::uint32_t>(i));

        // Stage 1: local dot-product loop. Each thread reads its own
        // strided slice (private addresses → no false sharing) and
        // does some ALU per element.
        const std::uint64_t slice_start =
            kSliceBase + static_cast<std::uint64_t>(i) * 0x200000ULL;
        for (int k = 0; k < slice_len; ++k) {
            t.load(slice_start + static_cast<std::uint64_t>(k) * 64ULL);
            t.alus(2);   // mul + add per element, roughly
        }

        // Stage 2: barrier — all threads done with local work.
        t.barrier(local_b);

        // Stage 3: contended accumulator update. Read-modify-write
        // through the shared address protected by sum_lock.
        t.lock(sum_lock);
        t.load(kAccumAddr);
        t.alus(2);
        t.store(kAccumAddr);
        t.unlock(sum_lock);

        // Stage 4: final barrier so all threads see the final sum.
        t.barrier(pub_b);

        // Post-barrier work (each thread does something with the
        // global result, e.g. writes its share to a result vector).
        t.alus(50);
    }

    prog.write(out_dir);
    std::cout << "dot_product: wrote " << threads
              << " threads to " << std::filesystem::absolute(out_dir).string()
              << "; run with: sim --config <cfg> --cores " << threads
              << " --program " << out_dir << "/dot_product.manifest\n";
    return 0;
}
