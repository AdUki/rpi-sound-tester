#include "capture.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>

#include "channel_layout.h"
#include "check.h"
#include "constants.h"
#include "generators.h"
#include "genie.h"
#include "rates.h"
#include "ring_buffer.h"

using namespace st;

namespace {

constexpr unsigned kPeriod = kTestPeriod;

enum class Stimulus { Broadband, PingTrain, BongSingle, BongTrain, ContinuousSine };

// A ringing burst: a decaying sine, `dur` seconds long.
void add_ring(std::vector<float>& src, uint64_t at, double rate, double freq, float tau,
              double dur) {
  const unsigned len = static_cast<unsigned>(rate * dur);
  for (unsigned i = 0; i < len && at + i < src.size(); ++i) {
    const double t = i / rate;
    const float env = std::exp(-static_cast<float>(t) / tau);
    src[at + i] += 0.8f * env * static_cast<float>(std::sin(2 * kPi * freq * t));
  }
}

// A bong-shaped burst: 440 Hz decaying sine, tau 80 ms, a quarter of a second long — the
// generator's ringing tone. (A bing is 1 kHz, tau 20 ms, 60 ms long.)
void add_bong(std::vector<float>& src, uint64_t at, double rate) {
  add_ring(src, at, rate, 440.0, 0.080f, 0.25);
}

// Writes `src` into the ring as channel 0, and delayed by `delay` samples as channel 1, over a
// low noise floor on every channel. Ring sample t carries src[t + 4096] (so a negative delay
// still has history to read).
void write_delayed(RingBuffer& ring, int64_t delay, uint64_t frames, const std::vector<float>& src,
                   std::mt19937& rng) {
  std::uniform_real_distribution<float> floor_noise(-0.02f, 0.02f);
  const unsigned total = st::channels().total();
  std::vector<float> block(kPeriod * total);
  for (uint64_t n = 0; n < frames; n += kPeriod) {
    for (unsigned i = 0; i < kPeriod; ++i) {
      const uint64_t t = n + i;
      for (unsigned c = 0; c < total; ++c) block[i * total + c] = floor_noise(rng);
      block[i * total + 0] += src[t + 4096];
      const int64_t td = static_cast<int64_t>(t) + 4096 - delay;
      if (td >= 0 && td < static_cast<int64_t>(src.size())) block[i * total + 1] += src[td];
    }
    ring.write(block.data(), kPeriod);
  }
}

// Fills the ring so that channel 1 carries channel 0's signal delayed by `delay` samples —
// the multiroom case.
void fill_with_delayed_copy(RingBuffer& ring, int64_t delay, uint64_t frames, Stimulus stim,
                            double rate) {
  std::mt19937 rng(1234);
  std::uniform_real_distribution<float> broadband(-0.5f, 0.5f);

  std::vector<float> src(frames + 8192, 0.0f);
  if (stim == Stimulus::Broadband) {
    for (auto& v : src) v = broadband(rng);
  } else if (stim == Stimulus::PingTrain) {
    // A repeating tick, exactly what the ping generator emits.
    for (uint64_t tick = 5000; tick < frames; tick += 30000) {
      for (unsigned i = 0; i < 600; ++i) {
        const double t = i / rate;
        const float env = std::exp(-static_cast<float>(t) / 0.0004f);
        src[tick + i] += 0.8f * env * static_cast<float>(std::sin(2 * kPi * 3000.0 * t));
      }
    }
  } else if (stim == Stimulus::BongSingle) {
    add_bong(src, 60000, rate);
  } else if (stim == Stimulus::BongTrain) {
    // One a second.
    for (uint64_t at = 20000; at < frames; at += static_cast<uint64_t>(rate))
      add_bong(src, at, rate);
  } else {
    for (size_t i = 0; i < src.size(); ++i)
      src[i] = 0.5f * static_cast<float>(std::sin(2 * kPi * 440.0 * (i / rate)));
  }
  write_delayed(ring, delay, frames, src, rng);
}

// Fills the ring with a steady waveform `wave(phase)` (phase in cycles) at `freq`, channel 1
// delayed by `delay`.
template <typename Wave>
void fill_with_steady_tone(RingBuffer& ring, int64_t delay, uint64_t frames, double rate,
                           double freq, Wave wave) {
  std::mt19937 rng(99);
  std::vector<float> src(frames + 8192);
  for (size_t i = 0; i < src.size(); ++i) {
    const double cyc = freq * static_cast<double>(i) / rate;
    src[i] = 0.5f * static_cast<float>(wave(cyc - std::floor(cyc)));
  }
  write_delayed(ring, delay, frames, src, rng);
}

void test_xcorr_recovers_a_known_delay(double rate) {
  for (int64_t delay : {0, 137, 4321, -960}) {
    RingBuffer ring(kRingFrames, st::channels().total(), 2 * kPeriod);
    CaptureStore cap(ring, rate, kPeriod);
    fill_with_delayed_copy(ring, delay, 400000, Stimulus::Broadband, rate);

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
void test_xcorr_reports_low_confidence_on_a_periodic_stimulus(double rate) {
  RingBuffer ring(kRingFrames, st::channels().total(), 2 * kPeriod);
  CaptureStore cap(ring, rate, kPeriod);
  fill_with_delayed_copy(ring, 500, 400000, Stimulus::PingTrain, rate);

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

// bing/bong are ringing tones: the correlation is a carrier under a slow envelope. A single
// burst bracketed on its own must read as one clear peak — sample-exact lag, high confidence
// — with the carrier's own oscillation never counted as a rival.
void test_xcorr_ringing_tone_single_burst_is_confident(double rate) {
  for (int64_t delay : {137, -960}) {
    RingBuffer ring(kRingFrames, st::channels().total(), 2 * kPeriod);
    CaptureStore cap(ring, rate, kPeriod);
    fill_with_delayed_copy(ring, delay, 400000, Stimulus::BongSingle, rate);

    const CaptureStatus cs = cap.freeze(0);
    CHECK(cs.frozen);

    // One bong, fully inside the window: sample-exact and unambiguous.
    const XcorrResult r = cap.xcorr(0, 1, cs.valid_start + 5000, 1 << 17);
    CHECK(r.ok);
    std::cout << "  single bong, delay " << delay << " -> lag " << r.lag_samples
              << " (confidence " << r.confidence << ")\n";
    CHECK_EQ(r.lag_samples, delay);
    CHECK(r.confidence > 3.0);
  }
}

void test_xcorr_ringing_tone_train_stays_on_the_true_interval(double rate) {
  RingBuffer ring(kRingFrames, st::channels().total(), 2 * kPeriod);
  CaptureStore cap(ring, rate, kPeriod);
  fill_with_delayed_copy(ring, 527, 400000, Stimulus::BongTrain, rate);

  const CaptureStatus cs = cap.freeze(0);
  CHECK(cs.frozen);

  // Several bongs in the window: the tallest envelope lobe is still the true lag (every
  // burst pair agrees there), so the lag must NOT slip by a ping interval — but the rival
  // lobe one interval away is nearly as tall, so the confidence must say ambiguous.
  const XcorrResult r = cap.xcorr(0, 1, cs.valid_start + 5000, 1 << 18);
  CHECK(r.ok);
  std::cout << "  bong train -> lag " << r.lag_samples << " (confidence " << r.confidence
            << ")\n";
  CHECK_EQ(r.lag_samples, 527);
  CHECK(r.confidence < 2.0);
}

// A continuous tone's delay is genuinely unknowable modulo its period: every carrier crest
// in the correlation is a hair from the winner, so whatever lag comes out, the confidence
// must never bless it. (The envelope alone can't catch this — a CW tone's envelope is one
// window-wide lobe with no rivals — hence the runner-up-crest cap.)
void test_xcorr_never_blesses_a_continuous_tone(double rate) {
  RingBuffer ring(kRingFrames, st::channels().total(), 2 * kPeriod);
  CaptureStore cap(ring, rate, kPeriod);
  fill_with_delayed_copy(ring, 137, 400000, Stimulus::ContinuousSine, rate);

  const CaptureStatus cs = cap.freeze(0);
  CHECK(cs.frozen);

  const XcorrResult r = cap.xcorr(0, 1, cs.valid_start + 5000, 1 << 17);
  CHECK(r.ok);
  std::cout << "  continuous sine -> lag " << r.lag_samples << " (confidence " << r.confidence
            << ")\n";
  CHECK(r.confidence < 2.0);
  // genie/sync's sliding windows are short: the linear correlation's taper over 2^14 must not
  // hide the tone's equal crests (it read 999, "no rival", and a wrong lag was trusted).
  for (const uint64_t win : {uint64_t{1} << 14, uint64_t{1} << 12}) {
    const XcorrResult w = cap.xcorr(0, 1, cs.valid_start + 5000, win);
    CHECK(w.ok);
    std::cout << "  continuous sine, window " << win << " -> lag " << w.lag_samples
              << " (confidence " << w.confidence << ")\n";
    CHECK(w.confidence < 1.5);
  }
}

// The Scope cursors, genie/sync's cur_x/cur_y and its last ping window bracket a ringing burst
// in a short window. Its onset is in the window on both channels and pins the lag: a bong (tau
// 80 ms, so ~0.9 of the cross-spectrum is its one partial) must read sample-exact and
// confident, not as a steady tone — that correction lifts its ring-out crests to the winner's
// height (it read confidence ~1.0). A window that opens mid ring-out has no onset to pin the
// cycle: it must stay unresolved (it read a wrong lag at 999 before the correction).
void test_xcorr_burst_onset_in_a_short_window(double rate) {
  constexpr uint64_t kOnsetSrc = 60000;
  constexpr uint64_t kOnset = kOnsetSrc - 4096;  // where the burst starts in the ring
  {
    RingBuffer ring(kRingFrames, st::channels().total(), 2 * kPeriod);
    CaptureStore cap(ring, rate, kPeriod);
    std::mt19937 rng(7);
    std::vector<float> src(400000 + 8192, 0.0f);
    add_bong(src, kOnsetSrc, rate);
    write_delayed(ring, 137, 400000, src, rng);
    const CaptureStatus cs = cap.freeze(0);
    CHECK(cs.frozen);
    for (const uint64_t len : {uint64_t{6000}, uint64_t{8192}}) {
      for (const uint64_t lead : {uint64_t{300}, uint64_t{1000}, uint64_t{2500}}) {
        const XcorrResult r = cap.xcorr(0, 1, cs.valid_start + kOnset - lead, len);
        CHECK(r.ok);
        std::cout << "  bong onset " << lead << " into a " << len << " window -> lag "
                  << r.lag_samples << " (confidence " << r.confidence << ")\n";
        CHECK_EQ(r.lag_samples, 137);
        CHECK(r.confidence > 3.0);
      }
      // Opening 2000 samples into the ring-out: no onset, the cycle is unknowable.
      const XcorrResult mid = cap.xcorr(0, 1, cs.valid_start + kOnset + 2000, len);
      CHECK(mid.ok);
      std::cout << "  bong mid ring-out, " << len << " window -> lag " << mid.lag_samples
                << " (confidence " << mid.confidence << ")\n";
      CHECK(mid.lag_samples == 137 || mid.confidence < 1.5);
    }
  }
  {
    // A bing a long way apart: 3000 samples, in a 4096 window that holds both onsets.
    RingBuffer ring(kRingFrames, st::channels().total(), 2 * kPeriod);
    CaptureStore cap(ring, rate, kPeriod);
    std::mt19937 rng(8);
    std::vector<float> src(400000 + 8192, 0.0f);
    add_ring(src, kOnsetSrc, rate, 1000.0, 0.020f, 0.060);
    write_delayed(ring, 3000, 400000, src, rng);
    const CaptureStatus cs = cap.freeze(0);
    CHECK(cs.frozen);
    const XcorrResult r = cap.xcorr(0, 1, cs.valid_start + kOnset - 500, 4096);
    CHECK(r.ok);
    std::cout << "  bing, delay 3000, 4096 window -> lag " << r.lag_samples << " (confidence "
              << r.confidence << ")\n";
    CHECK_EQ(r.lag_samples, 3000);
    CHECK(r.confidence > 3.0);
  }
}

// A steady tone rich in harmonics is as ambiguous as a sine, but its fundamental holds only
// 0.79-0.90 of the cross-spectrum: the steady-tone test must count its harmonic series, or
// genie/sync's sliding windows trust a lag a cycle off (28 or -103 instead of 137).
void test_xcorr_never_blesses_a_harmonic_rich_tone(double rate) {
  struct Shape {
    const char* name;
    double (*wave)(double);
  };
  const Shape shapes[] = {
      {"square", [](double p) { return p < 0.5 ? 1.0 : -1.0; }},
      {"sine + 30 % third",
       [](double p) { return (std::sin(2 * kPi * p) + 0.3 * std::sin(6 * kPi * p)) / 1.3; }},
      {"clipped sine", [](double p) { return std::clamp(1.6 * std::sin(2 * kPi * p), -1.0, 1.0); }},
  };
  for (const Shape& shape : shapes) {
    for (const double freq : {200.0, 440.0}) {
      RingBuffer ring(kRingFrames, st::channels().total(), 2 * kPeriod);
      CaptureStore cap(ring, rate, kPeriod);
      fill_with_steady_tone(ring, 137, 200000, rate, freq, shape.wave);
      const CaptureStatus cs = cap.freeze(0);
      CHECK(cs.frozen);
      for (const uint64_t win : {uint64_t{1} << 12, uint64_t{1} << 13, uint64_t{1} << 14}) {
        const XcorrResult r = cap.xcorr(0, 1, cs.valid_start + 5000, win);
        CHECK(r.ok);
        if (r.lag_samples != 137 && r.confidence >= 1.5) {
          std::cout << "  " << shape.name << " " << freq << " Hz, window " << win << " -> lag "
                    << r.lag_samples << " (confidence " << r.confidence << ")\n";
        }
        CHECK(r.lag_samples == 137 || r.confidence < 1.5);
      }
    }
  }
}

// A tone so low that two of its periods outlast half the correlation lobe leaves no crest to
// compare against the winner: the envelope alone read 20 Hz in an 8192 window at 48 kHz as lag
// 106 at confidence 143.
void test_xcorr_never_blesses_a_very_low_tone(double rate) {
  RingBuffer ring(kRingFrames, st::channels().total(), 2 * kPeriod);
  CaptureStore cap(ring, rate, kPeriod);
  fill_with_steady_tone(ring, 137, 300000, rate, 20.0,
                        [](double p) { return std::sin(2 * kPi * p); });
  const CaptureStatus cs = cap.freeze(0);
  CHECK(cs.frozen);
  for (const uint64_t win : {uint64_t{1} << 12, uint64_t{1} << 13, uint64_t{1} << 14}) {
    for (const uint64_t at : {uint64_t{5000}, uint64_t{40000}, uint64_t{77777}}) {
      const XcorrResult r = cap.xcorr(0, 1, cs.valid_start + at, win);
      CHECK(r.ok);
      if (r.confidence > 1.5) {
        std::cout << "  20 Hz, window " << win << " at " << at << " -> lag " << r.lag_samples
                  << " (confidence " << r.confidence << ")\n";
      }
      CHECK(r.confidence <= 1.5);
    }
  }
}

// The simulator's music in genie/sync's sliding windows: held synth notes are about as tonal as
// music gets, yet it must stay measurable — about a third of the windows clear 1.5, every one of
// them at the true lag.
void test_xcorr_music_stays_measurable(double rate) {
  RingBuffer ring(kRingFrames, st::channels().total(), 2 * kPeriod);
  CaptureStore cap(ring, rate, kPeriod);
  Generators gen;
  gen.init(rate);
  const uint64_t frames = static_cast<uint64_t>(10 * rate);
  std::vector<float> src(frames + 8192);
  gen.render_music(0, src.size(), 0.5f, src.data());
  std::mt19937 rng(5);
  write_delayed(ring, 137, frames, src, rng);
  const CaptureStatus cs = cap.freeze(0);
  CHECK(cs.frozen);
  const uint64_t win = sliding_default_window(rate);
  unsigned windows = 0, kept = 0, wrong = 0;
  for (uint64_t at = 0; at + win <= cs.valid_len; at += win) {
    const XcorrResult r = cap.xcorr(0, 1, cs.valid_start + at, win);
    CHECK(r.ok);
    ++windows;
    if (r.confidence >= 1.5 && r.peak >= kPingMinPeak) {
      ++kept;
      if (r.lag_samples != 137) ++wrong;
    }
  }
  std::cout << "  music, " << windows << " windows of " << win << ": " << kept << " kept, "
            << wrong << " at a wrong lag\n";
  CHECK(kept * 4 >= windows);
  CHECK_EQ(wrong, 0u);
}

void test_xcorr_needs_a_freeze(double rate) {
  RingBuffer ring(kRingFrames, st::channels().total(), 2 * kPeriod);
  CaptureStore cap(ring, rate, kPeriod);
  fill_with_delayed_copy(ring, 100, 200000, Stimulus::Broadband, rate);

  const XcorrResult r = cap.xcorr(0, 1, 1000, 1 << 16);
  CHECK(!r.ok);
  CHECK(r.error.find("freeze") != std::string::npos);
}

void test_window_returns_raw_and_columns(double rate) {
  RingBuffer ring(kRingFrames, st::channels().total(), 2 * kPeriod);
  CaptureStore cap(ring, rate, kPeriod);
  fill_with_delayed_copy(ring, 0, 200000, Stimulus::Broadband, rate);
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
  CHECK_EQ(cols.cols, 500u);  // echoed, so a caller sees what it was served

  // Columns past the maximum are refused, not clamped: clamped, 4096 columns over 8192 frames
  // would come back as min/max pairs that a caller asked for as raw samples.
  const WindowResult wide = cap.window(0, cs.valid_start + 100, 8192, kWindowMaxCols + 1);
  CHECK(!wide.ok);
  CHECK(wide.error.find("cols must be 1..2048") != std::string::npos);
  CHECK(!cap.window(0, cs.valid_start + 100, 512, 0).ok);
  const WindowResult max = cap.window(0, cs.valid_start + 100, 2 * kWindowMaxCols, kWindowMaxCols);
  CHECK(max.ok);
  CHECK(max.raw);
  CHECK_EQ(max.samples.size(), 2u * kWindowMaxCols);

  // A range error names both the span asked for and the span there is.
  const WindowResult out = cap.window(0, cs.valid_start - 1000, 512, 1024);
  CHECK(!out.ok);
  CHECK(out.error.find("[" + std::to_string(cs.valid_start - 1000) + ", ") == 0);
  CHECK(out.error.find("frozen range [" + std::to_string(cs.valid_start) + ", ") !=
        std::string::npos);
  const WindowResult zero = cap.window(0, cs.valid_start, 0, 1024);
  CHECK(zero.error.find("len must be 1..") == 0);
}

void test_analyze_length_is_configurable(double rate) {
  RingBuffer ring(kRingFrames, st::channels().total(), 2 * kPeriod);
  CaptureStore cap(ring, rate, kPeriod);

  // Default: a modest fixed window (kCaptureDefaultSeconds), capped at the maximum.
  CHECK(cap.max_frames() > 0);
  const uint64_t expect_default =
      std::min<uint64_t>(cap.max_frames(), static_cast<uint64_t>(kCaptureDefaultSeconds * rate));
  CHECK_EQ(cap.analyze_frames(), expect_default);

  fill_with_delayed_copy(ring, 0, 400000, Stimulus::Broadband, rate);

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
  // large pins to the maximum.
  cap.set_analyze_frames(1);
  CHECK_EQ(cap.analyze_frames(), kCaptureMinFrames);
  cap.set_analyze_frames(kRingFrames * 4);
  CHECK_EQ(cap.analyze_frames(), cap.max_frames());
}

// The analyze length is the snapshot's *allocation*, not just its copy length: the console
// tells the operator that a longer buffer costs RAM, and on a 1 GB Pi that has to be true.
void test_analyze_length_drives_the_allocation(double rate) {
  RingBuffer ring(kRingFrames, st::channels().total(), 2 * kPeriod);
  CaptureStore cap(ring, rate, kPeriod);

  const uint64_t at_default = cap.pinned_bytes();
  CHECK_EQ(at_default, cap.analyze_frames() * st::channels().total() * sizeof(float));
  // Nowhere near the ceiling, which is what used to be pinned unconditionally.
  CHECK(at_default < cap.max_frames() * st::channels().total() * sizeof(float) / 2);

  CHECK(cap.set_analyze_frames(static_cast<uint64_t>(5 * rate)));
  const uint64_t at_five = cap.pinned_bytes();
  CHECK(at_five < at_default);
  CHECK_EQ(at_five, cap.analyze_frames() * st::channels().total() * sizeof(float));

  CHECK(cap.set_analyze_frames(static_cast<uint64_t>(40 * rate)));
  CHECK(cap.pinned_bytes() > at_five);
}

// A resize while frozen must not pull the snapshot out from under an in-flight measurement.
void test_resize_while_frozen_is_deferred(double rate) {
  RingBuffer ring(kRingFrames, st::channels().total(), 2 * kPeriod);
  CaptureStore cap(ring, rate, kPeriod);
  fill_with_delayed_copy(ring, 137, 400000, Stimulus::PingTrain, rate);

  const CaptureStatus cs = cap.freeze(0);
  CHECK(cs.frozen);
  const uint64_t pinned_before = cap.pinned_bytes();

  CHECK(cap.set_analyze_frames(static_cast<uint64_t>(2 * rate)));
  // Setting took effect; the buffer did not move, so the frozen window still reads.
  CHECK_EQ(cap.analyze_frames(), static_cast<uint64_t>(2 * rate));
  CHECK_EQ(cap.pinned_bytes(), pinned_before);
  CHECK(cap.status().frozen);
  const WindowResult w = cap.window(0, cs.valid_start + 10, 1024, 512);
  CHECK(w.ok);

  // Resuming releases the measurement, and the deferred shrink lands.
  cap.resume();
  CHECK(!cap.status().frozen);
  CHECK(cap.pinned_bytes() < pinned_before);
  CHECK_EQ(cap.pinned_bytes(), static_cast<uint64_t>(2 * rate) * st::channels().total() * sizeof(float));
}

// A freeze may never copy more frames than the buffer holds, however the two got out of step.
void test_freeze_never_overruns_the_snapshot(double rate) {
  RingBuffer ring(kRingFrames, st::channels().total(), 2 * kPeriod);
  CaptureStore cap(ring, rate, kPeriod);
  CHECK(cap.set_analyze_frames(static_cast<uint64_t>(1 * rate)));
  fill_with_delayed_copy(ring, 0, 400000, Stimulus::Broadband, rate);

  const CaptureStatus cs = cap.freeze(0);
  CHECK(cs.frozen);
  CHECK(cs.valid_len <= cap.pinned_bytes() / (st::channels().total() * sizeof(float)));
  const WindowResult w = cap.window(0, cs.valid_start, 1024, 512);
  CHECK(w.ok);
}

void test_snapshot_frames_copies_every_channel(double rate) {
  RingBuffer ring(kRingFrames, st::channels().total(), 2 * kPeriod);
  CaptureStore cap(ring, rate, kPeriod);
  fill_with_delayed_copy(ring, 0, 200000, Stimulus::Broadband, rate);
  const CaptureStatus cs = cap.freeze(0);
  CHECK(cs.frozen);
  const unsigned total = st::channels().total();

  const uint64_t start = cs.valid_start + 777, len = 1000;
  std::vector<float> all(len * total), one(len);
  CHECK(cap.snapshot_frames(cs.freeze_sample, start, len, all.data()));
  for (unsigned c = 0; c < total; c += total - 1) {  // the first and the last column
    const WindowResult w = cap.window(c, start, len, 1024);
    CHECK(w.ok && w.raw);
    bool same = w.samples.size() == len;
    for (uint64_t i = 0; same && i < len; ++i) same = all[i * total + c] == w.samples[i];
    CHECK(same);
  }
  // Out of the frozen range, or a different snapshot: refused.
  CHECK(!cap.snapshot_frames(cs.freeze_sample, cs.valid_start - 1, len, all.data()));
  CHECK(!cap.snapshot_frames(cs.freeze_sample, cs.valid_start + cs.valid_len - 10, len, all.data()));
  CHECK(!cap.snapshot_frames(cs.freeze_sample + 1, start, len, all.data()));
  cap.resume();
  CHECK(!cap.snapshot_frames(cs.freeze_sample, start, len, all.data()));
}

void test_window_rejects_out_of_range(double rate) {
  RingBuffer ring(kRingFrames, st::channels().total(), 2 * kPeriod);
  CaptureStore cap(ring, rate, kPeriod);
  fill_with_delayed_copy(ring, 0, 200000, Stimulus::Broadband, rate);
  const CaptureStatus cs = cap.freeze(0);

  const WindowResult bad = cap.window(0, cs.freeze_sample + 10000, 1024, 512);
  CHECK(!bad.ok);
  CHECK(!bad.error.empty());

  // A huge start (a negative number pushed through the query string's u64 parse) must fail
  // the range check, not wrap start + len around and index far outside the snapshot.
  const WindowResult wrapped = cap.window(0, ~0ull - 500, 1024, 512);
  CHECK(!wrapped.ok);
}

}  // namespace

int main() {
  for (const unsigned r : kTestRates) {
    std::cout << "at " << r << " Hz\n";
    const double rate = r;
    test_xcorr_recovers_a_known_delay(rate);
    test_xcorr_reports_low_confidence_on_a_periodic_stimulus(rate);
    test_xcorr_ringing_tone_single_burst_is_confident(rate);
    test_xcorr_ringing_tone_train_stays_on_the_true_interval(rate);
    test_xcorr_never_blesses_a_continuous_tone(rate);
    test_xcorr_burst_onset_in_a_short_window(rate);
    test_xcorr_never_blesses_a_harmonic_rich_tone(rate);
    test_xcorr_never_blesses_a_very_low_tone(rate);
    test_xcorr_music_stays_measurable(rate);
    test_xcorr_needs_a_freeze(rate);
    test_window_returns_raw_and_columns(rate);
    test_window_rejects_out_of_range(rate);
    test_snapshot_frames_copies_every_channel(rate);
    test_analyze_length_is_configurable(rate);
    test_analyze_length_drives_the_allocation(rate);
    test_resize_while_frozen_is_deferred(rate);
    test_freeze_never_overruns_the_snapshot(rate);
  }
  return report("capture");
}
