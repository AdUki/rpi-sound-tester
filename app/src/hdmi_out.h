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

// ---- The pieces of the HDMI path that need no hardware, exposed so they can be tested ----------

enum class HdmiPull {
  Ok,       // copied, and *r_n advanced
  Starved,  // not written yet: the reader has caught up with the engine
  Lapped,   // already overwritten: the reader fell a whole ring behind
};

// Copies the interleaved frames [*r_n, *r_n + frames) out of the handoff ring. Only an Ok moves
// *r_n; on either failure it is left where it was, so the caller decides what "where" becomes.
HdmiPull hdmi_pull(const RingBuffer& ring, uint64_t* r_n, size_t frames, float* out);

// Holds the HDMI path's latency constant against the drift between the Octo's clock and the Pi's.
//
// `total` is how far behind the card's current sample the HDMI output is: the frames still in the
// handoff ring plus those queued in the HDMI driver, in engine frames. It is steered to `target` by
// trimming the converter's ratio, never by a step, the same loop the network input uses. What a
// delay measurement through HDMI needs is exactly this: a latency that does not move.
struct HdmiServo {
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

struct HdmiStatus {
  bool enabled = false;   // what the operator asked for
  bool open = false;      // the PCM is open
  bool playing = false;   // anchored and streaming audio from the engine
  std::string device;
  unsigned sample_rate = 0;  // what was asked for
  unsigned device_rate = 0;  // what the driver agreed to; 0 while closed
  unsigned period_frames = 0;
  unsigned buffer_frames = 0;
  uint64_t xruns = 0;      // the HDMI driver ran dry
  uint64_t underruns = 0;  // the engine stopped supplying audio (its card reopening, say)
  uint64_t overruns = 0;   // this thread fell a whole ring behind
  uint64_t resyncs = 0;    // latency strayed too far to trim back, so it was re-anchored
  // How long after the engine renders a sample it leaves the HDMI driver, averaged, and the value
  // that is held. Excludes the firmware's pipeline and the sink's own processing: those are
  // constant, and a one-off calibration takes them out.
  double latency_ms = 0.0;
  double target_ms = 0.0;
  double ring_ms = 0.0;  // of which: waiting in the handoff ring (instantaneous)
  double alsa_ms = 0.0;  // of which: queued in the driver (instantaneous)
  double trim_ppm = 0.0;
  std::string error;
};

// Plays the HDMI pair the audio thread renders into `ring()`, on its own thread, on its own clock.
//
// Nothing here is on the audio thread's path: the engine only ever writes the ring, so whether
// this thread is running, stalled, retrying a missing card or being reconfigured, the Octo does
// not notice. Like the engine, a device that will not open is never fatal: the thread retries it
// forever and says why in status().
class HdmiOutput {
 public:
  HdmiOutput(Control& ctl, const AudioEngine& engine, std::string device, unsigned sample_rate);
  ~HdmiOutput();
  HdmiOutput(const HdmiOutput&) = delete;
  HdmiOutput& operator=(const HdmiOutput&) = delete;

  RingBuffer& ring() { return ring_; }
  uint64_t pinned_bytes() const { return ring_.pinned_bytes(); }

  // Both idempotent. start() only spawns the thread; the device is opened there.
  void start();
  void stop();
  bool running() const { return running_.load(); }

  // Takes effect on the next open: a running output is restarted to pick it up.
  void configure(std::string device, unsigned sample_rate);
  std::string device() const;
  unsigned sample_rate() const;

  HdmiStatus status() const;

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
  HdmiServo servo_;
  uint64_t r_n_ = 0;
  size_t chunk_in_ = 0;  // engine frames pulled per pass: one HDMI period's worth
  double rate_ = 0.0;    // engine rate
  double target_ = 0.0;  // latency held, in engine frames
  std::vector<float> in_;
  std::vector<float> out_;
  std::vector<int16_t> pcm_buf_;

  // Published for status().
  std::atomic<bool> open_{false};
  std::atomic<bool> playing_{false};
  std::atomic<unsigned> dev_rate_{0};
  std::atomic<unsigned> dev_period_{0};
  std::atomic<unsigned> dev_buffer_{0};
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
