#pragma once

#include <cstdint>
#include <vector>

#include "util/clock.h"

namespace st {

// A clock that moves only when it is told to. A sleep returns at once, having moved the clock on
// to its deadline if it was not there yet, which is what a sleep that woke exactly on time looks
// like to the thread that slept; every deadline asked for is kept, in order, so a test can read a
// cadence back. For one thread driving the engine a step at a time, not for racing one.
class ManualClock final : public Clock {
 public:
  // Far from zero, so that a time this clock stamped cannot pass for one never set.
  static constexpr uint64_t kStartNs = 1000000000000ull;  // 1000 s

  explicit ManualClock(uint64_t start_ns = kStartNs) : now_(start_ns) {}

  uint64_t now_ns() const override { return now_; }
  void sleep_until(uint64_t t_ns) override {
    sleeps.push_back(t_ns);
    if (t_ns > now_) now_ = t_ns;
  }

  void advance(uint64_t dt_ns) { now_ += dt_ns; }

  std::vector<uint64_t> sleeps;

 private:
  uint64_t now_;
};

}  // namespace st
