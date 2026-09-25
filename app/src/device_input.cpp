#include "device_input.h"

#include <errno.h>
#include <time.h>

#include <cmath>

#include "audio_engine.h"
#include "util/log.h"
#include "util/rt.h"

namespace st {

namespace {

// A device that has delivered nothing for this long is treated as gone, and reopened.
constexpr unsigned kReadStallMs = 2000;

size_t timeline_frames(double rate) {
  const size_t want = static_cast<size_t>(rate * kNetTimelineMs / 1000.0);
  size_t p = 1;
  while (p < want) p <<= 1;
  return p;
}

void sleep_ms(unsigned ms) {
  timespec ts{static_cast<time_t>(ms / 1000), static_cast<long>((ms % 1000) * 1000000L)};
  nanosleep(&ts, nullptr);
}

}  // namespace

DeviceInput::DeviceInput(std::string id, std::string alsa, unsigned channels, Control& ctl,
                         const AudioEngine& engine)
    : id_(std::move(id)),
      alsa_(std::move(alsa)),
      channels_(channels),
      ctl_(ctl),
      engine_(engine),
      clock_(engine.clock()) {
  for (unsigned c = 0; c < channels_; ++c)
    timelines_.push_back(std::make_unique<NetTimeline>(timeline_frames(engine.rate())));
}

DeviceInput::~DeviceInput() { stop(); }

void DeviceInput::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void DeviceInput::stop() {
  running_.store(false);
  if (thread_.joinable()) thread_.join();
}

void DeviceInput::set_error(std::string msg) {
  std::lock_guard<std::mutex> lk(m_);
  error_ = std::move(msg);
}

DeviceInputStatus DeviceInput::status() const {
  DeviceInputStatus s;
  s.open = open_.load();
  s.capturing = capturing_.load();
  s.device = alsa_;
  s.channels = channels_;
  s.device_rate = dev_rate_pub_.load();
  s.format = format_pub_.load();
  s.xruns = xruns_.load();
  s.resyncs = resyncs_.load();
  s.late = late_.load();
  s.trim_ppm = trim_ppm_.load();
  std::lock_guard<std::mutex> lk(m_);
  s.error = error_;
  return s;
}

void DeviceInput::run() {
  make_realtime(kSinkRtPriority, "input");
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
      LOG_WARN("{}: capture stopped — reopening in {} s", id_, kReopenDelayS);
      for (unsigned i = 0; i < kReopenDelayS * 10 && running_.load(); ++i) sleep_ms(100);
    }
  }
}

