#include "generators.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

#include "check.h"
#include "control.h"
#include "util/dsp.h"

using namespace st;

namespace {

constexpr double kRate = 96000.0;

// Runs the generator in blocks, exactly as the audio thread does.
void render_blocks(Generators& g, Control& ctl, PingLog& log, size_t blocks, size_t period,
                   std::vector<float>* sine, std::vector<float>* noise, std::vector<float>* ping) {
  std::vector<float> bs(period), bn(period), bp(period);
  for (size_t b = 0; b < blocks; ++b) {
    g.render(b * period, period, ctl, bs.data(), bn.data(), bp.data(), nullptr, log);
    if (sine) sine->insert(sine->end(), bs.begin(), bs.end());
    if (noise) noise->insert(noise->end(), bn.begin(), bn.end());
    if (ping) ping->insert(ping->end(), bp.begin(), bp.end());
  }
}

void test_ping_spacing_is_exact() {
  // The period (1024) does not divide the ping interval, which is exactly the case a
  // naive "n % interval == 0" block-start test would never fire on.
  Control ctl;
  Generators g;
  g.init(kRate);
  PingLog log;

  ctl.ping.interval_s.store(0.5f);
  ctl.ping.variant.store(static_cast<uint8_t>(PingVariant::Tick));
  ctl.ping.level_db.store(-6.0f);
  ctl.ping.epoch.fetch_add(1);

  const size_t period = 1024;
  render_blocks(g, ctl, log, 600, period, nullptr, nullptr, nullptr);  // ~6.4 s

  const auto pings = log.recent();
  CHECK(pings.size() >= 10);

  const uint64_t expected = static_cast<uint64_t>(std::llround(0.5 * kRate));
  for (size_t i = 1; i < pings.size(); ++i) {
    CHECK_EQ(pings[i].sample - pings[i - 1].sample, expected);
  }
  for (const auto& p : pings) CHECK_EQ(static_cast<int>(p.variant), 0);
}

void test_ping_energy_lands_at_logged_sample() {
  Control ctl;
  Generators g;
  g.init(kRate);
  PingLog log;

  ctl.ping.interval_s.store(0.5f);
  ctl.ping.level_db.store(0.0f);
  ctl.ping.epoch.fetch_add(1);

  std::vector<float> ping;
  const size_t period = 1024;
  render_blocks(g, ctl, log, 200, period, nullptr, nullptr, &ping);

  const auto pings = log.recent();
  CHECK(!pings.empty());
  if (pings.empty()) return;

  const uint64_t at = pings[0].sample;
  CHECK(at < ping.size());

  // Silent right before the logged sample, loud right after: the burst starts exactly there.
  float before = 0.0f;
  for (uint64_t i = at > 200 ? at - 200 : 0; i < at; ++i) before = std::max(before, std::fabs(ping[i]));
  float after = 0.0f;
  for (uint64_t i = at; i < at + 200 && i < ping.size(); ++i) after = std::max(after, std::fabs(ping[i]));

  CHECK(before < 1e-6f);
  CHECK(after > 0.3f);
}

void test_ping_interval_change_reschedules() {
  Control ctl;
  Generators g;
  g.init(kRate);
  PingLog log;

  ctl.ping.interval_s.store(0.5f);
  ctl.ping.epoch.fetch_add(1);
  render_blocks(g, ctl, log, 200, 1024, nullptr, nullptr, nullptr);
  const size_t before = log.recent().size();
  CHECK(before >= 3);

  // Change the interval mid-stream on the SAME generator: the epoch bump must make it
  // reschedule from the current sample, and every gap after that uses the new interval.
  ctl.ping.interval_s.store(1.0f);
  ctl.ping.epoch.fetch_add(1);
  std::vector<float> bs(1024), bn(1024), bp(1024);
  for (size_t b = 200; b < 800; ++b) {
    g.render(b * 1024, 1024, ctl, bs.data(), bn.data(), bp.data(), nullptr, log);
  }

  const auto pings = log.recent();
  CHECK(pings.size() >= before + 3);
  const uint64_t expected = static_cast<uint64_t>(std::llround(1.0 * kRate));
  // The gap straddling the change mixes both schedules; every gap after it is the new one.
  for (size_t i = before + 1; i < pings.size(); ++i) {
    CHECK_EQ(pings[i].sample - pings[i - 1].sample, expected);
  }
}

// Outputs play at n but capture can be held back, so the log has to name where a ping will be
// SEEN in the ring rather than where it was emitted. Get this wrong and the scope's ping markers
// and genie/sync both aim a whole delay away from the arrival they are looking for — and they
// fail silently, finding noise instead of a peak.
void test_ping_log_carries_the_capture_delay() {
  Control ctl;
  Generators g;
  g.init(kRate);
  ctl.ping.interval_s.store(0.5f);
  ctl.ping.level_db.store(-6.0f);

  const uint64_t offset = 96000;  // one second at 96 kHz
  const size_t period = 1024;
  std::vector<float> bs(period), bn(period), bp(period);

  PingLog plain, shifted;
  Generators g2;
  g2.init(kRate);
  for (size_t b = 0; b < 200; ++b) {
    g.render(b * period, period, ctl, bs.data(), bn.data(), bp.data(), nullptr, plain, 0);
    g2.render(b * period, period, ctl, bs.data(), bn.data(), bp.data(), nullptr, shifted, offset);
  }

  const auto a = plain.recent();
  const auto d = shifted.recent();
  CHECK(!a.empty());
  CHECK_EQ(a.size(), d.size());
  for (size_t i = 0; i < a.size(); ++i) {
    CHECK_EQ(d[i].sample, a[i].sample + offset);
    CHECK_EQ(d[i].variant, a[i].variant);
  }
}

void test_ping_log_wraps_keeping_the_newest_entries() {
  PingLog log;
  const size_t total = kPingLogEntries + 40;
  for (size_t i = 0; i < total; ++i) {
    log.push(1000 * i, static_cast<uint8_t>(i % 3));
  }

  // recent() after wrapping: exactly the newest kPingLogEntries, in order, with the variant
  // unpacked from the same word as the sample.
  const auto pings = log.recent();
  CHECK_EQ(pings.size(), kPingLogEntries);
  for (size_t i = 0; i < pings.size(); ++i) {
    const size_t k = total - kPingLogEntries + i;
    CHECK_EQ(pings[i].sample, 1000 * k);
    CHECK_EQ(static_cast<int>(pings[i].variant), static_cast<int>(k % 3));
  }
}

void test_sine_frequency_and_level() {
  Control ctl;
  Generators g;
  g.init(kRate);
  PingLog log;

  ctl.sine.freq_hz.store(1000.0f);
  ctl.sine.level_db.store(-6.0f);

  std::vector<float> sine;
  render_blocks(g, ctl, log, 100, 1024, &sine, nullptr, nullptr);

  double sum2 = 0.0;
  for (float v : sine) sum2 += static_cast<double>(v) * v;
  const double rms = std::sqrt(sum2 / sine.size());
  // A sine of amplitude a has RMS a/sqrt(2).
  CHECK_NEAR(rms, db_to_lin(-6.0f) / std::sqrt(2.0), 0.005);

  // Count zero crossings: 1 kHz over the rendered span.
  size_t crossings = 0;
  for (size_t i = 1; i < sine.size(); ++i) {
    if ((sine[i - 1] < 0.0f) != (sine[i] < 0.0f)) ++crossings;
  }
  const double seconds = static_cast<double>(sine.size()) / kRate;
  CHECK_NEAR(crossings / (2.0 * seconds), 1000.0, 2.0);
}

void test_sine_phase_is_continuous_across_blocks() {
  Control ctl;
  Generators g;
  g.init(kRate);
  PingLog log;
  ctl.sine.freq_hz.store(997.0f);
  ctl.sine.level_db.store(0.0f);

  std::vector<float> sine;
  render_blocks(g, ctl, log, 4, 1024, &sine, nullptr, nullptr);

  // A discontinuity at a block seam would show up as a sample-to-sample jump far larger
  // than the per-sample step of a 997 Hz sine.
  const float max_step = static_cast<float>(2.0 * kPi * 997.0 / kRate) * 1.5f;
  for (size_t i = 1; i < sine.size(); ++i) {
    CHECK(std::fabs(sine[i] - sine[i - 1]) < max_step);
  }
}

void test_noise_is_bounded_and_nonzero() {
  Control ctl;
  Generators g;
  g.init(kRate);
  PingLog log;
  ctl.noise.level_db.store(-6.0f);

  for (uint8_t mode : {0, 1}) {
    ctl.noise.mode.store(mode);
    std::vector<float> noise;
    render_blocks(g, ctl, log, 50, 1024, nullptr, &noise, nullptr);

    double sum2 = 0.0;
    for (float v : noise) {
      CHECK(std::isfinite(v));
      CHECK(std::fabs(v) < 1.5f);
      sum2 += static_cast<double>(v) * v;
    }
    CHECK(std::sqrt(sum2 / noise.size()) > 1e-3);
  }
}


// ---- Music -------------------------------------------------------------------------------------

// Renders [start, start + total) of the melody bus in blocks of `block` frames.
std::vector<float> render_music(Generators& g, uint64_t start, size_t total, size_t block,
                                float amp) {
  std::vector<float> out(total);
  for (size_t done = 0; done < total; done += block) {
    const size_t len = std::min(block, total - done);
    g.render_music(start + done, len, amp, out.data() + done);
  }
  return out;
}

// The melody is a pure function of the absolute index. Block size must not matter, and neither
// may when rendering began — a sink that starts listening mid-tune, or an engine that skipped the
// blocks nobody was routed to, hears exactly what it would have heard anyway.
void test_music_is_pure_in_n() {
  Generators a, b, c;
  a.init(kRate);
  b.init(kRate);
  c.init(kRate);
  const size_t total = 3 * 96000;
  const uint64_t start = 5 * 96000 + 123;  // mid-loop, mid-note
  const auto x = render_music(a, start, total, 1024, 0.5f);
  const auto y = render_music(b, start, total, 777, 0.5f);
  CHECK(x == y);

  // A fresh instance that starts half-way through, against the tail of the first rendering.
  const size_t skip = 100000;
  const auto z = render_music(c, start + skip, total - skip, 512, 0.5f);
  bool same = true;
  for (size_t i = 0; i < z.size(); ++i) same &= z[i] == x[skip + i];
  CHECK(same);

  // And it goes through Generators::render unchanged.
  Control ctl;
  ctl.music.level_db.store(-6.0f);
  PingLog log;
  std::vector<float> bs(1024), bn(1024), bp(1024), bm(1024), direct(1024);
  Generators d;
  d.init(kRate);
  d.render(start, 1024, ctl, bs.data(), bn.data(), bp.data(), bm.data(), log);
  d.render_music(start, 1024, db_to_lin(-6.0f), direct.data());
  CHECK(bm == direct);
}

void test_music_loops_seamlessly() {
  Generators g;
  g.init(kRate);
  const uint64_t L = g.music_loop_frames();
  CHECK_EQ(L, static_cast<uint64_t>(12.8 * kRate));

  // Exactly periodic: loop k and loop k+1 are the same samples.
  const auto x = render_music(g, 0, 2 * L, 1024, 1.0f);
  bool periodic = true;
  for (size_t i = 0; i < L; ++i) periodic &= x[i] == x[i + L];
  CHECK(periodic);

  // And not trivially so: there is sound in the loop.
  float peak = 0.0f;
  for (size_t i = 0; i < L; ++i) peak = std::max(peak, std::fabs(x[i]));
  CHECK(peak > 0.5f);
}

// No clicks anywhere, including the loop seam and every note boundary: the largest step between
// adjacent samples stays within what the fastest partial at full envelope can produce. A note cut
// off mid-waveform would jump by a sizeable fraction of full scale.
void test_music_has_no_clicks() {
  Generators g;
  g.init(kRate);
  const uint64_t L = g.music_loop_frames();
  const float amp = 1.0f;
  const auto x = render_music(g, L - 96000, 2 * 96000 + L, 1000, amp);  // spans a seam and a loop

  const double f_max = 440.0 * std::pow(2.0, (79 - 69) / 12.0);  // G5, the highest note
  // Carrier slope w(1 + 2 * 0.25) plus the 5 ms attack's own slope, with a margin.
  const double bound = 1.2 * amp * (2.0 * kPi * f_max * 1.5 + 1.0 / 0.005) / kRate;
  double worst = 0.0;
  for (size_t i = 1; i < x.size(); ++i)
    worst = std::max(worst, static_cast<double>(std::fabs(x[i] - x[i - 1])));
  CHECK(worst <= bound);
  CHECK(worst > 0.1 * bound);  // it does move
}

void test_music_level() {
  Generators g;
  g.init(kRate);
  const uint64_t L = g.music_loop_frames();
  for (float db : {0.0f, -20.0f}) {
    const float amp = db_to_lin(db);
    const auto x = render_music(g, 0, L, 4096, amp);
    float peak = 0.0f;
    for (float v : x) {
      CHECK(std::isfinite(v));
      peak = std::max(peak, std::fabs(v));
    }
    CHECK(peak <= amp);
    CHECK(peak >= 0.5f * amp);
  }
}

// The tune lasts the same time at any rate: the grid is in seconds, not frames.
void test_music_is_rate_independent() {
  Generators a, b;
  a.init(48000.0);
  b.init(96000.0);
  CHECK_EQ(b.music_loop_frames(), 2 * a.music_loop_frames());
  CHECK_EQ(a.music_loop_frames(), static_cast<uint64_t>(12.8 * 48000));
}

}  // namespace

int main() {
  test_ping_spacing_is_exact();
  test_ping_energy_lands_at_logged_sample();
  test_ping_interval_change_reschedules();
  test_ping_log_wraps_keeping_the_newest_entries();
  test_ping_log_carries_the_capture_delay();
  test_sine_frequency_and_level();
  test_sine_phase_is_continuous_across_blocks();
  test_noise_is_bounded_and_nonzero();
  test_music_is_pure_in_n();
  test_music_loops_seamlessly();
  test_music_has_no_clicks();
  test_music_level();
  test_music_is_rate_independent();
  return report("generators");
}
