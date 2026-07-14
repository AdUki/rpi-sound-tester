#pragma once

#include <pthread.h>

#include <atomic>
#include <string>
#include <vector>

#include "constants.h"
#include "control.h"
#include "generators.h"
#include "ring_buffer.h"

typedef struct _snd_pcm snd_pcm_t;

namespace st {

inline constexpr double kIdentifySeconds = 0.5;  // 3 x 100 ms bursts, 100 ms apart

struct EngineOptions {
  bool sim = false;
  std::string device = "hw:audioinjectoroc,0";
  unsigned rate = kDefaultRate;
  unsigned period = kDefaultPeriod;
  unsigned periods = kDefaultPeriods;
  unsigned capture_channels = kTdmSlots;  // falls back to kInputs if 8 ch cannot be opened
  unsigned open_retry_s = 5;   // per attempt; the audio thread retries forever
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
  double rate() const { return static_cast<double>(opt_.rate); }
  unsigned period() const { return opt_.period; }
  uint64_t identify_frames() const { return identify_frames_; }
  uint64_t counter() const { return ring_.counter(); }

 private:
  static void* thread_entry(void* self);

  void wait_before_retry();
  void size_buffers();
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
  // in6 is modified in place: input gain is applied to it before anything else reads it.
  void process_block(uint64_t n, size_t frames, float* in6, float* out8);

  Control& ctl_;
  RingBuffer& ring_;
  EngineOptions opt_;
  Generators gen_;

  snd_pcm_t* capture_ = nullptr;
  snd_pcm_t* playback_ = nullptr;
  unsigned cap_ch_ = 0;
  uint64_t identify_frames_ = 0;

  pthread_t thread_{};
  bool thread_valid_ = false;
  std::atomic<bool> running_{false};
  std::atomic<bool> streaming_{false};  // true only while audio is actually flowing
  std::atomic<uint64_t> xruns_{0};
  std::atomic<uint32_t> generation_{0};
  std::string last_error_;

  std::vector<int32_t> raw_in_;
  std::vector<int32_t> raw_out_;
  std::vector<float> in6_;
  std::vector<float> out8_;
  std::vector<float> gen_sine_, gen_noise_, gen_ping_;

  std::vector<float> sim_delay_;
  size_t sim_delay_len_ = 0;
};

}  // namespace st
