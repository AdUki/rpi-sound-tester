#include "genie_watch.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "util/dsp.h"

namespace st {

namespace {

constexpr const char* kKindNames[kWatchKinds] = {"zero_run", "gap",        "onset",      "offset",
                                                 "click",    "level_step", "phase_slip", "resid"};

constexpr double kPi = kTwoPi / 2;

// To (-pi, pi].
double wrap(double a) {
  a = std::fmod(a, kTwoPi);
  if (a > kPi) a -= kTwoPi;
  if (a <= -kPi) a += kTwoPi;
  return a;
}

double db10(double p) { return p > 1e-15 ? 10.0 * std::log10(p) : -150.0; }

double median_of(std::vector<double> v) {
  const size_t mid = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + mid, v.end());
  return v[mid];
}

// A fitted block counts towards phase and frequency only when the tone clearly dominates it:
// the residual at most a tenth of the tone's power (-10 dBc). A block half filled by a hole or
// straddling a slip fails this and is skipped, not trusted.
constexpr double kFitMaxResidRatio = 0.1;
// How far apart two clean blocks may sit for their phases to be compared: across a gap of up to
// this, a tone that came back shifted is still told from one that did not.
constexpr double kPhaseBridgeS = 1.0;
// A level step is judged between two stretches of this long, each steady: its blocks within this
// many dB of their median (one may stray), so music's own dynamics, and fades, are not steps.
constexpr double kStepWindowS = 0.1;
constexpr double kStepSteadyDb = 1.5;
// A zero run counts as a hole inside audio when there is level within this before and after it.
constexpr double kZeroContextS = 0.05;
// A gap's (and onset's, offset's) edges must be this loud (over silence_db) within this long of
// the quiet: an abrupt cut, not a fade.
constexpr double kEdgeLoudDb = 20.0;
constexpr double kEdgeS = 0.03;
// ...and the sound on that side must be sustained: the median block level over this long next to
// the quiet (as much of it as the span has) is that loud too. So the pauses of an intermittent
// signal (the ping or tick generator, a click track, speech) are neither dropouts nor edges.
constexpr double kSustainS = 0.5;

}  // namespace

const char* watch_kind_name(WatchKind k) {
  const size_t i = static_cast<size_t>(k);
  return i < kWatchKinds ? kKindNames[i] : "?";
}

WatchAnalyzer::WatchAnalyzer(const WatchParams& p) : p_(p) {
  if (p_.block < 16) p_.block = 16;
  zmin_ = std::max<uint64_t>(1, static_cast<uint64_t>(std::llround(p_.rate / 1000.0)));
  ctol_ = std::max<uint64_t>(1, static_cast<uint64_t>(std::llround(p_.rate * 0.002)));
  if (p_.tone_hz) {
    w_ = kTwoPi * *p_.tone_hz / p_.rate;
    ws_ = std::sin(w_);
    wc_ = std::cos(w_);
    start_tone_block();
  }
  d2_.reserve(p_.block);
  scratch_.reserve(p_.block);
  d2_start_ = 1;  // sample 0 has no second difference
}

void WatchAnalyzer::feed(const float* x, size_t n) {
  const bool tone = p_.tone_hz.has_value();
  for (size_t k = 0; k < n; ++k) {
    const float v = x[k];
    const uint64_t i = pos_++;
    const float a = std::fabs(v);
    sumsq_ += static_cast<double>(v) * v;
    if (a > peak_) peak_ = a;
    if (a >= 0.999f) ++clipped_;
    if (tone) {
      const double s = rs_, c = rc_, t = t_;
      const double u[5] = {s, c, t * s, t * c, 1.0};
      for (int r = 0, g = 0; r < 5; ++r) {
        proj_[r] += v * u[r];
        for (int q = r; q < 5; ++q) gram_[g++] += u[r] * u[q];
      }
      t_ += 1.0;
      rs_ = s * wc_ + c * ws_;
      rc_ = c * wc_ - s * ws_;
    }

    if (v == 0.0f) {
      if (!in_zero_) {
        in_zero_ = true;
        zstart_ = i;
      }
    } else if (in_zero_) {
      end_zero_run(i);
    }

    // The second difference centred on i - 1; it closes i - 1's block when i starts a new one.
    if (i >= 2) {
      d2_.push_back(std::fabs(v - 2.0f * x1_ + x2_));
      if (bn_ == 0) end_d2_block();
    }
    x2_ = x1_;
    x1_ = v;

    if (++bn_ == p_.block) end_block();
  }
}

