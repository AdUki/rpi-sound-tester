// The parts of a network session that are pure decisions rather than sockets: which channels a
// sender is given, how a wire sample becomes a float, and where a sender's audio is placed on the
// engine's axis. All were only ever exercised through a live connection before, which is a poor
// place to discover an off-by-one.
#include <sys/socket.h>
#include <unistd.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "control.h"
#include "manual_clock.h"
#include "net_audio.h"
#include "net_proto.h"
#include "rates.h"
#include "vorbis_decode.h"

#include <vorbis/vorbisenc.h>

namespace st {

// The friend of NetAudioServer: one sender's session, driven a message at a time on the test's own
// thread, with no connection behind it and no audio thread in front of it.
struct NetTestAccess {
  using Session = NetAudioServer::Session;

  static bool format(Session& s, const uint8_t* p, uint32_t len) { return s.format(p, len); }
  static bool audio(Session& s, const std::vector<uint8_t>& p) {
    return s.audio(p.data(), static_cast<uint32_t>(p.size()));
  }
  static void distribute(Session& s, const float* src, size_t frames, uint64_t at, uint64_t reader,
                         uint64_t max_lead) {
    s.distribute(src, frames, at, reader, max_lead);
  }
};

}  // namespace st

using namespace st;

namespace {

// Where nothing depends on the engine's rate, the Pi's.
constexpr double kRate = 96000.0;

// ---- how a wire sample becomes a float ---------------------------------------------------

void test_every_wire_format_converts() {
  uint8_t b[4];

  st_put_u16(b, 0x4000);  // +0.5 in S16
  CHECK_NEAR(st_sample_to_float(b, ST_FMT_S16_LE), 0.5f, 1e-6);
  st_put_u16(b, 0xC000);  // -0.5
  CHECK_NEAR(st_sample_to_float(b, ST_FMT_S16_LE), -0.5f, 1e-6);
  st_put_u16(b, 0x8000);  // most negative
  CHECK_NEAR(st_sample_to_float(b, ST_FMT_S16_LE), -1.0f, 1e-6);

  st_put_u32(b, 0x40000000u);
  CHECK_NEAR(st_sample_to_float(b, ST_FMT_S32_LE), 0.5f, 1e-6);
  st_put_u32(b, 0xC0000000u);
  CHECK_NEAR(st_sample_to_float(b, ST_FMT_S32_LE), -0.5f, 1e-6);

  const float f = -0.25f;
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  st_put_u32(b, bits);
  CHECK_EQ(st_sample_to_float(b, ST_FMT_FLOAT_LE), -0.25f);
}

// Packed 24-bit is the one with a sign that is easy to lose: three bytes, no native type, and a
// negative value that reads as a large positive one if it is not extended.
void test_packed_24_bit_keeps_its_sign() {
  uint8_t b[3];
  b[0] = 0x00; b[1] = 0x00; b[2] = 0x40;   // +0.5
  CHECK_NEAR(st_sample_to_float(b, ST_FMT_S24_3LE), 0.5f, 1e-6);
  b[0] = 0x00; b[1] = 0x00; b[2] = 0xC0;   // -0.5
  CHECK_NEAR(st_sample_to_float(b, ST_FMT_S24_3LE), -0.5f, 1e-6);
  b[0] = 0x00; b[1] = 0x00; b[2] = 0x80;   // most negative
  CHECK_NEAR(st_sample_to_float(b, ST_FMT_S24_3LE), -1.0f, 1e-6);
  b[0] = 0xFF; b[1] = 0xFF; b[2] = 0x7F;   // most positive
  CHECK(st_sample_to_float(b, ST_FMT_S24_3LE) > 0.999f);
  b[0] = 0xFF; b[1] = 0xFF; b[2] = 0xFF;   // -1 LSB: must be a small NEGATIVE number
  CHECK(st_sample_to_float(b, ST_FMT_S24_3LE) < 0.0f);
  CHECK(st_sample_to_float(b, ST_FMT_S24_3LE) > -1e-4f);
}

void test_sample_sizes_match_the_formats() {
  CHECK_EQ(st_fmt_bytes(ST_FMT_S16_LE), 2u);
  CHECK_EQ(st_fmt_bytes(ST_FMT_S24_3LE), 3u);
  CHECK_EQ(st_fmt_bytes(ST_FMT_S32_LE), 4u);
  CHECK_EQ(st_fmt_bytes(ST_FMT_FLOAT_LE), 4u);
  CHECK_EQ(st_fmt_bytes(999u), 0u);  // an unknown format must be refused, not guessed at
}

// ---- which channels a sender is given ----------------------------------------------------

void test_a_sender_gets_the_lowest_free_run() {
  Control ctl;
  NetAudioServer net(ctl, kRate, kTestPeriod);
  CHECK_EQ(net.claim_channels(ST_HELLO_ANY_CHANNEL, "10.0.0.1", 1), 0);
  CHECK_EQ(net.claim_channels(ST_HELLO_ANY_CHANNEL, "10.0.0.2", 1), 1);
}

void test_a_multichannel_sender_gets_adjacent_channels() {
  Control ctl;
  NetAudioServer net(ctl, kRate, kTestPeriod);
  CHECK_EQ(net.claim_channels(ST_HELLO_ANY_CHANNEL, "10.0.0.1", 1), 0);
  // A pair cannot start at 1 if that would run past a taken channel; here 1..2 are free.
  CHECK_EQ(net.claim_channels(ST_HELLO_ANY_CHANNEL, "10.0.0.2", 2), 1);
  CHECK_EQ(net.claim_channels(ST_HELLO_ANY_CHANNEL, "10.0.0.3", 2), 3);
}

// A run that does not fit must leave nothing claimed behind it, or a later sender would find a
// channel owned by no one.
void test_a_run_that_does_not_fit_claims_nothing() {
  Control ctl;
  NetAudioServer net(ctl, kRate, kTestPeriod);
  // Take one in the middle so no run of the full width can fit.
  CHECK_EQ(net.claim_channels(2, "10.0.0.9", 1), 2);
  CHECK_EQ(net.claim_channels(ST_HELLO_ANY_CHANNEL, "10.0.0.1", kNetInputs), -1);
  // Everything except the one deliberately taken must still be free.
  for (unsigned c = 0; c < kNetInputs; ++c) {
    if (c == 2) continue;
    CHECK_EQ(net.claim_channels(c, "10.0.0.5", 1), static_cast<int>(c));
  }
}

void test_an_explicit_channel_wins_and_can_be_refused() {
  Control ctl;
  NetAudioServer net(ctl, kRate, kTestPeriod);
  CHECK_EQ(net.claim_channels(3, "10.0.0.1", 1), 3);
  CHECK_EQ(net.claim_channels(3, "10.0.0.2", 1), -1);  // taken: no silent fallback elsewhere
  CHECK_EQ(net.claim_channels(kNetInputs, "10.0.0.2", 1), 0);  // out of range = "any"
}

// The property routing depends on: a machine that comes back gets the channel it had, so an
// output routed to NET 4 still means that machine.
void test_a_returning_sender_gets_its_old_channel() {
  Control ctl;
  NetAudioServer net(ctl, kRate, kTestPeriod);
  const int first = net.claim_channels(3, "10.0.0.7", 2);
  CHECK_EQ(first, 3);
  net.release_channels(3, 2);

  // Something else takes the lowest free run in between.
  CHECK_EQ(net.claim_channels(ST_HELLO_ANY_CHANNEL, "10.0.0.8", 1), 0);
  // The original machine still comes back to 3, not to the next free channel.
  CHECK_EQ(net.claim_channels(ST_HELLO_ANY_CHANNEL, "10.0.0.7", 2), 3);
}

// ---- which channels one mixer drives -----------------------------------------------------

// The bug this pins down: a mixer opened before anything played found a run one channel wide, and
// went on driving only NET 1 after a stereo sender took NET 1+2.
void test_a_mixer_follows_its_sender_onto_the_whole_run() {
  Control ctl;
  NetAudioServer net(ctl, kRate, kTestPeriod);

  // Nothing known about this machine yet: one channel, and it is the first one.
  auto run = net.mixer_run("10.0.0.7", ST_CTL_ANY_CHANNEL);
  CHECK_EQ(run.base, 0u);
  CHECK_EQ(run.count, 1u);

  // Its stereo stream arrives and lands on 2+3, because something else already has 0+1.
  CHECK_EQ(net.claim_channels(ST_HELLO_ANY_CHANNEL, "10.0.0.9", 2), 0);
  CHECK_EQ(net.claim_channels(ST_HELLO_ANY_CHANNEL, "10.0.0.7", 2), 2);

  run = net.mixer_run("10.0.0.7", ST_CTL_ANY_CHANNEL);
  CHECK_EQ(run.base, 2u);
  CHECK_EQ(run.count, 2u);

  // Still that machine's pair once the stream has gone: an open alsamixer must not jump to
  // somebody else's channel on a disconnect.
  net.release_channels(2, 2);
  run = net.mixer_run("10.0.0.7", ST_CTL_ANY_CHANNEL);
  CHECK_EQ(run.base, 2u);
  CHECK_EQ(run.count, 2u);
}

// An explicit channel is a deliberate pin and must not wander off after an address. It still
// widens to the run that channel belongs to: half of a stereo pair is not a source.
void test_a_pinned_mixer_stays_on_its_channel() {
  Control ctl;
  NetAudioServer net(ctl, kRate, kTestPeriod);
  CHECK_EQ(net.claim_channels(ST_HELLO_ANY_CHANNEL, "10.0.0.7", 2), 0);

  auto run = net.mixer_run("10.0.0.7", 4);
  CHECK_EQ(run.base, 4u);
  CHECK_EQ(run.count, 1u);

  // Pinned to the second half of a pair: the run it drives is the whole pair, from its base.
  run = net.mixer_run("10.0.0.7", 1);
  CHECK_EQ(run.base, 0u);
  CHECK_EQ(run.count, 2u);
}

void test_a_channel_is_only_in_use_once_it_has_been() {
  Control ctl;
  NetAudioServer net(ctl, kRate, kTestPeriod);
  CHECK(!net.channel_in_use(0));
  CHECK_EQ(net.claim_channels(0, "10.0.0.1", 1), 0);
  CHECK(net.channel_in_use(0));
  net.release_channels(0, 1);
  // Still "in use" would be wrong here: nothing was ever sent, so there is nothing to look at.
  CHECK(!net.channel_in_use(0));
  CHECK(!net.channel_in_use(kNetInputs));  // out of range is never in use
}

// ---- where a sender's audio lands --------------------------------------------------------

// A mono sender that has been through the handshake, as hello() leaves one without the socket and
// the resolver it needs: the lowest free channel is its own. It sends float PCM at `src_rate`. The
// STATUS messages the session sends go into a socket pair, read off the other end after every
// packet: a full socket would block the session's write, and the test with it.
struct Sender {
  Sender(NetAudioServer& net, unsigned src_rate) : s(net) {
    CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    s.fd = fds[0];
    s.ip = "10.0.0.1";
    s.peer = "10.0.0.1:40000";
    s.channels = 1;
    s.base = net.claim_channels(ST_HELLO_ANY_CHANNEL, s.ip, 1);
    CHECK_EQ(s.base, 0);

    uint8_t f[ST_FORMAT_BYTES] = {0};
    st_put_u32(f + ST_FMT_O_RATE, src_rate);
    st_put_u32(f + ST_FMT_O_FORMAT, ST_FMT_FLOAT_LE);
    st_put_u32(f + ST_FMT_O_CHANNELS, 1);
    st_put_u32(f + ST_FMT_O_ENCODING, ST_ENC_PCM);
    CHECK(NetTestAccess::format(s, f, sizeof(f)));
  }
  ~Sender() {
    ::close(fds[0]);
    ::close(fds[1]);
  }

