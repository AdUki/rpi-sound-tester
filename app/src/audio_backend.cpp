#include "audio_backend.h"

#include "channel_layout.h"

#include <alsa/asoundlib.h>

#include <algorithm>

#include "util/dsp.h"
#include "util/log.h"

namespace st {

void AudioBackend::set_error(std::string msg) {
  std::lock_guard<std::mutex> lock(err_m_);
  last_error_ = std::move(msg);
}

std::string AudioBackend::error() const {
  std::lock_guard<std::mutex> lock(err_m_);
  return last_error_;
}

const char* backend_strerror(int err) { return snd_strerror(err); }

// ---- Interleaved S32 frames ---------------------------------------------------------------------

SlotMaps snapshot_slot_maps(const Control& ctl, unsigned capture_channels) {
  SlotMaps m;
  for (unsigned c = 0; c < kInputs; ++c) {
    const unsigned slot = ctl.input_map[c].load(std::memory_order_relaxed);
    m.in[c] = slot < capture_channels ? slot : c;
  }
  for (unsigned c = 0; c < kOutputs; ++c) {
    const unsigned slot = ctl.output_map[c].load(std::memory_order_relaxed);
    m.out[c] = slot < kOutputs ? slot : c;
  }
  return m;
}

void s32_to_inputs(const int32_t* raw, size_t frames, unsigned channels, const SlotMaps& maps,
                   float* in_all) {
  const unsigned stride = st::channels().total();
  for (unsigned c = 0; c < kInputs; ++c) {
    const int32_t* src = raw + maps.in[c];
    float* dst = in_all + c;
    for (size_t i = 0; i < frames; ++i) dst[i * stride] = s32_to_float(src[i * channels]);
  }
}

void outputs_to_s32(const float* out8, size_t frames, const SlotMaps& maps, int32_t* raw) {
  for (unsigned c = 0; c < kOutputs; ++c) {
    const float* src = out8 + c;
    int32_t* dst = raw + maps.out[c];
    for (size_t i = 0; i < frames; ++i) dst[i * kOutputs] = float_to_s32(src[i * kOutputs]);
  }
}

// ---- AlsaLinkedBackend --------------------------------------------------------------------------

AlsaLinkedBackend::AlsaLinkedBackend(const Control& ctl, const EngineOptions& opt, Clock& clock)
    : ctl_(ctl), clock_(clock), opt_(opt), cap_ch_(opt.capture_channels) {}

AlsaLinkedBackend::~AlsaLinkedBackend() { close_alsa(); }

bool AlsaLinkedBackend::open() {
  if (!open_alsa()) return false;
  init_mixer();
  // The driver may have renegotiated the period, so size the buffers only once it has agreed to
  // something.
  raw_in_.assign(static_cast<size_t>(opt_.period) * cap_ch_, 0);
  raw_out_.assign(static_cast<size_t>(opt_.period) * kOutputs, 0);
  return true;
}

void AlsaLinkedBackend::close() { close_alsa(); }

bool AlsaLinkedBackend::start() { return prefill_and_start(); }

BackendShape AlsaLinkedBackend::shape() const {
  return {opt_.rate, opt_.period, cap_ch_, kOutputs, "S32_LE"};
}

bool AlsaLinkedBackend::configure(snd_pcm_t* pcm, unsigned channels, const char* what) {
  snd_pcm_hw_params_t* hw;
  snd_pcm_hw_params_alloca(&hw);

  int err = snd_pcm_hw_params_any(pcm, hw);
  if (err < 0) {
    set_error(std::string(what) + ": hw_params_any: " + snd_strerror(err));
    return false;
  }

  err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (err < 0) {
    set_error(std::string(what) + ": set_access: " + snd_strerror(err));
    return false;
  }

  err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE);
  if (err < 0) {
    set_error(std::string(what) + ": S32_LE not available: " + snd_strerror(err));
    return false;
  }

  err = snd_pcm_hw_params_set_channels(pcm, hw, channels);
  if (err < 0) {
    set_error(std::string(what) + ": " + std::to_string(channels) +
                  " channels not available: " + snd_strerror(err));
    return false;
  }

  err = snd_pcm_hw_params_set_rate(pcm, hw, opt_.rate, 0);
  if (err < 0) {
    set_error(std::string(what) + ": rate " + std::to_string(opt_.rate) +
                  " not available: " + snd_strerror(err));
    return false;
  }

