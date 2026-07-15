#include "capture.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>

#include "check.h"
#include "constants.h"
#include "ring_buffer.h"

using namespace st;

namespace {

constexpr double kRate = 96000.0;
constexpr unsigned kPeriod = 1024;

enum class Stimulus { Broadband, PingTrain };

// Fills the ring so that channel 1 carries channel 0's signal delayed by `delay` samples —
// the multiroom case.
void fill_with_delayed_copy(RingBuffer& ring, int64_t delay, uint64_t frames, Stimulus stim) {
  std::mt19937 rng(1234);
  std::uniform_real_distribution<float> floor_noise(-0.02f, 0.02f);
  std::uniform_real_distribution<float> broadband(-0.5f, 0.5f);

  std::vector<float> src(frames + 8192, 0.0f);
  if (stim == Stimulus::Broadband) {
    for (auto& v : src) v = broadband(rng);
  } else {
    // A repeating tick, exactly what the ping generator emits.
    for (uint64_t tick = 5000; tick < frames; tick += 30000) {
      for (unsigned i = 0; i < 600; ++i) {
        const double t = i / kRate;
        const float env = std::exp(-static_cast<float>(t) / 0.0004f);
        src[tick + i] += 0.8f * env * static_cast<float>(std::sin(2 * M_PI * 3000.0 * t));
      }
    }
  }

  std::vector<float> block(kPeriod * kInputs);
  for (uint64_t n = 0; n < frames; n += kPeriod) {
    for (unsigned i = 0; i < kPeriod; ++i) {
      const uint64_t t = n + i;
      for (unsigned c = 0; c < kInputs; ++c) block[i * kInputs + c] = floor_noise(rng);
      block[i * kInputs + 0] += src[t + 4096];
      const int64_t td = static_cast<int64_t>(t) + 4096 - delay;
      if (td >= 0 && td < static_cast<int64_t>(src.size())) block[i * kInputs + 1] += src[td];
    }
    ring.write(block.data(), kPeriod);
  }
}

void test_xcorr_recovers_a_known_delay() {
  for (int64_t delay : {0, 137, 4321, -960}) {
    RingBuffer ring(kRingFrames, kInputs, 2 * kPeriod);
    CaptureStore cap(ring, kRate, kPeriod);
    fill_with_delayed_copy(ring, delay, 400000, Stimulus::Broadband);

    const CaptureStatus cs = cap.freeze(0);
    CHECK(cs.frozen);

    const uint64_t len = 1 << 17;
    const uint64_t start = cs.valid_start + 5000;
    const XcorrResult r = cap.xcorr(0, 1, start, len);

    if (!r.ok) {
      std::cout << "  xcorr failed: " << r.error << "\n";
      CHECK(r.ok);
      continue;
    }
    std::cout << "  delay " << delay << " -> lag " << r.lag_samples << " (confidence "
              << r.confidence << ")\n";
    // Channel 1 lags channel 0 by `delay`, so a positive delay must give a positive lag.
    // Sample-exact is the requirement.
    CHECK_EQ(r.lag_samples, delay);
    CHECK(r.confidence > 3.0);
  }
}

// A repeating stimulus correlates with itself one ping-period away, so the delay is only
// known modulo the interval. The lag still comes out right, but the confidence must drop
// to say so.
void test_xcorr_reports_low_confidence_on_a_periodic_stimulus() {
  RingBuffer ring(kRingFrames, kInputs, 2 * kPeriod);
  CaptureStore cap(ring, kRate, kPeriod);
  fill_with_delayed_copy(ring, 500, 400000, Stimulus::PingTrain);

  const CaptureStatus cs = cap.freeze(0);
  CHECK(cs.frozen);

  // A window spanning several ticks: ambiguous by construction.
  const XcorrResult r = cap.xcorr(0, 1, cs.valid_start + 5000, 1 << 17);
  CHECK(r.ok);
  std::cout << "  periodic stimulus -> lag " << r.lag_samples << " (confidence " << r.confidence
            << ")\n";
  CHECK_EQ(r.lag_samples, 500);
  CHECK(r.confidence < 2.0);

  // A window bracketing exactly one tick is unambiguous again. Ticks land at ring sample
  // 904 + 30000k, so [25000, 41384) contains the k=1 tick and no other.
  const XcorrResult one = cap.xcorr(0, 1, cs.valid_start + 25000, 16384);
  CHECK(one.ok);
  std::cout << "  single tick      -> lag " << one.lag_samples << " (confidence "
            << one.confidence << ")\n";
  CHECK_EQ(one.lag_samples, 500);
  CHECK(one.confidence > 3.0);
}

void test_xcorr_needs_a_freeze() {
  RingBuffer ring(kRingFrames, kInputs, 2 * kPeriod);
  CaptureStore cap(ring, kRate, kPeriod);
  fill_with_delayed_copy(ring, 100, 200000, Stimulus::Broadband);

  const XcorrResult r = cap.xcorr(0, 1, 1000, 1 << 16);
  CHECK(!r.ok);
  CHECK(r.error.find("freeze") != std::string::npos);
}

void test_window_returns_raw_and_columns() {
  RingBuffer ring(kRingFrames, kInputs, 2 * kPeriod);
  CaptureStore cap(ring, kRate, kPeriod);
  fill_with_delayed_copy(ring, 0, 200000, Stimulus::Broadband);
  const CaptureStatus cs = cap.freeze(0);
  CHECK(cs.frozen);

  const WindowResult raw = cap.window(0, cs.valid_start + 100, 512, 1024);
  CHECK(raw.ok);
  CHECK(raw.raw);
  CHECK_EQ(raw.samples.size(), 512u);

  const WindowResult cols = cap.window(0, cs.valid_start + 100, 100000, 500);
  CHECK(cols.ok);
  CHECK(!cols.raw);
  CHECK_EQ(cols.mins.size(), 500u);
  CHECK_EQ(cols.maxs.size(), 500u);
  for (size_t i = 0; i < cols.mins.size(); ++i) CHECK(cols.mins[i] <= cols.maxs[i]);
}

void test_analyze_length_is_configurable() {
  RingBuffer ring(kRingFrames, kInputs, 2 * kPeriod);
  CaptureStore cap(ring, kRate, kPeriod);

  // Default: a modest fixed window (kCaptureDefaultSeconds), capped at capacity.
  CHECK(cap.capacity_frames() > 0);
  const uint64_t expect_default =
      std::min<uint64_t>(cap.capacity_frames(), static_cast<uint64_t>(kCaptureDefaultSeconds * kRate));
  CHECK_EQ(cap.analyze_frames(), expect_default);

  fill_with_delayed_copy(ring, 0, 400000, Stimulus::Broadband);

  // A shorter analyze length freezes only that many of the most recent frames.
  cap.set_analyze_frames(50000);
  CHECK_EQ(cap.analyze_frames(), 50000u);
  const CaptureStatus cs = cap.freeze(0);
  CHECK(cs.frozen);
  CHECK_EQ(cs.valid_len, 50000u);
  CHECK_EQ(cs.valid_start, cs.freeze_sample - 50000);

  // The window endpoint still serves within the (now shorter) frozen range, and rejects
  // anything reaching before its start.
  const WindowResult in = cap.window(0, cs.valid_start + 10, 1024, 512);
  CHECK(in.ok);
  const WindowResult before = cap.window(0, cs.valid_start - 100, 1024, 512);
  CHECK(!before.ok);

  // Out-of-band requests clamp instead of taking effect: too small pins to the floor, too
  // large pins to the capacity.
  cap.set_analyze_frames(1);
  CHECK_EQ(cap.analyze_frames(), kCaptureMinFrames);
  cap.set_analyze_frames(kRingFrames * 4);
  CHECK_EQ(cap.analyze_frames(), cap.capacity_frames());
}

void test_window_rejects_out_of_range() {
  RingBuffer ring(kRingFrames, kInputs, 2 * kPeriod);
  CaptureStore cap(ring, kRate, kPeriod);
  fill_with_delayed_copy(ring, 0, 200000, Stimulus::Broadband);
  const CaptureStatus cs = cap.freeze(0);

  const WindowResult bad = cap.window(0, cs.freeze_sample + 10000, 1024, 512);
  CHECK(!bad.ok);
  CHECK(!bad.error.empty());
}

}  // namespace

int main() {
  test_xcorr_recovers_a_known_delay();
  test_xcorr_reports_low_confidence_on_a_periodic_stimulus();
  test_xcorr_needs_a_freeze();
  test_window_returns_raw_and_columns();
  test_window_rejects_out_of_range();
  test_analyze_length_is_configurable();
  return report("capture");
}
