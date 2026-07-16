#pragma once

#include <cstdint>

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

  // Renders `frames` samples starting at absolute sample index `n` into the three
  // generator buses. Emissions are appended to `log`.
  void render(uint64_t n, size_t frames, const Control& ctl, float* sine, float* noise,
              float* ping, PingLog& log);

  // Renders the Identify pattern (short 1 kHz bursts) sample by sample.
  float identify_sample(uint64_t elapsed) const;

 private:
  void render_sine(size_t frames, float freq_hz, float amp, float* out);
  void render_noise(size_t frames, NoiseMode mode, float amp, float* out);
  void render_ping(uint64_t n, size_t frames, const Control& ctl, float* out, PingLog& log);

  float white();

  double rate_ = 96000.0;
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
