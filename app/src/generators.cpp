#include "generators.h"

#include <algorithm>
#include <cmath>

#include "util/dsp.h"

namespace st {

namespace {

constexpr float kAttackS = 0.001f;  // 1 ms linear attack on every ping

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

// ---- The melody --------------------------------------------------------------------------------
//
// The theme of Beethoven's "Ode to Joy" (public domain), both phrases, so the loop ends on the
// tonic and comes round again without sounding cut off: eight bars of 4/4 at 150 bpm, 12.8 s.
// A plucked, music-box voice for the tune over a root note per bar.
//
// Everything is on a sixteenth-note grid, so every note boundary and the loop length are whole
// numbers of frames at any sane rate, and the loop is exactly periodic rather than drifting a
// fraction of a sample per pass.
constexpr double kMusicSixteenthS = 0.1;  // 150 bpm
constexpr unsigned kMusicLoopSixteenths = 128;
constexpr double kMusicAttackS = 0.005;
// Every note fades to exactly zero by the end of its slot. That is what keeps the loop seam and
// every repeated note click-free: nothing is ever cut off mid-waveform.
constexpr double kMusicReleaseS = 0.030;
constexpr float kMusicH2 = 0.25f;  // second harmonic, for a less glassy tone
// sin(x) + 0.25 sin(2x) peaks at 1.101, and the voice weights below sum to 1, so this keeps the
// bus peak at or below the level asked for.
constexpr float kMusicNorm = 1.0f / 1.11f;

struct MusicEvent {
  uint8_t start;  // sixteenths from the top of the loop
  uint8_t len;    // sixteenths
  uint8_t midi;
  bool bass;
};

constexpr MusicEvent kMusicScore[] = {
    // Melody, an octave up (C5 = 72) so it carries on a small speaker.
    {0, 4, 76, false},   {4, 4, 76, false},   {8, 4, 77, false},   {12, 4, 79, false},   // E E F G
    {16, 4, 79, false},  {20, 4, 77, false},  {24, 4, 76, false},  {28, 4, 74, false},   // G F E D
    {32, 4, 72, false},  {36, 4, 72, false},  {40, 4, 74, false},  {44, 4, 76, false},   // C C D E
    {48, 6, 76, false},  {54, 2, 74, false},  {56, 8, 74, false},                        // E. D D
    {64, 4, 76, false},  {68, 4, 76, false},  {72, 4, 77, false},  {76, 4, 79, false},   // E E F G
    {80, 4, 79, false},  {84, 4, 77, false},  {88, 4, 76, false},  {92, 4, 74, false},   // G F E D
    {96, 4, 72, false},  {100, 4, 72, false}, {104, 4, 74, false}, {108, 4, 76, false},  // C C D E
    {112, 6, 74, false}, {118, 2, 72, false}, {120, 8, 72, false},                       // D. C C
    // Bass: the root of each bar, tonic and dominant, resolving home in the last half bar.
    {0, 16, 48, true},   {16, 16, 43, true},  {32, 16, 48, true},  {48, 16, 43, true},
    {64, 16, 48, true},  {80, 16, 43, true},  {96, 16, 48, true},
    {112, 8, 43, true},  {120, 8, 48, true},
};

constexpr float kMusicMelodyGain = 0.65f;
constexpr float kMusicBassGain = 0.35f;
constexpr double kMusicMelodyTauS = 0.6;
constexpr double kMusicBassTauS = 1.0;

}  // namespace

void Generators::init(double rate) {
  rate_ = rate;
  sine_phase_ = 0.0;
  ping_scheduled_ = false;
  burst_active_ = false;

  const uint64_t sixteenth = static_cast<uint64_t>(std::llround(kMusicSixteenthS * rate));
  music_loop_ = std::max<uint64_t>(1, sixteenth * kMusicLoopSixteenths);
  music_attack_frames_ = static_cast<float>(std::max(1.0, kMusicAttackS * rate));
  music_release_frames_ = static_cast<float>(std::max(1.0, kMusicReleaseS * rate));
  music_.clear();
  for (const MusicEvent& ev : kMusicScore) {
    MusicNote m;
    m.start = ev.start * sixteenth;
    m.len = ev.len * sixteenth;
    m.cyc_per_frame = 440.0 * std::pow(2.0, (ev.midi - 69) / 12.0) / rate;
    m.gain = ev.bass ? kMusicBassGain : kMusicMelodyGain;
    m.inv_tau_frames =
        static_cast<float>(1.0 / ((ev.bass ? kMusicBassTauS : kMusicMelodyTauS) * rate));
    music_.push_back(m);
  }
}

float Generators::white() { return xorshift_white(prng_); }

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
                             PingLog& log, uint64_t log_offset) {
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
      log.push(t + log_offset, static_cast<uint8_t>(variant));
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

// One voice's sample `k` frames into note `e`. Computed from note-local time alone, never from an
// accumulated phase: that is what makes the bus a pure function of the absolute index.
float Generators::music_voice(const MusicNote& e, uint64_t k) const {
  const float kf = static_cast<float>(k);
  const float attack = std::min(1.0f, kf / music_attack_frames_);
  // Reaches exactly zero on the note's last frame.
  const float release =
      std::min(1.0f, static_cast<float>(e.len - 1 - k) / music_release_frames_);
  const float env = attack * release * std::exp(-kf * e.inv_tau_frames);
  // Phase in cycles, reduced in double before it becomes a float angle, so a long note stays in
  // tune to the last sample.
  const double cyc = static_cast<double>(k) * e.cyc_per_frame;
  const float ph = static_cast<float>(kTwoPi * (cyc - std::floor(cyc)));
  return e.gain * env * (std::sin(ph) + kMusicH2 * std::sin(2.0f * ph));
}

void Generators::render_music(uint64_t n, size_t frames, float amp, float* out) const {
  std::fill(out, out + frames, 0.0f);
  // One 64-bit modulo per block, not per sample: on 32-bit ARM it is a library call.
  uint64_t pos = n % music_loop_;
  size_t done = 0;
  while (done < frames) {
    // Split at the loop seam, so every sample is computed at its own place in the loop.
    const size_t chunk =
        static_cast<size_t>(std::min<uint64_t>(frames - done, music_loop_ - pos));
    const uint64_t end = pos + chunk;
    for (const MusicNote& e : music_) {
      const uint64_t a = std::max(pos, e.start);
      const uint64_t b = std::min(end, e.start + e.len);
      for (uint64_t p = a; p < b; ++p) out[done + (p - pos)] += music_voice(e, p - e.start);
    }
    done += chunk;
    pos = 0;
  }
  const float scale = amp * kMusicNorm;
  for (size_t i = 0; i < frames; ++i) out[i] *= scale;
}

void Generators::render(uint64_t n, size_t frames, const Control& ctl, float* sine, float* noise,
                        float* ping, float* music, PingLog& log, uint64_t log_offset) {
  const float sine_freq = std::clamp(ctl.sine.freq_hz.load(std::memory_order_relaxed), 1.0f,
                                     static_cast<float>(rate_ * 0.45));
  render_sine(frames, sine_freq, db_to_lin(ctl.sine.level_db.load(std::memory_order_relaxed)), sine);
  render_noise(frames, static_cast<NoiseMode>(ctl.noise.mode.load(std::memory_order_relaxed)),
               db_to_lin(ctl.noise.level_db.load(std::memory_order_relaxed)), noise);
  render_ping(n, frames, ctl, ping, log, log_offset);
  if (music) {
    render_music(n, frames, db_to_lin(ctl.music.level_db.load(std::memory_order_relaxed)), music);
  }
}

float Generators::identify_sample(uint64_t elapsed) const {
  const double t = static_cast<double>(elapsed) / rate_;
  const double slot = std::fmod(t, 2.0 * kIdentifyBurstS);
  if (t >= kIdentifySeconds || slot >= kIdentifyBurstS) return 0.0f;
  return 0.35f * static_cast<float>(std::sin(kTwoPi * 1000.0 * t));
}

}  // namespace st
