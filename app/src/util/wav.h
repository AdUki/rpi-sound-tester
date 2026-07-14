#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace st {

// A 44-byte canonical WAV header whose RIFF and data sizes are 0xFFFFFFFF.
//
// FFmpeg's wav demuxer (so Chrome) special-cases 0xFFFFFFFF as "unknown length" and reads
// until the connection ends; VLC ignores a declared size larger than the stream. Players
// that do trust the size stop after 4 GiB, which at 96 kHz 16-bit mono is ~6.2 hours.
inline std::string endless_wav_header(unsigned rate, unsigned channels = 1,
                                      unsigned bits = 16) {
  const uint32_t kUnknown = 0xFFFFFFFFu;
  const uint16_t block_align = static_cast<uint16_t>(channels * bits / 8);
  const uint32_t byte_rate = rate * block_align;

  std::string h(44, '\0');
  char* p = h.data();
  auto put32 = [&p](uint32_t v) { std::memcpy(p, &v, 4); p += 4; };
  auto put16 = [&p](uint16_t v) { std::memcpy(p, &v, 2); p += 2; };
  auto tag = [&p](const char* s) { std::memcpy(p, s, 4); p += 4; };

  tag("RIFF");
  put32(kUnknown);
  tag("WAVE");
  tag("fmt ");
  put32(16);
  put16(1);  // PCM
  put16(static_cast<uint16_t>(channels));
  put32(rate);
  put32(byte_rate);
  put16(block_align);
  put16(static_cast<uint16_t>(bits));
  tag("data");
  put32(kUnknown);
  return h;
}

}  // namespace st
