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

ListenPacer::ListenPacer(const RingBuffer& ring, unsigned ch)
    : ring_(ring), ch_(ch), cursor_(ring.counter()), buf_(kListenChunkFrames) {}

bool ListenPacer::next(const std::atomic<bool>& running, std::vector<int16_t>* out,
                       uint64_t* start) {
  while (running.load()) {
    const uint64_t now = ring_.counter();

    if (now < cursor_ + kListenChunkFrames) {
      sleep_ms(5);
      continue;
    }

    // A listener that has fallen behind the ring cannot be served the samples it wants;
    // skipping forward to live audio beats sending nothing.
    const uint64_t oldest = ring_.oldest(now);
    if (cursor_ < oldest) {
      LOG_WARN("listener on ch{} fell behind by {} frames — skipping to live", ch_,
               oldest - cursor_);
      cursor_ = now - kListenChunkFrames;
    }

    if (!ring_.read_channel(cursor_, kListenChunkFrames, ch_, buf_.data())) {
      cursor_ = ring_.counter() - kListenChunkFrames;
      continue;
    }

    out->resize(kListenChunkFrames);
    for (unsigned i = 0; i < kListenChunkFrames; ++i) (*out)[i] = float_to_s16(buf_[i]);
    *start = cursor_;
    cursor_ += kListenChunkFrames;
    return true;
  }
  return false;
}

StreamSlot::StreamSlot(std::atomic<unsigned>& active) : active_(active) {
  unsigned cur = active_.load();
  for (;;) {
    if (cur >= kMaxListenStreams) {
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
