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
