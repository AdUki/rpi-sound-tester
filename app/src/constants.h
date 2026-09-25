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

// The most columns the ring can have: what every per-channel array is sized for. How many a run
// actually has is channels().total() (channel_layout.h), decided once at startup. The telemetry
// mask is one bit per column in a u32, so this may not pass 32.
inline constexpr unsigned kMaxInputs = 24;
static_assert(kInputs + kNetInputs <= kMaxInputs, "the Octo's layout must fit");

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

// Shared clamp ranges, enforced both when a config file is applied (config.cpp) and on the
// live PUT handlers (webserver.cpp) so the two paths cannot drift.
inline constexpr float kLevelMinDb = -60.0f;  // output gain and generator levels
inline constexpr float kLevelMaxDb = 0.0f;
inline constexpr float kSineFreqMinHz = 1.0f;
inline constexpr float kSineFreqMaxHz = 40000.0f;
inline constexpr float kPingIntervalMinS = 0.5f;
inline constexpr float kPingIntervalMaxS = 60.0f;

// How long a device that would not open, or stopped, is left before it is opened again: the card
// by the audio thread, each SoC sink by its own. One open of the card also waits this long for its
// device node to appear, since the codec probes a few seconds into boot.
inline constexpr unsigned kReopenDelayS = 5;

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

// The scope's envelope: 200 min/max columns per second of capture, 480 frames each at 96 kHz, and
// 60 s of history. Counted in seconds rather than frames so that every rate gets the same history.
inline constexpr unsigned kEnvColumnsPerS = 200;
inline constexpr size_t kEnvColumns = 12000;

// How many frames one envelope column covers at `rate`. Never zero, whatever a config file says the
// rate is: the analysis thread divides by it.
inline constexpr unsigned env_column_frames(unsigned rate) {
  return rate >= kEnvColumnsPerS ? rate / kEnvColumnsPerS : 1;
}

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
// The base port, clamped so that base + kNetInputs, the last per-channel port, still exists.
inline constexpr int kNetPortMin = 1;
inline constexpr int kNetPortMax = 65535 - static_cast<int>(kNetInputs);
inline constexpr uint32_t kNetProtoVersion = ST_NET_PROTO_VERSION;

// Every local capture frame is held back by this much before entering the ring, so that ring
// index n means the same real-world instant on a network channel as on an ADC channel. Zero
// when no network input is enabled, which keeps the local-only device bit-identical to before.
inline constexpr unsigned kNetDelayDefaultMs = 1000;
inline constexpr unsigned kNetDelayMinMs = 0;
inline constexpr unsigned kNetDelayMaxMs = 2000;

// The block the network write guard is counted in: two of these ahead of the reader (see
// NetAudioServer::guard_frames), whatever period the engine runs. A guard that grew with the
// period would drop every packet of a short net.delay_ms (21 to 43 ms) as late.
inline constexpr unsigned kNetGuardPeriod = 1024;

// Per-channel timeline depth. Two readers sit in it at once — the playout read at n and the
// trailing ring read at n - delay — while the sender writes a delay ahead of playout, so the
// live span is about twice the delay. This has to clear 2 * kNetDelayMaxMs with room over.
inline constexpr unsigned kNetTimelineMs = 6000;

// How long the lead measurement is averaged over before it steers the converter, and how far the
// lead may wander before the stream is simply re-anchored instead. At the trim's authority a
// quarter-second offset would take two minutes to walk off; past that, one discontinuity now is
// better than being wrong for the next two minutes.
inline constexpr double kNetLeadFilterTauS = 2.0;
inline constexpr double kNetResyncS = 0.25;

// How quickly a converter's ratio trim closes a residual offset, and how far it may stray from
// nominal. 0.2% is far more than two crystals can differ by, and small enough that the audio does
// not audibly change pitch while it is being applied. Shared by every clock this device has to
// follow: a network sender's, and every sink's.
inline constexpr double kAsrcTauS = 5.0;
inline constexpr double kAsrcTrimMax = 0.002;

