#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "constants.h"
#include "pcm_format.h"
#include "control.h"
#include "ring_buffer.h"
#include "util/asrc.h"
#include "util/clock.h"

typedef struct _snd_pcm snd_pcm_t;

namespace st {

class AudioEngine;

// ---- The pieces of the path that need no hardware, exposed so they can be tested ---------------

enum class SinkPull {
  Ok,       // copied, and *r_n advanced
  Starved,  // not written yet: the reader has caught up with the engine
  Lapped,   // already overwritten: the reader fell a whole ring behind
};

// Copies the interleaved frames [*r_n, *r_n + frames) out of the handoff ring. Only an Ok moves
// *r_n; on either failure it is left where it was, so the caller decides what "where" becomes.
SinkPull sink_pull(const RingBuffer& ring, uint64_t* r_n, size_t frames, float* out);

// Keeps the first `channels` slots of each `stride`-wide ring frame: the converter then works on
// the slots in play only, and its cost follows the layout rather than the ring's width.
void sink_select(const float* in, size_t frames, unsigned stride, unsigned channels, float* out);

// How many slots the converter carries for a layout: mono's one, else the PCM's width.
inline unsigned sink_converted_channels(const SinkLayoutInfo& l) {
  return l.speakers == 1 ? 1 : l.pcm_channels;
}

// Holds a sink's latency constant against the drift between the engine's clock and the device's.
//
// `total` is how far behind the card's current sample the output is: the frames still in the
// handoff ring plus those queued in the driver, in engine frames. It is steered to `target` by
// trimming the converter's ratio, never by a step, the same loop the network input uses. What a
// delay measurement through the sink needs is exactly this: a latency that does not move.
struct SinkServo {
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

// ---- The output itself --------------------------------------------------------------------------

// The device a sink slot plays on: found by Devices, and fixed while the slot is bound to it.
struct SinkDevice {
  std::string id;     // "<card id>,<device>", or the ALSA name of a --sink that is not hw
  std::string alsa;   // what it is opened as
  std::string label;  // what the console calls it
  bool hdmi = false;  // offers HDMI's speaker layouts
  bool usb = false;
  std::vector<SinkLayout> layouts{kSinkLayoutDefault};  // never empty
  std::vector<unsigned> rates;                          // of kSinkRates; never empty
  // Whether layouts and rates are the device's own: false when it could not be opened to ask,
  // and they are guesses until it can.
  bool probed = false;

  bool offers(SinkLayout l) const;
  bool offers_rate(unsigned r) const;
  // What a slot bound to this device starts with, unless its config says otherwise.
  SinkLayout default_layout() const;
  unsigned default_rate() const;
};

struct SinkStatus {
  bool enabled = false;   // what the operator asked for
  bool open = false;      // the PCM is open
  bool playing = false;   // anchored and streaming audio from the engine
  std::string device;
  unsigned sample_rate = 0;  // what was asked for
  unsigned device_rate = 0;  // what the driver agreed to; 0 while closed
  SinkLayout layout = kSinkLayoutDefault;
  unsigned speakers = 0;         // how many are routed and played
  unsigned device_channels = 0;  // how many the PCM was opened with (2 for mono); 0 while closed
  const char* format = "";       // what the PCM was opened in; "" while closed
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

// Plays the channels the audio thread renders into `ring()` on a playback device, on its own
// thread, on its own clock. One instance per sink slot, made before the audio thread starts; the
// device is bound to it when Devices finds one.
//
// Nothing here is on the audio thread's path: the engine only ever writes the ring, so whether
// this thread is running, stalled, retrying a missing device or being reconfigured, the engine
// does not notice. Like the engine, a device that will not open is never fatal: the thread retries
// it forever and says why in status(). A USB device that is unplugged is simply one that will not
// open, until it is plugged back in.
class SinkOutput {
 public:
  SinkOutput(unsigned slot, SinkControl& sctl, Control& ctl, const AudioEngine& engine);
  ~SinkOutput();
  SinkOutput(const SinkOutput&) = delete;
  SinkOutput& operator=(const SinkOutput&) = delete;

  unsigned slot() const { return slot_; }
  SinkControl& control() { return sctl_; }
  const SinkControl& control() const { return sctl_; }
  RingBuffer& ring() { return ring_; }
  uint64_t pinned_bytes() const { return ring_.pinned_bytes(); }

  // Both idempotent. start() only spawns the thread; the device is opened there.
  void start();
  void stop();
  bool running() const { return running_.load(); }

  // The device this slot plays on, and the rate its PCM is opened at. Takes effect on the next
  // open: a running output is restarted to pick it up.
  void bind(SinkDevice device, unsigned sample_rate);
  void set_sample_rate(unsigned sample_rate);
  // Reopens the PCM if the output is running, to pick up a new layout.
  void restart();
  SinkDevice device() const;
  unsigned sample_rate() const;

  SinkStatus status() const;

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
  // Writes all of `frames` of pcm_buf_ from `from`, waiting for room. A negative ALSA error on
  // failure.
  long write_all(size_t from, size_t frames);
  void sleep_ms(unsigned ms) const;
  void set_error(std::string msg);

  const unsigned slot_;
  SinkControl& sctl_;
  Control& ctl_;
  const AudioEngine& engine_;
  // The engine's: the clock its anchors are stamped with, and so the only one an estimate from
  // them comes out right on.
  Clock& clock_;
  RingBuffer ring_;

  mutable std::mutex m_;  // guards device_, sample_rate_, error_
  SinkDevice device_;
  unsigned sample_rate_ = kSinkRateDefault;
  std::string error_;

  // Serialises start/stop/bind, which web handlers and Devices may call concurrently.
  std::mutex life_m_;
  std::thread thread_;
  std::atomic<bool> running_{false};

  // Thread-owned.
  std::string name_;  // the device id, as the log prefixes its lines
  snd_pcm_t* pcm_ = nullptr;
  PcmFormat format_ = PcmFormat::S16_LE;
  Asrc asrc_;
  SinkServo servo_;
  uint64_t r_n_ = 0;
  unsigned ch_ = 2;      // slots converted, fixed while the PCM is open: sink_converted_channels
  unsigned pcm_ch_ = 2;  // the PCM's own width: the layout's pcm_channels
  size_t chunk_in_ = 0;  // engine frames pulled per pass: one device period's worth
  double rate_ = 0.0;    // engine rate
  double target_ = 0.0;  // latency held, in engine frames
  std::vector<float> in_;   // as pulled from the ring: kMaxSinkWidth wide
  std::vector<float> sel_;  // the slots in play: ch_ wide
  std::vector<float> out_;
  std::vector<uint8_t> pcm_buf_;  // pcm_ch_ wide, in format_

  // Published for status().
  std::atomic<bool> open_{false};
  std::atomic<bool> playing_{false};
  std::atomic<unsigned> dev_rate_{0};
  std::atomic<unsigned> dev_period_{0};
  std::atomic<unsigned> dev_buffer_{0};
  std::atomic<unsigned> dev_channels_{0};
  std::atomic<const char*> dev_format_{""};
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