  // One AUDIO message: `frames` frames of a quiet constant, the first of them the sender's `pos`th.
  bool send(uint64_t pos, uint32_t frames) {
    std::vector<uint8_t> p(ST_AUDIO_FIXED + 4ull * frames);
    st_put_u64(p.data() + ST_AUDIO_O_POS, pos);
    st_put_u32(p.data() + ST_AUDIO_O_FRAMES, frames);
    const float v = 0.25f;
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    for (uint32_t i = 0; i < frames; ++i) st_put_u32(p.data() + ST_AUDIO_FIXED + 4ull * i, bits);
    const bool ok = NetTestAccess::audio(s, p);
    uint8_t drain[512];
    while (::recv(fds[1], drain, sizeof(drain), MSG_DONTWAIT) > 0) {
    }
    return ok;
  }

  int fds[2] = {-1, -1};
  NetTestAccess::Session s;
};

// The audio thread's side of it, with no engine: a block of `period` frames due every period/rate
// of the clock from when it was made, each publishing its first sample as the anchor, stamped
// with the time it was due, and reading the network channels at that sample.
struct AudioThread {
  AudioThread(Control& c, NetAudioServer& n, ManualClock& k, double r, unsigned p)
      : ctl(c), net(n), clock(k), rate(r), period(p), start_ns(k.now_ns()),
        live(static_cast<size_t>(p) * kTotalInputs), ring(static_cast<size_t>(p) * kTotalInputs) {}

