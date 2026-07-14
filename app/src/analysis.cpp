#include "analysis.h"

#include <pocketfft_hdronly.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "util/log.h"

namespace st {

namespace {

constexpr double kTickHz = 10.0;
constexpr double kRmsWindowS = 0.1;
constexpr double kPeakHoldS = 3.0;
constexpr float kToneThresholdDb = -40.0f;
constexpr unsigned kFundamentalBins = 10;  // Hann leakage skirt; a narrower notch invents THD
constexpr float kThdLowHz = 20.0f;
constexpr float kThdHighHz = 20000.0f;
constexpr float kSpecLowHz = 20.0f;

}  // namespace

Analysis::Analysis(const RingBuffer& ring, double rate) : ring_(ring), rate_(rate) {
  window_.resize(kSpectrumFft);
  for (unsigned i = 0; i < kSpectrumFft; ++i) {
    window_[i] = 0.5f * (1.0f - std::cos(2.0f * 3.14159265358979f * static_cast<float>(i) /
                                         static_cast<float>(kSpectrumFft - 1)));
  }
  scratch_.resize(kSpectrumFft);
  spec_.resize(kSpectrumFft / 2 + 1);
  power_.resize(kSpectrumFft / 2 + 1);

  const double bin_hz = rate_ / kSpectrumFft;
  const double hi = std::min(rate_ * 0.5, 40000.0);
  bin_freqs_.resize(kSpectrumBins);
  for (unsigned b = 0; b < kSpectrumBins; ++b) {
    const double f0 = kSpecLowHz * std::pow(hi / kSpecLowHz,
                                            static_cast<double>(b) / kSpectrumBins);
    const double f1 = kSpecLowHz * std::pow(hi / kSpecLowHz,
                                            static_cast<double>(b + 1) / kSpectrumBins);
    bin_freqs_[b] = static_cast<float>(std::sqrt(f0 * f1));
    unsigned k0 = static_cast<unsigned>(std::floor(f0 / bin_hz));
    unsigned k1 = static_cast<unsigned>(std::ceil(f1 / bin_hz));
    k0 = std::max(1u, std::min<unsigned>(k0, kSpectrumFft / 2));
    k1 = std::max(k0 + 1, std::min<unsigned>(k1, kSpectrumFft / 2 + 1));
    bin_ranges_[b] = {k0, k1};
  }

  for (auto& s : snap_.spectrum) s.assign(kSpectrumBins, kMinDb);
  env_buf_.resize(kEnvColumnFrames * kInputs);
}

Analysis::~Analysis() { stop(); }

void Analysis::start() {
  running_.store(true);
  thread_ = std::thread([this] { run(); });
}

void Analysis::stop() {
  running_.store(false);
  if (thread_.joinable()) thread_.join();
}

AnalysisSnapshot Analysis::snapshot() const {
  std::lock_guard<std::mutex> lock(m_);
  return snap_;
}

void Analysis::update_meters(unsigned ch, const float* buf, size_t len, uint64_t now) {
  double sum2 = 0.0;
  double sum = 0.0;
  float peak = 0.0f;
  for (size_t i = 0; i < len; ++i) {
    const float v = buf[i];
    sum2 += static_cast<double>(v) * v;
    sum += v;
    peak = std::max(peak, std::fabs(v));
  }
  const float rms = static_cast<float>(std::sqrt(sum2 / static_cast<double>(len)));

  const uint64_t hold = static_cast<uint64_t>(kPeakHoldS * rate_);
  if (peak >= peak_hold_[ch] || now - peak_time_[ch] > hold) {
    peak_hold_[ch] = peak;
    peak_time_[ch] = now;
  }

  snap_.meters[ch].rms_db = lin_to_db(rms);
  snap_.meters[ch].peak_db = lin_to_db(peak_hold_[ch]);
  snap_.meters[ch].dc = static_cast<float>(sum / static_cast<double>(len));
}

void Analysis::update_spectrum(unsigned ch, const float* buf) {
  for (unsigned i = 0; i < kSpectrumFft; ++i) scratch_[i] = buf[i] * window_[i];

  const pocketfft::shape_t shape{kSpectrumFft};
  const pocketfft::stride_t stride_in{static_cast<ptrdiff_t>(sizeof(float))};
  const pocketfft::stride_t stride_out{static_cast<ptrdiff_t>(sizeof(std::complex<float>))};
  pocketfft::r2c(shape, stride_in, stride_out, 0, pocketfft::FORWARD, scratch_.data(),
                 spec_.data(), 1.0f);

  // Coherent gain of a Hann window is 0.5; scale so a full-scale sine reads 0 dBFS.
  const float norm = 2.0f / (0.5f * static_cast<float>(kSpectrumFft));
  for (size_t k = 0; k < power_.size(); ++k) {
    const float mag = std::abs(spec_[k]) * norm;
    power_[k] = mag * mag;
  }

  std::vector<float>& out = snap_.spectrum[ch];
  out.assign(kSpectrumBins, kMinDb);
  for (unsigned b = 0; b < kSpectrumBins; ++b) {
    const auto [k0, k1] = bin_ranges_[b];
    float peak = 0.0f;
    for (unsigned k = k0; k < k1 && k < power_.size(); ++k) peak = std::max(peak, power_[k]);
    out[b] = peak > 0.0f ? std::max(kMinDb, 10.0f * std::log10(peak)) : kMinDb;
  }

  // Tone metrics: locate the dominant partial, then measure everything else in band.
  ToneMetrics tm;
  const double bin_hz = rate_ / kSpectrumFft;
  const unsigned k_lo = std::max(1u, static_cast<unsigned>(kThdLowHz / bin_hz));
  const unsigned k_hi = std::min<unsigned>(static_cast<unsigned>(kThdHighHz / bin_hz),
                                           static_cast<unsigned>(power_.size()) - 1);
  unsigned kp = 0;
  float pmax = 0.0f;
  for (unsigned k = k_lo; k <= k_hi; ++k) {
    if (power_[k] > pmax) {
      pmax = power_[k];
      kp = k;
    }
  }

  if (kp > 0 && 10.0f * std::log10(std::max(pmax, 1e-20f)) > kToneThresholdDb) {
    // Parabolic interpolation on the log-magnitude peak for sub-bin frequency accuracy.
    const float a = 10.0f * std::log10(std::max(power_[kp - 1], 1e-20f));
    const float b = 10.0f * std::log10(std::max(power_[kp], 1e-20f));
    const float c = 10.0f * std::log10(std::max(power_[kp + 1], 1e-20f));
    const float denom = a - 2.0f * b + c;
    const float delta = denom != 0.0f ? 0.5f * (a - c) / denom : 0.0f;

    double fund = 0.0;
    double rest = 0.0;
    for (unsigned k = k_lo; k <= k_hi; ++k) {
      if (k + kFundamentalBins >= kp && k <= kp + kFundamentalBins) {
        fund += power_[k];
      } else {
        rest += power_[k];
      }
    }

    tm.valid = true;
    tm.freq_hz = static_cast<float>((kp + delta) * bin_hz);
    tm.level_db = 10.0f * std::log10(std::max(fund, 1e-20));
    if (fund > 0.0) {
      const float ratio = static_cast<float>(std::sqrt(rest / fund));
      tm.thd_n_pct = 100.0f * ratio;
      tm.thd_n_db = 20.0f * std::log10(std::max(ratio, 1e-9f));
    }
  }
  snap_.tone[ch] = tm;
}

void Analysis::update_envelope(uint64_t now) {
  const uint64_t newest_col = now / kEnvColumnFrames;
  const uint64_t oldest_sample = ring_.oldest(now);
  const uint64_t oldest_col = (oldest_sample + kEnvColumnFrames - 1) / kEnvColumnFrames;
  if (env_col_ < oldest_col) env_col_ = oldest_col;

  while (env_col_ < newest_col) {
    const uint64_t start = env_col_ * kEnvColumnFrames;
    if (!ring_.read_interleaved(start, kEnvColumnFrames, env_buf_.data())) {
      env_col_ = ring_.oldest(ring_.counter()) / kEnvColumnFrames + 1;
      break;
    }
    EnvColumn col{};
    for (unsigned c = 0; c < kInputs; ++c) {
      float lo = 1.0f, hi = -1.0f;
      for (unsigned i = 0; i < kEnvColumnFrames; ++i) {
        const float v = env_buf_[i * kInputs + c];
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      }
      col.min[c] = float_to_s16(lo);
      col.max[c] = float_to_s16(hi);
    }
    env_.push(env_col_, col);
    ++env_col_;
  }
}

void Analysis::run() {
  const size_t rms_frames = static_cast<size_t>(kRmsWindowS * rate_);
  std::vector<float> mbuf(rms_frames);
  std::vector<float> sbuf(kSpectrumFft);

  auto next = std::chrono::steady_clock::now();
  bool spectrum_tick = false;

  while (running_.load()) {
    next += std::chrono::microseconds(static_cast<int64_t>(1e6 / kTickHz));
    std::this_thread::sleep_until(next);

    const uint64_t now = ring_.counter();
    if (now < kSpectrumFft) continue;

    std::lock_guard<std::mutex> lock(m_);
    snap_.sample = now;

    for (unsigned ch = 0; ch < kInputs; ++ch) {
      if (ring_.read_channel(now - rms_frames, rms_frames, ch, mbuf.data())) {
        update_meters(ch, mbuf.data(), rms_frames, now);
      }
      // Spectra are the expensive part; half rate is plenty for a 5 Hz display push.
      if (spectrum_tick && ring_.read_channel(now - kSpectrumFft, kSpectrumFft, ch, sbuf.data())) {
        update_spectrum(ch, sbuf.data());
      }
    }
    spectrum_tick = !spectrum_tick;

    update_envelope(now);
  }
}

}  // namespace st