  snd_pcm_uframes_t period = opt_.period;
  err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr);
  if (err < 0) {
    set_error(std::string(what) + ": set_period_size: " + snd_strerror(err));
    return false;
  }

  snd_pcm_uframes_t buffer = static_cast<snd_pcm_uframes_t>(opt_.period) * opt_.periods;
  err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
  if (err < 0) {
    set_error(std::string(what) + ": set_buffer_size: " + snd_strerror(err));
    return false;
  }

  err = snd_pcm_hw_params(pcm, hw);
  if (err < 0) {
    set_error(std::string(what) + ": hw_params: " + snd_strerror(err));
    return false;
  }

  if (period != opt_.period) {
    LOG_WARN("{}: driver chose period {} (asked {})", what, static_cast<unsigned>(period),
             opt_.period);
    opt_.period = static_cast<unsigned>(period);
  }
  LOG_INFO("{}: {} ch, {} Hz, S32_LE, period {}, buffer {}", what, channels, opt_.rate,
           static_cast<unsigned>(period), static_cast<unsigned>(buffer));

  snd_pcm_sw_params_t* sw;
  snd_pcm_sw_params_alloca(&sw);
  err = snd_pcm_sw_params_current(pcm, sw);
  if (err < 0) {
    set_error(std::string(what) + ": sw_params_current: " + snd_strerror(err));
    return false;
  }
  // Nothing may auto-start: the linked group is started once, explicitly, so capture and
  // playback share sample zero.
  snd_pcm_sw_params_set_start_threshold(pcm, sw, 0x7fffffff);
  snd_pcm_sw_params_set_avail_min(pcm, sw, opt_.period);
  err = snd_pcm_sw_params(pcm, sw);
  if (err < 0) {
    set_error(std::string(what) + ": sw_params: " + snd_strerror(err));
    return false;
  }
  return true;
}

