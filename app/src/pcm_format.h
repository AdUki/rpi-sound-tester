#pragma once

#include <alsa/asoundlib.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "util/dsp.h"

namespace st {

// The sample formats a device other than the engine card is opened in, in the order they are
// tried: the widest the device takes, since this is a measuring instrument. The Pi's HDMI and jack
// take S16_LE alone.
enum class PcmFormat : uint8_t { S16_LE, S32_LE, S24_3LE, S24_LE };
inline constexpr PcmFormat kPcmFormats[] = {PcmFormat::S32_LE, PcmFormat::S24_LE,
                                            PcmFormat::S24_3LE, PcmFormat::S16_LE};

inline const char* pcm_format_name(PcmFormat f) {
  switch (f) {
    case PcmFormat::S32_LE: return "S32_LE";
    case PcmFormat::S24_3LE: return "S24_3LE";
    case PcmFormat::S24_LE: return "S24_LE";
    default: return "S16_LE";
  }
}

inline constexpr unsigned pcm_format_bytes(PcmFormat f) {
  return f == PcmFormat::S16_LE ? 2 : f == PcmFormat::S24_3LE ? 3 : 4;
}

inline snd_pcm_format_t pcm_alsa_format(PcmFormat f) {
  switch (f) {
    case PcmFormat::S32_LE: return SND_PCM_FORMAT_S32_LE;
    case PcmFormat::S24_3LE: return SND_PCM_FORMAT_S24_3LE;
    case PcmFormat::S24_LE: return SND_PCM_FORMAT_S24_LE;
    default: return SND_PCM_FORMAT_S16_LE;
  }
}

namespace detail {

template <PcmFormat F>
inline void put_sample(float v, uint8_t* p) {
  if constexpr (F == PcmFormat::S16_LE) {
    const int16_t x = float_to_s16(v);
    std::memcpy(p, &x, 2);
  } else if constexpr (F == PcmFormat::S32_LE) {
    const int32_t x = float_to_s32(v);
    std::memcpy(p, &x, 4);
  } else {
    const int32_t x = float_to_s32(v) >> 8;  // the top 24 bits, sign-extended
    p[0] = static_cast<uint8_t>(x);
    p[1] = static_cast<uint8_t>(x >> 8);
    p[2] = static_cast<uint8_t>(x >> 16);
    if constexpr (F == PcmFormat::S24_LE) p[3] = static_cast<uint8_t>(x >> 24);
  }
}

template <PcmFormat F>
inline float get_sample(const uint8_t* p) {
  if constexpr (F == PcmFormat::S16_LE) {
    int16_t x;
    std::memcpy(&x, p, 2);
    return static_cast<float>(x) * (1.0f / 32768.0f);
  } else if constexpr (F == PcmFormat::S32_LE) {
    int32_t x;
    std::memcpy(&x, p, 4);
    return s32_to_float(x);
  } else {
    // S24_LE is 24 bits in the low three bytes of four; the top byte is padding, whatever it holds.
    const uint32_t u = static_cast<uint32_t>(p[0]) << 8 | static_cast<uint32_t>(p[1]) << 16 |
                       static_cast<uint32_t>(p[2]) << 24;
    return s32_to_float(static_cast<int32_t>(u));
  }
}

template <PcmFormat F>
void from_float_as(const float* in, size_t frames, unsigned channels, unsigned pcm_channels,
                   uint8_t* out) {
  constexpr unsigned b = pcm_format_bytes(F);
  if (channels == 1 && pcm_channels == 2) {
    for (size_t i = 0; i < frames; ++i) {
      put_sample<F>(in[i], out + (2 * i) * b);
      put_sample<F>(in[i], out + (2 * i + 1) * b);
    }
    return;
  }
  for (size_t i = 0; i < frames * channels; ++i) put_sample<F>(in[i], out + i * b);
}

template <PcmFormat F>
void to_float_as(const uint8_t* in, size_t samples, float* out) {
  constexpr unsigned b = pcm_format_bytes(F);
  for (size_t i = 0; i < samples; ++i) out[i] = get_sample<F>(in + i * b);
}

}  // namespace detail

// Converts `channels`-wide float frames to `pcm_channels`-wide PCM frames in format `f`. One
// channel into a two-channel PCM (mono) is written to both L and R; otherwise the two are the same
// and the channels pass straight through. The format is fixed while a PCM is open, so the branch is
// once per chunk and each format's loop is a plain one.
inline void pcm_from_float(const float* in, size_t frames, unsigned channels,
                           unsigned pcm_channels, PcmFormat f, uint8_t* out) {
  switch (f) {
    case PcmFormat::S32_LE:
      return detail::from_float_as<PcmFormat::S32_LE>(in, frames, channels, pcm_channels, out);
    case PcmFormat::S24_3LE:
      return detail::from_float_as<PcmFormat::S24_3LE>(in, frames, channels, pcm_channels, out);
    case PcmFormat::S24_LE:
      return detail::from_float_as<PcmFormat::S24_LE>(in, frames, channels, pcm_channels, out);
    default:
      return detail::from_float_as<PcmFormat::S16_LE>(in, frames, channels, pcm_channels, out);
  }
}

// `samples` PCM samples in format `f` (interleaved frames, any width) to floats.
inline void pcm_to_float(const uint8_t* in, size_t samples, PcmFormat f, float* out) {
  switch (f) {
    case PcmFormat::S32_LE: return detail::to_float_as<PcmFormat::S32_LE>(in, samples, out);
    case PcmFormat::S24_3LE: return detail::to_float_as<PcmFormat::S24_3LE>(in, samples, out);
    case PcmFormat::S24_LE: return detail::to_float_as<PcmFormat::S24_LE>(in, samples, out);
    default: return detail::to_float_as<PcmFormat::S16_LE>(in, samples, out);
  }
}

}  // namespace st
