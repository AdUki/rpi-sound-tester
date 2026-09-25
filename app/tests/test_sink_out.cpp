#include "sink_out.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "channel_layout.h"
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
constexpr unsigned kStereo = 2;
}

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
    CHECK(sink_pull(ring, &r_n, 777, out.data()) == SinkPull::Ok);
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
  CHECK(sink_pull(ring, &r_n, 512, out.data()) == SinkPull::Starved);
  CHECK_EQ(r_n, 700u);

  // Beyond the head altogether.
  uint64_t far = 5000;
  CHECK(sink_pull(ring, &far, 16, out.data()) == SinkPull::Starved);
  CHECK_EQ(far, 5000u);

  write_ramp(ring, 1024, 1024);
  CHECK(sink_pull(ring, &r_n, 512, out.data()) == SinkPull::Ok);
  CHECK_EQ(out[0], 700.0f);
}

// A reader that fell a whole ring behind must be told so, not handed newer audio as if it were
// the frames it asked for.
void test_pull_lapped_is_reported() {
  RingBuffer ring(1u << 12, 2, 256);
  write_ramp(ring, 3 * 4096, 1024);
  std::vector<float> out(2 * 256);
  uint64_t r_n = 0;
  CHECK(sink_pull(ring, &r_n, 256, out.data()) == SinkPull::Lapped);
  CHECK_EQ(r_n, 0u);
}

// A model of a sink's loop: each pass pulls a fixed chunk, converts it at the trimmed ratio and
// hands it to a driver that drains at its own, slightly wrong, rate. The card runs slightly wrong
// too. The servo has to hold the latency anyway, and do it by slewing the ratio, never stepping.
struct LoopModel {
  explicit LoopModel(double engine_rate)
      : rate(engine_rate), chunk_in(engine_rate * kSinkPeriodMs / 1000) {}

  double rate;                // engine nominal
  double dev_rate = 48000.0;  // device nominal
  double engine_ppm = 0.0;
  double device_ppm = 0.0;
  double chunk_in;            // one device period, in engine frames
  double buffer = 3840.0;     // PCM buffer, device frames
  double ring_lag = static_cast<double>(kSinkRingLagPeriods) * kTestPeriod;

  double target() const { return ring_lag + buffer * rate / dev_rate; }
};

struct LoopResult {
  double final_error = 0.0;  // frames, filtered
  double worst_trim_step = 0.0;
  double worst_trim_dev = 0.0;
  bool ever_adrift = false;
};

LoopResult run_loop(const LoopModel& m, double initial_error, double seconds) {
  SinkServo servo;
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
    r.ever_adrift |= servo.adrift(target, kSinkResyncS * m.rate);
    prev_trim = servo.trim;
  }
  r.final_error = servo.filter.avg - target;
  return r;
}

