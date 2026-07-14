#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "constants.h"
#include "ring_buffer.h"

namespace st {

// Walks one input channel forward in kListenChunkFrames steps, in real time, converting to
// S16. Shared by the WebSocket listen path and the endless-WAV path.
class ListenPacer {
 public:
  ListenPacer(const RingBuffer& ring, unsigned ch);

  // Sleeps until the next chunk exists, then fills `out` and reports the absolute sample
  // index it starts at. False once `running` goes down.
  bool next(const std::atomic<bool>& running, std::vector<int16_t>* out, uint64_t* start);

 private:
  const RingBuffer& ring_;
  const unsigned ch_;
  uint64_t cursor_;
  std::vector<float> buf_;
};

// Caps how many concurrent listeners the server will feed (each holds a worker thread).
class StreamSlot {
 public:
  explicit StreamSlot(std::atomic<unsigned>& active);
  ~StreamSlot();

  bool acquired() const { return acquired_; }

 private:
  std::atomic<unsigned>& active_;
  bool acquired_ = false;
};

}  // namespace st
