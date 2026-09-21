#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "constants.h"
#include "control.h"
#include "ring_buffer.h"
#include "util/asrc.h"

typedef struct _snd_pcm snd_pcm_t;

namespace st {

class AudioEngine;

// ---- The pieces of the path that need no hardware, exposed so they can be tested ---------------

enum class SocPull {
  Ok,       // copied, and *r_n advanced
  Starved,  // not written yet: the reader has caught up with the engine
  Lapped,   // already overwritten: the reader fell a whole ring behind
};

// Copies the interleaved frames [*r_n, *r_n + frames) out of the handoff ring. Only an Ok moves
// *r_n; on either failure it is left where it was, so the caller decides what "where" becomes.
SocPull soc_pull(const RingBuffer& ring, uint64_t* r_n, size_t frames, float* out);

// Keeps the first `channels` slots of each `stride`-wide ring frame: the converter then works on
// the slots in play only, and its cost follows the layout rather than the ring's width.
void soc_select(const float* in, size_t frames, unsigned stride, unsigned channels, float* out);

// Converts `channels`-wide float frames to the PCM's S16 frames. One channel (mono) is written to
// both L and R of a stereo PCM; anything else passes straight through, `channels` wide.
void soc_to_s16(const float* in, size_t frames, unsigned channels, int16_t* out);

// How many slots the converter carries for a layout: mono's one, else the PCM's width.
inline unsigned soc_converted_channels(const HdmiLayoutInfo& l) {
  return l.speakers == 1 ? 1 : l.pcm_channels;
}

// Holds a sink's latency constant against the drift between the Octo's clock and the Pi's.
//
// `total` is how far behind the card's current sample the output is: the frames still in the
// handoff ring plus those queued in the driver, in engine frames. It is steered to `target` by
// trimming the converter's ratio, never by a step, the same loop the network input uses. What a
// delay measurement through the sink needs is exactly this: a latency that does not move.
struct SocServo {
  LeadFilter filter;
  double trim = 1.0;

  void reset() {
    filter.reset();
    trim = 1.0;
  }

  // Returns the trim to apply to the next chunk.
  double update(double total, double dt_s, double target, double rate) {
    const double avg = filter.update(total, dt_s, kNetLeadFilterTauS);
    trim = asrc_trim(avg, target, rate, kAsrcTauS, kAsrcTrimMax);
    return trim;
  }

  // Further out than the trim should be asked to walk back.
  bool adrift(double target, double limit) const {
    const double e = filter.avg - target;
    return filter.primed && (e > limit || e < -limit);
  }
};

// A playback device the sinks could be pointed at: every PCM device of every card present, named
// the way the config and the API want it (hw:<card id>,<device>), so the console can offer a list
// rather than ask for an ALSA name. Cards come and go (a USB interface, a driver reloaded), so
// it is read fresh each time.
struct PlaybackDevice {
  std::string device;  // hw:b1,0
  std::string card;    // b1
  std::string name;    // bcm2835 HDMI 1
};
std::vector<PlaybackDevice> list_playback_devices();

// ---- The output itself --------------------------------------------------------------------------

// What tells the two sinks apart. Everything else about them is the same code.
struct SocSink {
  const char* name;   // "hdmi" | "lineout": the log prefix, and the realtime thread's name
  unsigned width;     // the ring's width: the most PCM slots the sink can have
  const char* where;  // why a device might not exist, appended to an open error that says so
};

inline constexpr SocSink kHdmiSink{
    "hdmi", kHdmiMaxChannels,
    "the Pi's HDMI audio needs dtparam=audio=on in config.txt; it is hw:b1,0 with "
    "snd_bcm2835.enable_compat_alsa=0 on the kernel command line, hw:ALSA,1 without"};
inline constexpr SocSink kLineoutSink{
    "lineout", kLineoutChannels,
    "the Pi's 3.5 mm jack needs dtparam=audio=on in config.txt; it is hw:Headphones,0 with "
    "snd_bcm2835.enable_compat_alsa=0 on the kernel command line, hw:ALSA,0 without"};

struct SocStatus {
  bool enabled = false;   // what the operator asked for
  bool open = false;      // the PCM is open
  bool playing = false;   // anchored and streaming audio from the engine
  std::string device;
  unsigned sample_rate = 0;  // what was asked for
  unsigned device_rate = 0;  // what the driver agreed to; 0 while closed
  HdmiLayout layout = kHdmiLayoutDefault;  // mono, stereo, 5.1 or 7.1; the line out is stereo
  unsigned speakers = 0;         // how many are routed and played
  unsigned device_channels = 0;  // how many the PCM was opened with (2 for mono); 0 while closed
  unsigned period_frames = 0;
  unsigned buffer_frames = 0;
  uint64_t xruns = 0;      // the driver ran dry
  uint64_t underruns = 0;  // the engine stopped supplying audio (its card reopening, say)
  uint64_t overruns = 0;   // this thread fell a whole ring behind
  uint64_t resyncs = 0;    // latency strayed too far to trim back, so it was re-anchored
  // How long after the engine renders a sample it leaves the driver, averaged, and the value that
  // is held. Excludes the firmware's pipeline and the sink's own processing: those are constant,
  // and a one-off calibration takes them out.
  double latency_ms = 0.0;
  double target_ms = 0.0;
  double ring_ms = 0.0;  // of which: waiting in the handoff ring (instantaneous)
  double alsa_ms = 0.0;  // of which: queued in the driver (instantaneous)
  double trim_ppm = 0.0;
  std::string error;
};

// Plays the channels the audio thread renders into `ring()` on one of the SoC's own outputs, on
// its own thread, on its own clock. One instance per sink: HDMI and the line out.
//
// Nothing here is on the audio thread's path: the engine only ever writes the ring, so whether
// this thread is running, stalled, retrying a missing card or being reconfigured, the Octo does
// not notice. Like the engine, a device that will not open is never fatal: the thread retries it
// forever and says why in status().
class SocOutput {
 public:
  SocOutput(const SocSink& sink, SocControl& sctl, Control& ctl, const AudioEngine& engine,
            std::string device, unsigned sample_rate);
  ~SocOutput();
  SocOutput(const SocOutput&) = delete;
  SocOutput& operator=(const SocOutput&) = delete;