bool DeviceInput::open_pcm() {
  rate_ = engine_.rate();
  int err = snd_pcm_open(&pcm_, alsa_.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
  if (err < 0) {
    pcm_ = nullptr;
    set_error(id_ + ": cannot open " + alsa_ + ": " + snd_strerror(err));
    return false;
  }
  auto fail = [&](const std::string& what, int e) {
    set_error(id_ + ": " + alsa_ + ": " + what + ": " + snd_strerror(e));
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
    return false;
  };

  snd_pcm_hw_params_t* hw;
  snd_pcm_hw_params_alloca(&hw);
  if ((err = snd_pcm_hw_params_any(pcm_, hw)) < 0) return fail("hw_params_any", err);
  if ((err = snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
    return fail("set_access", err);
  if ((err = snd_pcm_hw_params_set_channels(pcm_, hw, channels_)) < 0)
    return fail(std::to_string(channels_) + " channels not available", err);
  // The engine's rate if the device has it, so the converter only has the drift to take out.
  unsigned rate = static_cast<unsigned>(rate_);
  if ((err = snd_pcm_hw_params_set_rate_near(pcm_, hw, &rate, nullptr)) < 0)
    return fail("set_rate", err);
  bool have_format = false;
  for (const PcmFormat f : kPcmFormats) {
    if (snd_pcm_hw_params_test_format(pcm_, hw, pcm_alsa_format(f)) == 0) {
      format_ = f;
      have_format = true;
      break;
    }
  }
  if (!have_format) return fail("none of S32_LE, S24_LE, S24_3LE, S16_LE available", -EINVAL);
  if ((err = snd_pcm_hw_params_set_format(pcm_, hw, pcm_alsa_format(format_))) < 0)
    return fail("set_format", err);
  snd_pcm_uframes_t period = static_cast<snd_pcm_uframes_t>(rate) * kInputPeriodMs / 1000;
  if ((err = snd_pcm_hw_params_set_period_size_near(pcm_, hw, &period, nullptr)) < 0)
    return fail("set_period_size", err);
  snd_pcm_uframes_t buffer = period * kInputPeriods;
  if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm_, hw, &buffer)) < 0)
    return fail("set_buffer_size", err);
  if ((err = snd_pcm_hw_params(pcm_, hw)) < 0) return fail("hw_params", err);
  snd_pcm_hw_params_get_period_size(hw, &period, nullptr);
  snd_pcm_hw_params_get_buffer_size(hw, &buffer);

  snd_pcm_sw_params_t* sw;
  snd_pcm_sw_params_alloca(&sw);
  if ((err = snd_pcm_sw_params_current(pcm_, sw)) < 0) return fail("sw_params_current", err);
  snd_pcm_sw_params_set_avail_min(pcm_, sw, period);
  if ((err = snd_pcm_sw_params(pcm_, sw)) < 0) return fail("sw_params", err);

  std::string aerr;
  if (!asrc_.configure(channels_, rate, rate_, &aerr)) return fail("converter: " + aerr, -EINVAL);

  dev_rate_ = rate;
  period_ = period;
  raw_.assign(period_ * channels_ * pcm_format_bytes(format_), 0);
  in_.assign(period_ * channels_, 0.0f);
  if ((err = snd_pcm_prepare(pcm_)) < 0) return fail("prepare", err);
  if ((err = snd_pcm_start(pcm_)) < 0) return fail("start", err);

  dev_rate_pub_.store(rate);
  format_pub_.store(pcm_format_name(format_));
  open_.store(true);
  LOG_INFO("{}: {} open for capture: {} ch, {} Hz, {}, period {}, buffer {}", id_, alsa_,
           channels_, rate, pcm_format_name(format_), static_cast<unsigned>(period),
           static_cast<unsigned>(buffer));
  return true;
}

void DeviceInput::close_pcm() {
  if (pcm_) {
    snd_pcm_drop(pcm_);
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
  }
  open_.store(false);
  capturing_.store(false);
  dev_rate_pub_.store(0);
  format_pub_.store("");
}

