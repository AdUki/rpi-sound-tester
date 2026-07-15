#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace st {

// Anti-aliasing decimator for the Opus listen path: brings the 96 kHz capture ring down to
// Opus's 48 kHz. It is stateful across blocks — a per-block stateless FIR would click at every
// chunk boundary — and carries a constant, identical group delay for every instance, so several
// channels decimated in parallel stay sample-aligned (the invariant the whole feature rests on).
// Monitor quality only: the measurement paths (analysis, freeze, xcorr) read the native 96 kHz
// ring and never touch this. factor 1 = passthrough (engine already at 48 kHz); factor 2 = /2.
class Decimator {
 public:
  explicit Decimator(unsigned factor) : factor_(factor) {
    if (factor_ == 2) {
      build_coeffs();
      hist_.assign(kTaps - 1, 0.0f);
    }
  }

  // Decimate `n` input frames (a multiple of the factor) into `out`.
  void process(const float* in, size_t n, std::vector<float>* out) {
    if (factor_ != 2) {
      out->assign(in, in + n);
      return;
    }
    const size_t outn = n / 2;
    out->resize(outn);
    ext_.resize(static_cast<size_t>(kTaps - 1) + n);
    std::copy(hist_.begin(), hist_.end(), ext_.begin());
    std::copy(in, in + n, ext_.begin() + (kTaps - 1));
    for (size_t j = 0; j < outn; ++j) {
      const float* w = &ext_[2 * j];
      float acc = 0.0f;
      for (int t = 0; t < kTaps; ++t) acc += h_[t] * w[t];
      (*out)[j] = acc;
    }
    std::copy(ext_.end() - (kTaps - 1), ext_.end(), hist_.begin());
  }

  void reset() { std::fill(hist_.begin(), hist_.end(), 0.0f); }

 private:
  static constexpr int kTaps = 127;

  void build_coeffs() {
    constexpr double kPi = 3.14159265358979323846;
    const int mid = (kTaps - 1) / 2;
    const double fc = 0.225;  // cutoff as a fraction of the 96 kHz input rate (~21.6 kHz)
    double sum = 0.0;
    for (int t = 0; t < kTaps; ++t) {
      const int k = t - mid;
      const double sinc = (k == 0) ? 2.0 * fc : std::sin(2.0 * kPi * fc * k) / (kPi * k);
      // 4-term Blackman-Harris window (~ -92 dB sidelobes) for a clean stopband below 24 kHz.
      const double x = 2.0 * kPi * t / (kTaps - 1);
      const double win = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2.0 * x) -
                         0.01168 * std::cos(3.0 * x);
      const double c = sinc * win;
      h_[t] = static_cast<float>(c);
      sum += c;
    }
    for (int t = 0; t < kTaps; ++t) h_[t] = static_cast<float>(h_[t] / sum);  // unity DC gain
  }

  const unsigned factor_;
  std::array<float, kTaps> h_{};
  std::vector<float> hist_;
  std::vector<float> ext_;
};

}  // namespace st