void WatchAnalyzer::end_zero_run(uint64_t end) {
  in_zero_ = false;
  if (end - zstart_ >= zmin_ && zero_runs_.size() < 100000) zero_runs_.push_back({zstart_, end - zstart_});
}

void WatchAnalyzer::end_block() {
  Block b;
  const unsigned n = bn_;
  b.rms_db = n ? lin_to_db(static_cast<float>(std::sqrt(sumsq_ / n))) : -120.0f;
  b.peak = peak_;
  if (p_.tone_hz && n >= 16) {
    // Least squares over {sin, cos, t sin, t cos, 1}: Cholesky of the 5x5 normal equations.
    double L[5][5] = {};
    double A[5][5];
    for (int r = 0, g = 0; r < 5; ++r)
      for (int q = r; q < 5; ++q) A[r][q] = A[q][r] = gram_[g++];
    bool ok = true;
    for (int j = 0; j < 5 && ok; ++j) {
      double d = A[j][j];
      for (int k = 0; k < j; ++k) d -= L[j][k] * L[j][k];
      if (!(d > 1e-9 * std::max(1.0, A[j][j]))) {
        ok = false;
        break;
      }
      L[j][j] = std::sqrt(d);
      for (int r = j + 1; r < 5; ++r) {
        double e = A[r][j];
        for (int k = 0; k < j; ++k) e -= L[r][k] * L[j][k];
        L[r][j] = e / L[j][j];
      }
    }
    if (ok) {
      double y[5], th[5];
      for (int r = 0; r < 5; ++r) {
        double e = proj_[r];
        for (int k = 0; k < r; ++k) e -= L[r][k] * y[k];
        y[r] = e / L[r][r];
      }
      for (int r = 4; r >= 0; --r) {
        double e = y[r];
        for (int k = r + 1; k < 5; ++k) e -= L[k][r] * th[k];
        th[r] = e / L[r][r];
      }
      double explained = 0;
      for (int r = 0; r < 5; ++r) explained += th[r] * proj_[r];
      const double amp2 = th[0] * th[0] + th[1] * th[1];
      const double resid = std::max(0.0, sumsq_ - explained);
      b.amp = std::sqrt(amp2);
      b.phase = std::atan2(th[1], th[0]);
      b.dc = th[4];
      b.resid_ratio = amp2 > 0 ? (resid / n) / (amp2 / 2) : std::numeric_limits<double>::infinity();
      b.solved = amp2 > 0;
    }
  }
  blocks_.push_back(b);
  bn_ = 0;
  sumsq_ = 0;
  peak_ = 0;
  if (p_.tone_hz) start_tone_block();
}

void WatchAnalyzer::start_tone_block() {
  std::fill(std::begin(gram_), std::end(gram_), 0.0);
  std::fill(std::begin(proj_), std::end(proj_), 0.0);
  // The phase is read at the block's centre: t runs from -(block-1)/2. The oscillator restarts
  // from the absolute phase each block, so its error never builds up.
  t_ = -0.5 * (p_.block - 1);
  const double th = std::fmod(w_ * static_cast<double>(pos_ - pos_ % p_.block), kTwoPi);
  rs_ = std::sin(th);
  rc_ = std::cos(th);
}

void WatchAnalyzer::end_d2_block() {
  if (d2_.empty()) return;
  scratch_.assign(d2_.begin(), d2_.end());
  const size_t mid = scratch_.size() / 2;
  std::nth_element(scratch_.begin(), scratch_.begin() + mid, scratch_.end());
  const float med = scratch_[mid];
  d2_median_.push_back(med);
  // Candidates against this block's own median; finish() holds them to the neighbours' too.
  const float floor = db_to_lin(static_cast<float>(p_.silence_db));
  const float thr = std::max(static_cast<float>(p_.click_k) * med, floor);
  for (size_t j = 0; j < d2_.size(); ++j) {
    const float d = d2_[j];
    if (!(d > thr)) continue;
    const uint64_t at = d2_start_ + j;
    if (open_click_ && at - open_click_->last <= ctol_) {
      open_click_->last = at;
      if (d > open_click_->peak) {
        open_click_->peak = d;
        open_click_->at = at;
      }
    } else {
      if (open_click_ && clicks_.size() < 100000) clicks_.push_back(*open_click_);
      open_click_ = ClickCand{at, at, at, d};
    }
  }
  d2_start_ += d2_.size();
  d2_.clear();
}

