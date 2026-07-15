#pragma once

#include <cstddef>
#include <cstdint>

namespace st {

inline constexpr unsigned kInputs = 6;   // CS42448 ADC channels
inline constexpr unsigned kOutputs = 8;  // CS42448 DAC channels

// The Octo machine driver raises capture channels_max to 8 while a stream runs
// (TDM frames carry 8 slots); slots 6..7 hold no ADC data.
inline constexpr unsigned kTdmSlots = 8;

// Digital make-up gain on the capture path, for devices whose output is too quiet to read.
// Amplification only: attenuating here cannot undo ADC clipping (the damage happened in the
// codec), it would just hide the clipped signal from the meters.
inline constexpr float kInputGainMinDb = 0.0f;
inline constexpr float kInputGainMaxDb = 40.0f;

inline constexpr unsigned kDefaultRate = 96000;
inline constexpr unsigned kDefaultPeriod = 1024;
inline constexpr unsigned kDefaultPeriods = 4;

// 2^23 frames = 87.4 s at 96 kHz, 192 MB of float32 x 6ch. Power of two: index by mask.
// This is the *ceiling*; how much of it a freeze actually copies is set at runtime by
// CaptureStore::set_analyze_frames (POST /api/capture/config). Note the true RAM cost is
// ~2x this: the live ring here plus an equally-sized frozen snapshot in CaptureStore. So
// 192 MB ring + 192 MB snapshot = 384 MB pinned. Raise this exponent only with headroom to
// spare — 2^24 would be 768 MB total, too tight on a 1 GB Pi.
inline constexpr size_t kRingFrames = 1u << 23;

// Freeze copies the ring minus a safety margin, so the copy has room to finish before the
// writer can lap its oldest sample. The margin is max(this many periods, kRingFrames/32):
// a fixed period count is too thin once the ring — and thus the memcpy — grows.
inline constexpr size_t kFreezeHeadroomPeriods = 8;

// Smallest analyze/snapshot length the config API will accept, so a fat-fingered 0 can't
// leave nothing to freeze. 4096 frames = ~43 ms at 96 kHz.
inline constexpr size_t kCaptureMinFrames = 4096;

// Default analyze/snapshot length on startup — a freeze grabs this much unless the config API
// changes it. Well under the ceiling so a fresh freeze is quick; raise it per-session as needed.
inline constexpr double kCaptureDefaultSeconds = 20.0;

// One envelope column per 480 frames = 200 columns/s at 96 kHz; 60 s of history.
inline constexpr unsigned kEnvColumnFrames = 480;
inline constexpr size_t kEnvColumns = 12000;

inline constexpr unsigned kSpectrumFft = 8192;
inline constexpr unsigned kSpectrumBins = 240;

inline constexpr size_t kXcorrMaxLen = 1u << 19;
inline constexpr size_t kXcorrMaxFft = 1u << 20;

inline constexpr unsigned kListenChunkFrames = 4096;
inline constexpr unsigned kMaxListenStreams = 12;

// Opus listen path. Opus only accepts 8/12/16/24/48 kHz input, so the 96 kHz ring is decimated
// to 48 kHz and encoded in 20 ms frames. One WebSocket message carries one frame:
// kOpusInFrames read from the ring -> (rate/kOpusRate):1 decimation -> kOpusFrameFrames -> one
// opus_encode. 4096 was never a legal Opus frame size; 960 (@48 kHz) is, which is why the
// encoded path uses its own chunk length rather than kListenChunkFrames.
inline constexpr unsigned kOpusRate = 48000;
inline constexpr unsigned kOpusFrameFrames = 960;  // 20 ms @ 48 kHz — a legal Opus frame size
// Ring frames per Opus frame at the default 96 kHz rate (2:1). The encoder recomputes this from
// the live engine rate; kOpusFrameFrames * (rate / kOpusRate).
inline constexpr unsigned kOpusInFramesAt96 = kOpusFrameFrames * (kDefaultRate / kOpusRate);

// Per-mono-channel Opus bitrate. The multichannel stream.ogg scales this by the channel count.
inline constexpr int kListenBitrateDefaultKbps = 96;
inline constexpr int kListenBitrateMinKbps = 16;
inline constexpr int kListenBitrateMaxKbps = 256;

// stream.ogg runs kInputs encoders on one worker thread; cap concurrent multichannel streams
// well below kMaxListenStreams so a handful of them cannot starve the audio/analysis threads.
inline constexpr unsigned kMaxOggStreams = 2;

enum class ListenCodec : uint8_t { Pcm = 0, Opus = 1 };

// Opus needs an integer decimation from the engine rate down to 48 kHz (factor 1 or 2 here).
inline constexpr bool opus_rate_supported(unsigned rate) {
  return rate == kOpusRate || rate == 2 * kOpusRate;
}

inline constexpr size_t kPingLogEntries = 64;

}  // namespace st
