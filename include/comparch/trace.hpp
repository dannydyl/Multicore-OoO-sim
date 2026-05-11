#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

namespace comparch::trace {

inline constexpr int kStandardDestRegs = 2;
inline constexpr int kStandardSrcRegs  = 4;
inline constexpr int kStandardDestMem  = 2;
inline constexpr int kStandardSrcMem   = 4;

inline constexpr std::size_t kStandardRecordBytes = 64;

// CasimV2 = thread-aware multi-record trace. 32-byte file header,
// then a stream of 64-byte records each tagged by a leading
// RecordType byte. Designed so that ChampSim files (no header)
// can still be read as Standard via header-magic auto-detection.
inline constexpr std::size_t kCasimV2RecordBytes = 64;
inline constexpr std::size_t kCasimV2HeaderBytes = 32;
inline constexpr char        kCasimV2Magic[8]    = {'C','A','S','I','M','T','R','2'};
inline constexpr std::uint16_t kCasimV2Version   = 2;

enum class Variant {
    Standard,   // legacy ChampSim 64-byte records, no file header
    CasimV2,    // 32-byte header + tagged 64-byte records (instr|sync|lifecycle)
};

// Leading byte of every CasimV2 record. Type 0 keeps the ChampSim
// payload in the remaining 63 bytes (with one byte of slack folded
// into the existing pad). Sync/Lifecycle use their own payloads.
enum class RecordType : std::uint8_t {
    Instr     = 0,
    Sync      = 1,
    Lifecycle = 2,
};

// Synchronization event kinds. Recorded by the tracer (DynamoRIO
// hooks pthread/atomic call sites) and consumed by the sim's
// SyncCoordinator to enforce happens-before across threads.
enum class SyncKind : std::uint8_t {
    LockAcquire   = 1,
    LockRelease   = 2,
    BarrierArrive = 3,
    BarrierLeave  = 4,
    AtomicRMW     = 5,
    AtomicLoad    = 6,
    AtomicStore   = 7,
    CondWait      = 8,
    CondSignal    = 9,
    CondBroadcast = 10,
};

// Thread lifecycle events. ThreadStart is emitted as the first
// record after the file header (so consumers know the TID even
// without parsing the header). Spawn/Join carry the peer TID in
// `peer_tid`.
enum class LifecycleKind : std::uint8_t {
    ThreadStart  = 1,
    ThreadExit   = 2,
    ThreadSpawn  = 3,   // this thread spawned peer_tid
    ThreadJoin   = 4,   // this thread joined peer_tid
};

// Sync pseudo-record body. When a CasimV2 reader encounters a
// signal-side sync (LockRelease, BarrierArrive, atomics), it
// surfaces a Record with has_sync_token=true and these fields
// populated. The OoO core's classify produces a sync pseudo-Inst
// that flows through the pipeline as a zero-dep ALU and fires
// SyncSink::notify_retire on retire. See SyncSink comment for the
// gate/signal split.
struct SyncToken {
    std::uint8_t  kind             = 0;  // SyncKind, stored raw
    std::uint64_t sync_object_addr = 0;
    std::uint64_t sequence_no      = 0;
    std::uint64_t extra_arg        = 0;
};

struct Record {
    std::uint64_t ip = 0;
    bool is_branch    = false;
    bool branch_taken = false;
    // CasimV2-only: explicit multiply hint. ChampSim records don't
    // carry an opcode class, so a record that's neither branch nor
    // memory falls through to Opcode::Alu by default — leaving the
    // MUL functional units unused. CasimV2 producers (casim_synth,
    // future DR client) can set is_mul=true to route the record to
    // the multi-stage MUL pipeline instead. Always false in
    // Standard-variant records.
    bool is_mul       = false;
    std::array<std::uint8_t,  kStandardDestRegs> destination_registers{};
    std::array<std::uint8_t,  kStandardSrcRegs>  source_registers{};
    std::array<std::uint64_t, kStandardDestMem>  destination_memory{};
    std::array<std::uint64_t, kStandardSrcMem>   source_memory{};

