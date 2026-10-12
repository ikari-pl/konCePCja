// konCePCja — tests for the KCPC binary protocol codec (beads-3o1.1).
//
// The codec is the part of the server that reads bytes a client chose, so the
// property pinned hardest here is that a short, oversized or hostile payload
// is refused without a single read past the bytes that arrived. Buffers are
// heap vectors sized exactly, so AddressSanitizer flags any such read.

#include "kcpc/kcpc_proto.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using kcpc::Reader;
using kcpc::Writer;

std::vector<std::uint8_t> header_bytes(std::uint8_t version, std::uint8_t kind,
                                       std::uint16_t opcode, std::uint32_t id,
                                       std::uint32_t length) {
  Writer w;
  w.bytes(kcpc::kMagic, sizeof(kcpc::kMagic));
  w.u8(version);
  w.u8(kind);
  w.u16(opcode);
  w.u32(id);
  w.u32(length);
  return w.take();
}

}  // namespace

TEST(KcpcCodec, WritesLittleEndian) {
  Writer w;
  w.u8(0x11);
  w.u16(0x2233);
  w.u32(0x44556677);
  w.u64(0x8899AABBCCDDEEFFULL);
  std::vector<std::uint8_t> const expected = {0x11, 0x33, 0x22, 0x77, 0x66,
                                              0x55, 0x44, 0xFF, 0xEE, 0xDD,
                                              0xCC, 0xBB, 0xAA, 0x99, 0x88};
  EXPECT_EQ(w.data(), expected);
}

TEST(KcpcCodec, RoundTripsEveryFieldType) {
  Writer w;
  w.u8(0xFE);
  w.u16(0xBEEF);
  w.u32(0xDEADBEEF);
  w.u64(0x0123456789ABCDEFULL);
  w.i16(-2);
  w.i32(INT32_MIN);
  EXPECT_TRUE(w.str("koncepcja-gui 6.5.0"));
  EXPECT_TRUE(w.str(""));
  const std::uint8_t tail[] = {1, 2, 3};
  w.bytes(tail, sizeof(tail));

  Reader r(w.data());
  std::uint8_t a = 0;
  std::uint16_t b = 0;
  std::uint32_t c = 0;
  std::uint64_t d = 0;
  std::int16_t e = 0;
  std::int32_t f = 0;
  std::string s;
  std::string empty = "x";
  std::vector<std::uint8_t> rest;
  EXPECT_TRUE(r.u8(a));
  EXPECT_TRUE(r.u16(b));
  EXPECT_TRUE(r.u32(c));
  EXPECT_TRUE(r.u64(d));
  EXPECT_TRUE(r.i16(e));
  EXPECT_TRUE(r.i32(f));
  EXPECT_TRUE(r.str(s));
  EXPECT_TRUE(r.str(empty));
  EXPECT_TRUE(r.bytes(rest));
  EXPECT_EQ(a, 0xFE);
  EXPECT_EQ(b, 0xBEEF);
  EXPECT_EQ(c, 0xDEADBEEFU);
  EXPECT_EQ(d, 0x0123456789ABCDEFULL);
  EXPECT_EQ(e, -2);
  EXPECT_EQ(f, INT32_MIN);
  EXPECT_EQ(s, "koncepcja-gui 6.5.0");
  EXPECT_EQ(empty, "");
  EXPECT_EQ(rest, (std::vector<std::uint8_t>{1, 2, 3}));
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.remaining(), 0U);
}

// Each integer width, with every possible shortfall: the read fails and the
// output is left alone.
TEST(KcpcCodec, TruncatedIntegersAreRejected) {
  for (std::size_t width : {1U, 2U, 4U, 8U}) {
    for (std::size_t have = 0; have < width; ++have) {
      std::vector<std::uint8_t> const buf(have, 0xFF);
      Reader r(buf);
      std::uint8_t v8 = 0x5A;
      std::uint16_t v16 = 0x5A5A;
      std::uint32_t v32 = 0x5A5A5A5A;
      std::uint64_t v64 = 0x5A5A5A5A5A5A5A5AULL;
      bool ok = true;
      switch (width) {
        case 1:
          ok = r.u8(v8);
          break;
        case 2:
          ok = r.u16(v16);
          break;
        case 4:
          ok = r.u32(v32);
          break;
        default:
          ok = r.u64(v64);
          break;
      }
      EXPECT_FALSE(ok) << "width " << width << " with " << have << " bytes";
      EXPECT_FALSE(r.ok());
      EXPECT_EQ(v8, 0x5A);
      EXPECT_EQ(v16, 0x5A5A);
      EXPECT_EQ(v32, 0x5A5A5A5AU);
      EXPECT_EQ(v64, 0x5A5A5A5A5A5A5A5AULL);
    }
  }
}

