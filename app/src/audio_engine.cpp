#include "audio_engine.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <time.h>

#include <algorithm>
#include <cmath>

#include "util/dsp.h"
#include "util/log.h"

namespace st {

namespace {

constexpr int kRtPriority = 80;
constexpr size_t kAudioStackBytes = 1024 * 1024;
constexpr size_t kPrefaultBytes = 256 * 1024;
constexpr unsigned kReopenDelayS = 5;

void sleep_ms(unsigned ms) {
  timespec ts{static_cast<time_t>(ms / 1000), static_cast<long>((ms % 1000) * 1000000L)};
  nanosleep(&ts, nullptr);
}

void make_realtime() {
  sched_param p{};
  p.sched_priority = kRtPriority;
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) != 0) {
    LOG_WARN("could not set SCHED_FIFO {} on the audio thread (running at normal priority)",
             kRtPriority);
  } else {
    LOG_INFO("audio thread running SCHED_FIFO {}", kRtPriority);
  }
}

// Touch the stack once up front so no page fault can land inside the audio loop. The
// asm barrier stops the optimizer from deleting a write-only local array.
void prefault_stack() {
  char scratch[kPrefaultBytes];
  for (size_t i = 0; i < kPrefaultBytes; i += 4096) scratch[i] = 0;
  asm volatile("" : : "r"(scratch) : "memory");
}

}  // namespace

AudioEngine::AudioEngine(Control& ctl, RingBuffer& ring, EngineOptions opt)
    : ctl_(ctl), ring_(ring), opt_(opt) {
  gen_.init(opt_.rate);
  identify_frames_ = static_cast<uint64_t>(kIdentifySeconds * opt_.rate);
}

AudioEngine::~AudioEngine() { stop(); }

EngineStats AudioEngine::stats() const {
  EngineStats s;
  s.running = streaming_.load();
  s.sim = opt_.sim;
  s.device = opt_.sim ? "simulator" : opt_.device;
  s.rate = opt_.rate;
  s.period = opt_.period;
  s.periods = opt_.periods;
  s.capture_channels = opt_.sim ? kInputs : cap_ch_;
  s.format = opt_.sim ? "float32 (simulated)" : "S32_LE";
  s.xruns = xruns_.load();
  s.generation = generation_.load();
  s.samples = ring_.counter();
  s.last_error = last_error_;
  return s;
}

bool AudioEngine::configure(snd_pcm_t* pcm, unsigned channels, const char* what) {
  snd_pcm_hw_params_t* hw;
  snd_pcm_hw_params_alloca(&hw);

  int err = snd_pcm_hw_params_any(pcm, hw);
  if (err < 0) {
    last_error_ = std::string(what) + ": hw_params_any: " + snd_strerror(err);
    return false;
  }

  err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (err < 0) {
    last_error_ = std::string(what) + ": set_access: " + snd_strerror(err);
    return false;
  }

  err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE);
  if (err < 0) {
    last_error_ = std::string(what) + ": S32_LE not available: " + snd_strerror(err);
    return false;
  }

  err = snd_pcm_hw_params_set_channels(pcm, hw, channels);
  if (err < 0) {
    last_error_ = std::string(what) + ": " + std::to_string(channels) +
                  " channels not available: " + snd_strerror(err);
    return false;
  }

  err = snd_pcm_hw_params_set_rate(pcm, hw, opt_.rate, 0);
  if (err < 0) {
    last_error_ = std::string(what) + ": rate " + std::to_string(opt_.rate) +
                  " not available: " + snd_strerror(err);
    return false;
  }

  snd_pcm_uframes_t period = opt_.period;
  err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr);
  if (err < 0) {
    last_error_ = std::string(what) + ": set_period_size: " + snd_strerror(err);
    return false;
  }

  snd_pcm_uframes_t buffer = static_cast<snd_pcm_uframes_t>(opt_.period) * opt_.periods;
  err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
  if (err < 0) {
    last_error_ = std::string(what) + ": set_buffer_size: " + snd_strerror(err);
    return false;
  }

  err = snd_pcm_hw_params(pcm, hw);
  if (err < 0) {
    last_error_ = std::string(what) + ": hw_params: " + snd_strerror(err);
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
    last_error_ = std::string(what) + ": sw_params_current: " + snd_strerror(err);
    return false;
  }
  // Nothing may auto-start: the linked group is started once, explicitly, so capture and
  // playback share sample zero.
  snd_pcm_sw_params_set_start_threshold(pcm, sw, 0x7fffffff);
  snd_pcm_sw_params_set_avail_min(pcm, sw, opt_.period);
  err = snd_pcm_sw_params(pcm, sw);
  if (err < 0) {
    last_error_ = std::string(what) + ": sw_params: " + snd_strerror(err);
    return false;
  }
  return true;
}

