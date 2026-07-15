#include "listen_stream.h"

#include <time.h>

#include "util/dsp.h"
#include "util/log.h"

namespace st {

namespace {

void sleep_ms(long ms) {
  timespec ts{ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, nullptr);
}

}  // namespace

ListenPacer::ListenPacer(const RingBuffer& ring, unsigned ch, unsigned chunk_frames)
    : ring_(ring), ch_(ch), chunk_(chunk_frames), cursor_(ring.counter()), buf_(chunk_frames) {}

bool ListenPacer::wait_and_read(const std::atomic<bool>& running, uint64_t* start, bool* skipped) {
  if (skipped) *skipped = false;
  while (running.load()) {
    const uint64_t now = ring_.counter();

    if (now < cursor_ + chunk_) {
      sleep_ms(5);
      continue;
    }

    // A listener that has fallen behind the ring cannot be served the samples it wants;
    // skipping forward to live audio beats sending nothing.
    const uint64_t oldest = ring_.oldest(now);
    if (cursor_ < oldest) {
      LOG_WARN("listener on ch{} fell behind by {} frames — skipping to live", ch_,
               oldest - cursor_);
      cursor_ = now - chunk_;
      if (skipped) *skipped = true;
    }

    if (!ring_.read_channel(cursor_, chunk_, ch_, buf_.data())) {
      cursor_ = ring_.counter() - chunk_;
      if (skipped) *skipped = true;
      continue;
    }

    *start = cursor_;
    cursor_ += chunk_;
    return true;
  }
  return false;
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
  if (skipped) *skipped = false;
  while (running.load()) {
    const uint64_t now = ring_.counter();

    if (now < cursor_ + chunk_) {
      sleep_ms(5);
      continue;
    }

    const uint64_t oldest = ring_.oldest(now);
    if (cursor_ < oldest) {
      LOG_WARN("multichannel listener fell behind by {} frames — skipping to live",
               oldest - cursor_);
      cursor_ = now - chunk_;
      if (skipped) *skipped = true;
    }

    if (!ring_.read_interleaved(cursor_, chunk_, buf_.data())) {
      cursor_ = ring_.counter() - chunk_;
      if (skipped) *skipped = true;
      continue;
    }

    *start = cursor_;
    cursor_ += chunk_;
    *out = buf_.data();
    return true;
  }
  return false;
}

StreamSlot::StreamSlot(std::atomic<unsigned>& active, unsigned limit)
    : active_(active), limit_(limit) {
  unsigned cur = active_.load();
  for (;;) {
    if (cur >= limit_) {
      LOG_WARN("refusing listen stream: {} already active", cur);
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
