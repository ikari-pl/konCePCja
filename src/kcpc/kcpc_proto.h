#pragma once

// KCPC binary frontend protocol: constants and the field codec.
//
// The contract is docs/protocol/kcpc-binary-v1.md. This header is the pure
// half of it: opcodes, status codes, capability bits, the 16-byte frame header
// and a bounds-checked reader and writer for the field types of section 4.
// No sockets, no threads, no emulator state, so test_runner covers all of it.
//
// Everything on the wire is little-endian and read or written field by field.
// Nothing here casts a buffer to a struct, so host byte order and compiler
// padding never reach the wire.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace kcpc {

inline constexpr std::uint8_t kMagic[4] = {0x4B, 0x43, 0x50, 0x43};  // "KCPC"
// Major 0 is the development version: client and server must come from the
// same checkout (section 6).
inline constexpr std::uint8_t kVersionMajor = 0;
inline constexpr std::uint16_t kVersionMinor = 0;

inline constexpr std::size_t kHeaderSize = 16;
// Largest payload a client may send; checked before anything is allocated.
inline constexpr std::uint32_t kMaxRequestPayload = 1U << 20;
// Largest payload the server ever sends, and what max_frame 0 means in HELLO.
inline constexpr std::uint32_t kMaxServerFrame = 16U << 20;
// A client may not ask for less: a 64 KiB memory read is always one reply.
inline constexpr std::uint32_t kMinClientMaxFrame = 128U << 10;

inline constexpr int kDefaultPort = 6545;
inline constexpr int kPortProbeCount = 10;  // 6545..6554
inline constexpr std::size_t kMaxClients = 8;

enum class Kind : std::uint8_t {
  kRequest = 1,
  kResponse = 2,
  kEvent = 3,
};

// Section 13. Later slices implement most of these; the numbers are fixed here
// so every slice and the Python client agree on one table.
enum class Opcode : std::uint16_t {
  kError = 0x0000,
  kHello = 0x0001,
  kPing = 0x0002,
  kSubscribe = 0x0003,
  kControl = 0x0004,
  kMachineInfo = 0x0005,
  kPause = 0x0100,
  kRun = 0x0101,
  kStep = 0x0102,
  kReset = 0x0103,
  kRunState = 0x0104,
  kRegsGet = 0x0200,
  kRegsSet = 0x0201,
  kMemInfo = 0x0210,
  kMemRead = 0x0211,
  kMemWrite = 0x0212,
  kBpList = 0x0300,
  kBpAdd = 0x0301,
  kBpDel = 0x0302,
  kBpClear = 0x0303,
  kKey = 0x0400,
  kKeysRelease = 0x0401,
  kText = 0x0402,
  kMouse = 0x0403,
  kInputState = 0x0404,
  kChipGet = 0x0500,
  kSymList = 0x0600,
  kSymAdd = 0x0601,
  kSymDel = 0x0602,
  kSymLoad = 0x0603,
  kFbInfo = 0x0700,
  kMediaInsert = 0x0800,
  kMediaEject = 0x0801,
  kMediaInfo = 0x0802,
  kTapeControl = 0x0803,
  kSnapshotSave = 0x0900,
  kSnapshotLoad = 0x0901,
  kConfigGet = 0x0A00,
  kConfigSet = 0x0A01,
  kConfigApply = 0x0A02,
  kEvtStopped = 0x8001,
  kEvtResumed = 0x8002,
  kEvtReset = 0x8003,
  kEvtFrame = 0x8004,
  kEvtMediaChanged = 0x8005,
  kEvtBreakpointsChanged = 0x8006,
  kEvtSymbolsChanged = 0x8007,
  kEvtMachineChanged = 0x8008,
  kEvtControlChanged = 0x8009,
  kEvtOverrun = 0x800A,
  kEvtShutdown = 0x800B,
};

// Section 5.
enum class Status : std::uint16_t {
  kOk = 0,
  kBadFrame = 1,
  kUnsupportedVersion = 2,
  kHelloRequired = 3,
  kTooManyClients = 4,
  kUnknownOpcode = 5,
  kBadPayload = 6,
  kNotController = 7,
  kBusy = 8,
  kOutOfRange = 9,
  kTooLarge = 10,
  kNotPresent = 11,
  kNotFound = 12,
  kLimitReached = 13,
  kShuttingDown = 14,
  kInternal = 15,
};

