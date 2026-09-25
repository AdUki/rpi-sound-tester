// The scope's envelope columns: a fixed 1/200 s of capture each, so their width in frames follows
// the engine's rate.
#include "analysis.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "channel_layout.h"
#include "check.h"
#include "constants.h"
#include "rates.h"
#include "ring_buffer.h"
#include "util/dsp.h"

using namespace st;

namespace {

// What column k's first sample holds on channel 0, and, negated, what its last sample holds: the
// column's min and max. Anything else is silence, so a column cut at the wrong frame would miss one
// of them or take a neighbour's.
float spike(uint64_t k) { return static_cast<float>(1 + k % 100) / 256.0f; }

void test_columns_are_a_two_hundredth_of_a_second(unsigned rate) {
  const unsigned frames = rate / 200;
  CHECK_EQ(env_column_frames(rate), frames);

  // Longer than the analysis thread's warm-up, which is its widest window.
  const uint64_t total = 48000;
  RingBuffer ring(1u << 16, st::channels().total(), 2 * kTestPeriod);
  std::vector<float> block(static_cast<size_t>(kTestPeriod) * st::channels().total());
  for (uint64_t n = 0; n < total; n += kTestPeriod) {
    std::fill(block.begin(), block.end(), 0.0f);
    for (unsigned i = 0; i < kTestPeriod; ++i) {
      const uint64_t t = n + i;
      if (t % frames == 0) block[i * st::channels().total()] = -spike(t / frames);
      if (t % frames == frames - 1) block[i * st::channels().total()] = spike(t / frames);
    }
    ring.write(block.data(), kTestPeriod);
  }

  Analysis analysis(ring, rate);
  CHECK_EQ(analysis.env_column_frames(), frames);
  analysis.start();
  const uint64_t want = total / frames;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (analysis.envelope().head() < want && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  analysis.stop();

  uint64_t first = 0;
  const std::vector<EnvColumn> cols = analysis.envelope().since(0, &first);
  CHECK_EQ(first, 0u);
  CHECK_EQ(cols.size(), static_cast<size_t>(want));
  bool exact = true;
  for (size_t k = 0; k < cols.size(); ++k) {
    exact &= cols[k].min[0] == float_to_s16(-spike(k));
    exact &= cols[k].max[0] == float_to_s16(spike(k));
    exact &= cols[k].min[1] == 0 && cols[k].max[1] == 0;
  }
  CHECK(exact);
}

// The Pi's columns are the 480 frames they were as a constant.
void test_the_pi_keeps_480() {
  CHECK_EQ(env_column_frames(96000), 480u);
  CHECK_EQ(env_column_frames(48000), 240u);
  // A rate too low to make sense still leaves the analysis thread something to divide by.
  CHECK_EQ(env_column_frames(0), 1u);
}

}  // namespace

int main() {
  for (const unsigned rate : kTestRates) {
    std::printf("  at %u Hz\n", rate);
    test_columns_are_a_two_hundredth_of_a_second(rate);
  }
  test_the_pi_keeps_480();
  return report("analysis");
}
