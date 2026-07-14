#include "generators.h"

#include <algorithm>
#include <cmath>

#include "util/dsp.h"

namespace st {

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr float kAttackS = 0.001f;  // 1 ms linear attack on every ping
}  // namespace

PingShape ping_shape(PingVariant v) {
  switch (v) {
    case PingVariant::Bing: return {1000.0f, 0.060f, 0.020f};
    case PingVariant::Bong: return {440.0f, 0.250f, 0.080f};
    // Tick is the measurement stimulus, so it decays in well under one carrier ring-down.
    // A tone burst that rings for many cycles correlates almost as well one carrier period
    // away as it does at the true lag: measured peak-to-rival ratio is 1.3 at tau=2.5 ms
    // versus 4.2 at tau=0.4 ms. Short decay = broadband = a sharp, unambiguous peak.
    default: return {3000.0f, 0.005f, 0.0004f};
  }
}

void Generators::init(double rate) {
  rate_ = rate;
  sine_phase_ = 0.0;
  ping_scheduled_ = false;
  burst_active_ = false;
}

float Generators::white() {
  // xorshift64*; take the top 24 bits so the value maps exactly onto a float mantissa.
  prng_ ^= prng_ >> 12;
  prng_ ^= prng_ << 25;
  prng_ ^= prng_ >> 27;
  const uint64_t x = prng_ * 0x2545f4914f6cdd1dull;
  return static_cast<float>(x >> 40) * (1.0f / 8388608.0f) - 1.0f;  // [-1, 1)
}

void Generators::render_sine(size_t frames, float freq_hz, float amp, float* out) {
  const double step = kTwoPi * static_cast<double>(freq_hz) / rate_;
  for (size_t i = 0; i < frames; ++i) {
    out[i] = amp * static_cast<float>(std::sin(sine_phase_));
    sine_phase_ += step;
    if (sine_phase_ >= kTwoPi) sine_phase_ -= kTwoPi;
  }
}

void Generators::render_noise(size_t frames, NoiseMode mode, float amp, float* out) {
  if (mode == NoiseMode::White) {
    for (size_t i = 0; i < frames; ++i) out[i] = amp * white();
    return;
  }
  // Paul Kellett's pink filter. The coefficients are tuned for 44.1 kHz; at 96 kHz the
  // corners shift up, tilting the slope by a couple of dB across the audio band.
  float* b = pink_;
  for (size_t i = 0; i < frames; ++i) {
    const float w = white();
    b[0] = 0.99886f * b[0] + w * 0.0555179f;
    b[1] = 0.99332f * b[1] + w * 0.0750759f;
    b[2] = 0.96900f * b[2] + w * 0.1538520f;
    b[3] = 0.86650f * b[3] + w * 0.3104856f;
    b[4] = 0.55000f * b[4] + w * 0.5329522f;
    b[5] = -0.7616f * b[5] - w * 0.0168980f;
    const float pink = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f) * 0.11f;
    b[6] = w * 0.115926f;
    out[i] = amp * pink;
  }
}

void Generators::render_ping(uint64_t n, size_t frames, const Control& ctl, float* out,
                             PingLog& log) {
  std::fill(out, out + frames, 0.0f);

  const uint32_t epoch = ctl.ping.epoch.load(std::memory_order_relaxed);
  const double interval_s = std::clamp(ctl.ping.interval_s.load(std::memory_order_relaxed), 0.5f,
                                       60.0f);
  const uint64_t interval = static_cast<uint64_t>(std::llround(interval_s * rate_));

  if (!ping_scheduled_ || epoch != ping_epoch_) {
    ping_epoch_ = epoch;
    ping_scheduled_ = true;
    next_ping_ = n + interval;
  }

  // Block-granular scheduling: a burst fires at its exact in-block offset. Testing
  // `n % interval == 0` at block starts would almost never be true.
  for (size_t i = 0; i < frames; ++i) {
    const uint64_t t = n + i;

    if (!burst_active_ && t >= next_ping_) {
      const auto variant = static_cast<PingVariant>(ctl.ping.variant.load(std::memory_order_relaxed));
      burst_shape_ = ping_shape(variant);
      burst_amp_ = db_to_lin(ctl.ping.level_db.load(std::memory_order_relaxed));
      burst_active_ = true;
      burst_start_ = t;
      log.push(t, static_cast<uint8_t>(variant));
      next_ping_ = t + interval;
    }

    if (burst_active_) {
      const double el = static_cast<double>(t - burst_start_) / rate_;
      if (el >= burst_shape_.duration_s) {
        burst_active_ = false;
      } else {
        const float env = el < kAttackS
                              ? static_cast<float>(el / kAttackS)
                              : std::exp(-static_cast<float>(el - kAttackS) / burst_shape_.tau_s);
        out[i] = burst_amp_ * env *
                 static_cast<float>(std::sin(kTwoPi * burst_shape_.freq_hz * el));
      }
    }
  }
}

void Generators::render(uint64_t n, size_t frames, const Control& ctl, float* sine, float* noise,
                        float* ping, PingLog& log) {
  const float sine_freq = std::clamp(ctl.sine.freq_hz.load(std::memory_order_relaxed), 1.0f,
                                     static_cast<float>(rate_ * 0.45));
  render_sine(frames, sine_freq, db_to_lin(ctl.sine.level_db.load(std::memory_order_relaxed)), sine);
  render_noise(frames, static_cast<NoiseMode>(ctl.noise.mode.load(std::memory_order_relaxed)),
               db_to_lin(ctl.noise.level_db.load(std::memory_order_relaxed)), noise);
  render_ping(n, frames, ctl, ping, log);
}

float Generators::identify_sample(uint64_t elapsed) const {
  // 3 x 100 ms of 1 kHz separated by 100 ms of silence.
  const double t = static_cast<double>(elapsed) / rate_;
  const double slot = std::fmod(t, 0.2);
  if (t >= 0.5 || slot >= 0.1) return 0.0f;
  return 0.35f * static_cast<float>(std::sin(kTwoPi * 1000.0 * t));
}

}  // namespace st
