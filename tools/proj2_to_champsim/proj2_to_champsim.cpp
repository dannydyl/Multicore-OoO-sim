#include "proj2_to_champsim.hpp"

#include <cstdint>
#include <istream>
#include <sstream>
#include <string>

namespace comparch::proj2 {

namespace {

// Project2 opcode encoding from project2_v2.1.0_all/procsim.hpp:28-32.
// (MUL=3 omitted because it has no behavior here; the range check below
// implicitly accepts it via the [kOpAdd, kOpBranch] bounds.)
constexpr int kOpAdd    = 2;
constexpr int kOpLoad   = 4;
constexpr int kOpStore  = 5;
constexpr int kOpBranch = 6;

// Project2 marks an unused register slot with -1 (or 0 in some traces). We
// mirror ChampSim convention: unused slots are 0. Anything outside the byte
// range is clamped to 0 too — the new sim ignores those fields for predictor
// regression and the OoO core (Phase 4) will repopulate them properly.
std::uint8_t reg_to_byte(int r) {
    if (r <= 0 || r > 255) return 0;
    return static_cast<std::uint8_t>(r);
}

bool is_blank(std::string_view s) {
    for (char c : s) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
    }
    return true;
}

} // namespace

ParseResult parse_line(std::string_view line, trace::Record& out) {
    if (is_blank(line)) {
        return {false, "blank line"};
    }

    // istringstream over a temporary string is the simplest way to reuse the
    // standard formatted-extract operators while still allowing string_view
    // input. The conversion is one-shot and not on a hot path.
    std::istringstream is{std::string(line)};

    std::uint64_t pc            = 0;
    int           opcode        = 0;
    int           dest          = 0;
    int           src1          = 0;
    int           src2          = 0;
    std::uint64_t mem_addr      = 0;
    int           br_taken      = 0;
    std::uint64_t br_target     = 0;
    int           icache_hit    = 0;
    int           dcache_hit    = 0;
    std::uint64_t dyn_inst_count = 0;

    // pc, mem_addr, and br_target are hex without 0x prefix in project2's
    // format, so set the stream's base to hex for those reads.
    is >> std::hex >> pc;
    is >> std::dec >> opcode >> dest >> src1 >> src2;
    is >> std::hex >> mem_addr;
    is >> std::dec >> br_taken;
    is >> std::hex >> br_target;
    is >> std::dec >> icache_hit >> dcache_hit >> dyn_inst_count;

    if (!is) {
        return {false, "expected 11 whitespace-separated fields"};
    }

    if (opcode < kOpAdd || opcode > kOpBranch) {
        std::ostringstream oss;
        oss << "opcode out of range [2..6]: " << opcode;
        return {false, oss.str()};
    }

    out = trace::Record{};
    out.ip           = pc;
    out.is_branch    = (opcode == kOpBranch);
    out.branch_taken = out.is_branch && (br_taken != 0);

    // Register fields: project2 uses one dest + two srcs (int8). ChampSim
    // standard slots are 2 dest + 4 src; the unused slots stay at 0.
    out.destination_registers[0] = reg_to_byte(dest);
    out.source_registers[0]      = reg_to_byte(src1);
    out.source_registers[1]      = reg_to_byte(src2);

    // Memory address slots: load goes into source_memory, store goes into
    // destination_memory. Branches do not carry mem addrs in this format.
    if (opcode == kOpLoad) {
        out.source_memory[0] = mem_addr;
    } else if (opcode == kOpStore) {
        out.destination_memory[0] = mem_addr;
    }

    // Reference fields we intentionally drop: br_target (predictor doesn't
    // need a target), icache_hit / dcache_hit (recomputed by the new sim),
    // dyn_instruction_count (ChampSim has no slot; readers count records).
    (void)br_target;
    (void)icache_hit;
    (void)dcache_hit;
    (void)dyn_inst_count;

    return {true, {}};
}

std::size_t convert(std::istream& in, trace::Writer& writer) {
    std::string line;
    std::size_t lineno = 0;
    std::size_t emitted = 0;
    trace::Record rec;

    while (std::getline(in, line)) {
        ++lineno;
        if (is_blank(line)) continue;

        const auto result = parse_line(line, rec);
        if (!result.ok) {
            std::ostringstream oss;
            oss << "proj2_to_champsim: line " << lineno << ": " << result.error;
            throw trace::TraceError(oss.str());
        }
        writer.write(rec);
        ++emitted;
    }
    return emitted;
}

} // namespace comparch::proj2
