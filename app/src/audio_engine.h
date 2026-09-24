#pragma once

#include <pthread.h>

#include <atomic>
#include <memory>
#include <string>

#include "audio_backend.h"
#include "constants.h"
#include "control.h"
#include "engine_core.h"
#include "ring_buffer.h"
#include "util/clock.h"

namespace st {

class NetAudioServer;

struct EngineStats {
  bool running = false;
  bool sim = false;
  std::string device;
  unsigned rate = 0;
  unsigned period = 0;
  unsigned periods = 0;
  unsigned capture_channels = 0;
  std::string format;
  uint64_t xruns = 0;
  uint32_t generation = 0;
  uint64_t samples = 0;
  std::string last_error;
};

// The audio thread: opens a backend, keeps it open, and hands each block it captures to the
// EngineCore, which does everything else with it.
class AudioEngine {
 public:
  // Opens the card `opt` names, or the simulator when opt.sim is set. `clock` stamps the anchor
  // each block publishes and paces the simulator and the retries; it is the host's CLOCK_MONOTONIC
  // unless a test runs the engine on time of its own.
  AudioEngine(Control& ctl, RingBuffer& ring, EngineOptions opt,
              Clock& clock = monotonic_clock());
  // Drives `backend` instead of what `opt` would open: a test's stand-in for the card. The engine
  // does not own it, and opt.sim still picks which of the two run loops drives it.
  AudioEngine(Control& ctl, RingBuffer& ring, EngineOptions opt, AudioBackend& backend,
              Clock& clock = monotonic_clock());
  ~AudioEngine();

  bool start();
  void stop();

  EngineStats stats() const;

  // Wired in by main() before start(): the engine reads its network channels each block.
  void set_net(NetAudioServer* net) { core_.set_net(net); }
  // Also before start(): the rings the HDMI speakers and the line out's pair are handed over
  // through. Each is written every block, its sink on or off, so its counter stays equal to the
  // capture ring's and an index in it means the same sample as everywhere else. False, and the
  // sink left without audio, for a ring the engine cannot write: see EngineCore.
  bool set_hdmi_ring(RingBuffer* ring) { return core_.set_hdmi_ring(ring); }
  bool set_lineout_ring(RingBuffer* ring) { return core_.set_lineout_ring(ring); }
  double rate() const { return static_cast<double>(opt_.rate); }
  unsigned period() const { return period_.load(std::memory_order_relaxed); }
  uint64_t identify_frames() const { return core_.identify_frames(); }
  // The clock each block's anchor is stamped with. Whatever extrapolates the anchor has to read
  // the time from this one, or its estimate is off by however far apart the two clocks are.
  Clock& clock() const { return clock_; }

 private:
  friend struct EngineTestAccess;
  static void* thread_entry(void* self);

  // The backend keeps the one error slot, so that what it reports while it opens the card and
  // what the engine reports around it are the same field.
  void set_error(std::string msg) { backend_->set_error(std::move(msg)); }
  std::string error() const { return backend_->error(); }

  void wait_before_retry();
  // What start() sets up before the audio thread exists.
  void prepare();
  // The card: opened by the audio thread, and opened again after anything that ends the stream,
  // forever.
  void run_card();
  // The simulator: opened once, and it cannot fail.
  void run_sim();
  // Streams until the engine stops or the stream fails past recovering.
  void run_stream();
  // One block, captured, processed and played. False when the stream has ended.
  bool run_block();
  // An xrun is a discontinuity on the sample axis: counted, then left to the backend to recover
  // from. False ends the stream.
  bool recover(int err);
  // Takes on the shape the backend just opened with, for stats() and period().
  void publish_shape();

  RingBuffer& ring_;
  EngineOptions opt_;
  Clock& clock_;
  std::unique_ptr<AudioBackend> own_backend_;  // null when a test handed one in
  AudioBackend* const backend_;

  pthread_t thread_{};
  bool thread_valid_ = false;
  std::atomic<bool> running_{false};
  std::atomic<bool> streaming_{false};  // true only while audio is actually flowing
  std::atomic<uint64_t> xruns_{0};
  std::atomic<uint32_t> generation_{0};
  // Written by the audio thread (channel fallback, period renegotiation), read by stats().
  std::atomic<unsigned> cap_ch_{0};
  std::atomic<unsigned> period_{0};

  EngineCore core_;
};

}  // namespace st
