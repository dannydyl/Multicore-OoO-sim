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

void put_u32(std::ostream& out, std::uint32_t v) {
    char buf[4];
    std::memcpy(buf, &v, 4);
    out.write(buf, 4);
}

void put_u16(std::ostream& out, std::uint16_t v) {
    char buf[2];
    std::memcpy(buf, &v, 2);
    out.write(buf, 2);
}

void put_u8(std::ostream& out, std::uint8_t v) {
    char c = static_cast<char>(v);
    out.write(&c, 1);
}

void put_pad(std::ostream& out, std::size_t n) {
    char zero[16] = {};
    while (n) {
        std::size_t k = n > sizeof(zero) ? sizeof(zero) : n;
        out.write(zero, static_cast<std::streamsize>(k));
        n -= k;
    }
}

bool get_u64(std::istream& in, std::uint64_t& v) {
    char buf[8];
    in.read(buf, 8);
    if (in.gcount() != 8) return false;
    std::memcpy(&v, buf, 8);
    return true;
}

bool get_u32(std::istream& in, std::uint32_t& v) {
    char buf[4];
    in.read(buf, 4);
    if (in.gcount() != 4) return false;
    std::memcpy(&v, buf, 4);
    return true;
}

bool get_u16(std::istream& in, std::uint16_t& v) {
    char buf[2];
    in.read(buf, 2);
    if (in.gcount() != 2) return false;
    std::memcpy(&v, buf, 2);
    return true;
}

bool get_u8(std::istream& in, std::uint8_t& v) {
    char c;
    in.read(&c, 1);
    if (in.gcount() != 1) return false;
    v = static_cast<std::uint8_t>(c);
    return true;
}

