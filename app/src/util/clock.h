#pragma once

#include <time.h>

#include <cstdint>

namespace st {

// CLOCK_MONOTONIC in nanoseconds — the clock both ends of the network link agree to talk in.
inline uint64_t mono_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// The time as a thread that paces itself sees it. The audio engine is handed one, so that a test
// can run it on time the test controls: the anchor each block publishes and the simulator's pacing
// then follow that clock rather than the host's, and nothing waits for real. Called a few times
// per block at most, never per sample.
struct Clock {
  virtual ~Clock() = default;
  virtual uint64_t now_ns() const = 0;
  // Sleeps until now_ns() reaches `t_ns`; a time already past returns at once. A signal can cut it
  // short, as it can any sleep, so a caller keeping a cadence steps its deadline, not the time it
  // woke up at.
  virtual void sleep_until(uint64_t t_ns) = 0;
};

// The host's CLOCK_MONOTONIC, the clock every thread on the device already shares.
struct MonotonicClock final : Clock {
  uint64_t now_ns() const override { return mono_ns(); }
  void sleep_until(uint64_t t_ns) override {
    const timespec ts{static_cast<time_t>(t_ns / 1000000000ull),
                      static_cast<long>(t_ns % 1000000000ull)};
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
  }
};

// The one the whole process runs on unless a test hands something else in.
inline Clock& monotonic_clock() {
  static MonotonicClock clock;
  return clock;
}

}  // namespace st
