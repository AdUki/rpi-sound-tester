#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "constants.h"

namespace st {

enum class SourceType : uint8_t { Silence = 0, Input = 1, Gen = 2 };
enum class GenId : uint8_t { Sine = 0, Noise = 1, Ping = 2, Count = 3 };
enum class NoiseMode : uint8_t { White = 0, Pink = 1 };
enum class PingVariant : uint8_t { Tick = 0, Bing = 1, Bong = 2 };

// An output's source is a compound {type, index}. Kept in ONE atomic: two separate atomics
// could be observed torn (type already Gen while index is still 5 from an Input source),
// which would index out of bounds in the audio thread.
inline constexpr uint32_t pack_source(SourceType t, uint8_t index) {
  return (static_cast<uint32_t>(t) << 8) | index;
}
inline constexpr SourceType source_type(uint32_t packed) {
  return static_cast<SourceType>((packed >> 8) & 0xff);
}
inline constexpr uint8_t source_index(uint32_t packed) { return packed & 0xff; }

struct InputControl {
  std::atomic<float> gain_db{0.0f};
};

struct OutputControl {
  std::atomic<uint32_t> source{pack_source(SourceType::Silence, 0)};
  std::atomic<float> gain_db{0.0f};
  std::atomic<bool> mute{false};
  std::atomic<uint64_t> identify_until{0};  // absolute sample index
};

struct SineControl {
  std::atomic<float> freq_hz{440.0f};
  std::atomic<float> level_db{-20.0f};
};

struct NoiseControl {
  std::atomic<uint8_t> mode{static_cast<uint8_t>(NoiseMode::White)};
  std::atomic<float> level_db{-20.0f};
};

struct PingControl {
  std::atomic<uint8_t> variant{static_cast<uint8_t>(PingVariant::Tick)};
  std::atomic<float> interval_s{2.0f};
  std::atomic<float> level_db{-20.0f};
  // Bumped on every change so the generator knows to reschedule its next emission.
  std::atomic<uint32_t> epoch{0};
};

// Default listen codec and per-channel Opus bitrate. Not read by the audio thread — these live
// here (rather than on the web server) only so they ride the Config<->Control save/restore path
// like every other persisted live setting. The wire default stays PCM regardless: `codec` is the
// frontend's preference (it opts in with ?codec=opus explicitly) and the stream.ogg default.
struct ListenControl {
  std::atomic<uint8_t> codec{static_cast<uint8_t>(ListenCodec::Pcm)};
  std::atomic<int> bitrate_kbps{kListenBitrateDefaultKbps};
};

struct PingEvent {
  uint64_t sample;
  uint8_t variant;
};

// Lock-free log of recent ping emissions. The audio thread must never take a lock, so each
// entry packs sample and variant into a single atomic word (sample stays well under 2^56:
// 2^56 frames at 96 kHz is ~23000 years).
class PingLog {
 public:
  void push(uint64_t sample, uint8_t variant) {
    const uint64_t c = count_.load(std::memory_order_relaxed);
    entries_[c % kPingLogEntries].store(
        (static_cast<uint64_t>(variant) << 56) | (sample & kSampleMask), std::memory_order_release);
    count_.store(c + 1, std::memory_order_release);
  }

  std::vector<PingEvent> recent() const {
    const uint64_t c = count_.load(std::memory_order_acquire);
    const uint64_t have = c < kPingLogEntries ? c : kPingLogEntries;
    std::vector<PingEvent> out;
    out.reserve(have);
    for (uint64_t i = c - have; i < c; ++i) {
      const uint64_t w = entries_[i % kPingLogEntries].load(std::memory_order_acquire);
      out.push_back({w & kSampleMask, static_cast<uint8_t>(w >> 56)});
    }
    return out;
  }

 private:
  static constexpr uint64_t kSampleMask = (1ull << 56) - 1;
  std::array<std::atomic<uint64_t>, kPingLogEntries> entries_{};
  std::atomic<uint64_t> count_{0};
};

// Written by web handlers, read by the audio thread at the top of each block. Scalars are
// independent atomics; tearing across a block boundary there is benign.
struct Control {
  std::array<InputControl, kInputs> inputs;
  std::array<OutputControl, kOutputs> outputs;
  SineControl sine;
  NoiseControl noise;
  PingControl ping;
  ListenControl listen;

  // input_map[logical] = physical TDM slot to capture from.
  // output_map[logical] = physical TDM slot to play into.
  std::array<std::atomic<uint8_t>, kInputs> input_map;
  std::array<std::atomic<uint8_t>, kOutputs> output_map;

  PingLog ping_log;

  Control() {
    for (unsigned i = 0; i < kInputs; ++i) input_map[i].store(static_cast<uint8_t>(i));
    for (unsigned i = 0; i < kOutputs; ++i) output_map[i].store(static_cast<uint8_t>(i));
  }
};

inline const char* to_string(SourceType t) {
  switch (t) {
    case SourceType::Input: return "input";
    case SourceType::Gen: return "gen";
    default: return "silence";
  }
}

inline const char* gen_name(GenId g) {
  switch (g) {
    case GenId::Sine: return "sine";
    case GenId::Noise: return "noise";
    case GenId::Ping: return "ping";
    default: return "?";
  }
}

inline bool parse_gen(const std::string& s, GenId* out) {
  if (s == "sine") { *out = GenId::Sine; return true; }
  if (s == "noise") { *out = GenId::Noise; return true; }
  if (s == "ping") { *out = GenId::Ping; return true; }
  return false;
}

inline const char* ping_name(PingVariant v) {
  switch (v) {
    case PingVariant::Bing: return "bing";
    case PingVariant::Bong: return "bong";
    default: return "tick";
  }
}

inline bool parse_ping(const std::string& s, PingVariant* out) {
  if (s == "tick") { *out = PingVariant::Tick; return true; }
  if (s == "bing") { *out = PingVariant::Bing; return true; }
  if (s == "bong") { *out = PingVariant::Bong; return true; }
  return false;
}

inline const char* noise_name(NoiseMode m) {
  return m == NoiseMode::Pink ? "pink" : "white";
}

inline bool parse_noise(const std::string& s, NoiseMode* out) {
  if (s == "white") { *out = NoiseMode::White; return true; }
  if (s == "pink") { *out = NoiseMode::Pink; return true; }
  return false;
}

inline const char* codec_name(ListenCodec c) {
  return c == ListenCodec::Opus ? "opus" : "pcm";
}

inline bool parse_codec(const std::string& s, ListenCodec* out) {
  if (s == "pcm") { *out = ListenCodec::Pcm; return true; }
  if (s == "opus") { *out = ListenCodec::Opus; return true; }
  return false;
}

// The audio loop writes each physical slot exactly once, via the channel maps. A map that is
// not a permutation would leave one slot never written (stale audio) and let two logical
// channels fight over another, so both the config loader and the web handler reject it.
template <class Vec>
inline bool is_slot_permutation(const Vec& v, unsigned limit) {
  bool seen[kTdmSlots] = {false};
  for (const auto& e : v) {
    const long x = static_cast<long>(e);
    if (x < 0 || x >= static_cast<long>(limit) || seen[x]) return false;
    seen[x] = true;
  }
  return true;
}

}  // namespace st
