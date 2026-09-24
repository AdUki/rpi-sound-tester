#include "soc_out.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "board_profile.h"
#include "check.h"
#include "constants.h"
#include "control.h"
#include "generators.h"
#include "output_route.h"
#include "rates.h"
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
// line up against the writer's blocks: that is what keeps each sink on the device's one sample
// axis.
void test_pull_tracks_absolute_index() {
  RingBuffer ring(1u << 12, 2, 256);
  write_ramp(ring, 3000, 1024);

  uint64_t r_n = 100;
  std::vector<float> out(2 * 777);
  for (int pass = 0; pass < 3; ++pass) {
    const uint64_t at = r_n;
    CHECK(soc_pull(ring, &r_n, 777, out.data()) == SocPull::Ok);
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
  CHECK(soc_pull(ring, &r_n, 512, out.data()) == SocPull::Starved);
  CHECK_EQ(r_n, 700u);

  // Beyond the head altogether.
  uint64_t far = 5000;
  CHECK(soc_pull(ring, &far, 16, out.data()) == SocPull::Starved);
  CHECK_EQ(far, 5000u);

  write_ramp(ring, 1024, 1024);
  CHECK(soc_pull(ring, &r_n, 512, out.data()) == SocPull::Ok);
  CHECK_EQ(out[0], 700.0f);
}

// A reader that fell a whole ring behind must be told so, not handed newer audio as if it were
// the frames it asked for.
void test_pull_lapped_is_reported() {
  RingBuffer ring(1u << 12, 2, 256);
  write_ramp(ring, 3 * 4096, 1024);
  std::vector<float> out(2 * 256);
  uint64_t r_n = 0;
  CHECK(soc_pull(ring, &r_n, 256, out.data()) == SocPull::Lapped);
  CHECK_EQ(r_n, 0u);
}

// A model of a sink's loop: each pass pulls a fixed chunk, converts it at the trimmed ratio and
// hands it to a driver that drains at its own, slightly wrong, rate. The card runs slightly wrong
// too. The servo has to hold the latency anyway, and do it by slewing the ratio, never stepping.
struct LoopModel {
  explicit LoopModel(double engine_rate)
      : rate(engine_rate), chunk_in(engine_rate * kSocPeriodMs / 1000) {}

  double rate;                // engine nominal
  double dev_rate = 48000.0;  // device nominal
  double engine_ppm = 0.0;
  double device_ppm = 0.0;
  double chunk_in;            // one device period, in engine frames
  double buffer = 3840.0;     // PCM buffer, device frames
  double ring_lag = static_cast<double>(kSocRingLagPeriods) * kTestPeriod;

  double target() const { return ring_lag + buffer * rate / dev_rate; }
};

struct LoopResult {
  double final_error = 0.0;  // frames, filtered
  double worst_trim_step = 0.0;
  double worst_trim_dev = 0.0;
  bool ever_adrift = false;
};

LoopResult run_loop(const LoopModel& m, double initial_error, double seconds) {
  SocServo servo;
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
    r.ever_adrift |= servo.adrift(target, kSocResyncS * m.rate);
    prev_trim = servo.trim;
  }
  r.final_error = servo.filter.avg - target;
  return r;
}

// The model with the engine at either rate against a 48 kHz device, so that the servo's frame
// counts and gain are the Pi's at 96 kHz and the VIM3L's at 48 kHz. Only the ratio is modelled: no
// converter runs here, and neither does SocOutput itself, whose anchor() and stream() need a PCM.
void test_servo_holds_latency_against_drift(double rate) {
  // Slews, never steps: from one pass to the next the filtered latency moves by a few frames of
  // the measurement's jitter, and the trim follows it at 1 / (rate x tau) per frame. The jitter is
  // a quarter of a 1024-frame block at either rate, so the bound is in frames too: 4.8 of them is
  // the 1e-5 of trim this was first held to at 96 kHz. At 48 kHz the same frames are twice as
  // long, and the trim moves twice as far for them.
  const double max_step = 4.8 / (rate * kAsrcTauS);
  for (double ppm : {-100.0, 0.0, 100.0}) {
    LoopModel m(rate);
    m.device_ppm = ppm;
    m.engine_ppm = -ppm / 2;
    const LoopResult r = run_loop(m, 0.0, 120.0);
    // A proportional loop settles a little off target, by drift x rate x tau: constant, and here
    // well under a millisecond.
    const double expected = std::fabs(ppm * 1.5e-6) * m.rate * kAsrcTauS;
    CHECK(std::fabs(r.final_error) <= expected + 0.001 * m.rate);
    CHECK(r.worst_trim_dev <= kAsrcTrimMax + 1e-12);
    CHECK(r.worst_trim_step < max_step);
    CHECK(!r.ever_adrift);
  }
}

