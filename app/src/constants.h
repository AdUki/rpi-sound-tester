#pragma once

#include <cstddef>
#include <cstdint>

// Shared with the ALSA plugin. Including it here rather than restating the numbers is what keeps
// the two ends of the wire from drifting apart.
#include "net_proto.h"

namespace st {

// Physical CS42448 channels. kInputs is the *ADC* count specifically: the TDM de-interleave,
// input_map, the mixer and the 8->6 capture fallback all mean this one, and none of them grow
// when network inputs are added.
inline constexpr unsigned kInputs = 6;
inline constexpr unsigned kOutputs = 8;  // CS42448 DAC channels

// Network input channels — how many senders the device can take at once. A remote machine
// running the soundtester ALSA plugin lands on one of these, and from there it is an input like
// any other: routable, metered, in the scope, in a freeze, and a valid xcorr operand.
//
// Each one costs (ring seconds + analyze seconds) x 384 kB of pinned RAM: about 41 MB at the
// 87 s ring and the 20 s analyze default, and it also adds one 8192-point FFT per 200 ms to the
// analysis thread. Twelve channels measure at ~495 MB pinned and a few percent of one Pi 3 core.
// Raising it further is one line, but check limits.pinned_mb against the board afterwards — and
// note it lowers the longest Analyze buffer that will still fit.
inline constexpr unsigned kNetInputs = 6;

// Ring width, and what every consumer of captured audio iterates: analysis, meters, spectrum,
// the scope envelope, listen/Ogg, capture snapshots and xcorr. Channels [0, kInputs) are the
// card's ADCs; [kInputs, kTotalInputs) are network inputs.
inline constexpr unsigned kTotalInputs = kInputs + kNetInputs;

inline constexpr bool is_net_input(unsigned ch) { return ch >= kInputs && ch < kTotalInputs; }

// The Octo machine driver raises capture channels_max to 8 while a stream runs
// (TDM frames carry 8 slots); slots 6..7 hold no ADC data.
inline constexpr unsigned kTdmSlots = 8;

// Digital make-up gain on the capture path, for devices whose output is too quiet to read.
// Amplification only: attenuating here cannot undo ADC clipping (the damage happened in the
// codec), it would just hide the clipped signal from the meters.
inline constexpr float kInputGainMinDb = 0.0f;
inline constexpr float kInputGainMaxDb = 40.0f;

// A network channel may be attenuated as well as amplified. The amplify-only rule above exists
// because attenuating after an ADC has clipped only hides the damage from the meters — and a
// network channel has no ADC. It is also what gives an ALSA mixer on the sending machine a
// playback volume worth the name, since a control that cannot go below unity is not one.
inline constexpr float kNetGainMinDb = -60.0f;

// The gain range depends on where the channel's audio comes from; there is one answer and both
// the config-apply path and the live PUT handler ask for it here.
inline constexpr float input_gain_min_db(unsigned ch) {
  return is_net_input(ch) ? kNetGainMinDb : kInputGainMinDb;
}

// Shared clamp ranges, enforced both when a config file is applied (config.cpp) and on the
// live PUT handlers (webserver.cpp) so the two paths cannot drift.
inline constexpr float kLevelMinDb = -60.0f;  // output gain and generator levels
inline constexpr float kLevelMaxDb = 0.0f;
inline constexpr float kSineFreqMinHz = 1.0f;
inline constexpr float kSineFreqMaxHz = 40000.0f;
inline constexpr float kPingIntervalMinS = 0.5f;
inline constexpr float kPingIntervalMaxS = 60.0f;

inline constexpr unsigned kDefaultRate = 96000;
inline constexpr unsigned kDefaultPeriod = 1024;
inline constexpr unsigned kDefaultPeriods = 4;

// 2^23 frames = 87.4 s at 96 kHz, 192 MB of float32 x 6ch. Power of two: index by mask.
// This is the *ceiling*; how much of it a freeze actually copies is set at runtime by
// CaptureStore::set_analyze_frames (POST /api/capture/config). Note the true RAM cost is
// ~2x this: the live ring here plus a nearly ring-sized frozen snapshot in CaptureStore —
// ~380 MB pinned in total. Raise this exponent only with headroom to spare — 2^24 would be
// ~760 MB total, too tight on a 1 GB Pi.
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
// kOpusFrameFrames * (rate / kOpusRate) ring frames -> decimation -> one opus_encode. 4096 was
// never a legal Opus frame size; 960 (@48 kHz) is, which is why the encoded path uses its own
// chunk length rather than kListenChunkFrames.
inline constexpr unsigned kOpusRate = 48000;
inline constexpr unsigned kOpusFrameFrames = 960;  // 20 ms @ 48 kHz — a legal Opus frame size

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

// ---- Network audio input -------------------------------------------------------------------
//
// A remote packet carries the absolute sample index it should be heard at, and the receiver
// writes it into a per-channel timeline at exactly that index. Ordering and duplication sort
// themselves out; loss is a hole; a packet that misses its slot is dropped and counted.
inline constexpr uint16_t kNetPort = ST_DEFAULT_PORT;
inline constexpr uint32_t kNetProtoVersion = ST_NET_PROTO_VERSION;

// Every local capture frame is held back by this much before entering the ring, so that ring
// index n means the same real-world instant on a network channel as on an ADC channel. Zero
// when no network input is enabled, which keeps the local-only device bit-identical to before.
inline constexpr unsigned kNetDelayDefaultMs = 1000;
inline constexpr unsigned kNetDelayMinMs = 0;
inline constexpr unsigned kNetDelayMaxMs = 2000;

// Per-channel timeline depth. Two readers sit in it at once — the playout read at n and the
// trailing ring read at n - delay — while the sender writes a delay ahead of playout, so the
// live span is about twice the delay. This has to clear 2 * kNetDelayMaxMs with room over.
inline constexpr unsigned kNetTimelineMs = 6000;

// How long the lead measurement is averaged over before it steers the converter, and how far the
// lead may wander before the stream is simply re-anchored instead. At the trim's authority a
// quarter-second offset would take two minutes to walk off; past that, one discontinuity now is
// better than being wrong for the next two minutes.
inline constexpr double kNetLeadFilterTauS = 2.0;
inline constexpr double kNetResyncFrames = 0.25 * kDefaultRate;

// Frames per audio packet. Small on purpose: a clock probe queued behind one of these on a
// 100 Mbit link waits under ~80 us, i.e. under ten samples of timing error at 96 kHz.
inline constexpr unsigned kNetPacketFrames = ST_PACKET_FRAMES;
inline constexpr unsigned kNetMaxPacketFrames = 4096;

static_assert(kDefaultRate == ST_DEFAULT_RATE, "the plugin's default rate must match the card's");

// "Genie" convenience helpers (GET /api/genie/sound, GET /api/genie/sync).
inline constexpr float kGenieSoundThresholdDb = -60.0f;  // peak_db above this reads as "sound"

// Per-ping xcorr window for GET /api/genie/sync, mirroring the console's Scope auto-measure
// (app.js PING_* constants): the window runs from a ping's emission up to just before the next, so
// it holds exactly one arrival wherever the loopback delay lands it — a fixed window centred on the
// emission misses the arrival once the loopback exceeds it.
inline constexpr uint64_t kPingWinGuard = 64;      // trim off the next emission -> one ping per window
inline constexpr uint64_t kPingMinWindow = 4096;   // a ping this near the buffer end can't be trusted
inline constexpr float kPingMinPeak = 0.05f;       // below this the correlation is noise, not an arrival

}  // namespace st
