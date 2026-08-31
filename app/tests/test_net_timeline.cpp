#include "net_audio.h"

#include <cmath>
#include <vector>

#include "check.h"
#include "constants.h"

using namespace st;

namespace {

constexpr size_t kFrames = 4096;  // power of two, like the real thing
constexpr uint64_t kGuard = 64;
constexpr uint64_t kMaxLead = 2048;

std::vector<float> ramp(size_t n, float base) {
  std::vector<float> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = base + static_cast<float>(i);
  return v;
}

// Reads one channel back out of a kTotalInputs-interleaved block.
std::vector<float> read_one(NetTimeline& t, uint64_t at, size_t n, bool clear = true) {
  std::vector<float> block(n * kTotalInputs, -1.0f);
  t.read(at, n, block.data(), kTotalInputs, clear);
  std::vector<float> out(n);
  for (size_t i = 0; i < n; ++i) out[i] = block[i * kTotalInputs];
  return out;
}

// A packet lands at the index it names, not at the position it arrived in. This is the property
// the whole feature rests on, so it is worth stating plainly.
void test_writes_land_at_their_absolute_index() {
  NetTimeline t(kFrames);
  const auto data = ramp(128, 100.0f);
  CHECK(t.write(1000, data.data(), 128, 0, kGuard, kMaxLead) == NetTimeline::Write::Ok);

  const auto got = read_one(t, 1000, 128);
  for (size_t i = 0; i < 128; ++i) CHECK_EQ(got[i], 100.0f + static_cast<float>(i));

  // Nothing was written before it, and nothing leaked backwards.
  const auto before = read_one(t, 800, 64);
  for (float v : before) CHECK_EQ(v, 0.0f);
}

// Reordering sorts itself out for free: the later packet may arrive first.
void test_out_of_order_arrival_still_lands_correctly() {
  NetTimeline t(kFrames);
  const auto second = ramp(64, 200.0f);
  const auto first = ramp(64, 100.0f);
  CHECK(t.write(1064, second.data(), 64, 0, kGuard, kMaxLead) == NetTimeline::Write::Ok);
  CHECK(t.write(1000, first.data(), 64, 0, kGuard, kMaxLead) == NetTimeline::Write::Ok);

  const auto got = read_one(t, 1000, 128);
  for (size_t i = 0; i < 64; ++i) CHECK_EQ(got[i], 100.0f + static_cast<float>(i));
  for (size_t i = 0; i < 64; ++i) CHECK_EQ(got[64 + i], 200.0f + static_cast<float>(i));
}

// A packet whose slot has already gone past the reader is dropped, never slid forward: sliding it
// would put the audio at the wrong instant, which is the one thing this device must not do.
void test_late_and_out_of_range_are_refused() {
  NetTimeline t(kFrames);
  const auto data = ramp(64, 1.0f);

  CHECK(t.write(500, data.data(), 64, 1000, kGuard, kMaxLead) == NetTimeline::Write::Late);
  // Inside the guard still counts as late: the reader is about to consume it.
  CHECK(t.write(1000 + kGuard - 1, data.data(), 64, 1000, kGuard, kMaxLead) ==
        NetTimeline::Write::Late);
  CHECK(t.write(1000 + kMaxLead + 1, data.data(), 64, 1000, kGuard, kMaxLead) ==
        NetTimeline::Write::OutOfRange);
  // A packet larger than the whole timeline cannot be placed anywhere.
  std::vector<float> huge(kFrames + 1, 0.0f);
  CHECK(t.write(2000, huge.data(), kFrames + 1, 1000, kGuard, kMaxLead) ==
        NetTimeline::Write::OutOfRange);

  CHECK(t.write(1000 + kGuard, data.data(), 64, 1000, kGuard, kMaxLead) == NetTimeline::Write::Ok);
}

// Frames that never arrived read as silence, and the reader clears behind itself so a later lap
// cannot serve up audio from a ring ago.
void test_gaps_read_as_silence_and_do_not_repeat() {
  NetTimeline t(kFrames);
  const auto data = ramp(64, 7.0f);
  CHECK(t.write(100, data.data(), 64, 0, kGuard, kMaxLead) == NetTimeline::Write::Ok);

  const auto got = read_one(t, 100, 128);
  for (size_t i = 0; i < 64; ++i) CHECK_EQ(got[i], 7.0f + static_cast<float>(i));
  for (size_t i = 64; i < 128; ++i) CHECK_EQ(got[i], 0.0f);  // the hole

  // One lap later the same slots must be silent, not a replay.
  const auto lap = read_one(t, 100 + kFrames, 64);
  for (float v : lap) CHECK_EQ(v, 0.0f);
}

// The two readers: playout at n reads without clearing, the trailing ring read at n-delay clears.
// If the live read cleared, the trailing read would find the audio already gone — which is
// exactly the bug that would silently empty every network channel in the scope.
void test_live_read_does_not_consume_what_the_trailing_read_needs() {
  NetTimeline t(kFrames);
  const auto data = ramp(64, 5.0f);
  CHECK(t.write(2000, data.data(), 64, 0, kGuard, kMaxLead) == NetTimeline::Write::Ok);

  const auto live = read_one(t, 2000, 64, /*clear=*/false);
  for (size_t i = 0; i < 64; ++i) CHECK_EQ(live[i], 5.0f + static_cast<float>(i));

  const auto trailing = read_one(t, 2000, 64, /*clear=*/true);
  for (size_t i = 0; i < 64; ++i) CHECK_EQ(trailing[i], 5.0f + static_cast<float>(i));

  const auto after = read_one(t, 2000, 64);
  for (float v : after) CHECK_EQ(v, 0.0f);
}

void test_write_and_read_wrap_the_ring() {
  NetTimeline t(kFrames);
  const uint64_t at = kFrames - 32;  // straddles the wrap
  const auto data = ramp(64, 11.0f);
  CHECK(t.write(at, data.data(), 64, 0, kGuard, 8192) == NetTimeline::Write::Ok);

  const auto got = read_one(t, at, 64);
  for (size_t i = 0; i < 64; ++i) CHECK_EQ(got[i], 11.0f + static_cast<float>(i));
}

void test_write_end_tracks_the_far_edge() {
  NetTimeline t(kFrames);
  const auto data = ramp(64, 1.0f);
  CHECK_EQ(t.write_end(), static_cast<uint64_t>(0));
  CHECK(t.write(1000, data.data(), 64, 0, kGuard, kMaxLead) == NetTimeline::Write::Ok);
  CHECK_EQ(t.write_end(), static_cast<uint64_t>(1064));
  // An earlier packet arriving late in wall-clock order must not pull write_end backwards.
  CHECK(t.write(900, data.data(), 64, 0, kGuard, kMaxLead) == NetTimeline::Write::Ok);
  CHECK_EQ(t.write_end(), static_cast<uint64_t>(1064));

  t.reset();
  CHECK_EQ(t.write_end(), static_cast<uint64_t>(0));
}

// ---- the control loop -------------------------------------------------------------------

// The measurement is noisy for reasons that are not drift: network jitter, and a reader position
// that only moves once per audio block. The filter has to take that out, or the trim spends its
// whole authority chasing it.
void test_the_filter_removes_measurement_noise() {
  LeadFilter f;
  const double dt = 256.0 / 44100.0;  // one packet
  const double truth = 96000.0;

  // A block's worth of quantisation, alternating — the worst case of what reader_n_ does.
  // Measured only once the filter has settled: it starts at the first sample it is given, so the
  // first few time constants are it converging, not noise it failed to reject.
  const int settle = static_cast<int>(6 * 2.0 / dt);
  double worst = 0.0;
  for (int i = 0; i < settle * 2; ++i) {
    const double noisy = truth + ((i % 2) ? 512.0 : -512.0);
    const double avg = f.update(noisy, dt, 2.0);
    if (i > settle) worst = std::max(worst, std::fabs(avg - truth));
  }
  CHECK(worst < 20.0);   // 512 frames of swing in, under 20 out
}

// It must still follow a real change, or it would filter out the very drift it exists to correct.
void test_the_filter_still_follows_a_real_change() {
  LeadFilter f;
  const double dt = 256.0 / 44100.0;
  for (int i = 0; i < 2000; ++i) f.update(96000.0, dt, 2.0);
  CHECK_NEAR(f.avg, 96000.0, 1.0);
  // Step the truth and give it four time constants.
  for (int i = 0; i < static_cast<int>(4 * 2.0 / dt); ++i) f.update(94000.0, dt, 2.0);
  CHECK_NEAR(f.avg, 94000.0, 100.0);
}

void test_the_first_measurement_is_not_averaged_with_nothing() {
  LeadFilter f;
  // Starting from zero would spend the first seconds insisting the stream was 96000 frames out.
  CHECK_EQ(f.update(96000.0, 0.006, 2.0), 96000.0);
  f.reset();
  CHECK(!f.primed);
}

void test_the_trim_pushes_the_right_way_and_is_bounded() {
  const double rate = 96000.0, tau = 5.0, lim = 0.002;
  // Running behind (lead short of target) means produce more output: ratio up.
  CHECK(asrc_trim(95000.0, 96000.0, rate, tau, lim) > 1.0);
  // Running ahead means produce less.
  CHECK(asrc_trim(97000.0, 96000.0, rate, tau, lim) < 1.0);
  // On target, nothing.
  CHECK_EQ(asrc_trim(96000.0, 96000.0, rate, tau, lim), 1.0);
  // However far out, the correction stays within its authority — no step, ever.
  CHECK_EQ(asrc_trim(0.0, 96000.0, rate, tau, lim), 1.0 + lim);
  CHECK_EQ(asrc_trim(1e9, 96000.0, rate, tau, lim), 1.0 - lim);
}

// The pair together: a sender whose crystal is 200 ppm fast must be held, not merely slowed.
void test_the_loop_holds_a_drifting_sender() {
  LeadFilter f;
  const double rate = 96000.0, target = 96000.0, dt = 256.0 / 44100.0;
  double lead = target;
  const double skew = 200e-6;  // the sender runs this much fast

  for (int i = 0; i < 40000; ++i) {  // about four minutes of packets
    const double avg = f.update(lead, dt, 2.0);
    const double trim = asrc_trim(avg, target, rate, 5.0, 0.002);
    // Output advances at the trimmed ratio; the reader advances at the card's rate. The
    // difference is what moves the lead.
    lead += (trim * (1.0 + skew) - 1.0) * rate * dt;
  }
  // Bounded, and settled near target — a proportional loop keeps a small standing offset, which
  // is fine: what matters is that it stopped walking.
  CHECK(std::fabs(lead - target) < rate * 0.05);   // within 50 ms, not drifting away
}

}  // namespace

int main() {
  test_writes_land_at_their_absolute_index();
  test_out_of_order_arrival_still_lands_correctly();
  test_late_and_out_of_range_are_refused();
  test_gaps_read_as_silence_and_do_not_repeat();
  test_live_read_does_not_consume_what_the_trailing_read_needs();
  test_write_and_read_wrap_the_ring();
  test_write_end_tracks_the_far_edge();
  test_the_filter_removes_measurement_noise();
  test_the_filter_still_follows_a_real_change();
  test_the_first_measurement_is_not_averaged_with_nothing();
  test_the_trim_pushes_the_right_way_and_is_bounded();
  test_the_loop_holds_a_drifting_sender();
  return report("net_timeline");
}