  // Every block that is due by now.
  void run_due() {
    for (;;) {
      const uint64_t due = start_ns + static_cast<uint64_t>(1e9 * static_cast<double>(n) / rate);
      if (due > clock.now_ns()) return;
      ctl.anchor.publish(n, due);
      net.read_block(n, period, ctl.net.delay_frames.load(), live.data(), ring.data());
      n += period;
    }
  }

  Control& ctl;
  NetAudioServer& net;
  ManualClock& clock;
  const double rate;
  const unsigned period;
  const uint64_t start_ns;
  uint64_t n = 0;
  std::vector<float> live, ring;
};

// A packet has to land two blocks ahead of where the audio thread is reading, or it is dropped as
// late: the reader could otherwise reach its frames while they are still being written. The blocks
// are the period the server was built with, so the guard follows it; main() builds it with the
// board's 1024, which makes the guard the 2048 frames it has always been on the Pi.
void test_a_packet_must_clear_the_guard() {
  for (const unsigned period : {256u, kTestPeriod, 2048u}) {
    Control ctl;
    NetAudioServer net(ctl, kRate, period);
    Sender tx(net, static_cast<unsigned>(kRate));
    const std::vector<float> chunk(kNetPacketFrames, 0.25f);
    const uint64_t reader = 40 * kTestPeriod;
    const uint64_t guard = 2ull * period;
    const uint64_t max_lead = static_cast<uint64_t>(kRate * kNetTimelineMs / 2000.0);

    NetTestAccess::distribute(tx.s, chunk.data(), chunk.size(), reader + guard - 1, reader,
                              max_lead);
    NetChannelStatus st = net.status()[0];
    CHECK_EQ(st.late_drops, 1u);
    CHECK_EQ(st.frames_received, 0u);

    NetTestAccess::distribute(tx.s, chunk.data(), chunk.size(), reader + guard, reader, max_lead);
    st = net.status()[0];
    CHECK_EQ(st.late_drops, 1u);
    CHECK_EQ(st.frames_received, static_cast<uint64_t>(kNetPacketFrames));
    CHECK_EQ(st.write_end, reader + guard + kNetPacketFrames);
  }
}

// A sender that stalls and then carries on where it left off, its stream still contiguous: every
// frame after the stall lands that much nearer playout than the delay asks for. A stall the trim
// can walk back is left to it; one of more than kNetResyncS is re-anchored, once, and counted in
// the channel's resyncs, at either rate. 0.2 s and 0.3 s sit either side of the quarter second:
// at 48 kHz 0.3 s is 14400 frames, under the 24000 of the constant the threshold was at 96 kHz,
// and at 96 kHz 0.2 s is 19200, over the 12000 of a threshold stuck at 48 kHz.
void test_a_sender_too_far_off_its_delay_is_re_anchored(unsigned rate) {
  for (const double stall_s : {0.2, 0.3}) {
    Control ctl;
    ManualClock clock;
    NetAudioServer net(ctl, rate, kTestPeriod, clock);
    ctl.net.delay_frames.store(static_cast<uint32_t>(0.5 * rate));  // net.delay_ms 500
    AudioThread audio(ctl, net, clock, rate, kTestPeriod);
    Sender tx(net, rate);

    // Packets sent the moment they are due at the sender's own pace, `behind_ns` late.
    const uint64_t t0 = clock.now_ns();
    uint64_t pos = 0;
    uint64_t behind_ns = 0;
    bool sent = true;
    auto send_until = [&](double until_s) {
      for (;;) {
        const uint64_t at = t0 + behind_ns + static_cast<uint64_t>(1e9 * pos / rate);
        if (at > t0 + static_cast<uint64_t>(until_s * 1e9)) return;
        if (at > clock.now_ns()) clock.advance(at - clock.now_ns());
        audio.run_due();
        sent &= tx.send(pos, kNetPacketFrames);
        pos += kNetPacketFrames;
      }
    };

    send_until(3.0);  // long enough for the lead to have settled on the delay
    CHECK_EQ(net.status()[0].resyncs, 0u);
    behind_ns = static_cast<uint64_t>(stall_s * 1e9);
    send_until(16.0);

    const NetChannelStatus st = net.status()[0];
    CHECK(sent);
    CHECK_EQ(st.resyncs, stall_s > kNetResyncS ? 1u : 0u);
    // Every packet still landed clear of the reader and inside the timeline.
    CHECK_EQ(st.late_drops, 0u);
    CHECK_EQ(st.range_drops, 0u);
    // Re-anchored back onto the delay, or left a stall's worth short of it and closing.
    const double off_s = (static_cast<double>(st.lead_frames) - 0.5 * rate) / rate;
    if (stall_s > kNetResyncS) {
      CHECK(std::fabs(off_s) < 0.01);
    } else {
      CHECK(off_s < -0.15 && off_s > -stall_s);
    }
  }
}

// On the Pi's clock both are the numbers they were when they were constants.
void test_the_pi_keeps_its_thresholds() {
  Control ctl;
  NetAudioServer net(ctl, 96000.0, 1024);
  CHECK_EQ(net.guard_frames(), 2048ull);
  CHECK_EQ(net.resync_frames(), 24000.0);
}

// ---- the decoder ---------------------------------------------------------------------------

// Encoded here with libvorbis and decoded by the class the daemon uses, so the pair is checked
// together rather than each against an assumption about the other.
void test_vorbis_round_trip() {
  const int channels = 2;
  const long rate = 44100;
  const size_t frames = rate;  // one second

  vorbis_info vi;
  vorbis_info_init(&vi);
  CHECK_EQ(vorbis_encode_init_vbr(&vi, channels, rate, 0.4f), 0);
  vorbis_comment vc;
  vorbis_comment_init(&vc);
  vorbis_dsp_state vd;
  CHECK_EQ(vorbis_analysis_init(&vd, &vi), 0);
  vorbis_block vb;
  vorbis_block_init(&vd, &vb);

  ogg_packet oh, oc, ob;
  CHECK_EQ(vorbis_analysis_headerout(&vd, &vc, &oh, &oc, &ob), 0);
  std::vector<std::vector<uint8_t>> headers;
  for (const ogg_packet* h : {&oh, &oc, &ob})
    headers.emplace_back(h->packet, h->packet + h->bytes);

  VorbisDecoder dec;
  std::string err;
  CHECK(dec.init(headers, &err));
  if (!err.empty()) std::printf("  init: %s\n", err.c_str());
  CHECK_EQ(dec.channels(), channels);
  CHECK_EQ(dec.rate(), rate);

  std::vector<float> out;
  size_t fed = 0;
  while (fed < frames) {
    const int n = 1024;
    float** buf = vorbis_analysis_buffer(&vd, n);
    for (int i = 0; i < n; ++i) {
      const double t = static_cast<double>(fed + i) / rate;
      buf[0][i] = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979 * 1000.0 * t));
      buf[1][i] = buf[0][i];
    }
    vorbis_analysis_wrote(&vd, n);
    fed += n;
    while (vorbis_analysis_blockout(&vd, &vb) == 1) {
      vorbis_analysis(&vb, nullptr);
      vorbis_bitrate_addblock(&vb);
      ogg_packet op;
      while (vorbis_bitrate_flushpacket(&vd, &op) == 1) dec.decode(op.packet, op.bytes, &out, &err);
    }
  }

  const size_t got = out.size() / channels;
  CHECK(got > frames * 9 / 10);   // most of what went in comes back; the tail is still in the encoder

  // The tone survived, and both channels came out identical, as they went in.
  double peak = 0.0, worst_pair = 0.0;
  for (size_t i = frames / 4; i < got - 16; ++i) {
    peak = std::max(peak, std::fabs(static_cast<double>(out[i * channels])));
    worst_pair = std::max(worst_pair,
                          std::fabs(static_cast<double>(out[i * channels] - out[i * channels + 1])));
  }
  CHECK(peak > 0.4);
  CHECK_EQ(worst_pair, 0.0);

  vorbis_block_clear(&vb);
  vorbis_dsp_clear(&vd);
  vorbis_comment_clear(&vc);
  vorbis_info_clear(&vi);
}