// Starting 30 ms off — what a re-anchor can leave behind — it walks back without a resync.
void test_servo_walks_back_an_offset(double rate) {
  LoopModel m(rate);
  m.device_ppm = 50.0;
  const LoopResult r = run_loop(m, 0.030 * m.rate, 90.0);
  CHECK(std::fabs(r.final_error) <= 0.001 * m.rate);
  CHECK(r.worst_trim_dev <= kAsrcTrimMax + 1e-12);
  CHECK(r.worst_trim_step < 1e-4);
  CHECK(!r.ever_adrift);
}

void test_servo_adrift() {
  SocServo s;
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

// The DACs, HDMI and the line out are rendered by the same helper at three strides. Whatever an
// OutputControl says, all must come out sample-for-sample identical: a source, a gain, a mute,
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
    std::vector<float> outh(kFrames * kHdmiMaxChannels, 99.0f);
    std::vector<float> outl(kFrames * kLineoutChannels, 99.0f);
    route_output<kOutputs>(oc, n, kFrames, in_all.data(), gens, gen, identify_frames,
                           out8.data() + 5);
    route_output<kHdmiMaxChannels>(oc, n, kFrames, in_all.data(), gens, gen, identify_frames,
                                   outh.data() + 1);
    route_output<kLineoutChannels>(oc, n, kFrames, in_all.data(), gens, gen, identify_frames,
                                   outl.data() + 1);

    const float g = c.mute ? 0.0f : db_to_lin(c.gain_db);
    bool same = true, right = true, untouched = true;
    for (size_t i = 0; i < kFrames; ++i) {
      const float a = out8[i * kOutputs + 5];
      const float b = outh[i * kHdmiMaxChannels + 1];
      const float l = outl[i * kLineoutChannels + 1];
      same &= a == b && a == l;
      untouched &= out8[i * kOutputs + 4] == 99.0f && outh[i * kHdmiMaxChannels] == 99.0f &&
                   outh[i * kHdmiMaxChannels + 2] == 99.0f && outl[i * kLineoutChannels] == 99.0f;

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

// The converter only ever sees the channels in play, in order, whatever the ring's width.
void test_select_keeps_the_channels_in_play() {
  constexpr size_t kFrames = 5;
  std::vector<float> ring(kFrames * kHdmiMaxChannels);
  for (size_t i = 0; i < kFrames; ++i)
    for (unsigned c = 0; c < kHdmiMaxChannels; ++c)
      ring[i * kHdmiMaxChannels + c] = static_cast<float>(100 * i + c);

  for (unsigned ch = 1; ch <= kHdmiMaxChannels; ++ch) {
    std::vector<float> sel(kFrames * ch, -1.0f);
    soc_select(ring.data(), kFrames, kHdmiMaxChannels, ch, sel.data());
    bool exact = true;
    for (size_t i = 0; i < kFrames; ++i)
      for (unsigned c = 0; c < ch; ++c) exact &= sel[i * ch + c] == static_cast<float>(100 * i + c);
    CHECK(exact);
  }
}

// Mono is one channel on both L and R; every other width is written through as it is.
void test_s16_layout() {
  const float mono[] = {0.5f, -0.25f, 2.0f};  // the last one clips
  int16_t pcm[6] = {};
  soc_to_s16(mono, 3, 1, pcm);
  CHECK_EQ(pcm[0], float_to_s16(0.5f));
  CHECK_EQ(pcm[1], float_to_s16(0.5f));
  CHECK_EQ(pcm[2], float_to_s16(-0.25f));
  CHECK_EQ(pcm[3], float_to_s16(-0.25f));
  CHECK_EQ(pcm[4], 32767);
  CHECK_EQ(pcm[5], 32767);

  std::vector<float> six(2 * 6);
  for (size_t i = 0; i < six.size(); ++i) six[i] = 0.01f * static_cast<float>(i);
  std::vector<int16_t> out(six.size(), 0);
  soc_to_s16(six.data(), 2, 6, out.data());
  bool through = true;
  for (size_t i = 0; i < six.size(); ++i) through &= out[i] == float_to_s16(six[i]);
  CHECK(through);
}

// Every layout sends each of its speakers to its own PCM slot, and fills the PCM: no slot carries
// two speakers (they would sum), none is left out (a speaker the sink expects would be silent),
// and the converter carries exactly what the PCM holds. Mono is the one exception by design.
void test_layout_tables_are_clean() {
  for (uint8_t i = 0; i < static_cast<uint8_t>(HdmiLayout::Count); ++i) {
    const auto l = static_cast<HdmiLayout>(i);
    const HdmiLayoutInfo& lay = hdmi_layout_info(l);
    CHECK(lay.speakers >= 1 && lay.speakers <= kHdmiMaxChannels);
    CHECK(lay.pcm_channels >= 2 && lay.pcm_channels <= kHdmiMaxChannels);
    if (l == HdmiLayout::Mono) {
      CHECK_EQ(lay.speakers, 1u);
      CHECK_EQ(lay.pcm_channels, 2u);
      CHECK_EQ(lay.slot[0], 0);
      CHECK_EQ(soc_converted_channels(lay), 1u);
      continue;
    }
    CHECK_EQ(lay.speakers, lay.pcm_channels);
    CHECK_EQ(soc_converted_channels(lay), lay.pcm_channels);
    bool used[kHdmiMaxChannels] = {};
    for (unsigned s = 0; s < lay.speakers; ++s) {
      CHECK(lay.slot[s] < lay.pcm_channels);
      if (lay.slot[s] < kHdmiMaxChannels) {
        CHECK(!used[lay.slot[s]]);
        used[lay.slot[s]] = true;
      }
    }
    // L and R are the first two slots in every layout: stereo content stays where it belongs.
    CHECK_EQ(lay.slot[kSpkL], 0);
    CHECK_EQ(lay.slot[kSpkR], 1);
    // The name round-trips.
    HdmiLayout back = HdmiLayout::Mono;
    CHECK(parse_hdmi_layout(lay.name, &back));
    CHECK_EQ(back, l);
  }
  HdmiLayout x;
  CHECK(!parse_hdmi_layout("5", &x));
  CHECK(!parse_hdmi_layout("quad", &x));
  CHECK_EQ(std::string(hdmi_speaker_name(HdmiLayout::Mono, 0)), std::string("M"));
  CHECK_EQ(std::string(hdmi_speaker_name(HdmiLayout::S71, kSpkLb)), std::string("Lb"));
}

// The slots are HDMI's order (CEA-861: FL FR LFE FC RL RR RLC RRC), which the firmware passes to
// the sink as they are. ALSA's order (FL FR RL RR FC LFE) put C on a soundbar's surround left and
// the surround pair on its subwoofer and centre.
void test_surround_slots_are_hdmi_order() {
  const HdmiLayoutInfo& s51 = hdmi_layout_info(HdmiLayout::S51);
  CHECK_EQ(s51.slot[kSpkL], 0);
  CHECK_EQ(s51.slot[kSpkR], 1);
  CHECK_EQ(s51.slot[kSpkLfe], 2);
  CHECK_EQ(s51.slot[kSpkC], 3);
  CHECK_EQ(s51.slot[kSpkLs], 4);
  CHECK_EQ(s51.slot[kSpkRs], 5);
  const HdmiLayoutInfo& s71 = hdmi_layout_info(HdmiLayout::S71);
  for (unsigned sp = 0; sp < 6; ++sp) CHECK_EQ(s71.slot[sp], s51.slot[sp]);
  CHECK_EQ(s71.slot[kSpkLb], 6);
  CHECK_EQ(s71.slot[kSpkRb], 7);
}

// The Pi carries more than two HDMI channels only up to 48 kHz.
void test_surround_is_refused_above_48k() {
  CHECK(hdmi_layout_rate_ok(HdmiLayout::Stereo, 96000));
  CHECK(hdmi_layout_rate_ok(HdmiLayout::Mono, 96000));
  CHECK(hdmi_layout_rate_ok(HdmiLayout::S51, 48000));
  CHECK(hdmi_layout_rate_ok(HdmiLayout::S71, 44100));
  CHECK(!hdmi_layout_rate_ok(HdmiLayout::S51, 96000));
  CHECK(!hdmi_layout_rate_ok(HdmiLayout::S71, 96000));
}

// The audio thread and a sink's thread both index the slot table with the stored layout and write
// its slots into a ring the sink's width, so a value outside the table, or a layout wider than the
// sink, must read back as something that fits rather than off either end.
void test_layout_is_always_usable() {
  SocControl h;
  CHECK_EQ(soc_layout(h, kHdmiMaxChannels), kHdmiLayoutDefault);
  h.layout.store(200);
  CHECK_EQ(soc_layout(h, kHdmiMaxChannels), kHdmiLayoutDefault);
  h.layout.store(static_cast<uint8_t>(HdmiLayout::S71));
  CHECK_EQ(soc_layout(h, kHdmiMaxChannels), HdmiLayout::S71);
  // The line out is two slots wide: surround stored there by anything reads as stereo.
  CHECK_EQ(soc_layout(h, kLineoutChannels), HdmiLayout::Stereo);
  h.layout.store(static_cast<uint8_t>(HdmiLayout::S51));
  CHECK_EQ(soc_layout(h, kLineoutChannels), HdmiLayout::Stereo);
  // And the line out's layout puts L and R in slots 0 and 1 of its two.
  const HdmiLayoutInfo& lay = hdmi_layout_info(soc_layout(SocControl{}, kLineoutChannels));
  CHECK_EQ(lay.speakers, kLineoutChannels);
  CHECK_EQ(lay.pcm_channels, kLineoutChannels);
  CHECK_EQ(lay.slot[kSpkL], 0);
  CHECK_EQ(lay.slot[kSpkR], 1);
}

// A two-wide ring (the line out) is read through as it is.
void test_select_at_the_line_outs_width() {
  const float ring[] = {1, 2, 3, 4, 5, 6};
  float sel[6] = {};
  soc_select(ring, 3, kLineoutChannels, kLineoutChannels, sel);
  CHECK(std::equal(ring, ring + 6, sel));
}

// Each sink says which one it is, so two of them never log or fail under the other's name, and
// its ring is as wide as the most slots it can have. All of it is what the board's profile says,
// and the hint for a missing device names the device the profile opens.
void test_sinks_are_told_apart() {
  const BoardProfile& board = rpi3_octo_profile();
  const SinkProfile* hdmi = board.sink("hdmi");
  const SinkProfile* lineout = board.sink("lineout");
  CHECK(hdmi && lineout);
  if (!hdmi || !lineout) return;

  CHECK_EQ(std::string(kHdmiSink.name), hdmi->id);
  CHECK_EQ(std::string(kLineoutSink.name), lineout->id);
  CHECK_EQ(std::string(kHdmiSink.name), std::string("hdmi"));
  CHECK_EQ(std::string(kLineoutSink.name), std::string("lineout"));
  CHECK_EQ(kHdmiSink.width, hdmi->width);
  CHECK_EQ(kLineoutSink.width, lineout->width);
  // The engine renders each sink at a width fixed when it is compiled.
  CHECK_EQ(kHdmiSink.width, kHdmiMaxChannels);
  CHECK_EQ(kLineoutSink.width, kLineoutChannels);
  CHECK_EQ(std::string(kHdmiSink.where), hdmi->where_hint);
  CHECK_EQ(std::string(kLineoutSink.where), lineout->where_hint);
  CHECK(hdmi->where_hint.find(hdmi->device) != std::string::npos);
  CHECK(lineout->where_hint.find(lineout->device) != std::string::npos);
}

}  // namespace

int main() {
  test_pull_tracks_absolute_index();
  test_pull_starved_leaves_the_reader_alone();
  test_pull_lapped_is_reported();
  for (const unsigned r : kTestRates) {
    std::printf("  at %u Hz\n", r);
    test_servo_holds_latency_against_drift(r);
    test_servo_walks_back_an_offset(r);
  }
  test_servo_adrift();
  test_route_identical_across_strides();
  test_select_keeps_the_channels_in_play();
  test_s16_layout();
  test_layout_tables_are_clean();
  test_surround_slots_are_hdmi_order();
  test_surround_is_refused_above_48k();
  test_layout_is_always_usable();
  test_select_at_the_line_outs_width();
  test_sinks_are_told_apart();
  return report("soc_out");
}
