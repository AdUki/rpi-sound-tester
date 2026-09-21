#pragma once

#include <cstdint>
#include <vector>

#include "control.h"

namespace st {

struct PingShape {
  float freq_hz;
  float duration_s;
  float tau_s;  // exponential decay time constant
};

// The per-output Identify pattern: kIdentifyBursts tone bursts of kIdentifyBurstS each,
// separated by equally long gaps. identify_sample() renders it; AudioEngine derives the
// total override length (identify_frames_) from kIdentifySeconds.
inline constexpr double kIdentifyBurstS = 0.1;
inline constexpr unsigned kIdentifyBursts = 3;
inline constexpr double kIdentifySeconds = (2 * kIdentifyBursts - 1) * kIdentifyBurstS;

// Owned exclusively by the audio thread. All timing derives from the absolute sample
// counter, so generated audio and captured audio share one time axis.
class Generators {
 public:
  void init(double rate);

  // Renders `frames` samples starting at absolute sample index `n` into the generator buses.
  // Emissions are appended to `log`, offset by `log_offset`: outputs play at n but capture may be
  // held back, and the log has to name where a ping will be SEEN in the ring, which is what every
  // consumer of it (scope markers, genie/sync) actually wants.
  //
  // `music` may be null, which skips it: the melody is a pure function of n, so a block nobody
  // listens to can be left out without the next one noticing.
  void render(uint64_t n, size_t frames, const Control& ctl, float* sine, float* noise,
              float* ping, float* music, PingLog& log, uint64_t log_offset = 0);

  // Renders the Identify pattern (short 1 kHz bursts) sample by sample.
  float identify_sample(uint64_t elapsed) const;

  // The melody bus on its own, at linear amplitude `amp` (its peak). A pure function of the
  // absolute index: sample n is the same whatever the block size and whenever rendering began,
  // and it repeats exactly every music_loop_frames().
  void render_music(uint64_t n, size_t frames, float amp, float* out) const;
  uint64_t music_loop_frames() const { return music_loop_; }

 private:
  void render_sine(size_t frames, float freq_hz, float amp, float* out);
  void render_noise(size_t frames, NoiseMode mode, float amp, float* out);
  void render_ping(uint64_t n, size_t frames, const Control& ctl, float* out, PingLog& log,
                   uint64_t log_offset);

  float white();

  // One note of the melody, expanded to frames for the current rate by init().
  struct MusicNote {
    uint64_t start;       // frame within the loop
    uint64_t len;         // frames; the note is silent again by start + len
    double cyc_per_frame; // carrier frequency / rate
    float gain;           // voice weight
    float inv_tau_frames; // 1 / decay time constant, in frames
  };
  float music_voice(const MusicNote& e, uint64_t k) const;

  double rate_ = 96000.0;

  std::vector<MusicNote> music_;
  uint64_t music_loop_ = 1;
  float music_attack_frames_ = 1.0f;
  float music_release_frames_ = 1.0f;

  double sine_phase_ = 0.0;

  uint64_t prng_ = 0x853c49e6748fea9bull;
  float pink_[7] = {0, 0, 0, 0, 0, 0, 0};

  uint64_t next_ping_ = 0;
  uint32_t ping_epoch_ = 0;
  bool ping_scheduled_ = false;

  bool burst_active_ = false;
  uint64_t burst_start_ = 0;
  PingShape burst_shape_{};
  float burst_amp_ = 0.0f;
};

}  // namespace st