// A str whose count says more than the payload holds: the classic length
// field attack. Nothing may be copied.
TEST(KcpcCodec, OversizedStrLengthIsRejected) {
  // Count 0xFFFF, three bytes present.
  std::vector<std::uint8_t> const lying = {0xFF, 0xFF, 'a', 'b', 'c'};
  Reader r(lying);
  std::string s = "untouched";
  EXPECT_FALSE(r.str(s));
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(s, "untouched");

  // Off by one: count 4, three bytes present.
  std::vector<std::uint8_t> const short_by_one = {0x04, 0x00, 'a', 'b', 'c'};
  Reader r2(short_by_one);
  EXPECT_FALSE(r2.str(s));
  EXPECT_EQ(s, "untouched");

  // The count itself is cut in half.
  std::vector<std::uint8_t> const half_count = {0x03};
  Reader r3(half_count);
  EXPECT_FALSE(r3.str(s));
  EXPECT_EQ(s, "untouched");

  // Exactly enough is fine.
  std::vector<std::uint8_t> const exact = {0x03, 0x00, 'a', 'b', 'c'};
  Reader r4(exact);
  EXPECT_TRUE(r4.str(s));
  EXPECT_EQ(s, "abc");
}

// One failed read poisons the reader: a parser can read a whole payload and
// check once, and can never continue from a misaligned position.
TEST(KcpcCodec, FailureIsSticky) {
  std::vector<std::uint8_t> const buf = {0x01, 0x02, 0x03};
  Reader r(buf);
  std::uint32_t big = 0;
  std::uint8_t small = 0x77;
  std::vector<std::uint8_t> rest = {9};
  EXPECT_FALSE(r.u32(big));
  EXPECT_FALSE(r.u8(small)) << "three bytes are left, but the reader failed";
  EXPECT_FALSE(r.skip(1));
  EXPECT_FALSE(r.bytes(rest));
  EXPECT_EQ(small, 0x77);
  EXPECT_EQ(rest, (std::vector<std::uint8_t>{9}));
  EXPECT_EQ(r.remaining(), 0U);
}

TEST(KcpcCodec, EmptyPayloadReadsNothing) {
  Reader r(nullptr, 0);
  std::uint8_t v = 0;
  std::vector<std::uint8_t> rest = {1};
  EXPECT_TRUE(r.bytes(rest)) << "`bytes` of an empty payload is empty";
  EXPECT_TRUE(rest.empty());
  EXPECT_FALSE(r.u8(v));
}

TEST(KcpcCodec, SkipIsBoundsChecked) {
  std::vector<std::uint8_t> const buf(4, 0);
  Reader r(buf);
  EXPECT_TRUE(r.skip(4));
  EXPECT_TRUE(r.skip(0));
  EXPECT_FALSE(r.skip(1));
  // A skip count near SIZE_MAX must not wrap the position.
  Reader r2(buf);
  EXPECT_TRUE(r2.skip(1));
  EXPECT_FALSE(r2.skip(SIZE_MAX));
}

TEST(KcpcCodec, StrLongerThanTheCountCanSayIsCut) {
  Writer w;
  EXPECT_FALSE(w.str(std::string(70000, 'x')));
  EXPECT_EQ(w.size(), 2U + 0xFFFFU);
  Reader r(w.data());
  std::string s;
  EXPECT_TRUE(r.str(s));
  EXPECT_EQ(s.size(), 0xFFFFU);
  EXPECT_EQ(r.remaining(), 0U);
}

TEST(KcpcHeader, RoundTrips) {
  std::vector<std::uint8_t> const payload = {0xAA, 0xBB, 0xCC};
  std::vector<std::uint8_t> const frame =
      kcpc::encode_frame(kcpc::Kind::kResponse, 0x0211, 0xCAFEF00D, payload);
  ASSERT_EQ(frame.size(), kcpc::kHeaderSize + payload.size());
  // The magic is the ASCII text a hexdump shows.
  EXPECT_EQ(std::string(frame.begin(), frame.begin() + 4), "KCPC");

  kcpc::FrameHeader h;
  ASSERT_EQ(kcpc::parse_header(frame.data(), kcpc::kMaxRequestPayload, h),
            kcpc::HeaderError::kOk);
  EXPECT_EQ(h.version, kcpc::kVersionMajor);
  EXPECT_EQ(h.kind, static_cast<std::uint8_t>(kcpc::Kind::kResponse));
  EXPECT_EQ(h.opcode, 0x0211);
  EXPECT_EQ(h.id, 0xCAFEF00DU);
  EXPECT_EQ(h.length, 3U);
}

