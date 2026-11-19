#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "constants.h"

namespace st {

// One cross-correlation delay reading between a channel pair (as CaptureStore::xcorr produces),
// reduced to the two fields the aggregate needs.
struct SyncMeasurement {
  int64_t lag_samples = 0;
  double confidence = 0.0;
};

// Aggregate of several readings between the same pair — one per ping marker, or one per sliding
// window. The median delay and the reading-to-reading spread (jitter), robust and plain, plus the
// median confidence so the caller can judge them. No filtering: every reading counts, and a
// low-confidence outlier simply shows in the spread and in its own confidence field. Lags stay in
// samples; the caller derives ms/m from the engine rate.
struct SyncSummary {
  int n = 0;
  double lag_samples_median = 0.0;
  int64_t lag_samples_min = 0;
  int64_t lag_samples_max = 0;
  int64_t lag_samples_spread = 0;  // max - min: the marker-to-marker jitter
  double lag_samples_mad = 0.0;    // median |lag - median|: the jitter, outliers ignored
  double lag_samples_stdev = 0.0;  // sample standard deviation (0 for one reading)
  double lag_samples_p10 = 0.0;    // 10th / 90th percentile, linearly interpolated
  double lag_samples_p90 = 0.0;
  double confidence_median = 0.0;
  bool bimodal = false;            // the lags form two clusters, not one: see lags_bimodal()
};

// Median of an already-populated vector; sorts it in place. Callers must guard on emptiness.
inline double median_in_place(std::vector<double>& v) {
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// The p-quantile (0..1) of an ascending, non-empty vector, linearly interpolated between ranks.
inline double quantile_sorted(const std::vector<double>& s, double p) {
  const double pos = std::clamp(p, 0.0, 1.0) * static_cast<double>(s.size() - 1);
  const size_t lo = static_cast<size_t>(pos);
  const size_t hi = std::min(lo + 1, s.size() - 1);
  return s[lo] + (pos - static_cast<double>(lo)) * (s[hi] - s[lo]);
}

// Median absolute deviation of [first, last) about `med`. Callers guard on emptiness.
inline double mad_about(std::vector<double>::const_iterator first,
                        std::vector<double>::const_iterator last, double med) {
  std::vector<double> d;
  d.reserve(static_cast<size_t>(last - first));
  for (auto it = first; it != last; ++it) d.push_back(std::fabs(*it - med));
  return median_in_place(d);
}

// Whether a set of lag readings (ascending, in samples) is one answer or two. Either:
//   - more than 20 % of them sit over 5 MAD from the median (MAD floored at one sample, so
//     integer lags that agree exactly do not make every stray reading an outlier), or
//   - the widest gap that leaves at least 20 % of the readings on each side is over 5x the larger
//     MAD of the two sides (again floored at one sample): two tight clusters, e.g. 14.1 ms vs
//     -3.3 ms between one pair. A 50/50 split slips past the first test (its MAD spans the gap).
// Fewer than 5 readings are never called bimodal: there is too little to tell.
inline bool lags_bimodal(const std::vector<double>& sorted) {
  const size_t n = sorted.size();
  if (n < 5) return false;
  const double med = quantile_sorted(sorted, 0.5);
  const double floor = std::max(1.0, mad_about(sorted.begin(), sorted.end(), med));
  size_t far = 0;
  for (double v : sorted)
    if (std::fabs(v - med) > 5.0 * floor) ++far;
  if (static_cast<double>(far) > 0.2 * static_cast<double>(n)) return true;

  const size_t min_side = std::max<size_t>(2, static_cast<size_t>(std::ceil(0.2 * n)));
  size_t split = 0;
  double gap = 0.0;
  for (size_t i = min_side; i + min_side <= n; ++i) {
    if (sorted[i] - sorted[i - 1] > gap) {
      gap = sorted[i] - sorted[i - 1];
      split = i;
    }
  }
  if (split == 0) return false;
  const auto mid = sorted.begin() + static_cast<std::ptrdiff_t>(split);
  const std::vector<double> lo(sorted.begin(), mid), hi(mid, sorted.end());
  const double mad_lo = mad_about(lo.begin(), lo.end(), quantile_sorted(lo, 0.5));
  const double mad_hi = mad_about(hi.begin(), hi.end(), quantile_sorted(hi, 0.5));
  return gap > 5.0 * std::max({1.0, mad_lo, mad_hi});
}

inline SyncSummary summarize_sync(const std::vector<SyncMeasurement>& all) {
  SyncSummary s;
  s.n = static_cast<int>(all.size());
  if (all.empty()) return s;

  std::vector<double> lags, confs;
  lags.reserve(all.size());
  confs.reserve(all.size());
  for (const auto& m : all) {
    lags.push_back(static_cast<double>(m.lag_samples));
    confs.push_back(m.confidence);
  }
  s.lag_samples_median = median_in_place(lags);   // sorts lags: everything below reads them sorted
  s.lag_samples_min = static_cast<int64_t>(lags.front());
  s.lag_samples_max = static_cast<int64_t>(lags.back());
  s.lag_samples_spread = s.lag_samples_max - s.lag_samples_min;
  s.lag_samples_mad = mad_about(lags.begin(), lags.end(), s.lag_samples_median);
  s.lag_samples_p10 = quantile_sorted(lags, 0.1);
  s.lag_samples_p90 = quantile_sorted(lags, 0.9);
  if (lags.size() > 1) {
    double mean = 0.0, ss = 0.0;
    for (double v : lags) mean += v;
    mean /= static_cast<double>(lags.size());
    for (double v : lags) ss += (v - mean) * (v - mean);
    s.lag_samples_stdev = std::sqrt(ss / static_cast<double>(lags.size() - 1));
  }
  s.bimodal = lags_bimodal(lags);
  s.confidence_median = median_in_place(confs);
  return s;
}

// genie/sync's sliding mode: the default window, the next power of two of 0.17 s at `rate` (16384
// at 96 kHz, 8192 at 48 kHz) — long enough for music to correlate, short enough for ~60 windows
// in the default 10 s snapshot.
inline uint64_t sliding_default_window(double rate) {
  const double want = std::max(64.0, 0.17 * rate);
  uint64_t w = 64;
  while (static_cast<double>(w) < want) w <<= 1;
  return w;
}

// Start samples of the sliding windows over [begin, end): every `step` while a whole `win` fits.
// Above `max_windows` they are instead `max_windows` starts spread evenly from `begin` to the last
// start that fits, so a long snapshot still costs a bounded number of xcorrs.
inline std::vector<uint64_t> sliding_window_starts(uint64_t begin, uint64_t end, uint64_t win,
                                                   uint64_t step, size_t max_windows) {
  std::vector<uint64_t> out;
  if (win == 0 || step == 0 || max_windows == 0 || end < begin || end - begin < win) return out;
  const uint64_t last = end - win;  // the last start a whole window fits from
  const uint64_t count = (last - begin) / step + 1;
  if (count <= max_windows) {
    for (uint64_t i = 0; i < count; ++i) out.push_back(begin + i * step);
    return out;
  }
  if (max_windows == 1) return {begin};
  const double span = static_cast<double>(last - begin);
  for (size_t i = 0; i < max_windows; ++i) {
    out.push_back(begin + static_cast<uint64_t>(std::llround(span * static_cast<double>(i) /
                                                             static_cast<double>(max_windows - 1))));
  }
  return out;
}

// Whether a summary can be taken at face value without reading the individual readings.
//   pings:   confidence_median > 3 and MAD <= 2 samples — clean, unambiguous arrivals that agree.
//   sliding: at least kSyncTrustMinWindows (10) kept, stdev under 0.5 ms and one cluster — music
//            correlates less
//            cleanly than a ping (on the simulator about a third of 16384 windows clear 1.5), so it
//            needs numbers and agreement instead of a high confidence.
//            And a window of at least a quarter of the default (4096 at 96 kHz), with the stdev
//            under 1/64 of it: a small window confines every lag to a few hundred samples, so
//            "under 0.5 ms" holds by construction, and on tonal music it locks onto a wrong lag
//            near 0 that every window agrees on.
inline bool sync_trusted_pings(const SyncSummary& s) {
  return s.n > 0 && s.confidence_median > 3.0 && s.lag_samples_mad <= 2.0;
}
inline bool sync_trusted_sliding(const SyncSummary& s, double rate, uint64_t win) {
  return s.n >= kSyncTrustMinWindows && rate > 0 && win >= sliding_default_window(rate) / 4 &&
         1000.0 * s.lag_samples_stdev / rate < 0.5 &&
         s.lag_samples_stdev < static_cast<double>(win) / 64.0 && !s.bimodal;
}

}  // namespace st