enum class Role : std::uint8_t {
  kObserver = 0,
  kController = 1,
};

// Section 6: a bit is set when the server implements the whole group.
inline constexpr std::uint64_t kCapMachineRead = 1ULL << 0;
inline constexpr std::uint64_t kCapMachineWrite = 1ULL << 1;
inline constexpr std::uint64_t kCapRunControl = 1ULL << 2;
inline constexpr std::uint64_t kCapEvents = 1ULL << 3;
inline constexpr std::uint64_t kCapBreakpoints = 1ULL << 4;
inline constexpr std::uint64_t kCapInput = 1ULL << 5;
inline constexpr std::uint64_t kCapChips = 1ULL << 6;
inline constexpr std::uint64_t kCapSymbols = 1ULL << 7;
inline constexpr std::uint64_t kCapFbInfo = 1ULL << 8;
inline constexpr std::uint64_t kCapFbShm = 1ULL << 9;
inline constexpr std::uint64_t kCapMemRom = 1ULL << 10;
inline constexpr std::uint64_t kCapMedia = 1ULL << 11;
inline constexpr std::uint64_t kCapSnapshot = 1ULL << 12;
inline constexpr std::uint64_t kCapConfig = 1ULL << 13;

// What this server implements today. Slice 1 completes no capability group:
// HELLO, PING and MACHINE_INFO need none.
inline constexpr std::uint64_t kServerCaps = 0;

// MACHINE_INFO flags.
inline constexpr std::uint8_t kMachineFlagHeadless = 1U << 0;
inline constexpr std::uint8_t kMachineFlagAsic = 1U << 1;

// Reads fields out of a payload it does not own. Every read is checked against
// the bytes that actually arrived; a failed read reads nothing, sets the
// reader's sticky failure flag and makes every later read fail too, so a
// parser can read a whole payload and test ok() once.
class Reader {
 public:
  Reader(const std::uint8_t* data, std::size_t size)
      : data_(data), size_(size) {}
  explicit Reader(const std::vector<std::uint8_t>& payload)
      : data_(payload.data()), size_(payload.size()) {}

  bool ok() const { return ok_; }
  std::size_t remaining() const { return ok_ ? size_ - pos_ : 0; }

  bool u8(std::uint8_t& out) {
    if (!take(1)) return false;
    out = data_[pos_ - 1];
    return true;
  }
  bool u16(std::uint16_t& out) {
    std::uint64_t v = 0;
    if (!unsigned_le(2, v)) return false;
    out = static_cast<std::uint16_t>(v);
    return true;
  }
  bool u32(std::uint32_t& out) {
    std::uint64_t v = 0;
    if (!unsigned_le(4, v)) return false;
    out = static_cast<std::uint32_t>(v);
    return true;
  }
  bool u64(std::uint64_t& out) { return unsigned_le(8, out); }
  bool i16(std::int16_t& out) {
    std::uint16_t v = 0;
    if (!u16(v)) return false;
    out = static_cast<std::int16_t>(v);
    return true;
  }
  bool i32(std::int32_t& out) {
    std::uint32_t v = 0;
    if (!u32(v)) return false;
    out = static_cast<std::int32_t>(v);
    return true;
  }
  // `str`: a u16 byte count, then that many bytes. The count is checked
  // against what is left before a single byte is copied.
  bool str(std::string& out) {
    std::uint16_t len = 0;
    if (!u16(len)) return false;
    if (!take(len)) return false;
    out.assign(reinterpret_cast<const char*>(data_ + pos_ - len), len);
    return true;
  }
  // `bytes`: the rest of the payload.
  bool bytes(std::vector<std::uint8_t>& out) {
    if (!ok_) return false;
    out.assign(data_ + pos_, data_ + size_);
    pos_ = size_;
    return true;
  }
  // Reserved fields.
  bool skip(std::size_t count) { return take(count); }

 private:
  bool take(std::size_t count) {
    if (!ok_ || count > size_ - pos_) {
      ok_ = false;
      return false;
    }
    pos_ += count;
    return true;
  }
  bool unsigned_le(std::size_t width, std::uint64_t& out) {
    if (!take(width)) return false;
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < width; ++i) {
      v |= static_cast<std::uint64_t>(data_[pos_ - width + i]) << (8 * i);
    }
    out = v;
    return true;
  }

  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

