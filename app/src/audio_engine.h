#pragma once

#include <pthread.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "constants.h"
#include "control.h"
#include "generators.h"
#include "ring_buffer.h"

typedef struct _snd_pcm snd_pcm_t;

namespace st {

class NetAudioServer;

struct EngineOptions {
  bool sim = false;
  std::string device = "hw:audioinjectoroc,0";
  unsigned rate = kDefaultRate;
  unsigned period = kDefaultPeriod;
  unsigned periods = kDefaultPeriods;
  unsigned capture_channels = kTdmSlots;  // falls back to kInputs if 8 ch cannot be opened
  // Simulator only: output channel c loops back into input channel c, delayed by
  // period + c*sim_stagger frames.
  unsigned sim_stagger = 0;
};

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

class AudioEngine {
 public:
  AudioEngine(Control& ctl, RingBuffer& ring, EngineOptions opt);
  ~AudioEngine();

  bool start();
  void stop();

  EngineStats stats() const;

  // Wired in by main() before start(): the engine reads its network channels each block.
  void set_net(NetAudioServer* net) { net_.store(net, std::memory_order_relaxed); }
  double rate() const { return static_cast<double>(opt_.rate); }
  unsigned period() const { return period_.load(std::memory_order_relaxed); }
  uint64_t identify_frames() const { return identify_frames_; }

 private:
  static void* thread_entry(void* self);

  // last_error_ is written by the audio thread and read by web handlers; a bare std::string
  // there would be a racing read against a reallocating write.
  void set_error(std::string msg);
  std::string error() const;

  void wait_before_retry();
  void size_buffers();
  // Picks up a change to ctl_.net.delay_frames and returns the delay now in force.
  unsigned sync_capture_delay();
  // Writes the local channels of `ring_out` from `live` delayed by cap_delay_frames_, so that
  // ring index n means the same real-world instant on an ADC channel as on a network channel.
  void apply_capture_delay(size_t frames, const float* live, float* ring_out);
  bool open_alsa();
  void close_alsa();
  bool configure(snd_pcm_t* pcm, unsigned channels, const char* what);
  bool prefill_and_start();
  bool recover(int err);
  void init_mixer();

  void run_alsa();
  void run_sim();

  // Shared by both backends: publishes one captured block to the ring and produces the
  // output block that sits on the same sample axis.
  // `in_all` is kTotalInputs wide: the backend fills channels [0, kInputs) with card audio and
  // process_block fills [kInputs, kTotalInputs) from the network timelines. It is modified in
  // place — input gain is applied to it before anything else reads it.
  void process_block(uint64_t n, size_t frames, float* in_all, float* out8);

  Control& ctl_;
  RingBuffer& ring_;
  EngineOptions opt_;
  Generators gen_;

  snd_pcm_t* capture_ = nullptr;
  snd_pcm_t* playback_ = nullptr;
  uint64_t identify_frames_ = 0;

  pthread_t thread_{};
  bool thread_valid_ = false;
  std::atomic<bool> running_{false};
  std::atomic<bool> streaming_{false};  // true only while audio is actually flowing
  std::atomic<uint64_t> xruns_{0};
  std::atomic<uint32_t> generation_{0};
  // Written by the audio thread (channel fallback, period renegotiation), read by stats().
  std::atomic<unsigned> cap_ch_{0};
  std::atomic<unsigned> period_{0};

  mutable std::mutex err_m_;
  std::string last_error_;

  std::vector<int32_t> raw_in_;
  std::vector<int32_t> raw_out_;
  // Set once before start(). The engine does not own it; a null pointer just means the network
  // channels stay silent.
  std::atomic<NetAudioServer*> net_{nullptr};

  // Two views of the same block, on the two axes this device has. `in_` is LIVE — what the card
  // just captured and what the network sender wants heard now — and is what outputs are routed
  // from, so a passthrough keeps its near-zero latency. `ring_block_` is the same audio on the
  // CAPTURE axis, every channel delayed alike, and is the only thing the ring ever sees.
  // With no delay configured the two are the same buffer and none of this costs anything.
  std::vector<float> in_;
  std::vector<float> ring_block_;
  std::vector<float> cap_delay_;   // kInputs wide, power-of-two frames, mask-indexed
  size_t cap_delay_len_ = 0;
  size_t cap_delay_mask_ = 0;
  size_t cap_delay_pos_ = 0;
  unsigned cap_delay_frames_ = 0;  // currently in force; a change resets the line
  std::vector<float> out8_;
  std::vector<float> gen_sine_, gen_noise_, gen_ping_;

  std::vector<float> sim_delay_;
  size_t sim_delay_len_ = 0;
};

}  // namespace st
