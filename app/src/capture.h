#pragma once

#include <complex>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "constants.h"
#include "ring_buffer.h"

namespace st {

struct CaptureStatus {
  bool frozen = false;
  uint64_t freeze_sample = 0;
  uint64_t valid_start = 0;
  uint64_t valid_len = 0;
  uint32_t generation = 0;
};

struct WindowResult {
  bool ok = false;
  std::string error;
  bool raw = false;             // true: `samples` holds one value per point
  uint64_t start = 0;
  uint64_t len = 0;
  std::vector<float> samples;   // raw mode
  std::vector<float> mins;      // column mode
  std::vector<float> maxs;
};

struct XcorrResult {
  bool ok = false;
  std::string error;
  int64_t lag_samples = 0;
  double lag_ms = 0.0;
  double confidence = 0.0;
  double peak = 0.0;
};

// Holds the frozen copy of the ring used for all sync measurements.
class CaptureStore {
 public:
  CaptureStore(const RingBuffer& ring, double rate, unsigned period);

  CaptureStatus freeze(uint32_t generation);
  void resume();
  CaptureStatus status() const;

  // start/len are absolute sample indices on the shared counter axis. Serves the frozen
  // snapshot when frozen, otherwise best-effort from the live ring.
  WindowResult window(unsigned ch, uint64_t start, uint64_t len, unsigned cols) const;

  // Frozen snapshot only: a live read of up to 2^19 frames would race the writer.
  XcorrResult xcorr(unsigned ch_a, unsigned ch_b, uint64_t start, uint64_t len);

 private:
  bool snapshot_read(unsigned ch, uint64_t start, uint64_t len, float* out) const;

  const RingBuffer& ring_;
  const double rate_;
  const unsigned period_;

  mutable std::mutex m_;
  std::vector<float> snap_;  // interleaved, kInputs channels
  uint64_t snap_base_ = 0;   // absolute sample index of snap_[0]
  CaptureStatus status_;

  std::mutex fft_m_;
  std::vector<float> fa_, fb_, corr_;
  std::vector<std::complex<float>> ca_, cb_;
};

}  // namespace st