    // When true, this Record is a sync pseudo-instruction (not a
    // real architectural instruction). Carries the SyncRecord
    // payload through the pipeline so it can be delivered to the
    // SyncCoordinator at retire time. Defaults to false so all
    // existing producers/consumers see no change.
    bool      has_sync_token = false;
    SyncToken sync_token{};
};

// SyncRecord. The `sequence_no` is set by the tracer at trace
// time and is the happens-before primitive: for one
// `sync_object_addr`, sequence_no values are monotonic across all
// threads in trace order. The sim uses (object, seq) to pair
// matching events (LockAcquire(obj, n) waits for LockRelease(obj,
// n-1) to retire on its owning thread, etc.).
struct SyncRecord {
    SyncKind        kind            = SyncKind::LockAcquire;
    std::uint64_t   sync_object_addr = 0;  // VA of mutex/barrier/cond/atomic loc
    std::uint64_t   sequence_no      = 0;  // per-object monotonic
    std::uint64_t   extra_arg        = 0;  // memorder, participant count, paired seq
    std::uint64_t   ip               = 0;  // call-site PC (debug)
    std::uint64_t   timestamp_ns     = 0;  // tracer wall clock (debug; NOT used in replay)
};

struct LifecycleRecord {
    LifecycleKind   kind     = LifecycleKind::ThreadStart;
    std::uint32_t   peer_tid = 0;          // for Spawn/Join
    std::uint64_t   ip       = 0;          // call-site PC (debug)
};

// Tagged record returned by Reader::next_any(). std::monostate
// indicates EOF without an error.
using AnyRecord = std::variant<std::monostate, Record, SyncRecord, LifecycleRecord>;

// Sink invoked by Reader (CasimV2 only) when sync/lifecycle records
// surface in the stream. SyncCoordinator implements this; the
// interface lives here so Reader can hold a SyncSink* without
// pulling in the coordinator header.
//
// Two-phase sync model
// --------------------
// Sync events have a *gate* phase and a *signal* phase. They map
// to retire-time hardware semantics:
//   - LockAcquire / BarrierLeave : pure gate. Stall fetch until
//     prior matching release/arrive has retired. Once approved,
//     drop the record (no pipeline presence).
//   - LockRelease / BarrierArrive : pure signal. Pass fetch
//     immediately, flow through pipeline as a pseudo-instr, fire
//     notify_retire when the pseudo-instr retires.
//   - AtomicRMW / AtomicLoad / AtomicStore : both. Gate at fetch
//     against the prior atomic seq, then flow as a pseudo-instr
//     that fires notify_retire on retire.
//
// try_consume_sync checks the gate (returns true if the record
// can flow past fetch). notify_retire advances coordinator state
// after the pseudo-instr has actually retired.
class SyncSink {
public:
    virtual ~SyncSink() = default;
    // Gate check. Returns true if reader for `tid` may consume the
    // sync record (preconditions satisfied). False = stall: reader
    // holds the record at the head and re-polls on the next next().
    // Does NOT advance coordinator state for signal-side events —
    // notify_retire does that.
    virtual bool try_consume_sync(std::uint32_t tid, const SyncRecord& s) = 0;
    // Signal-side: called by the OoO core's retire stage when a
    // sync pseudo-instr commits. Advances coordinator state (e.g.
    // last_completed_seq for locks). No-op for pure-gate kinds.
    virtual void notify_retire(std::uint32_t tid, const SyncRecord& s) = 0;
    // Advisory only; never gates progress.
    virtual void on_lifecycle(std::uint32_t tid, const LifecycleRecord& l) = 0;
};

// File header for CasimV2. Written once at offset 0; record stream
// follows at offset 32. `header_size` is recorded so future fields
// can be appended without breaking older readers (they skip the
// extra bytes).
struct FileHeader {
    std::uint16_t format_version = kCasimV2Version;
    std::uint16_t header_size    = static_cast<std::uint16_t>(kCasimV2HeaderBytes);
    std::uint32_t thread_id      = 0;     // this file's TID (one file = one thread)
    std::uint32_t thread_count   = 1;     // total threads in this program
    std::uint64_t program_uid    = 0;     // shared across all files of one program
};

bool operator==(const Record& a, const Record& b);
inline bool operator!=(const Record& a, const Record& b) { return !(a == b); }
bool operator==(const SyncRecord& a, const SyncRecord& b);
inline bool operator!=(const SyncRecord& a, const SyncRecord& b) { return !(a == b); }
bool operator==(const LifecycleRecord& a, const LifecycleRecord& b);
inline bool operator!=(const LifecycleRecord& a, const LifecycleRecord& b) { return !(a == b); }

class TraceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Reader {
public:
    Reader(std::istream& in, Variant v);
    explicit Reader(const std::filesystem::path& path, Variant v = Variant::Standard);

