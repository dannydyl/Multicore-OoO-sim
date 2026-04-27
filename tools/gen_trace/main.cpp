#include <filesystem>
#include <iostream>
#include <string>

#include <CLI/CLI.hpp>

#include "comparch/trace.hpp"
#include "gen_trace.hpp"

int main(int argc, char** argv) {
    using namespace comparch;

    CLI::App app{"gen_trace - synthesize ChampSim traces for tests / dev"};

    std::filesystem::path out_path;
    std::size_t records = 1024;
    std::string pattern_str = "sequential";
    gen_trace::GenParams p;

    app.add_option("--out", out_path, "Output .champsimtrace path")
        ->required();
    app.add_option("--records", records, "Number of records to emit")
        ->check(CLI::PositiveNumber);
    app.add_option("--pattern", pattern_str,
                   "sequential | loop | stream | random")
        ->check(CLI::IsMember({"sequential", "loop", "stream", "random"},
                               CLI::ignore_case));
    app.add_option("--branch-rate", p.branch_rate, "Fraction of records that are branches");
    app.add_option("--load-rate",   p.load_rate,
                   "Fraction of non-branch records that are loads");
    app.add_option("--store-rate",  p.store_rate,
                   "Fraction of non-branch records that are stores");
    app.add_option("--seed",        p.seed,        "RNG seed");
    app.add_option("--pc-base",     p.pc_base,     "Base PC");
    app.add_option("--pc-stride",   p.pc_stride,   "PC stride");
    app.add_option("--addr-base",   p.addr_base,   "Base data address");
    app.add_option("--addr-stride", p.addr_stride, "Address stride (sequential / stream / loop)");
    app.add_option("--loop-size",   p.loop_size,   "PC range for the loop pattern");

    CLI11_PARSE(app, argc, argv);

    if (auto pat = gen_trace::parse_pattern(pattern_str)) {
        p.pattern = *pat;
    } else {
        std::cerr << "unknown pattern: " << pattern_str << '\n';
        return 1;
    }
    p.records = records;

    try {
        trace::Writer w(out_path, trace::Variant::Standard);
        gen_trace::generate_records(p, w);
        w.flush();
    } catch (const trace::TraceError& e) {
        std::cerr << "gen_trace: " << e.what() << '\n';
        return 2;
    }

    std::cout << "wrote " << records << " records ("
              << gen_trace::pattern_name(p.pattern)
              << ", seed=" << p.seed << ") -> " << out_path << '\n';
    return 0;
}
