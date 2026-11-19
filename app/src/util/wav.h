#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace st {

// A 44-byte canonical WAV header (16-bit mono PCM, the only shape the daemon streams) whose
// RIFF and data sizes are 0xFFFFFFFF.
//
// FFmpeg's wav demuxer (so Chrome) special-cases 0xFFFFFFFF as "unknown length" and reads
// until the connection ends; VLC ignores a declared size larger than the stream. Players
// that do trust the size stop after 4 GiB, which at 96 kHz 16-bit mono is ~6.2 hours.
inline std::string endless_wav_header(unsigned rate) {
  const uint32_t kUnknown = 0xFFFFFFFFu;
  const uint16_t block_align = 2;  // 1 channel x 16 bits

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
  put16(1);  // mono
  put32(rate);
  put32(rate * block_align);
  put16(block_align);
  put16(16);  // bits per sample
  tag("data");
  put32(kUnknown);
  return h;
}

// ---- Multichannel export (GET /api/capture/wav) ------------------------------------------------

// f32: IEEE float, the ring's own values. s32/s16: integer PCM, full scale = 1.0.
enum class WavFormat { F32, S32, S16 };

inline bool parse_wav_format(const std::string& s, WavFormat* out) {
  if (s == "f32") *out = WavFormat::F32;
  else if (s == "s32") *out = WavFormat::S32;
  else if (s == "s16") *out = WavFormat::S16;
  else return false;
  return true;
}

inline const char* wav_format_name(WavFormat f) {
  return f == WavFormat::F32 ? "f32" : f == WavFormat::S32 ? "s32" : "s16";
}

inline unsigned wav_sample_bytes(WavFormat f) { return f == WavFormat::S16 ? 2 : 4; }

// The header of a WAV of known length, up to and including the data chunk's size: what goes in
// front of frames x channels samples of `f`. fmt is plain PCM (tag 1) for s16/s32 and IEEE float
// (tag 3, with the cbSize and `fact` chunk the spec asks of a non-PCM format) for f32 — no
// WAVE_FORMAT_EXTENSIBLE, which every reader that matters (libsndfile, scipy, sox, ffmpeg) accepts
// at any channel count. A non-empty `comment` goes in a LIST/INFO ICMT chunk before the data, so a
// saved file keeps where its samples came from. The caller keeps the whole file under 4 GiB.
inline std::string wav_header(unsigned rate, unsigned channels, WavFormat f, uint64_t frames,
                              const std::string& comment = {}) {
  const bool flt = f == WavFormat::F32;
  const uint32_t bps = wav_sample_bytes(f);
  const uint32_t align = channels * bps;
  const uint32_t data = static_cast<uint32_t>(frames * align);

  std::string icmt;  // the ICMT payload: NUL-terminated, padded to an even size
  if (!comment.empty()) {
    icmt = comment;
    icmt.push_back('\0');
    if (icmt.size() % 2) icmt.push_back('\0');
  }

  std::string h;
  auto put32 = [&h](uint32_t v) { h.append(reinterpret_cast<const char*>(&v), 4); };
  auto put16 = [&h](uint16_t v) { h.append(reinterpret_cast<const char*>(&v), 2); };
  h += "RIFF";
  put32(0);  // patched below
  h += "WAVE";
  h += "fmt ";
  put32(flt ? 18 : 16);
  put16(flt ? 3 : 1);
  put16(static_cast<uint16_t>(channels));
  put32(rate);
  put32(rate * align);
  put16(static_cast<uint16_t>(align));
  put16(static_cast<uint16_t>(bps * 8));
  if (flt) {
    put16(0);  // cbSize
    h += "fact";
    put32(4);
    put32(static_cast<uint32_t>(frames));
  }
  if (!icmt.empty()) {
    h += "LIST";
    put32(static_cast<uint32_t>(4 + 8 + icmt.size()));
    h += "INFO";
    h += "ICMT";
    put32(static_cast<uint32_t>(icmt.size()));
    h += icmt;
  }
  h += "data";
  put32(data);
  const uint32_t riff = static_cast<uint32_t>(h.size() - 8 + data);
  std::memcpy(h.data() + 4, &riff, 4);
  return h;
}

// Encodes `frames` frames of interleaved floats (`stride` channels per frame) into `out` as
// little-endian samples of `f`, taking the columns `cols` in that order: the export's own
// interleave. Integers round to nearest and clip at full scale (the ring can carry a network
// sender's converter overshoot past 1.0); NaN encodes as 0. f32 is copied bit for bit.
inline void wav_encode(const float* src, size_t frames, unsigned stride,
                       const std::vector<unsigned>& cols, WavFormat f, char* out) {
  for (size_t i = 0; i < frames; ++i) {
    const float* fr = src + i * stride;
    for (unsigned c : cols) {
      const float x = fr[c];
      if (f == WavFormat::F32) {
        std::memcpy(out, &x, 4);
        out += 4;
        continue;
      }
      const double full = f == WavFormat::S16 ? 32768.0 : 2147483648.0;
      double v = std::isnan(x) ? 0.0 : std::nearbyint(static_cast<double>(x) * full);
      if (v > full - 1) v = full - 1;
      if (v < -full) v = -full;
      if (f == WavFormat::S16) {
        const int16_t s = static_cast<int16_t>(v);
        std::memcpy(out, &s, 2);
        out += 2;
      } else {
        const int32_t s = static_cast<int32_t>(v);
        std::memcpy(out, &s, 4);
        out += 4;
      }
    }
  }
}

}  // namespace st
