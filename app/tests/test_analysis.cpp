// The scope's envelope columns: a fixed 1/200 s of capture each, so their width in frames follows
// the engine's rate.
#include "analysis.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "channel_layout.h"
#include "check.h"
#include "constants.h"
#include "rates.h"
#include "ring_buffer.h"
#include "util/dsp.h"

using namespace st;

namespace {

// What column k's first sample holds on channel 0, and, negated, what its last sample holds: the
// column's min and max. Anything else is silence, so a column cut at the wrong frame would miss one
// of them or take a neighbour's.
float spike(uint64_t k) { return static_cast<float>(1 + k % 100) / 256.0f; }

void test_columns_are_a_two_hundredth_of_a_second(unsigned rate) {
  const unsigned frames = rate / 200;
  CHECK_EQ(env_column_frames(rate), frames);

  // Longer than the analysis thread's warm-up, which is its widest window.
  const uint64_t total = 48000;
  RingBuffer ring(1u << 16, st::channels().total(), 2 * kTestPeriod);
  std::vector<float> block(static_cast<size_t>(kTestPeriod) * st::channels().total());
  for (uint64_t n = 0; n < total; n += kTestPeriod) {
    std::fill(block.begin(), block.end(), 0.0f);
    for (unsigned i = 0; i < kTestPeriod; ++i) {
      const uint64_t t = n + i;
      if (t % frames == 0) block[i * st::channels().total()] = -spike(t / frames);
      if (t % frames == frames - 1) block[i * st::channels().total()] = spike(t / frames);
    }
    ring.write(block.data(), kTestPeriod);
  }

  Analysis analysis(ring, rate);
  CHECK_EQ(analysis.env_column_frames(), frames);
  analysis.start();
  const uint64_t want = total / frames;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (analysis.envelope().head() < want && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  analysis.stop();

  uint64_t first = 0;
  const std::vector<EnvColumn> cols = analysis.envelope().since(0, &first);
  CHECK_EQ(first, 0u);
  CHECK_EQ(cols.size(), static_cast<size_t>(want));
  bool exact = true;
  for (size_t k = 0; k < cols.size(); ++k) {
    exact &= cols[k].min[0] == float_to_s16(-spike(k));
    exact &= cols[k].max[0] == float_to_s16(spike(k));
    exact &= cols[k].min[1] == 0 && cols[k].max[1] == 0;
  }
  CHECK(exact);
}

// The Pi's columns are the 480 frames they were as a constant.
void test_the_pi_keeps_480() {
  CHECK_EQ(env_column_frames(96000), 480u);
  CHECK_EQ(env_column_frames(48000), 240u);
  // A rate too low to make sense still leaves the analysis thread something to divide by.
  CHECK_EQ(env_column_frames(0), 1u);
}

// A single partial in an otherwise empty spectrum: a tone, at that bin, at that level.
void test_tone_single_partial() {
  std::vector<float> power(4097, 0.0f);
  const double bin_hz = 48000.0 / 8192;
  power[171] = 1e-6f;  // -60 dBFS at ~1002 Hz
  const ToneMetrics t = tone_from_power(power, bin_hz);
  CHECK(t.valid);
  CHECK(t.reason == ToneReason::None);
  CHECK_NEAR(t.freq_hz, 171 * bin_hz, 1e-2);
  CHECK_NEAR(t.peak_hz, t.freq_hz, 1e-6);
  CHECK_NEAR(t.peak_db, -60.0, 1e-3);
  CHECK_NEAR(t.thd_n_pct, 0.0, 1e-6);
}

// The floor moved from -40 to -80 dBFS: a -70 dB tone is a tone, a -85 dB one is "silent" but
// still says where its dominant bin is.
void test_tone_floor() {
  const double bin_hz = 96000.0 / 8192;
  std::vector<float> power(4097, 0.0f);
  power[100] = 1e-7f;  // -70 dB
  CHECK(tone_from_power(power, bin_hz).valid);
  power[100] = static_cast<float>(std::pow(10.0, -8.5));  // -85 dB
  const ToneMetrics t = tone_from_power(power, bin_hz);
  CHECK(!t.valid);
  CHECK(t.reason == ToneReason::Silent);
  CHECK_EQ(t.freq_hz, 0.0f);
  CHECK_NEAR(t.peak_db, -85.0, 1e-3);
  CHECK_NEAR(t.peak_hz, 100 * bin_hz, 1e-2);
  CHECK_EQ(t.thd_n_pct, 0.0f);

  const ToneMetrics zero = tone_from_power(std::vector<float>(4097, 0.0f), bin_hz);
  CHECK(!zero.valid);
  CHECK(zero.reason == ToneReason::Silent);
  CHECK_EQ(zero.peak_db, kMinDb);
  CHECK(tone_from_power({}, bin_hz).reason == ToneReason::Silent);
}

// Two equal partials (or broadband content) are not one tone: THD+N past 30 % is "not_a_tone",
// with THD+N kept and no frequency.
void test_tone_purity() {
  const double bin_hz = 48000.0 / 8192;
  std::vector<float> power(4097, 0.0f);
  power[171] = 1e-4f;
  power[400] = 1e-4f;
  const ToneMetrics t = tone_from_power(power, bin_hz);
  CHECK(!t.valid);
  CHECK(t.reason == ToneReason::NotATone);
  CHECK_NEAR(t.thd_n_pct, 100.0, 1e-3);
  CHECK_EQ(t.freq_hz, 0.0f);
  CHECK(t.peak_db > -41.0f);
  // A harmonic 20 dB down is 10 % THD: still a tone.
  power[400] = 1e-6f;
  const ToneMetrics h = tone_from_power(power, bin_hz);
  CHECK(h.valid);
  CHECK_NEAR(h.thd_n_pct, 10.0, 1e-3);
  // Flat noise: every bin alike, far past the purity rule.
  std::fill(power.begin(), power.end(), 1e-8f);
  power[1000] = 2e-8f;
  CHECK(tone_from_power(power, bin_hz).reason == ToneReason::NotATone);
}

// Through the analysis thread: a quiet sine is a tone, noise is not, -90 dB is silent; and the
// unheld peak drops with the signal while the held one and last_loud remember it.
void test_meters_and_tone_live() {
  const unsigned rate = 48000;
  const unsigned nch = st::channels().total();
  RingBuffer ring(1u << 17, nch, 2 * kTestPeriod);
  std::vector<float> block(static_cast<size_t>(kTestPeriod) * nch);
  uint32_t lcg = 1;
  auto noise = [&lcg] {
    lcg = lcg * 1664525u + 1013904223u;
    return static_cast<float>(lcg >> 8) / static_cast<float>(1u << 24) * 2.0f - 1.0f;
  };
  uint64_t n = 0;
  auto write = [&](uint64_t frames, bool loud) {
    for (uint64_t end = n + frames; n < end; n += kTestPeriod) {
      std::fill(block.begin(), block.end(), 0.0f);
      for (unsigned i = 0; loud && i < kTestPeriod; ++i) {
        const double ph = kTwoPi * 1000.0 * static_cast<double>(n + i) / rate;
        block[i * nch + 0] = static_cast<float>(db_to_lin(-70.0f) * std::sin(ph));
        block[i * nch + 1] = 0.05f * noise();
        block[i * nch + 2] = static_cast<float>(db_to_lin(-90.0f) * std::sin(ph));
      }
      ring.write(block.data(), kTestPeriod);
    }
  };
  write(32768, true);

  Analysis analysis(ring, rate);
  analysis.start();
  auto wait_for = [&](auto pred) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!pred(analysis.snapshot()) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return analysis.snapshot();
  };
  AnalysisSnapshot s = wait_for([](const AnalysisSnapshot& a) {
    return a.sample > 0 && a.tone[0].valid;
  });
  CHECK(s.tone[0].valid);
  CHECK_NEAR(s.tone[0].freq_hz, 1000.0, 1.0);
  CHECK_NEAR(s.tone[0].peak_db, -70.0, 1.0);
  CHECK(s.tone[1].reason == ToneReason::NotATone);
  CHECK(s.tone[1].thd_n_pct > 100.0f);
  CHECK(s.tone[2].reason == ToneReason::Silent);
  CHECK_NEAR(s.tone[2].peak_hz, 1000.0, 6.0);
  CHECK_NEAR(s.meters[0].peak_now_db, -70.0, 0.5);
  CHECK(!s.meters[0].loud_seen);  // -70 dB is under the genie's -60 dB "sound"
  CHECK(s.meters[1].loud_seen);
  CHECK_EQ(s.meters[1].last_loud, s.sample);
  CHECK(!s.meters[3].loud_seen);
  CHECK_EQ(s.meters[3].peak_now_db, kMinDb);

  // Half a second of silence: past the 100 ms window, well inside the 3 s hold.
  const uint64_t loud_end = n;
  write(rate / 2, false);
  s = wait_for([&](const AnalysisSnapshot& a) { return a.sample == n; });
  analysis.stop();
  CHECK_EQ(s.sample, n);
  CHECK_EQ(s.meters[1].peak_now_db, kMinDb);
  CHECK(s.meters[1].peak_db > -30.0f);  // held
  CHECK(s.meters[1].loud_seen);
  CHECK(s.meters[1].last_loud <= loud_end + rate / 10 + kTestPeriod);
  CHECK(s.meters[1].last_loud < s.sample);
}

// The meter's peak hold, fed 100 ms windows the way the analysis thread does.
struct HoldFeed {
  unsigned rate;
  PeakHold hold;
  std::vector<float> sig;  // the whole signal, from sample 0
  explicit HoldFeed(unsigned r, double seconds)
      : rate(r),
        hold(static_cast<uint64_t>(std::llround(kPeakHoldS * r)), r / 200),
        sig(static_cast<size_t>(seconds * r), 0.0f) {}
  void burst(double at_s, double ms, float amp) {
    const size_t a = static_cast<size_t>(at_s * rate), n = static_cast<size_t>(ms * rate / 1000);
    for (size_t i = 0; i < n; ++i) sig[a + i] = (i % 2 ? -amp : amp);
  }
  // The held peak (dBFS) after each 100 ms tick, by the tick's end sample.
  std::vector<std::pair<uint64_t, float>> run() {
    std::vector<std::pair<uint64_t, float>> out;
    const size_t w = rate / 10;
    for (size_t end = w; end <= sig.size(); end += w) {
      hold.add(sig.data() + end - w, w, end - w);
      out.push_back({end, lin_to_db(hold.value(end))});
    }
    return out;
  }
};

// A single 1 ms burst at -20 dBFS, then silence: held the full 3 s after it, then released.
void test_peak_hold_single_burst() {
  for (const unsigned rate : {48000u, 96000u}) {
    HoldFeed f(rate, 6.0);
    const double at = 1.234;
    f.burst(at, 1.0, 0.1f);
    const uint64_t b = static_cast<uint64_t>(at * rate);
    double held_until = 0, released_at = 0;
    for (const auto& [end, db] : f.run()) {
      if (end <= b) {
        CHECK_EQ(db, kMinDb);
        continue;
      }
      const double after = static_cast<double>(end - b) / rate;
      if (db > -20.5f) {
        CHECK_NEAR(db, -20.0, 0.01);
        CHECK(released_at == 0);  // never held again once released
        held_until = after;
      } else {
        CHECK_EQ(db, kMinDb);
        if (released_at == 0) released_at = after;
      }
    }
    CHECK(held_until >= 2.9);
    CHECK(held_until <= kPeakHoldS + 0.001);
    CHECK(released_at > kPeakHoldS);
    CHECK(released_at <= kPeakHoldS + 0.1 + 0.001);
  }
}

// A smaller transient during a louder one's hold is held its own full hold; a louder one re-arms.
void test_peak_hold_rearm() {
  const unsigned rate = 48000;
  HoldFeed f(rate, 9.0);
  f.burst(0.5, 1.0, 0.5f);   // -6 dB
  f.burst(2.3, 1.0, 0.1f);   // -20 dB, 1.8 s into the -6 dB one's hold
  f.burst(6.0, 1.0, 0.05f);  // -26 dB
  f.burst(6.5, 1.0, 0.2f);   // -14 dB: re-arms over the -26
  auto at = [&](const std::vector<std::pair<uint64_t, float>>& v, double t) {
    for (const auto& [end, db] : v)
      if (end >= static_cast<uint64_t>(t * rate)) return db;
    return kMinDb;
  };
  const auto v = f.run();
  CHECK_NEAR(at(v, 3.4), -6.0, 0.1);    // the loud one, 2.9 s on
  CHECK_NEAR(at(v, 3.6), -20.0, 0.1);   // its hold ran out: the -20 one's is still running
  CHECK_NEAR(at(v, 5.2), -20.0, 0.1);   // 2.9 s after the -20 one
  CHECK_EQ(at(v, 5.4), kMinDb);
  CHECK_NEAR(at(v, 6.2), -26.0, 0.1);
  CHECK_NEAR(at(v, 6.7), -14.0, 0.1);
  CHECK_NEAR(at(v, 8.9), -14.0, 0.1);
}

// A steady tone holds its own level, and a window that overlaps the last one counts nothing twice.
void test_peak_hold_steady() {
  const unsigned rate = 48000;
  PeakHold h(static_cast<uint64_t>(kPeakHoldS * rate), rate / 200);
  std::vector<float> x(rate / 10);
  uint64_t n = 0;
  for (int t = 0; t < 50; ++t) {
    for (size_t i = 0; i < x.size(); ++i)
      x[i] = static_cast<float>(0.25 * std::sin(kTwoPi * 1000.0 * static_cast<double>(n + i) / rate));
    // Overlapping by half: the analysis thread's windows may.
    h.add(x.data(), x.size(), n);
    n += x.size() / 2;
    CHECK_NEAR(lin_to_db(h.value(n + x.size() / 2)), lin_to_db(0.25f), 0.01);
  }
  CHECK_EQ(h.value(n + 10 * rate), 0.0f);
}

}  // namespace

int main() {
  test_peak_hold_single_burst();
  test_peak_hold_rearm();
  test_peak_hold_steady();
  test_tone_single_partial();
  test_tone_floor();
  test_tone_purity();
  test_meters_and_tone_live();
  for (const unsigned rate : kTestRates) {
    std::printf("  at %u Hz\n", rate);
    test_columns_are_a_two_hundredth_of_a_second(rate);
  }
  test_the_pi_keeps_480();
  return report("analysis");
}