  const SocSink& sink() const { return sink_; }
  RingBuffer& ring() { return ring_; }
  uint64_t pinned_bytes() const { return ring_.pinned_bytes(); }

  // Both idempotent. start() only spawns the thread; the device is opened there.
  void start();
  void stop();
  bool running() const { return running_.load(); }

  // Takes effect on the next open: a running output is restarted to pick it up.
  void configure(std::string device, unsigned sample_rate);
  // Reopens the PCM if the output is running, to pick up a new layout.
  void restart();
  std::string device() const;
  unsigned sample_rate() const;

  SocStatus status() const;

 private:
  void start_locked();
  void stop_locked();
  void run();
  bool open_pcm();
  void close_pcm();
  // Streams until the PCM fails or the output is stopped. False means the PCM needs reopening.
  bool stream();
  // Restarts playback from silence with the reader placed so the latency starts at its target.
  bool anchor();
  // Waits until the engine is producing audio. False if stopped meanwhile.
  bool wait_for_engine();
  // Writes all of `frames`, waiting for room. A negative ALSA error on failure.
  long write_all(const int16_t* buf, size_t frames);
  void sleep_ms(unsigned ms) const;
  void set_error(std::string msg);

  const SocSink sink_;
  SocControl& sctl_;
  Control& ctl_;
  const AudioEngine& engine_;
  RingBuffer ring_;

  mutable std::mutex m_;  // guards device_, sample_rate_, error_
  std::string device_;
  unsigned sample_rate_;
  std::string error_;

  // Serialises start/stop/configure, which web handlers may call concurrently.
  std::mutex life_m_;
  std::thread thread_;
  std::atomic<bool> running_{false};

  // Thread-owned.
  snd_pcm_t* pcm_ = nullptr;
  Asrc asrc_;
  SocServo servo_;
  uint64_t r_n_ = 0;
  unsigned ch_ = 2;      // slots converted, fixed while the PCM is open: soc_converted_channels
  unsigned pcm_ch_ = 2;  // the PCM's own width: the layout's pcm_channels
  size_t chunk_in_ = 0;  // engine frames pulled per pass: one device period's worth
  double rate_ = 0.0;    // engine rate
  double target_ = 0.0;  // latency held, in engine frames
  std::vector<float> in_;   // as pulled from the ring: sink_.width wide
  std::vector<float> sel_;  // the slots in play: ch_ wide
  std::vector<float> out_;
  std::vector<int16_t> pcm_buf_;

  // Published for status().
  std::atomic<bool> open_{false};
  std::atomic<bool> playing_{false};
  std::atomic<unsigned> dev_rate_{0};
  std::atomic<unsigned> dev_period_{0};
  std::atomic<unsigned> dev_buffer_{0};
  std::atomic<unsigned> dev_channels_{0};
  std::atomic<uint64_t> xruns_{0};
  std::atomic<uint64_t> underruns_{0};
  std::atomic<uint64_t> overruns_{0};
  std::atomic<uint64_t> resyncs_{0};
  std::atomic<float> latency_ms_{0.0f};
  std::atomic<float> target_ms_{0.0f};
  std::atomic<float> ring_ms_{0.0f};
  std::atomic<float> alsa_ms_{0.0f};
  std::atomic<float> trim_ppm_{0.0f};
};

}  // namespace st
