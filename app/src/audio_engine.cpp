#include "audio_engine.h"

#include <pthread.h>

#include "util/log.h"
#include "util/rt.h"

namespace st {

namespace {

constexpr int kRtPriority = 80;
constexpr size_t kAudioStackBytes = 1024 * 1024;
constexpr size_t kPrefaultBytes = 256 * 1024;

// Touch the stack once up front so no page fault can land inside the audio loop. The
// asm barrier stops the optimizer from deleting a write-only local array.
void prefault_stack() {
  char scratch[kPrefaultBytes];
  for (size_t i = 0; i < kPrefaultBytes; i += 4096) scratch[i] = 0;
  asm volatile("" : : "r"(scratch) : "memory");
}

std::unique_ptr<AudioBackend> make_backend(const Control& ctl, const EngineOptions& opt,
                                           Clock& clock) {
  if (opt.sim) return std::make_unique<TimerBackend>(opt, clock);
  return std::make_unique<AlsaLinkedBackend>(ctl, opt, clock);
}

}  // namespace

AudioEngine::AudioEngine(Control& ctl, RingBuffer& ring, EngineOptions opt, Clock& clock)
    : ring_(ring),
      opt_(opt),
      clock_(clock),
      own_backend_(make_backend(ctl, opt_, clock)),
      backend_(own_backend_.get()),
      core_(ctl, ring, clock, opt_.rate, generation_) {}

AudioEngine::AudioEngine(Control& ctl, RingBuffer& ring, EngineOptions opt, AudioBackend& backend,
                         Clock& clock)
    : ring_(ring),
      opt_(opt),
      clock_(clock),
      backend_(&backend),
      core_(ctl, ring, clock, opt_.rate, generation_) {}

AudioEngine::~AudioEngine() { stop(); }

EngineStats AudioEngine::stats() const {
  EngineStats s;
  s.running = streaming_.load();
  s.sim = opt_.sim;
  s.device = opt_.sim ? "simulator" : opt_.device;
  s.rate = opt_.rate;
  s.period = period_.load(std::memory_order_relaxed);
  s.periods = opt_.periods;
  s.capture_channels = opt_.sim ? kInputs : cap_ch_.load(std::memory_order_relaxed);
  s.format = opt_.sim ? "float32 (simulated)" : "S32_LE";
  s.xruns = xruns_.load();
  s.generation = generation_.load();
  s.samples = ring_.counter();
  s.last_error = error();
  return s;
}

bool AudioEngine::recover(int err) {
  xruns_.fetch_add(1);
  generation_.fetch_add(1);
  LOG_WARN("xrun/recover: {} (total {})", backend_strerror(err), xruns_.load());
  return backend_->recover(err);
}

void AudioEngine::publish_shape() {
  const BackendShape shape = backend_->shape();
  cap_ch_.store(shape.capture_channels, std::memory_order_relaxed);
  period_.store(shape.period, std::memory_order_relaxed);
}

bool AudioEngine::run_block() {
  const uint64_t n = ring_.counter();
  float* const in = core_.in();
  float* const out8 = core_.out8();
  const long got = backend_->read_block(n, in);
  if (got < 0) {
    // An unrecoverable xrun ends this stream, not the engine: run_card() closes the card and
    // reopens it, the same never-fatal path as a failed open.
    return recover(static_cast<int>(got));
  }
  const size_t frames = static_cast<size_t>(got);

  core_.process_block(n, frames, in, out8);

  const long put = backend_->write_block(n, out8, frames);
  if (put < 0) return recover(static_cast<int>(put));
  return true;
}

void AudioEngine::run_stream() {
  while (running_.load(std::memory_order_relaxed)) {
    if (!run_block()) return;
  }
}

void AudioEngine::run_sim() {
  // The simulated card opens every time and never fails, so none of run_card()'s retrying.
  backend_->open();
  backend_->start();
  streaming_.store(true);
  run_stream();
  streaming_.store(false);
}

void AudioEngine::run_card() {
  // The card is opened here, not in start(), and a failure is never fatal: it retries forever.
  // Exiting when the card is unhappy would take the web console down with it, and that console
  // is the only way to find out why. The error is reported through /api/state instead.
  while (running_.load()) {
    const bool opened = backend_->open();
    // Even a failed open may have settled the capture width or the period on the way.
    publish_shape();
    if (!opened) {
      LOG_ERROR("{} — retrying in {} s", error(), kReopenDelayS);
      wait_before_retry();
      continue;
    }
    core_.size_buffers(backend_->shape().period);

    if (!backend_->start()) {
      LOG_ERROR("cannot start stream: {} — retrying in {} s", error(), kReopenDelayS);
      backend_->close();
      wait_before_retry();
      continue;
    }

    LOG_INFO("stream running");
    set_error({});
    streaming_.store(true);
    run_stream();
    streaming_.store(false);
    backend_->close();

    if (running_.load()) {
      LOG_WARN("stream stopped — reopening the card in {} s", kReopenDelayS);
      wait_before_retry();
    }
  }
}

void* AudioEngine::thread_entry(void* self) {
  auto* e = static_cast<AudioEngine*>(self);
  prefault_stack();
  make_realtime(kRtPriority, "audio");

  if (e->opt_.sim) {
    e->run_sim();
  } else {
    e->run_card();
  }
  return nullptr;
}

void AudioEngine::wait_before_retry() {
  for (unsigned i = 0; i < kReopenDelayS * 10 && running_.load(); ++i)
    clock_.sleep_until(clock_.now_ns() + 100000000ull);  // 100 ms
}

void AudioEngine::prepare() {
  // Sized for the period the backend will ask for; run_card() sizes them again for whatever the
  // driver agrees to.
  const unsigned period = backend_->shape().period;
  cap_ch_.store(opt_.capture_channels, std::memory_order_relaxed);
  period_.store(period, std::memory_order_relaxed);
  core_.size_buffers(period);
}

bool AudioEngine::start() {
  prepare();

  // The card is opened by the audio thread, which retries until it succeeds — see
  // run_card(). start() failing here means the thread could not be created at all.
  running_.store(true);

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, kAudioStackBytes);
  const int rc = pthread_create(&thread_, &attr, &AudioEngine::thread_entry, this);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    set_error("cannot create audio thread");
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
  backend_->close();
}

}  // namespace st