bool skip_n(std::istream& in, std::size_t n) {
    in.ignore(static_cast<std::streamsize>(n));
    return static_cast<std::size_t>(in.gcount()) == n;
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

// ---- CasimV2 ---------------------------------------------------------
//
// Each on-disk record is exactly kCasimV2RecordBytes (64) bytes.
// Layout for an Instr record matches Standard byte-for-byte except
// the leading is_branch byte is replaced by RecordType=Instr (=0)
// and a new is_branch byte is appended into the existing pad. This
// keeps Standard parsing trivially compatible at the byte level for
// the IP field and means the ChampSim payload only shifts by 1.
//
// For Sync/Lifecycle records, only the leading type byte is shared
// with Instr; the remaining 63 bytes are a private payload.

// Bit-pack is_branch+branch_taken into one byte so the v2 instr
// record (with the leading type tag) still fits in 64 bytes:
//   1 (type) + 1 (br flags) + 8 (ip) + 2 + 4 + 16 + 32 = 64.
void write_casimv2_instr_packed(std::ostream& out, const Record& r) {
    // Bit layout: 0=is_branch, 1=branch_taken, 2=is_mul. Bits 3-7
    // reserved for future opcode-class hints (FP, vector, etc.).
    std::uint8_t flags = (r.is_branch    ? 0x1 : 0x0)
                       | (r.branch_taken ? 0x2 : 0x0)
                       | (r.is_mul       ? 0x4 : 0x0);
    put_u8(out, static_cast<std::uint8_t>(RecordType::Instr));   // 1
    put_u8(out, flags);                                          // 1
    put_u64(out, r.ip);                                          // 8
    for (auto v : r.destination_registers) put_u8(out, v);       // 2
    for (auto v : r.source_registers)      put_u8(out, v);       // 4
    for (auto v : r.destination_memory)    put_u64(out, v);      // 16
    for (auto v : r.source_memory)         put_u64(out, v);      // 32
    // Total: 1+1+8+2+4+16+32 = 64 ✓
}

bool read_casimv2_instr_payload(std::istream& in, Record& r) {
    // Type byte already consumed by caller.
    std::uint8_t flags = 0;
    if (!get_u8(in, flags)) throw TraceError("truncated v2 instr: flags");
    r.is_branch    = (flags & 0x1) != 0;
    r.branch_taken = (flags & 0x2) != 0;
    r.is_mul       = (flags & 0x4) != 0;
    if (!get_u64(in, r.ip)) throw TraceError("truncated v2 instr: ip");
    for (auto& v : r.destination_registers)
        if (!get_u8(in, v)) throw TraceError("truncated v2 instr: dst regs");
    for (auto& v : r.source_registers)
        if (!get_u8(in, v)) throw TraceError("truncated v2 instr: src regs");
    for (auto& v : r.destination_memory)
        if (!get_u64(in, v)) throw TraceError("truncated v2 instr: dst mem");
    for (auto& v : r.source_memory)
        if (!get_u64(in, v)) throw TraceError("truncated v2 instr: src mem");
    return true;
}

void write_casimv2_sync(std::ostream& out, const SyncRecord& s) {
    put_u8(out, static_cast<std::uint8_t>(RecordType::Sync));    // 1
    put_u8(out, static_cast<std::uint8_t>(s.kind));              // 1
    put_pad(out, 6);                                             // 6  (reserved)
    put_u64(out, s.sync_object_addr);                            // 8
    put_u64(out, s.sequence_no);                                 // 8
    put_u64(out, s.extra_arg);                                   // 8
    put_u64(out, s.ip);                                          // 8
    put_u64(out, s.timestamp_ns);                                // 8
    put_pad(out, 16);                                            // 16
    // Total: 1+1+6+8+8+8+8+8+16 = 64 ✓
}

bool read_casimv2_sync_payload(std::istream& in, SyncRecord& s) {
    std::uint8_t k = 0;
    if (!get_u8(in, k)) throw TraceError("truncated v2 sync: kind");
    s.kind = static_cast<SyncKind>(k);
    if (!skip_n(in, 6)) throw TraceError("truncated v2 sync: reserved");
    if (!get_u64(in, s.sync_object_addr)) throw TraceError("truncated v2 sync: object");
    if (!get_u64(in, s.sequence_no))      throw TraceError("truncated v2 sync: seq");
    if (!get_u64(in, s.extra_arg))        throw TraceError("truncated v2 sync: extra");
    if (!get_u64(in, s.ip))               throw TraceError("truncated v2 sync: ip");
    if (!get_u64(in, s.timestamp_ns))     throw TraceError("truncated v2 sync: ts");
    if (!skip_n(in, 16))                  throw TraceError("truncated v2 sync: tail pad");
    return true;
}

void write_casimv2_lifecycle(std::ostream& out, const LifecycleRecord& l) {
    put_u8(out, static_cast<std::uint8_t>(RecordType::Lifecycle));   // 1
    put_u8(out, static_cast<std::uint8_t>(l.kind));                  // 1
    put_pad(out, 2);                                                 // 2
    put_u32(out, l.peer_tid);                                        // 4
    put_u64(out, l.ip);                                              // 8
    put_pad(out, 48);                                                // 48
    // Total: 1+1+2+4+8+48 = 64 ✓
}

bool read_casimv2_lifecycle_payload(std::istream& in, LifecycleRecord& l) {
    std::uint8_t k = 0;
    if (!get_u8(in, k)) throw TraceError("truncated v2 lifecycle: kind");
    l.kind = static_cast<LifecycleKind>(k);
    if (!skip_n(in, 2))                  throw TraceError("truncated v2 lifecycle: rsv");
    if (!get_u32(in, l.peer_tid))        throw TraceError("truncated v2 lifecycle: peer_tid");
    if (!get_u64(in, l.ip))              throw TraceError("truncated v2 lifecycle: ip");
    if (!skip_n(in, 48))                 throw TraceError("truncated v2 lifecycle: tail pad");
    return true;
}

// File header: 32 bytes. Magic (8) + version (2) + header_size (2)
// + thread_id (4) + thread_count (4) + program_uid (8) + reserved (4).
void write_casimv2_header(std::ostream& out, const FileHeader& h) {
    out.write(kCasimV2Magic, 8);                 // 8
    put_u16(out, h.format_version);              // 2
    put_u16(out, h.header_size);                 // 2
    put_u32(out, h.thread_id);                   // 4
    put_u32(out, h.thread_count);                // 4
    put_u64(out, h.program_uid);                 // 8
    put_pad(out, 4);                             // 4
    // Total: 8+2+2+4+4+8+4 = 32 ✓
}

bool read_casimv2_header(std::istream& in, FileHeader& h) {
    char magic[8];
    in.read(magic, 8);
    if (in.gcount() != 8) return false;
    if (std::memcmp(magic, kCasimV2Magic, 8) != 0) return false;
    if (!get_u16(in, h.format_version)) throw TraceError("v2 header: format_version");
    if (!get_u16(in, h.header_size))    throw TraceError("v2 header: header_size");
    if (!get_u32(in, h.thread_id))      throw TraceError("v2 header: thread_id");
    if (!get_u32(in, h.thread_count))   throw TraceError("v2 header: thread_count");
    if (!get_u64(in, h.program_uid))    throw TraceError("v2 header: program_uid");
    // Skip any extension bytes beyond the fields we know. header_size
    // fixes forward-compatibility.
    if (h.header_size < kCasimV2HeaderBytes) {
        throw TraceError("v2 header: header_size shorter than required");
    }
    const std::size_t consumed = 8 + 2 + 2 + 4 + 4 + 8;  // = 28
    const std::size_t skip = static_cast<std::size_t>(h.header_size) - consumed;
    if (!skip_n(in, skip)) throw TraceError("v2 header: truncated extension bytes");
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

bool operator==(const SyncRecord& a, const SyncRecord& b) {
    return a.kind == b.kind
        && a.sync_object_addr == b.sync_object_addr
        && a.sequence_no      == b.sequence_no
        && a.extra_arg        == b.extra_arg
        && a.ip               == b.ip
        && a.timestamp_ns     == b.timestamp_ns;
}

bool operator==(const LifecycleRecord& a, const LifecycleRecord& b) {
    return a.kind == b.kind
        && a.peer_tid == b.peer_tid
        && a.ip       == b.ip;
}

std::size_t record_bytes(Variant v) {
    switch (v) {
        case Variant::Standard: return kStandardRecordBytes;
        case Variant::CasimV2:  return kCasimV2RecordBytes;
    }
    return 0;
}

std::string_view variant_name(Variant v) {
    switch (v) {
        case Variant::Standard: return "input_instr";
        case Variant::CasimV2:  return "casim_v2";
    }
    return "?";
}

Reader::Reader() = default;

Reader::Reader(std::istream& in, Variant v)
    : in_(&in), variant_(v) {
    if (variant_ == Variant::CasimV2) {
        if (!read_casimv2_header(*in_, header_)) {
            throw TraceError("expected CasimV2 header but magic missing on stream");
        }
    }
}

Reader::Reader(const std::filesystem::path& path, Variant v)
    : variant_(v) {
    owned_ = open_in(path);
    in_ = owned_.get();
    if (variant_ == Variant::CasimV2) {
        if (!read_casimv2_header(*in_, header_)) {
            throw TraceError("expected CasimV2 header but magic missing in: " +
                             path.string());
        }
    }
}

Reader Reader::open_auto(const std::filesystem::path& path) {
    Reader r;
    r.owned_ = open_in(path);
    r.in_ = r.owned_.get();
    // Peek 8 bytes for magic. If it matches, we're CasimV2 and the
    // header parse must succeed; otherwise rewind and use Standard.
    char peek[8];
    r.in_->read(peek, 8);
    const auto got = r.in_->gcount();
    r.in_->clear();
    r.in_->seekg(0, std::ios::beg);
    if (got == 8 && std::memcmp(peek, kCasimV2Magic, 8) == 0) {
        r.variant_ = Variant::CasimV2;
        if (!read_casimv2_header(*r.in_, r.header_)) {
            throw TraceError("v2 magic present but header parse failed: " +
                             path.string());
        }
    } else {
        r.variant_ = Variant::Standard;
    }
    return r;
}

Reader::~Reader() = default;

namespace {

// Syncs that need a gate check at fetch (LockAcquire, BarrierLeave,
// and atomics). Atomics also need retire-side notification.
bool sync_needs_gate(SyncKind k) {
    switch (k) {
        case SyncKind::LockAcquire:
        case SyncKind::BarrierLeave:
        case SyncKind::AtomicRMW:
        case SyncKind::AtomicLoad:
        case SyncKind::AtomicStore:
            return true;
        default:
            return false;
    }
}

// Syncs that need to surface as a pseudo-Inst so SyncSink::notify_retire
// fires at retire time (LockRelease, BarrierArrive, atomics).
bool sync_surfaces_as_pseudo(SyncKind k) {
    switch (k) {
        case SyncKind::LockRelease:
        case SyncKind::BarrierArrive:
        case SyncKind::AtomicRMW:
        case SyncKind::AtomicLoad:
        case SyncKind::AtomicStore:
            return true;
        default:
            return false;
    }
}

void make_pseudo_record(Record& out, const SyncRecord& s) {
    out = Record{};
    out.ip             = s.ip;
    out.has_sync_token = true;
    out.sync_token.kind             = static_cast<std::uint8_t>(s.kind);
    out.sync_token.sync_object_addr = s.sync_object_addr;
    out.sync_token.sequence_no      = s.sequence_no;
    out.sync_token.extra_arg        = s.extra_arg;
}

} // namespace

bool Reader::next(Record& out) {
    out = Record{};
    if (variant_ == Variant::Standard) {
        const bool ok = read_standard(*in_, out);
        if (ok) ++records_read_;
        return ok;
    }
    // CasimV2 path. If a sync record is held pending sink approval,
    // re-poll the sink first.
    while (true) {
        if (pending_sync_) {
            if (sync_sink_ && sync_sink_->try_consume_sync(sync_tid_, *pending_sync_)) {
                const SyncRecord held = *pending_sync_;
                pending_sync_.reset();
                // Gate passed. If this sync also surfaces as a
                // pseudo-Inst (atomic), emit it now.
                if (sync_surfaces_as_pseudo(held.kind)) {
                    make_pseudo_record(out, held);
                    ++records_read_;
                    return true;
                }
                // Otherwise it's a pure-gate sync (LockAcquire,
                // BarrierLeave): drop and continue reading.
            } else {
                // Still blocked — caller will retry next tick.
                return false;
            }
        }

        std::uint8_t t = 0;
        if (!get_u8(*in_, t)) return false;
        const auto type = static_cast<RecordType>(t);

        if (type == RecordType::Instr) {
            const bool ok = read_casimv2_instr_payload(*in_, out);
            if (ok) ++records_read_;
            return ok;
        }

        if (type == RecordType::Sync) {
            SyncRecord s;
            if (!read_casimv2_sync_payload(*in_, s)) return false;
            if (!sync_sink_) {
                // No sink wired: silently drop (legacy behavior).
                continue;
            }
            const bool needs_gate     = sync_needs_gate(s.kind);
            const bool surfaces_retire = sync_surfaces_as_pseudo(s.kind);
            if (needs_gate) {
                if (!sync_sink_->try_consume_sync(sync_tid_, s)) {
                    // Stalled — buffer for re-poll.
                    pending_sync_ = s;
                    return false;
                }
            }
            if (surfaces_retire) {
                make_pseudo_record(out, s);
                ++records_read_;
                return true;
            }
            // Pure-gate, gate passed (or stub passed): drop and continue.
            continue;
        }

        if (type == RecordType::Lifecycle) {
            LifecycleRecord l;
            if (!read_casimv2_lifecycle_payload(*in_, l)) return false;
            if (sync_sink_) sync_sink_->on_lifecycle(sync_tid_, l);
            continue;
        }

        throw TraceError("v2 reader: unknown record type byte=" +
                         std::to_string(static_cast<int>(t)));
    }
}

void Reader::set_sync_sink(SyncSink* sink, std::uint32_t tid) {
    if (variant_ != Variant::CasimV2) {
        throw TraceError("set_sync_sink() requires CasimV2 reader");
    }
    sync_sink_ = sink;
    sync_tid_  = tid;
    // Detaching a sink while a sync is pending is a programmer
    // error — we'd lose the held record. Detach is only safe at
    // EOF or before any next() call.
    if (sync_sink_ == nullptr && pending_sync_) {
        throw TraceError("set_sync_sink(nullptr) called with pending sync");
    }
}

AnyRecord Reader::next_any() {
    if (variant_ != Variant::CasimV2) {
        throw TraceError("next_any() requires CasimV2 reader");
    }
    std::uint8_t t = 0;
    if (!get_u8(*in_, t)) return std::monostate{};
    const auto type = static_cast<RecordType>(t);
    switch (type) {
        case RecordType::Instr: {
            Record r;
            if (!read_casimv2_instr_payload(*in_, r))
                throw TraceError("v2 next_any: instr payload");
            ++records_read_;
            return r;
        }
        case RecordType::Sync: {
            SyncRecord s;
            if (!read_casimv2_sync_payload(*in_, s))
                throw TraceError("v2 next_any: sync payload");
            ++records_read_;
            return s;
        }
        case RecordType::Lifecycle: {
            LifecycleRecord l;
            if (!read_casimv2_lifecycle_payload(*in_, l))
                throw TraceError("v2 next_any: lifecycle payload");
            ++records_read_;
            return l;
        }
    }
    throw TraceError("v2 next_any: unknown record type byte=" +
                     std::to_string(static_cast<int>(t)));
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

void Writer::write_header(const FileHeader& h) {
    if (variant_ != Variant::CasimV2) {
        throw TraceError("write_header() requires CasimV2 writer");
    }
    if (header_written_) throw TraceError("write_header() called twice");
    write_casimv2_header(*out_, h);
    if (!*out_) throw TraceError("write failed (header)");
    header_written_ = true;
}

void Writer::write(const Record& r) {
    if (variant_ == Variant::Standard) {
        write_standard(*out_, r);
    } else {
        if (!header_written_)
            throw TraceError("write_header() must precede write() on CasimV2");
        write_casimv2_instr_packed(*out_, r);
    }
    if (!*out_) throw TraceError("write failed");
    ++records_written_;
}

void Writer::write(const SyncRecord& s) {
    if (variant_ != Variant::CasimV2)
        throw TraceError("SyncRecord write requires CasimV2 writer");
    if (!header_written_)
        throw TraceError("write_header() must precede write() on CasimV2");
    write_casimv2_sync(*out_, s);
    if (!*out_) throw TraceError("write failed (sync)");
    ++records_written_;
}

void Writer::write(const LifecycleRecord& l) {
    if (variant_ != Variant::CasimV2)
        throw TraceError("LifecycleRecord write requires CasimV2 writer");
    if (!header_written_)
        throw TraceError("write_header() must precede write() on CasimV2");
    write_casimv2_lifecycle(*out_, l);
    if (!*out_) throw TraceError("write failed (lifecycle)");
    ++records_written_;
}

void Writer::flush() {
    if (out_) out_->flush();
}

} // namespace comparch::trace
