#include "listen_encoder.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include <opus.h>

#include "check.h"
#include "constants.h"

using namespace st;

namespace {

constexpr unsigned kRate96 = 96000;

// A linear chirp: a sharp autocorrelation peak, so a cross-correlation recovers a delay cleanly.
float chirp(long n) {
  const double t = n / static_cast<double>(kRate96);
  const double f0 = 500.0, f1 = 12000.0, sweep = 1.0;  // 500 Hz -> 12 kHz over 1 s
  const double k = (f1 - f0) / sweep;
  const double phase = 2.0 * kPi * (f0 * t + 0.5 * k * t * t);
  return 0.3f * static_cast<float>(std::sin(phase));
}

// Encode a mono signal (gen gives sample n at 96 kHz) through the real OpusMonoEncoder and return
// the decoded 48 kHz PCM.
std::vector<float> encode_decode_mono(const std::function<float(long)>& gen, int blocks) {
  bool ok = false;
  OpusMonoEncoder enc(kRate96, kListenBitrateDefaultKbps, &ok);
  CHECK(ok);
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
// scheduled by absolute sample index stay sample-aligned. Delay channel B by K (96 kHz) samples;
// after 2:1 decimation the decoded streams must sit exactly K/2 (48 kHz) samples apart. The
// identical codec + decimator delay cancels in the cross-channel measurement.
void test_cross_channel_alignment() {
  const int K = 192;    // 96 kHz samples, even
  const int d = K / 2;  // expected 48 kHz lag
  const int blocks = 60;
  const std::vector<float> a = encode_decode_mono([](long n) { return chirp(n); }, blocks);
  const std::vector<float> b = encode_decode_mono([K](long n) { return chirp(n - K); }, blocks);
  CHECK_EQ(a.size(), b.size());

  const int start = 4800, len = 12000;  // skip the codec warm-up, correlate a steady window
  const int lag = best_lag(a, b, d - 20, d + 20, start, len);
  CHECK_NEAR(lag, d, 2);
}

// A sanity check that the encoder produces valid audio at roughly the right level: a 0.5-amplitude
// tone has an RMS of 0.5/sqrt(2) = -9.03 dBFS, and Opus at 96 kbps should preserve that within
// ~1.5 dB.
void test_mono_level_preserved() {
  const std::vector<float> out = encode_decode_mono(
      [](long n) { return 0.5f * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * n / kRate96)); },
      40);
  CHECK(out.size() > kOpusFrameFrames);
  double sumsq = 0.0;
  int cnt = 0;
  for (size_t i = out.size() / 2; i < out.size(); ++i) {  // steady tail only
    sumsq += static_cast<double>(out[i]) * out[i];
    ++cnt;
  }
  const double db = 20.0 * std::log10(std::sqrt(sumsq / cnt));
  CHECK_NEAR(db, -9.03, 1.5);
}

}  // namespace

int main() {
  test_cross_channel_alignment();
  test_mono_level_preserved();
  return report("opus");
}