    // Auto-detecting constructor. Peeks the first 8 bytes for the
    // CasimV2 magic; on match, opens as CasimV2 and parses the
    // header. On miss, rewinds and opens as Standard. This is the
    // preferred entry point for code that needs to consume both
    // formats (e.g. full_mode where some traces are champsim and
    // some are casim).
    static Reader open_auto(const std::filesystem::path& path);

    ~Reader();

    Reader(const Reader&)            = delete;
    Reader& operator=(const Reader&) = delete;
    Reader(Reader&&)                 = default;
    Reader& operator=(Reader&&)      = default;

    // Read the next instruction record. For Standard variant this
    // is the only call that makes sense. For CasimV2 with no sync
    // sink, sync/lifecycle records are skipped silently. With a
    // sync sink wired, sync records are routed through the sink:
    // if try_consume_sync returns false the record is held at the
    // head, blocked() becomes true, and next() returns false
    // without setting EOF. The caller (typically OoO fetch) must
    // distinguish blocked() from real EOF before stopping.
    bool next(Record& out);

    // Read the next record of any kind (CasimV2 only). Returns
    // monostate at EOF. Throws TraceError if called on a Standard
    // reader. Bypasses the sync sink — intended for tools that
    // need to inspect every record (replay debuggers, dumpers).
    AnyRecord next_any();

    // CasimV2 only. Wires the SyncSink and tells it which TID this
    // file represents. Once wired, sync records gate progress.
    // Lifecycle records are forwarded too (advisory). Pass nullptr
    // to detach.
    void set_sync_sink(SyncSink* sink, std::uint32_t tid);

    // True when next() last returned false because the head sync
    // record was rejected by the sink (not because of EOF). Cleared
    // when the sink later approves and the record is consumed.
    bool blocked() const { return pending_sync_.has_value(); }

    Variant            variant() const { return variant_; }
    std::size_t        records_read() const { return records_read_; }
    const FileHeader&  header() const { return header_; }
    std::uint32_t      sync_tid() const { return sync_tid_; }
    SyncSink*          sync_sink() const { return sync_sink_; }

private:
    Reader();   // for open_auto

    std::istream*                  in_ = nullptr;
    std::unique_ptr<std::istream>  owned_;
    Variant                        variant_ = Variant::Standard;
    std::size_t                    records_read_ = 0;
    FileHeader                     header_{};

    // Sync-sink integration. Used only when variant_ == CasimV2 and
    // sync_sink_ != nullptr. pending_sync_ holds a sync record that
    // was read off the stream but not yet approved by the sink — on
    // the next next() call we re-poll. Without a sink, sync records
    // are dropped on the floor (legacy v2 behavior).
    SyncSink*                      sync_sink_ = nullptr;
    std::uint32_t                  sync_tid_  = 0;
    std::optional<SyncRecord>      pending_sync_;
};

class Writer {
public:
    Writer(std::ostream& out, Variant v);
    explicit Writer(const std::filesystem::path& path, Variant v = Variant::Standard);
    ~Writer();

    Writer(const Writer&)            = delete;
    Writer& operator=(const Writer&) = delete;

    // CasimV2 only: must be called once before any write() to
    // populate the file header. Throws if called on Standard.
    void write_header(const FileHeader& h);

    // Append an instruction record. For Standard this writes the
    // legacy 64-byte ChampSim layout. For CasimV2 this prefixes
    // RecordType::Instr and writes the same payload (with the
    // existing 1 byte of struct padding repurposed for the type
    // tag, keeping the on-disk record at 64 bytes).
    void write(const Record& r);

    // CasimV2-only writers for the new record kinds.
    void write(const SyncRecord& s);
    void write(const LifecycleRecord& l);

    void flush();

    Variant      variant() const { return variant_; }
    std::size_t  records_written() const { return records_written_; }

private:
    std::ostream*            out_;
    std::unique_ptr<std::ostream> owned_;
    Variant                  variant_;
    std::size_t              records_written_ = 0;
    bool                     header_written_ = false;
};

std::size_t      record_bytes(Variant v);
std::string_view variant_name(Variant v);

} // namespace comparch::trace
