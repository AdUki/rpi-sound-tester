// Characterisation of the audio engine as it stands: what one block does to the ring, the DAC
// outputs and the SoC sinks' handoff rings, what the simulated card loops back, and what the audio
// thread's loop does around a card that fails. These pin what the engine does today rather than what
// a specification says, so that a change to it (moving it behind the backend interface was the
// first) can be checked against them block for block. Where today's behaviour looks wrong the test
// says so and pins it anyway: changing it should be a decision of its own, not a side effect.
//
// Every case runs at both of the rates in rates.h: the Pi's 96 kHz, and the 48 kHz the VIM3L runs
// the simulator at, so that nothing here is right at 96 kHz only by accident.
#include "audio_engine.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "audio_backend.h"
#include "capture.h"
#include "check.h"
#include "constants.h"
#include "control.h"
#include "fake_backend.h"
#include "generators.h"
#include "hdmi_layout.h"
#include "manual_clock.h"
#include "net_audio.h"
#include "output_route.h"
#include "rates.h"
#include "ring_buffer.h"
#include "soc_out.h"
#include "util/dsp.h"

namespace st {

// The friend of AudioEngine and EngineCore: drives process_block(), or the audio thread's loop
// body, a block at a time with no audio thread, and reads back the state a block leaves behind.
struct EngineTestAccess {
  // What start() does before the audio thread exists.
  static void prepare(AudioEngine& e) { e.prepare(); }
  static void process_block(AudioEngine& e, uint64_t n, size_t frames, float* in_all,
                            float* out8) {
    e.core_.process_block(n, frames, in_all, out8);
  }
  static unsigned cap_delay_frames(const AudioEngine& e) { return e.core_.cap_delay_frames_; }
  static size_t cap_delay_len(const AudioEngine& e) { return e.core_.cap_delay_len_; }
  static const Generators& gen(const AudioEngine& e) { return e.core_.gen_; }
  // The bus the last block rendered for generator `g`.
  static const float* bus(const AudioEngine& e, GenId g) {
    const EngineCore& c = e.core_;
    switch (g) {
      case GenId::Sine: return c.gen_sine_.data();
      case GenId::Noise: return c.gen_noise_.data();
      case GenId::Ping: return c.gen_ping_.data();
      default: return c.gen_music_.data();
    }
  }
  // The output block the last block rendered, kOutputs wide.
  static const float* out8(AudioEngine& e) { return e.core_.out8(); }
  // How many frames the block buffers hold: the longest block the engine can take without
  // overrunning them.
  static size_t block_frames(const AudioEngine& e) {
    const EngineCore& c = e.core_;
    return std::min(c.in_.size() / kTotalInputs, c.out8_.size() / kOutputs);
  }

  // One pass of the audio thread's streaming loop: read, process, write.
  static bool run_block(AudioEngine& e) { return e.run_block(); }
  // The audio thread's retry-forever loop around a card, run on the caller's thread. It returns
  // once the engine is stopped, which the backend's script has to do.
  static void run_card(AudioEngine& e) { e.run_card(); }
  static void set_running(AudioEngine& e, bool on) { e.running_.store(on); }
  static AudioBackend& backend(AudioEngine& e) { return *e.backend_; }
};

}  // namespace st

using namespace st;

namespace {

// The period both boards run, and the one an EngineOptions left alone asks for.
constexpr size_t kPeriod = kTestPeriod;
// What `make run` passes as --sim-stagger, and what CLAUDE.md's 137/274 reading assumes.
constexpr unsigned kStagger = 137;
// The simulated card adds white noise of this amplitude to every input it loops back (run_sim).
constexpr float kSimNoise = 3e-5f;
// The generators schedule their first ping one interval after the first block they render. The
// tests use the shortest interval there is, so that ping is half a second in.
constexpr float kPingIntervalS = kPingIntervalMinS;

// What main() asks of an engine on a board clocked at `rate`: the compiled-in board's clock with
// the rate replaced, as the VIM3L's factory config.json replaces it with 48000.
EngineOptions options_at(unsigned rate) {
  EngineOptions o;
  o.rate = rate;
  return o;
}

// One simulated block on the engine's clock, in whole nanoseconds as the pacing keeps it: 1024
// frames are 10666666.67 ns at 96 kHz and 21333333.33 ns at 48 kHz.
uint64_t sim_block_ns(unsigned rate) {
  return static_cast<uint64_t>(1e9 * static_cast<double>(kPeriod) / static_cast<double>(rate));
}

// The sample that first ping is played at.
uint64_t first_ping(unsigned rate) {
  return static_cast<uint64_t>(std::llround(static_cast<double>(kPingIntervalS) * rate));
}

constexpr size_t kBlockRingFrames = 1u << 16;
constexpr size_t kSinkRingFrames = 1u << 12;  // four blocks: a few blocks overwrite all of it
// 5.5 s. The simulated runs stop well under a second in; the rest is room for a poller that wakes
// late, so that the start of a run is still in the ring when it is read back.
constexpr size_t kSimRingFrames = 1u << 19;
// Only turns a hung audio thread into a failure. Nothing is asserted about how long a run takes.
constexpr auto kSimDeadline = std::chrono::seconds(60);

uint32_t input_src(unsigned c) { return pack_source(SourceType::Input, static_cast<uint8_t>(c)); }
uint32_t gen_src(GenId g) { return pack_source(SourceType::Gen, static_cast<uint8_t>(g)); }

// A sample that names its own index and channel exactly: an integer below 2^24 scaled by a power
// of two. A copy, a delay or a passthrough can then be checked for equality rather than closeness,
// and a frame that lands on the wrong index or channel cannot pass for the right one. Never zero,
// so silence cannot pass for it either. Good for t < 2^20, far more than any test here runs.
float code(uint64_t t, unsigned c) {
  return static_cast<float>((static_cast<uint64_t>(c) << 20) + t + 1) * (1.0f / 16777216.0f);
}

// Whether `ring` holds want(t, c) at every sample of [from, to), on every channel. Names the first
// sample that does not, since a bare false says nothing about how far off it is.
template <class Want>
bool ring_holds(const RingBuffer& ring, uint64_t from, uint64_t to, Want want) {
  const unsigned ch = ring.channels();
  const size_t len = static_cast<size_t>(to - from);
  std::vector<float> b(len * ch);
  if (!ring.read_interleaved(from, len, b.data())) {
    std::printf("  [%llu, %llu) is not in the ring\n", static_cast<unsigned long long>(from),
                static_cast<unsigned long long>(to));
    return false;
  }
  for (uint64_t t = from; t < to; ++t) {
    for (unsigned c = 0; c < ch; ++c) {
      const float got = b[(t - from) * ch + c];
      const float w = want(t, c);
      if (got != w) {
        std::printf("  sample %llu, channel %u: got %.9g, want %.9g\n",
                    static_cast<unsigned long long>(t), c, got, w);
        return false;
      }
    }
  }
  return true;
}

// ---- One block at a time ------------------------------------------------------------------------

// An engine that is never started: process_block() is called by hand, the way run_alsa() and
// run_sim() call it, with n taken from the ring's counter and one period per block. No network
// server is wired in. A second copy of the generators is fed the same blocks, so every bus the
// engine renders has a reference to be compared with.
struct BlockRig {
  Control ctl;
  RingBuffer ring{kBlockRingFrames, kTotalInputs, 2 * kPeriod};
  AudioEngine engine;
  std::vector<float> in = std::vector<float>(kPeriod * kTotalInputs);
  std::vector<float> out = std::vector<float>(kPeriod * kOutputs);

  Generators ref;
  PingLog ref_log;
  std::vector<float> ref_sine = std::vector<float>(kPeriod);
  std::vector<float> ref_noise = std::vector<float>(kPeriod);
  std::vector<float> ref_ping = std::vector<float>(kPeriod);
  std::vector<float> ref_music = std::vector<float>(kPeriod);

  explicit BlockRig(unsigned rate) : engine(ctl, ring, options_at(rate)) {
    EngineTestAccess::prepare(engine);
    ref.init(rate);
  }

