// RateMeter: GET /api/net frames_per_s, a counter's rate over at least a second, from marks taken
// by whoever reads it. Pure arithmetic on (ns, count) pairs; no clock, no threads.
#include "check.h"
#include "net_rate.h"

using namespace st;

namespace {

constexpr uint64_t kMs = 1000000ull;
constexpr uint64_t kT0 = 5000 * kMs;  // anything but zero

// Nothing to measure against until a mark is a second old.
void test_no_rate_before_a_second_of_history() {
  RateMeter r;
  CHECK(!r.per_second(kT0, 0));
  r.mark(kT0, 0);
  CHECK(!r.per_second(kT0 + 999 * kMs, 95904));
  const auto fps = r.per_second(kT0 + 1000 * kMs, 96000);
  CHECK(fps.has_value());
  CHECK_NEAR(*fps, 96000.0, 1e-6);
}

// Marked every 250 ms, the window is the newest mark at least a second old: 1.0-1.25 s, so a stop
// shows within about a second, not averaged over the whole history.
void test_the_window_follows_the_newest_old_enough_mark() {
  RateMeter r;
  uint64_t frames = 0;
  // 3 s at 96 kHz, marked every 250 ms.
  for (int i = 0; i <= 12; ++i) {
    r.mark(kT0 + i * 250 * kMs, frames);
    frames += 24000;
  }
  frames -= 24000;  // the count at the last mark
  const uint64_t t_last = kT0 + 12 * 250 * kMs;
  CHECK_NEAR(*r.per_second(t_last, frames), 96000.0, 1e-6);
  // The memory stays bounded: only what the next rates can need.
  CHECK(r.marks() <= 6u);

  // The sender stops. 1.25 s later the window holds nothing but the silence.
  for (int i = 1; i <= 5; ++i) r.mark(t_last + i * 250 * kMs, frames);
  CHECK_NEAR(*r.per_second(t_last + 1250 * kMs, frames), 0.0, 1e-9);
  // Half a second after the stop it still reads part of the audio before it.
  RateMeter h;
  h.mark(kT0, 0);
  h.mark(kT0 + 500 * kMs, 48000);
  const auto half = h.per_second(kT0 + 1000 * kMs, 48000);  // base is the mark at kT0
  CHECK_NEAR(*half, 48000.0, 1e-6);
}

// Marks closer than the spacing are not kept: a burst of reads does not crowd out the old mark.
void test_marks_are_spaced() {
  RateMeter r;
  r.mark(kT0, 0);
  for (int i = 1; i < 100; ++i) r.mark(kT0 + i * kMs, i * 96ull);
  CHECK_EQ(r.marks(), 1u);
  r.mark(kT0 + 250 * kMs, 24000);
  CHECK_EQ(r.marks(), 2u);
}

// A forgotten channel's counter goes back to zero: the history before it means nothing.
void test_a_counter_that_goes_back_starts_over() {
  RateMeter r;
  r.mark(kT0, 500000);
  r.mark(kT0 + 1000 * kMs, 596000);
  CHECK_NEAR(*r.per_second(kT0 + 1000 * kMs, 596000), 96000.0, 1e-6);
  // Read with a count below the base, before any mark notices: no rate rather than a huge one.
  CHECK(!r.per_second(kT0 + 1100 * kMs, 10));
  r.mark(kT0 + 1250 * kMs, 0);
  CHECK_EQ(r.marks(), 1u);
  CHECK(!r.per_second(kT0 + 1500 * kMs, 1000));
  CHECK_NEAR(*r.per_second(kT0 + 2250 * kMs, 96000), 96000.0, 1e-6);
}

// Two markers racing: one read the clock first but marks second. Its older mark is dropped, and
// the history (and so the rate) survives.
void test_an_out_of_order_mark_is_ignored() {
  RateMeter r;
  r.mark(kT0, 0);
  r.mark(kT0 + 1000 * kMs, 96000);
  r.mark(kT0 + 1000 * kMs - 5000, 95999);
  CHECK_EQ(r.marks(), 2u);
  CHECK_NEAR(*r.per_second(kT0 + 1000 * kMs, 96000), 96000.0, 1e-6);
}

// Read long after the last mark (nothing watched for a while): still a rate, over that long span.
void test_a_rate_after_a_long_gap_spans_the_gap() {
  RateMeter r;
  r.mark(kT0, 0);
  CHECK_NEAR(*r.per_second(kT0 + 10000 * kMs, 96000), 9600.0, 1e-6);
}

}  // namespace

int main() {
  test_no_rate_before_a_second_of_history();
  test_the_window_follows_the_newest_old_enough_mark();
  test_marks_are_spaced();
  test_a_counter_that_goes_back_starts_over();
  test_an_out_of_order_mark_is_ignored();
  test_a_rate_after_a_long_gap_spans_the_gap();
  return report("net_rate");
}
