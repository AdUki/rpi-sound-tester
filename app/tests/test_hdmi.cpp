#include "hdmi_out.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "check.h"
#include "constants.h"
#include "control.h"
#include "generators.h"
#include "output_route.h"
#include "ring_buffer.h"
#include "util/dsp.h"

using namespace st;

namespace {

// Writes a stereo ramp whose samples name their own index: L = n, R = -n. Exact in float up to
// 2^24, which is far more than these tests use.
void write_ramp(RingBuffer& ring, size_t frames, size_t block) {
  std::vector<float> buf(block * 2);
  for (size_t done = 0; done < frames; done += block) {
    const uint64_t n = ring.counter();
    const size_t len = std::min(block, frames - done);
    for (size_t i = 0; i < len; ++i) {
      buf[2 * i] = static_cast<float>(n + i);
      buf[2 * i + 1] = -static_cast<float>(n + i);
    }
    ring.write(buf.data(), len);
  }
}

// The reader asks for absolute indices and gets exactly those frames back, however the chunks
// line up against the writer's blocks: that is what keeps HDMI on the device's one sample axis.
void test_pull_tracks_absolute_index() {
  RingBuffer ring(1u << 12, 2, 256);
  write_ramp(ring, 3000, 1024);

  uint64_t r_n = 100;
  std::vector<float> out(2 * 777);
  for (int pass = 0; pass < 3; ++pass) {
    const uint64_t at = r_n;
    CHECK(hdmi_pull(ring, &r_n, 777, out.data()) == HdmiPull::Ok);
    CHECK_EQ(r_n, at + 777);
    bool exact = true;
    for (size_t i = 0; i < 777; ++i) {
      exact &= out[2 * i] == static_cast<float>(at + i);
      exact &= out[2 * i + 1] == -static_cast<float>(at + i);
    }
    CHECK(exact);
  }
}

// Asking past the write head is not an error, just early: nothing moves, and the same pull
// succeeds once the engine has written the block.
void test_pull_starved_leaves_the_reader_alone() {
  RingBuffer ring(1u << 12, 2, 256);
  write_ramp(ring, 1024, 1024);
  std::vector<float> out(2 * 512);

  uint64_t r_n = 700;
  CHECK(hdmi_pull(ring, &r_n, 512, out.data()) == HdmiPull::Starved);
  CHECK_EQ(r_n, 700u);

  // Beyond the head altogether.
  uint64_t far = 5000;
  CHECK(hdmi_pull(ring, &far, 16, out.data()) == HdmiPull::Starved);
  CHECK_EQ(far, 5000u);

  write_ramp(ring, 1024, 1024);
  CHECK(hdmi_pull(ring, &r_n, 512, out.data()) == HdmiPull::Ok);
  CHECK_EQ(out[0], 700.0f);
}

// A reader that fell a whole ring behind must be told so, not handed newer audio as if it were
// the frames it asked for.
void test_pull_lapped_is_reported() {
  RingBuffer ring(1u << 12, 2, 256);
  write_ramp(ring, 3 * 4096, 1024);
  std::vector<float> out(2 * 256);
  uint64_t r_n = 0;
  CHECK(hdmi_pull(ring, &r_n, 256, out.data()) == HdmiPull::Lapped);
  CHECK_EQ(r_n, 0u);
}

// A model of the HDMI loop: each pass pulls a fixed chunk, converts it at the trimmed ratio and
// hands it to a driver that drains at its own, slightly wrong, rate. The card runs slightly wrong
// too. The servo has to hold the latency anyway, and do it by slewing the ratio, never stepping.
struct LoopModel {
  double rate = 96000.0;      // engine nominal
  double dev_rate = 48000.0;  // HDMI nominal
  double engine_ppm = 0.0;
  double device_ppm = 0.0;
  double chunk_in = 1920.0;   // 20 ms of engine frames
  double buffer = 3840.0;     // HDMI buffer, device frames
  double ring_lag = 3072.0;

