#include "util/asrc.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "check.h"

using namespace st;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double freq, double rate, size_t n, double amp = 0.5) {
  std::vector<float> v(n);
  for (size_t i = 0; i < n; ++i)
    v[i] = static_cast<float>(amp * std::sin(2 * kPi * freq * static_cast<double>(i) / rate));
  return v;
}

// Magnitude at `freq`, Hann-windowed so the tone's own skirts cannot masquerade as a spur.
double bin_at(const std::vector<float>& x, size_t stride, size_t chan, double rate, double freq,
              size_t skip) {
  const size_t n = x.size() / stride - skip;
  double re = 0, im = 0;
  for (size_t i = 0; i < n; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / (n - 1));
    const double a = 2 * kPi * freq * static_cast<double>(i) / rate;
    const double v = x[(skip + i) * stride + chan] * w;
    re += v * std::cos(a);
    im -= v * std::sin(a);
  }
  return 2 * std::sqrt(re * re + im * im) / static_cast<double>(n);
}

// Matched rates still go through the converter — that is what leaves the ratio available as a
// clock discipline — so the guarantee is unity gain and a clean spectrum, not bit-exactness.
void test_matched_rates_pass_through_cleanly() {
  Asrc a;
  std::string err;
  CHECK(a.configure(1, 96000, 96000, &err));
  CHECK(a.unity());
  const auto in = sine(1000, 96000, 96000);
  std::vector<float> out;
  a.process(in.data(), in.size(), 1.0, &out, &err);
  CHECK(out.size() > in.size() - 2048);

  const double tone = bin_at(out, 1, 0, 96000, 1000, 8192);
  CHECK(tone > 0.2);  // unity gain (the Hann probe reads half the amplitude)
  double worst = 0;
  for (double f = 100; f < 48000; f += 100) {
    if (std::abs(f - 1000) < 1500) continue;
    worst = std::max(worst, bin_at(out, 1, 0, 96000, f, 8192));
  }
  CHECK(worst < tone / 1000.0);
}

void test_upsample_keeps_the_tone_and_adds_nothing() {
  Asrc a;
  std::string err;
  CHECK(a.configure(1, 44100, 96000, &err));
  CHECK(!a.unity());
  const auto in = sine(1000, 44100, 44100 * 2);
  std::vector<float> out;
  a.process(in.data(), in.size(), 1.0, &out, &err);

  const double expect = in.size() * 96000.0 / 44100.0;
  CHECK(std::abs(static_cast<double>(out.size()) - expect) < 512);

  // The Hann-windowed probe reads half the amplitude, so unity gain on a 0.5 sine is 0.25.
  const double tone = bin_at(out, 1, 0, 96000, 1000, 8192);
  CHECK(tone > 0.2);
  double worst = 0;
  for (double f = 100; f < 48000; f += 100) {
    if (std::abs(f - 1000) < 1500) continue;
    worst = std::max(worst, bin_at(out, 1, 0, 96000, f, 8192));
  }
  CHECK(worst < tone / 1000.0);  // everything else below -60 dB relative
}

void test_downsample_rejects_what_would_alias() {
  // 30 kHz at 96 k is past the Nyquist of a 48 k output: unfiltered it folds to 18 kHz and lands
  // in the passband as a tone that was never sent.
  Asrc a;
  std::string err;
  CHECK(a.configure(1, 96000, 48000, &err));
  const auto in = sine(30000, 96000, 96000);
  std::vector<float> out;
  a.process(in.data(), in.size(), 1.0, &out, &err);
  CHECK(bin_at(out, 1, 0, 48000, 18000, 4096) < 0.005);
}

// Audio arrives in packets, so the result must not depend on where the packet boundaries fell.
void test_packet_boundaries_do_not_show() {
  const auto in = sine(997, 44100, 44100);
  std::string err;

  Asrc whole;
  whole.configure(1, 44100, 96000, &err);
  std::vector<float> a;
  whole.process(in.data(), in.size(), 1.0, &a, &err);

  Asrc chunked;
  chunked.configure(1, 44100, 96000, &err);
  std::vector<float> b;
  for (size_t i = 0; i < in.size(); i += 257)
    chunked.process(in.data() + i, std::min<size_t>(257, in.size() - i), 1.0, &b, &err);

  CHECK_EQ(a.size(), b.size());
  double worst = 0.0;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i)
    worst = std::max(worst, std::abs(static_cast<double>(a[i] - b[i])));
  CHECK(worst < 1e-6);
}

