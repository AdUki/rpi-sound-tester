#include "util/wav.h"

#include <cstring>
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

}  // namespace

int main() {
  test_endless_header_layout();
  test_rate_is_honoured();
  return report("wav");
}