WatchChannelResult WatchAnalyzer::finish(const WatchReread& reread) {
  WatchChannelResult r;
  // Flush what the last, partial, block holds.
  const uint64_t last_len = bn_;
  if (bn_ > 0) end_block();
  end_d2_block();
  if (open_click_) clicks_.push_back(*open_click_);
  open_click_.reset();
  if (in_zero_) end_zero_run(pos_);

  const uint64_t B = p_.block;
  const size_t nb = blocks_.size();
  r.samples = pos_;
  r.blocks = nb;
  r.clipped_samples = clipped_;
  if (nb == 0) return r;
  auto block_start = [&](size_t b) { return static_cast<uint64_t>(b) * B; };
  auto block_len = [&](size_t b) { return b + 1 == nb && last_len ? last_len : B; };
  auto block_of = [&](uint64_t s) { return std::min<size_t>(nb - 1, static_cast<size_t>(s / B)); };

  // ---- Levels --------------------------------------------------------------------------------
  std::vector<bool> quiet(nb);
  std::vector<double> levels(nb);
  uint64_t quiet_samples = 0;
  float peak = 0;
  for (size_t b = 0; b < nb; ++b) {
    levels[b] = blocks_[b].rms_db;
    quiet[b] = blocks_[b].rms_db < p_.silence_db;
    if (quiet[b]) quiet_samples += block_len(b);
    peak = std::max(peak, blocks_[b].peak);
  }
  {
    std::vector<double> s = levels;
    std::sort(s.begin(), s.end());
    r.rms_db_min = s.front();
    r.rms_db_max = s.back();
    r.rms_db_median = s[s.size() / 2];
    r.rms_db_p10 = s[s.size() / 10];
  }
  r.peak_max_db = lin_to_db(peak);
  r.silent_s = static_cast<double>(quiet_samples) / p_.rate;
  r.sound_pct = pos_ ? 100.0 * static_cast<double>(pos_ - quiet_samples) / static_cast<double>(pos_) : 0;

  std::vector<WatchEvent> ev;
  auto add = [&](WatchEvent e) { ev.push_back(std::move(e)); };

  // ---- Gaps, onsets, offsets: runs of quiet blocks, by what is right around them -------------
  const size_t edge = std::max<size_t>(1, static_cast<size_t>(std::llround(kEdgeS * p_.rate / B)));
  const double loud = p_.silence_db + kEdgeLoudDb;
  const size_t sustain = std::max<size_t>(1, static_cast<size_t>(std::llround(kSustainS * p_.rate / B)));
  auto median_level = [&](size_t a, size_t b) {  // blocks [a, b); -200 when empty
    if (a >= b) return -200.0;
    std::vector<double> v(levels.begin() + a, levels.begin() + b);
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
  };
  for (size_t p = 0; p < nb;) {
    if (!quiet[p]) {
      ++p;
      continue;
    }
    size_t q = p;
    while (q + 1 < nb && quiet[q + 1]) ++q;
    double before = -200, after = -200;
    for (size_t b = p >= edge ? p - edge : 0; b < p; ++b) before = std::max(before, levels[b]);
    for (size_t b = q + 1; b < nb && b <= q + edge; ++b) after = std::max(after, levels[b]);
    const bool loud_before = before >= loud && median_level(p >= sustain ? p - sustain : 0, p) >= loud;
    const bool loud_after = after >= loud && median_level(q + 1, std::min(nb, q + 1 + sustain)) >= loud;
    const uint64_t s0 = block_start(p), s1 = block_start(q) + block_len(q);
    WatchEvent e;
    e.offset = s0;
    e.len = s1 - s0;
    if (loud_before && loud_after) {
      e.kind = WatchKind::Gap;
      add(e);
    } else if (loud_before) {
      e.kind = WatchKind::Offset;
      add(e);
    } else if (loud_after) {
      e.kind = WatchKind::Onset;
      e.offset = s1;
      e.len = 0;
      add(e);
    }
    p = q + 1;
  }

  // ---- Exact-zero runs inside audio; those in a gap make it a sample-exact `zeros` gap ---------
  {
    const uint64_t ctx = static_cast<uint64_t>(kZeroContextS * p_.rate);
    auto any_level = [&](uint64_t a, uint64_t b) {  // [a, b) samples
      if (a >= b) return false;
      for (size_t k = block_of(a); k <= block_of(b - 1); ++k)
        if (!quiet[k]) return true;
      return false;
    };
    std::vector<std::pair<uint64_t, uint64_t>> inside;
    for (const auto& [z0, zl] : zero_runs_) {
      const uint64_t z1 = z0 + zl;
      if (z0 == 0 || z1 >= pos_) continue;
      if (any_level(z0 > ctx ? z0 - ctx : 0, z0) && any_level(z1, std::min(pos_, z1 + ctx)))
        inside.push_back({z0, zl});
    }
    std::vector<bool> used(inside.size());
    for (WatchEvent& g : ev) {
      if (g.kind != WatchKind::Gap) continue;
      std::vector<size_t> over;
      for (size_t k = 0; k < inside.size(); ++k) {
        const uint64_t z0 = inside[k].first, z1 = z0 + inside[k].second;
        if (z0 < g.offset + g.len + B && z1 + B > g.offset) over.push_back(k);
      }
      for (size_t k : over) used[k] = true;
      if (over.size() == 1 && inside[over[0]].second + 2 * B >= g.len) {
        g.zeros = true;
        g.offset = inside[over[0]].first;
        g.len = inside[over[0]].second;
      }
    }
    for (size_t k = 0; k < inside.size(); ++k) {
      if (used[k]) continue;
      WatchEvent e;
      e.kind = WatchKind::ZeroRun;
      e.offset = inside[k].first;
      e.len = inside[k].second;
      add(e);
    }
  }

  // ---- Level steps: between two steady stretches, quiet blocks left out ----------------------
  {
    const size_t W = std::max<size_t>(2, static_cast<size_t>(std::llround(kStepWindowS * p_.rate / B)));
    // Mean power (dB) of the non-quiet blocks in [a, b): false unless at least half of them are
    // non-quiet and steady, every one but one within kStepSteadyDb of their median.
    std::vector<double> lv;
    auto stretch = [&](size_t a, size_t b, double* db) {
      lv.clear();
      double pw = 0;
      for (size_t k = a; k < b; ++k) {
        if (quiet[k]) continue;
        lv.push_back(levels[k]);
        pw += std::pow(10.0, levels[k] / 10.0);
      }
      if (lv.empty() || lv.size() * 2 < b - a) return false;
      const double med = median_of(lv);
      size_t out = 0;
      for (double l : lv) out += std::fabs(l - med) > kStepSteadyDb;
      if (out > 1) return false;
      *db = db10(pw / static_cast<double>(lv.size()));
      return true;
    };
    // Boundary b sits at block b's start; the block on either side of it is left out, so a step
    // inside one of them does not blur both stretches.
    std::vector<double> diff(nb + 1, 0.0), from(nb + 1, 0.0), to(nb + 1, 0.0);
    std::vector<bool> cand(nb + 1, false);
    for (size_t b = W + 1; b + W + 1 <= nb; ++b) {
      double x, y;
      if (!stretch(b - 1 - W, b - 1, &x) || !stretch(b + 1, b + 1 + W, &y)) continue;
      diff[b] = y - x;
      from[b] = x;
      to[b] = y;
      cand[b] = std::fabs(y - x) > p_.level_step_db;
    }
    for (size_t b = 0; b <= nb;) {
      if (!cand[b]) {
        ++b;
        continue;
      }
      size_t e = b;
      while (e + 1 <= nb && cand[e + 1]) ++e;
      size_t best = b;
      for (size_t k = b; k <= e; ++k)
        if (std::fabs(diff[k]) > std::fabs(diff[best])) best = k;
      // The boundaries tied with the best (within 0.5 dB) straddle the step: take their middle.
      size_t lo = best, hi = best;
      while (lo > b && std::fabs(diff[lo - 1]) >= std::fabs(diff[best]) - 0.5) --lo;
      while (hi < e && std::fabs(diff[hi + 1]) >= std::fabs(diff[best]) - 0.5) ++hi;
      WatchEvent ls;
      ls.kind = WatchKind::LevelStep;
      ls.offset = (lo + hi) * B / 2;
      ls.len = 0;
      ls.from_db = from[best];
      ls.to_db = to[best];
      add(ls);
      b = e + 1;
    }
  }

  // ---- Tone: phase slips, frequency, residual ------------------------------------------------
  // Slip regions [before block, after block], kept to leave them out of the frequency figures.
  std::vector<std::pair<size_t, size_t>> slip_regions;
  if (p_.tone_hz) {
    WatchToneStats t;
    t.expect_hz = *p_.tone_hz;
    std::vector<double> resid_db, thd;
    std::vector<bool> bad(nb, false);
    for (size_t b = 0; b < nb; ++b) {
      const Block& k = blocks_[b];
      // A partial last block too short to fit (a span that ends a few samples into a block) says
      // nothing about the tone: not a bad block, nor a `resid` at the span's end.
      if (quiet[b] || (block_len(b) < B && !k.solved)) continue;
      ++t.blocks;
      const double rd = k.solved ? db10(k.resid_ratio) : 0.0;
      if (k.solved) {
        resid_db.push_back(rd);
        thd.push_back(100.0 * std::sqrt(k.resid_ratio));
      }
      if (!k.solved || rd > p_.resid_dbc) {
        bad[b] = true;
        ++t.bad_blocks;
      }
    }
    if (!resid_db.empty()) {
      t.resid_dbc_median = median_of(resid_db);
      t.resid_dbc_max = *std::max_element(resid_db.begin(), resid_db.end());
      t.thd_n_pct_median = median_of(thd);
      t.thd_n_pct_max = *std::max_element(thd.begin(), thd.end());
    }

    // blocks clean enough for their phase to count. Not a partial last block: its phase is read
    // at a full block's centre, past its last sample, so it is an extrapolation of a fit over a
    // few samples (and the drift terms make it wilder the shorter the block): a phase slip at the
    // span's end that a span ending elsewhere never shows.
    std::vector<size_t> F;
    std::vector<bool> fit(nb, false);
    for (size_t b = 0; b < nb; ++b) {
      const Block& k = blocks_[b];
      if (!quiet[b] && k.solved && k.resid_ratio <= kFitMaxResidRatio && block_len(b) == B) {
        F.push_back(b);
        fit[b] = true;
      }
    }
    // The phase's own drift per block: a tone a little off expect_hz (or a sender's clock)
    // advances it steadily, and only a jump against that is a slip.
    std::vector<double> steps;
    for (size_t b = 0; b + 1 < nb; ++b)
      if (fit[b] && fit[b + 1]) steps.push_back(wrap(blocks_[b + 1].phase - blocks_[b].phase));
    if (!steps.empty() && F.size() >= 3) {
      const double m = median_of(steps);
      const double thr = p_.slip_deg * kPi / 180.0;
      const size_t bridge = static_cast<size_t>(kPhaseBridgeS * p_.rate / B) + 2;
      auto dev = [&](size_t a, size_t b) {  // phase jump from block a to block b, drift removed
        return wrap(blocks_[b].phase - blocks_[a].phase - m * static_cast<double>(b - a));
      };
      // Each clean block's neighbours compared over its head: a slip anywhere between them,
      // even half-way through a block that then reads in between, shows in full.
      std::vector<double> D(F.size(), 0.0);
      std::vector<bool> over(F.size(), false);
      for (size_t k = 1; k + 1 < F.size(); ++k) {
        if (F[k + 1] - F[k - 1] > bridge) continue;
        D[k] = dev(F[k - 1], F[k + 1]);
        over[k] = std::fabs(D[k]) > thr;
      }
      for (size_t k = 1; k + 1 < F.size();) {
        if (!over[k]) {
          ++k;
          continue;
        }
        size_t k2 = k;
        while (k2 + 1 < F.size() - 1 && over[k2 + 1]) ++k2;
        const size_t kb = k - 1, ka = k2 + 1;
        const size_t bb = F[kb], ba = F[ka];
        k = k2 + 1;
        const double total = dev(bb, ba);
        if (std::fabs(total) <= thr) continue;
        // A slip moves the tone for good: the phase on either side of it must be the tone's, as
        // its own neighbour on that side agrees. A block disturbed enough to read off-phase yet
        // still fit (a burst on one channel) would otherwise read as a slip against each of its
        // neighbours; and at the span's ends there is no neighbour to agree, so a disturbance in
        // the last (or first) block or two is not called a slip.
        auto agrees = [&](size_t x, size_t y) {  // F indices: x < y
          return F[y] - F[x] <= bridge && std::fabs(dev(F[x], F[y])) <= thr;
        };
        if (kb == 0 || !agrees(kb - 1, kb) || ka + 1 >= F.size() || !agrees(ka, ka + 1)) continue;
        slip_regions.push_back({bb, ba});
        WatchEvent e;
        e.kind = WatchKind::PhaseSlip;
        e.slip_samples = total / w_;
        e.slip_deg = total * 180.0 / kPi;
        const uint64_t w0 = block_start(bb), w1 = block_start(ba) + block_len(ba);
        e.offset = (block_start(bb) + block_len(bb) + block_start(ba)) / 2;
        // To the sample: where the signal stops matching the tone before and starts matching the
        // tone after (both carried along the drift).
        std::vector<float> x(static_cast<size_t>(w1 - w0));
        if (reread && !x.empty() && reread(w0, x.size(), x.data())) {
          const Block& P = blocks_[bb];
          const Block& Q = blocks_[ba];
          const double cp = centre(bb), cq = centre(ba);
          const double drift = m / static_cast<double>(B);
          std::vector<double> e2(x.size() + 1, 0.0);  // suffix sums of the after-model error
          std::vector<double> e1(x.size());
          for (size_t i = 0; i < x.size(); ++i) {
            const double n = static_cast<double>(w0 + i);
            const double y1 = P.amp * std::sin(w_ * n + P.phase + drift * (n - cp)) + P.dc;
            const double y2 = Q.amp * std::sin(w_ * n + Q.phase + drift * (n - cq)) + Q.dc;
            e1[i] = (x[i] - y1) * (x[i] - y1);
            e2[i] = (x[i] - y2) * (x[i] - y2);
          }
          for (size_t i = x.size(); i-- > 0;) e2[i] += e2[i + 1];
          double best = e2[0], acc = 0;
          size_t at = 0;
          for (size_t s = 1; s <= x.size(); ++s) {
            acc += e1[s - 1];
            if (acc + e2[s] < best) {
              best = acc + e2[s];
              at = s;
            }
          }
          e.offset = w0 + at;
        }
        add(e);
      }

      // Frequency: the phase advance between consecutive clean blocks outside any slip.
      std::vector<double> f;
      for (size_t b = 0; b + 1 < nb; ++b) {
        if (!fit[b] || !fit[b + 1] || bad[b] || bad[b + 1]) continue;
        bool in_slip = false;
        for (const auto& [a, z] : slip_regions) in_slip |= b + 1 >= a && b <= z;
        if (in_slip) continue;
        const double d = wrap(blocks_[b + 1].phase - blocks_[b].phase);
        f.push_back(*p_.tone_hz + d / static_cast<double>(B) * p_.rate / kTwoPi);
      }
      if (!f.empty()) {
        t.freq_hz_median = median_of(f);
        t.freq_hz_min = *std::min_element(f.begin(), f.end());
        t.freq_hz_max = *std::max_element(f.begin(), f.end());
      }
    }

    // Bad blocks in a row: one resid event (dropped below if another event explains it).
    for (size_t b = 0; b < nb;) {
      if (!bad[b]) {
        ++b;
        continue;
      }
      size_t e = b;
      double worst = -150;
      while (e < nb && bad[e]) {
        worst = std::max(worst, blocks_[e].solved ? db10(blocks_[e].resid_ratio) : 0.0);
        ++e;
      }
      WatchEvent rv;
      rv.kind = WatchKind::Resid;
      rv.offset = block_start(b);
      rv.len = block_start(e - 1) + block_len(e - 1) - rv.offset;
      rv.resid_dbc = worst;
      add(rv);
      b = e;
    }
    r.tone = t;
  }

  // A slip across a hole or a gap belongs to it: "the tone came back N samples off".
  for (auto it = ev.begin(); it != ev.end();) {
    if (it->kind != WatchKind::PhaseSlip) {
      ++it;
      continue;
    }
    const uint64_t s = it->offset;
    WatchEvent* host = nullptr;
    for (WatchEvent& g : ev) {
      if (g.kind != WatchKind::Gap && g.kind != WatchKind::ZeroRun) continue;
      if (s + B >= g.offset && s <= g.offset + g.len + B) host = &g;
    }
    if (host) {
      host->slip_samples = it->slip_samples;
      host->slip_deg = it->slip_deg;
      it = ev.erase(it);
    } else {
      ++it;
    }
  }

  // ---- Clicks: against the larger of the median of their block and its neighbours ------------
  {
    const float floor = db_to_lin(static_cast<float>(p_.silence_db));
    const size_t nm = d2_median_.size();
    const size_t n_before = ev.size();
    for (const ClickCand& c : clicks_) {
      const size_t b = std::min(nm - 1, static_cast<size_t>(c.at / B));
      float scale = d2_median_[b];
      if (b > 0) scale = std::max(scale, d2_median_[b - 1]);
      if (b + 1 < nm) scale = std::max(scale, d2_median_[b + 1]);
      if (!(c.peak > std::max(static_cast<float>(p_.click_k) * scale, floor))) continue;
      // An edge of a hole, a gap, a slip or a step clicks too: fold it into that event.
      bool folded = false;
      for (size_t k = 0; k < n_before; ++k) {
        WatchEvent& h = ev[k];
        if (h.kind == WatchKind::Resid) continue;
        const bool exact = h.kind == WatchKind::ZeroRun || h.kind == WatchKind::PhaseSlip ||
                           (h.kind == WatchKind::Gap && h.zeros);
        const uint64_t tol = exact ? ctol_ : B + ctol_;
        if (c.last + tol >= h.offset && c.first <= h.offset + h.len + tol) {
          h.click = true;
          folded = true;
        }
      }
      if (folded) continue;
      WatchEvent e;
      e.kind = WatchKind::Click;
      e.offset = c.at;
      e.len = c.last - c.first + 1;
      e.level_db = lin_to_db(c.peak);
      add(e);
    }
  }

  // A resid run that another event overlaps is that event's doing.
  {
    std::vector<bool> drop(ev.size(), false);
    for (size_t i = 0; i < ev.size(); ++i) {
      const WatchEvent& rv = ev[i];
      if (rv.kind != WatchKind::Resid) continue;
      for (const WatchEvent& h : ev) {
        if (h.kind != WatchKind::Resid && h.offset + h.len + B >= rv.offset &&
            h.offset <= rv.offset + rv.len + B)
          drop[i] = true;
      }
    }
    size_t w = 0;
    for (size_t i = 0; i < ev.size(); ++i)
      if (!drop[i]) ev[w++] = std::move(ev[i]);
    ev.resize(w);
  }

  std::stable_sort(ev.begin(), ev.end(),
                   [](const WatchEvent& a, const WatchEvent& b) { return a.offset < b.offset; });
  for (const WatchEvent& e : ev) ++r.counts[static_cast<size_t>(e.kind)];
  if (ev.size() > kWatchMaxChannelEvents) {
    ev.resize(kWatchMaxChannelEvents);
    r.events_truncated = true;
  }
  r.events = std::move(ev);
  return r;
}

