#include "hdmi_out.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <time.h>

#include <algorithm>
#include <cmath>

#include "audio_engine.h"
#include "util/dsp.h"
#include "util/log.h"
#include "util/rt.h"

namespace st {

namespace {

constexpr unsigned kReopenDelayS = 5;

// How long the reader waits for the engine's next block before calling it a stall. Well inside
// what the HDMI buffer holds, so a block that is merely late costs nothing audible.
constexpr unsigned kStarveWaitMs = 40;

// A device that has taken no audio for this long is treated as gone, and reopened.
constexpr unsigned kWriteStallMs = 2000;

}  // namespace

HdmiPull hdmi_pull(const RingBuffer& ring, uint64_t* r_n, size_t frames, float* out) {
  if (frames == 0) return HdmiPull::Ok;
  const uint64_t head = ring.counter();
  if (*r_n > head || frames > head - *r_n) return HdmiPull::Starved;
  if (!ring.read_interleaved(*r_n, frames, out)) return HdmiPull::Lapped;
  *r_n += frames;
  return HdmiPull::Ok;
}

HdmiOutput::HdmiOutput(Control& ctl, const AudioEngine& engine, std::string device,
                       unsigned sample_rate)
    : ctl_(ctl),
      engine_(engine),
      ring_(kHdmiRingFrames, kHdmiChannels, kHdmiRingFrames / 8),
      device_(std::move(device)),
      sample_rate_(hdmi_rate_ok(sample_rate) ? sample_rate : kHdmiRateDefault) {}

HdmiOutput::~HdmiOutput() { stop(); }

void HdmiOutput::start() {
  std::lock_guard<std::mutex> life(life_m_);
  start_locked();
}

void HdmiOutput::stop() {
  std::lock_guard<std::mutex> life(life_m_);
  stop_locked();
}

void HdmiOutput::start_locked() {
  if (running_.load()) return;
  running_.store(true);
  thread_ = std::thread([this] { run(); });
}

void HdmiOutput::stop_locked() {
  running_.store(false);
  if (thread_.joinable()) thread_.join();
}

void HdmiOutput::configure(std::string device, unsigned sample_rate) {
  std::lock_guard<std::mutex> life(life_m_);
  const bool was_running = running_.load();
  stop_locked();
  {
    std::lock_guard<std::mutex> lk(m_);
    device_ = std::move(device);
    sample_rate_ = hdmi_rate_ok(sample_rate) ? sample_rate : kHdmiRateDefault;
    error_.clear();
  }
  if (was_running) start_locked();
}

std::string HdmiOutput::device() const {
  std::lock_guard<std::mutex> lk(m_);
  return device_;
}

unsigned HdmiOutput::sample_rate() const {
  std::lock_guard<std::mutex> lk(m_);
  return sample_rate_;
}

void HdmiOutput::set_error(std::string msg) {
  std::lock_guard<std::mutex> lk(m_);
  error_ = std::move(msg);
}

HdmiStatus HdmiOutput::status() const {
  HdmiStatus s;
  s.enabled = ctl_.hdmi.enabled.load();
  s.open = open_.load();
  s.playing = playing_.load();
  {
    std::lock_guard<std::mutex> lk(m_);
    s.device = device_;
    s.sample_rate = sample_rate_;
    s.error = error_;
  }
  s.device_rate = dev_rate_.load();
  s.period_frames = dev_period_.load();
  s.buffer_frames = dev_buffer_.load();
  s.xruns = xruns_.load();
  s.underruns = underruns_.load();
  s.overruns = overruns_.load();
  s.resyncs = resyncs_.load();
  s.latency_ms = latency_ms_.load();
  s.target_ms = target_ms_.load();
  s.ring_ms = ring_ms_.load();
  s.alsa_ms = alsa_ms_.load();
  s.trim_ppm = trim_ppm_.load();
  return s;
}

void HdmiOutput::sleep_ms(unsigned ms) const {
  timespec ts{static_cast<time_t>(ms / 1000), static_cast<long>((ms % 1000) * 1000000L)};
  nanosleep(&ts, nullptr);
}

void HdmiOutput::run() {
  make_realtime(kHdmiRtPriority, "hdmi");
  std::string logged;  // log a failure once, not every five seconds for as long as it lasts
  while (running_.load()) {
    if (!open_pcm()) {
      std::string err;
      {
        std::lock_guard<std::mutex> lk(m_);
        err = error_;
      }
      if (err != logged) {
        LOG_ERROR("{} — retrying every {} s", err, kReopenDelayS);
        logged = err;
      }
      for (unsigned i = 0; i < kReopenDelayS * 10 && running_.load(); ++i) sleep_ms(100);
      continue;
    }
    logged.clear();
    set_error({});
    const bool clean = stream();
    close_pcm();
    if (!clean && running_.load()) {
      LOG_WARN("hdmi: stream stopped — reopening in {} s", kReopenDelayS);
      for (unsigned i = 0; i < kReopenDelayS * 10 && running_.load(); ++i) sleep_ms(100);
    }
  }
  LOG_INFO("hdmi: stopped");
}

bool HdmiOutput::open_pcm() {
  std::string dev;
  unsigned want = 0;
  {
    std::lock_guard<std::mutex> lk(m_);
    dev = device_;
    want = sample_rate_;
  }
  rate_ = engine_.rate();

  // Non-blocking open: a busy hw device would otherwise park this thread in open() until whoever
  // holds it lets go, and stop() would wait just as long. Writes stay non-blocking too, paced by
  // snd_pcm_wait with a timeout, for the same reason.
  int err = snd_pcm_open(&pcm_, dev.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
  if (err < 0) {
    pcm_ = nullptr;
    std::string msg = "hdmi: cannot open " + dev + ": " + snd_strerror(err);
    if (err == -ENOENT || err == -ENODEV) {
      msg += " (the Pi's HDMI audio needs dtparam=audio=on in config.txt; it is hw:b1,0 with "
             "snd_bcm2835.enable_compat_alsa=0 on the kernel command line, hw:ALSA,1 without)";
    }
    set_error(msg);
    return false;
  }

  auto fail = [&](const char* what, int e) {
    set_error("hdmi: " + dev + ": " + what + ": " + snd_strerror(e));
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
    return false;
  };

  snd_pcm_hw_params_t* hw;
  snd_pcm_hw_params_alloca(&hw);
  if ((err = snd_pcm_hw_params_any(pcm_, hw)) < 0) return fail("hw_params_any", err);
  if ((err = snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
    return fail("set_access", err);
  if ((err = snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_S16_LE)) < 0)
    return fail("S16_LE not available", err);
  if ((err = snd_pcm_hw_params_set_channels(pcm_, hw, kHdmiChannels)) < 0)
    return fail("stereo not available", err);
  unsigned rate = want;
  if ((err = snd_pcm_hw_params_set_rate_near(pcm_, hw, &rate, nullptr)) < 0)
    return fail("set_rate", err);
  snd_pcm_uframes_t period = static_cast<snd_pcm_uframes_t>(rate) * kHdmiPeriodMs / 1000;
  if ((err = snd_pcm_hw_params_set_period_size_near(pcm_, hw, &period, nullptr)) < 0)
    return fail("set_period_size", err);
  snd_pcm_uframes_t buffer = period * kHdmiPeriods;
  if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm_, hw, &buffer)) < 0)
    return fail("set_buffer_size", err);
  if ((err = snd_pcm_hw_params(pcm_, hw)) < 0) return fail("hw_params", err);
  snd_pcm_hw_params_get_period_size(hw, &period, nullptr);
  snd_pcm_hw_params_get_buffer_size(hw, &buffer);
  if (buffer < 2 * period) return fail("buffer is under two periods", -EINVAL);

