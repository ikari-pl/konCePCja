#include "voc_import.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {

// 26-byte VOC header: magic, u16 data offset (0x14), u16 version — the
// importer keys on the magic and the offset only.
std::vector<uint8_t> voc_header() {
  std::vector<uint8_t> v(26, 0);
  std::memcpy(v.data(), "Creative Voice File\x1a", 20);
  v[0x14] = 26;
  v[0x15] = 0;
  v[0x16] = 0x0a;  // version 1.10
  v[0x17] = 0x01;
  return v;
}

void push_u24(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x & 0xff);
  v.push_back((x >> 8) & 0xff);
  v.push_back((x >> 16) & 0xff);
}

// [0x01][u24 size][rate][codec][samples...]
void push_sound_block(std::vector<uint8_t>& v, uint8_t rate, uint8_t codec,
                      const std::vector<uint8_t>& samples) {
  v.push_back(0x01);
  push_u24(v, 2 + samples.size());
  v.push_back(rate);
  v.push_back(codec);
  v.insert(v.end(), samples.begin(), samples.end());
}

// [0x03][u24 size=3][u16 period-1][rate]
void push_silence_block(std::vector<uint8_t>& v, uint16_t period_minus_1,
                        uint8_t rate) {
  v.push_back(0x03);
  push_u24(v, 3);
  v.push_back(period_minus_1 & 0xff);
  v.push_back(period_minus_1 >> 8);
  v.push_back(rate);
}

constexpr uint8_t kRate4kHz = 0x06;  // 1000000 / (256 - 6) = 4000 Hz

TEST(VocImport, SingleSoundBlockBecomesPauseThenDirectRecording) {
  // 10 samples — enough to exercise bit packing MSB-first AND a partial
  // last byte. Level = sample > 0x80: 0x81 is high, 0x80 is NOT.
  const std::vector<uint8_t> samples = {0xff, 0xff, 0xff, 0xff, 0x00,
                                        0x00, 0x81, 0x80, 0xff, 0x00};
  std::vector<uint8_t> voc = voc_header();
  push_sound_block(voc, kRate4kHz, 0, samples);
  voc.push_back(0x00);  // terminator

  const std::vector<uint8_t> tzx = voc_to_tzx(voc.data(), voc.size());
  ASSERT_EQ(tzx.size(), 24u);

  // TZX header, version 1.20.
  EXPECT_EQ(0, std::memcmp(tzx.data(), "ZXTape!\x1a", 8));
  EXPECT_EQ(tzx[8], 1);
  EXPECT_EQ(tzx[9], 20);

  // Leading pause block: 2000 ms.
  EXPECT_EQ(tzx[10], 0x20);
  EXPECT_EQ(tzx[11] | (tzx[12] << 8), 2000);

  // Direct recording block. T-states/sample = 3500000 / 4000 = 875.
  EXPECT_EQ(tzx[13], 0x15);
  EXPECT_EQ(tzx[14] | (tzx[15] << 8), 875);
  EXPECT_EQ(tzx[16] | (tzx[17] << 8), 0);  // no trailing pause
  EXPECT_EQ(tzx[18], 2);                   // 10 samples: 2 bits in last byte
  EXPECT_EQ(tzx[19], 2);                   // u24 data length = 2 bytes
  EXPECT_EQ(tzx[20], 0);
  EXPECT_EQ(tzx[21], 0);
  // Payload bits, MSB-first: 1111 0010 then 10 padded with zeros.
  EXPECT_EQ(tzx[22], 0xf2);
  EXPECT_EQ(tzx[23], 0x80);
}

TEST(VocImport, SilenceBetweenSoundBlocksBecomesAPauseBlock) {
  std::vector<uint8_t> voc = voc_header();
  push_sound_block(voc, kRate4kHz, 0, std::vector<uint8_t>(8, 0xff));
  push_silence_block(voc, 3999, kRate4kHz);  // 4000 samples @4 kHz = 1000 ms
  push_sound_block(voc, kRate4kHz, 0, std::vector<uint8_t>(8, 0x00));
  voc.push_back(0x00);

  const std::vector<uint8_t> tzx = voc_to_tzx(voc.data(), voc.size());
  ASSERT_EQ(tzx.size(), 36u);

  // header(10) + pause(3) + 0x15 with one data byte (10).
  EXPECT_EQ(tzx[13], 0x15);
  EXPECT_EQ(tzx[18], 8);  // all 8 bits of the last byte used
  EXPECT_EQ(tzx[22], 0xff);

  // The silence: a 1000 ms pause block between the two 0x15 blocks.
  EXPECT_EQ(tzx[23], 0x20);
  EXPECT_EQ(tzx[24] | (tzx[25] << 8), 1000);

  // Second sound run: its own 0x15 block.
  EXPECT_EQ(tzx[26], 0x15);
  EXPECT_EQ(tzx[35], 0x00);
}

