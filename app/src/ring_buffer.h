#pragma once

#include <atomic>
#include <cstring>
#include <sys/mman.h>

#include <algorithm>
#include <vector>

#include "constants.h"

namespace st {

// Single-writer (audio thread) / many-reader lock-free ring of interleaved float frames.
//
// Readers must tolerate being lapped by the writer. A pre-check alone is not enough: a
// reader that stalls mid-copy (page fault, preemption) would return a mix of old and new
// samples while believing them valid. Every read therefore re-reads the counter after
// copying and fails if the writer has since wrapped past the requested start.
class RingBuffer {
 public:
  RingBuffer(size_t frames, unsigned channels, size_t safety_frames)
      : frames_(frames),
        mask_(frames - 1),
        channels_(channels),
        safety_(safety_frames),
        buf_(frames * channels, 0.0f) {
    // frames must be a power of two: the index is computed with a mask.
    mlock(buf_.data(), buf_.size() * sizeof(float));
  }

  ~RingBuffer() { munlock(buf_.data(), buf_.size() * sizeof(float)); }

  size_t frames() const { return frames_; }
  unsigned channels() const { return channels_; }

  uint64_t counter() const { return n_.load(std::memory_order_acquire); }

  // Oldest sample index a reader may still ask for, given the current write head.
  uint64_t oldest(uint64_t head) const {
    const size_t span = frames_ - safety_;
    return head > span ? head - span : 0;
  }

  // Audio thread only.
  void write(const float* interleaved, size_t frames) {
    const uint64_t n = n_.load(std::memory_order_relaxed);
    const size_t idx = static_cast<size_t>(n & mask_);
    const size_t first = std::min(frames, frames_ - idx);
    std::memcpy(&buf_[idx * channels_], interleaved, first * channels_ * sizeof(float));
    if (first < frames) {
      std::memcpy(&buf_[0], interleaved + first * channels_,
                  (frames - first) * channels_ * sizeof(float));
    }
    n_.store(n + frames, std::memory_order_release);
  }

  // Copies one channel of [start, start+len) into out. False if the range is not (or no
  // longer) entirely present in the ring.
  bool read_channel(uint64_t start, size_t len, unsigned ch, float* out) const {
    if (ch >= channels_ || len == 0 || len > frames_) return false;
    const uint64_t n1 = n_.load(std::memory_order_acquire);
    if (start + len > n1) return false;
    if (n1 - start > frames_ - safety_) return false;

    for (size_t i = 0; i < len; ++i) {
      const size_t idx = static_cast<size_t>((start + i) & mask_);
      out[i] = buf_[idx * channels_ + ch];
    }

    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t n2 = n_.load(std::memory_order_relaxed);
    return n2 - start <= frames_;
  }

  // Copies all channels of [start, start+len) interleaved into out (len*channels floats).
  bool read_interleaved(uint64_t start, size_t len, float* out) const {
    if (len == 0 || len > frames_) return false;
    const uint64_t n1 = n_.load(std::memory_order_acquire);
    if (start + len > n1) return false;
    if (n1 - start > frames_ - safety_) return false;

    const size_t idx = static_cast<size_t>(start & mask_);
    const size_t first = std::min(len, frames_ - idx);
    std::memcpy(out, &buf_[idx * channels_], first * channels_ * sizeof(float));
    if (first < len) {
      std::memcpy(out + first * channels_, &buf_[0], (len - first) * channels_ * sizeof(float));
    }

    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t n2 = n_.load(std::memory_order_relaxed);
    return n2 - start <= frames_;
  }

  const float* raw() const { return buf_.data(); }

 private:
  const size_t frames_;
  const size_t mask_;
  const unsigned channels_;
  const size_t safety_;
  std::vector<float> buf_;
  std::atomic<uint64_t> n_{0};
};

}  // namespace st