  snd_pcm_sw_params_t* sw;
  snd_pcm_sw_params_alloca(&sw);
  if ((err = snd_pcm_sw_params_current(pcm_, sw)) < 0) return fail("sw_params_current", err);
  // Starts itself once the prefill is in, which anchor() arranges to be exactly this much.
  snd_pcm_sw_params_set_start_threshold(pcm_, sw, buffer - period);
  snd_pcm_sw_params_set_avail_min(pcm_, sw, period);
  if ((err = snd_pcm_sw_params(pcm_, sw)) < 0) return fail("sw_params", err);

  std::string aerr;
  if (!asrc_.configure(kHdmiChannels, rate_, rate, &aerr)) {
    set_error("hdmi: converter: " + aerr);
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
    return false;
  }

  dev_rate_.store(rate);
  dev_period_.store(static_cast<unsigned>(period));
  dev_buffer_.store(static_cast<unsigned>(buffer));
  // One HDMI period's worth of engine frames per pass, so each pass hands the driver about one
  // period and the driver's own pace sets the loop's.
  chunk_in_ = static_cast<size_t>(std::llround(static_cast<double>(period) * rate_ / rate));
  in_.assign(chunk_in_ * kHdmiChannels, 0.0f);
  open_.store(true);
  if (rate != want) LOG_WARN("hdmi: {} asked for {} Hz, driver chose {}", dev, want, rate);
  LOG_INFO("hdmi: {} open: {} Hz, S16_LE, {} ch, period {}, buffer {}", dev, rate, kHdmiChannels,
           static_cast<unsigned>(period), static_cast<unsigned>(buffer));
  return true;
}

