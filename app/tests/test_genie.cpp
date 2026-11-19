#include "genie.h"

#include <cmath>
#include <vector>

#include "check.h"

using namespace st;

namespace {

void test_empty_has_no_readings() {
  const SyncSummary s = summarize_sync({});
  CHECK_EQ(s.n, 0);
}

void test_odd_count() {
  const SyncSummary s = summarize_sync({{137, 40.0}, {138, 35.0}, {136, 42.0}});
  CHECK_EQ(s.n, 3);
  CHECK_NEAR(s.lag_samples_median, 137.0, 1e-9);
  CHECK_EQ(s.lag_samples_min, static_cast<int64_t>(136));
  CHECK_EQ(s.lag_samples_max, static_cast<int64_t>(138));
  CHECK_EQ(s.lag_samples_spread, static_cast<int64_t>(2));
  CHECK_NEAR(s.confidence_median, 40.0, 1e-9);
}

void test_even_count_averages_the_two_middles() {
  const SyncSummary s = summarize_sync({{100, 40.0}, {101, 30.0}});
  CHECK_EQ(s.n, 2);
  CHECK_NEAR(s.lag_samples_median, 100.5, 1e-9);
  CHECK_NEAR(s.confidence_median, 35.0, 1e-9);
}

// Nothing is filtered any more: a low-confidence outlier (a cycle-slip) still counts, so it
// widens the spread and is visible in the reading — the caller judges it by the confidence field.
void test_low_confidence_still_counts() {
  const SyncSummary s = summarize_sync({{100, 40.0}, {101, 30.0}, {999, 1.2}});
  CHECK_EQ(s.n, 3);
  CHECK_NEAR(s.lag_samples_median, 101.0, 1e-9);  // sorted {100,101,999}
  CHECK_EQ(s.lag_samples_max, static_cast<int64_t>(999));
  CHECK_EQ(s.lag_samples_spread, static_cast<int64_t>(899));
}

void test_negative_lags() {
  const SyncSummary s = summarize_sync({{-960, 3.0}, {-961, 5.0}, {-959, 4.0}});
  CHECK_EQ(s.n, 3);
  CHECK_NEAR(s.lag_samples_median, -960.0, 1e-9);
  CHECK_EQ(s.lag_samples_min, static_cast<int64_t>(-961));
  CHECK_EQ(s.lag_samples_max, static_cast<int64_t>(-959));
  CHECK_EQ(s.lag_samples_spread, static_cast<int64_t>(2));
}

std::vector<SyncMeasurement> lags(const std::vector<int64_t>& v, double conf = 10.0) {
  std::vector<SyncMeasurement> out;
  for (int64_t l : v) out.push_back({l, conf});
  return out;
}

void test_robust_stats() {
  // 137 x 8, one 138, one far outlier: MAD ignores the outlier, stdev does not.
  const SyncSummary s = summarize_sync(lags({137, 137, 137, 137, 138, 137, 137, 137, 137, 5000}));
  CHECK_EQ(s.n, 10);
  CHECK_NEAR(s.lag_samples_median, 137.0, 1e-9);
  CHECK_NEAR(s.lag_samples_mad, 0.0, 1e-9);
  CHECK(s.lag_samples_stdev > 1000.0);
  CHECK_NEAR(s.lag_samples_p10, 137.0, 1e-9);
  // p90 of 10 sorted values sits at rank 8.1: 138 + 0.1 * (5000 - 138)
  CHECK_NEAR(s.lag_samples_p90, 138.0 + 0.1 * (5000.0 - 138.0), 1e-6);
  CHECK(!s.bimodal);  // one stray reading out of ten is not a second cluster
}

void test_mad_and_stdev_small() {
  const SyncSummary s = summarize_sync(lags({10, 12, 14, 16, 18}));
  CHECK_NEAR(s.lag_samples_median, 14.0, 1e-9);
  CHECK_NEAR(s.lag_samples_mad, 2.0, 1e-9);           // |d| = 4,2,0,2,4
  CHECK_NEAR(s.lag_samples_stdev, std::sqrt(10.0), 1e-9);  // sum d^2 = 40, / 4
  CHECK_NEAR(s.lag_samples_p10, 10.8, 1e-9);
  CHECK_NEAR(s.lag_samples_p90, 17.2, 1e-9);
  CHECK(!s.bimodal);
}

void test_one_reading() {
  const SyncSummary s = summarize_sync(lags({42}));
  CHECK_NEAR(s.lag_samples_stdev, 0.0, 1e-12);
  CHECK_NEAR(s.lag_samples_mad, 0.0, 1e-12);
  CHECK_NEAR(s.lag_samples_p10, 42.0, 1e-12);
  CHECK_NEAR(s.lag_samples_p90, 42.0, 1e-12);
  CHECK(!s.bimodal);
}

// The 2318fe08 case: one pair reading 14.1 ms half the time and -3.3 ms the other half (at 48 kHz:
// 677 and -158). The median lands in one cluster and the MAD spans the gap, so only the split test
// sees it.
void test_bimodal_even_split() {
  std::vector<int64_t> v;
  for (int i = 0; i < 12; ++i) v.push_back(677 + (i % 3) - 1);
  for (int i = 0; i < 12; ++i) v.push_back(-158 + (i % 3) - 1);
  CHECK(summarize_sync(lags(v)).bimodal);
}

// 70/30: the minority sits far outside 5 MAD of the median.
void test_bimodal_minority_cluster() {
  std::vector<int64_t> v(14, 137);
  for (int i = 0; i < 6; ++i) v.push_back(1337);
  CHECK(summarize_sync(lags(v)).bimodal);
}

// Outliers scattered on both sides, each side under 20 %, together over it: not one answer.
void test_bimodal_scattered_outliers() {
  std::vector<int64_t> v(15, 137);
  for (int64_t o : {-4000, -3000, 3000, 4000, 9000}) v.push_back(o);
  CHECK(summarize_sync(lags(v)).bimodal);
}

// A smooth drift (an ASRC settling) or +-1 sample jitter is one cluster, however wide.
void test_not_bimodal_smooth() {
  std::vector<int64_t> drift;
  for (int i = 0; i < 40; ++i) drift.push_back(100 + i);
  CHECK(!summarize_sync(lags(drift)).bimodal);
  std::vector<int64_t> jitter;
  for (int i = 0; i < 40; ++i) jitter.push_back(137 + (i % 3) - 1);
  CHECK(!summarize_sync(lags(jitter)).bimodal);
  CHECK(!summarize_sync(lags({1, 1000, 1, 1})).bimodal);  // under 5 readings: never
}

void test_default_window() {
  CHECK_EQ(sliding_default_window(96000.0), static_cast<uint64_t>(16384));
  CHECK_EQ(sliding_default_window(48000.0), static_cast<uint64_t>(8192));
  CHECK_EQ(sliding_default_window(44100.0), static_cast<uint64_t>(8192));
}

void test_window_starts() {
  // Exactly fitting windows: 100..(100+4*10) with win 10 over [100, 150).
  auto s = sliding_window_starts(100, 150, 10, 10, 200);
  CHECK_EQ(s.size(), static_cast<size_t>(5));
  CHECK_EQ(s.front(), static_cast<uint64_t>(100));
  CHECK_EQ(s.back(), static_cast<uint64_t>(140));
  // Overlapping step.
  s = sliding_window_starts(0, 100, 40, 20, 200);
  CHECK_EQ(s.size(), static_cast<size_t>(4));  // 0 20 40 60
  CHECK_EQ(s.back(), static_cast<uint64_t>(60));
  // A partial window at the end is dropped.
  s = sliding_window_starts(0, 109, 10, 10, 200);
  CHECK_EQ(s.size(), static_cast<size_t>(10));
  CHECK_EQ(s.back() + 10, static_cast<uint64_t>(100));
  // Shorter than one window, or nothing at all.
  CHECK(sliding_window_starts(0, 9, 10, 10, 200).empty());
  CHECK(sliding_window_starts(50, 40, 10, 10, 200).empty());
  CHECK(sliding_window_starts(0, 100, 10, 0, 200).empty());
}

void test_window_starts_capped() {
  // 1001 windows would fit; 5 evenly spaced ones are taken, first and last included.
  const auto s = sliding_window_starts(0, 1010, 10, 1, 5);
  CHECK_EQ(s.size(), static_cast<size_t>(5));
  CHECK_EQ(s[0], static_cast<uint64_t>(0));
  CHECK_EQ(s[1], static_cast<uint64_t>(250));
  CHECK_EQ(s[4], static_cast<uint64_t>(1000));
  for (size_t i = 1; i < s.size(); ++i) CHECK(s[i] > s[i - 1]);
  CHECK(sliding_window_starts(0, 1010, 10, 1, 1) == std::vector<uint64_t>{0});
}

void test_trusted() {
  SyncSummary p = summarize_sync(lags({137, 137, 138, 137}, 999.0));
  CHECK(sync_trusted_pings(p));
  p = summarize_sync(lags({137, 137, 138, 137}, 2.5));  // ambiguous correlations
  CHECK(!sync_trusted_pings(p));
  p = summarize_sync(lags({100, 110, 120, 130, 140}, 999.0));  // MAD 10
  CHECK(!sync_trusted_pings(p));
  CHECK(!sync_trusted_pings(summarize_sync({})));

  std::vector<int64_t> v(25, 137);
  v[3] = 138;
  CHECK(sync_trusted_sliding(summarize_sync(lags(v)), 96000.0, 16384));
  CHECK(!sync_trusted_sliding(summarize_sync(lags(std::vector<int64_t>(9, 137))), 96000.0, 16384));
  CHECK(sync_trusted_sliding(summarize_sync(lags(std::vector<int64_t>(10, 137))), 96000.0, 16384));
  // The same agreement in a small window proves nothing: every lag is confined to it.
  CHECK(sync_trusted_sliding(summarize_sync(lags(v)), 96000.0, 4096));
  CHECK(!sync_trusted_sliding(summarize_sync(lags(v)), 96000.0, 2048));
  CHECK(!sync_trusted_sliding(summarize_sync(lags(v)), 48000.0, 1024));
  CHECK(sync_trusted_sliding(summarize_sync(lags(v)), 48000.0, 2048));
  // A stdev under 0.5 ms but over 1/64 of the window: a spread-out guess, not a measurement.
  std::vector<int64_t> w;
  for (int i = 0; i < 25; ++i) w.push_back(-8 + (i - 12) * 5);  // stdev 36 = 0.375 ms
  CHECK(!sync_trusted_sliding(summarize_sync(lags(w)), 96000.0, 2048));
  CHECK(sync_trusted_sliding(summarize_sync(lags(w)), 96000.0, 4096));
  v[5] = 137 + 96000;  // one window a second off: stdev ~19 ms
  CHECK(!sync_trusted_sliding(summarize_sync(lags(v)), 96000.0, 16384));
}

}  // namespace

int main() {
  test_empty_has_no_readings();
  test_odd_count();
  test_even_count_averages_the_two_middles();
  test_low_confidence_still_counts();
  test_negative_lags();
  test_robust_stats();
  test_mad_and_stdev_small();
  test_one_reading();
  test_bimodal_even_split();
  test_bimodal_minority_cluster();
  test_bimodal_scattered_outliers();
  test_not_bimodal_smooth();
  test_default_window();
  test_window_starts();
  test_window_starts_capped();
  test_trusted();
  return report("genie");
}