  double target() const { return ring_lag + buffer * rate / dev_rate; }
};

struct LoopResult {
  double final_error = 0.0;  // frames, filtered
  double worst_trim_step = 0.0;
  double worst_trim_dev = 0.0;
  bool ever_adrift = false;
};

LoopResult run_loop(const LoopModel& m, double initial_error, double seconds) {
  HdmiServo servo;
  const double target = m.target();
  double lag = m.ring_lag + initial_error;  // the driver term is a full buffer at each measurement
  uint64_t prng = 0x1234567887654321ull;
  LoopResult r;
  double prev_trim = 1.0;
  double t = 0.0;
  bool first = true;
  while (t < seconds) {
    const double out = m.chunk_in * (m.dev_rate / m.rate) * servo.trim;
    const double dt = out / (m.dev_rate * (1.0 + m.device_ppm * 1e-6));
    lag += m.rate * (1.0 + m.engine_ppm * 1e-6) * dt - m.chunk_in;
    t += dt;
    // Measurement noise: a quarter of a period of jitter, as the block-granular anchor and the
    // driver's coarse position reporting would give.
    const double noise = 256.0 * xorshift_white(prng);
    const double total = lag + m.buffer * m.rate / m.dev_rate + noise;
    servo.update(total, dt, target, m.rate);
    // The first measurement primes the filter, and the trim moves from free-running to wherever
    // that reading puts it in one go — once per anchor, by design. Every step after it must slew.
    if (!first) r.worst_trim_step = std::max(r.worst_trim_step, std::fabs(servo.trim - prev_trim));
    first = false;
    r.worst_trim_dev = std::max(r.worst_trim_dev, std::fabs(servo.trim - 1.0));
    r.ever_adrift |= servo.adrift(target, kHdmiResyncS * m.rate);
    prev_trim = servo.trim;
  }
  r.final_error = servo.filter.avg - target;
  return r;
}

void test_servo_holds_latency_against_drift() {
  for (double ppm : {-100.0, 0.0, 100.0}) {
    LoopModel m;
    m.device_ppm = ppm;
    m.engine_ppm = -ppm / 2;
    const LoopResult r = run_loop(m, 0.0, 120.0);
    // A proportional loop settles a little off target, by drift x rate x tau: constant, and here
    // well under a millisecond.
    const double expected = std::fabs(ppm * 1.5e-6) * m.rate * kAsrcTauS;
    CHECK(std::fabs(r.final_error) <= expected + 0.001 * m.rate);
    CHECK(r.worst_trim_dev <= kAsrcTrimMax + 1e-12);
    CHECK(r.worst_trim_step < 1e-5);  // slews, never steps
    CHECK(!r.ever_adrift);
  }
}

// Starting 30 ms off — what a re-anchor can leave behind — it walks back without a resync.
void test_servo_walks_back_an_offset() {
  LoopModel m;
  m.device_ppm = 50.0;
  const LoopResult r = run_loop(m, 0.030 * m.rate, 90.0);
  CHECK(std::fabs(r.final_error) <= 0.001 * m.rate);
  CHECK(r.worst_trim_dev <= kAsrcTrimMax + 1e-12);
  CHECK(r.worst_trim_step < 1e-4);
  CHECK(!r.ever_adrift);
}

void test_servo_adrift() {
  HdmiServo s;
  CHECK(!s.adrift(1000.0, 100.0));  // nothing measured yet
  s.update(1050.0, 0.02, 1000.0, 96000.0);
  CHECK(!s.adrift(1000.0, 100.0));
  s.reset();
  s.update(1200.0, 0.02, 1000.0, 96000.0);
  CHECK(s.adrift(1000.0, 100.0));
  s.reset();
  s.update(800.0, 0.02, 1000.0, 96000.0);
  CHECK(s.adrift(1000.0, 100.0));
  CHECK_EQ(s.trim > 1.0, true);  // behind target: consume slower, let the latency grow back
}

// The HDMI pair and the DACs are rendered by the same helper at different strides. Whatever an
// OutputControl says, both must come out sample-for-sample identical: a source, a gain, a mute,
// a passthrough of a network channel, and an Identify burst that begins and ends mid-block.
void test_route_identical_across_strides() {
  constexpr size_t kFrames = 1024;
  constexpr double kRate = 96000.0;
  Generators gen;
  gen.init(kRate);
  const uint64_t identify_frames = static_cast<uint64_t>(kIdentifySeconds * kRate);

  std::vector<float> bus(kFrames);
  for (size_t i = 0; i < kFrames; ++i) bus[i] = 0.5f * std::sin(0.01f * static_cast<float>(i));
  const float* gens[static_cast<size_t>(GenId::Count)] = {bus.data(), bus.data(), bus.data(),
                                                          bus.data()};
  std::vector<float> in_all(kFrames * kTotalInputs);
  for (size_t i = 0; i < in_all.size(); ++i) in_all[i] = 0.001f * static_cast<float>(i % 997);

  struct Case {
    SourceType type;
    uint8_t index;
    float gain_db;
    bool mute;
    int64_t identify_offset;  // identify_until relative to n; <= 0 means none
  };
  const Case cases[] = {
      {SourceType::Gen, static_cast<uint8_t>(GenId::Music), -6.0f, false, 0},
      {SourceType::Input, 7, 0.0f, false, 0},  // a network channel, live
      {SourceType::Input, 2, -20.0f, true, 0},
      {SourceType::Silence, 0, 0.0f, false, 0},
      {SourceType::Gen, static_cast<uint8_t>(GenId::Ping), -3.0f, false, kFrames / 2},
      {SourceType::Silence, 0, 0.0f, false, kFrames / 3},
  };

  const uint64_t n = 5 * 96000;
  for (const Case& c : cases) {
    OutputControl oc;
    oc.source.store(pack_source(c.type, c.index));
    oc.gain_db.store(c.gain_db);
    oc.mute.store(c.mute);
    if (c.identify_offset > 0) oc.identify_until.store(n + c.identify_offset);

    std::vector<float> out8(kFrames * kOutputs, 99.0f);
    std::vector<float> out2(kFrames * kHdmiChannels, 99.0f);
    route_output<kOutputs>(oc, n, kFrames, in_all.data(), gens, gen, identify_frames,
                           out8.data() + 5);
    route_output<kHdmiChannels>(oc, n, kFrames, in_all.data(), gens, gen, identify_frames,
                                out2.data() + 1);

    const float g = c.mute ? 0.0f : db_to_lin(c.gain_db);
    bool same = true, right = true, untouched = true;
    for (size_t i = 0; i < kFrames; ++i) {
      const float a = out8[i * kOutputs + 5];
      const float b = out2[i * kHdmiChannels + 1];
      same &= a == b;
      untouched &= out8[i * kOutputs + 4] == 99.0f && out2[i * kHdmiChannels] == 99.0f;

      float want = 0.0f;
      if (c.identify_offset > 0 && static_cast<int64_t>(i) < c.identify_offset) {
        want = gen.identify_sample(identify_frames - (c.identify_offset - i));
      } else if (c.type == SourceType::Gen) {
        want = g * bus[i];
      } else if (c.type == SourceType::Input) {
        want = g * in_all[i * kTotalInputs + c.index];
      }
      right &= a == want;
    }
    CHECK(same);
    CHECK(right);
    CHECK(untouched);  // each writes its own channel and nothing else
  }
}

}  // namespace

int main() {
  test_pull_tracks_absolute_index();
  test_pull_starved_leaves_the_reader_alone();
  test_pull_lapped_is_reported();
  test_servo_holds_latency_against_drift();
  test_servo_walks_back_an_offset();
  test_servo_adrift();
  test_route_identical_across_strides();
  return report("hdmi");
}
