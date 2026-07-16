#include "listen_stream.h"

#include <time.h>

#include <string>

#include "util/dsp.h"
#include "util/log.h"

namespace st {

namespace {

void sleep_ms(long ms) {
  timespec ts{ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, nullptr);
}

// The pacing loop both pacer classes share: sleep until the next chunk exists, skip forward to
// live audio when the listener has fallen behind the ring, and advance the cursor only once
// `read(cursor)` — the one thing the two pacers do differently — has succeeded.
template <class ReadFn>
bool pace(const RingBuffer& ring, unsigned chunk, uint64_t* cursor,
          const std::atomic<bool>& running, uint64_t* start, bool* skipped,
          const std::string& who, ReadFn read) {
  if (skipped) *skipped = false;
  while (running.load()) {
    const uint64_t now = ring.counter();

    if (now < *cursor + chunk) {
      sleep_ms(5);
      continue;
    }

    // A listener that has fallen behind the ring cannot be served the samples it wants;
    // skipping forward to live audio beats sending nothing.
    const uint64_t oldest = ring.oldest(now);
    if (*cursor < oldest) {
      LOG_WARN("{} fell behind by {} frames — skipping to live", who, oldest - *cursor);
      *cursor = now - chunk;
      if (skipped) *skipped = true;
    }

    if (!read(*cursor)) {
      *cursor = ring.counter() - chunk;
      if (skipped) *skipped = true;
      continue;
    }

    *start = *cursor;
    *cursor += chunk;
    return true;
  }
  return false;
}

}  // namespace

ListenPacer::ListenPacer(const RingBuffer& ring, unsigned ch, unsigned chunk_frames)
    : ring_(ring), ch_(ch), chunk_(chunk_frames), cursor_(ring.counter()), buf_(chunk_frames) {}

bool ListenPacer::wait_and_read(const std::atomic<bool>& running, uint64_t* start, bool* skipped) {
  return pace(ring_, chunk_, &cursor_, running, start, skipped,
              "listener on ch" + std::to_string(ch_),
              [this](uint64_t cur) { return ring_.read_channel(cur, chunk_, ch_, buf_.data()); });
}

bool ListenPacer::next(const std::atomic<bool>& running, std::vector<int16_t>* out,
                       uint64_t* start) {
  if (!wait_and_read(running, start, nullptr)) return false;
  out->resize(chunk_);
  for (unsigned i = 0; i < chunk_; ++i) (*out)[i] = float_to_s16(buf_[i]);
  return true;
}

bool ListenPacer::next_float(const std::atomic<bool>& running, const float** out, uint64_t* start,
                             bool* skipped) {
  if (!wait_and_read(running, start, skipped)) return false;
  *out = buf_.data();
  return true;
}

MultiListenPacer::MultiListenPacer(const RingBuffer& ring, unsigned chunk_frames)
    : ring_(ring), chunk_(chunk_frames), cursor_(ring.counter()), buf_(chunk_frames * kInputs) {}

bool MultiListenPacer::next_float(const std::atomic<bool>& running, const float** out,
                                  uint64_t* start, bool* skipped) {
  if (!pace(ring_, chunk_, &cursor_, running, start, skipped, "multichannel listener",
            [this](uint64_t cur) { return ring_.read_interleaved(cur, chunk_, buf_.data()); })) {
    return false;
  }
  *out = buf_.data();
  return true;
}

StreamSlot::StreamSlot(std::atomic<unsigned>& active, unsigned limit, const char* what)
    : active_(active), limit_(limit) {
  unsigned cur = active_.load();
  for (;;) {
    if (cur >= limit_) {
      LOG_WARN("refusing {} stream: {} already active", what, cur);
      return;
    }
    if (active_.compare_exchange_weak(cur, cur + 1)) {
      acquired_ = true;
      return;
    }
  }
}

StreamSlot::~StreamSlot() {
  if (acquired_) active_.fetch_sub(1);
}

}  // namespace st
