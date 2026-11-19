#include "analysis.h"
#include "channel_layout.h"

#include <pocketfft_hdronly.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "util/log.h"

namespace st {

namespace {

constexpr double kTickHz = 10.0;
constexpr double kRmsWindowS = 0.1;
// The longest stretch one tick meters for the held peak: what a late tick has to catch up on.
constexpr double kMeterCatchUpS = 1.0;
constexpr unsigned kFundamentalBins = 10;  // Hann leakage skirt; a narrower notch invents THD
constexpr float kThdLowHz = 20.0f;
constexpr float kThdHighHz = 20000.0f;
constexpr float kSpecLowHz = 20.0f;

}  // namespace

Analysis::Analysis(const RingBuffer& ring, double rate)
    : ring_(ring),
      rate_(rate),
      env_col_frames_(st::env_column_frames(static_cast<unsigned>(rate))) {
  window_.resize(kSpectrumFft);
  for (unsigned i = 0; i < kSpectrumFft; ++i) {
    window_[i] = 0.5f * (1.0f - static_cast<float>(std::cos(kTwoPi * i / (kSpectrumFft - 1))));
  }
  scratch_.resize(kSpectrumFft);
  spec_.resize(kSpectrumFft / 2 + 1);
  power_.resize(kSpectrumFft / 2 + 1);

  const double bin_hz = rate_ / kSpectrumFft;
  const double hi = std::min(rate_ * 0.5, 40000.0);
  for (unsigned b = 0; b < kSpectrumBins; ++b) {
    const double f0 = kSpecLowHz * std::pow(hi / kSpecLowHz,
                                            static_cast<double>(b) / kSpectrumBins);
    const double f1 = kSpecLowHz * std::pow(hi / kSpecLowHz,
                                            static_cast<double>(b + 1) / kSpectrumBins);
    unsigned k0 = static_cast<unsigned>(std::floor(f0 / bin_hz));
    unsigned k1 = static_cast<unsigned>(std::ceil(f1 / bin_hz));
    k0 = std::max(1u, std::min<unsigned>(k0, kSpectrumFft / 2));
    k1 = std::max(k0 + 1, std::min<unsigned>(k1, kSpectrumFft / 2 + 1));
    bin_ranges_[b] = {k0, k1};
    // Geometric mid-band of [f0, f1] — the natural "center" of a log-spaced bin.
    bin_hz_[b] = static_cast<float>(
        kSpecLowHz * std::pow(hi / kSpecLowHz, (static_cast<double>(b) + 0.5) / kSpectrumBins));
  }

  for (auto& s : snap_.spectrum) s.assign(kSpectrumBins, kMinDb);
  const uint64_t hold = static_cast<uint64_t>(std::llround(kPeakHoldS * rate_));
  for (auto& h : peak_hold_) h = PeakHold(hold, env_col_frames_);
  env_buf_.resize(static_cast<size_t>(env_col_frames_) * st::channels().total());
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

void PeakHold::add(const float* x, size_t len, uint64_t first) {
  size_t i = 0;
  if (first < fed_) i = static_cast<size_t>(std::min<uint64_t>(fed_ - first, len));
  while (i < len) {
    // One slice: up to the next multiple of grain_, so slices sit still whatever the windows do.
    const uint64_t s0 = first + i;
    const size_t n = static_cast<size_t>(std::min<uint64_t>(grain_ - s0 % grain_, len - i));
    float peak = 0.0f;
    size_t at = 0;
    for (size_t k = 0; k < n; ++k) {
      const float a = std::fabs(x[i + k]);
      if (a > peak) {
        peak = a;
        at = k;
      }
    }
    // A newer peak at least as high outlasts every older one it is not below.
    while (!q_.empty() && q_.back().second <= peak) q_.pop_back();
    if (peak > 0.0f) q_.push_back({s0 + at, peak});
    i += n;
  }
  fed_ = std::max(fed_, first + len);
}

float PeakHold::value(uint64_t now) {
  while (!q_.empty() && now > q_.front().first && now - q_.front().first > hold_) q_.pop_front();
  return q_.empty() ? 0.0f : q_.front().second;
}

void Analysis::update_meters(unsigned ch, const float* buf, size_t len, size_t rms_len,
                             uint64_t now) {
  const float* win = buf + (len - rms_len);
  double sum2 = 0.0;
  float peak = 0.0f;
  for (size_t i = 0; i < rms_len; ++i) {
    const float v = win[i];
    sum2 += static_cast<double>(v) * v;
    peak = std::max(peak, std::fabs(v));
  }
  const float rms = static_cast<float>(std::sqrt(sum2 / static_cast<double>(rms_len)));

  // Every sample since the last tick (not only the 100 ms window: a late tick would skip some),
  // so a 1 ms transient is never missed and is held kPeakHoldS after its own sample.
  PeakHold& h = peak_hold_[ch];
  h.add(buf, len, now - len);
  const float held = h.value(now);
  float span_peak = peak;
  for (size_t i = 0; i < len - rms_len; ++i) span_peak = std::max(span_peak, std::fabs(buf[i]));

  ChannelMeters& m = snap_.meters[ch];
  m.rms_db = lin_to_db(rms);
  m.peak_db = lin_to_db(held);
  m.peak_now_db = lin_to_db(peak);
  if (lin_to_db(span_peak) > kGenieSoundThresholdDb) {
    m.last_loud = now;
    m.loud_seen = true;
  }
}

const char* tone_reason_name(ToneReason r) {
  switch (r) {
    case ToneReason::Silent: return "silent";
    case ToneReason::NotATone: return "not_a_tone";
    case ToneReason::None: break;
  }
  return nullptr;
}

ToneMetrics tone_from_power(const std::vector<float>& power, double bin_hz) {
  // Locate the dominant partial, then measure everything else in band against it.
  ToneMetrics tm;
  if (power.size() < 3 || bin_hz <= 0.0) return tm;
  const unsigned k_lo = std::max(1u, static_cast<unsigned>(kThdLowHz / bin_hz));
  const unsigned k_hi = std::min<unsigned>(static_cast<unsigned>(kThdHighHz / bin_hz),
                                           static_cast<unsigned>(power.size()) - 2);
  unsigned kp = 0;
  float pmax = 0.0f;
  for (unsigned k = k_lo; k <= k_hi; ++k) {
    if (power[k] > pmax) {
      pmax = power[k];
      kp = k;
    }
  }
  if (kp == 0) return tm;  // all zero: digital silence

  // Parabolic interpolation on the log-magnitude peak for sub-bin frequency accuracy.
  const float a = 10.0f * std::log10(std::max(power[kp - 1], 1e-20f));
  const float b = 10.0f * std::log10(std::max(power[kp], 1e-20f));
  const float c = 10.0f * std::log10(std::max(power[kp + 1], 1e-20f));
  const float denom = a - 2.0f * b + c;
  const float delta = denom != 0.0f ? 0.5f * (a - c) / denom : 0.0f;
  tm.peak_hz = static_cast<float>((kp + delta) * bin_hz);
  // The parabola's vertex: the bin straddling two FFT bins reads up to 1.4 dB (Hann) low otherwise.
  tm.peak_db = std::max(kMinDb, b - 0.25f * (a - c) * delta);
  if (tm.peak_db <= kToneFloorDb) return tm;  // reason stays Silent

  double fund = 0.0;
  double rest = 0.0;
  for (unsigned k = k_lo; k <= k_hi; ++k) {
    if (k + kFundamentalBins >= kp && k <= kp + kFundamentalBins) {
      fund += power[k];
    } else {
      rest += power[k];
    }
  }
  if (fund > 0.0) tm.thd_n_pct = 100.0f * static_cast<float>(std::sqrt(rest / fund));
  if (tm.thd_n_pct > kToneMaxThdNPct) {
    tm.reason = ToneReason::NotATone;
    return tm;
  }
  tm.valid = true;
  tm.reason = ToneReason::None;
  tm.freq_hz = tm.peak_hz;
  return tm;
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

  snap_.tone[ch] = tone_from_power(power_, rate_ / kSpectrumFft);
}

void Analysis::update_envelope(uint64_t now) {
  const unsigned frames = env_col_frames_;
  const uint64_t newest_col = now / frames;
  const uint64_t oldest_sample = ring_.oldest(now);
  const uint64_t oldest_col = (oldest_sample + frames - 1) / frames;
  if (env_col_ < oldest_col) env_col_ = oldest_col;

  while (env_col_ < newest_col) {
    const uint64_t start = env_col_ * frames;
    if (!ring_.read_interleaved(start, frames, env_buf_.data())) {
      env_col_ = ring_.oldest(ring_.counter()) / frames + 1;
      break;
    }
    EnvColumn col{};
    for (unsigned c = 0; c < st::channels().total(); ++c) {
      float lo = 1.0f, hi = -1.0f;
      for (unsigned i = 0; i < frames; ++i) {
        const float v = env_buf_[i * st::channels().total() + c];
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
  const size_t max_frames =
      std::max(rms_frames, static_cast<size_t>(kMeterCatchUpS * rate_));
  std::vector<float> mbuf(max_frames);
  std::vector<float> sbuf(kSpectrumFft);
  // Wait until the widest window fits: `now - rms_frames` below must not go negative.
  const uint64_t warmup = std::max<uint64_t>(kSpectrumFft, rms_frames);

  auto next = std::chrono::steady_clock::now();
  bool spectrum_tick = false;
  uint64_t prev = 0;  // the last tick's `now`: the meters cover every sample since

  while (running_.load()) {
    next += std::chrono::microseconds(static_cast<int64_t>(1e6 / kTickHz));
    std::this_thread::sleep_until(next);

    const uint64_t now = ring_.counter();
    if (now < warmup) continue;
    // The samples since the last tick, at least the 100 ms window, at most kMeterCatchUpS.
    const size_t span = static_cast<size_t>(std::clamp<uint64_t>(
        now > prev ? now - prev : 0, rms_frames, std::min<uint64_t>(max_frames, now)));
    prev = now;

    std::lock_guard<std::mutex> lock(m_);
    snap_.sample = now;

    for (unsigned ch = 0; ch < st::channels().total(); ++ch) {
      if (ring_.read_channel(now - span, span, ch, mbuf.data())) {
        update_meters(ch, mbuf.data(), span, rms_frames, now);
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