// The model with the engine at either rate against a 48 kHz device, so that the servo's frame
// counts and gain are the Pi's at 96 kHz and the VIM3L's at 48 kHz. Only the ratio is modelled: no
// converter runs here, and neither does SinkOutput itself, whose anchor() and stream() need a PCM.
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
  SinkServo s;
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
  std::vector<float> in_all(kFrames * st::channels().total());
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
    std::vector<float> outh(kFrames * kMaxSinkWidth, 99.0f);
    std::vector<float> outl(kFrames * kStereo, 99.0f);
    const unsigned in_stride = st::channels().total();
    route_output<kOutputs>(oc, n, kFrames, in_all.data(), in_stride, gens, gen, identify_frames,
                           out8.data() + 5);
    route_output<kMaxSinkWidth>(oc, n, kFrames, in_all.data(), in_stride, gens, gen,
                                identify_frames, outh.data() + 1);
    route_output<kStereo>(oc, n, kFrames, in_all.data(), in_stride, gens, gen, identify_frames,
                          outl.data() + 1);

    const float g = c.mute ? 0.0f : db_to_lin(c.gain_db);
    bool same = true, right = true, untouched = true;
    for (size_t i = 0; i < kFrames; ++i) {
      const float a = out8[i * kOutputs + 5];
      const float b = outh[i * kMaxSinkWidth + 1];
      const float l = outl[i * kStereo + 1];
      same &= a == b && a == l;
      untouched &= out8[i * kOutputs + 4] == 99.0f && outh[i * kMaxSinkWidth] == 99.0f &&
                   outh[i * kMaxSinkWidth + 2] == 99.0f && outl[i * kStereo] == 99.0f;

      float want = 0.0f;
      if (c.identify_offset > 0 && static_cast<int64_t>(i) < c.identify_offset) {
        want = gen.identify_sample(identify_frames - (c.identify_offset - i));
      } else if (c.type == SourceType::Gen) {
        want = g * bus[i];
      } else if (c.type == SourceType::Input) {
        want = g * in_all[i * st::channels().total() + c.index];
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
  std::vector<float> ring(kFrames * kMaxSinkWidth);
  for (size_t i = 0; i < kFrames; ++i)
    for (unsigned c = 0; c < kMaxSinkWidth; ++c)
      ring[i * kMaxSinkWidth + c] = static_cast<float>(100 * i + c);

  for (unsigned ch = 1; ch <= kMaxSinkWidth; ++ch) {
    std::vector<float> sel(kFrames * ch, -1.0f);
    sink_select(ring.data(), kFrames, kMaxSinkWidth, ch, sel.data());
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
  pcm_from_float(mono, 3, 1, 2, PcmFormat::S16_LE, reinterpret_cast<uint8_t*>(pcm));
  CHECK_EQ(pcm[0], float_to_s16(0.5f));
  CHECK_EQ(pcm[1], float_to_s16(0.5f));
  CHECK_EQ(pcm[2], float_to_s16(-0.25f));
  CHECK_EQ(pcm[3], float_to_s16(-0.25f));
  CHECK_EQ(pcm[4], 32767);
  CHECK_EQ(pcm[5], 32767);

  std::vector<float> six(2 * 6);
  for (size_t i = 0; i < six.size(); ++i) six[i] = 0.01f * static_cast<float>(i);
  std::vector<int16_t> out(six.size(), 0);
  pcm_from_float(six.data(), 2, 6, 6, PcmFormat::S16_LE, reinterpret_cast<uint8_t*>(out.data()));
  bool through = true;
  for (size_t i = 0; i < six.size(); ++i) through &= out[i] == float_to_s16(six[i]);
  CHECK(through);

  // A one-channel device is one channel wide, not two.
  int16_t one[3] = {};
  pcm_from_float(mono, 3, 1, 1, PcmFormat::S16_LE, reinterpret_cast<uint8_t*>(one));
  CHECK_EQ(one[1], float_to_s16(-0.25f));
}

// Every layout sends each of its speakers to its own PCM slot, and fills the PCM: no slot carries
// two speakers (they would sum), none is left out (a speaker the sink expects would be silent),
// and the converter carries exactly what the PCM holds. Mono is the one exception by design.
void test_layout_tables_are_clean() {
  for (uint8_t i = 0; i < static_cast<uint8_t>(SinkLayout::Count); ++i) {
    const auto l = static_cast<SinkLayout>(i);
    const SinkLayoutInfo& lay = sink_layout_info(l);
    CHECK(lay.speakers >= 1 && lay.speakers <= kMaxSinkWidth);
    CHECK(lay.pcm_channels >= 1 && lay.pcm_channels <= kMaxSinkWidth);
    if (l == SinkLayout::Mono) {
      CHECK_EQ(lay.speakers, 1u);
      CHECK_EQ(lay.pcm_channels, 2u);
      CHECK_EQ(lay.slot[0], 0);
      CHECK_EQ(sink_converted_channels(lay), 1u);
      continue;
    }
    CHECK_EQ(lay.speakers, lay.pcm_channels);
    CHECK_EQ(sink_converted_channels(lay), lay.pcm_channels);
    bool used[kMaxSinkWidth] = {};
    for (unsigned s = 0; s < lay.speakers; ++s) {
      CHECK(lay.slot[s] < lay.pcm_channels);
      if (lay.slot[s] < kMaxSinkWidth) {
        CHECK(!used[lay.slot[s]]);
        used[lay.slot[s]] = true;
      }
    }
    // L and R are the first two slots in every layout: stereo content stays where it belongs.
    CHECK_EQ(lay.slot[kSpkL], 0);
    if (lay.pcm_channels >= 2) CHECK_EQ(lay.slot[kSpkR], 1);
    // Only HDMI's layouts reorder: any other device's channels are its own.
    if (!lay.hdmi && l != SinkLayout::Stereo)
      for (unsigned c = 0; c < lay.speakers; ++c) CHECK_EQ(lay.slot[c], c);
    // The name round-trips.
    SinkLayout back = SinkLayout::Mono;
    CHECK(parse_sink_layout(lay.name, &back));
    CHECK_EQ(back, l);
  }
  SinkLayout x;
  CHECK(!parse_sink_layout("5", &x));
  CHECK(!parse_sink_layout("quad", &x));
  CHECK_EQ(std::string(sink_speaker_name(SinkLayout::Mono, 0)), std::string("M"));
  CHECK_EQ(std::string(sink_speaker_name(SinkLayout::S71, kSpkLb)), std::string("Lb"));
}

// The slots are HDMI's order (CEA-861: FL FR LFE FC RL RR RLC RRC), which the firmware passes to
// the sink as they are. ALSA's order (FL FR RL RR FC LFE) put C on a soundbar's surround left and
// the surround pair on its subwoofer and centre.
void test_surround_slots_are_hdmi_order() {
  const SinkLayoutInfo& s51 = sink_layout_info(SinkLayout::S51);
  CHECK_EQ(s51.slot[kSpkL], 0);
  CHECK_EQ(s51.slot[kSpkR], 1);
  CHECK_EQ(s51.slot[kSpkLfe], 2);
  CHECK_EQ(s51.slot[kSpkC], 3);
  CHECK_EQ(s51.slot[kSpkLs], 4);
  CHECK_EQ(s51.slot[kSpkRs], 5);
  const SinkLayoutInfo& s71 = sink_layout_info(SinkLayout::S71);
  for (unsigned sp = 0; sp < 6; ++sp) CHECK_EQ(s71.slot[sp], s51.slot[sp]);
  CHECK_EQ(s71.slot[kSpkLb], 6);
  CHECK_EQ(s71.slot[kSpkRb], 7);
}

// The Pi carries more than two HDMI channels only up to 48 kHz.
void test_surround_is_refused_above_48k() {
  CHECK(surround_rate_ok(SinkLayout::Stereo, 96000));
  CHECK(surround_rate_ok(SinkLayout::Mono, 96000));
  CHECK(surround_rate_ok(SinkLayout::S51, 48000));
  CHECK(surround_rate_ok(SinkLayout::S71, 44100));
  CHECK(!surround_rate_ok(SinkLayout::S51, 96000));
  CHECK(!surround_rate_ok(SinkLayout::S71, 96000));
}

// The audio thread and a sink's thread both index the slot table with the stored layout, so a
// value outside the table must read back as something that fits rather than off its end.
void test_layout_is_always_usable() {
  SinkControl h;
  CHECK_EQ(sink_layout(h), kSinkLayoutDefault);
  h.layout.store(200);
  CHECK_EQ(sink_layout(h), kSinkLayoutDefault);
  h.layout.store(static_cast<uint8_t>(SinkLayout::S71));
  CHECK_EQ(sink_layout(h), SinkLayout::S71);
  // Stereo puts L and R in slots 0 and 1 of its two.
  const SinkLayoutInfo& lay = sink_layout_info(sink_layout(SinkControl{}));
  CHECK_EQ(lay.speakers, kStereo);
  CHECK_EQ(lay.pcm_channels, kStereo);
  CHECK_EQ(lay.slot[kSpkL], 0);
  CHECK_EQ(lay.slot[kSpkR], 1);
}

// A two-wide ring is read through as it is.
void test_select_at_stereo_width() {
  const float ring[] = {1, 2, 3, 4, 5, 6};
  float sel[6] = {};
  sink_select(ring, 3, kStereo, kStereo, sel);
  CHECK(std::equal(ring, ring + 6, sel));
}

// A slot starts with what its device offers: stereo and 48 kHz where it has them, else its first.
void test_device_defaults() {
  SinkDevice usb;
  usb.layouts = {SinkLayout::Stereo};
  usb.rates = {44100, 48000};
  CHECK_EQ(usb.default_layout(), SinkLayout::Stereo);
  CHECK_EQ(usb.default_rate(), 48000u);
  CHECK(usb.offers(SinkLayout::Stereo) && !usb.offers(SinkLayout::S51));
  CHECK(usb.offers_rate(44100) && !usb.offers_rate(96000));

  SinkDevice odd;
  odd.layouts = {SinkLayout::Ch1};
  odd.rates = {96000};
  CHECK_EQ(odd.default_layout(), SinkLayout::Ch1);
  CHECK_EQ(odd.default_rate(), 96000u);
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
  test_select_at_stereo_width();
  test_device_defaults();
  return report("sink_out");
}