// Builds a payload, field by field.
class Writer {
 public:
  const std::vector<std::uint8_t>& data() const { return buf_; }
  std::vector<std::uint8_t> take() { return std::move(buf_); }
  std::size_t size() const { return buf_.size(); }

  void u8(std::uint8_t v) { buf_.push_back(v); }
  void u16(std::uint16_t v) { unsigned_le(2, v); }
  void u32(std::uint32_t v) { unsigned_le(4, v); }
  void u64(std::uint64_t v) { unsigned_le(8, v); }
  void i16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }
  void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
  // A `str` cannot say more than 65535 bytes. A longer one is cut at the
  // limit rather than written with a wrapped count that would desynchronise
  // every field after it; false tells the caller it was cut.
  bool str(const std::string& s) {
    constexpr std::size_t kMaxStr = 0xFFFF;
    std::size_t const len = s.size() < kMaxStr ? s.size() : kMaxStr;
    u16(static_cast<std::uint16_t>(len));
    buf_.insert(buf_.end(), s.begin(),
                s.begin() + static_cast<std::ptrdiff_t>(len));
    return len == s.size();
  }
  void bytes(const std::uint8_t* data, std::size_t size) {
    buf_.insert(buf_.end(), data, data + size);
  }
  void zeros(std::size_t count) { buf_.insert(buf_.end(), count, 0); }

 private:
  void unsigned_le(std::size_t width, std::uint64_t v) {
    for (std::size_t i = 0; i < width; ++i) {
      buf_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
  }

  std::vector<std::uint8_t> buf_;
};

struct FrameHeader {
  std::uint8_t version = kVersionMajor;
  std::uint8_t kind = 0;  // raw: parse_header() has checked it names a Kind
  std::uint16_t opcode = 0;
  std::uint32_t id = 0;
  std::uint32_t length = 0;
};

enum class HeaderError : std::uint8_t {
  kOk,
  kBadMagic,
  kBadVersion,
  kBadKind,
  kTooLarge,
};

// Checks in wire order, so the first thing wrong is the one reported: a
// client on the wrong port fails on the magic, not on whatever its fifth byte
// happened to be. `out` is filled only on kOk.
inline HeaderError parse_header(const std::uint8_t* data,
                                std::uint32_t max_payload, FrameHeader& out) {
  for (std::size_t i = 0; i < sizeof(kMagic); ++i) {
    if (data[i] != kMagic[i]) return HeaderError::kBadMagic;
  }
  Reader r(data + sizeof(kMagic), kHeaderSize - sizeof(kMagic));
  FrameHeader h;
  r.u8(h.version);
  r.u8(h.kind);
  r.u16(h.opcode);
  r.u32(h.id);
  r.u32(h.length);
  if (h.version != kVersionMajor) return HeaderError::kBadVersion;
  if (h.kind < static_cast<std::uint8_t>(Kind::kRequest) ||
      h.kind > static_cast<std::uint8_t>(Kind::kEvent)) {
    return HeaderError::kBadKind;
  }
  if (h.length > max_payload) return HeaderError::kTooLarge;
  out = h;
  return HeaderError::kOk;
}

// The status a header error is reported with (section 5).
inline Status header_error_status(HeaderError e) {
  switch (e) {
    case HeaderError::kOk:
      return Status::kOk;
    case HeaderError::kBadVersion:
      return Status::kUnsupportedVersion;
    case HeaderError::kBadMagic:
    case HeaderError::kBadKind:
    case HeaderError::kTooLarge:
      return Status::kBadFrame;
  }
  return Status::kBadFrame;
}

// One whole frame: header, then the payload.
inline std::vector<std::uint8_t> encode_frame(
    Kind kind, std::uint16_t opcode, std::uint32_t id,
    const std::vector<std::uint8_t>& payload) {
  Writer w;
  w.bytes(kMagic, sizeof(kMagic));
  w.u8(kVersionMajor);
  w.u8(static_cast<std::uint8_t>(kind));
  w.u16(opcode);
  w.u32(id);
  w.u32(static_cast<std::uint32_t>(payload.size()));
  w.bytes(payload.data(), payload.size());
  return w.take();
}