void HdmiOutput::close_pcm() {
  if (pcm_) {
    snd_pcm_drop(pcm_);
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
  }
  open_.store(false);
  playing_.store(false);
  dev_rate_.store(0);
}

long HdmiOutput::write_all(const int16_t* buf, size_t frames) {
  size_t done = 0;
  unsigned waited = 0;
  while (done < frames) {
    if (!running_.load()) return static_cast<long>(done);
    const snd_pcm_sframes_t w =
        snd_pcm_writei(pcm_, buf + done * kHdmiChannels, frames - done);
    if (w == -EAGAIN) {
      const int r = snd_pcm_wait(pcm_, 100);
      if (r < 0) return r;
      if (r == 0 && (waited += 100) >= kWriteStallMs) return -ETIMEDOUT;
      continue;
    }
    if (w < 0) return static_cast<long>(w);
    done += static_cast<size_t>(w);
    waited = 0;
  }
  return static_cast<long>(done);
}

bool HdmiOutput::wait_for_engine() {
  for (;;) {
    if (!running_.load()) return false;
    const uint64_t h0 = ring_.counter();
    sleep_ms(50);
    if (ring_.counter() > h0 && ctl_.anchor.estimate(mono_ns(), rate_) != 0) return true;
  }
}

bool HdmiOutput::anchor() {
  playing_.store(false);
  if (!wait_for_engine()) return false;

  snd_pcm_drop(pcm_);
  const int err = snd_pcm_prepare(pcm_);
  if (err < 0) {
    set_error(std::string("hdmi: prepare: ") + snd_strerror(err));
    return false;
  }
  asrc_.reset();
  servo_.reset();

  // Silence up to the start threshold, which starts the device.
  const unsigned dev_rate = dev_rate_.load();
  const unsigned period = dev_period_.load();
  const unsigned buffer = dev_buffer_.load();
  pcm_buf_.assign(static_cast<size_t>(buffer - period) * kHdmiChannels, 0);
  const long w = write_all(pcm_buf_.data(), buffer - period);
  if (w < 0) {
    set_error(std::string("hdmi: prefill: ") + snd_strerror(static_cast<int>(w)));
    return false;
  }
  if (snd_pcm_state(pcm_) == SND_PCM_STATE_PREPARED) snd_pcm_start(pcm_);

  // The latency held from here on: a few engine periods of slack in the ring, so the next chunk
  // is always already written, plus the driver's whole buffer. Recomputed per anchor because the
  // engine's period is whatever its driver last agreed to.
  const double ring_lag = static_cast<double>(kHdmiRingLagPeriods) * engine_.period();
  target_ = ring_lag + static_cast<double>(buffer) * rate_ / dev_rate;
  target_ms_.store(static_cast<float>(1000.0 * target_ / rate_));

  // Place the reader so the first measurement already reads the target: after the first pass it
  // will have taken one chunk, and the driver will hold about a full buffer.
  const uint64_t est = ctl_.anchor.estimate(mono_ns(), rate_);
  const uint64_t now = std::min(est, ring_.counter());
  const uint64_t back = static_cast<uint64_t>(ring_lag) + chunk_in_;
  r_n_ = now > back ? now - back : 0;
  playing_.store(true);
  return true;
}

