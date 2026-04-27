#include "comparch/trace.hpp"

#include <bit>
#include <cstring>
#include <fstream>
#include <istream>
#include <ostream>
#include <sstream>

namespace comparch::trace {

static_assert(std::endian::native == std::endian::little,
              "Trace reader/writer assumes little-endian native byte order; "
              "the on-disk ChampSim format is little-endian and we don't "
              "currently byte-swap. Add byte-swapping if you need to support "
              "a big-endian host.");

namespace {

void put_u64(std::ostream& out, std::uint64_t v) {
    char buf[8];
    std::memcpy(buf, &v, 8);
    out.write(buf, 8);
}

void put_u8(std::ostream& out, std::uint8_t v) {
    char c = static_cast<char>(v);
    out.write(&c, 1);
}

bool get_u64(std::istream& in, std::uint64_t& v) {
    char buf[8];
    in.read(buf, 8);
    if (in.gcount() != 8) return false;
    std::memcpy(&v, buf, 8);
    return true;
}

bool get_u8(std::istream& in, std::uint8_t& v) {
    char c;
    in.read(&c, 1);
    if (in.gcount() != 1) return false;
    v = static_cast<std::uint8_t>(c);
    return true;
}

void write_standard(std::ostream& out, const Record& r) {
    put_u64(out, r.ip);
    put_u8(out, r.is_branch    ? 1 : 0);
    put_u8(out, r.branch_taken ? 1 : 0);
    for (auto v : r.destination_registers) put_u8(out, v);
    for (auto v : r.source_registers)      put_u8(out, v);
    for (auto v : r.destination_memory)    put_u64(out, v);
    for (auto v : r.source_memory)         put_u64(out, v);
}

bool read_standard(std::istream& in, Record& r) {
    std::uint8_t b = 0;
    if (!get_u64(in, r.ip)) return false;
    if (!get_u8(in, b)) throw TraceError("truncated record: missing is_branch");
    r.is_branch = (b != 0);
    if (!get_u8(in, b)) throw TraceError("truncated record: missing branch_taken");
    r.branch_taken = (b != 0);
    for (auto& v : r.destination_registers)
        if (!get_u8(in, v)) throw TraceError("truncated record: destination_registers");
    for (auto& v : r.source_registers)
        if (!get_u8(in, v)) throw TraceError("truncated record: source_registers");
    for (auto& v : r.destination_memory)
        if (!get_u64(in, v)) throw TraceError("truncated record: destination_memory");
    for (auto& v : r.source_memory)
        if (!get_u64(in, v)) throw TraceError("truncated record: source_memory");
    return true;
}

std::unique_ptr<std::ifstream> open_in(const std::filesystem::path& p) {
    auto f = std::make_unique<std::ifstream>(p, std::ios::binary);
    if (!*f) {
        std::ostringstream oss;
        oss << "cannot open trace for reading: " << p;
        throw TraceError(oss.str());
    }
    return f;
}

std::unique_ptr<std::ofstream> open_out(const std::filesystem::path& p) {
    auto f = std::make_unique<std::ofstream>(p, std::ios::binary);
    if (!*f) {
        std::ostringstream oss;
        oss << "cannot open trace for writing: " << p;
        throw TraceError(oss.str());
    }
    return f;
}

} // namespace

bool operator==(const Record& a, const Record& b) {
    return a.ip == b.ip
        && a.is_branch == b.is_branch
        && a.branch_taken == b.branch_taken
        && a.destination_registers == b.destination_registers
        && a.source_registers      == b.source_registers
        && a.destination_memory    == b.destination_memory
        && a.source_memory         == b.source_memory;
}

std::size_t record_bytes(Variant v) {
    switch (v) {
        case Variant::Standard: return kStandardRecordBytes;
    }
    return 0;
}

std::string_view variant_name(Variant v) {
    switch (v) {
        case Variant::Standard: return "input_instr";
    }
    return "?";
}

Reader::Reader(std::istream& in, Variant v)
    : in_(&in), variant_(v) {}

Reader::Reader(const std::filesystem::path& path, Variant v)
    : variant_(v) {
    owned_ = open_in(path);
    in_ = owned_.get();
}

Reader::~Reader() = default;

bool Reader::next(Record& out) {
    out = Record{};
    bool ok = false;
    switch (variant_) {
        case Variant::Standard: ok = read_standard(*in_, out); break;
    }
    if (ok) ++records_read_;
    return ok;
}

Writer::Writer(std::ostream& out, Variant v)
    : out_(&out), variant_(v) {}

Writer::Writer(const std::filesystem::path& path, Variant v)
    : variant_(v) {
    owned_ = open_out(path);
    out_ = owned_.get();
}

Writer::~Writer() {
    if (out_ && *out_) out_->flush();
}

void Writer::write(const Record& r) {
    switch (variant_) {
        case Variant::Standard: write_standard(*out_, r); break;
    }
    if (!*out_) throw TraceError("write failed");
    ++records_written_;
}

void Writer::flush() {
    if (out_) out_->flush();
}

} // namespace comparch::trace
