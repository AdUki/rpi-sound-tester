#include "generators.h"

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
    g.render(b * period, period, ctl, bs.data(), bn.data(), bp.data(), log);
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

  ctl.ping.interval_s.store(1.0f);
  ctl.ping.epoch.fetch_add(1);
  PingLog log2;
  Generators g2;
  g2.init(kRate);
  render_blocks(g2, ctl, log2, 600, 1024, nullptr, nullptr, nullptr);

  const auto pings = log2.recent();
  CHECK(pings.size() >= 3);
  const uint64_t expected = static_cast<uint64_t>(std::llround(1.0 * kRate));
  for (size_t i = 1; i < pings.size(); ++i) {
    CHECK_EQ(pings[i].sample - pings[i - 1].sample, expected);
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
  const float max_step = 2.0f * 3.14159f * 997.0f / static_cast<float>(kRate) * 1.5f;
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

}  // namespace

int main() {
  test_ping_spacing_is_exact();
  test_ping_energy_lands_at_logged_sample();
  test_ping_interval_change_reschedules();
  test_sine_frequency_and_level();
  test_sine_phase_is_continuous_across_blocks();
  test_noise_is_bounded_and_nonzero();
  return report("generators");
}
