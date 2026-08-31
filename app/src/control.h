#pragma once

#include <time.h>

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
  // Silences the channel before it reaches the ring, so the meters, the scope and anything
  // routed from it all agree it is off — the same "one version of the truth" the gain follows.
  // This is the switch an ALSA mixer pairs with the volume.
  std::atomic<bool> mute{false};
  // Set while a sender that declared its stream un-mixable (`mixer off`) holds a network channel:
  // the gain and the mute above are then ignored for it. Belongs to the stream, so it is never
  // persisted, and never set on an ADC channel.
  std::atomic<bool> bypass{false};
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
// like every other persisted live setting. `codec` is only the frontend's preference (it opts into
// Opus explicitly with ?codec=opus); the raw /api/listen wire default stays PCM regardless.
struct ListenControl {
  std::atomic<uint8_t> codec{static_cast<uint8_t>(ListenCodec::Opus)};
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

// Maps the card's sample counter onto CLOCK_MONOTONIC, so a network client can aim a packet at
// an absolute sample index on the one axis that means anything here. The audio thread publishes
// one pair per block; readers extrapolate between them.
//
// A seqlock rather than two independent atomics: n and t_ns must be observed as a single reading.
// A reader that caught a new n against a stale t would compute an offset off by a whole block and
// aim every packet that client ever sends at the wrong place.
class TimeAnchor {
 public:
  // Audio thread only. Cheap by design — clock_gettime(CLOCK_MONOTONIC) resolves in the vDSO,
  // so this costs no syscall and no allocation.
  void publish(uint64_t n, uint64_t t_ns) {
    const uint32_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);  // odd: a write is in progress
    std::atomic_thread_fence(std::memory_order_release);
    n_.store(n, std::memory_order_relaxed);
    t_ns_.store(t_ns, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    seq_.store(s + 2, std::memory_order_release);  // even again: consistent
  }

  // False when nothing has been published yet, or when the writer kept winning the race.
  bool read(uint64_t* n, uint64_t* t_ns) const {
    for (int attempt = 0; attempt < 8; ++attempt) {
      const uint32_t s1 = seq_.load(std::memory_order_acquire);
      if (s1 == 0 || (s1 & 1u) != 0) continue;
      const uint64_t rn = n_.load(std::memory_order_relaxed);
      const uint64_t rt = t_ns_.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (seq_.load(std::memory_order_relaxed) == s1) {
        *n = rn;
        *t_ns = rt;
        return true;
      }
    }
    return false;
  }

  // The sample index the card is expected to be at right now, extrapolated from the last block
  // boundary. Returns 0 while no anchor exists, which callers must treat as "not ready".
  uint64_t estimate(uint64_t t_now_ns, double rate) const {
    uint64_t n = 0, t = 0;
    if (!read(&n, &t)) return 0;
    if (t_now_ns <= t) return n;
    const double dt_s = static_cast<double>(t_now_ns - t) * 1e-9;
    return n + static_cast<uint64_t>(dt_s * rate);
  }

 private:
  std::atomic<uint32_t> seq_{0};
  std::atomic<uint64_t> n_{0};
  std::atomic<uint64_t> t_ns_{0};
};

// CLOCK_MONOTONIC in nanoseconds — the clock both ends of the network link agree to talk in.
inline uint64_t mono_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// Network audio input. `delay_frames` is the one the audio thread reads: local capture is held
// back by exactly this much before entering the ring, so that ring index n means the same real
// instant on a network channel as on an ADC channel. It is derived from enabled/delay_ms by the
// API layer rather than recomputed per block, and is 0 whenever network input is off — which is
// what keeps a local-only device bit-identical to how it behaved before any of this existed.
struct NetControl {
  std::atomic<bool> enabled{false};
  std::atomic<uint32_t> delay_ms{kNetDelayDefaultMs};
  std::atomic<uint32_t> delay_frames{0};
};

// Written by web handlers, read by the audio thread at the top of each block. Scalars are
// independent atomics; tearing across a block boundary there is benign.
struct Control {
  std::array<InputControl, kTotalInputs> inputs;
  std::array<OutputControl, kOutputs> outputs;
  SineControl sine;
  NoiseControl noise;
  PingControl ping;
  ListenControl listen;
  NetControl net;

  // input_map[logical] = physical TDM slot to capture from.
  // output_map[logical] = physical TDM slot to play into.
  std::array<std::atomic<uint8_t>, kInputs> input_map;
  std::array<std::atomic<uint8_t>, kOutputs> output_map;

  PingLog ping_log;
  TimeAnchor anchor;

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
