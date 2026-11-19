#include "util/wav.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <iostream>

#include "check.h"

using namespace st;

namespace {

uint32_t le32(const std::string& s, size_t off) {
  uint32_t v = 0;
  std::memcpy(&v, s.data() + off, 4);
  return v;
}
uint16_t le16(const std::string& s, size_t off) {
  uint16_t v = 0;
  std::memcpy(&v, s.data() + off, 2);
  return v;
}

void test_endless_header_layout() {
  const std::string h = endless_wav_header(96000);
  CHECK_EQ(h.size(), 44u);

  CHECK_EQ(h.substr(0, 4), std::string("RIFF"));
  CHECK_EQ(h.substr(8, 4), std::string("WAVE"));
  CHECK_EQ(h.substr(12, 4), std::string("fmt "));
  CHECK_EQ(h.substr(36, 4), std::string("data"));

  // Both sizes say "unknown", which FFmpeg (Chrome) and VLC read as "keep going until the
  // connection ends".
  CHECK_EQ(le32(h, 4), 0xFFFFFFFFu);
  CHECK_EQ(le32(h, 40), 0xFFFFFFFFu);

  CHECK_EQ(le32(h, 16), 16u);  // PCM fmt chunk size
  CHECK_EQ(le16(h, 20), 1u);   // PCM
  CHECK_EQ(le16(h, 22), 1u);   // mono
  CHECK_EQ(le32(h, 24), 96000u);
  CHECK_EQ(le32(h, 28), 96000u * 2u);  // byte rate
  CHECK_EQ(le16(h, 32), 2u);           // block align
  CHECK_EQ(le16(h, 34), 16u);          // bits
}

void test_rate_is_honoured() {
  const std::string h = endless_wav_header(48000);
  CHECK_EQ(le32(h, 24), 48000u);
  CHECK_EQ(le32(h, 28), 96000u);
}

// Walks the RIFF chunks after "WAVE": tag -> {payload offset, size}. A malformed size ends it.
struct Chunk {
  std::string tag;
  size_t off;
  uint32_t size;
};
std::vector<Chunk> chunks(const std::string& h) {
  std::vector<Chunk> out;
  size_t p = 12;
  while (p + 8 <= h.size()) {
    const uint32_t sz = le32(h, p + 4);
    out.push_back({h.substr(p, 4), p + 8, sz});
    if (h.substr(p, 4) == "data") break;  // its payload follows the header
    p += 8 + sz + (sz & 1);
  }
  return out;
}

void test_export_header_f32() {
  const std::string h = wav_header(96000, 3, WavFormat::F32, 1000, "start_sample=42");
  CHECK_EQ(h.substr(0, 4), std::string("RIFF"));
  CHECK_EQ(h.substr(8, 4), std::string("WAVE"));
  const std::vector<Chunk> c = chunks(h);
  CHECK_EQ(c.size(), 4u);
  if (c.size() != 4) return;
  CHECK_EQ(c[0].tag, std::string("fmt "));
  CHECK_EQ(c[0].size, 18u);                // non-PCM: cbSize present
  CHECK_EQ(le16(h, c[0].off), 3u);         // IEEE float
  CHECK_EQ(le16(h, c[0].off + 2), 3u);     // channels
  CHECK_EQ(le32(h, c[0].off + 4), 96000u);
  CHECK_EQ(le32(h, c[0].off + 8), 96000u * 12u);  // byte rate
  CHECK_EQ(le16(h, c[0].off + 12), 12u);          // block align
  CHECK_EQ(le16(h, c[0].off + 14), 32u);
  CHECK_EQ(le16(h, c[0].off + 16), 0u);           // cbSize
  CHECK_EQ(c[1].tag, std::string("fact"));
  CHECK_EQ(le32(h, c[1].off), 1000u);             // frames
  CHECK_EQ(c[2].tag, std::string("LIST"));
  CHECK_EQ(h.substr(c[2].off, 8), std::string("INFOICMT"));
  CHECK_EQ(c[2].size % 2, 0u);
  CHECK(h.find("start_sample=42") != std::string::npos);
  CHECK_EQ(c[3].tag, std::string("data"));
  CHECK_EQ(c[3].size, 1000u * 12u);
  CHECK_EQ(c[3].off, h.size());                   // the header ends where the samples start
  CHECK_EQ(le32(h, 4), static_cast<uint32_t>(h.size() - 8 + 12000));  // RIFF size = whole file - 8
}

void test_export_header_pcm() {
  for (WavFormat f : {WavFormat::S16, WavFormat::S32}) {
    const unsigned bps = f == WavFormat::S16 ? 2 : 4;
    const std::string h = wav_header(48000, 2, f, 7);
    CHECK_EQ(h.size(), 44u);  // canonical: no fact, no LIST without a comment
    CHECK_EQ(le32(h, 16), 16u);
    CHECK_EQ(le16(h, 20), 1u);  // PCM
    CHECK_EQ(le16(h, 22), 2u);
    CHECK_EQ(le32(h, 28), 48000u * 2 * bps);
    CHECK_EQ(le16(h, 32), 2 * bps);
    CHECK_EQ(le16(h, 34), 8 * bps);
    CHECK_EQ(le32(h, 40), 7u * 2 * bps);
    CHECK_EQ(le32(h, 4), 36u + 7u * 2 * bps);
  }
  // An odd-length comment is padded so the data chunk stays word-aligned.
  const std::string h = wav_header(48000, 1, WavFormat::S16, 1, "ab");
  const std::vector<Chunk> c = chunks(h);
  CHECK_EQ(c.back().tag, std::string("data"));
  CHECK_EQ(h.size() % 2, 0u);
}

void test_format_names() {
  WavFormat f;
  CHECK(parse_wav_format("f32", &f) && f == WavFormat::F32);
  CHECK(parse_wav_format("s32", &f) && f == WavFormat::S32);
  CHECK(parse_wav_format("s16", &f) && f == WavFormat::S16);
  CHECK(!parse_wav_format("S16", &f));
  CHECK(!parse_wav_format("", &f));
  CHECK_EQ(std::string(wav_format_name(WavFormat::S32)), std::string("s32"));
  CHECK_EQ(wav_sample_bytes(WavFormat::S16), 2u);
  CHECK_EQ(wav_sample_bytes(WavFormat::F32), 4u);
}

void test_encode_picks_columns_in_order() {
  // Two frames of a 4-channel ring; export columns 3 then 1.
  const float src[8] = {0.f, 0.25f, 0.5f, -0.5f, 1.f, -1.f, 2.f, 0.125f};
  const std::vector<unsigned> cols{3, 1};

  std::string f32(16, '\0');
  wav_encode(src, 2, 4, cols, WavFormat::F32, f32.data());
  float got[4];
  std::memcpy(got, f32.data(), 16);
  CHECK_EQ(got[0], -0.5f);
  CHECK_EQ(got[1], 0.25f);
  CHECK_EQ(got[2], 0.125f);
  CHECK_EQ(got[3], -1.f);

  std::string s16(8, '\0');
  wav_encode(src, 2, 4, cols, WavFormat::S16, s16.data());
  int16_t g16[4];
  std::memcpy(g16, s16.data(), 8);
  CHECK_EQ(g16[0], -16384);
  CHECK_EQ(g16[1], 8192);
  CHECK_EQ(g16[2], 4096);
  CHECK_EQ(g16[3], -32768);

  std::string s32(16, '\0');
  wav_encode(src, 2, 4, cols, WavFormat::S32, s32.data());
  int32_t g32[4];
  std::memcpy(g32, s32.data(), 16);
  CHECK_EQ(g32[0], -1073741824);
  CHECK_EQ(g32[1], 536870912);
  CHECK_EQ(g32[3], INT32_MIN);
}

void test_encode_clips_and_zeroes_nan() {
  const float src[4] = {1.0f, 1.5f, -3.0f, std::nanf("")};
  const std::vector<unsigned> cols{0};
  std::string s16(8, '\0');
  wav_encode(src, 4, 1, cols, WavFormat::S16, s16.data());
  int16_t g[4];
  std::memcpy(g, s16.data(), 8);
  CHECK_EQ(g[0], 32767);   // +1.0 is one step past the largest code
  CHECK_EQ(g[1], 32767);
  CHECK_EQ(g[2], -32768);
  CHECK_EQ(g[3], 0);
  std::string s32(16, '\0');
  wav_encode(src, 4, 1, cols, WavFormat::S32, s32.data());
  int32_t h[4];
  std::memcpy(h, s32.data(), 16);
  CHECK_EQ(h[0], INT32_MAX);
  CHECK_EQ(h[2], INT32_MIN);
  CHECK_EQ(h[3], 0);
  // The Octo's codes come back exactly: x = code / 2^31.
  const float code = static_cast<float>(-123456 * 256) / 2147483648.0f;
  int32_t back = 0;
  std::string one(4, '\0');
  wav_encode(&code, 1, 1, cols, WavFormat::S32, one.data());
  std::memcpy(&back, one.data(), 4);
  CHECK_EQ(back, -123456 * 256);
}

}  // namespace

int main() {
  test_endless_header_layout();
  test_rate_is_honoured();
  test_export_header_f32();
  test_export_header_pcm();
  test_format_names();
  test_encode_picks_columns_in_order();
  test_encode_clips_and_zeroes_nan();
  return report("wav");
}
