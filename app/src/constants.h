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

// 2^20 frames = 10.9 s at 96 kHz, 24 MB of float32 x 6ch. Power of two: index by mask.
inline constexpr size_t kRingFrames = 1u << 20;

// Freeze copies the ring minus this many periods, so the copy has room to finish before
// the writer can lap its oldest sample (the ring's own 2-period margin is too tight for a
// 24 MB memcpy on a Pi 3).
inline constexpr size_t kFreezeHeadroomPeriods = 8;

// One envelope column per 480 frames = 200 columns/s at 96 kHz; 60 s of history.
inline constexpr unsigned kEnvColumnFrames = 480;
inline constexpr size_t kEnvColumns = 12000;

inline constexpr unsigned kSpectrumFft = 8192;
inline constexpr unsigned kSpectrumBins = 240;

inline constexpr size_t kXcorrMaxLen = 1u << 19;
inline constexpr size_t kXcorrMaxFft = 1u << 20;

inline constexpr unsigned kListenChunkFrames = 4096;
inline constexpr unsigned kMaxListenStreams = 12;

inline constexpr size_t kPingLogEntries = 64;

}  // namespace st
