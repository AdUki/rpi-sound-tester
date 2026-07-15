#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "constants.h"
#include "ring_buffer.h"

namespace st {

// Walks one input channel forward in chunk_frames steps, in real time. The float chunk is the
// primitive (the Opus encoder needs it); the int16 overload converts on top. Shared by the
// WebSocket listen path and the endless-WAV path.
class ListenPacer {
 public:
  ListenPacer(const RingBuffer& ring, unsigned ch, unsigned chunk_frames = kListenChunkFrames);

  // Sleeps until the next chunk exists, then converts it to S16 in `out` and reports the absolute
  // sample index it starts at. False once `running` goes down.
  bool next(const std::atomic<bool>& running, std::vector<int16_t>* out, uint64_t* start);

  // Same pacing, but hands back the raw float chunk (chunk_frames() samples) owned by the pacer.
  // `*skipped` is set when the listener had fallen behind and the cursor jumped forward to live —
  // a signal to reset any stateful encoder before feeding the returned block.
  bool next_float(const std::atomic<bool>& running, const float** out, uint64_t* start,
                  bool* skipped);

  unsigned chunk_frames() const { return chunk_; }

 private:
  bool wait_and_read(const std::atomic<bool>& running, uint64_t* start, bool* skipped);

  const RingBuffer& ring_;
  const unsigned ch_;
  const unsigned chunk_;
  uint64_t cursor_;
  std::vector<float> buf_;
};

// Like ListenPacer but reads every input channel at one cursor, so all channels share a single
// sample clock — the property the multichannel Ogg/Opus stream needs to stay sample-aligned.
// Hands back kInputs-interleaved float frames (chunk_frames() * kInputs samples).
class MultiListenPacer {
 public:
  explicit MultiListenPacer(const RingBuffer& ring, unsigned chunk_frames = kListenChunkFrames);

  bool next_float(const std::atomic<bool>& running, const float** out, uint64_t* start,
                  bool* skipped);

  unsigned chunk_frames() const { return chunk_; }

 private:
  const RingBuffer& ring_;
  const unsigned chunk_;
  uint64_t cursor_;
  std::vector<float> buf_;
};

// Caps how many concurrent listeners the server will feed (each holds a worker thread). The
// multichannel Ogg endpoint reuses this with its own (smaller) counter and limit.
class StreamSlot {
 public:
  explicit StreamSlot(std::atomic<unsigned>& active, unsigned limit = kMaxListenStreams);
  ~StreamSlot();

  bool acquired() const { return acquired_; }

 private:
  std::atomic<unsigned>& active_;
  const unsigned limit_;
  bool acquired_ = false;
};

}  // namespace st