bool AudioEngine::open_alsa() {
  const unsigned deadline_ms = opt_.open_retry_s * 1000;
  unsigned waited = 0;
  int err = 0;

  // The codec probes asynchronously after the overlay loads, so at boot the device may not
  // exist yet for the first seconds.
  for (;;) {
    err = snd_pcm_open(&capture_, opt_.device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err >= 0) break;
    if (waited >= deadline_ms) {
      last_error_ = "cannot open capture device " + opt_.device + ": " + snd_strerror(err);
      LOG_ERROR("{}", last_error_);
      return false;
    }
    if (waited == 0) LOG_WARN("waiting for {} to appear ({})", opt_.device, snd_strerror(err));
    sleep_ms(500);
    waited += 500;
  }

  err = snd_pcm_open(&playback_, opt_.device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
  if (err < 0) {
    last_error_ = "cannot open playback device " + opt_.device + ": " + snd_strerror(err);
    LOG_ERROR("{}", last_error_);
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
    LOG_WARN("{} — retrying capture with {} channels", last_error_, kInputs);
    snd_pcm_close(capture_);
    capture_ = nullptr;
    err = snd_pcm_open(&capture_, opt_.device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
      last_error_ = std::string("reopen capture: ") + snd_strerror(err);
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
    last_error_ = std::string("snd_pcm_link failed: ") + snd_strerror(err);
    LOG_ERROR("{}", last_error_);
    close_alsa();
    return false;
  }
  LOG_INFO("capture and playback linked: one clock, one start");
  return true;
}

void AudioEngine::close_alsa() {
  if (capture_) {
    snd_pcm_close(capture_);
    capture_ = nullptr;
  }
  if (playback_) {
    snd_pcm_close(playback_);
    playback_ = nullptr;
  }
}

bool AudioEngine::prefill_and_start() {
  int err = snd_pcm_prepare(capture_);
  if (err < 0) {
    last_error_ = std::string("prepare: ") + snd_strerror(err);
    return false;
  }

  // Three periods of silence: once readi starts returning, playback still holds two
  // periods, giving the loop ~21 ms of jitter budget before it underruns.
  std::fill(raw_out_.begin(), raw_out_.end(), 0);
  for (unsigned i = 0; i < 3; ++i) {
    snd_pcm_sframes_t w = snd_pcm_writei(playback_, raw_out_.data(), opt_.period);
    if (w < 0) {
      last_error_ = std::string("prefill writei: ") + snd_strerror(static_cast<int>(w));
      return false;
    }
  }

  err = snd_pcm_start(capture_);
  if (err < 0) {
    last_error_ = std::string("start: ") + snd_strerror(err);
    return false;
  }
  return true;
}

bool AudioEngine::recover(int err) {
  xruns_.fetch_add(1);
  generation_.fetch_add(1);
  LOG_WARN("xrun/recover: {} (total {})", snd_strerror(err), xruns_.load());

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

void AudioEngine::init_mixer() {
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

void AudioEngine::process_block(uint64_t n, size_t frames, float* in6, float* out8) {
  // Input gain lands here, upstream of the ring, so there is one version of the truth: meters,
  // spectrum, THD+N, the scope, listen streams, cross-correlation and anything routed to an
  // output all see the amplified signal. Gaining further downstream (say, only in the listen
  // path) would make the number on the meter disagree with what the operator hears.
  //
  // Clamped to full scale on the way in, because every consumer of the ring assumes |x| <= 1:
  // the envelope columns and the listen stream both convert to int16, and letting a sample past
  // 0 dBFS through would wrap into loud garbage. Clamping instead flat-tops the waveform on the
  // scope and pins the peak meter at 0.0 dBFS.
  for (unsigned c = 0; c < kInputs; ++c) {
    const float g = db_to_lin(ctl_.inputs[c].gain_db.load(std::memory_order_relaxed));
    if (g == 1.0f) continue;  // unity: the common case, and bit-exact — do not touch the samples
    for (size_t i = 0; i < frames; ++i) {
      in6[i * kInputs + c] = clampf(g * in6[i * kInputs + c], -1.0f, 1.0f);
    }
  }

  ring_.write(in6, frames);

  gen_.render(n, frames, ctl_, gen_sine_.data(), gen_noise_.data(), gen_ping_.data(),
              ctl_.ping_log);

  const float* gens[3] = {gen_sine_.data(), gen_noise_.data(), gen_ping_.data()};

  for (unsigned o = 0; o < kOutputs; ++o) {
    const OutputControl& oc = ctl_.outputs[o];
    // Every control value is read once per block, never per sample: an atomic load inside
    // the sample loop would defeat the vectorizer.
    const uint32_t packed = oc.source.load(std::memory_order_relaxed);
    const SourceType type = source_type(packed);
    const uint8_t index = source_index(packed);
    const float gain = oc.mute.load(std::memory_order_relaxed)
                           ? 0.0f
                           : db_to_lin(oc.gain_db.load(std::memory_order_relaxed));
    const uint64_t identify_until = oc.identify_until.load(std::memory_order_relaxed);

    const float* src = nullptr;
    size_t stride = 1;
    if (type == SourceType::Input && index < kInputs) {
      src = in6 + index;
      stride = kInputs;
    } else if (type == SourceType::Gen && index < static_cast<uint8_t>(GenId::Count)) {
      src = gens[index];
    }

    if (identify_until > n) {
      // Identify overrides whatever is routed here, then reverts on its own. Rare and
      // brief, so it gets the slow path all to itself.
      for (size_t i = 0; i < frames; ++i) {
        const uint64_t t = n + i;
        out8[i * kOutputs + o] =
            t < identify_until ? gen_.identify_sample(identify_frames_ - (identify_until - t))
            : src              ? gain * src[i * stride]
                               : 0.0f;
      }
    } else if (!src) {
      for (size_t i = 0; i < frames; ++i) out8[i * kOutputs + o] = 0.0f;
    } else {
      for (size_t i = 0; i < frames; ++i) out8[i * kOutputs + o] = gain * src[i * stride];
    }
  }
}

void AudioEngine::run_alsa() {
  while (running_.load(std::memory_order_relaxed)) {
    snd_pcm_sframes_t got = snd_pcm_readi(capture_, raw_in_.data(), opt_.period);
    if (got < 0) {
      if (!recover(static_cast<int>(got))) {
        running_.store(false);
        return;
      }
      continue;
    }
    const size_t frames = static_cast<size_t>(got);
    const uint64_t n = ring_.counter();

    // Snapshot the channel maps once per block; loading an atomic per sample would keep
    // the conversion loops scalar.
    unsigned imap[kInputs];
    unsigned omap[kOutputs];
    for (unsigned c = 0; c < kInputs; ++c) {
      const unsigned slot = ctl_.input_map[c].load(std::memory_order_relaxed);
      imap[c] = slot < cap_ch_ ? slot : c;
    }
    for (unsigned c = 0; c < kOutputs; ++c) {
      const unsigned slot = ctl_.output_map[c].load(std::memory_order_relaxed);
      omap[c] = slot < kOutputs ? slot : c;
    }

    const unsigned cap_ch = cap_ch_;
    for (unsigned c = 0; c < kInputs; ++c) {
      const int32_t* src = raw_in_.data() + imap[c];
      float* dst = in6_.data() + c;
      for (size_t i = 0; i < frames; ++i) dst[i * kInputs] = s32_to_float(src[i * cap_ch]);
    }

    process_block(n, frames, in6_.data(), out8_.data());

    for (unsigned c = 0; c < kOutputs; ++c) {
      const float* src = out8_.data() + c;
      int32_t* dst = raw_out_.data() + omap[c];
      for (size_t i = 0; i < frames; ++i) dst[i * kOutputs] = float_to_s32(src[i * kOutputs]);
    }

    size_t written = 0;
    while (written < frames) {
      snd_pcm_sframes_t w =
          snd_pcm_writei(playback_, raw_out_.data() + written * kOutputs, frames - written);
      if (w < 0) {
        if (!recover(static_cast<int>(w))) {
          running_.store(false);
          return;
        }
        break;
      }
      written += static_cast<size_t>(w);
    }
  }
}

void AudioEngine::run_sim() {
  const size_t period = opt_.period;
  const double block_ns = 1e9 * static_cast<double>(period) / static_cast<double>(opt_.rate);

  timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);

  uint64_t seed = 0x9e3779b97f4a7c15ull;
  auto noise = [&seed]() {
    seed ^= seed >> 12;
    seed ^= seed << 25;
    seed ^= seed >> 27;
    return static_cast<float>((seed * 0x2545f4914f6cdd1dull) >> 40) / 8388608.0f - 1.0f;
  };

  while (running_.load(std::memory_order_relaxed)) {
    const uint64_t n = ring_.counter();

    // The simulated card is a loopback: output channel c reappears on input channel c
    // delayed by period + c*stagger frames.
    for (size_t i = 0; i < period; ++i) {
      for (unsigned c = 0; c < kInputs; ++c) {
        const size_t delay = period + static_cast<size_t>(c) * opt_.sim_stagger;
        const size_t pos = (n + i + sim_delay_len_ - delay) % sim_delay_len_;
        in6_[i * kInputs + c] = sim_delay_[pos * kInputs + c] + 3e-5f * noise();
      }
    }

    process_block(n, period, in6_.data(), out8_.data());

    for (size_t i = 0; i < period; ++i) {
      const size_t pos = (n + i) % sim_delay_len_;
      for (unsigned c = 0; c < kInputs; ++c) sim_delay_[pos * kInputs + c] = out8_[i * kOutputs + c];
    }

    next.tv_nsec += static_cast<long>(block_ns);
    while (next.tv_nsec >= 1000000000L) {
      next.tv_nsec -= 1000000000L;
      next.tv_sec += 1;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
  }
}

void* AudioEngine::thread_entry(void* self) {
  auto* e = static_cast<AudioEngine*>(self);
  prefault_stack();
  make_realtime();

  if (e->opt_.sim) {
    LOG_INFO("simulator: {} Hz, period {}, virtual loopback OUT->IN (stagger {} frames/ch)",
             e->opt_.rate, e->opt_.period, e->opt_.sim_stagger);
    e->streaming_.store(true);
    e->run_sim();
    e->streaming_.store(false);
    return nullptr;
  }

  // The card is opened here, not in start(), and a failure is never fatal: it retries forever.
  // Exiting when the card is unhappy would take the web console down with it, and that console
  // is the only way to find out why. The error is reported through /api/state instead.
  while (e->running_.load()) {
    if (!e->open_alsa()) {
      LOG_ERROR("{} — retrying in {} s", e->last_error_, kReopenDelayS);
      e->wait_before_retry();
      continue;
    }
    e->init_mixer();
    e->size_buffers();

    if (!e->prefill_and_start()) {
      LOG_ERROR("cannot start stream: {} — retrying in {} s", e->last_error_, kReopenDelayS);
      e->close_alsa();
      e->wait_before_retry();
      continue;
    }

    LOG_INFO("stream running");
    e->last_error_.clear();
    e->streaming_.store(true);
    e->run_alsa();
    e->streaming_.store(false);
    e->close_alsa();

    if (e->running_.load()) {
      LOG_WARN("stream stopped — reopening the card in {} s", kReopenDelayS);
      e->wait_before_retry();
    }
  }
  return nullptr;
}

void AudioEngine::wait_before_retry() {
  for (unsigned i = 0; i < kReopenDelayS * 10 && running_.load(); ++i) sleep_ms(100);
}

void AudioEngine::size_buffers() {
  // The driver may have renegotiated the period, so size the DSP buffers only once it has
  // agreed to something.
  raw_in_.assign(static_cast<size_t>(opt_.period) * cap_ch_, 0);
  raw_out_.assign(static_cast<size_t>(opt_.period) * kOutputs, 0);
  in6_.assign(static_cast<size_t>(opt_.period) * kInputs, 0.0f);
  out8_.assign(static_cast<size_t>(opt_.period) * kOutputs, 0.0f);
  gen_sine_.assign(opt_.period, 0.0f);
  gen_noise_.assign(opt_.period, 0.0f);
  gen_ping_.assign(opt_.period, 0.0f);
  gen_.init(opt_.rate);
  identify_frames_ = static_cast<uint64_t>(kIdentifySeconds * opt_.rate);
}

bool AudioEngine::start() {
  cap_ch_ = opt_.capture_channels;
  size_buffers();

  if (opt_.sim) {
    sim_delay_len_ = static_cast<size_t>(opt_.period) * 4 + kInputs * opt_.sim_stagger + 16;
    sim_delay_.assign(sim_delay_len_ * kInputs, 0.0f);
  }

  // The card is opened by the audio thread, which retries until it succeeds — see
  // thread_entry(). start() failing here means the thread could not be created at all.
  running_.store(true);

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, kAudioStackBytes);
  const int rc = pthread_create(&thread_, &attr, &AudioEngine::thread_entry, this);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    last_error_ = "cannot create audio thread";
    running_.store(false);
    return false;
  }
  thread_valid_ = true;
  return true;
}

void AudioEngine::stop() {
  running_.store(false);
  if (thread_valid_) {
    pthread_join(thread_, nullptr);
    thread_valid_ = false;
  }
  close_alsa();
}

}  // namespace st
