#include "capture.h"

#include <pocketfft_hdronly.h>
#include <sys/mman.h>

#include <algorithm>
#include <cmath>

#include "util/log.h"

namespace st {

namespace {

size_t next_pow2(size_t n) {
  size_t p = 1;
  while (p < n) p <<= 1;
  return p;
}

}  // namespace

CaptureStore::CaptureStore(const RingBuffer& ring, double rate, unsigned period)
    : ring_(ring), rate_(rate), period_(period) {
  // The copy margin scales with the ring: a fixed period count is fine at 24 MB but far too
  // thin once a 192 MB memcpy needs room to finish before the writer laps its start.
  const size_t margin = std::max<size_t>(kFreezeHeadroomPeriods * period_, kRingFrames / 32);
  max_frames_ = kRingFrames - margin;
  // A freeze can never copy more than max_frames_, so the margin's worth of snapshot memory
  // would be pinned for nothing.
  snap_.assign(max_frames_ * kInputs, 0.0f);
  mlock(snap_.data(), snap_.size() * sizeof(float));
  // Default to a modest window, capped at the maximum, so a fresh freeze is quick out of the box.
  analyze_frames_ =
      std::min<uint64_t>(max_frames_, static_cast<uint64_t>(kCaptureDefaultSeconds * rate_));
}

void CaptureStore::set_analyze_frames(uint64_t frames) {
  std::lock_guard<std::mutex> lock(m_);
  analyze_frames_ = std::clamp<uint64_t>(frames, kCaptureMinFrames, max_frames_);
}

uint64_t CaptureStore::analyze_frames() const {
  std::lock_guard<std::mutex> lock(m_);
  return analyze_frames_;
}

CaptureStatus CaptureStore::freeze(uint32_t generation) {
  std::lock_guard<std::mutex> lock(m_);

  // Copy at most the configured analyze length, and never more than the ring can safely hand
  // out (max_frames_ keeps a margin below the write head so the copy finishes before the
  // writer laps the oldest sample). The ring's post-copy validation is the backstop, and a
  // lapped copy is discarded, not handed out as mixed data.
  const size_t span = static_cast<size_t>(std::min<uint64_t>(analyze_frames_, max_frames_));

  for (int attempt = 0; attempt < 2; ++attempt) {
    const uint64_t n1 = ring_.counter();
    const uint64_t start = n1 > span ? n1 - span : 0;
    const uint64_t len = n1 - start;
    if (len == 0) break;

    if (!ring_.read_interleaved(start, static_cast<size_t>(len), snap_.data())) {
      LOG_WARN("freeze: writer lapped the snapshot mid-copy, retrying");
      continue;
    }

    status_.frozen = true;
    status_.freeze_sample = n1;
    status_.valid_start = start;
    status_.valid_len = len;
    status_.generation = generation;

    LOG_INFO("freeze at sample {}: {} frames ({:.2f} s) valid", n1, len,
             static_cast<double>(len) / rate_);
    return status_;
  }

  status_ = CaptureStatus{};
  return status_;
}

void CaptureStore::resume() {
  std::lock_guard<std::mutex> lock(m_);
  status_ = CaptureStatus{};
}

CaptureStatus CaptureStore::status() const {
  std::lock_guard<std::mutex> lock(m_);
  return status_;
}

bool CaptureStore::snapshot_read(unsigned ch, uint64_t start, uint64_t len, float* out) const {
  if (!status_.frozen || ch >= kInputs) return false;
  if (start < status_.valid_start) return false;
  const uint64_t off = start - status_.valid_start;
  // off > valid_len - len is the overflow-safe form of start + len > valid_start + valid_len:
  // a huge start from the query string must fail here, not wrap into a "valid" offset.
  if (len > status_.valid_len || off > status_.valid_len - len) return false;
  for (uint64_t i = 0; i < len; ++i) out[i] = snap_[(off + i) * kInputs + ch];
  return true;
}

std::string CaptureStore::frozen_range_error() const {
  return "outside the frozen range [" + std::to_string(status_.valid_start) + ", " +
         std::to_string(status_.valid_start + status_.valid_len) + ")";
}

WindowResult CaptureStore::window(unsigned ch, uint64_t start, uint64_t len, unsigned cols) const {
  WindowResult r;
  if (ch >= kInputs) {
    r.error = "channel out of range";
    return r;
  }
  if (len == 0 || len > kRingFrames) {
    r.error = "len out of range";
    return r;
  }
  cols = std::clamp(cols, 1u, 2048u);

  std::vector<float> buf(static_cast<size_t>(len));
  {
    std::lock_guard<std::mutex> lock(m_);
    if (status_.frozen) {
      if (!snapshot_read(ch, start, len, buf.data())) {
        r.error = frozen_range_error();
        return r;
      }
    } else if (!ring_.read_channel(start, static_cast<size_t>(len), ch, buf.data())) {
      const uint64_t now = ring_.counter();
      r.error = "outside the live ring [" + std::to_string(ring_.oldest(now)) + ", " +
                std::to_string(now) + ")";
      return r;
    }
  }

  r.ok = true;
  r.start = start;
  r.len = len;

  if (len <= 2ull * cols) {
    r.raw = true;
    r.samples = std::move(buf);
    return r;
  }

  r.mins.resize(cols);
  r.maxs.resize(cols);
  for (unsigned c = 0; c < cols; ++c) {
    const size_t i0 = static_cast<size_t>(static_cast<double>(len) * c / cols);
    const size_t i1 = static_cast<size_t>(static_cast<double>(len) * (c + 1) / cols);
    float lo = buf[i0], hi = buf[i0];
    for (size_t i = i0; i < i1 && i < buf.size(); ++i) {
      lo = std::min(lo, buf[i]);
      hi = std::max(hi, buf[i]);
    }
    r.mins[c] = lo;
    r.maxs[c] = hi;
  }
  return r;
}

XcorrResult CaptureStore::xcorr(unsigned ch_a, unsigned ch_b, uint64_t start, uint64_t len) {
  XcorrResult r;
  if (ch_a >= kInputs || ch_b >= kInputs) {
    r.error = "channel out of range";
    return r;
  }
  if (len < 64 || len > kXcorrMaxLen) {
    r.error = "len must be between 64 and " + std::to_string(kXcorrMaxLen);
    return r;
  }

  std::lock_guard<std::mutex> fft_lock(fft_m_);

  // Zero-padding to >= 2*len makes the circular correlation a linear one; without it every
  // lag aliases modulo N and the answer is quietly wrong.
  const size_t n = next_pow2(2 * static_cast<size_t>(len));
  if (n > kXcorrMaxFft) {
    r.error = "window too large";
    return r;
  }

  fa_.assign(n, 0.0f);
  fb_.assign(n, 0.0f);
  corr_.assign(n, 0.0f);
  env_.assign(n, 0.0f);
  ca_.assign(n / 2 + 1, {});
  cb_.assign(n / 2 + 1, {});
  analytic_.assign(n, {});

  {
    std::lock_guard<std::mutex> lock(m_);
    if (!status_.frozen) {
      r.error = "freeze the capture first";
      return r;
    }
    if (!snapshot_read(ch_a, start, len, fa_.data()) ||
        !snapshot_read(ch_b, start, len, fb_.data())) {
      r.error = frozen_range_error();
      return r;
    }
  }

  // Remove DC: a common offset would otherwise dominate the correlation.
  double ma = 0.0, mb = 0.0;
  for (size_t i = 0; i < len; ++i) {
    ma += fa_[i];
    mb += fb_[i];
  }
  ma /= static_cast<double>(len);
  mb /= static_cast<double>(len);
  double ea = 0.0, eb = 0.0;
  for (size_t i = 0; i < len; ++i) {
    fa_[i] -= static_cast<float>(ma);
    fb_[i] -= static_cast<float>(mb);
    ea += static_cast<double>(fa_[i]) * fa_[i];
    eb += static_cast<double>(fb_[i]) * fb_[i];
  }
  if (ea <= 0.0 || eb <= 0.0) {
    r.error = "one of the channels is silent";
    return r;
  }

  const pocketfft::shape_t shape{n};
  const pocketfft::stride_t sr{static_cast<ptrdiff_t>(sizeof(float))};
  const pocketfft::stride_t sc{static_cast<ptrdiff_t>(sizeof(std::complex<float>))};

  pocketfft::r2c(shape, sr, sc, 0, pocketfft::FORWARD, fa_.data(), ca_.data(), 1.0f);
  pocketfft::r2c(shape, sr, sc, 0, pocketfft::FORWARD, fb_.data(), cb_.data(), 1.0f);

  // conj(A)*B -> r[m] = sum_t a[t]*b[t+m], so a peak at m>0 means ch_b lags ch_a by m.
  for (size_t k = 0; k < ca_.size(); ++k) ca_[k] = std::conj(ca_[k]) * cb_[k];

  pocketfft::c2r(shape, sc, sr, 0, pocketfft::BACKWARD, ca_.data(), corr_.data(),
                 1.0f / static_cast<float>(n));

  // Lag and confidence are decided on the ENVELOPE of the correlation (the magnitude of its
  // analytic signal, which is carrier-free); only the final sample-exact lag is read from the
  // raw correlation, restricted to the winning lobe. A ringing tone (bing/bong) correlates as
  // an oscillating carrier under a slowly decaying envelope, whose crests one carrier cycle
  // from the true lag sit within a percent of the winner — working on the envelope keeps that
  // oscillation from being read as a separate feature by either the peak pick or the rival
  // search, so the lag stays on the right cycle and the confidence reflects real ambiguity.
  const auto wrap = [n](int64_t m) {
    return static_cast<size_t>((m + static_cast<int64_t>(n)) % static_cast<int64_t>(n));
  };

  // Envelope via the analytic signal: zero the negative frequencies of the correlation
  // spectrum (double the positive ones), inverse-transform, take the magnitude.
  analytic_[0] = ca_[0];
  for (size_t k = 1; k < n / 2; ++k) analytic_[k] = 2.0f * ca_[k];
  analytic_[n / 2] = ca_[n / 2];
  pocketfft::c2c(shape, sc, sc, {0}, pocketfft::BACKWARD, analytic_.data(), analytic_.data(),
                 1.0f / static_cast<float>(n));
  for (size_t i = 0; i < n; ++i) env_[i] = std::abs(analytic_[i]);

  const int64_t max_lag = static_cast<int64_t>(len) - 1;
  int64_t best_env = 0;
  float env_best = -1.0f;
  for (int64_t m = -max_lag; m <= max_lag; ++m) {
    const float v = env_[wrap(m)];
    if (v > env_best) {
      env_best = v;
      best_env = m;
    }
  }

  // The winning lobe: the contiguous span around the envelope peak still above half its
  // height. The reported lag is the tallest raw-correlation crest inside it — for a
  // broadband stimulus that is the envelope peak itself; for a ringing tone it is the
  // carrier crest nearest the envelope peak, i.e. the right cycle.
  int64_t lobe_lo = best_env, lobe_hi = best_env;
  while (lobe_lo > -max_lag && env_[wrap(lobe_lo - 1)] >= 0.5f * env_best) --lobe_lo;
  while (lobe_hi < max_lag && env_[wrap(lobe_hi + 1)] >= 0.5f * env_best) ++lobe_hi;
  int64_t best = best_env;
  float best_v = -1.0f;
  for (int64_t m = lobe_lo; m <= lobe_hi; ++m) {
    const float v = std::fabs(corr_[wrap(m)]);
    if (v > best_v) {
      best_v = v;
      best = m;
    }
  }

  // A continuous tone is the one case the envelope cannot resolve: its envelope is a single
  // window-wide lobe with no separate rival, yet its lag is only knowable modulo the carrier
  // period, so the confidence must still be held down. Detect it by a near-equal crest far
  // from the winner: a decaying burst's crests fall off exponentially with distance (bong,
  // the slowest, is ~5 % down two cycles out), a continuous tone's stay within a fraction of
  // a percent across the whole lobe. Crests within 1.5 carrier cycles of the winner are
  // excluded — the ping's 1 ms attack flattens the peak enough to lift its immediate
  // neighbours, and they don't distinguish the two cases anyway. The carrier period is read
  // off the dominant cross-spectrum bin (for a broadband stimulus that radius is tiny and the
  // lobe holds no distant crests, so the test cannot fire).
  size_t k_dom = 1;
  for (size_t k = 2; k < ca_.size(); ++k) {
    if (std::abs(ca_[k]) > std::abs(ca_[k_dom])) k_dom = k;
  }
  const int64_t exclude =
      static_cast<int64_t>(1.5 * static_cast<double>(n) / static_cast<double>(k_dom));
  float far_crest = 0.0f;
  for (int64_t m = lobe_lo; m <= lobe_hi; ++m) {
    if (std::llabs(m - best) <= exclude) continue;
    const float v = std::fabs(corr_[wrap(m)]);
    if (v > far_crest && v >= std::fabs(corr_[wrap(m - 1)]) &&
        v >= std::fabs(corr_[wrap(m + 1)])) {
      far_crest = v;
    }
  }
  const bool cycle_unresolved = far_crest > 0.985f * best_v;

  // Confidence: the envelope peak against the tallest genuinely separate rival lobe. A
  // rival must rise out of a real dip — walking outward from the peak, a sample only
  // counts by how far it climbs back above the deepest envelope value crossed on the way
  // (capped at its own height). The main lobe's own skirt climbs nowhere and scores ~0,
  // so a lone ping scores its noise floor (high ratio), while a stimulus that repeats in
  // the window puts up a near-equal lobe one interval away (ratio -> 1).
  float second = 0.0f;
  for (const int dir : {-1, +1}) {
    float dip = env_best;
    for (int64_t m = best_env + dir; m >= -max_lag && m <= max_lag; m += dir) {
      const float e = env_[wrap(m)];
      dip = std::min(dip, e);
      second = std::max(second, std::min(e, 2.0f * (e - dip)));
    }
  }

  r.ok = true;
  r.lag_samples = best;
  r.lag_ms = 1000.0 * static_cast<double>(best) / rate_;
  r.peak = best_v / std::sqrt(ea * eb);
  r.confidence = second > 0.0f ? env_best / second : 999.0;
  if (cycle_unresolved) {
    r.confidence =
        std::min(r.confidence, static_cast<double>(best_v) / static_cast<double>(far_crest));
  }
  r.confidence = std::min(r.confidence, 999.0);
  return r;
}

}  // namespace st