// Every response payload starts with these four bytes (section 5).
inline void write_status(Writer& w, Status status) {
  w.u16(static_cast<std::uint16_t>(status));
  w.u16(0);
}

// A failed response: the status header and a message for logs and humans.
inline std::vector<std::uint8_t> error_payload(Status status,
                                               const std::string& message) {
  Writer w;
  write_status(w, status);
  w.str(message);
  return w.take();
}

// `stamp`: when a read was taken.
struct Stamp {
  std::uint64_t frame = 0;  // completed frames since the core started
  bool paused = false;
};

inline void write_stamp(Writer& w, const Stamp& s) {
  w.u64(s.frame);
  w.u8(s.paused ? 1 : 0);
  w.zeros(3);
}

inline bool read_stamp(Reader& r, Stamp& out) {
  std::uint8_t run_state = 0;
  r.u64(out.frame);
  r.u8(run_state);
  r.skip(3);
  out.paused = run_state != 0;
  return r.ok();
}

// The MACHINE_INFO reply fields (section 10); also the tail of the HELLO reply.
struct MachineInfo {
  std::uint8_t model = 0;           // 0 = 464, 1 = 664, 2 = 6128, 3 = 6128+
  std::uint8_t crtc_type = 0;       // 0-3
  std::uint8_t effective_tier = 0;  // 0 fast, 1 wake, 2 soldered, 3 faithful
  std::uint8_t flags = 0;           // kMachineFlag*
  std::uint32_t ram_bytes = 0;
  std::uint32_t rom_slots = 0;  // bit N = a ROM is fitted in slot N
  Stamp stamp;
};

inline void write_machine_info(Writer& w, const MachineInfo& m) {
  w.u8(m.model);
  w.u8(m.crtc_type);
  w.u8(m.effective_tier);
  w.u8(m.flags);
  w.u32(m.ram_bytes);
  w.u32(m.rom_slots);
  write_stamp(w, m.stamp);
}

inline bool read_machine_info(Reader& r, MachineInfo& out) {
  r.u8(out.model);
  r.u8(out.crtc_type);
  r.u8(out.effective_tier);
  r.u8(out.flags);
  r.u32(out.ram_bytes);
  r.u32(out.rom_slots);
  return read_stamp(r, out.stamp);
}

struct HelloRequest {
  std::uint16_t client_minor = 0;
  std::uint8_t role = 0;        // raw; the server validates it
  std::uint32_t max_frame = 0;  // 0 = kMaxServerFrame
  std::uint64_t want_caps = 0;
  std::string client_name;
};

inline bool read_hello_request(Reader& r, HelloRequest& out) {
  r.u16(out.client_minor);
  r.u8(out.role);
  r.skip(1);
  r.u32(out.max_frame);
  r.u64(out.want_caps);
  return r.str(out.client_name);
}

inline void write_hello_request(Writer& w, const HelloRequest& h) {
  w.u16(h.client_minor);
  w.u8(h.role);
  w.u8(0);
  w.u32(h.max_frame);
  w.u64(h.want_caps);
  w.str(h.client_name);
}

struct HelloReply {
  std::uint16_t server_minor = kVersionMinor;
  Role role = Role::kObserver;
  std::uint32_t max_frame = kMaxServerFrame;
  std::uint64_t caps = kServerCaps;
  std::uint32_t client_id = 0;
  std::uint32_t pid = 0;
  std::string server_version;
  MachineInfo machine;
};

inline void write_hello_reply(Writer& w, const HelloReply& h) {
  w.u16(h.server_minor);
  w.u8(static_cast<std::uint8_t>(h.role));
  w.u8(0);
  w.u32(h.max_frame);
  w.u64(h.caps);
  w.u32(h.client_id);
  w.u32(h.pid);
  w.str(h.server_version);
  write_machine_info(w, h.machine);
}

inline bool read_hello_reply(Reader& r, HelloReply& out) {
  std::uint8_t role = 0;
  r.u16(out.server_minor);
  r.u8(role);
  r.skip(1);
  r.u32(out.max_frame);
  r.u64(out.caps);
  r.u32(out.client_id);
  r.u32(out.pid);
  r.str(out.server_version);
  out.role = role == 1 ? Role::kController : Role::kObserver;
  return read_machine_info(r, out.machine);
}

}  // namespace kcpc
