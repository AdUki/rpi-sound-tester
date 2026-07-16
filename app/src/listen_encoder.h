#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "constants.h"

namespace st {

// Encodes one mono input channel for the browser listen path: a chunk of engine-rate float PCM
// (in_frames()) is decimated to 48 kHz and turned into a single 20 ms Opus packet. Raw packets,
// no container — the browser frames them itself as [u64 sample index][packet] and decodes with a
// stateful WASM decoder. One instance per WebSocket connection (opus_encode is not reentrant).
class OpusMonoEncoder {
 public:
  // `rate` is the engine sample rate; *ok is false if the rate is unsupported or Opus init fails.
  OpusMonoEncoder(unsigned rate, int bitrate_kbps, bool* ok);
  ~OpusMonoEncoder();
  OpusMonoEncoder(const OpusMonoEncoder&) = delete;
  OpusMonoEncoder& operator=(const OpusMonoEncoder&) = delete;

  unsigned in_frames() const { return in_frames_; }  // ring frames consumed per encode()

  // Encode in_frames() float samples into `packet`. Applies bitrate_kbps if it changed. False on
  // encoder error.
  bool encode(const float* pcm, int bitrate_kbps, std::vector<uint8_t>* packet);
  void reset();  // OPUS_RESET_STATE + decimator flush, on a pacer skip-to-live discontinuity

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
  unsigned in_frames_ = 0;  // set from the engine rate before *ok can become true
};

// Encodes all kInputs channels together as one Ogg/Opus logical stream (mapping family 255,
// uncoupled), so every channel shares a single granulepos clock and stays sample-aligned — the
// only shape a generic external player (ffmpeg) can consume without cross-channel drift. Emits
// Ogg pages: headers() once at stream start, then audio pages from each encode().
class OpusOggMultiEncoder {
 public:
  OpusOggMultiEncoder(unsigned rate, int bitrate_kbps, uint32_t serial, bool* ok);
  ~OpusOggMultiEncoder();
  OpusOggMultiEncoder(const OpusOggMultiEncoder&) = delete;
  OpusOggMultiEncoder& operator=(const OpusOggMultiEncoder&) = delete;

  unsigned in_frames() const { return in_frames_; }  // ring frames (per channel) per encode()

  // The OpusHead + OpusTags pages, to write once before any audio.
  std::string headers();

  // Encode in_frames()*kInputs interleaved float frames, appending any Ogg pages produced.
  bool encode(const float* interleaved, int bitrate_kbps, std::string* out);
  void reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
  unsigned in_frames_ = 0;  // set from the engine rate before *ok can become true
};

}  // namespace st