// The byte-by-byte example of the design document, section 14: a PING request
// with id 2.
TEST(KcpcHeader, MatchesTheDocumentedBytes) {
  std::vector<std::uint8_t> const expected = {
      0x4B, 0x43, 0x50, 0x43, 0x00, 0x01, 0x02, 0x00,
      0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  EXPECT_EQ(kcpc::encode_frame(kcpc::Kind::kRequest,
                               static_cast<std::uint16_t>(kcpc::Opcode::kPing),
                               2, {}),
            expected);
}

TEST(KcpcHeader, RejectsBadMagic) {
  // What a text IPC client would send to this port by mistake.
  const char* const text = "ping\nping\nping\n.";
  kcpc::FrameHeader h;
  h.id = 77;
  EXPECT_EQ(kcpc::parse_header(reinterpret_cast<const std::uint8_t*>(text),
                               kcpc::kMaxRequestPayload, h),
            kcpc::HeaderError::kBadMagic);
  EXPECT_EQ(h.id, 77U) << "the header is filled only on success";
}

TEST(KcpcHeader, RejectsWrongMajor) {
  kcpc::FrameHeader h;
  std::vector<std::uint8_t> const raw = header_bytes(1, 1, 2, 1, 0);
  EXPECT_EQ(kcpc::parse_header(raw.data(), kcpc::kMaxRequestPayload, h),
            kcpc::HeaderError::kBadVersion);
  EXPECT_EQ(kcpc::header_error_status(kcpc::HeaderError::kBadVersion),
            kcpc::Status::kUnsupportedVersion);
}

TEST(KcpcHeader, RejectsUnknownKind) {
  kcpc::FrameHeader h;
  for (std::uint8_t kind : {0, 4, 255}) {
    std::vector<std::uint8_t> const raw = header_bytes(0, kind, 2, 1, 0);
    EXPECT_EQ(kcpc::parse_header(raw.data(), kcpc::kMaxRequestPayload, h),
              kcpc::HeaderError::kBadKind)
        << "kind " << static_cast<int>(kind);
  }
  EXPECT_EQ(kcpc::header_error_status(kcpc::HeaderError::kBadKind),
            kcpc::Status::kBadFrame);
}

// A length above 1 MiB is refused from the header alone, before anyone
// allocates or reads a payload.
TEST(KcpcHeader, RejectsLengthAboveOneMebibyte) {
  kcpc::FrameHeader h;
  std::vector<std::uint8_t> const at_limit = header_bytes(0, 1, 2, 1, 1U << 20);
  EXPECT_EQ(kcpc::parse_header(at_limit.data(), kcpc::kMaxRequestPayload, h),
            kcpc::HeaderError::kOk);
  for (std::uint32_t length : {(1U << 20) + 1, 0x7FFFFFFFU, 0xFFFFFFFFU}) {
    std::vector<std::uint8_t> const raw = header_bytes(0, 1, 2, 1, length);
    EXPECT_EQ(kcpc::parse_header(raw.data(), kcpc::kMaxRequestPayload, h),
              kcpc::HeaderError::kTooLarge)
        << "length " << length;
  }
  EXPECT_EQ(kcpc::header_error_status(kcpc::HeaderError::kTooLarge),
            kcpc::Status::kBadFrame);
}

// The first thing wrong is the one reported: bad magic wins over everything
// after it, so a client on the wrong port is told so.
TEST(KcpcHeader, ReportsTheFirstFault) {
  std::vector<std::uint8_t> raw = header_bytes(9, 9, 2, 1, 0xFFFFFFFFU);
  kcpc::FrameHeader h;
  EXPECT_EQ(kcpc::parse_header(raw.data(), kcpc::kMaxRequestPayload, h),
            kcpc::HeaderError::kBadVersion);
  raw[0] = 'X';
  EXPECT_EQ(kcpc::parse_header(raw.data(), kcpc::kMaxRequestPayload, h),
            kcpc::HeaderError::kBadMagic);
}

TEST(KcpcPayloads, ErrorPayloadIsStatusThenMessage) {
  std::vector<std::uint8_t> const p =
      kcpc::error_payload(kcpc::Status::kTooManyClients, "full");
  std::vector<std::uint8_t> const expected = {0x04, 0x00, 0x00, 0x00, 0x04,
                                              0x00, 'f',  'u',  'l',  'l'};
  EXPECT_EQ(p, expected);
}

TEST(KcpcPayloads, StampIsTwelveBytes) {
  Writer w;
  kcpc::write_stamp(w, {298, true});
  std::vector<std::uint8_t> const expected = {0x2A, 0x01, 0, 0, 0, 0,
                                              0,    0,    1, 0, 0, 0};
  EXPECT_EQ(w.data(), expected);
  Reader r(w.data());
  kcpc::Stamp s;
  EXPECT_TRUE(kcpc::read_stamp(r, s));
  EXPECT_EQ(s.frame, 298U);
  EXPECT_TRUE(s.paused);
}

TEST(KcpcPayloads, HelloRequestRoundTripsAndRejectsEveryTruncation) {
  kcpc::HelloRequest in;
  in.client_minor = 0;
  in.role = 1;
  in.max_frame = 1U << 20;
  in.want_caps = kcpc::kCapMachineRead | kcpc::kCapEvents;
  in.client_name = "py";
  Writer w;
  kcpc::write_hello_request(w, in);
  // Section 14 shows this payload as 20 bytes.
  ASSERT_EQ(w.size(), 20U);

  Reader r(w.data());
  kcpc::HelloRequest out;
  ASSERT_TRUE(kcpc::read_hello_request(r, out));
  EXPECT_EQ(out.role, 1);
  EXPECT_EQ(out.max_frame, 1U << 20);
  EXPECT_EQ(out.want_caps, in.want_caps);
  EXPECT_EQ(out.client_name, "py");

  for (std::size_t cut = 0; cut < w.size(); ++cut) {
    std::vector<std::uint8_t> const part(w.data().begin(),
                                         w.data().begin() + cut);
    Reader short_reader(part);
    kcpc::HelloRequest ignored;
    EXPECT_FALSE(kcpc::read_hello_request(short_reader, ignored))
        << "HELLO cut to " << cut << " bytes was accepted";
  }
}

TEST(KcpcPayloads, HelloReplyRoundTrips) {
  kcpc::HelloReply in;
  in.max_frame = 123456;
  in.caps = kcpc::kCapMachineRead;
  in.client_id = 7;
  in.pid = 4242;
  in.server_version = "v6.5.0";
  in.machine.model = 3;
  in.machine.crtc_type = 3;
  in.machine.effective_tier = 1;
  in.machine.flags = kcpc::kMachineFlagHeadless | kcpc::kMachineFlagAsic;
  in.machine.ram_bytes = 576U * 1024U;
  in.machine.rom_slots = 0x81;
  in.machine.stamp = {1000, false};
  Writer w;
  kcpc::write_hello_reply(w, in);

  Reader r(w.data());
  kcpc::HelloReply out;
  ASSERT_TRUE(kcpc::read_hello_reply(r, out));
  EXPECT_EQ(r.remaining(), 0U);
  EXPECT_EQ(out.role, kcpc::Role::kObserver);
  EXPECT_EQ(out.max_frame, 123456U);
  EXPECT_EQ(out.caps, kcpc::kCapMachineRead);
  EXPECT_EQ(out.client_id, 7U);
  EXPECT_EQ(out.pid, 4242U);
  EXPECT_EQ(out.server_version, "v6.5.0");
  EXPECT_EQ(out.machine.model, 3);
  EXPECT_EQ(out.machine.crtc_type, 3);
  EXPECT_EQ(out.machine.effective_tier, 1);
  EXPECT_EQ(out.machine.flags, 3);
  EXPECT_EQ(out.machine.ram_bytes, 576U * 1024U);
  EXPECT_EQ(out.machine.rom_slots, 0x81U);
  EXPECT_EQ(out.machine.stamp.frame, 1000U);
  EXPECT_FALSE(out.machine.stamp.paused);
}

// A later minor version may append fields; a reader must ignore what follows
// the fields it knows (section 4, "Growing a payload").
TEST(KcpcPayloads, TrailingBytesAreIgnored) {
  Writer w;
  kcpc::MachineInfo in;
  in.model = 2;
  in.ram_bytes = 128U * 1024U;
  kcpc::write_machine_info(w, in);
  w.u32(0xFEEDFACE);  // a field from the future
  Reader r(w.data());
  kcpc::MachineInfo out;
  EXPECT_TRUE(kcpc::read_machine_info(r, out));
  EXPECT_EQ(out.ram_bytes, 128U * 1024U);
  EXPECT_EQ(r.remaining(), 4U);
}

// ---- Slice 2: the machine reads (beads-3o1.2) ----------------------------

// Section 12: twelve 16-bit registers, six bytes, two reserved zero bytes.
TEST(KcpcProto, RegisterBlockIs32BytesInWireOrder) {
  kcpc::Regs in;
  in.af = 0x0102;
  in.bc = 0x0304;
  in.de = 0x0506;
  in.hl = 0x0708;
  in.ix = 0x090A;
  in.iy = 0x0B0C;
  in.sp = 0x0D0E;
  in.pc = 0x0F10;
  in.af_alt = 0x1112;
  in.bc_alt = 0x1314;
  in.de_alt = 0x1516;
  in.hl_alt = 0x1718;
  in.i = 0x19;
  in.r = 0x1A;
  in.iff1 = 0x1B;
  in.iff2 = 0x1C;
  in.im = 0x1D;
  in.halt = 0x1E;
  kcpc::Writer w;
  kcpc::write_regs(w, in);
  ASSERT_EQ(w.size(), kcpc::kRegsSize);
  // Every field is one more than the one before, low byte first, so the block
  // reads 02 01 04 03 ... 18 17, then 19 .. 1E, then the two reserved zeros.
  for (std::size_t field = 0; field < 12; ++field) {
    EXPECT_EQ(w.data()[field * 2], 2 * field + 2) << "field " << field;
    EXPECT_EQ(w.data()[(field * 2) + 1], 2 * field + 1) << "field " << field;
  }
  for (std::size_t i = 24; i < 30; ++i) EXPECT_EQ(w.data()[i], i + 1);
  EXPECT_EQ(w.data()[30], 0);
  EXPECT_EQ(w.data()[31], 0);

  kcpc::Reader r(w.data());
  kcpc::Regs out;
  ASSERT_TRUE(kcpc::read_regs(r, out));
  EXPECT_EQ(r.remaining(), 0U);
  EXPECT_EQ(out.af, in.af);
  EXPECT_EQ(out.iy, in.iy);
  EXPECT_EQ(out.pc, in.pc);
  EXPECT_EQ(out.af_alt, in.af_alt);
  EXPECT_EQ(out.hl_alt, in.hl_alt);
  EXPECT_EQ(out.i, in.i);
  EXPECT_EQ(out.r, in.r);
  EXPECT_EQ(out.iff1, in.iff1);
  EXPECT_EQ(out.iff2, in.iff2);
  EXPECT_EQ(out.im, in.im);
  EXPECT_EQ(out.halt, in.halt);

  kcpc::Reader cut(w.data().data(), kcpc::kRegsSize - 1);
  EXPECT_FALSE(kcpc::read_regs(cut, out));
}

TEST(KcpcProto, MemInfoRoundTrips) {
  kcpc::MemInfo in;
  in.ram_bytes = 0x00090000;
  in.ram_config = 0xC4;
  in.rom_config = 0x8D;
  in.upper_rom = 7;
  in.rom_slots = 0x80000081;
  in.stamp = {0x0102030405060708ULL, true};
  kcpc::Writer w;
  kcpc::write_mem_info(w, in);
  ASSERT_EQ(w.size(), 24U);  // u32, 3 x u8, reserved, u32, stamp
  EXPECT_EQ(w.data()[7], 0) << "reserved";
  kcpc::Reader r(w.data());
  kcpc::MemInfo out;
  ASSERT_TRUE(kcpc::read_mem_info(r, out));
  EXPECT_EQ(out.ram_bytes, in.ram_bytes);
  EXPECT_EQ(out.ram_config, in.ram_config);
  EXPECT_EQ(out.rom_config, in.rom_config);
  EXPECT_EQ(out.upper_rom, in.upper_rom);
  EXPECT_EQ(out.rom_slots, in.rom_slots);
  EXPECT_EQ(out.stamp.frame, in.stamp.frame);
  EXPECT_TRUE(out.stamp.paused);
}

TEST(KcpcProto, MemReadRequestIs12Bytes) {
  kcpc::MemReadRequest in;
  in.space = 4;
  in.bank = 0x0107;
  in.addr = 0x00012345;
  in.len = 0x00ABCDEF;
  kcpc::Writer w;
  kcpc::write_mem_read_request(w, in);
  const std::vector<std::uint8_t> expected = {
      0x04, 0x00, 0x07, 0x01, 0x45, 0x23, 0x01, 0x00, 0xEF, 0xCD, 0xAB, 0x00};
  EXPECT_EQ(w.data(), expected);
  kcpc::Reader r(w.data());
  kcpc::MemReadRequest out;
  ASSERT_TRUE(kcpc::read_mem_read_request(r, out));
  EXPECT_EQ(out.space, in.space);
  EXPECT_EQ(out.bank, in.bank);
  EXPECT_EQ(out.addr, in.addr);
  EXPECT_EQ(out.len, in.len);
  kcpc::Reader cut(w.data().data(), 11);
  EXPECT_FALSE(kcpc::read_mem_read_request(cut, out));
}

// addr + len must not pass the end of the space, and the check itself must
// not wrap for any pair of 32-bit values.
TEST(KcpcProto, MemRangeNeverWraps) {
  EXPECT_TRUE(kcpc::mem_range_ok(0, 0x10000, 0x10000));
  EXPECT_TRUE(kcpc::mem_range_ok(0xFFFF, 1, 0x10000));
  EXPECT_TRUE(kcpc::mem_range_ok(0x10000, 0, 0x10000)) << "empty, at the end";
  EXPECT_FALSE(kcpc::mem_range_ok(0xFFFF, 2, 0x10000));
  EXPECT_FALSE(kcpc::mem_range_ok(0x10001, 0, 0x10000));
  EXPECT_FALSE(kcpc::mem_range_ok(0, 0x10001, 0x10000));
  EXPECT_FALSE(kcpc::mem_range_ok(0xFFFFFFFF, 0xFFFFFFFF, 0x10000));
  EXPECT_FALSE(kcpc::mem_range_ok(0xFFFFFFFF, 2, 0x10000));
  EXPECT_FALSE(kcpc::mem_range_ok(1, 0xFFFFFFFF, 0x410000));
  EXPECT_TRUE(kcpc::mem_range_ok(0x400000, 0x10000, 0x410000));
}

// -- slice 3: control and writes (beads-3o1.3) ------------------------------

TEST(KcpcPayloads, ControlRequestAndReplyRoundTrip) {
  Writer w;
  kcpc::write_control_request(w,
                              {kcpc::kControlAcquire, kcpc::kControlFlagForce});
  std::vector<std::uint8_t> const expected = {1, 1, 0, 0};
  EXPECT_EQ(w.data(), expected);
  Reader r(w.data());
  kcpc::ControlRequest request;
  ASSERT_TRUE(kcpc::read_control_request(r, request));
  EXPECT_EQ(request.action, kcpc::kControlAcquire);
  EXPECT_EQ(request.flags, kcpc::kControlFlagForce);
  for (std::size_t cut = 0; cut < w.size(); ++cut) {
    Reader short_reader(w.data().data(), cut);
    kcpc::ControlRequest ignored;
    EXPECT_FALSE(kcpc::read_control_request(short_reader, ignored)) << cut;
  }

  Writer reply;
  kcpc::write_control_reply(reply, {kcpc::Role::kController, 0x01020304});
  std::vector<std::uint8_t> const reply_bytes = {1, 0, 0, 0, 4, 3, 2, 1};
  EXPECT_EQ(reply.data(), reply_bytes);
  Reader rr(reply.data());
  kcpc::ControlReply out;
  ASSERT_TRUE(kcpc::read_control_reply(rr, out));
  EXPECT_EQ(out.role, kcpc::Role::kController);
  EXPECT_EQ(out.controller, 0x01020304U);
}

TEST(KcpcPayloads, RegsSetRequestIsTheMaskThenTheBlock) {
  kcpc::RegsSetRequest in;
  in.mask = kcpc::kRegBitPc | 1U;
  in.regs.af = 0x1122;
  in.regs.pc = 0x4000;
  in.regs.im = 2;
  Writer w;
  kcpc::write_regs_set_request(w, in);
  ASSERT_EQ(w.size(), 4U + kcpc::kRegsSize);
  EXPECT_EQ(w.data()[0], 0x81);
  EXPECT_EQ(w.data()[4], 0x22) << "AF is the first field of the block";
  EXPECT_EQ(w.data()[4 + 14], 0x00) << "PC is the eighth: bit 7";
  EXPECT_EQ(w.data()[4 + 15], 0x40);

  Reader r(w.data());
  kcpc::RegsSetRequest out;
  ASSERT_TRUE(kcpc::read_regs_set_request(r, out));
  EXPECT_EQ(out.mask, in.mask);
  EXPECT_EQ(out.regs.af, 0x1122);
  EXPECT_EQ(out.regs.pc, 0x4000);
  EXPECT_EQ(out.regs.im, 2);
  for (std::size_t cut = 0; cut < w.size(); ++cut) {
    Reader short_reader(w.data().data(), cut);
    kcpc::RegsSetRequest ignored;
    EXPECT_FALSE(kcpc::read_regs_set_request(short_reader, ignored)) << cut;
  }
}

TEST(KcpcPayloads, RegsSetRefusalLooksOnlyAtSelectedFields) {
  kcpc::RegsSetRequest s;
  EXPECT_EQ(kcpc::regs_set_refusal(s), nullptr) << "an empty mask";
  s.mask = kcpc::kRegsSettableMask;
  s.regs.im = 2;
  s.regs.iff1 = 1;
  EXPECT_EQ(kcpc::regs_set_refusal(s), nullptr) << "all 17 settable fields";
  EXPECT_EQ(kcpc::kRegsSettableMask, 0x1FFFFU);

  s.mask = kcpc::kRegBitHalt;
  EXPECT_NE(kcpc::regs_set_refusal(s), nullptr) << "HALT is read-only";
  s.mask = 1U << 18;
  EXPECT_NE(kcpc::regs_set_refusal(s), nullptr) << "no such field";
  s.mask = 1U << 31;
  EXPECT_NE(kcpc::regs_set_refusal(s), nullptr);

  s.regs.im = 3;
  s.regs.iff1 = 2;
  s.regs.iff2 = 0xFF;
  s.mask = 1;  // AF only: the bad values are not selected
  EXPECT_EQ(kcpc::regs_set_refusal(s), nullptr);
  for (std::uint32_t const bit :
       {kcpc::kRegBitIm, kcpc::kRegBitIff1, kcpc::kRegBitIff2}) {
    s.mask = bit;
    EXPECT_NE(kcpc::regs_set_refusal(s), nullptr) << "bit " << bit;
  }
}

TEST(KcpcPayloads, MemWriteRequestIsEightBytesThenTheData) {
  kcpc::MemWriteRequest in;
  in.space = static_cast<std::uint8_t>(kcpc::MemSpace::kPhysRam);
  in.bank = 0x0201;
  in.addr = 0x00012345;
  Writer w;
  kcpc::write_mem_write_request(w, in);
  std::vector<std::uint8_t> const expected = {2,    0,    0x01, 0x02,
                                              0x45, 0x23, 0x01, 0x00};
  EXPECT_EQ(w.data(), expected);
  EXPECT_EQ(w.size(), kcpc::kMemWriteHeaderSize);
  Reader r(w.data());
  kcpc::MemWriteRequest out;
  ASSERT_TRUE(kcpc::read_mem_write_request(r, out));
  EXPECT_EQ(out.space, in.space);
  EXPECT_EQ(out.bank, 0x0201);
  EXPECT_EQ(out.addr, 0x00012345U);
  EXPECT_EQ(r.remaining(), 0U);
  for (std::size_t cut = 0; cut < w.size(); ++cut) {
    Reader short_reader(w.data().data(), cut);
    kcpc::MemWriteRequest ignored;
    EXPECT_FALSE(kcpc::read_mem_write_request(short_reader, ignored)) << cut;
  }
}

TEST(KcpcPayloads, OnlyTheTwoRamSpacesAreWritable) {
  for (unsigned space = 0; space < 256; ++space) {
    bool const ram = space == 1 || space == 2;
    EXPECT_EQ(kcpc::mem_space_writable(static_cast<std::uint8_t>(space)), ram)
        << "space " << space;
  }
}

TEST(KcpcPayloads, ServerCapsAnnounceTheWrites) {
  EXPECT_NE(kcpc::kServerCaps & kcpc::kCapMachineWrite, 0U);
  EXPECT_EQ(static_cast<std::uint16_t>(kcpc::Status::kIdleTimeout), 16);
}

// -- slice 4 ---------------------------------------------------------------

TEST(KcpcCodec, SubscriptionRoundTrips) {
  Writer w;
  kcpc::write_subscription(w,
                           {kcpc::kEventClassRun | kcpc::kEventClassFrame, 5});
  ASSERT_EQ(w.size(), 8U);
  Reader r(w.data());
  kcpc::Subscription s;
  ASSERT_TRUE(kcpc::read_subscription(r, s));
  EXPECT_EQ(s.classes, 3U);
  EXPECT_EQ(s.frame_interval, 5U);

  Reader cut(w.data().data(), 7);
  EXPECT_FALSE(kcpc::read_subscription(cut, s));
}

TEST(KcpcCodec, StepRequestIsTwelveBytes) {
  Writer w;
  kcpc::StepRequest step;
  step.mode = static_cast<std::uint8_t>(kcpc::StepMode::kTo);
  step.addr = 0xBB5A;
  step.count = 0x01020304;
  step.timeout_ms = 2500;
  kcpc::write_step_request(w, step);
  ASSERT_EQ(w.size(), 12U);
  EXPECT_EQ(w.data()[0], 3);
  EXPECT_EQ(w.data()[1], 0) << "reserved";
  EXPECT_EQ(w.data()[2], 0x5A);
  EXPECT_EQ(w.data()[3], 0xBB);
  Reader r(w.data());
  kcpc::StepRequest out;
  ASSERT_TRUE(kcpc::read_step_request(r, out));
  EXPECT_EQ(out.mode, 3);
  EXPECT_EQ(out.addr, 0xBB5A);
  EXPECT_EQ(out.count, 0x01020304U);
  EXPECT_EQ(out.timeout_ms, 2500U);

  Reader cut(w.data().data(), 11);
  EXPECT_FALSE(kcpc::read_step_request(cut, out));
}

TEST(KcpcCodec, RunStateRoundTrips) {
  Writer w;
  kcpc::RunState state;
  state.stamp = {123456789012ULL, true};
  state.walk_active = true;
  state.controller = 0xCAFE;
  kcpc::write_run_state(w, state);
  ASSERT_EQ(w.size(), 20U);
  Reader r(w.data());
  kcpc::RunState out;
  ASSERT_TRUE(kcpc::read_run_state(r, out));
  EXPECT_EQ(out.stamp.frame, 123456789012ULL);
  EXPECT_TRUE(out.stamp.paused);
  EXPECT_TRUE(out.walk_active);
  EXPECT_EQ(out.controller, 0xCAFEU);
}

TEST(KcpcCodec, TheServerAnnouncesRunControlAndEvents) {
  EXPECT_NE(kcpc::kServerCaps & kcpc::kCapRunControl, 0U);
  EXPECT_NE(kcpc::kServerCaps & kcpc::kCapEvents, 0U);
  EXPECT_NE(kcpc::kServerCaps & kcpc::kCapBreakpoints, 0U);
  EXPECT_EQ(kcpc::kServerCaps & kcpc::kCapInput, 0U) << "slice 6";
}

// -- slice 5: breakpoints ----------------------------------------------------

TEST(KcpcCodec, ABreakpointEntryRoundTrips) {
  kcpc::BpEntry in;
  in.kind = static_cast<std::uint8_t>(kcpc::BpKind::kIo);
  in.access = kcpc::kBpAccessWrite;
  in.addr = 0x7F00;
  in.len_or_mask = 0xFF00;
  in.pass_count = 7;
  in.hit_count = 3;
  in.condition = "value = 0x10";
  kcpc::Writer w;
  kcpc::write_bp_entry(w, in);
  // kind, access, addr, len_or_mask, 2 reserved, pass, hits, str.
  ASSERT_EQ(w.size(), 16U + 2U + in.condition.size());
  EXPECT_EQ(w.data()[2], 0x00);
  EXPECT_EQ(w.data()[3], 0x7F) << "little-endian";
  EXPECT_EQ(w.data()[6], 0) << "reserved";
  EXPECT_EQ(w.data()[7], 0) << "reserved";

  kcpc::Reader r(w.data());
  kcpc::BpEntry out;
  ASSERT_TRUE(kcpc::read_bp_entry(r, out));
  EXPECT_EQ(r.remaining(), 0U);
  EXPECT_EQ(out.kind, in.kind);
  EXPECT_EQ(out.access, in.access);
  EXPECT_EQ(out.addr, in.addr);
  EXPECT_EQ(out.len_or_mask, in.len_or_mask);
  EXPECT_EQ(out.pass_count, in.pass_count);
  EXPECT_EQ(out.hit_count, in.hit_count);
  EXPECT_EQ(out.condition, in.condition);

  // Every shorter payload is refused, wherever it was cut.
  for (std::size_t cut = 0; cut < w.size(); ++cut) {
    kcpc::Reader shorter(w.data().data(), cut);
    kcpc::BpEntry ignored;
    EXPECT_FALSE(kcpc::read_bp_entry(shorter, ignored)) << "cut at " << cut;
  }
}

TEST(KcpcCodec, ABreakpointEntryIsRefusedForItsFields) {
  kcpc::BpEntry exec;
  EXPECT_EQ(kcpc::bp_entry_refusal(exec), nullptr);
  exec.pass_count = kcpc::kMaxBpPassCount;
  EXPECT_EQ(kcpc::bp_entry_refusal(exec), nullptr);
  exec.pass_count = kcpc::kMaxBpPassCount + 1;
  EXPECT_NE(kcpc::bp_entry_refusal(exec), nullptr);
  exec.pass_count = 0;
  exec.access = kcpc::kBpAccessRead;
  EXPECT_NE(kcpc::bp_entry_refusal(exec), nullptr);
  exec.access = 0;
  exec.len_or_mask = 1;
  EXPECT_NE(kcpc::bp_entry_refusal(exec), nullptr);
  exec.len_or_mask = 0;
  exec.kind = 3;
  EXPECT_NE(kcpc::bp_entry_refusal(exec), nullptr) << "no such kind";

  kcpc::BpEntry watch;
  watch.kind = static_cast<std::uint8_t>(kcpc::BpKind::kWatch);
  watch.len_or_mask = 1;
  for (std::uint8_t access = 0; access <= 4; ++access) {
    watch.access = access;
    EXPECT_EQ(kcpc::bp_entry_refusal(watch) == nullptr,
              access >= 1 && access <= 3)
        << "access " << int{access};
  }
  watch.access = kcpc::kBpAccessBoth;
  watch.len_or_mask = 0;
  EXPECT_NE(kcpc::bp_entry_refusal(watch), nullptr) << "no bytes to watch";
  watch.addr = 0xFFFF;
  watch.len_or_mask = 1;
  EXPECT_TRUE(kcpc::bp_watch_range_ok(watch));
  watch.len_or_mask = 2;
  EXPECT_FALSE(kcpc::bp_watch_range_ok(watch)) << "nothing wraps at 64 KiB";

  kcpc::BpEntry io;
  io.kind = static_cast<std::uint8_t>(kcpc::BpKind::kIo);
  io.access = kcpc::kBpAccessRead;
  EXPECT_EQ(kcpc::bp_entry_refusal(io), nullptr) << "mask 0 matches any port";
}

TEST(KcpcCodec, ABreakpointDeleteRoundTrips) {
  kcpc::BpDelRequest in;
  in.kind = static_cast<std::uint8_t>(kcpc::BpKind::kIo);
  in.addr = 0xBC00;
  in.len_or_mask = 0xFF00;
  kcpc::Writer w;
  kcpc::write_bp_del_request(w, in);
  ASSERT_EQ(w.size(), 8U);
  kcpc::Reader r(w.data());
  kcpc::BpDelRequest out;
  ASSERT_TRUE(kcpc::read_bp_del_request(r, out));
  EXPECT_EQ(out.kind, in.kind);
  EXPECT_EQ(out.addr, 0xBC00U);
  EXPECT_EQ(out.len_or_mask, 0xFF00U);
  kcpc::Reader shorter(w.data().data(), 7);
  EXPECT_FALSE(kcpc::read_bp_del_request(shorter, out));
}

TEST(KcpcCodec, OnlyOverOutAndToWalk) {
  EXPECT_FALSE(kcpc::step_mode_walks(kcpc::StepMode::kIn));
  EXPECT_TRUE(kcpc::step_mode_walks(kcpc::StepMode::kOver));
  EXPECT_TRUE(kcpc::step_mode_walks(kcpc::StepMode::kOut));
  EXPECT_TRUE(kcpc::step_mode_walks(kcpc::StepMode::kTo));
  EXPECT_FALSE(kcpc::step_mode_walks(kcpc::StepMode::kFrames));
}