  // Runs one block at the ring's head and returns its n. `fill(t, c)` is what the input block
  // holds before the engine sees it, the network channels included: they are never silence here,
  // so the engine is seen to overwrite them. `out` starts full of a value no route produces.
  template <class Fill>
  uint64_t block(Fill fill) {
    const uint64_t n = ring.counter();
    for (size_t i = 0; i < kPeriod; ++i)
      for (unsigned c = 0; c < kTotalInputs; ++c) in[i * kTotalInputs + c] = fill(n + i, c);
    std::fill(out.begin(), out.end(), 99.0f);
    EngineTestAccess::process_block(engine, n, kPeriod, in.data(), out.data());
    // The melody always, whatever the engine skipped: it is a pure function of n.
    ref.render(n, kPeriod, ctl, ref_sine.data(), ref_noise.data(), ref_ping.data(),
               ref_music.data(), ref_log);
    return n;
  }

  uint32_t generation() const { return engine.stats().generation; }
  float in_at(size_t i, unsigned c) const { return in[i * kTotalInputs + c]; }
  float out_at(size_t i, unsigned o) const { return out[i * kOutputs + o]; }
};

// With no network server wired in, the network channels are silence on both axes, whatever the
// buffer held; the ADC channels at unity reach the ring untouched; and each block publishes its own
// first sample as the anchor, with the time it was processed.
void test_block_without_a_net_server(unsigned rate) {
  BlockRig r(rate);
  auto live = [](uint64_t t, unsigned c) { return c < kInputs ? code(t, c) : 0.0f; };
  bool net_silent = true, local_untouched = true;
  for (int b = 0; b < 3; ++b) {
    const uint64_t before = mono_ns();
    const uint64_t n = r.block(code);
    uint64_t an = 0, at = 0;
    CHECK(r.ctl.anchor.read(&an, &at));
    CHECK_EQ(an, n);
    CHECK(at >= before && at <= mono_ns());
    for (size_t i = 0; i < kPeriod; ++i) {
      for (unsigned c = 0; c < kTotalInputs; ++c) {
        const float v = r.in_at(i, c);
        if (c < kInputs) {
          local_untouched &= v == code(n + i, c);
        } else {
          net_silent &= v == 0.0f && !std::signbit(v);
        }
      }
    }
    CHECK(ring_holds(r.ring, n, n + kPeriod, live));
  }
  CHECK(net_silent);
  CHECK(local_untouched);
  CHECK_EQ(r.ring.counter(), 3 * kPeriod);
  CHECK_EQ(r.generation(), 0u);
}

// Input gain is applied in place, upstream of the ring and of every route, and clamped to full
// scale. Unity leaves the samples alone entirely, clamp included, so a value past full scale goes
// through at 0 dB. Mute beats gain; bypass beats both and is unity. -100 dB and below reads as off.
void test_input_gain_mute_bypass_and_clamp(unsigned rate) {
  BlockRig r(rate);
  r.ctl.inputs[1].gain_db.store(6.0f);
  r.ctl.inputs[2].gain_db.store(20.0f);
  r.ctl.inputs[2].mute.store(true);
  // The API only ever sets bypass on a network channel; the audio thread honours it on any.
  r.ctl.inputs[3].gain_db.store(20.0f);
  r.ctl.inputs[3].mute.store(true);
  r.ctl.inputs[3].bypass.store(true);
  r.ctl.inputs[4].gain_db.store(40.0f);
  r.ctl.inputs[5].gain_db.store(-120.0f);
  r.ctl.inputs[kInputs].gain_db.store(20.0f);  // silence without a server, gained or not
  // A routed copy is taken after the gain: one version of the truth.
  r.ctl.outputs[0].source.store(input_src(1));
  r.ctl.outputs[1].source.store(input_src(0));

  // -2 .. +1.94 in steps of 1/16: inside full scale, and past it on both sides.
  auto ramp = [](uint64_t t, unsigned) { return (static_cast<float>(t % 64) - 32.0f) / 16.0f; };
  const uint64_t n = r.block(ramp);

  // Read back from the atomics, as the audio thread does, rather than folded from a literal: the
  // compiler's pow() and the library's may differ in the last bit.
  const float g1 = db_to_lin(r.ctl.inputs[1].gain_db.load());
  const float g4 = db_to_lin(r.ctl.inputs[4].gain_db.load());
  auto want = [&](uint64_t t, unsigned c) {
    const float x = ramp(t, c);
    switch (c) {
      case 0: return x;  // unity
      case 1: return clampf(g1 * x, -1.0f, 1.0f);
      case 3: return x;  // bypassed
      case 4: return clampf(g4 * x, -1.0f, 1.0f);
      default: return 0.0f;  // muted, -120 dB, and the network channels
    }
  };

  bool in_place = true, routed_after_gain = true;
  for (size_t i = 0; i < kPeriod; ++i) {
    for (unsigned c = 0; c < kTotalInputs; ++c) in_place &= r.in_at(i, c) == want(n + i, c);
    routed_after_gain &= r.out_at(i, 0) == want(n + i, 1) && r.out_at(i, 1) == want(n + i, 0);
  }
  CHECK(in_place);
  CHECK(routed_after_gain);
  CHECK(ring_holds(r.ring, n, n + kPeriod, want));
  CHECK_EQ(r.generation(), 0u);
}

// A change of ctl.net.delay_frames moves the capture axis. The delay line restarts from silence,
// the generation counts the move exactly once, and from then on the ring's ADC channels are the
// live ones that many frames late, while the outputs keep playing the live block. Gain is applied
// on the live axis, so a change of it reaches the ring a delay later than it reaches an output.
void test_capture_delay_realigns_the_ring(unsigned rate) {
  BlockRig r(rate);
  r.ctl.outputs[0].source.store(input_src(2));
  r.ctl.outputs[1].source.store(input_src(0));
  auto live = [](uint64_t t, unsigned c) { return c < kInputs ? code(t, c) : 0.0f; };

  uint64_t n = r.block(code);
  CHECK_EQ(r.generation(), 0u);
  CHECK_EQ(EngineTestAccess::cap_delay_frames(r.engine), 0u);
  CHECK(ring_holds(r.ring, n, n + kPeriod, live));

  // Longer than a block: the usual case, since the console's field moves in 50 ms steps.
  constexpr unsigned kDelay = 3000;
  r.ctl.net.delay_frames.store(kDelay);
  const uint64_t moved = r.ring.counter();
  bool outputs_live = true;
  for (int b = 0; b < 6; ++b) {
    n = r.block(code);
    for (size_t i = 0; i < kPeriod; ++i)
      outputs_live &= r.out_at(i, 0) == code(n + i, 2) && r.in_at(i, 2) == code(n + i, 2);
  }
  CHECK(outputs_live);
  CHECK_EQ(r.generation(), 1u);  // once, however many blocks follow
  CHECK_EQ(EngineTestAccess::cap_delay_frames(r.engine), kDelay);
  auto delayed = [&](uint64_t t, unsigned c) {
    return c < kInputs && t >= moved + kDelay ? code(t - kDelay, c) : 0.0f;
  };
  CHECK(ring_holds(r.ring, moved, r.ring.counter(), delayed));

  r.ctl.inputs[0].gain_db.store(6.0f);
  const float g = db_to_lin(r.ctl.inputs[0].gain_db.load());
  const uint64_t gained = r.ring.counter();
  bool gain_live = true;
  for (int b = 0; b < 4; ++b) {
    n = r.block(code);
    for (size_t i = 0; i < kPeriod; ++i)
      gain_live &= r.out_at(i, 1) == clampf(g * code(n + i, 0), -1.0f, 1.0f);
  }
  CHECK(gain_live);
  auto delayed_gain = [&](uint64_t t, unsigned c) {
    const float v = delayed(t, c);
    return c == 0 && t >= gained + kDelay ? clampf(g * v, -1.0f, 1.0f) : v;
  };
  CHECK(ring_holds(r.ring, gained, r.ring.counter(), delayed_gain));
  r.ctl.inputs[0].gain_db.store(0.0f);

  // Storing the delay already in force is not a change.
  r.ctl.net.delay_frames.store(kDelay);
  r.block(code);
  CHECK_EQ(r.generation(), 1u);

  // From one delay to another the line is not just read at the new offset, though it holds what
  // that needs: it starts again from silence, as it does from no delay at all.
  constexpr unsigned kShorter = 2000;
  r.ctl.net.delay_frames.store(kShorter);
  const uint64_t moved_again = r.ring.counter();
  for (int b = 0; b < 4; ++b) r.block(code);
  CHECK_EQ(r.generation(), 2u);
  auto redelayed = [&](uint64_t t, unsigned c) {
    return c < kInputs && t >= moved_again + kShorter ? code(t - kShorter, c) : 0.0f;
  };
  CHECK(ring_holds(r.ring, moved_again, r.ring.counter(), redelayed));

  // Past what the line holds: clamped to its longest, silently, and still a move. The API cannot
  // ask for this much; the line has room for kNetDelayMaxMs.
  const size_t line = EngineTestAccess::cap_delay_len(r.engine);
  CHECK(line - 1 >= static_cast<size_t>(kNetDelayMaxMs * static_cast<double>(rate) / 1000));
  r.ctl.net.delay_frames.store(1u << 30);
  r.block(code);
  CHECK_EQ(r.generation(), 3u);
  CHECK_EQ(EngineTestAccess::cap_delay_frames(r.engine), line - 1);

  // Back to zero: the ring is the live block again at once, and that is a move of the axis too.
  r.ctl.net.delay_frames.store(0);
  n = r.block(code);
  CHECK_EQ(r.generation(), 4u);
  CHECK(ring_holds(r.ring, n, n + kPeriod, live));
}

// A delay shorter than a block is where apply_capture_delay() does not do what its comment says.
// It reads the whole block's delayed frames out before writing any of the live ones in, so frame i
// of a block is right only while i < delay; every later frame reads a part of the line this block
// has not written yet, which holds silence for the first lap after a change and the audio of one
// whole line earlier after that. Pinned as it is, because correcting it changes what the ring
// holds. Reachable with network input on and a delay_ms under one period (1..10 ms at 96 kHz),
// from the API or typed into the console's field; its spinner steps in 50 ms.
void test_capture_delay_shorter_than_a_block(unsigned rate) {
  BlockRig r(rate);
  constexpr unsigned kShort = 100;
  r.ctl.net.delay_frames.store(kShort);  // in force from block 0, so the line restarts at 0
  const int64_t line = static_cast<int64_t>(EngineTestAccess::cap_delay_len(r.engine));
  auto today = [line](uint64_t t, unsigned c) {
    if (c >= kInputs) return 0.0f;
    const int64_t src = static_cast<int64_t>(t) - kShort - (t % kPeriod < kShort ? 0 : line);
    return src >= 0 ? code(static_cast<uint64_t>(src), c) : 0.0f;
  };

  for (int b = 0; b < 4; ++b) r.block(code);
  CHECK_EQ(r.generation(), 1u);
  CHECK_EQ(EngineTestAccess::cap_delay_frames(r.engine), kShort);
  CHECK(ring_holds(r.ring, 0, r.ring.counter(), today));

  while (r.ring.counter() < static_cast<uint64_t>(line) + 4 * kPeriod) r.block(code);
  CHECK(ring_holds(r.ring, r.ring.counter() - 4 * kPeriod, r.ring.counter(), today));
  CHECK_EQ(r.generation(), 1u);
}

// A ping is played at its own sample but logged where the ring will show it, a capture delay
// later, which is where the scope's markers and genie/sync look for it.
void test_ping_log_names_where_the_ring_shows_it(unsigned rate) {
  BlockRig r(rate);
  r.ctl.ping.interval_s.store(kPingIntervalS);
  r.ctl.outputs[0].source.store(gen_src(GenId::Ping));
  constexpr unsigned kDelay = 5000;
  r.ctl.net.delay_frames.store(kDelay);

  const uint64_t first = first_ping(rate);
  bool played_live = true;
  float peak = 0.0f;
  while (r.ring.counter() < first + kPeriod) {
    r.block(code);
    for (size_t i = 0; i < kPeriod; ++i) {
      played_live &= r.out_at(i, 0) == r.ref_ping[i];
      peak = std::max(peak, std::fabs(r.out_at(i, 0)));
    }
  }
  CHECK(played_live);
  CHECK(peak > 0.05f);

  const std::vector<PingEvent> ref = r.ref_log.recent();
  const std::vector<PingEvent> log = r.ctl.ping_log.recent();
  CHECK_EQ(ref.size(), 1u);
  CHECK_EQ(log.size(), 1u);
  if (!ref.empty()) CHECK_EQ(ref[0].sample, first);
  if (!log.empty()) {
    CHECK_EQ(log[0].sample, first + kDelay);
    CHECK_EQ(log[0].variant, static_cast<uint8_t>(PingVariant::Tick));
  }
}

// Every DAC output is route_output() of its own OutputControl over the live, gained block and the
// buses the engine rendered at the same n, and those buses are exactly what a second copy of the
// generators renders for the same blocks. Every sample of every output is written, every block.
void test_each_dac_renders_its_route(unsigned rate) {
  BlockRig r(rate);
  r.ctl.ping.interval_s.store(kPingIntervalS);
  OutputControl* o = r.ctl.outputs.data();
  o[0].source.store(input_src(0));
  o[1].source.store(input_src(3));
  o[1].gain_db.store(-6.0f);
  o[2].source.store(input_src(kInputs + 2));  // a network channel: silence without a server
  o[3].source.store(gen_src(GenId::Sine));
  o[3].gain_db.store(-3.0f);
  o[4].source.store(gen_src(GenId::Noise));
  o[4].mute.store(true);
  o[5].source.store(gen_src(GenId::Ping));
  o[6].source.store(gen_src(GenId::Music));
  o[6].gain_db.store(-10.0f);
  o[7].source.store(pack_source(SourceType::Input, 200));  // no such input: silence

  const uint64_t identify_frames = r.engine.identify_frames();
  CHECK_EQ(identify_frames, static_cast<uint64_t>(kIdentifySeconds * rate));
  const Generators& gen = EngineTestAccess::gen(r.engine);
  const float g1 = db_to_lin(o[1].gain_db.load());
  const float g3 = db_to_lin(o[3].gain_db.load());
  const float g6 = db_to_lin(o[6].gain_db.load());

  bool buses_match = true, as_route_output = true, routed = true, identified = false;
  float ping_peak = 0.0f;
  std::vector<float> want(kPeriod * kOutputs);
  while (r.ring.counter() < first_ping(rate) + kPeriod) {
    // An Identify on OUT 8 that ends 300 frames into the third block.
    if (r.ring.counter() == 2 * kPeriod) o[7].identify_until.store(2 * kPeriod + 300);
    const uint64_t n = r.block(code);

    const float* bus[static_cast<size_t>(GenId::Count)];
    for (uint8_t g = 0; g < static_cast<uint8_t>(GenId::Count); ++g)
      bus[g] = EngineTestAccess::bus(r.engine, static_cast<GenId>(g));
    buses_match &= std::equal(r.ref_sine.begin(), r.ref_sine.end(), bus[0]) &&
                   std::equal(r.ref_noise.begin(), r.ref_noise.end(), bus[1]) &&
                   std::equal(r.ref_ping.begin(), r.ref_ping.end(), bus[2]) &&
                   std::equal(r.ref_music.begin(), r.ref_music.end(), bus[3]);

    for (unsigned k = 0; k < kOutputs; ++k) {
      route_output<kOutputs>(r.ctl.outputs[k], n, kPeriod, r.in.data(), bus, gen, identify_frames,
                             want.data() + k);
    }
    as_route_output &= want == r.out;

    const uint64_t id_until = o[7].identify_until.load();
    for (size_t i = 0; i < kPeriod; ++i) {
      const uint64_t t = n + i;
      const float id =
          t < id_until ? r.ref.identify_sample(identify_frames - (id_until - t)) : 0.0f;
      routed &= r.out_at(i, 0) == code(t, 0);
      routed &= r.out_at(i, 1) == g1 * code(t, 3);
      routed &= r.out_at(i, 2) == 0.0f;
      routed &= r.out_at(i, 3) == g3 * r.ref_sine[i];
      routed &= r.out_at(i, 4) == 0.0f;
      routed &= r.out_at(i, 5) == r.ref_ping[i];
      routed &= r.out_at(i, 6) == g6 * r.ref_music[i];
      routed &= r.out_at(i, 7) == id;
      identified |= id != 0.0f;
      ping_peak = std::max(ping_peak, std::fabs(r.out_at(i, 5)));
    }
  }
  CHECK(buses_match);
  CHECK(as_route_output);
  CHECK(routed);
  CHECK(identified);        // the burst's tail really was inside the run
  CHECK(ping_peak > 0.05f);  // and so was a ping
}

// Each SoC sink's handoff ring is written every block, whether the sink is on or not, so its
// counter is always the capture ring's. While on, each speaker of the layout is rendered into the
// layout's PCM slot for it and every other slot is silence (mono is not duplicated here; the sink's
// thread does that). While off, the whole block is silence whatever the speakers are routed to.
// The line out plays stereo whatever layout is stored for it.
void test_sink_rings_are_written_every_block(unsigned rate) {
  BlockRig r(rate);
  RingBuffer hdmi(kSinkRingFrames, kHdmiMaxChannels, 256);
  RingBuffer lineout(kSinkRingFrames, kLineoutChannels, 256);
  r.engine.set_hdmi_ring(&hdmi);
  r.engine.set_lineout_ring(&lineout);

  // Every speaker from a source of its own, so a speaker in the wrong slot cannot pass.
  for (unsigned s = 0; s < kInputs; ++s) r.ctl.hdmi_outputs[s].source.store(input_src(s));
  r.ctl.hdmi_outputs[kSpkLb].source.store(gen_src(GenId::Sine));
  r.ctl.hdmi_outputs[kSpkRb].source.store(gen_src(GenId::Noise));
  r.ctl.lineout_outputs[kSpkL].source.store(input_src(4));
  r.ctl.lineout_outputs[kSpkR].source.store(input_src(5));
  r.ctl.lineout.layout.store(static_cast<uint8_t>(HdmiLayout::S51));
  r.ctl.hdmi.enabled.store(true);
  r.ctl.lineout.enabled.store(true);

  // What speaker s plays at sample t of the block just rendered.
  auto speaker = [&r](unsigned s, uint64_t t) {
    if (s < kInputs) return code(t, s);
    return (s == kSpkLb ? r.ref_sine : r.ref_noise)[t % kPeriod];
  };
  auto in_step = [&] {
    return hdmi.counter() == r.ring.counter() && lineout.counter() == r.ring.counter();
  };

  for (uint8_t l = 0; l < static_cast<uint8_t>(HdmiLayout::Count); ++l) {
    r.ctl.hdmi.layout.store(l);
    const HdmiLayoutInfo& lay = hdmi_layout_info(static_cast<HdmiLayout>(l));
    const uint64_t n = r.block(code);
    CHECK(in_step());
    auto hdmi_want = [&](uint64_t t, unsigned slot) {
      for (unsigned s = 0; s < lay.speakers; ++s)
        if (lay.slot[s] == slot) return speaker(s, t);
      return 0.0f;
    };
    auto lineout_want = [](uint64_t t, unsigned slot) { return code(t, 4 + slot); };
    CHECK(ring_holds(hdmi, n, n + kPeriod, hdmi_want));
    CHECK(ring_holds(lineout, n, n + kPeriod, lineout_want));
  }

  // Enough blocks to overwrite everything the loop above left, so silence read back was written.
  r.ctl.hdmi.enabled.store(false);
  r.ctl.lineout.enabled.store(false);
  auto silence = [](uint64_t, unsigned) { return 0.0f; };
  bool stepped = true, silent = true;
  for (size_t b = 0; b < kSinkRingFrames / kPeriod; ++b) {
    const uint64_t n = r.block(code);
    stepped &= in_step();
    silent &= ring_holds(hdmi, n, n + kPeriod, silence);
    silent &= ring_holds(lineout, n, n + kPeriod, silence);
  }
  CHECK(stepped);
  CHECK(silent);
}

// The melody is rendered only while something plays it: a DAC output that routes it, or a speaker
// in the layout of an enabled sink. Otherwise its bus is silence.
void test_music_is_rendered_only_while_played(unsigned rate) {
  BlockRig r(rate);
  RingBuffer hdmi(kSinkRingFrames, kHdmiMaxChannels, 256);
  RingBuffer lineout(kSinkRingFrames, kLineoutChannels, 256);
  r.engine.set_hdmi_ring(&hdmi);
  r.engine.set_lineout_ring(&lineout);
  const uint32_t music = gen_src(GenId::Music);
  const uint32_t silence = pack_source(SourceType::Silence, 0);

  // Rendered, and exactly the tune at this n; the tune is never silent this early in the loop.
  auto rendered = [&r] {
    const float* m = EngineTestAccess::bus(r.engine, GenId::Music);
    return std::equal(r.ref_music.begin(), r.ref_music.end(), m) &&
           std::any_of(m, m + kPeriod, [](float v) { return v != 0.0f; });
  };
  auto skipped = [&r] {
    const float* m = EngineTestAccess::bus(r.engine, GenId::Music);
    return std::all_of(m, m + kPeriod, [](float v) { return v == 0.0f; });
  };

  r.block(code);
  CHECK(skipped());  // nothing routes it

  r.ctl.hdmi_outputs[kSpkL].source.store(music);
  r.block(code);
  CHECK(skipped());  // HDMI is off

  r.ctl.hdmi.enabled.store(true);
  r.block(code);
  CHECK(rendered());  // HDMI L, stereo

  r.ctl.hdmi_outputs[kSpkL].source.store(silence);
  r.ctl.hdmi_outputs[kSpkC].source.store(music);
  r.block(code);
  CHECK(skipped());  // stereo has no centre

  r.ctl.hdmi.layout.store(static_cast<uint8_t>(HdmiLayout::S51));
  const uint64_t n = r.block(code);
  CHECK(rendered());
  const uint8_t c_slot = hdmi_layout_info(HdmiLayout::S51).slot[kSpkC];
  auto centre = [&](uint64_t t, unsigned slot) {
    return slot == c_slot ? r.ref_music[t % kPeriod] : 0.0f;
  };
  CHECK(ring_holds(hdmi, n, n + kPeriod, centre));

  r.ctl.hdmi.enabled.store(false);
  r.ctl.lineout.enabled.store(true);
  r.ctl.lineout_outputs[kSpkR].source.store(music);
  r.block(code);
  CHECK(rendered());  // the line out's R

  r.ctl.lineout.enabled.store(false);
  r.block(code);
  CHECK(skipped());

  r.ctl.outputs[7].source.store(music);
  r.block(code);
  CHECK(rendered());  // OUT 8
}

// ---- The simulated card, through the public API ------------------------------------------------

EngineOptions sim_options(unsigned rate) {
  EngineOptions o = options_at(rate);
  o.sim = true;
  o.sim_stagger = kStagger;
  return o;
}

// The objects main() builds around the engine, wired the same way: the network server exists but
// is not started (network input off), and both SoC sinks hand their rings to the engine before the
// audio thread starts, though neither sink's own thread ever runs here.
struct SimRig {
  Control ctl;
  RingBuffer ring{kSimRingFrames, kTotalInputs, 2 * kPeriod};
  AudioEngine engine;
  NetAudioServer net{ctl, engine.rate(), static_cast<unsigned>(kPeriod), engine.clock()};
  SocOutput hdmi{kHdmiSink, ctl.hdmi, ctl, engine, "", kSocRateDefault};
  SocOutput lineout{kLineoutSink, ctl.lineout, ctl, engine, "", kSocRateDefault};

