#include "bt_input.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <time.h>

#include <vector>

#include "bluetooth.h"
#include "channel_layout.h"
#include "net_audio.h"
#include "util/log.h"

namespace st {

namespace {

// How often the device being recorded is checked against the one that should be: a second phone
// that starts streaming takes over, as bluez-alsa's own "most recent" default would.
constexpr uint64_t kSourceCheckNs = 500 * 1000000ull;
// With no audio for this long the stream reads as waiting rather than streaming: the phone paused.
constexpr uint64_t kIdleNs = 1000 * 1000000ull;
// After a PCM that would not open, before trying again.
constexpr unsigned kRetryMs = 2000;
// The least alignment delay a stream can be placed with: two guard blocks ahead of the reader is
// what a timeline write keeps clear (NetAudioServer's guard), and the radio's jitter needs more
// than that on top.
constexpr uint32_t kBtMinDelayFrames = 4 * kNetGuardPeriod;

float s16_to_float(const uint8_t* p) {
  const int16_t v = static_cast<int16_t>(p[0] | (p[1] << 8));
  return static_cast<float>(v) / 32768.0f;
}

float s32_to_float(const uint8_t* p, bool s24) {
  uint32_t u = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
  if (s24) u <<= 8;  // 24 valid bits in the low three bytes
  return static_cast<float>(static_cast<int32_t>(u)) / 2147483648.0f;
}

}  // namespace

BtInput::BtInput(Control& ctl, NetAudioServer& net, const BtManager& bt)
    : ctl_(ctl), net_(net), bt_(bt) {}

BtInput::~BtInput() { stop(); }

void BtInput::start() {
  std::lock_guard<std::mutex> life(life_m_);
  if (running_.load()) return;
  running_.store(true);
  set_state("waiting", {});
  thread_ = std::thread([this] { run(); });
}

void BtInput::stop() {
  std::lock_guard<std::mutex> life(life_m_);
  running_.store(false);
  if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lk(m_);
  st_ = BtInputStatus{};
}

BtInputStatus BtInput::status() const {
  std::lock_guard<std::mutex> lk(m_);
  BtInputStatus s = st_;
  s.enabled = ctl_.net.bt_input.load();
  if (!running_.load()) s.state = "off";
  return s;
}

void BtInput::set_state(const std::string& state, const std::string& error) {
  std::lock_guard<std::mutex> lk(m_);
  st_.state = state;
  st_.error = error;
}

void BtInput::sleep_ms(unsigned ms) const {
  for (unsigned waited = 0; waited < ms && running_.load(); waited += 50) {
    timespec ts{0, 50 * 1000000L};
    nanosleep(&ts, nullptr);
  }
}

void BtInput::run() {
  LOG_INFO("bluetooth input: on");
  while (running_.load()) {
    std::string address, name;
    unsigned rate = 0, channels = 0;
    if (!bt_.capture_source(&address, &name, &rate, &channels)) {
      {
        std::lock_guard<std::mutex> lk(m_);
        st_.state = "waiting";
        st_.address.clear();
        st_.name.clear();
        st_.rate = 0;
        st_.channels = 0;
        st_.input = -1;
      }
      sleep_ms(250);
      continue;
    }
    if (!record(address, name, rate, channels)) sleep_ms(kRetryMs);
  }
  LOG_INFO("bluetooth input: off");
}

bool BtInput::record(const std::string& address, const std::string& name, unsigned rate,
                     unsigned channels) {
  if (channels == 0 || channels > kNetInputs) channels = kBtChannels;
  const std::string dev = bt_pcm_name(address);
  {
    // Named before anything can fail, so an error says whose stream it is about.
    std::lock_guard<std::mutex> lk(m_);
    st_.address = address;
    st_.name = name;
    st_.rate = rate;
    st_.channels = channels;
  }

  // Non-blocking, like the SoC outputs: a stalled device must never hold stop() hostage.
  snd_pcm_t* pcm = nullptr;
  int err = snd_pcm_open(&pcm, dev.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
  if (err < 0) {
    set_state("error", "cannot open " + dev + ": " + snd_strerror(err));
    return false;
  }
  auto fail = [&](const std::string& what, int e) {
    set_state("error", dev + ": " + what + ": " + snd_strerror(e));
    snd_pcm_close(pcm);
    return false;
  };

  snd_pcm_hw_params_t* hw;
  snd_pcm_hw_params_alloca(&hw);
  if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0) return fail("hw_params_any", err);
  if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
    return fail("set_access", err);
  // bluez-alsa offers exactly the codec's own format: 16-bit for SBC, wider for the high-
  // resolution codecs. Take whichever it has.
  snd_pcm_format_t fmt = SND_PCM_FORMAT_UNKNOWN;
  for (snd_pcm_format_t f : {SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_S24_LE, SND_PCM_FORMAT_S32_LE}) {
    if (snd_pcm_hw_params_test_format(pcm, hw, f) == 0) {
      fmt = f;
      break;
    }
  }
  if (fmt == SND_PCM_FORMAT_UNKNOWN) return fail("no 16, 24 or 32-bit format", -EINVAL);
  if ((err = snd_pcm_hw_params_set_format(pcm, hw, fmt)) < 0) return fail("set_format", err);
  if ((err = snd_pcm_hw_params_set_channels(pcm, hw, channels)) < 0)
    return fail(std::to_string(channels) + " channels", err);
  unsigned dev_rate = rate ? rate : 48000;
  if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw, &dev_rate, nullptr)) < 0)
    return fail("set_rate", err);
  snd_pcm_uframes_t period = static_cast<snd_pcm_uframes_t>(dev_rate) * kBtInputPeriodMs / 1000;
  if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr)) < 0)
    return fail("set_period_size", err);
  snd_pcm_uframes_t buffer = period * kBtInputPeriods;
  if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0)
    return fail("set_buffer_size", err);
  if ((err = snd_pcm_hw_params(pcm, hw)) < 0) return fail("hw_params", err);
  snd_pcm_hw_params_get_period_size(hw, &period, nullptr);

  const int base = net_.claim_channels(kNetInputs, "bt:" + address, channels);
  if (base < 0) {
    return fail("no " + std::to_string(channels) + " adjacent network channels are free",
                -EBUSY);
  }
  net_.label_channels(static_cast<unsigned>(base), channels, address, name, "bluetooth");

  NetAudioServer::Feed feed(net_);
  feed.set_who("bluetooth: " + name);
  std::string ferr;
  if (!feed.configure(base, channels, dev_rate, &ferr)) {
    net_.release_channels(static_cast<unsigned>(base), channels);
    return fail("converter: " + ferr, -EINVAL);
  }

  if ((err = snd_pcm_prepare(pcm)) < 0 || (err = snd_pcm_start(pcm)) < 0) {
    net_.release_channels(static_cast<unsigned>(base), channels);
    return fail("start", err);
  }

  LOG_INFO("bluetooth input: {} ({}) at {} Hz, {} ch, {} -> input {}", name, address, dev_rate,
           channels, snd_pcm_format_name(fmt), st::channels().net_base() + base);
  {
    std::lock_guard<std::mutex> lk(m_);
    st_.state = "waiting";
    st_.error.clear();
    st_.address = address;
    st_.name = name;
    st_.rate = dev_rate;
    st_.channels = channels;
    st_.input = static_cast<int>(st::channels().net_base()) + base;
    st_.frames = 0;
  }

  const unsigned bytes = static_cast<unsigned>(snd_pcm_format_physical_width(fmt) / 8);
  std::vector<uint8_t> raw(static_cast<size_t>(period) * channels * bytes);
  std::vector<float> il(static_cast<size_t>(period) * channels);
  uint64_t last_check = mono_ns();
  uint64_t last_audio = 0;
  bool pcm_ok = true;
  bool short_delay = false;

  while (running_.load()) {
    const uint64_t now = mono_ns();
    if (now - last_check >= kSourceCheckNs) {
      last_check = now;
      std::string a, n;
      unsigned r = 0, c = 0;
      if (!bt_.capture_source(&a, &n, &r, &c) || a != address) {
        LOG_INFO("bluetooth input: {} {}", name,
                 a.empty() ? "stopped streaming" : "gave way to " + n);
        break;
      }
    }
    if (last_audio && now - last_audio > kIdleNs) {
      std::lock_guard<std::mutex> lk(m_);
      st_.state = "waiting";
    }

    const int w = snd_pcm_wait(pcm, 100);
    if (w < 0 && w != -EPIPE) {
      LOG_WARN("bluetooth input: {}: {}", name, snd_strerror(w));
      pcm_ok = false;
      break;
    }
    const snd_pcm_sframes_t got = snd_pcm_readi(pcm, raw.data(), period);
    if (got == -EAGAIN || got == 0) continue;
    if (got == -EPIPE) {
      // Overrun: this thread was late and bluez-alsa's buffer overflowed. What was lost cannot
      // be placed, so the stream starts again from the next audio.
      {
        std::lock_guard<std::mutex> lk(m_);
        ++st_.overruns;
      }
      LOG_WARN("bluetooth input: {}: overrun", name);
      snd_pcm_prepare(pcm);
      snd_pcm_start(pcm);
      feed.restart();
      continue;
    }
    if (got < 0) {
      LOG_WARN("bluetooth input: {}: {}", name, snd_strerror(static_cast<int>(got)));
      pcm_ok = false;
      break;
    }

    const size_t n = static_cast<size_t>(got) * channels;
    for (size_t i = 0; i < n; ++i) {
      const uint8_t* p = raw.data() + i * bytes;
      il[i] = fmt == SND_PCM_FORMAT_S16_LE ? s16_to_float(p)
                                           : s32_to_float(p, fmt == SND_PCM_FORMAT_S24_LE);
    }

    // The stream is placed the alignment delay ahead of playout. Below the guard a write must
    // keep, nothing it sends could ever land: say so once, rather than re-anchor every read.
    if (ctl_.net.delay_frames.load() < kBtMinDelayFrames) {
      if (!short_delay) {
        LOG_WARN("bluetooth input: the alignment delay is too short to place anything — raise it");
        short_delay = true;
      }
      set_state("error", "the alignment delay is too short for Bluetooth: raise it to 100 ms or more");
      continue;
    }
    short_delay = false;

    // A phone that paused, or a link that stalled for longer than the alignment delay, comes back
    // with its stream already behind playout: everything it sends would be late. Start it again
    // where it is, rather than drop audio until the lead filter notices.
    const bool resumed = feed.behind();
    if (resumed) {
      LOG_INFO("bluetooth input: {} resumed — re-anchoring", name);
      std::lock_guard<std::mutex> lk(m_);
      ++st_.restarts;
    }
    feed.push(il.data(), static_cast<size_t>(got), resumed);
    last_audio = now;
    std::lock_guard<std::mutex> lk(m_);
    st_.state = "streaming";
    st_.frames += static_cast<uint64_t>(got);
  }

  snd_pcm_drop(pcm);
  snd_pcm_close(pcm);
  net_.release_channels(static_cast<unsigned>(base), channels);
  {
    std::lock_guard<std::mutex> lk(m_);
    st_.input = -1;
    st_.state = "waiting";
  }
  // A device that vanished mid-read is not an error worth showing: it disconnected. A PCM that
  // failed while the device is still listed is, and it is retried after a pause.
  if (!pcm_ok && running_.load()) {
    std::string a, n;
    unsigned r = 0, c = 0;
    if (bt_.capture_source(&a, &n, &r, &c) && a == address) {
      set_state("error", "the capture PCM for " + name + " failed; retrying");
      return false;
    }
  }
  return true;
}

}  // namespace st