TEST(VocImport, SoundContinueExtendsTheSameRun) {
  std::vector<uint8_t> voc = voc_header();
  push_sound_block(voc, kRate4kHz, 0, std::vector<uint8_t>(4, 0xff));
  voc.push_back(0x02);  // sound continue: 4 more samples, same rate
  push_u24(voc, 4);
  voc.insert(voc.end(), 4, 0x00);
  voc.push_back(0x00);

  const std::vector<uint8_t> tzx = voc_to_tzx(voc.data(), voc.size());
  ASSERT_EQ(tzx.size(), 23u);  // ONE 0x15 block with one packed byte
  EXPECT_EQ(tzx[13], 0x15);
  EXPECT_EQ(tzx[18], 8);
  EXPECT_EQ(tzx[22], 0xf0);  // 1111 0000
}

TEST(VocImport, MarkerAndAsciiBlocksAreSkipped) {
  std::vector<uint8_t> voc = voc_header();
  voc.push_back(0x04);  // marker
  push_u24(voc, 2);
  voc.push_back(0xaa);
  voc.push_back(0x55);
  voc.push_back(0x05);  // ASCII annotation
  push_u24(voc, 3);
  voc.insert(voc.end(), {'c', 'p', 'c'});
  push_sound_block(voc, kRate4kHz, 0, std::vector<uint8_t>(8, 0xff));
  voc.push_back(0x00);

  const std::vector<uint8_t> tzx = voc_to_tzx(voc.data(), voc.size());
  ASSERT_EQ(tzx.size(), 23u);
  EXPECT_EQ(tzx[13], 0x15);
}

TEST(VocImport, BadMagicIsRejected) {
  std::vector<uint8_t> voc = voc_header();
  voc[0] = 'X';
  push_sound_block(voc, kRate4kHz, 0, std::vector<uint8_t>(8, 0xff));
  voc.push_back(0x00);
  EXPECT_TRUE(voc_to_tzx(voc.data(), voc.size()).empty());
}

TEST(VocImport, TooShortForHeaderIsRejected) {
  const std::vector<uint8_t> voc(10, 0);
  EXPECT_TRUE(voc_to_tzx(voc.data(), voc.size()).empty());
  EXPECT_TRUE(voc_to_tzx(nullptr, 0).empty());
}

TEST(VocImport, TruncatedSoundBlockIsRejected) {
  std::vector<uint8_t> voc = voc_header();
  voc.push_back(0x01);
  push_u24(voc, 100);        // declares 100 payload bytes...
  voc.push_back(kRate4kHz);  // ...but the file ends after two
  voc.push_back(0x00);
  EXPECT_TRUE(voc_to_tzx(voc.data(), voc.size()).empty());
}

TEST(VocImport, NonPcmCodecIsRejected) {
  std::vector<uint8_t> voc = voc_header();
  push_sound_block(voc, kRate4kHz, /*codec=*/1, std::vector<uint8_t>(8, 0xff));
  voc.push_back(0x00);
  EXPECT_TRUE(voc_to_tzx(voc.data(), voc.size()).empty());
}

TEST(VocImport, MidFileSampleRateChangeIsRejected) {
  std::vector<uint8_t> voc = voc_header();
  push_sound_block(voc, kRate4kHz, 0, std::vector<uint8_t>(8, 0xff));
  push_sound_block(voc, 0x64, 0, std::vector<uint8_t>(8, 0xff));
  voc.push_back(0x00);
  EXPECT_TRUE(voc_to_tzx(voc.data(), voc.size()).empty());
}

TEST(VocImport, SoundContinueBeforeAnySoundBlockIsRejected) {
  std::vector<uint8_t> voc = voc_header();
  voc.push_back(0x02);
  push_u24(voc, 4);
  voc.insert(voc.end(), 4, 0xff);
  voc.push_back(0x00);
  EXPECT_TRUE(voc_to_tzx(voc.data(), voc.size()).empty());
}

TEST(VocImport, UnknownBlockTypeIsRejected) {
  std::vector<uint8_t> voc = voc_header();
  voc.push_back(0x06);  // repeat — unsupported
  push_u24(voc, 2);
  voc.push_back(0x00);
  voc.push_back(0x00);
  EXPECT_TRUE(voc_to_tzx(voc.data(), voc.size()).empty());
}

}  // namespace