// Frames per audio packet. Small on purpose: a clock probe queued behind one of these on a
// 100 Mbit link waits under ~80 us, i.e. under ten samples of timing error at 96 kHz.
inline constexpr unsigned kNetPacketFrames = ST_PACKET_FRAMES;
inline constexpr unsigned kNetMaxPacketFrames = 4096;

// ---- Sinks: every playback device other than the engine card ------------------------------
//
// HDMI, a Pi's 3.5 mm jack, a USB interface: each plays the same buses and routed inputs as the
// engine card's outputs, rendered by the audio thread at the same n and handed to a thread of its
// own through a ring. None of them runs on the engine's clock, so that thread follows it with a
// trimmed sample-rate converter, as a network input does.
//
// A sink has up to 8 channels: all HDMI carries as plain PCM (7.1). Its ring and routing are always
// that wide, so a change of layout only reopens its PCM; the audio thread renders the channels in
// play into their PCM slots and zeroes the rest. There are up to kMaxSinks of them, each made when
// a device is found for it and kept for the life of the process.
inline constexpr unsigned kMaxSinkWidth = 8;
inline constexpr unsigned kMaxSinks = 8;

// The rate a PCM is opened at unless its config says otherwise. 48 kHz because every HDMI sink and
// nearly every USB device accepts it: HDMI drivers advertise anything up to 192 kHz whatever the
// TV can actually play, so asking for more "succeeds" even when the sink then resamples or drops it
// out of sight. The choices are HDMI's own audio rates; the converter takes the engine's rate to
// any of them, and a device is offered those of them it accepts.
inline constexpr unsigned kSinkRateDefault = 48000;
inline constexpr unsigned kSinkRates[] = {32000, 44100, 48000, 88200, 96000, 176400, 192000};
inline constexpr bool sink_rate_ok(unsigned r) {
  for (unsigned x : kSinkRates)
    if (x == r) return true;
  return false;
}

// Handoff ring, in engine frames: 2^16 is 0.68 s at 96 kHz, 2 MB pinned at 8 channels. The reader
// sits a few periods behind the writer, so this only has to outlast a stall of its thread.
inline constexpr size_t kSinkRingFrames = 1u << 16;

// The PCM's own buffering. The Pi's firmware driver reports its position in coarse steps, so
// generous periods are worth more than a few milliseconds of latency: what a delay measurement needs is for
// the latency to be constant, not small.
inline constexpr unsigned kSinkPeriodMs = 20;
inline constexpr unsigned kSinkPeriods = 4;

// How far behind the card's current sample the reader aims, in engine periods, on top of the
// PCM's buffer itself. The engine publishes a whole period at a time, so the reader needs at least
// one period of slack to never find its next chunk not yet written; three leaves room for
// scheduling jitter.
inline constexpr unsigned kSinkRingLagPeriods = 3;

// Past this much latency error the output is re-anchored rather than walked back by the trim.
inline constexpr double kSinkResyncS = 0.05;

// How long after an anchor the latency readings are ignored. A driver that has just started
// reports its queue in a transient way for the first few periods — measured under PipeWire at
// 70 ms short — and letting that prime the filter trips a resync against nothing.
inline constexpr double kSinkSettleS = 0.5;

// Below the audio thread (80): a hiccup on a sink must never cost the engine a block.
inline constexpr int kSinkRtPriority = 60;

// ---- Device inputs: every capture device other than the engine card ----------------------------
//
// A USB interface's inputs, the VIM3L's HDMI loopback: captured on the device's own thread and
// clock, and placed on the engine's axis through a trimmed converter, into ring columns reserved for
// them at startup (board.json's device_inputs).
//
// While any is bound the capture axis is held back at least this much, so a frame captured now has
// arrived before the ring reads it: the driver's period and queue, the converter's chunk and the
// timeline's guard of two engine blocks all fit in it.
inline constexpr unsigned kDeviceInputDelayMs = 150;
// The capture PCM's own buffering: short periods, so a frame waits in the driver as little as
// possible, and enough of them to ride out a stall of its thread.
inline constexpr unsigned kInputPeriodMs = 10;
inline constexpr unsigned kInputPeriods = 8;

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
