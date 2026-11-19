// GET /api/genie/watch's analyser on synthetic signals: a clean tone reads clean, and each kind of
// damage (a hole, a spike, a slip, a level step, a dropout) is found exactly once where it was
// made; music-like noise raises no clicks at the default sensitivity.
#include "genie_watch.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "check.h"

using namespace st;

namespace {

constexpr double kRate = 96000.0;

std::vector<float> sine(double hz, double amp, size_t n, double phase = 0.3) {
  std::vector<float> x(n);
  for (size_t i = 0; i < n; ++i)
    x[i] = static_cast<float>(amp * std::sin(2 * kPi * hz * static_cast<double>(i) / kRate + phase));
  return x;
}

WatchParams params(std::optional<double> tone = std::nullopt) {
  WatchParams p;
  p.rate = kRate;
  p.block = 960;
  p.tone_hz = tone;
  return p;
}

// Fed in uneven chunks, so nothing depends on chunk and block lining up; finish() rereads x.
WatchChannelResult analyse(const std::vector<float>& x, const WatchParams& p) {
  WatchAnalyzer a(p);
  size_t i = 0, k = 0;
  const size_t sizes[] = {4096, 777, 1, 9000, 333};
  while (i < x.size()) {
    const size_t n = std::min(sizes[k++ % 5], x.size() - i);
    a.feed(x.data() + i, n);
    i += n;
  }
  return a.finish([&x](uint64_t off, size_t n, float* out) {
    if (off + n > x.size()) return false;
    std::copy(x.begin() + off, x.begin() + off + n, out);
    return true;
  });
}

uint64_t count(const WatchChannelResult& r, WatchKind k) { return r.counts[static_cast<size_t>(k)]; }

uint64_t total(const WatchChannelResult& r) {
  uint64_t n = 0;
  for (uint64_t c : r.counts) n += c;
  return n;
}

const WatchEvent* first(const WatchChannelResult& r, WatchKind k) {
  for (const WatchEvent& e : r.events)
    if (e.kind == k) return &e;
  return nullptr;
}

void dump(const char* what, const WatchChannelResult& r) {
  if (total(r) == 0) return;
  std::printf("  %s:", what);
  for (const WatchEvent& e : r.events)
    std::printf(" %s@%llu+%llu", watch_kind_name(e.kind), static_cast<unsigned long long>(e.offset),
                static_cast<unsigned long long>(e.len));
  std::printf("\n");
}

void test_clean_sine() {
  const auto x = sine(1000, 0.5, 10 * 96000);
  const auto r = analyse(x, params(1000.0));
  dump("clean", r);
  CHECK_EQ(total(r), 0u);
  CHECK_EQ(r.samples, x.size());
  CHECK_NEAR(r.sound_pct, 100.0, 1e-9);
  CHECK_NEAR(r.rms_db_median, 20 * std::log10(0.5 / std::sqrt(2.0)), 0.05);
  CHECK_NEAR(r.peak_max_db, 20 * std::log10(0.5), 0.05);
  CHECK(r.tone.has_value());
  CHECK_NEAR(*r.tone->freq_hz_median, 1000.0, 0.01);
  CHECK(*r.tone->resid_dbc_max < -100);
  CHECK_EQ(r.tone->bad_blocks, 0u);
  // Without tone mode, too.
  const auto n = analyse(x, params());
  CHECK_EQ(total(n), 0u);
  CHECK(!n.tone.has_value());
}

// A tone a little off what was expected (auto takes the analysis' estimate) and not a whole
// number of cycles per block: still clean, and its frequency read from the phase advance.
void test_offset_frequency() {
  const auto x = sine(997.3, 0.3, 5 * 96000);
  const auto r = analyse(x, params(1000.0));
  dump("offset", r);
  CHECK_EQ(total(r), 0u);
  CHECK_NEAR(*r.tone->freq_hz_median, 997.3, 0.01);
  CHECK_NEAR(*r.tone->freq_hz_min, 997.3, 0.05);
  CHECK_NEAR(*r.tone->freq_hz_max, 997.3, 0.05);
}

void test_zero_run() {
  auto x = sine(1000, 0.5, 8 * 96000);
  const size_t at = 300123, len = 480;  // 5 ms
  std::fill(x.begin() + at, x.begin() + at + len, 0.0f);
  const auto r = analyse(x, params(1000.0));
  dump("zero_run", r);
  CHECK_EQ(count(r, WatchKind::ZeroRun), 1u);
  CHECK_EQ(total(r), 1u);  // its edges' clicks and the bad block fold into it
  const WatchEvent* e = first(r, WatchKind::ZeroRun);
  CHECK(e && e->offset == at && e->len == len);
  CHECK(e && e->click);
  CHECK(e && !e->slip_samples);  // a hole that kept its place: no slip
  // Without tone mode the same.
  const auto n = analyse(x, params());
  CHECK_EQ(count(n, WatchKind::ZeroRun), 1u);
  CHECK_EQ(total(n), 1u);
}

void test_spike() {
  auto x = sine(1000, 0.5, 8 * 96000);
  const size_t at = 400050;
  x[at] += 0.05f;
  for (const auto& p : {params(1000.0), params()}) {
    const auto r = analyse(x, p);
    dump("spike", r);
    CHECK_EQ(count(r, WatchKind::Click), 1u);
    CHECK_EQ(total(r), 1u);
    const WatchEvent* e = first(r, WatchKind::Click);
    CHECK(e && e->offset == at);
  }
}

void test_phase_slip() {
  const auto clean = sine(1000, 0.5, 8 * 96000);
  const size_t at = 500077, drop = 12;
  std::vector<float> x(clean.begin(), clean.begin() + at);
  x.insert(x.end(), clean.begin() + at + drop, clean.end());
  const auto r = analyse(x, params(1000.0));
  dump("slip", r);
  CHECK_EQ(count(r, WatchKind::PhaseSlip), 1u);
  CHECK_EQ(total(r), 1u);
  const WatchEvent* e = first(r, WatchKind::PhaseSlip);
  CHECK(e && e->offset == at);
  CHECK(e && e->slip_samples && std::fabs(*e->slip_samples - 12.0) < 0.1);
  CHECK(e && e->slip_deg && std::fabs(*e->slip_deg - 45.0) < 0.5);
  CHECK(e && e->click);
  // The other way: 8 samples repeated (30 degrees; slip_deg is 20).
  std::vector<float> y(clean.begin(), clean.begin() + at);
  y.insert(y.end(), clean.begin() + at - 8, clean.end());
  const auto r2 = analyse(y, params(1000.0));
  const WatchEvent* e2 = first(r2, WatchKind::PhaseSlip);
  CHECK_EQ(count(r2, WatchKind::PhaseSlip), 1u);
  CHECK(e2 && e2->slip_samples && std::fabs(*e2->slip_samples + 8.0) < 0.1);
  CHECK(e2 && e2->offset == at);
  // A slip at a block boundary.
  std::vector<float> z(clean.begin(), clean.begin() + 960 * 300);
  z.insert(z.end(), clean.begin() + 960 * 300 + 30, clean.end());
  const auto r3 = analyse(z, params(1000.0));
  CHECK_EQ(count(r3, WatchKind::PhaseSlip), 1u);
  CHECK_EQ(total(r3), 1u);
  const WatchEvent* e3 = first(r3, WatchKind::PhaseSlip);
  CHECK(e3 && e3->offset == 960 * 300);
  CHECK(e3 && std::fabs(*e3->slip_samples - 30.0) < 0.1);
}

// A clean tone that runs to the span's very end, whose last block is partial (a span that is not
// a whole number of blocks): no slip there. The partial block's phase was read at a full block's
// centre, past its last sample, and a few samples of tone with a little noise put it anywhere.
void test_partial_last_block() {
  std::mt19937 rng(3);
  std::normal_distribution<double> nd(0, 1);
  // {played, expected}: a little off, as auto's estimate is.
  for (const auto& [hz, expect] : {std::pair{997.0, 1000.0}, std::pair{437.0, 440.0}})
  for (const double noise_db : {-200.0, -90.0, -70.0, -50.0}) {
    for (const size_t extra : {1u, 12u, 16u, 17u, 20u, 40u, 100u, 479u, 500u, 959u}) {
      auto x = sine(hz, 0.5, 960 * 300 + extra);
      const double na = std::pow(10.0, noise_db / 20);
      for (float& v : x) v += static_cast<float>(na * nd(rng));
      const auto r = analyse(x, params(expect));
      if (count(r, WatchKind::PhaseSlip)) {
        std::printf("  noise %.0f dB, last block %zu samples:\n", noise_db, extra);
        dump("partial", r);
      }
      CHECK_EQ(count(r, WatchKind::PhaseSlip), 0u);
      CHECK_EQ(r.samples, x.size());
      // Nor a `resid` there: a few samples too short to fit are no bad block.
      if (noise_db < -60) CHECK_EQ(total(r), 0u);
      if (noise_db < -60 && total(r)) dump("partial", r);
    }
  }
}

// One block disturbed by something at the tone's own frequency: it still fits the tone, but up to
// ~35 degrees off. That is no slip — the tone is back in place on the next block — wherever the
// block is, the span's last ones included (where no later block was there to say so).
void test_off_phase_block_is_no_slip() {
  std::mt19937 rng(7);
  std::normal_distribution<double> nd(0, 1);
  const size_t n = 960 * 300;
  for (const size_t b0 : {n - 960, n - 1920, n - 2880, size_t{150 * 960}, size_t{960}, size_t{0}}) {
    for (int k = 0; k < 8; ++k) {
      auto x = sine(1000, 0.5, n);
      const double ph = 0.8 * k;
      for (size_t i = b0; i < b0 + 960; ++i)
        x[i] += static_cast<float>(0.3 * std::sin(2 * kPi * 1000.0 * static_cast<double>(i) / kRate + ph) +
                                   1e-4 * nd(rng));
      const auto r = analyse(x, params(1000.0));
      if (count(r, WatchKind::PhaseSlip)) dump("off-phase block", r);
      CHECK_EQ(count(r, WatchKind::PhaseSlip), 0u);
    }
  }
}

void test_level_step() {
  const size_t at = 600000;  // inside block 625
  auto x = sine(1000, 0.1, 10 * 96000);
  for (size_t i = at; i < x.size(); ++i) x[i] *= static_cast<float>(std::pow(10.0, 10.0 / 20));
  const auto r = analyse(x, params(1000.0));
  dump("step", r);
  CHECK_EQ(count(r, WatchKind::LevelStep), 1u);
  CHECK_EQ(total(r), 1u);
  const WatchEvent* e = first(r, WatchKind::LevelStep);
  CHECK(e && std::llabs(static_cast<long long>(e->offset) - static_cast<long long>(at)) <= 960);
  CHECK(e && std::fabs(*e->to_db - *e->from_db - 10.0) < 0.2);
  // Down, on noise, at a block boundary, without tone mode.
  std::mt19937 rng(7);
  std::normal_distribution<float> g(0.0f, 0.1f);
  std::vector<float> n(6 * 96000);
  for (size_t i = 0; i < n.size(); ++i) n[i] = g(rng) * (i >= 960 * 250 ? 0.25f : 1.0f);
  const auto rn = analyse(n, params());
  dump("noise step", rn);
  CHECK_EQ(count(rn, WatchKind::LevelStep), 1u);
  CHECK_EQ(total(rn), 1u);
  const WatchEvent* en = first(rn, WatchKind::LevelStep);
  CHECK(en && std::llabs(static_cast<long long>(en->offset) - 960 * 250) <= 960);
  CHECK(en && std::fabs(*en->to_db - *en->from_db + 12.04) < 0.3);
}

// A dropout to exact zeros (a network hole, a muted path): one gap, to the sample, with the slip
// across it when the tone came back shifted.
void test_gap() {
  auto x = sine(1000, 0.5, 8 * 96000);
  const size_t at = 333333, len = 9600 * 2 + 17;  // ~200 ms
  std::fill(x.begin() + at, x.begin() + at + len, 0.0f);
  const auto r = analyse(x, params(1000.0));
  dump("gap", r);
  CHECK_EQ(count(r, WatchKind::Gap), 1u);
  CHECK_EQ(total(r), 1u);
  const WatchEvent* e = first(r, WatchKind::Gap);
  CHECK(e && e->zeros && e->offset == at && e->len == len);
  CHECK(e && !e->slip_samples);
  CHECK_NEAR(r.silent_s, 0.2, 0.025);

  // Same hole, but the tone resumes 20 samples ahead (a sender that skipped).
  const auto clean = sine(1000, 0.5, 8 * 96000);
  std::vector<float> y(clean.begin(), clean.begin() + at);
  y.insert(y.end(), len, 0.0f);
  y.insert(y.end(), clean.begin() + at + len + 20, clean.end());
  const auto r2 = analyse(y, params(1000.0));
  dump("gap+slip", r2);
  CHECK_EQ(count(r2, WatchKind::Gap), 1u);
  CHECK_EQ(total(r2), 1u);
  const WatchEvent* e2 = first(r2, WatchKind::Gap);
  CHECK(e2 && e2->slip_samples && std::fabs(*e2->slip_samples - 20.0) < 0.2);
}

// An intermittent signal (the tester's tick: a short burst every 2 s on a noise floor) pauses
// between bursts; the pauses are not dropouts, nor its bursts onsets.
void test_intermittent_is_not_gaps() {
  std::mt19937 rng(5);
  std::normal_distribution<float> g(0.0f, 1e-5f);  // -100 dBFS floor
  std::vector<float> x(10 * 96000);
  for (size_t i = 0; i < x.size(); ++i) x[i] = g(rng);
  const auto burst = sine(2000, 0.1, 960 * 2);  // 20 ms
  for (size_t t = 48000; t + burst.size() < x.size(); t += 2 * 96000)
    for (size_t i = 0; i < burst.size(); ++i) x[t + i] += burst[i];
  const auto r = analyse(x, params());
  dump("ticks", r);
  CHECK_EQ(count(r, WatchKind::Gap), 0u);
  CHECK_EQ(count(r, WatchKind::Onset), 0u);
  CHECK_EQ(count(r, WatchKind::Offset), 0u);

  // A real dropout in a tone still is one, with another 100 ms away.
  auto y = sine(1000, 0.5, 4 * 96000);
  std::fill(y.begin() + 150000, y.begin() + 150000 + 4800, 0.0f);
  std::fill(y.begin() + 164400, y.begin() + 164400 + 4800, 0.0f);
  const auto r2 = analyse(y, params(1000.0));
  CHECK_EQ(count(r2, WatchKind::Gap), 2u);
}

// Fades are a track ending and the next starting, not a dropout: an offset and an onset.
void test_fades() {
  const size_t n = 6 * 96000;
  auto x = sine(440, 0.5, n);
  const size_t f0 = 96000, f1 = 96000 * 2, f2 = 96000 * 3, f3 = 96000 * 4;  // out, silent, in
  for (size_t i = f0; i < f3; ++i) {
    double g = 1;
    if (i < f1) g = std::pow(10.0, -80.0 * static_cast<double>(i - f0) / (f1 - f0) / 20);
    else if (i < f2) g = 0;
    else g = std::pow(10.0, -80.0 * static_cast<double>(f3 - i) / (f3 - f2) / 20);
    x[i] = static_cast<float>(x[i] * g);
  }
  const auto r = analyse(x, params());
  dump("fades", r);
  CHECK_EQ(count(r, WatchKind::Gap), 0u);
  CHECK_EQ(count(r, WatchKind::Click), 0u);
  CHECK_EQ(count(r, WatchKind::ZeroRun), 0u);
  CHECK_EQ(count(r, WatchKind::Onset), 0u);   // soft: no abrupt edge either side
  CHECK_EQ(count(r, WatchKind::Offset), 0u);

  // An abrupt start after silence, and an abrupt stop before the end: onset, offset.
  auto y = sine(440, 0.5, n);
  std::fill(y.begin(), y.begin() + 96000, 0.0f);
  std::fill(y.begin() + 5 * 96000, y.end(), 0.0f);
  const auto r2 = analyse(y, params());
  dump("edges", r2);
  CHECK_EQ(count(r2, WatchKind::Onset), 1u);
  CHECK_EQ(count(r2, WatchKind::Offset), 1u);
  CHECK_EQ(count(r2, WatchKind::Gap), 0u);
  CHECK_EQ(count(r2, WatchKind::ZeroRun), 0u);
  CHECK_EQ(count(r2, WatchKind::Click), 0u);  // the edges' clicks fold into them
  const WatchEvent* on = first(r2, WatchKind::Onset);
  CHECK(on && on->offset == 96000);
  const WatchEvent* off = first(r2, WatchKind::Offset);
  CHECK(off && off->offset == 5 * 96000 && off->len == 96000);
  CHECK_NEAR(r2.sound_pct, 400.0 / 6, 0.01);
}

// Music-like: pink-ish noise (a few one-pole low-passes summed) with a beat of attacks and
// decays, and a band-limited bass line. The default click_k must stay quiet on it.
std::vector<float> music_like(size_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> g(0.0f, 1.0f);
  std::vector<float> x(n);
  double l1 = 0, l2 = 0, l3 = 0, env = 0.2;
  bool attack = false;
  for (size_t i = 0; i < n; ++i) {
    const double w = g(rng);
    l1 += 0.5 * (w - l1);
    l2 += 0.05 * (w - l2);
    l3 += 0.005 * (w - l3);
    // A hit every 250 ms: a ~1 ms attack, then a decay.
    if (i % 24000 == 0) attack = true;
    if (attack) {
      env += (1.0 - env) * 0.01;
      attack = env < 0.99;
    } else {
      env = std::max(0.15, env * 0.99993);
    }
    const double bass = 0.2 * std::sin(2 * kPi * 55.0 * static_cast<double>(i) / kRate);
    x[i] = static_cast<float>(0.25 * env * (0.02 * l1 + 1.0 * l2 + 3.0 * l3) + bass * env);
  }
  return x;
}

void test_music_no_clicks() {
  for (uint32_t seed : {1u, 2u, 3u}) {
    const auto x = music_like(20 * 96000, seed);
    const auto r = analyse(x, params());
    dump("music", r);
    CHECK_EQ(count(r, WatchKind::Click), 0u);
    CHECK_EQ(count(r, WatchKind::Gap), 0u);
    CHECK_EQ(count(r, WatchKind::ZeroRun), 0u);
    CHECK_EQ(count(r, WatchKind::LevelStep), 0u);  // its dynamics are not steady stretches
  }
  // White noise at the default k.
  std::mt19937 rng(11);
  std::normal_distribution<float> g(0.0f, 0.2f);
  std::vector<float> w(20 * 96000);
  for (float& v : w) v = g(rng);
  const auto r = analyse(w, params());
  dump("white", r);
  CHECK_EQ(total(r), 0u);
  // And a spike in the music is still found.
  auto m = music_like(10 * 96000, 4);
  m[480011] += 0.2f;
  const auto rm = analyse(m, params());
  dump("music+spike", rm);
  CHECK_EQ(count(rm, WatchKind::Click), 1u);
  const WatchEvent* e = first(rm, WatchKind::Click);
  CHECK(e && e->offset == 480011);
}

// Hard-clipped samples are counted; a tone that is not there (expect_hz wrong) is one resid run.
void test_clip_and_wrong_tone() {
  auto x = sine(1000, 1.2, 96000);
  for (float& v : x) v = std::max(-1.0f, std::min(1.0f, v));
  const auto r = analyse(x, params());
  CHECK(r.clipped_samples > 10000);
  const auto t = analyse(sine(1500, 0.5, 96000), params(1000.0));
  dump("wrong tone", t);
  CHECK_EQ(count(t, WatchKind::Resid), 1u);
  CHECK_EQ(count(t, WatchKind::PhaseSlip), 0u);
  CHECK_EQ(t.tone->bad_blocks, t.tone->blocks);
}

void test_silence_and_short() {
  const std::vector<float> z(96000, 0.0f);
  const auto r = analyse(z, params(1000.0));
  CHECK_EQ(total(r), 0u);
  CHECK_NEAR(r.sound_pct, 0.0, 1e-9);
  CHECK_NEAR(r.silent_s, 1.0, 1e-9);
  CHECK_NEAR(r.rms_db_max, -120.0, 1e-9);
  WatchAnalyzer a(params());
  const auto e = a.finish();
  CHECK_EQ(e.blocks, 0u);
  const auto s = analyse(sine(1000, 0.5, 1000), params(1000.0));  // one block and a bit
  CHECK_EQ(s.blocks, 2u);
}

void test_merge() {
  WatchEvent a;
  a.kind = WatchKind::ZeroRun;
  a.offset = 1000;
  a.len = 480;
  WatchEvent b = a;
  b.offset = 1100;  // within 2 ms (192)
  b.len = 500;
  WatchEvent c = a;
  c.offset = 5000;
  WatchEvent d = a;
  d.kind = WatchKind::Click;
  d.offset = 1001;
  const std::vector<WatchEvent> l{a, c}, rr{b, d}, other{a};
  const auto m = merge_watch_events({{20, 3, &l}, {21, 3, &rr}, {6, -1, &other}}, 192);
  // 20+21's hole at 1000 is one; 21's click is its own; 6 is no member; 20's 5000 alone.
  CHECK_EQ(m.size(), 4u);
  CHECK_EQ(m[0].ev.offset, 1000u);
  CHECK_EQ(m[0].ch.size(), 2u);
  CHECK_EQ(m[0].ev.len, 500u);
  size_t clicks = 0, singles6 = 0;
  for (const auto& e : m) {
    clicks += e.ev.kind == WatchKind::Click;
    singles6 += e.ch.size() == 1 && e.ch[0] == 6;
  }
  CHECK_EQ(clicks, 1u);
  CHECK_EQ(singles6, 1u);
  CHECK_EQ(m.back().ev.offset, 5000u);
  // Further apart than tol: two.
  WatchEvent far = a;
  far.offset = 1000 + 400;
  const std::vector<WatchEvent> f{far};
  const auto m2 = merge_watch_events({{20, 3, &l}, {21, 3, &f}}, 192);
  CHECK_EQ(m2.size(), 3u);
}

// 40 s x 2 channels at 96 kHz in tone mode, as the endpoint would run it on a Pi 3: printed, and
// held to a loose bound so a regression to something super-linear shows.
void test_speed() {
  const auto x = sine(1000, 0.5, 40 * 96000);
  const auto t0 = std::chrono::steady_clock::now();
  for (int ch = 0; ch < 2; ++ch) {
    WatchAnalyzer a(params(1000.0));
    for (size_t i = 0; i < x.size(); i += 4096) a.feed(x.data() + i, std::min<size_t>(4096, x.size() - i));
    const auto r = a.finish();
    CHECK_EQ(total(r), 0u);
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  40 s x 2 ch x 96 kHz, tone mode: %.0f ms\n", ms);
  CHECK(ms < 5000);
}

}  // namespace

int main() {
  test_clean_sine();
  test_offset_frequency();
  test_zero_run();
  test_spike();
  test_phase_slip();
  test_partial_last_block();
  test_off_phase_block_is_no_slip();
  test_level_step();
  test_gap();
  test_intermittent_is_not_gaps();
  test_fades();
  test_music_no_clicks();
  test_clip_and_wrong_tone();
  test_silence_and_short();
  test_merge();
  test_speed();
  return report("test_genie_watch");
}
