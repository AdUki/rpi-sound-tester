#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace st {

// One cross-correlation delay reading between a channel pair (as CaptureStore::xcorr produces),
// reduced to the two fields the aggregate needs.
struct SyncMeasurement {
  int64_t lag_samples = 0;
  double confidence = 0.0;
};

// Aggregate of several readings between the same pair — one per ping marker. The median delay and
// the marker-to-marker spread (jitter), plus the median confidence so the caller can judge them.
// No filtering: every reading counts, and a low-confidence outlier simply shows in the spread and
// in its own confidence field. Lags stay in samples; the caller derives ms/m from the engine rate.
struct SyncSummary {
  int n = 0;
  double lag_samples_median = 0.0;
  int64_t lag_samples_min = 0;
  int64_t lag_samples_max = 0;
  int64_t lag_samples_spread = 0;  // max - min: the marker-to-marker jitter
  double confidence_median = 0.0;
};

// Median of an already-populated vector; sorts it in place. Callers must guard on emptiness.
inline double median_in_place(std::vector<double>& v) {
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
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
  const auto mm = std::minmax_element(lags.begin(), lags.end());
  s.lag_samples_min = static_cast<int64_t>(*mm.first);
  s.lag_samples_max = static_cast<int64_t>(*mm.second);
  s.lag_samples_spread = s.lag_samples_max - s.lag_samples_min;
  s.lag_samples_median = median_in_place(lags);   // sorts lags — do min/max first
  s.confidence_median = median_in_place(confs);
  return s;
}

}  // namespace st
