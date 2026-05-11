#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "comparch/program_manifest.hpp"
#include "comparch/trace.hpp"

using comparch::trace::FileHeader;
using comparch::trace::ManifestError;
using comparch::trace::parse_program_manifest;
using comparch::trace::ProgramManifest;
using comparch::trace::Record;
using comparch::trace::Variant;
using comparch::trace::Writer;

namespace fs = std::filesystem;

namespace {

// Create a small v2 trace file with one no-op instruction record so
// it exists on disk and the manifest parser's existence check passes.
void touch_v2_trace(const fs::path& p, std::uint32_t tid) {
    Writer w(p, Variant::CasimV2);
    FileHeader h;
    h.thread_id = tid;
    h.thread_count = 1;
    w.write_header(h);
    Record r{};
    r.ip = 0x1000 + tid;
    w.write(r);
}

fs::path scratch_dir(const std::string& tag) {
    auto d = fs::temp_directory_path() / ("comparch_manifest_" + tag);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

} // namespace

TEST_CASE("Manifest: well-formed 4-thread program parses", "[manifest]") {
    const auto dir = scratch_dir("ok4");
    for (int i = 0; i < 4; ++i) {
        touch_v2_trace(dir / ("t" + std::to_string(i) + ".casim"),
                       static_cast<std::uint32_t>(i));
    }
    const auto mpath = dir / "prog.manifest";
    {
        std::ofstream f(mpath);
        f << "# example\n"
          << "program: lu_b\n"
          << "threads: 4\n"
          << "program_uid: 0xC0FFEE\n"
          << "t0: t0.casim\n"
          << "t1: t1.casim\n"
          << "t2: t2.casim\n"
          << "t3: t3.casim\n";
    }

    auto m = parse_program_manifest(mpath);
    REQUIRE(m.name == "lu_b");
    REQUIRE(m.thread_count == 4u);
    REQUIRE(m.program_uid  == 0xC0FFEEu);
    REQUIRE(m.paths.size() == 4);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(m.paths[i].filename() == ("t" + std::to_string(i) + ".casim"));
        REQUIRE(fs::exists(m.paths[i]));
    }
}

TEST_CASE("Manifest: missing 'threads' key fails", "[manifest]") {
    const auto dir = scratch_dir("nothreads");
    const auto mpath = dir / "prog.manifest";
    { std::ofstream f(mpath); f << "program: x\n"; }
    REQUIRE_THROWS_AS(parse_program_manifest(mpath), ManifestError);
}

TEST_CASE("Manifest: t<N> count must match threads", "[manifest]") {
    const auto dir = scratch_dir("countmismatch");
    touch_v2_trace(dir / "t0.casim", 0);
    const auto mpath = dir / "prog.manifest";
    { std::ofstream f(mpath); f << "threads: 2\nt0: t0.casim\n"; }
    REQUIRE_THROWS_AS(parse_program_manifest(mpath), ManifestError);
}

TEST_CASE("Manifest: gap in t<N> indices fails", "[manifest]") {
    const auto dir = scratch_dir("gap");
    touch_v2_trace(dir / "t0.casim", 0);
    touch_v2_trace(dir / "t2.casim", 2);
    const auto mpath = dir / "prog.manifest";
    { std::ofstream f(mpath); f << "threads: 2\nt0: t0.casim\nt2: t2.casim\n"; }
    REQUIRE_THROWS_AS(parse_program_manifest(mpath), ManifestError);
}

TEST_CASE("Manifest: duplicate t<N> fails", "[manifest]") {
    const auto dir = scratch_dir("dup");
    touch_v2_trace(dir / "t0.casim", 0);
    const auto mpath = dir / "prog.manifest";
    { std::ofstream f(mpath); f << "threads: 1\nt0: t0.casim\nt0: t0.casim\n"; }
    REQUIRE_THROWS_AS(parse_program_manifest(mpath), ManifestError);
}

TEST_CASE("Manifest: missing trace file on disk fails", "[manifest]") {
    const auto dir = scratch_dir("notrace");
    const auto mpath = dir / "prog.manifest";
    { std::ofstream f(mpath); f << "threads: 1\nt0: nonexistent.casim\n"; }
    REQUIRE_THROWS_AS(parse_program_manifest(mpath), ManifestError);
}

TEST_CASE("Manifest: unknown key fails", "[manifest]") {
    const auto dir = scratch_dir("unkkey");
    touch_v2_trace(dir / "t0.casim", 0);
    const auto mpath = dir / "prog.manifest";
    { std::ofstream f(mpath); f << "threads: 1\nt0: t0.casim\nfrobnicate: 7\n"; }
    REQUIRE_THROWS_AS(parse_program_manifest(mpath), ManifestError);
}

TEST_CASE("Manifest: comments + blank lines + inline comments ignored",
          "[manifest]") {
    const auto dir = scratch_dir("comments");
    touch_v2_trace(dir / "t0.casim", 0);
    const auto mpath = dir / "prog.manifest";
    {
        std::ofstream f(mpath);
        f << "# header comment\n"
          << "\n"
          << "   \n"
          << "program: thing   # inline comment\n"
          << "threads: 1\n"
          << "t0: t0.casim\n";
    }
    auto m = parse_program_manifest(mpath);
    REQUIRE(m.name == "thing");
    REQUIRE(m.thread_count == 1u);
}

TEST_CASE("Manifest: program_uid defaults to 0 when omitted", "[manifest]") {
    const auto dir = scratch_dir("nouid");
    touch_v2_trace(dir / "t0.casim", 0);
    const auto mpath = dir / "prog.manifest";
    { std::ofstream f(mpath); f << "threads: 1\nt0: t0.casim\n"; }
    auto m = parse_program_manifest(mpath);
    REQUIRE(m.program_uid == 0u);
}