// The reason for an ASRC rather than a fixed kernel: the ratio is the control input, and moving
// it must change the output rate without putting a step in the audio.
void test_trimming_the_ratio_changes_the_output_rate_smoothly() {
  std::string err;
  const auto in = sine(1000, 48000, 48000);

  size_t counts[3];
  const double trims[3] = {0.999, 1.0, 1.001};
  for (int t = 0; t < 3; ++t) {
    Asrc a;
    a.configure(1, 48000, 96000, &err);
    std::vector<float> out;
    for (size_t i = 0; i < in.size(); i += 512)
      a.process(in.data() + i, std::min<size_t>(512, in.size() - i), trims[t], &out, &err);
    counts[t] = out.size();
  }
  // A 0.1% trim moves the output count by about 0.1% — that is the drift being absorbed.
  CHECK(counts[0] < counts[1]);
  CHECK(counts[1] < counts[2]);
  CHECK(std::abs(static_cast<double>(counts[2] - counts[0]) / counts[1] - 0.002) < 0.0005);

  // And it stays clean while the ratio moves: no step, no broadband splatter.
  Asrc a;
  a.configure(1, 48000, 96000, &err);
  std::vector<float> out;
  for (size_t i = 0; i < in.size(); i += 512) {
    const double trim = 1.0 + 0.0005 * std::sin(2 * kPi * static_cast<double>(i) / in.size());
    a.process(in.data() + i, std::min<size_t>(512, in.size() - i), trim, &out, &err);
  }
  const double tone = bin_at(out, 1, 0, 96000, 1000, 8192);
  double worst = 0;
  for (double f = 2000; f < 48000; f += 250) worst = std::max(worst, bin_at(out, 1, 0, 96000, f, 8192));
  CHECK(tone > 0.2);
  CHECK(worst < tone / 300.0);
}

// One converter state for the whole interleaved stream is what guarantees a stereo sender's two
// halves stay on the same samples through the conversion.
void test_channels_stay_aligned_through_the_converter() {
  std::string err;
  const size_t n = 44100;
  std::vector<float> in(n * 2);
  for (size_t i = 0; i < n; ++i) {
    const float v = static_cast<float>(0.5 * std::sin(2 * kPi * 997.0 * i / 44100.0));
    in[i * 2 + 0] = v;
    in[i * 2 + 1] = v;  // identical on both channels
  }
  Asrc a;
  CHECK(a.configure(2, 44100, 96000, &err));
  std::vector<float> out;
  const size_t frames = a.process(in.data(), n, 1.0, &out, &err);
  CHECK(frames > 0);
  CHECK_EQ(out.size(), frames * 2);
  double worst = 0.0;
  for (size_t i = 0; i < frames; ++i)
    worst = std::max(worst, std::abs(static_cast<double>(out[i * 2] - out[i * 2 + 1])));
  CHECK_EQ(worst, 0.0);  // not "close": identical
}

// Every rate a sender may declare arrives at the right RATE. The converter holds a few hundred
// frames of priming, so the count is short by a fixed amount — what matters is that the shortfall
// does not grow with time, because a shortfall that grows is a wrong ratio, and a wrong ratio
// walks a sender off the card's axis over a long session.
void test_every_rate_runs_at_the_right_rate() {
  const unsigned rates[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000, 64000, 88200, 176400,
                            192000};
  std::string err;
  for (unsigned in_rate : rates) {
    Asrc a;
    CHECK(a.configure(1, in_rate, 96000, &err));
    const std::vector<float> in(in_rate * 4, 0.1f);  // four seconds

    std::vector<float> one;
    a.process(in.data(), in_rate, 1.0, &one, &err);
    const long short_1s = 96000L - static_cast<long>(one.size());

    std::vector<float> rest;
    a.process(in.data() + in_rate, in_rate * 3, 1.0, &rest, &err);
    const long short_4s = 4L * 96000L - static_cast<long>(one.size() + rest.size());

    if (short_1s != short_4s)
      std::printf("  %u Hz: short by %ld after 1 s but %ld after 4 s — the ratio is wrong\n",
                  in_rate, short_1s, short_4s);
    CHECK_EQ(short_1s, short_4s);   // a one-off priming cost, not drift
    CHECK(short_1s >= 0 && short_1s < 2048);
  }
}

}  // namespace

int main() {
  test_matched_rates_pass_through_cleanly();
  test_upsample_keeps_the_tone_and_adds_nothing();
  test_downsample_rejects_what_would_alias();
  test_packet_boundaries_do_not_show();
  test_trimming_the_ratio_changes_the_output_rate_smoothly();
  test_channels_stay_aligned_through_the_converter();
  test_every_rate_runs_at_the_right_rate();
  return report("resample");
}