  explicit SimRig(unsigned rate) : engine(ctl, ring, sim_options(rate)) {
    engine.set_net(&net);
    CHECK(engine.set_hdmi_ring(&hdmi.ring()));
    CHECK(engine.set_lineout_ring(&lineout.ring()));
  }
  // The audio thread writes the sinks' rings, so it has to stop before they go.
  ~SimRig() { engine.stop(); }

  // Runs the simulated card until the ring holds at least `frames`, then stops it.
  bool run_until(uint64_t frames) {
    if (!engine.start()) return false;
    const auto deadline = std::chrono::steady_clock::now() + kSimDeadline;
    while (ring.counter() < frames && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    engine.stop();
    return ring.counter() >= frames;
  }
};

// The public API, as main() drives it on a desktop: every counter the engine publishes moves in
// whole periods and names the same sample on every ring it writes, and stats() reports the
// simulator.
void test_sim_run_keeps_one_sample_axis(unsigned rate) {
  SimRig rig(rate);
  const uint64_t started_ns = mono_ns();
  CHECK(rig.engine.start());

  bool whole = true, forward = true, anchored = true, in_step = true, reported = false;
  uint64_t last = 0;
  // A few observations at least, however far the audio thread got before the first of them.
  unsigned polls = 0;
  const auto deadline = std::chrono::steady_clock::now() + kSimDeadline;
  while ((rig.ring.counter() < 16 * kPeriod || polls < 20) &&
         std::chrono::steady_clock::now() < deadline) {
    ++polls;
    const uint64_t h1 = rig.hdmi.ring().counter();
    const uint64_t l1 = rig.lineout.ring().counter();
    const uint64_t c1 = rig.ring.counter();
    uint64_t an = 0, at = 0;
    const bool have = rig.ctl.anchor.read(&an, &at);
    const uint64_t c2 = rig.ring.counter();
    const uint64_t h2 = rig.hdmi.ring().counter();
    const uint64_t l2 = rig.lineout.ring().counter();
    whole &= c1 % kPeriod == 0 && h1 % kPeriod == 0 && l1 % kPeriod == 0;
    forward &= c1 >= last && c2 >= c1;
    last = c2;
    // Published at the top of a block, and the ring written at its bottom: the anchor names the
    // block just written or the one in flight, never anything else.
    if (have) anchored &= an % kPeriod == 0 && an + kPeriod >= c1 && an <= c2;
    // The sinks' rings are written straight after the capture ring, in the same block, so they
    // trail it by at most the block in flight.
    in_step &= h1 <= c1 && l1 <= c1 && c1 <= h2 + kPeriod && c1 <= l2 + kPeriod;
    if (c1 > 0) reported |= rig.engine.stats().running;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  rig.engine.stop();

  const uint64_t head = rig.ring.counter();
  CHECK(head >= 16 * kPeriod);
  CHECK(whole);
  CHECK(forward);
  CHECK(anchored);
  CHECK(in_step);
  CHECK(reported);
  CHECK_EQ(head % kPeriod, 0u);
  CHECK_EQ(rig.hdmi.ring().counter(), head);
  CHECK_EQ(rig.lineout.ring().counter(), head);

  uint64_t an = 0, at = 0;
  CHECK(rig.ctl.anchor.read(&an, &at));
  CHECK_EQ(an, head - kPeriod);  // the last block's first sample
  CHECK(at >= started_ns && at <= mono_ns());

  // Both sinks are off: their rings carry silence, but they carry it.
  auto silence = [](uint64_t, unsigned) { return 0.0f; };
  CHECK(ring_holds(rig.hdmi.ring(), head - kPeriod, head, silence));
  CHECK(ring_holds(rig.lineout.ring(), head - kPeriod, head, silence));

  const EngineStats s = rig.engine.stats();
  CHECK(!s.running);
  CHECK(s.sim);
  CHECK_EQ(s.device, std::string("simulator"));
  CHECK_EQ(s.rate, rate);
  CHECK_EQ(rig.engine.rate(), static_cast<double>(rate));
  CHECK_EQ(static_cast<size_t>(s.period), kPeriod);
  CHECK_EQ(s.periods, rpi3_octo_profile().clock.periods);
  CHECK_EQ(s.capture_channels, kInputs);
  CHECK_EQ(s.format, std::string("float32 (simulated)"));
  CHECK_EQ(s.xruns, 0u);
  CHECK_EQ(s.generation, 0u);
  CHECK_EQ(s.samples, head);
  CHECK(s.last_error.empty());
  CHECK_EQ(static_cast<size_t>(rig.engine.period()), kPeriod);
}

// The simulated card's loopback: output c comes back on input c period + c*stagger frames later,
// under the simulator's noise floor and nothing else. Checked sample by sample against a second
// copy of the generators, and through the xcorr the console measures with, which must read exactly
// 137 samples from IN 1 to IN 2 and 274 from IN 1 to IN 3.
void test_sim_loops_outputs_back_staggered(unsigned rate) {
  SimRig rig(rate);
  // A tick on each looped-back output, routed as PUT /api/outputs/:ch routes it.
  rig.ctl.ping.interval_s.store(kPingIntervalS);
  for (unsigned c = 0; c < kInputs; ++c) rig.ctl.outputs[c].source.store(gen_src(GenId::Ping));

  constexpr uint64_t kWindow = 8192;
  const uint64_t first = first_ping(rate);
  const uint64_t until = first + kWindow + 2 * kPeriod;
  CHECK(rig.run_until(until));
  const uint64_t head = rig.ring.counter();
  CHECK_EQ(rig.hdmi.ring().counter(), head);
  CHECK_EQ(rig.lineout.ring().counter(), head);

  // What was played: the ping bus of the same generators, fed the same blocks.
  Generators ref;
  ref.init(rate);
  PingLog ref_log;
  std::vector<float> ping(head + kPeriod), sine(kPeriod), noise(kPeriod);
  for (uint64_t n = 0; n < head; n += kPeriod)
    ref.render(n, kPeriod, rig.ctl, sine.data(), noise.data(), ping.data() + n, nullptr, ref_log);

  // Logged at the tick's own sample, with no capture delay in force; a run that went on longer than
  // asked has later ones, one interval apart.
  const std::vector<PingEvent> log = rig.ctl.ping_log.recent();
  CHECK(!log.empty());
  for (size_t k = 0; k < log.size(); ++k) CHECK_EQ(log[k].sample, first * (k + 1));

  std::vector<float> x(until);
  for (unsigned c = 0; c < kInputs; ++c) {
    CHECK(rig.ring.read_channel(0, until, c, x.data()));
    const uint64_t d = kPeriod + c * kStagger;
    float err = 0.0f;
    for (uint64_t t = 0; t < until; ++t)
      err = std::max(err, std::fabs(x[t] - (t >= d ? ping[t - d] : 0.0f)));
    CHECK(err <= kSimNoise + 1e-7f);
    CHECK(err >= 0.9f * kSimNoise);  // and the floor is there: a simulated ADC is never silent
    // A sample early or late is already far outside that floor: the delay is exact.
    for (const uint64_t off : {d - 1, d + 1}) {
      float e = 0.0f;
      for (uint64_t t = first + d; t < first + d + 480; ++t)
        e = std::max(e, std::fabs(x[t] - ping[t - off]));
      CHECK(e > 1e-3f);
    }
  }
  for (unsigned c = kInputs; c < kTotalInputs; ++c) {
    CHECK(rig.ring.read_channel(0, until, c, x.data()));
    CHECK(std::all_of(x.begin(), x.end(), [](float v) { return v == 0.0f; }));
  }

  CaptureStore cap(rig.ring, rate, kPeriod);
  CHECK(cap.freeze(rig.engine.stats().generation).frozen);
  for (unsigned c = 1; c < kInputs; ++c) {
    const XcorrResult r = cap.xcorr(0, c, first, kWindow);
    CHECK(r.ok);
    CHECK_EQ(r.lag_samples, static_cast<int64_t>(c * kStagger));
    CHECK(r.confidence > 3.0);
  }
}

// ---- The audio thread's loop, stepped by hand ---------------------------------------------------

// An engine on a scripted card, with a clock that moves only when told to. It is never started:
// the audio thread's loop is run on the test's own thread, a block or a whole run_card() at a time.
struct FakeRig {
  Control ctl;
  RingBuffer ring{kBlockRingFrames, kTotalInputs, 2 * kPeriod};
  FakeBackend card;
  ManualClock clock;
  AudioEngine engine;

  explicit FakeRig(unsigned rate, unsigned capture_channels = kTdmSlots)
      : card(ctl, capture_channels, kPeriod, rate),
        engine(ctl, ring, options_at(rate), card, clock) {
    EngineTestAccess::prepare(engine);
  }
};

// Through the card's S32 frames: the slot maps pick each ADC's slot out of a captured frame and put
// each DAC into its slot of a played one, and a map entry past the stream's width means the
// channel's own slot. Input gain lands between the two. Every block publishes its first sample as
// the anchor, stamped with the engine's clock as the block ran, and is played at the same n it was
// captured at.
void test_fake_card_remaps_both_ways(unsigned rate) {
  FakeRig r(rate);
  RingBuffer hdmi(kSinkRingFrames, kHdmiMaxChannels, 256);
  RingBuffer lineout(kSinkRingFrames, kLineoutChannels, 256);
  r.engine.set_hdmi_ring(&hdmi);
  r.engine.set_lineout_ring(&lineout);

  const uint8_t imap[kInputs] = {7, 5, 3, 1, 0, 9};
  const unsigned in_slot[kInputs] = {7, 5, 3, 1, 0, 5};  // 9 is past 8 slots: IN 6's own
  const uint8_t omap[kOutputs] = {1, 0, 3, 2, 5, 4, 6, 200};
  const unsigned out_slot[kOutputs] = {1, 0, 3, 2, 5, 4, 6, 7};  // 200 is too: OUT 8's own
  for (unsigned c = 0; c < kInputs; ++c) r.ctl.input_map[c].store(imap[c]);
  for (unsigned o = 0; o < kOutputs; ++o) r.ctl.output_map[o].store(omap[o]);
  // Each DAC plays an input, OUT 7 and 8 doubling up on two of them.
  const unsigned routed[kOutputs] = {0, 1, 2, 3, 4, 5, 2, 4};
  for (unsigned o = 0; o < kOutputs; ++o) r.ctl.outputs[o].source.store(input_src(routed[o]));
  r.ctl.inputs[2].gain_db.store(6.0f);
  const float g2 = db_to_lin(r.ctl.inputs[2].gain_db.load());

  // IN c at sample t. Device frame and ring index are the same thing here: nothing was lost.
  auto in_want = [&](uint64_t t, unsigned c) {
    if (c >= kInputs) return 0.0f;
    const float x = s32_to_float(fake_capture_code(t, in_slot[c]));
    return c == 2 ? clampf(g2 * x, -1.0f, 1.0f) : x;
  };

  CHECK(r.card.open());
  CHECK(r.card.start());
  constexpr size_t kBlocks = 6;
  bool anchored = true, in_step = true;
  for (size_t b = 0; b < kBlocks; ++b) {
    r.clock.advance(sim_block_ns(rate) + 1000 * b * b);  // unevenly, as a real card wakes it
    CHECK(EngineTestAccess::run_block(r.engine));
    uint64_t an = 0, at = 0;
    anchored &= r.ctl.anchor.read(&an, &at) && an == b * kPeriod && at == r.clock.now_ns();
    in_step &= hdmi.counter() == r.ring.counter() && lineout.counter() == r.ring.counter();
  }
  CHECK(anchored);
  CHECK(in_step);
  CHECK_EQ(r.ring.counter(), kBlocks * kPeriod);
  CHECK(ring_holds(r.ring, 0, r.ring.counter(), in_want));

  bool same_n = r.card.read_n.size() == kBlocks && r.card.write_n == r.card.read_n;
  for (size_t b = 0; same_n && b < kBlocks; ++b) same_n &= r.card.read_n[b] == b * kPeriod;
  CHECK(same_n);

  CHECK_EQ(r.card.played.size(), kBlocks * kPeriod * kOutputs);
  bool played = true;
  for (uint64_t t = 0; t < kBlocks * kPeriod; ++t)
    for (unsigned o = 0; o < kOutputs; ++o)
      played &= r.card.played_at(t, out_slot[o]) == float_to_s32(in_want(t, routed[o]));
  CHECK(played);

  const EngineStats s = r.engine.stats();
  CHECK_EQ(s.capture_channels, kTdmSlots);
  CHECK_EQ(s.format, std::string("S32_LE"));
  CHECK_EQ(s.xruns, 0u);
  CHECK_EQ(s.generation, 0u);
}

// A card that captures only the six ADCs' slots, as the Octo's does when its driver will not widen
// capture to 8 TDM slots. The engine reports the width it asked for until the card is open and the
// width it got from then on, and a map entry for slot 7 or 8 means the channel's own slot. Which
// width to fall back to is AlsaLinkedBackend's decision, made between its ALSA calls where no test
// reaches; this pins what the engine does with the answer.
void test_capture_falls_back_to_six_slots(unsigned rate) {
  FakeRig r(rate, kInputs);
  const uint8_t imap[kInputs] = {7, 6, 5, 4, 3, 2};
  const unsigned in_slot[kInputs] = {0, 1, 5, 4, 3, 2};
  for (unsigned c = 0; c < kInputs; ++c) r.ctl.input_map[c].store(imap[c]);
  CHECK_EQ(r.engine.stats().capture_channels, kTdmSlots);

  EngineStats during;
  r.card.on_read = [&r, &during] {
    during = r.engine.stats();
    if (r.card.reads == 3) EngineTestAccess::set_running(r.engine, false);
  };
  EngineTestAccess::set_running(r.engine, true);
  EngineTestAccess::run_card(r.engine);

  CHECK(during.running);
  CHECK_EQ(during.capture_channels, kInputs);
  CHECK_EQ(r.engine.stats().capture_channels, kInputs);
  CHECK(!r.engine.stats().running);
  CHECK_EQ(r.ring.counter(), 3 * kPeriod);
  auto in_want = [&](uint64_t t, unsigned c) {
    return c < kInputs ? s32_to_float(fake_capture_code(t, in_slot[c])) : 0.0f;
  };
  CHECK(ring_holds(r.ring, 0, r.ring.counter(), in_want));
  // Stopped, not failed: closed once, and no wait for a retry.
  CHECK_EQ(r.card.opens, 1u);
  CHECK_EQ(r.card.closes, 1u);
  CHECK(r.clock.sleeps.empty());
}

// An xrun, on a read or on a write, is counted, moves the generation exactly once, and is handed
// to the backend with the error it returned; then the stream goes on. A read that failed captured
// nothing and never reached the ring, so the block after it is published at the same n. A write
// that failed leaves its block in the ring, since it was captured. A failed recover ends the stream.
void test_xrun_moves_the_generation_once(unsigned rate) {
  FakeRig r(rate);
  r.card.xrun_on_read = 3;
  r.card.xrun_on_write = 5;
  CHECK(r.card.open());
  CHECK(r.card.start());

  for (int b = 0; b < 3; ++b) CHECK(EngineTestAccess::run_block(r.engine));
  CHECK_EQ(r.engine.stats().xruns, 1u);
  CHECK_EQ(r.engine.stats().generation, 1u);
  CHECK_EQ(r.ring.counter(), 2 * kPeriod);
  CHECK(r.card.recover_errs == std::vector<int>{-EPIPE});
  CHECK_EQ(r.card.writes, 2u);

  for (int b = 0; b < 3; ++b) CHECK(EngineTestAccess::run_block(r.engine));
  CHECK_EQ(r.engine.stats().xruns, 2u);
  CHECK_EQ(r.engine.stats().generation, 2u);
  CHECK_EQ(r.ring.counter(), 5 * kPeriod);
  CHECK(r.card.recover_errs == (std::vector<int>{-EPIPE, -EPIPE}));
  const std::vector<uint64_t> reads = {0, kPeriod, 2 * kPeriod, 2 * kPeriod, 3 * kPeriod,
                                       4 * kPeriod};
  const std::vector<uint64_t> writes = {0, kPeriod, 2 * kPeriod, 3 * kPeriod, 4 * kPeriod};
  CHECK(r.card.read_n == reads);
  CHECK(r.card.write_n == writes);
  CHECK_EQ(r.card.played.size(), 4 * kPeriod * kOutputs);

  r.card.recover_fails = true;
  r.card.xrun_on_read = r.card.reads + 1;
  CHECK(!EngineTestAccess::run_block(r.engine));
  CHECK_EQ(r.engine.stats().xruns, 3u);
  CHECK_EQ(r.engine.stats().generation, 3u);
  CHECK_EQ(r.ring.counter(), 5 * kPeriod);
}

// The retry-forever loop, with nothing real to wait for. A card that will not open, or opens and
// will not start, is reported through last_error and tried again after 5 s of the engine's clock,
// polled in 100 ms steps; one that started is closed again first. A stream that ends in a failed
// recover is closed and reopened after the same wait, and "stream running" clears the error. Each
// open sizes the block buffers again, which restarts the capture delay line, so with a delay in
// force the first block after a reopen moves the axis once more: a generation bump of its own, on
// top of the xrun's.
void test_card_is_retried_until_it_streams(unsigned rate) {
  FakeRig r(rate);
  r.card.fail_opens = 1;
  r.card.fail_starts = 1;
  r.card.xrun_on_read = 3;
  r.card.recover_fails = true;
  r.ctl.net.delay_frames.store(3000);

  std::vector<EngineStats> during;
  r.card.on_read = [&r, &during] {
    during.push_back(r.engine.stats());
    if (r.card.reads == 6) EngineTestAccess::set_running(r.engine, false);
  };
  EngineTestAccess::set_running(r.engine, true);
  EngineTestAccess::run_card(r.engine);

  CHECK_EQ(r.card.opens, 4u);
  CHECK_EQ(r.card.starts, 3u);
  CHECK_EQ(r.card.closes, 3u);
  CHECK_EQ(r.card.recover_errs.size(), 1u);
  // What /api/state said as each open began: nothing yet, the open that failed, the start that
  // failed, and nothing once a stream had run.
  const std::vector<std::string> errors = {"", "fake: no card", "fake: will not start", ""};
  CHECK(r.card.error_at_open == errors);

  CHECK_EQ(r.clock.sleeps.size(), 3 * 50u);
  bool polled = true;
  for (size_t i = 0; i < r.clock.sleeps.size(); ++i)
    polled &= r.clock.sleeps[i] == ManualClock::kStartNs + (i + 1) * 100000000ull;
  CHECK(polled);

  CHECK_EQ(during.size(), 6u);
  bool streaming = true;
  for (const EngineStats& s : during) streaming &= s.running && s.last_error.empty();
  CHECK(streaming);
  CHECK(!r.engine.stats().running);

  CHECK_EQ(r.engine.stats().xruns, 1u);
  CHECK_EQ(r.engine.stats().generation, 3u);
  CHECK_EQ(r.ring.counter(), 5 * kPeriod);
  const std::vector<uint64_t> reads = {0, kPeriod, 2 * kPeriod, 2 * kPeriod, 3 * kPeriod,
                                       4 * kPeriod};
  CHECK(r.card.read_n == reads);
}

// The simulated card, stepped a block at a time on a clock that moves only when told to. Blocks are
// due one period apart in whole nanoseconds, counted from the start of the stream, and each
// block's anchor is stamped when that block was due. The loopback is exact to the bit: input c at
// sample t is what output c played period + c*stagger samples earlier, plus the simulated ADC's
// noise drawn from the one xorshift sequence, frame by frame and channel by channel, as it always
// has been.
void test_simulator_steps_on_a_manual_clock(unsigned rate) {
  Control ctl;
  RingBuffer ring(kBlockRingFrames, kTotalInputs, 2 * kPeriod);
  RingBuffer hdmi(kSinkRingFrames, kHdmiMaxChannels, 256);
  RingBuffer lineout(kSinkRingFrames, kLineoutChannels, 256);
  ManualClock clock;
  AudioEngine engine(ctl, ring, sim_options(rate), clock);
  CHECK(engine.set_hdmi_ring(&hdmi));
  CHECK(engine.set_lineout_ring(&lineout));
  const uint64_t block_ns = sim_block_ns(rate);

  // Something different on every output that loops back, two of them passing the loopback itself
  // back out.
  ctl.outputs[0].source.store(gen_src(GenId::Sine));
  ctl.outputs[1].source.store(gen_src(GenId::Noise));
  ctl.outputs[2].source.store(gen_src(GenId::Music));
  ctl.outputs[3].source.store(input_src(0));
  ctl.outputs[4].source.store(gen_src(GenId::Sine));
  ctl.outputs[4].gain_db.store(-6.0f);
  ctl.outputs[5].source.store(input_src(3));

  EngineTestAccess::prepare(engine);
  AudioBackend& sim = EngineTestAccess::backend(engine);
  const BackendShape shape = sim.shape();
  CHECK_EQ(shape.rate, rate);
  CHECK_EQ(static_cast<size_t>(shape.period), kPeriod);
  CHECK_EQ(shape.capture_channels, kInputs);
  CHECK_EQ(shape.playback_channels, kOutputs);
  CHECK_EQ(std::string(shape.format), std::string("float32 (simulated)"));
  CHECK(sim.open());
  CHECK(sim.start());

  constexpr size_t kBlocks = 12;
  std::vector<float> played(kBlocks * kPeriod * kOutputs);
  bool anchored = true, in_step = true;
  for (size_t b = 0; b < kBlocks; ++b) {
    CHECK(EngineTestAccess::run_block(engine));
    uint64_t an = 0, at = 0;
    anchored &= ctl.anchor.read(&an, &at) && an == b * kPeriod &&
                at == ManualClock::kStartNs + b * block_ns;
    in_step &= hdmi.counter() == ring.counter() && lineout.counter() == ring.counter();
    const float* out8 = EngineTestAccess::out8(engine);
    std::copy(out8, out8 + kPeriod * kOutputs, played.begin() + b * kPeriod * kOutputs);
  }
  CHECK(anchored);
  CHECK(in_step);
  CHECK_EQ(clock.sleeps.size(), kBlocks);
  bool paced = true;
  for (size_t b = 0; b < clock.sleeps.size(); ++b)
    paced &= clock.sleeps[b] == ManualClock::kStartNs + (b + 1) * block_ns;
  CHECK(paced);

  const uint64_t end = kBlocks * kPeriod;
  CHECK_EQ(ring.counter(), end);
  std::vector<float> x(end * kTotalInputs);
  CHECK(ring.read_interleaved(0, end, x.data()));
  uint64_t seed = 0x9e3779b97f4a7c15ull;
  bool exact = true, net_silent = true;
  for (uint64_t t = 0; t < end; ++t) {
    for (unsigned c = 0; c < kInputs; ++c) {
      const uint64_t d = kPeriod + c * kStagger;
      const float looped = t >= d ? played[(t - d) * kOutputs + c] : 0.0f;
      exact &= x[t * kTotalInputs + c] == looped + kSimNoise * xorshift_white(seed);
    }
    for (unsigned c = kInputs; c < kTotalInputs; ++c) net_silent &= x[t * kTotalInputs + c] == 0.0f;
  }
  CHECK(exact);
  CHECK(net_silent);
  // And what looped back was a signal on every channel, not only the noise.
  for (unsigned c = 0; c < kInputs; ++c) {
    float peak = 0.0f;
    for (uint64_t t = 0; t < end; ++t) peak = std::max(peak, std::fabs(played[t * kOutputs + c]));
    CHECK(peak > 0.01f);
  }
}

// A driver that renegotiates the period when the card opens, as AlsaLinkedBackend's configure()
// lets one: the engine reports the period it asked for until the card is open and the card's from
// then on, and sizes its block buffers again for the card's before the first block. A card that
// settles on a longer period than was asked for is what that second sizing is for: blocks sized
// for the request would overrun them.
void test_the_card_decides_the_period(unsigned rate) {
  for (const unsigned settled : {512u, 2048u}) {
    FakeRig r(rate);
    r.card.settle_period = settled;
    CHECK_EQ(static_cast<size_t>(r.engine.period()), kPeriod);
    CHECK_EQ(static_cast<size_t>(r.engine.stats().period), kPeriod);
    CHECK_EQ(EngineTestAccess::block_frames(r.engine), kPeriod);

    bool reported = true, sized = true, anchored = true;
    r.card.on_read = [&] {
      reported &= r.engine.stats().period == settled && r.engine.period() == settled;
      sized &= EngineTestAccess::block_frames(r.engine) == settled;
      // The block before this one published its first sample.
      uint64_t an = 0, at = 0;
      if (r.card.reads > 1)
        anchored &= r.ctl.anchor.read(&an, &at) && an == (r.card.reads - 2) * uint64_t{settled};
      if (r.card.reads == 4) EngineTestAccess::set_running(r.engine, false);
    };
    EngineTestAccess::set_running(r.engine, true);
    EngineTestAccess::run_card(r.engine);
    CHECK(reported);
    CHECK(sized);
    CHECK(anchored);
    CHECK_EQ(r.card.reads, 4u);
    CHECK_EQ(r.ring.counter(), 4 * uint64_t{settled});
    CHECK_EQ(r.card.played.size(), 4 * size_t{settled} * kOutputs);
    CHECK_EQ(r.engine.stats().period, settled);
    CHECK_EQ(r.engine.period(), settled);
  }
}

// A handoff ring the engine cannot write at the width it renders that sink at is refused rather
// than written: a wider one would have the audio thread copy past the end of its block, a narrower
// one would hand the sink misframed audio. A refused ring is never written, the sink is left out as
// if nothing had been wired in, and the block goes on as before. A ring of the right width is taken
// in its place.
void test_a_sink_ring_of_another_width_is_refused(unsigned rate) {
  BlockRig r(rate);
  r.ctl.hdmi.enabled.store(true);
  r.ctl.lineout.enabled.store(true);
  for (unsigned s = 0; s < kHdmiMaxChannels; ++s) r.ctl.hdmi_outputs[s].source.store(input_src(0));
  for (unsigned s = 0; s < kLineoutChannels; ++s)
    r.ctl.lineout_outputs[s].source.store(input_src(1));

  RingBuffer wide(kSinkRingFrames, kHdmiMaxChannels + 2, 256);
  RingBuffer six(kSinkRingFrames, 6, 256);
  RingBuffer mono(kSinkRingFrames, 1, 256);
  RingBuffer eight(kSinkRingFrames, kHdmiMaxChannels, 256);
  auto live = [](uint64_t t, unsigned c) { return c < kInputs ? code(t, c) : 0.0f; };

  CHECK(!r.engine.set_hdmi_ring(&wide));
  CHECK(!r.engine.set_lineout_ring(&eight));  // HDMI's width, on the line out
  r.block(code);
  CHECK(!r.engine.set_hdmi_ring(&six));
  CHECK(!r.engine.set_lineout_ring(&mono));
  uint64_t n = r.block(code);
  CHECK_EQ(wide.counter(), 0u);
  CHECK_EQ(six.counter(), 0u);
  CHECK_EQ(mono.counter(), 0u);
  CHECK_EQ(eight.counter(), 0u);
  CHECK_EQ(r.ring.counter(), 2 * kPeriod);
  CHECK(ring_holds(r.ring, n, n + kPeriod, live));

  // No ring at all is always taken: the sink is simply not wired in.
  CHECK(r.engine.set_hdmi_ring(nullptr));
  CHECK(r.engine.set_lineout_ring(nullptr));

  RingBuffer hdmi(kSinkRingFrames, kHdmiMaxChannels, 256);
  RingBuffer lineout(kSinkRingFrames, kLineoutChannels, 256);
  CHECK(r.engine.set_hdmi_ring(&hdmi));
  CHECK(r.engine.set_lineout_ring(&lineout));
  n = r.block(code);
  CHECK_EQ(hdmi.counter(), kPeriod);
  CHECK_EQ(lineout.counter(), kPeriod);
  const HdmiLayoutInfo& lay = hdmi_layout_info(soc_layout(r.ctl.hdmi, kHdmiMaxChannels));
  auto hdmi_want = [&](uint64_t t, unsigned slot) {
    for (unsigned s = 0; s < lay.speakers; ++s)
      if (lay.slot[s] == slot) return code(n + t, 0);
    return 0.0f;
  };
  auto lineout_want = [&](uint64_t t, unsigned) { return code(n + t, 1); };
  // Wired in two blocks late, so each ring's index 0 is the capture ring's n.
  CHECK(ring_holds(hdmi, 0, kPeriod, hdmi_want));
  CHECK(ring_holds(lineout, 0, kPeriod, lineout_want));
}

// The simulator woken late does not notice it was. Every block it missed is already due, so the
// blocks run back to back until the pacing has caught up with its own deadlines, which stay where
// they were: one period apart from the start of the stream, never moved on to the late time. Each
// block's anchor is stamped when it actually ran, so while the pacing catches up the anchor's n
// moves on a block at a time and its time stands still. Nothing is counted: no xrun, no generation
// bump, and the ring's indices run on without a gap. Pinned as it stands, since a policy of its own
// (a short burst, then counting the rest as an xrun and moving the deadlines) would change every
// line of it.
void test_simulator_catches_up_after_a_late_wakeup(unsigned rate) {
  Control ctl;
  RingBuffer ring(kBlockRingFrames, kTotalInputs, 2 * kPeriod);
  ManualClock clock;
  AudioEngine engine(ctl, ring, sim_options(rate), clock);
  EngineTestAccess::prepare(engine);
  AudioBackend& sim = EngineTestAccess::backend(engine);
  CHECK(sim.open());
  CHECK(sim.start());
  const uint64_t block_ns = sim_block_ns(rate);
  const uint64_t start = ManualClock::kStartNs;

  // Two blocks on time, then the thread wakes from the second's sleep five blocks late.
  constexpr unsigned kLate = 5;
  std::vector<uint64_t> stamped;
  auto run = [&] {
    CHECK(EngineTestAccess::run_block(engine));
    uint64_t an = 0, at = 0;
    CHECK(ctl.anchor.read(&an, &at));
    CHECK_EQ(an, stamped.size() * kPeriod);
    stamped.push_back(at);
  };
  run();
  run();
  clock.advance(kLate * block_ns);
  const uint64_t woke = clock.now_ns();
  CHECK_EQ(woke, start + (2 + kLate) * block_ns);
  // The five that were missed and the one due as it woke all run at once.
  for (unsigned b = 0; b <= kLate; ++b) run();
  CHECK_EQ(clock.now_ns(), start + (3 + kLate) * block_ns);
  // And from then on it is back on the cadence it started with.
  run();
  run();

  const std::vector<uint64_t> want_stamps = {start,
                                             start + block_ns,
                                             woke, woke, woke, woke, woke, woke,
                                             start + (3 + kLate) * block_ns,
                                             start + (4 + kLate) * block_ns};
  CHECK(stamped == want_stamps);
  bool paced = clock.sleeps.size() == want_stamps.size();
  for (size_t b = 0; paced && b < clock.sleeps.size(); ++b)
    paced &= clock.sleeps[b] == start + (b + 1) * block_ns;
  CHECK(paced);

  const EngineStats s = engine.stats();
  CHECK_EQ(s.xruns, 0u);
  CHECK_EQ(s.generation, 0u);
  CHECK_EQ(ring.counter(), want_stamps.size() * kPeriod);
}

// What each real backend says it will open before it has opened anything: the engine publishes
// this after an open that failed before the card settled anything.
void test_backends_report_the_shape_they_ask_for() {
  Control ctl;
  ManualClock clock;
  EngineOptions opt;
  const BackendShape octo = AlsaLinkedBackend(ctl, opt, clock).shape();
  CHECK_EQ(octo.rate, rpi3_octo_profile().clock.rate);
  CHECK_EQ(octo.period, rpi3_octo_profile().clock.period);
  CHECK_EQ(octo.capture_channels, rpi3_octo_profile().clock.capture_slots.front());
  CHECK_EQ(octo.capture_channels, kTdmSlots);
  CHECK_EQ(octo.playback_channels, kOutputs);
  CHECK_EQ(std::string(octo.format), std::string("S32_LE"));

  opt.capture_channels = kInputs;
  opt.period = 512;
  opt.rate = 48000;
  const BackendShape six = AlsaLinkedBackend(ctl, opt, clock).shape();
  CHECK_EQ(six.capture_channels, kInputs);
  CHECK_EQ(six.period, 512u);
  CHECK_EQ(six.rate, 48000u);
}

}  // namespace

int main() {
  for (const unsigned rate : kTestRates) {
    std::printf("  at %u Hz\n", rate);
    test_block_without_a_net_server(rate);
    test_input_gain_mute_bypass_and_clamp(rate);
    test_capture_delay_realigns_the_ring(rate);
    test_capture_delay_shorter_than_a_block(rate);
    test_ping_log_names_where_the_ring_shows_it(rate);
    test_each_dac_renders_its_route(rate);
    test_sink_rings_are_written_every_block(rate);
    test_a_sink_ring_of_another_width_is_refused(rate);
    test_music_is_rendered_only_while_played(rate);
    test_sim_run_keeps_one_sample_axis(rate);
    test_sim_loops_outputs_back_staggered(rate);
    test_fake_card_remaps_both_ways(rate);
    test_capture_falls_back_to_six_slots(rate);
    test_xrun_moves_the_generation_once(rate);
    test_card_is_retried_until_it_streams(rate);
    test_simulator_steps_on_a_manual_clock(rate);
    test_simulator_catches_up_after_a_late_wakeup(rate);
    test_the_card_decides_the_period(rate);
  }
  test_backends_report_the_shape_they_ask_for();
  return report("engine");
}
