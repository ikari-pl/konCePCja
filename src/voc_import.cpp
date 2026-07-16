// voc_import.cpp — clean-room Creative Voice File (.voc) → TZX converter.
// See voc_import.h. Layout per the Creative Voice File spec:
//   header: "Creative Voice File\x1a" + u16 data offset at 0x14 (+ version
//           and checksum words, which carry no information we need);
//   blocks: u8 type, u24 size, then `size` payload bytes — except type 0x00
//           (terminator), which is the bare type byte.
//   0x01 sound data:     [rate byte][codec][samples...] (codec 0 = 8-bit
//                        unsigned PCM; anything else is rejected)
//   0x02 sound continue: [samples...] at the rate of the preceding block
//   0x03 silence:        [u16 period-1][rate byte]
//   0x04 marker / 0x05 ASCII annotation: metadata, skipped
// Sample rate: hz = 1000000 / (256 - rate byte). One rate per tape — the
// 0x15 Direct Recording block has a single T-states-per-sample field, so a
// mid-file rate change has no faithful encoding and is rejected.

#include "voc_import.h"

#include <algorithm>
#include <cstring>

#include "log.h"

namespace {

constexpr uint8_t kLevelThreshold = 0x80;  // sample > threshold → line high
constexpr uint32_t kZ80Hz = 3500000;       // TZX timings are Spectrum T-states
constexpr uint32_t kLeaderPauseMs = 2000;
constexpr size_t kDirectDataMax = 0x00ffffff;  // u24 length field

uint32_t rd_u16(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
}

uint32_t rd_u24(const uint8_t* p) {
  return rd_u16(p) | (static_cast<uint32_t>(p[2]) << 16);
}

void put_u16(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x & 0xff);
  v.push_back((x >> 8) & 0xff);
}

void put_pause(std::vector<uint8_t>& v, uint32_t ms) {
  v.push_back(0x20);
  put_u16(v, ms);
}

// Accumulates one run of samples, packed MSB-first, and flushes it as a TZX
// 0x15 Direct Recording block.
struct DirectRun {
  std::vector<uint8_t> packed;
  uint32_t nsamples = 0;
  uint8_t cur = 0;
  int nbits = 0;

  void add(uint8_t sample) {
    cur = (cur << 1) | (sample > kLevelThreshold ? 1 : 0);
    if (++nbits == 8) {
      packed.push_back(cur);
      cur = 0;
      nbits = 0;
    }
    ++nsamples;
  }

  // Emit the pending run (if any) and reset. False = run too long for the
  // block's 24-bit length field.
  bool flush(std::vector<uint8_t>& out, uint32_t tstates_per_sample) {
    if (nsamples == 0) return true;
    if (nbits) packed.push_back(cur << (8 - nbits));  // pad toward the LSBs
    if (packed.size() > kDirectDataMax) {
      LOG_ERROR(
          "VOC import: sound run too long for one direct recording "
          "block ("
          << packed.size() << " bytes packed)");
      return false;
    }
    out.push_back(0x15);
    put_u16(out, tstates_per_sample);
    put_u16(out, 0);  // no pause after the block — silence blocks carry it
    out.push_back(nsamples % 8 ? nsamples % 8 : 8);  // used bits, last byte
    out.push_back(packed.size() & 0xff);
    out.push_back((packed.size() >> 8) & 0xff);
    out.push_back((packed.size() >> 16) & 0xff);
    out.insert(out.end(), packed.begin(), packed.end());
    packed.clear();
    nsamples = 0;
    cur = 0;
    nbits = 0;
    return true;
  }
};

}  // namespace

std::vector<uint8_t> voc_to_tzx(const uint8_t* data, size_t len) {
  static const char kMagic[] = "Creative Voice File\x1a";  // 20 bytes
  if (data == nullptr || len < 26 || memcmp(data, kMagic, 20) != 0) {
    LOG_ERROR("VOC import: not a Creative Voice File (bad header)");
    return {};
  }
  size_t off = rd_u16(data + 0x14);
  if (off < 26 || off > len) {
    LOG_ERROR("VOC import: header data offset " << off << " outside the file ("
                                                << len << " bytes)");
    return {};
  }

  std::vector<uint8_t> out = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1a, 1, 20};
  put_pause(out, kLeaderPauseMs);  // let the firmware settle before data

  bool has_rate = false;
  uint8_t rate = 0;
  uint32_t sample_hz = 0;
  uint32_t tstates = 0;
  DirectRun run;

  // One rate per tape (see file comment). First rate byte seen wins.
  auto set_rate = [&](uint8_t r) -> bool {
    if (has_rate) {
      if (r != rate) {
        LOG_ERROR("VOC import: unsupported mid-file sample rate change ("
                  << static_cast<int>(rate) << " -> " << static_cast<int>(r)
                  << ")");
        return false;
      }
      return true;
    }
    rate = r;
    sample_hz = 1000000u / (256u - r);  // 256-r >= 1: no division by zero
    tstates = kZ80Hz / sample_hz;
    has_rate = true;
    return true;
  };

  while (off < len) {
    const uint8_t type = data[off++];
    if (type == 0x00) break;  // terminator: bare type byte, no size field
    if (len - off < 3) {
      LOG_ERROR("VOC import: truncated block header at offset " << off - 1);
      return {};
    }
    const uint32_t size = rd_u24(data + off);
    off += 3;
    if (size > len - off) {
      LOG_ERROR("VOC import: block 0x" << static_cast<int>(type)
                                       << " extends past end of file");
      return {};
    }
    const uint8_t* body = data + off;

    switch (type) {
      case 0x01: {  // sound data
        if (size < 2) {
          LOG_ERROR(
              "VOC import: sound data block too short for its "
              "rate/codec bytes");
          return {};
        }
        if (body[1] != 0) {
          LOG_ERROR("VOC import: unsupported codec "
                    << static_cast<int>(body[1])
                    << " (8-bit unsigned PCM only)");
          return {};
        }
        if (!set_rate(body[0])) return {};
        for (uint32_t i = 2; i < size; ++i) run.add(body[i]);
        break;
      }
      case 0x02: {  // sound continue
        if (!has_rate) {
          LOG_ERROR("VOC import: sound continue block before any sound block");
          return {};
        }
        for (uint32_t i = 0; i < size; ++i) run.add(body[i]);
        break;
      }
      case 0x03: {  // silence
        if (size < 3) {
          LOG_ERROR("VOC import: silence block too short");
          return {};
        }
        if (!set_rate(body[2])) return {};
        if (!run.flush(out, tstates)) return {};
        const uint32_t samples = rd_u16(body) + 1;  // stored as period-1
        uint64_t ms = static_cast<uint64_t>(samples) * 1000u / sample_hz;
        ms = std::min<uint64_t>(ms, 0xffff);
        if (ms == 0) ms = 1;  // a 0 ms TZX pause means "stop the tape"
        put_pause(out, static_cast<uint32_t>(ms));
        break;
      }
      case 0x04:  // marker
      case 0x05:  // ASCII annotation
        break;    // metadata — nothing on the wire
      default:
        LOG_ERROR("VOC import: unsupported block type "
                  << static_cast<int>(type));
        return {};
    }
    off += size;
  }

  if (!run.flush(out, tstates)) return {};
  return out;
}