bool DeviceInput::stream() {
  const uint64_t max_lead = static_cast<uint64_t>(rate_ * kNetTimelineMs / 2000.0);
  bool anchored = false;
  uint64_t out_next = 0;     // where the frame after the last one converted goes, on the axis
  uint64_t last_ns = 0;
  uint64_t settled_ns = 0;   // readings before this are not trusted
  unsigned waited = 0;

  while (running_.load()) {
    const int w = snd_pcm_wait(pcm_, 100);
    if (w == 0) {
      if ((waited += 100) >= kReadStallMs) {
        set_error(id_ + ": no audio for " + std::to_string(kReadStallMs / 1000) + " s");
        return false;
      }
      continue;
    }
    const snd_pcm_sframes_t got =
        w < 0 ? w : snd_pcm_readi(pcm_, raw_.data(), static_cast<snd_pcm_uframes_t>(period_));
    if (got == -EAGAIN) continue;
    if (got == -ENODEV || got == -EBADFD) {  // unplugged: open it again when it is back
      set_error(id_ + ": " + snd_strerror(static_cast<int>(got)));
      return false;
    }
    if (got < 0) {
      // An overrun: frames were lost, so the placement starts over from the next read.
      xruns_.fetch_add(1);
      capturing_.store(false);
      anchored = false;
      LOG_WARN("{}: capture xrun ({})", id_, snd_strerror(static_cast<int>(got)));
      if (snd_pcm_recover(pcm_, static_cast<int>(got), 1) < 0 || snd_pcm_start(pcm_) < 0) {
        set_error(id_ + ": " + snd_strerror(static_cast<int>(got)));
        return false;
      }
      continue;
    }
    waited = 0;
    if (got == 0) continue;
    const size_t frames = static_cast<size_t>(got);
    pcm_to_float(raw_.data(), frames * channels_, format_, in_.data());

    // Where the frame after the last one just read sits on the engine's axis: the engine's position
    // now, less what the driver has captured since and still holds.
    snd_pcm_sframes_t queued = 0;
    if (snd_pcm_delay(pcm_, &queued) < 0) queued = 0;
    const uint64_t now_ns = clock_.now_ns();
    const uint64_t engine_now = ctl_.anchor.estimate(now_ns, rate_);
    if (engine_now == 0) {  // the engine is not running: nothing to place it against
      anchored = false;
      capturing_.store(false);
      continue;
    }
    const double scale = rate_ / static_cast<double>(dev_rate_);
    const double place = static_cast<double>(engine_now) - static_cast<double>(queued) * scale;

    double trim = 1.0;
    if (anchored) {
      // How far ahead of where it belongs the converted audio has got. Held at zero by trimming the
      // converter's ratio, the same loop as the network input's and the sinks'.
      const double lead = static_cast<double>(out_next) -
                          (place - static_cast<double>(frames) * scale);
      const double dt_s = static_cast<double>(now_ns - last_ns) * 1e-9;
      if (now_ns >= settled_ns) {
        const double avg = filter_.update(lead, dt_s, kNetLeadFilterTauS);
        trim = asrc_trim(avg, 0.0, rate_, kAsrcTauS, kAsrcTrimMax);
        if (filter_.primed && std::fabs(avg) > kSinkResyncS * rate_) {
          resyncs_.fetch_add(1);
          LOG_WARN("{}: {:.1f} ms off, past what the trim can pull back — re-anchoring", id_,
                   1000.0 * avg / rate_);
          anchored = false;
        }
      }
    }
    last_ns = now_ns;
    if (!anchored) {
      asrc_.reset();
      filter_.reset();
      trim = 1.0;
      settled_ns = now_ns + static_cast<uint64_t>(kSinkSettleS * 1e9);
    }

    out_.clear();
    std::string aerr;
    const size_t m = asrc_.process(in_.data(), frames, trim, &out_, &aerr);
    if (!aerr.empty()) {
      set_error(id_ + ": converter: " + aerr);
      return false;
    }
    if (!anchored) {
      // The first converted frames end where the frames just read end; the converter's own delay
      // is folded into that, once, as a constant.
      out_next = static_cast<uint64_t>(std::llround(place));
      anchored = true;
      capturing_.store(true);
      if (m > out_next) continue;
      out_next -= m;
    }
    if (m == 0) continue;
    trim_ppm_.store(static_cast<float>((trim - 1.0) * 1e6));

    // The audio thread reads a capture delay behind the engine's position, and must find these
    // frames there.
    // Until the engine has taken its first block with this input bound, it has not yet held the axis
    // back to make room for it, and every frame would be refused as late.
    const uint64_t delay = ctl_.capture_delay.load(std::memory_order_relaxed);
    if (delay == 0) {
      out_next += m;
      continue;
    }
    const uint64_t reader = engine_now > delay ? engine_now - delay : 0;
    const uint64_t guard = 2ull * engine_.period();
    bool late = false;
    chan_.resize(m);
    for (unsigned c = 0; c < channels_; ++c) {
      for (size_t i = 0; i < m; ++i) chan_[i] = out_[i * channels_ + c];
      late |= timelines_[c]->write(out_next, chan_.data(), m, reader, guard, max_lead) !=
              NetTimeline::Write::Ok;
    }
    if (late) late_.fetch_add(1);
    out_next += m;
  }
  return true;
}

}  // namespace st