void test_a_decoder_refuses_nonsense_setup() {
  VorbisDecoder dec;
  std::string err;
  CHECK(!dec.init({}, &err));            // no packets at all
  CHECK(!err.empty());
  std::vector<std::vector<uint8_t>> junk(3, std::vector<uint8_t>(16, 0xAB));
  CHECK(!dec.init(junk, &err));          // three packets of rubbish
  CHECK(!dec.ready());
  // And decoding without a successful init must be a no-op, not a crash.
  std::vector<float> out;
  CHECK_EQ(dec.decode(junk[0].data(), junk[0].size(), &out, &err), 0u);
  CHECK(out.empty());
}

}  // namespace

int main() {
  test_every_wire_format_converts();
  test_packed_24_bit_keeps_its_sign();
  test_sample_sizes_match_the_formats();
  test_a_sender_gets_the_lowest_free_run();
  test_a_multichannel_sender_gets_adjacent_channels();
  test_a_run_that_does_not_fit_claims_nothing();
  test_an_explicit_channel_wins_and_can_be_refused();
  test_a_returning_sender_gets_its_old_channel();
  test_a_channel_is_only_in_use_once_it_has_been();
  test_a_mixer_follows_its_sender_onto_the_whole_run();
  test_a_pinned_mixer_stays_on_its_channel();
  test_a_packet_must_clear_the_guard();
  for (const unsigned rate : kTestRates) {
    std::printf("  at %u Hz\n", rate);
    test_a_sender_too_far_off_its_delay_is_re_anchored(rate);
  }
  test_the_pi_keeps_its_thresholds();
  test_vorbis_round_trip();
  test_a_decoder_refuses_nonsense_setup();
  return report("net_session");
}
