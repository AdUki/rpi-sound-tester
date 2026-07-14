#include "ring_buffer.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

#include "check.h"

using namespace st;

namespace {

constexpr size_t kFrames = 1024;
constexpr unsigned kCh = 2;
constexpr size_t kSafety = 64;

void test_read_back_what_was_written() {
  RingBuffer ring(kFrames, kCh, kSafety);
  std::vector<float> block(128 * kCh);
  for (size_t i = 0; i < 128; ++i) {
    block[i * kCh + 0] = static_cast<float>(i);
    block[i * kCh + 1] = static_cast<float>(i) + 1000.0f;
  }
  ring.write(block.data(), 128);
  CHECK_EQ(ring.counter(), 128u);

  std::vector<float> out(128);
  CHECK(ring.read_channel(0, 128, 0, out.data()));
  for (size_t i = 0; i < 128; ++i) CHECK_EQ(out[i], static_cast<float>(i));

  CHECK(ring.read_channel(0, 128, 1, out.data()));
  for (size_t i = 0; i < 128; ++i) CHECK_EQ(out[i], static_cast<float>(i) + 1000.0f);
}

void test_wraparound_is_seamless() {
  RingBuffer ring(kFrames, kCh, kSafety);
  std::vector<float> block(100 * kCh);

  // Write past the end of the ring so every read has to stitch two segments.
  for (unsigned b = 0; b < 15; ++b) {
    for (size_t i = 0; i < 100; ++i) {
      const float v = static_cast<float>(b * 100 + i);
      block[i * kCh + 0] = v;
      block[i * kCh + 1] = -v;
    }
    ring.write(block.data(), 100);
  }
  CHECK_EQ(ring.counter(), 1500u);

  const uint64_t start = 1400;
  std::vector<float> out(100);
  CHECK(ring.read_channel(start, 100, 0, out.data()));
  for (size_t i = 0; i < 100; ++i) CHECK_EQ(out[i], static_cast<float>(start + i));

  std::vector<float> inter(100 * kCh);
  CHECK(ring.read_interleaved(start, 100, inter.data()));
  for (size_t i = 0; i < 100; ++i) {
    CHECK_EQ(inter[i * kCh + 0], static_cast<float>(start + i));
    CHECK_EQ(inter[i * kCh + 1], -static_cast<float>(start + i));
  }
}

void test_rejects_future_and_overwritten_ranges() {
  RingBuffer ring(kFrames, kCh, kSafety);
  std::vector<float> block(kFrames * kCh, 1.0f);
  ring.write(block.data(), 512);

  std::vector<float> out(kFrames);
  // Not written yet.
  CHECK(!ring.read_channel(500, 100, 0, out.data()));
  // Older than the ring can still hold.
  ring.write(block.data(), 900);
  CHECK(!ring.read_channel(0, 100, 0, out.data()));
  // Inside the guaranteed span.
  const uint64_t now = ring.counter();
  CHECK(ring.read_channel(ring.oldest(now), 100, 0, out.data()));
}

void test_oldest_tracks_the_safety_margin() {
  RingBuffer ring(kFrames, kCh, kSafety);
  CHECK_EQ(ring.oldest(0), 0u);
  CHECK_EQ(ring.oldest(100), 0u);
  CHECK_EQ(ring.oldest(5000), 5000u - (kFrames - kSafety));
}

// Runs a writer and a reader concurrently. The writer stamps sample n with the value n, so
// any inconsistency in what the reader gets back is detectable. Returns {accepted, torn}.
std::pair<uint64_t, uint64_t> race(size_t ring_frames, size_t block, size_t read_len,
                                   int reads, long writer_pause_us, long reader_pause_us) {
  RingBuffer ring(ring_frames, 1, kSafety);
  std::atomic<bool> stop{false};
  uint64_t torn = 0, accepted = 0;

  std::thread writer([&] {
    std::vector<float> b(block);
    uint64_t n = 0;
    while (!stop.load()) {
      for (size_t i = 0; i < block; ++i) b[i] = static_cast<float>(n + i);
      ring.write(b.data(), block);
      n += block;
      if (writer_pause_us) std::this_thread::sleep_for(std::chrono::microseconds(writer_pause_us));
    }
  });

  std::vector<float> out(read_len);
  for (int iter = 0; iter < reads; ++iter) {
    if (reader_pause_us) std::this_thread::sleep_for(std::chrono::microseconds(reader_pause_us));
    const uint64_t now = ring.counter();
    if (now < 2 * read_len) continue;  // ring not primed yet
    const uint64_t start = now - read_len;
    if (!ring.read_channel(start, read_len, 0, out.data())) continue;
    ++accepted;
    for (size_t i = 0; i < read_len; ++i) {
      if (out[i] != static_cast<float>(start + i)) {
        ++torn;
        break;
      }
    }
  }

  stop.store(true);
  writer.join();
  return {accepted, torn};
}

// What the seqlock post-check is for: a reader racing the writer must never be handed a
// silently stitched-together mix of old and new samples. The writer here runs flat out,
// lapping the whole ring in microseconds — far more hostile than a sound card, which advances
// one period per 10.7 ms. Most reads get rejected, which is fine: rejected beats wrong.
void test_a_hostile_writer_never_produces_a_torn_read() {
  const auto [accepted, torn] = race(kFrames, 64, 256, 200000, 0, 0);
  std::cout << "  hostile writer: " << accepted << " reads accepted, " << torn << " torn\n";
  CHECK_EQ(torn, 0u);
}

// With the writer paced anything like a real card, reads must actually succeed — a ring that
// only ever rejects would be safe and useless.
void test_reads_succeed_against_a_realistically_paced_writer() {
  // Writer: 1024 frames per 200 us (~5 M frames/s, still ~50x a 96 kHz card). Reader polls
  // every 100 us, like the analysis and streaming threads do.
  const auto [accepted, torn] = race(1 << 16, 1024, 4096, 1500, 200, 100);
  std::cout << "  paced writer:   " << accepted << " reads accepted, " << torn << " torn\n";
  CHECK(accepted > 0);
  CHECK_EQ(torn, 0u);
}

}  // namespace

int main() {
  test_read_back_what_was_written();
  test_wraparound_is_seamless();
  test_rejects_future_and_overwritten_ranges();
  test_oldest_tracks_the_safety_margin();
  test_a_hostile_writer_never_produces_a_torn_read();
  test_reads_succeed_against_a_realistically_paced_writer();
  return report("ring_buffer");
}
