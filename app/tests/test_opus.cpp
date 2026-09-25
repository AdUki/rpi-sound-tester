#include "listen_encoder.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <opus.h>

#include "channel_layout.h"
#include "check.h"
#include "constants.h"
#include "rates.h"

using namespace st;

namespace {

// A linear chirp: a sharp autocorrelation peak, so a cross-correlation recovers a delay cleanly.
float chirp(long n, unsigned rate) {
  const double t = n / static_cast<double>(rate);
  const double f0 = 500.0, f1 = 12000.0, sweep = 1.0;  // 500 Hz -> 12 kHz over 1 s
  const double k = (f1 - f0) / sweep;
  const double phase = 2.0 * kPi * (f0 * t + 0.5 * k * t * t);
  return 0.3f * static_cast<float>(std::sin(phase));
}

// Encode a mono signal (gen gives sample n at `rate`) through the real OpusMonoEncoder and return
// the decoded 48 kHz PCM.
std::vector<float> encode_decode_mono(const std::function<float(long)>& gen, int blocks,
                                      unsigned rate) {
  bool ok = false;
  OpusMonoEncoder enc(rate, kListenBitrateDefaultKbps, &ok);
  CHECK(ok);
  // One 20 ms Opus frame of ring audio per packet: the decimator passes 48 kHz straight through
  // and halves 96 kHz.
  CHECK_EQ(enc.in_frames(), kOpusFrameFrames * (rate / kOpusRate));
  int derr = 0;
  OpusDecoder* dec = opus_decoder_create(kOpusRate, 1, &derr);
  CHECK(derr == OPUS_OK);

  const unsigned inN = enc.in_frames();
  std::vector<float> in(inN), out(kOpusFrameFrames), decoded;
  std::vector<uint8_t> pkt;
  long n = 0;
  for (int b = 0; b < blocks; ++b) {
    for (unsigned i = 0; i < inN; ++i) in[i] = gen(n++);
    CHECK(enc.encode(in.data(), kListenBitrateDefaultKbps, &pkt));
    const int got = opus_decode_float(dec, pkt.data(), static_cast<opus_int32>(pkt.size()),
                                      out.data(), kOpusFrameFrames, 0);
    CHECK(got == static_cast<int>(kOpusFrameFrames));
    if (got > 0) decoded.insert(decoded.end(), out.begin(), out.begin() + got);
  }
  opus_decoder_destroy(dec);
  return decoded;
}

// Lag in [lo,hi] maximizing sum a[n]*b[n+lag] over [start, start+len).
int best_lag(const std::vector<float>& a, const std::vector<float>& b, int lo, int hi, int start,
             int len) {
  double best = -1e30;
  int arg = lo;
  for (int lag = lo; lag <= hi; ++lag) {
    double s = 0.0;
    for (int n = start; n < start + len; ++n) {
      const int m = n + lag;
      if (m < 0 || m >= static_cast<int>(b.size())) continue;
      s += static_cast<double>(a[n]) * b[m];
    }
    if (s > best) {
      best = s;
      arg = lag;
    }
  }
  return arg;
}

// The invariant the whole feature rests on: two channels encoded by identical Opus encoders and
// scheduled by absolute sample index stay sample-aligned. Delay channel B by K ring samples; after
// the decimation to 48 kHz (none at 48 kHz, 2:1 at 96 kHz) the decoded streams must sit exactly
// K/factor (48 kHz) samples apart. The identical codec + decimator delay cancels in the
// cross-channel measurement.
void test_cross_channel_alignment(unsigned rate) {
  const int factor = static_cast<int>(rate / kOpusRate);
  const int K = 192;         // ring samples, even
  const int d = K / factor;  // expected 48 kHz lag
  const int blocks = 60;
  const std::vector<float> a =
      encode_decode_mono([rate](long n) { return chirp(n, rate); }, blocks, rate);
  const std::vector<float> b =
      encode_decode_mono([rate, K](long n) { return chirp(n - K, rate); }, blocks, rate);
  CHECK_EQ(a.size(), b.size());

  const int start = 4800, len = 12000;  // skip the codec warm-up, correlate a steady window
  const int lag = best_lag(a, b, d - 20, d + 20, start, len);
  CHECK_NEAR(lag, d, 2);
}

// A sanity check that the encoder produces valid audio at roughly the right level: a 0.5-amplitude
// tone has an RMS of 0.5/sqrt(2) = -9.03 dBFS, and Opus at 96 kbps should preserve that within
// ~1.5 dB.
void test_mono_level_preserved(unsigned rate) {
  const std::vector<float> out = encode_decode_mono(
      [rate](long n) { return 0.5f * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * n / rate)); },
      40, rate);
  CHECK(out.size() > kOpusFrameFrames);
  double sumsq = 0.0;
  int cnt = 0;
  for (size_t i = out.size() / 2; i < out.size(); ++i) {  // steady tail only
    sumsq += static_cast<double>(out[i]) * out[i];
    ++cnt;
  }
  const double db = 20.0 * std::log10(std::sqrt(sumsq / cnt));
  CHECK_NEAR(db, -9.03, 1.5);

  // And at its own pitch: the decimation has to match the rate, or a 1 kHz tone comes out at
  // another frequency while its level stays right.
  size_t crossings = 0;
  for (size_t i = out.size() / 2 + 1; i < out.size(); ++i)
    crossings += (out[i - 1] < 0.0f) != (out[i] < 0.0f);
  const double seconds = static_cast<double>(out.size() - out.size() / 2 - 1) / kOpusRate;
  CHECK_NEAR(crossings / (2.0 * seconds), 1000.0, 10.0);
}

