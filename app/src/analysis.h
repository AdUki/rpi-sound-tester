#pragma once

#include <array>
#include <atomic>
#include <complex>
#include <deque>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "constants.h"
#include "ring_buffer.h"
#include "util/dsp.h"

namespace st {

// How long a peak meter holds a peak: each peak for this long after the sample it was at.
inline constexpr double kPeakHoldS = 3.0;

// A peak meter's hold: the largest |x| of the last `hold` samples. Every peak is held the full
// hold after its own sample, whatever came before it: a smaller transient during a louder one's
// hold is still held its own full hold once the louder one's runs out, and a louder one re-arms it.
// Kept as one entry per `grain` samples (that slice's largest |x|, at its sample) in a queue whose
// peaks fall as their samples rise, so a slice's later, smaller peak is held to within `grain`.
// Pure (no clock, no ring): the analysis thread feeds it each tick's new samples, tests feed it
// synthetic windows.
class PeakHold {
 public:
  PeakHold() = default;
  PeakHold(uint64_t hold, unsigned grain) : hold_(hold), grain_(grain ? grain : 1) {}

  // Samples [first, first + len) of the channel. Samples already fed (an overlapping window) are
  // skipped, so windows may overlap; a window that skips ahead leaves the gap unmetered.
  void add(const float* x, size_t len, uint64_t first);
  // The hold as of sample `now` (at or after the last sample fed): the largest |x| at a sample s
  // with now - s <= hold, 0 when none.
  float value(uint64_t now);

 private:
  uint64_t hold_ = 0;
  unsigned grain_ = 1;
  uint64_t fed_ = 0;  // samples before this have been fed
  std::deque<std::pair<uint64_t, float>> q_;  // {sample, peak}: samples rising, peaks falling
};

struct ChannelMeters {
  float rms_db = kMinDb;       // the latest 100 ms window
  float peak_db = kMinDb;      // max |x| of the last kPeakHoldS (PeakHold)
  float peak_now_db = kMinDb;  // max |x| of the latest 100 ms window, no hold
  // The last analysis tick (its sample counter) whose samples since the tick before (at least its
  // 100 ms window) peaked above kGenieSoundThresholdDb; meaningful only while loud_seen.
  uint64_t last_loud = 0;
  bool loud_seen = false;
};

// Why a channel has no tone. None while valid.
enum class ToneReason : uint8_t { None, Silent, NotATone };

// Below this the dominant partial is the noise floor rather than a tone: "silent".
inline constexpr float kToneFloorDb = -80.0f;
// A tone is one sinusoid that dominates its band: everything else (THD+N) at most this much of
// it. Music, speech and noise read far above it (50-1000 %), a clean tone in a 16-bit noise floor
// well below.
inline constexpr float kToneMaxThdNPct = 30.0f;

struct ToneMetrics {
  bool valid = false;
  float freq_hz = 0.0f;    // the tone's frequency; 0 unless valid
  float thd_n_pct = 0.0f;  // kept for NotATone, 0 for Silent
  // The dominant FFT bin in 20 Hz-20 kHz, interpolated, whatever the verdict.
  float peak_hz = 0.0f;
  float peak_db = kMinDb;
  ToneReason reason = ToneReason::Silent;
};

const char* tone_reason_name(ToneReason r);  // nullptr for None

// The tone detector over one Hann-windowed power spectrum (|X|^2 per FFT bin, scaled so a
// full-scale sine reads 1.0, i.e. 0 dBFS), bins `bin_hz` apart. Pure, so tests reach it directly.
ToneMetrics tone_from_power(const std::vector<float>& power, double bin_hz);

struct EnvColumn {
  int16_t min[kMaxInputs];
  int16_t max[kMaxInputs];
};

// One min/max column per env_column_frames(rate) of capture, indexed by column number
// (= sample / env_column_frames(rate)) so every consumer agrees on the time axis.
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
  std::array<ChannelMeters, kMaxInputs> meters{};
  std::array<std::vector<float>, kMaxInputs> spectrum;  // dBFS, kSpectrumBins log-spaced
  std::array<ToneMetrics, kMaxInputs> tone{};
};

class Analysis {
 public:
  Analysis(const RingBuffer& ring, double rate);
  ~Analysis();

  void start();
  void stop();

  AnalysisSnapshot snapshot() const;
  EnvelopeRing& envelope() { return env_; }
  // Frames per envelope column at the engine's rate: what a column index is multiplied by to name
  // its first sample.
  unsigned env_column_frames() const { return env_col_frames_; }

  // Center frequency (Hz, geometric mid-band) of each of the kSpectrumBins log-spaced spectrum
  // bins. Fixed at construction — it depends only on the rate — and AnalysisSnapshot::spectrum is
  // indexed the same way, so a headless client can label GET /api/spectrum without reconstructing
  // the log mapping. Read-only after the constructor, so no lock is needed.
  const std::array<float, kSpectrumBins>& bin_hz() const { return bin_hz_; }

 private:
  void run();
  // buf holds samples [now - len, now); the last `rms_len` of them are the 100 ms window.
  void update_meters(unsigned ch, const float* buf, size_t len, size_t rms_len, uint64_t now);
  void update_spectrum(unsigned ch, const float* buf);
  void update_envelope(uint64_t now);

  const RingBuffer& ring_;
  const double rate_;
  const unsigned env_col_frames_;

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

  std::array<std::pair<unsigned, unsigned>, kSpectrumBins> bin_ranges_{};
  std::array<float, kSpectrumBins> bin_hz_{};  // center frequency of each spectrum bin

  std::array<PeakHold, kMaxInputs> peak_hold_{};

  std::vector<float> env_buf_;
};

}  // namespace st
