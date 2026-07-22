#include "genie.h"

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

}  // namespace

int main() {
  test_empty_has_no_readings();
  test_odd_count();
  test_even_count_averages_the_two_middles();
  test_low_confidence_still_counts();
  test_negative_lags();
  return report("genie");
}
