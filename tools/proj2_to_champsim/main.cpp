#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <CLI/CLI.hpp>

#include "comparch/trace.hpp"
#include "proj2_to_champsim.hpp"

int main(int argc, char** argv) {
    using namespace comparch;

    CLI::App app{"proj2_to_champsim - convert project2 11-field text traces to ChampSim binary"};

    std::filesystem::path in_path;
    std::filesystem::path out_path;

    app.add_option("--in",  in_path,  "Project2-format text trace (- for stdin)")->required();
    app.add_option("--out", out_path, "Destination ChampSim binary trace")->required();

    CLI11_PARSE(app, argc, argv);

    try {
        std::unique_ptr<std::istream> owned;
        std::istream* in = nullptr;
        if (in_path == "-") {
            in = &std::cin;
        } else {
            owned = std::make_unique<std::ifstream>(in_path);
            if (!*owned) {
                std::cerr << "proj2_to_champsim: cannot open input: " << in_path << '\n';
                return 2;
            }
            in = owned.get();
        }

        trace::Writer writer(out_path, trace::Variant::Standard);
        const auto n = proj2::convert(*in, writer);
        writer.flush();
        std::cout << "wrote " << n << " records -> " << out_path << '\n';
    } catch (const trace::TraceError& e) {
        std::cerr << e.what() << '\n';
        return 3;
    }
    return 0;
}