std::vector<WatchMergedEvent> merge_watch_events(const std::vector<WatchChannelEvents>& chans,
                                                 uint64_t tol) {
  struct Item {
    unsigned ch;
    int group;
    const WatchEvent* ev;
  };
  std::vector<Item> all;
  for (const WatchChannelEvents& c : chans) {
    if (!c.events) continue;
    for (const WatchEvent& e : *c.events) all.push_back({c.ch, c.group, &e});
  }
  std::stable_sort(all.begin(), all.end(),
                   [](const Item& a, const Item& b) { return a.ev->offset < b.ev->offset; });
  std::vector<WatchMergedEvent> out;
  std::vector<int> groups;
  for (const Item& it : all) {
    bool merged = false;
    if (it.group >= 0) {
      for (size_t j = out.size(); j-- > 0;) {
        WatchMergedEvent& m = out[j];
        if (it.ev->offset > m.ev.offset + tol) break;
        if (groups[j] != it.group || m.ev.kind != it.ev->kind) continue;
        if (std::find(m.ch.begin(), m.ch.end(), it.ch) != m.ch.end()) continue;
        m.ch.push_back(it.ch);
        m.ev.len = std::max(m.ev.len, it.ev->len);
        m.ev.click |= it.ev->click;
        m.ev.zeros |= it.ev->zeros;
        merged = true;
        break;
      }
    }
    if (merged) continue;
    out.push_back({*it.ev, {it.ch}});
    groups.push_back(it.group);
  }
  return out;
}

}  // namespace st
