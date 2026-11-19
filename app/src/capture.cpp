#include "capture.h"
#include "channel_layout.h"

#include <pocketfft_hdronly.h>
#include <sys/mman.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>

#include "util/log.h"
#include "util/sysinfo.h"

namespace st {

namespace {

size_t next_pow2(size_t n) {
  size_t p = 1;
  while (p < n) p <<= 1;
  return p;
}

// Never leave the box with less than this much room, whatever the operator asks for.
constexpr uint64_t kMemHeadroomBytes = 64ull * 1024 * 1024;

std::string mb_str(size_t floats) {
  const double mb = static_cast<double>(floats) * sizeof(float) / (1024.0 * 1024.0);
  char buf[32];
  snprintf(buf, sizeof(buf), "%.0f", mb);
  return buf;
}

// Fills cum[i] with the energy of x[0, i), for i = 0..len.
void running_energy(const float* x, size_t len, std::vector<double>& cum) {
  cum.resize(len + 1);
  cum[0] = 0.0;
  for (size_t i = 0; i < len; ++i) cum[i + 1] = cum[i] + static_cast<double>(x[i]) * x[i];
}

// Whether a signal's short-term level climbs out of a quieter stretch earlier in the window: any
// block more than kXcorrOnsetRiseDb above the quietest block before it. `cum` is its running
// energy (running_energy). A block is one period of the window's dominant partial (`period`, in
// samples) or longer — any periodic signal's energy over a block that long varies by at most
// 3 dB wherever it starts, so a steady tone, however rich in harmonics, never reads as one — and
// blocks step by a quarter of that, so an onset is seen once a period of quieter signal precedes
// it.
bool has_onset(const std::vector<double>& cum, double period) {
  const size_t len = cum.size() - 1;
  const size_t block = std::max<size_t>(32, static_cast<size_t>(std::ceil(period)));
  if (len < 2 * block) return false;
  const size_t hop = block / 4;
  const double rise = std::pow(10.0, kXcorrOnsetRiseDb / 10.0);
  double quietest = cum[block];
  for (size_t b = hop; b + block <= len; b += hop) {
    const double e = cum[b + block] - cum[b];
    if (e > rise * quietest) return true;
    quietest = std::min(quietest, e);
  }
  return false;
}

}  // namespace

CaptureStore::CaptureStore(const RingBuffer& ring, double rate, unsigned period)
    : ring_(ring), rate_(rate), period_(period) {
  // The copy margin scales with the ring: a fixed period count is fine at 24 MB but far too
  // thin once a 192 MB memcpy needs room to finish before the writer laps its start.
  const size_t margin = std::max<size_t>(kFreezeHeadroomPeriods * period_, ring.frames() / 32);
  max_frames_ = ring.frames() - margin;

  // The ring is not the only ceiling. With a dozen channels a full-length snapshot is larger than
  // a 1 GB board has, and because the buffer is written on allocation the kernel reaches for the
  // OOM killer long before `new` would ever throw — so bounding this only by the ring would put a
  // slider in the console that kills the daemon. A third of RAM leaves room for the ring itself,
  // for the transient where both the old and new buffers are resident during a resize, and for
  // everything else on the box.
  if (const uint64_t total = meminfo_bytes("MemTotal:")) {
    const uint64_t budget = total / 4;
    const uint64_t cap = budget / (st::channels().total() * sizeof(float));
    if (cap < max_frames_) {
      LOG_INFO("capture: analyze length capped at {:.0f} s by RAM ({} MB total), not by the ring",
               static_cast<double>(cap) / rate_, total / (1024 * 1024));
      max_frames_ = std::max<uint64_t>(cap, kCaptureMinFrames);
    }
  }
  // Default to a modest window, capped at the maximum, so a fresh freeze is quick out of the box.
  analyze_frames_ =
      std::min<uint64_t>(max_frames_, static_cast<uint64_t>(kCaptureDefaultSeconds * rate_));
  pending_frames_ = analyze_frames_;
  // The snapshot is sized to the analyze length, not to max_frames_. Pinning the ceiling here
  // regardless of the setting made the console's "longer means more RAM" promise a fiction and
  // cost ~150 MB that the default 10 s window never touches.
  std::string err;
  if (!resize_snapshot(analyze_frames_, &err)) {
    LOG_ERROR("capture: cannot pin the snapshot buffer: {}", err);
  }
}

bool CaptureStore::resize_snapshot(uint64_t frames, std::string* err) {
  const size_t want = static_cast<size_t>(frames) * st::channels().total();
  if (snap_.size() == want) return true;

  // The static cap above bounds what the console may offer; this bounds what is actually
  // attempted. They are not the same question — a resize holds the old buffer while it fills the
  // new one, so a request that fits the cap can still not fit the machine right now. Checking
  // first matters because assign() writes every page: the kernel would reach the OOM killer long
  // before new threw anything to catch.
  if (want > snap_.size()) {
    const uint64_t need = static_cast<uint64_t>(want - snap_.size()) * sizeof(float);
    const uint64_t avail = meminfo_bytes("MemAvailable:");
    if (avail != 0 && need + kMemHeadroomBytes > avail) {
      if (err) {
        *err = "not enough free memory for a " + mb_str(want) + " MB snapshot (" +
               std::to_string(avail / (1024 * 1024)) + " MB available)";
      }
      return false;
    }
  }

  std::vector<float> next;
  try {
    next.assign(want, 0.0f);
  } catch (const std::bad_alloc&) {
    if (err) *err = "not enough memory for a " + mb_str(want) + " MB snapshot";
    return false;
  }
  if (mlock(next.data(), next.size() * sizeof(float)) != 0) {
    if (err) *err = "cannot pin " + mb_str(want) + " MB — check RLIMIT_MEMLOCK";
    return false;
  }

  if (!snap_.empty()) munlock(snap_.data(), snap_.size() * sizeof(float));
  snap_ = std::move(next);
  // The old samples are gone with the old buffer; a stale "frozen" flag would hand out zeros.
  status_ = CaptureStatus{};
  LOG_INFO("capture: snapshot buffer now {} frames ({} MB pinned)", frames, mb_str(want));
  return true;
}

bool CaptureStore::set_analyze_frames(uint64_t frames, std::string* err) {
  std::lock_guard<std::mutex> lock(m_);
  const uint64_t want = std::clamp<uint64_t>(frames, kCaptureMinFrames, max_frames_);

  // A frozen snapshot is somebody's in-flight measurement: record the new size and apply it
  // on resume() or at the next freeze(), both of which discard the data anyway.
  if (status_.frozen) {
    analyze_frames_ = want;
    pending_frames_ = want;
    return true;
  }

  if (!resize_snapshot(want, err)) return false;
  analyze_frames_ = want;
  pending_frames_ = want;
  return true;
}

uint64_t CaptureStore::analyze_frames() const {
  std::lock_guard<std::mutex> lock(m_);
  return analyze_frames_;
}

uint64_t CaptureStore::pinned_bytes() const {
  std::lock_guard<std::mutex> lock(m_);
  return static_cast<uint64_t>(snap_.size()) * sizeof(float);
}

CaptureStatus CaptureStore::freeze(uint32_t generation) {
  std::lock_guard<std::mutex> lock(m_);

  // Copy at most the configured analyze length, and never more than the ring can safely hand
  // out (max_frames_ keeps a margin below the write head so the copy finishes before the
  // writer laps the oldest sample). The ring's post-copy validation is the backstop, and a
  // lapped copy is discarded, not handed out as mixed data.
  // Apply a resize deferred while the previous snapshot was frozen. A failure here keeps the
  // old buffer, so the freeze still succeeds — just at the old length.
  if (pending_frames_ != 0 && pending_frames_ * st::channels().total() != snap_.size()) {
    std::string err;
    if (!resize_snapshot(pending_frames_, &err)) {
      LOG_WARN("capture: keeping the {}-frame snapshot buffer: {}", snap_.size() / st::channels().total(), err);
    }
  }

  const size_t span = static_cast<size_t>(std::min<uint64_t>(analyze_frames_, max_frames_));
  if (span * st::channels().total() > snap_.size()) {
    LOG_WARN("capture: snapshot buffer holds fewer frames than requested — freezing short");
  }

  for (int attempt = 0; attempt < 2; ++attempt) {
    const uint64_t n1 = ring_.counter();
    const uint64_t cap = snap_.size() / st::channels().total();  // the buffer is the hard bound on the copy
    const uint64_t want = std::min<uint64_t>(span, cap);
    const uint64_t start = n1 > want ? n1 - want : 0;
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
  if (pending_frames_ != 0 && pending_frames_ * st::channels().total() != snap_.size()) {
    std::string err;
    if (!resize_snapshot(pending_frames_, &err)) LOG_WARN("capture: deferred resize failed: {}", err);
  }
}

CaptureStatus CaptureStore::status() const {
  std::lock_guard<std::mutex> lock(m_);
  return status_;
}

bool CaptureStore::snapshot_read(unsigned ch, uint64_t start, uint64_t len, float* out) const {
  if (!status_.frozen || ch >= st::channels().total()) return false;
  if (start < status_.valid_start) return false;
  const uint64_t off = start - status_.valid_start;
  // off > valid_len - len is the overflow-safe form of start + len > valid_start + valid_len:
  // a huge start from the query string must fail here, not wrap into a "valid" offset.
  if (len > status_.valid_len || off > status_.valid_len - len) return false;
  for (uint64_t i = 0; i < len; ++i) out[i] = snap_[(off + i) * st::channels().total() + ch];
  return true;
}

std::string CaptureStore::frozen_range_error() const {
  return "outside the frozen range [" + std::to_string(status_.valid_start) + ", " +
         std::to_string(status_.valid_start + status_.valid_len) + ")";
}

WindowResult CaptureStore::window(unsigned ch, uint64_t start, uint64_t len, unsigned cols) const {
  WindowResult r;
  if (ch >= st::channels().total()) {
    r.error = "ch must be 0.." + std::to_string(st::channels().total() - 1);
    return r;
  }
  if (len == 0 || len > ring_.span()) {
    r.error = "len must be 1.." + std::to_string(ring_.span()) + " (the ring)";
    return r;
  }
  // Refused rather than clamped: a caller asking for 4096 columns to get 8192 raw samples would
  // otherwise get min/max pairs back and read them as samples.
  if (cols == 0 || cols > kWindowMaxCols) {
    r.error = "cols must be 1.." + std::to_string(kWindowMaxCols) +
              " (raw needs len <= 2 x cols, so at most " + std::to_string(2 * kWindowMaxCols) +
              " samples per call)";
    return r;
  }
  // Which span was asked for, in front of the span there is: the error says both.
  const std::string asked =
      "[" + std::to_string(start) + ", " + std::to_string(start + len) + ") is ";

  std::vector<float> buf(static_cast<size_t>(len));
  {
    std::lock_guard<std::mutex> lock(m_);
    if (status_.frozen) {
      if (!snapshot_read(ch, start, len, buf.data())) {
        r.error = asked + frozen_range_error();
        return r;
      }
    } else if (!ring_.read_channel(start, static_cast<size_t>(len), ch, buf.data())) {
      const uint64_t now = ring_.counter();
      r.error = asked + "outside the live ring [" + std::to_string(ring_.oldest(now)) + ", " +
                std::to_string(now) + ")";
      return r;
    }
  }

  r.ok = true;
  r.start = start;
  r.len = len;
  r.cols = cols;

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

bool CaptureStore::snapshot_frames(uint64_t freeze_sample, uint64_t start, uint64_t len,
                                   float* out) const {
  std::lock_guard<std::mutex> lock(m_);
  if (!status_.frozen || status_.freeze_sample != freeze_sample || len == 0) return false;
  if (start < status_.valid_start) return false;
  const uint64_t off = start - status_.valid_start;
  if (len > status_.valid_len || off > status_.valid_len - len) return false;
  const size_t stride = st::channels().total();
  std::memcpy(out, snap_.data() + off * stride, static_cast<size_t>(len) * stride * sizeof(float));
  return true;
}

bool CaptureStore::rms_db(unsigned ch, uint64_t start, uint64_t len, double* out,
                          uint64_t freeze_sample) const {
  if (len == 0) return false;
  std::lock_guard<std::mutex> lock(m_);
  if (!status_.frozen || ch >= st::channels().total() || start < status_.valid_start) return false;
  if (freeze_sample != 0 && status_.freeze_sample != freeze_sample) return false;
  const uint64_t off = start - status_.valid_start;
  if (len > status_.valid_len || off > status_.valid_len - len) return false;
  const size_t stride = st::channels().total();
  const float* p = snap_.data() + off * stride + ch;
  double sum = 0.0, sq = 0.0;
  for (uint64_t i = 0; i < len; ++i, p += stride) {
    sum += *p;
    sq += static_cast<double>(*p) * *p;
  }
  const double n = static_cast<double>(len);
  const double var = std::max(0.0, sq / n - (sum / n) * (sum / n));
  *out = var > 1e-12 ? std::max(-120.0, 10.0 * std::log10(var)) : -120.0;
  return true;
}

XcorrResult CaptureStore::xcorr(unsigned ch_a, unsigned ch_b, uint64_t start, uint64_t len,
                                uint64_t freeze_sample) {
  XcorrResult r;
  if (ch_a >= st::channels().total() || ch_b >= st::channels().total()) {
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
      r.error = freeze_sample != 0 ? kSnapshotLost : "freeze the capture first";
      return r;
    }
    if (freeze_sample != 0 && status_.freeze_sample != freeze_sample) {
      r.error = kSnapshotLost;
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
  const double period = static_cast<double>(n) / static_cast<double>(k_dom);
  const int64_t exclude = static_cast<int64_t>(1.5 * period);
  // A steady tone's crests need a correction first: the zero-padded (linear) correlation at
  // lag m sums only len - |m| products, so even a steady tone's crests fall off with |m| — in a
  // short window (genie/sync's sliding 16384) by more than the 1.5 % this test allows. Each crest
  // is therefore divided by the energy it overlaps (per_overlap, below). That is applied only to
  // a window that is TONAL: one harmonic series carries nearly all of the cross-spectrum
  // magnitude — the dominant partial and its multiples. A sine puts ~0.975 there, a square wave,
  // a clipped sine or a sine + 30 % third 0.94-0.98; white noise < 0.1, pink < 0.65. Counting
  // the harmonics is what catches the harmonic-rich tones: their fundamental alone holds only
  // 0.79-0.90, and genie/sync trusted their lag a cycle off. Elsewhere the uncorrected test is
  // the right one (music's and a ping's crests do decay off the true lag). The simulator's music,
  // held synth notes with a second harmonic over a bass, peaks at ~0.95: the windows over 0.93
  // are a held melody note whose period divides the bass note's (D5 over G2, C5 over C3), and
  // their correlation does repeat every bass period (10.2 / 7.6 ms) to within 0.3 % — genuinely
  // ambiguous; the uncorrected test only picked the true lag because it was the nearer to 0.
  // The partial's own band is +-4 frequency-resolution bins (n / len FFT bins each: its main
  // lobe and near side lobes), each harmonic's +-2. Harmonics are counted only while their teeth
  // leave a third of the bins out; for a lower partial the comb would cover (nearly) everything
  // and pass any spectrum, so the partial alone counts.
  float spec_total = 0.0f, spec_comb = 0.0f;
  const size_t dom_radius = std::max<size_t>(2, 4 * n / static_cast<size_t>(len));
  const size_t harm_radius = std::max<size_t>(1, 2 * n / static_cast<size_t>(len));
  const bool comb = 2 * k_dom >= 3 * (2 * harm_radius + 1);
  for (size_t k = 1; k < ca_.size(); ++k) {
    const float a = std::abs(ca_[k]);
    spec_total += a;
    const size_t h = comb ? std::max<size_t>(1, (k + k_dom / 2) / k_dom) : 1;
    const size_t c = h * k_dom;
    const size_t radius = h == 1 ? dom_radius : harm_radius;
    if (k + radius >= c && k <= c + radius) spec_comb += a;
  }
  const bool tonal_spectrum = spec_total > 0.0f && spec_comb > kXcorrTonalShare * spec_total;
  bool tonal = false;
  if (tonal_spectrum) {
    running_energy(fa_.data(), len, cum_a_);
    running_energy(fb_.data(), len, cum_b_);
    tonal = !(has_onset(cum_a_, period) && has_onset(cum_b_, period));
  }
  // The overlap at lag m pairs a[t] with b[t + m]: a's [max(0, -m), len - max(0, m)) with b's
  // [max(0, m), len - max(0, -m)). Dividing by the geometric mean of the two stretches' energy
  // is dividing by len - |m| for a steady level, and also right for a level that moves:
  // - A window that opens mid ring-out (bong, bing) has nothing to pin the cycle, but its decay
  //   leaves the crests off the true lag a percent or two short of it per overlap sample; per
  //   unit energy they are level (an exponential is self-similar), so it reads unresolved
  //   instead of a cycle off at confidence 999.
  // - A burst whose onset is in the window keeps its lag: a crest a cycle off pairs the loudest
  //   first cycle with the silence before it on the other channel, and loses that energy.
  // An onset on both channels (has_onset) skips the correction altogether, as a guard: the
  // attack pins the lag, and the uncorrected test reads it (it keeps a few more of the music's
  // windows with a note attack in them measurable).
  const int64_t slen = static_cast<int64_t>(len);
  const auto per_overlap = [this, slen, tonal](float v, int64_t m) {
    if (!tonal) return v;
    const int64_t lo = std::max<int64_t>(0, m), hi = std::max<int64_t>(0, -m);
    const double ea = cum_a_[slen - lo] - cum_a_[hi];
    const double eb = cum_b_[slen - hi] - cum_b_[lo];
    const double norm = std::sqrt(ea * eb);
    return norm > 0.0 ? static_cast<float>(v / norm) : 0.0f;
  };
  float far_crest = 0.0f;
  for (int64_t m = lobe_lo; m <= lobe_hi; ++m) {
    if (std::llabs(m - best) <= exclude) continue;
    const float v = std::fabs(corr_[wrap(m)]);
    if (v >= std::fabs(corr_[wrap(m - 1)]) && v >= std::fabs(corr_[wrap(m + 1)]))
      far_crest = std::max(far_crest, per_overlap(v, m));
  }
  const float best_crest = per_overlap(best_v, best);
  // A tone so low that the half-height lobe cannot hold a crest past the exclusion (two periods
  // beyond half its width) leaves the crest test nothing to compare: unresolved by definition —
  // provided the lobe does oscillate (the correlation changes sign in it): a slow drift whose
  // spectrum merely piles up in the lowest bins correlates as one smooth hump, no carrier.
  bool too_low = tonal && 2.0 * period > static_cast<double>(lobe_hi - lobe_lo) / 2.0;
  if (too_low) {
    bool crosses = false;
    for (int64_t m = lobe_lo; m < lobe_hi && !crosses; ++m)
      crosses = (corr_[wrap(m)] < 0.0f) != (corr_[wrap(m + 1)] < 0.0f);
    too_low = crosses;
  }
  const bool cycle_unresolved = too_low || far_crest > 0.985f * best_crest;

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
  if (too_low) {
    r.confidence = std::min(r.confidence, 1.0);
  } else if (cycle_unresolved) {
    r.confidence =
        std::min(r.confidence, static_cast<double>(best_crest) / static_cast<double>(far_crest));
  }
  r.confidence = std::min(r.confidence, 999.0);
  return r;
}

}  // namespace st