bool HdmiOutput::stream() {
  bool need_anchor = true;
  uint64_t last_ns = 0;
  uint64_t settled_ns = 0;  // readings before this are not trusted

  while (running_.load()) {
    if (need_anchor) {
      if (!anchor()) return !running_.load();
      need_anchor = false;
      last_ns = mono_ns();
      settled_ns = last_ns + static_cast<uint64_t>(kHdmiSettleS * 1e9);
    }

    HdmiPull p = hdmi_pull(ring_, &r_n_, chunk_in_, in_.data());
    // The engine publishes a period at a time, so the next chunk can be a moment away. Wait for
    // it in small steps: this is a FIFO thread, and spinning would starve everything below it.
    for (unsigned waited = 0; p == HdmiPull::Starved && waited < kStarveWaitMs; ++waited) {
      if (!running_.load()) return true;
      sleep_ms(1);
      p = hdmi_pull(ring_, &r_n_, chunk_in_, in_.data());
    }
    if (p == HdmiPull::Starved) {
      // The engine has stopped: its card is reopening, typically. Nothing to play, and when it
      // comes back its counter will not have moved on in step with this clock — so start over.
      underruns_.fetch_add(1);
      LOG_WARN("hdmi: the engine stopped supplying audio — re-anchoring once it resumes");
      snd_pcm_drop(pcm_);
      need_anchor = true;
      continue;
    }
    if (p == HdmiPull::Lapped) {
      overruns_.fetch_add(1);
      LOG_WARN("hdmi: fell a whole ring behind the engine — re-anchoring");
      need_anchor = true;
      continue;
    }

    out_.clear();
    std::string aerr;
    const size_t got = asrc_.process(in_.data(), chunk_in_, servo_.trim, &out_, &aerr);
    if (!aerr.empty()) {
      set_error("hdmi: converter: " + aerr);
      return false;
    }
    pcm_buf_.resize(got * kHdmiChannels);
    for (size_t i = 0; i < pcm_buf_.size(); ++i) pcm_buf_[i] = float_to_s16(out_[i]);

    if (got) {
      const long w = write_all(pcm_buf_.data(), got);
      if (w == -EPIPE || w == -ESTRPIPE) {
        xruns_.fetch_add(1);
        LOG_WARN("hdmi: xrun ({}) — re-anchoring", snd_strerror(static_cast<int>(w)));
        if (snd_pcm_recover(pcm_, static_cast<int>(w), 1) < 0) return false;
        need_anchor = true;
        continue;
      }
      if (w < 0) {
        set_error(std::string("hdmi: write: ") + snd_strerror(static_cast<int>(w)));
        return false;
      }
    }

    // Measured right after the write, every pass at the same point in the cycle. The ring term is
    // taken against the card's position interpolated to now, not its last block boundary, and the
    // driver term moves opposite to it as a write goes in: the sum does not see either sawtooth.
    snd_pcm_sframes_t delay = 0;
    if (snd_pcm_delay(pcm_, &delay) < 0) delay = 0;
    const uint64_t now_ns = mono_ns();
    const uint64_t est = ctl_.anchor.estimate(now_ns, rate_);
    const double ring_part =
        static_cast<double>(static_cast<int64_t>(est) - static_cast<int64_t>(r_n_));
    const double alsa_part = static_cast<double>(delay) * rate_ / dev_rate_.load();
    const double dt_s = static_cast<double>(now_ns - last_ns) * 1e-9;
    last_ns = now_ns;
    ring_ms_.store(static_cast<float>(1000.0 * ring_part / rate_));
    alsa_ms_.store(static_cast<float>(1000.0 * alsa_part / rate_));
    // The converter free-runs until the driver has settled; the servo then starts from a reading
    // that means something.
    if (now_ns < settled_ns) continue;
    servo_.update(ring_part + alsa_part, dt_s, target_, rate_);

    latency_ms_.store(static_cast<float>(1000.0 * servo_.filter.avg / rate_));
    trim_ppm_.store(static_cast<float>((servo_.trim - 1.0) * 1e6));

    if (servo_.adrift(target_, kHdmiResyncS * rate_)) {
      resyncs_.fetch_add(1);
      LOG_WARN("hdmi: latency {:.1f} ms off target, past what the trim can pull back — "
               "re-anchoring",
               1000.0 * (servo_.filter.avg - target_) / rate_);
      need_anchor = true;
    }
  }
  return true;
}

}  // namespace st