// The granule position of the last Ogg page in `pages`, or -1 if they do not parse as pages.
int64_t last_granule(const std::string& pages) {
  int64_t granule = -1;
  size_t o = 0;
  while (o + 27 <= pages.size()) {
    if (pages.compare(o, 4, "OggS") != 0) return -1;
    const auto* p = reinterpret_cast<const uint8_t*>(pages.data() + o);
    uint64_t g = 0;
    for (int i = 7; i >= 0; --i) g = (g << 8) | p[6 + i];
    const size_t segments = p[26];
    if (o + 27 + segments > pages.size()) return -1;
    size_t body = 0;
    for (size_t i = 0; i < segments; ++i) body += p[27 + i];
    granule = static_cast<int64_t>(g);
    o += 27 + segments + body;
  }
  return o == pages.size() ? granule : -1;
}

// stream.ogg takes the same 20 ms of ring per encode, for every channel at once, and its pages
// count granules at Opus's 48 kHz whatever the ring runs at.
void test_ogg_stream_encodes(unsigned rate) {
  bool ok = false;
  OpusOggMultiEncoder enc(rate, kListenBitrateDefaultKbps, 1234, &ok);
  CHECK(ok);
  if (!ok) return;
  CHECK_EQ(enc.in_frames(), kOpusFrameFrames * (rate / kOpusRate));
  CHECK_EQ(last_granule(enc.headers()), 0);

  const int packets = 50;
  std::vector<float> in(static_cast<size_t>(enc.in_frames()) * st::channels().total());
  std::string pages;
  long n = 0;
  for (int b = 0; b < packets; ++b) {
    for (unsigned i = 0; i < enc.in_frames(); ++i, ++n) {
      const float v = 0.25f * static_cast<float>(std::sin(2.0 * kPi * 440.0 * n / rate));
      for (unsigned c = 0; c < st::channels().total(); ++c) in[i * st::channels().total() + c] = v;
    }
    CHECK(enc.encode(in.data(), kListenBitrateDefaultKbps, &pages));
  }
  // Whole 20 ms frames, and no more of them than went in: the muxer may still hold the last few.
  const int64_t g = last_granule(pages);
  CHECK(g > 0);
  CHECK_EQ(g % kOpusFrameFrames, 0);
  CHECK(g <= static_cast<int64_t>(packets) * kOpusFrameFrames);
}

// Opus takes only an integer decimation to 48 kHz, so any other rate has no encoded stream.
void test_other_rates_are_refused() {
  bool ok = true;
  OpusMonoEncoder mono(44100, kListenBitrateDefaultKbps, &ok);
  CHECK(!ok);
  ok = true;
  OpusOggMultiEncoder multi(44100, kListenBitrateDefaultKbps, 1, &ok);
  CHECK(!ok);
}

}  // namespace

int main() {
  for (const unsigned rate : kTestRates) {
    std::printf("  at %u Hz\n", rate);
    test_cross_channel_alignment(rate);
    test_mono_level_preserved(rate);
    test_ogg_stream_encodes(rate);
  }
  test_other_rates_are_refused();
  return report("opus");
}
