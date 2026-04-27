#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <stdexcept>
#include <string>

namespace comparch::trace {

inline constexpr int kStandardDestRegs = 2;
inline constexpr int kStandardSrcRegs  = 4;
inline constexpr int kStandardDestMem  = 2;
inline constexpr int kStandardSrcMem   = 4;

inline constexpr std::size_t kStandardRecordBytes = 64;

enum class Variant {
    Standard,
};

struct Record {
    std::uint64_t ip = 0;
    bool is_branch    = false;
    bool branch_taken = false;
    std::array<std::uint8_t,  kStandardDestRegs> destination_registers{};
    std::array<std::uint8_t,  kStandardSrcRegs>  source_registers{};
    std::array<std::uint64_t, kStandardDestMem>  destination_memory{};
    std::array<std::uint64_t, kStandardSrcMem>   source_memory{};
};

bool operator==(const Record& a, const Record& b);
inline bool operator!=(const Record& a, const Record& b) { return !(a == b); }

class TraceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Reader {
public:
    Reader(std::istream& in, Variant v);
    explicit Reader(const std::filesystem::path& path, Variant v = Variant::Standard);
    ~Reader();

    Reader(const Reader&)            = delete;
    Reader& operator=(const Reader&) = delete;

    bool next(Record& out);

    Variant      variant() const { return variant_; }
    std::size_t  records_read() const { return records_read_; }

private:
    std::istream*            in_;
    std::unique_ptr<std::istream> owned_;
    Variant                  variant_;
    std::size_t              records_read_ = 0;
};

class Writer {
public:
    Writer(std::ostream& out, Variant v);
    explicit Writer(const std::filesystem::path& path, Variant v = Variant::Standard);
    ~Writer();

    Writer(const Writer&)            = delete;
    Writer& operator=(const Writer&) = delete;

    void write(const Record& r);
    void flush();

    Variant      variant() const { return variant_; }
    std::size_t  records_written() const { return records_written_; }

private:
    std::ostream*            out_;
    std::unique_ptr<std::ostream> owned_;
    Variant                  variant_;
    std::size_t              records_written_ = 0;
};

std::size_t record_bytes(Variant v);
std::string_view variant_name(Variant v);

} // namespace comparch::trace