bool AlsaLinkedBackend::open_alsa() {
  // Give the device node this long to appear per attempt; the engine retries the whole open
  // forever on the same cadence.
  const unsigned deadline_ms = kReopenDelayS * 1000;
  unsigned waited = 0;
  int err = 0;

  // The codec probes asynchronously after the overlay loads, so at boot the device may not
  // exist yet for the first seconds.
  for (;;) {
    err = snd_pcm_open(&capture_, opt_.device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err >= 0) break;
    if (waited >= deadline_ms) {
      set_error("cannot open capture device " + opt_.device + ": " + snd_strerror(err));
      LOG_ERROR("{}", error());
      return false;
    }
    if (waited == 0) LOG_WARN("waiting for {} to appear ({})", opt_.device, snd_strerror(err));
    clock_.sleep_until(clock_.now_ns() + 500000000ull);  // 500 ms
    waited += 500;
  }

  err = snd_pcm_open(&playback_, opt_.device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
  if (err < 0) {
    set_error("cannot open playback device " + opt_.device + ": " + snd_strerror(err));
    LOG_ERROR("{}", error());
    close_alsa();
    return false;
  }

  cap_ch_ = opt_.capture_channels;
  if (!configure(capture_, cap_ch_, "capture")) {
    if (cap_ch_ == kInputs) {
      close_alsa();
      return false;
    }
    // The driver only widens capture to 8 TDM slots while a stream is open; if it refuses,
    // the plain 6 ADC channels always work.
    LOG_WARN("{} — retrying capture with {} channels", error(), kInputs);
    snd_pcm_close(capture_);
    capture_ = nullptr;
    err = snd_pcm_open(&capture_, opt_.device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
      set_error(std::string("reopen capture: ") + snd_strerror(err));
      close_alsa();
      return false;
    }
    cap_ch_ = kInputs;
    if (!configure(capture_, cap_ch_, "capture")) {
      close_alsa();
      return false;
    }
  }

  if (!configure(playback_, kOutputs, "playback")) {
    close_alsa();
    return false;
  }

  err = snd_pcm_link(capture_, playback_);
  if (err < 0) {
    // Without a linked start the two streams have an unknown offset between them.
    set_error(std::string("snd_pcm_link failed: ") + snd_strerror(err));
    LOG_ERROR("{}", error());
    close_alsa();
    return false;
  }
  LOG_INFO("capture and playback linked: one clock, one start");
  return true;
}

void AlsaLinkedBackend::close_alsa() {
  if (capture_) {
    snd_pcm_close(capture_);
    capture_ = nullptr;
  }
  if (playback_) {
    snd_pcm_close(playback_);
    playback_ = nullptr;
  }
}

bool AlsaLinkedBackend::prefill_and_start() {
  int err = snd_pcm_prepare(capture_);
  if (err < 0) {
    set_error(std::string("prepare: ") + snd_strerror(err));
    return false;
  }

  // Three periods of silence: once readi starts returning, playback still holds two
  // periods, giving the loop ~21 ms of jitter budget before it underruns.
  std::fill(raw_out_.begin(), raw_out_.end(), 0);
  for (unsigned i = 0; i < 3; ++i) {
    snd_pcm_sframes_t w = snd_pcm_writei(playback_, raw_out_.data(), opt_.period);
    if (w < 0) {
      set_error(std::string("prefill writei: ") + snd_strerror(static_cast<int>(w)));
      return false;
    }
  }

  err = snd_pcm_start(capture_);
  if (err < 0) {
    set_error(std::string("start: ") + snd_strerror(err));
    return false;
  }
  return true;
}

bool AlsaLinkedBackend::recover(int err) {
  int r = snd_pcm_recover(capture_, err, 1);
  if (r < 0) {
    LOG_ERROR("capture recover failed: {}", snd_strerror(r));
    return false;
  }
  const snd_pcm_state_t ps = snd_pcm_state(playback_);
  if (ps != SND_PCM_STATE_PREPARED && ps != SND_PCM_STATE_RUNNING) {
    r = snd_pcm_prepare(playback_);
    if (r < 0) {
      LOG_ERROR("playback prepare failed: {}", snd_strerror(r));
      return false;
    }
  }
  return prefill_and_start();
}

void AlsaLinkedBackend::init_mixer() {
  std::string card = opt_.device;
  const size_t comma = card.find(',');
  if (comma != std::string::npos) card = card.substr(0, comma);

  snd_mixer_t* mixer = nullptr;
  if (snd_mixer_open(&mixer, 0) < 0) return;
  if (snd_mixer_attach(mixer, card.c_str()) < 0 ||
      snd_mixer_selem_register(mixer, nullptr, nullptr) < 0 || snd_mixer_load(mixer) < 0) {
    snd_mixer_close(mixer);
    LOG_WARN("no mixer on {} — leaving the codec at driver defaults", card);
    return;
  }

  // Start from a known codec state rather than whatever the driver left behind, and "known"
  // means unity gain — 0 dB.
  //
  // Unity is not the control's maximum. On the CS42448 the DAC volume range tops out at 0 dB
  // but the ADC range tops out at +24 dB, so driving both to their raw maximum leaves all six
  // ADCs at +24 dB (VOLAIN1..6 = 0x30, read back from the codec on the running board) and every
  // reported level is 24 dB too high. So ask ALSA for the 0 dB point from the control's own TLV
  // instead of inferring it from the raw range. Fall back to the maximum only if a control
  // carries no dB information at all, and log the dB actually achieved so the codec state is
  // visible in the journal.
  for (snd_mixer_elem_t* e = snd_mixer_first_elem(mixer); e; e = snd_mixer_elem_next(e)) {
    const char* name = snd_mixer_selem_get_name(e);
    long lo = 0, hi = 0, raw = 0, mdb = 0;

    if (snd_mixer_selem_has_playback_volume(e)) {
      if (snd_mixer_selem_set_playback_dB_all(e, 0, 0) < 0) {
        snd_mixer_selem_get_playback_volume_range(e, &lo, &hi);
        snd_mixer_selem_set_playback_volume_all(e, hi);
        LOG_WARN("mixer: {} has no dB scale — using raw maximum {}", name ? name : "?", hi);
      }
      snd_mixer_selem_get_playback_volume(e, SND_MIXER_SCHN_FRONT_LEFT, &raw);
      const bool has_db = snd_mixer_selem_get_playback_dB(e, SND_MIXER_SCHN_FRONT_LEFT, &mdb) == 0;
      LOG_INFO("mixer: {} playback -> raw {} ({:.1f} dB)", name ? name : "?", raw,
               has_db ? static_cast<double>(mdb) / 100.0 : 0.0);
    }

    if (snd_mixer_selem_has_capture_volume(e)) {
      if (snd_mixer_selem_set_capture_dB_all(e, 0, 0) < 0) {
        snd_mixer_selem_get_capture_volume_range(e, &lo, &hi);
        snd_mixer_selem_set_capture_volume_all(e, hi);
        LOG_WARN("mixer: {} has no dB scale — using raw maximum {}", name ? name : "?", hi);
      }
      snd_mixer_selem_get_capture_volume(e, SND_MIXER_SCHN_FRONT_LEFT, &raw);
      const bool has_db = snd_mixer_selem_get_capture_dB(e, SND_MIXER_SCHN_FRONT_LEFT, &mdb) == 0;
      LOG_INFO("mixer: {} capture -> raw {} ({:.1f} dB)", name ? name : "?", raw,
               has_db ? static_cast<double>(mdb) / 100.0 : 0.0);
    }
  }
  snd_mixer_close(mixer);
}

long AlsaLinkedBackend::read_block(uint64_t, float* in_all) {
  const snd_pcm_sframes_t got = snd_pcm_readi(capture_, raw_in_.data(), opt_.period);
  if (got < 0) return got;

  // Snapshot the channel maps once per block; loading an atomic per sample would keep the
  // conversion loops scalar. The write that follows plays through the same snapshot.
  maps_ = snapshot_slot_maps(ctl_, cap_ch_);
  s32_to_inputs(raw_in_.data(), static_cast<size_t>(got), cap_ch_, maps_, in_all);
  return got;
}

long AlsaLinkedBackend::write_block(uint64_t, const float* out8, size_t frames) {
  outputs_to_s32(out8, frames, maps_, raw_out_.data());

  size_t written = 0;
  while (written < frames) {
    snd_pcm_sframes_t w =
        snd_pcm_writei(playback_, raw_out_.data() + written * kOutputs, frames - written);
    if (w < 0) return w;
    written += static_cast<size_t>(w);
  }
  return static_cast<long>(written);
}

// ---- TimerBackend -------------------------------------------------------------------------------

TimerBackend::TimerBackend(const EngineOptions& opt, Clock& clock)
    : opt_(opt),
      clock_(clock),
      // Whole nanoseconds, the fraction dropped every block: the simulated card runs some 60 ppb
      // fast, which nothing downstream can tell from a real card's own drift.
      block_ns_(static_cast<uint64_t>(1e9 * static_cast<double>(opt.period) /
                                      static_cast<double>(opt.rate))),
      sim_delay_len_(static_cast<size_t>(opt.period) * 4 + kInputs * opt.sim_stagger + 16) {
  sim_delay_.assign(sim_delay_len_ * kInputs, 0.0f);
}

bool TimerBackend::open() {
  if (opt_.sim) {
    LOG_INFO("simulator: {} Hz, period {}, virtual loopback OUT->IN (stagger {} frames/ch)",
             opt_.rate, opt_.period, opt_.sim_stagger);
  } else {
    LOG_INFO("no engine card: a timer paces the engine at {} Hz, period {}", opt_.rate,
             opt_.period);
  }
  return true;
}

bool TimerBackend::start() {
  seed_ = 0x9e3779b97f4a7c15ull;
  next_ns_ = clock_.now_ns();
  return true;
}

BackendShape TimerBackend::shape() const {
  if (!opt_.sim) return {opt_.rate, opt_.period, 0, 0, "none (timer)"};
  return {opt_.rate, opt_.period, kInputs, kOutputs, "float32 (simulated)"};
}

long TimerBackend::read_block(uint64_t n, float* in_all) {
  const size_t period = opt_.period;
  if (!opt_.sim) return static_cast<long>(period);  // no card, so nothing captured
  const unsigned stride = st::channels().total();
  uint64_t seed = seed_;
  auto noise = [&seed] { return xorshift_white(seed); };

  // The simulated card is a loopback: output channel c reappears on input channel c
  // delayed by period + c*stagger frames.
  for (size_t i = 0; i < period; ++i) {
    for (unsigned c = 0; c < kInputs; ++c) {
      const size_t delay = period + static_cast<size_t>(c) * opt_.sim_stagger;
      const size_t pos = (n + i + sim_delay_len_ - delay) % sim_delay_len_;
      in_all[i * stride + c] = sim_delay_[pos * kInputs + c] + 3e-5f * noise();
    }
  }
  seed_ = seed;
  return static_cast<long>(period);
}

long TimerBackend::write_block(uint64_t n, const float* out8, size_t frames) {
  for (size_t i = 0; opt_.sim && i < frames; ++i) {
    const size_t pos = (n + i) % sim_delay_len_;
    for (unsigned c = 0; c < kInputs; ++c) sim_delay_[pos * kInputs + c] = out8[i * kOutputs + c];
  }

  next_ns_ += block_ns_;
  clock_.sleep_until(next_ns_);
  return static_cast<long>(frames);
}

}  // namespace st
