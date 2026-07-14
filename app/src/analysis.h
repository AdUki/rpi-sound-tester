#pragma once

#include <array>
#include <atomic>
#include <complex>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "constants.h"
#include "ring_buffer.h"
#include "util/dsp.h"

namespace st {

struct ChannelMeters {
  float rms_db = kMinDb;
  float peak_db = kMinDb;
  float dc = 0.0f;
};

struct ToneMetrics {
  bool valid = false;
  float freq_hz = 0.0f;
  float level_db = kMinDb;
  float thd_n_pct = 0.0f;
  float thd_n_db = 0.0f;
};

struct EnvColumn {
  int16_t min[kInputs];
  int16_t max[kInputs];
};

// One min/max column per kEnvColumnFrames of capture, indexed by column number
// (= sample / kEnvColumnFrames) so every consumer agrees on the time axis.
class EnvelopeRing {
 public:
  EnvelopeRing() : cols_(kEnvColumns) {}

  void push(uint64_t col_index, const EnvColumn& c) {
    std::lock_guard<std::mutex> lock(m_);
    cols_[col_index % kEnvColumns] = c;
    head_ = col_index + 1;
  }

  uint64_t head() const {
    std::lock_guard<std::mutex> lock(m_);
    return head_;
  }

  // Copies columns [from, head) — at most kEnvColumns back — and reports the first index
  // actually returned.
  std::vector<EnvColumn> since(uint64_t from, uint64_t* first) const {
    std::lock_guard<std::mutex> lock(m_);
    const uint64_t oldest = head_ > kEnvColumns ? head_ - kEnvColumns : 0;
    if (from < oldest) from = oldest;
    *first = from;
    std::vector<EnvColumn> out;
    out.reserve(static_cast<size_t>(head_ - from));
    for (uint64_t i = from; i < head_; ++i) out.push_back(cols_[i % kEnvColumns]);
    return out;
  }

 private:
  mutable std::mutex m_;
  std::vector<EnvColumn> cols_;
  uint64_t head_ = 0;
};

struct AnalysisSnapshot {
  uint64_t sample = 0;
  std::array<ChannelMeters, kInputs> meters{};
  std::array<std::vector<float>, kInputs> spectrum;  // dBFS, kSpectrumBins log-spaced
  std::array<ToneMetrics, kInputs> tone{};
};

class Analysis {
 public:
  Analysis(const RingBuffer& ring, double rate);
  ~Analysis();

  void start();
  void stop();

  AnalysisSnapshot snapshot() const;
  EnvelopeRing& envelope() { return env_; }
  const std::vector<float>& bin_freqs() const { return bin_freqs_; }

 private:
  void run();
  void update_meters(unsigned ch, const float* buf, size_t len, uint64_t now);
  void update_spectrum(unsigned ch, const float* buf);
  void update_envelope(uint64_t now);

  const RingBuffer& ring_;
  const double rate_;

  std::thread thread_;
  std::atomic<bool> running_{false};

  mutable std::mutex m_;
  AnalysisSnapshot snap_;

  EnvelopeRing env_;
  uint64_t env_col_ = 0;  // next column index to produce

  std::vector<float> window_;   // Hann
  std::vector<float> scratch_;  // kSpectrumFft windowed samples
  std::vector<std::complex<float>> spec_;
  std::vector<float> power_;  // |X|^2 per FFT bin

  std::vector<float> bin_freqs_;  // centre frequency of each display bin
  std::array<std::pair<unsigned, unsigned>, kSpectrumBins> bin_ranges_{};

  std::array<float, kInputs> peak_hold_{};
  std::array<uint64_t, kInputs> peak_time_{};

  std::vector<float> env_buf_;
};

}  // namespace st
