#include "engine_core.h"

#include "net_audio.h"

#include <algorithm>
#include <cmath>

#include "channel_layout.h"
#include "sink_layout.h"
#include "output_route.h"
#include "util/dsp.h"
#include "util/log.h"

namespace st {

namespace {

// A sink slot, as the audio thread sees it for one block.
struct SinkFeed {
  RingBuffer* ring;           // null until main() wires it in
  const SinkLayoutInfo* lay;  // puts each channel in its PCM slot
  unsigned speakers;          // how many are rendered: none while the sink is off
};

SinkFeed sink_feed(const std::atomic<RingBuffer*>& ring, const SinkControl& sc) {
  // Acquire: the ring was built on another thread, perhaps while this one was running.
  RingBuffer* const r = ring.load(std::memory_order_acquire);
  const SinkLayoutInfo& lay = sink_layout_info(sink_layout(sc));
  const bool on = r && sc.enabled.load(std::memory_order_relaxed);
  return {r, &lay, on ? lay.speakers : 0u};
}

// A sink's channels, rendered at the same n by the same code as the engine card's outputs, each
// into its layout's PCM slot of a kMaxSinkWidth-wide block, then handed to the sink's thread.
// Written at n whether or not anything is playing them, so the handoff ring's counter is the capture
// ring's from the first block the slot is wired in; that write is a memcpy and an atomic store.
// Slots no channel of the layout uses stay silent.
void feed_sink(const SinkFeed& f, const OutputControl* outs, uint64_t n, size_t frames,
               const float* in_all, unsigned in_stride, const float* const* gens,
               const Generators& gen, uint64_t identify_frames, float* block) {
  if (!f.ring) return;
  if (f.speakers < kMaxSinkWidth) std::fill(block, block + frames * kMaxSinkWidth, 0.0f);
  for (unsigned s = 0; s < f.speakers; ++s) {
    route_output<kMaxSinkWidth>(outs[s], n, frames, in_all, in_stride, gens, gen,
                                identify_frames, block + f.lay->slot[s]);
  }
  f.ring->write_at(n, block, frames);
}

}  // namespace

EngineCore::EngineCore(Control& ctl, RingBuffer& ring, Clock& clock, unsigned rate,
                       std::atomic<uint32_t>& generation)
    : ctl_(ctl),
      ring_(ring),
      clock_(clock),
      rate_(rate),
      generation_(generation),
      device_delay_(static_cast<unsigned>(1ull * kDeviceInputDelayMs * rate / 1000)) {}

void EngineCore::set_input_timeline(unsigned column, NetTimeline* timeline) {
  if (column < kMaxInputs) input_timelines_[column].store(timeline, std::memory_order_release);
}

bool EngineCore::set_sink_ring(unsigned slot, RingBuffer* ring) {
  if (slot >= kMaxSinks) return false;
  // No ring is always taken: the slot is simply not wired in.
  const bool ok = !ring || ring->channels() == kMaxSinkWidth;
  if (!ok) {
    LOG_ERROR("sink {}: its handoff ring is {} channels wide, but the engine renders it {} wide — "
              "the sink gets no audio",
              slot, ring->channels(), kMaxSinkWidth);
  }
  // Release: what the ring's constructor wrote is visible to the audio thread that loads it.
  sink_rings_[slot].store(ok ? ring : nullptr, std::memory_order_release);
  return ok;
}

void EngineCore::size_buffers(unsigned period) {
  // The driver may have renegotiated the period, so size the DSP buffers only once it has
  // agreed to something.
  const unsigned total = channels().total();
  in_.assign(static_cast<size_t>(period) * total, 0.0f);
  ring_block_.assign(static_cast<size_t>(period) * total, 0.0f);

  // Sized for the maximum delay plus a block of slack, rounded up to a power of two so the
  // circular index is a mask rather than a modulo in a per-sample loop.
  {
    const size_t want =
        static_cast<size_t>(kNetDelayMaxMs / 1000.0 * rate_) + period + 16;
    size_t p = 1;
    while (p < want) p <<= 1;
    cap_delay_len_ = p;
    cap_delay_mask_ = p - 1;
    cap_delay_pos_ = 0;
    cap_delay_frames_ = 0;
    cap_delay_.assign(cap_delay_len_ * std::max(channels().local, 1u), 0.0f);
  }
  out8_.assign(static_cast<size_t>(period) * kOutputs, 0.0f);
  sink_block_.assign(static_cast<size_t>(period) * kMaxSinkWidth, 0.0f);
  gen_sine_.assign(period, 0.0f);
  gen_noise_.assign(period, 0.0f);
  gen_ping_.assign(period, 0.0f);
  gen_music_.assign(period, 0.0f);
  gen_.init(rate_);
  identify_frames_ = static_cast<uint64_t>(kIdentifySeconds * rate_);
}

void EngineCore::process_block(uint64_t n, size_t frames, float* in_all, float* out8) {
  // Publish where the sample counter is right now, so a network client can aim a packet at an
  // absolute index. This is the only place the card's clock is tied to CLOCK_MONOTONIC (clock_,
  // which is that everywhere but in a test), and it is deliberately the same n that generators
  // render at.
  //
  // n corresponds to audio the card captured a moment before this call, so the anchor carries a
  // small systematic offset (a period plus the driver's own buffering). It is constant, so it
  // shifts network audio uniformly rather than smearing it — loopback_offset_samples is the
  // existing knob for taking it out.
  ctl_.anchor.publish(n, clock_.now_ns());

  const ChannelLayout& lay = channels();
  const unsigned total = lay.total();
  NetTimeline* dev_tl[kMaxInputs] = {};
  bool any_device = false;
  for (unsigned c = lay.device_base(); c < total; ++c) {
    dev_tl[c] = input_timelines_[c].load(std::memory_order_acquire);
    any_device |= dev_tl[c] != nullptr;
  }
  const unsigned delay = sync_capture_delay(any_device);

  // Outputs are routed from the live block, so a passthrough keeps its near-zero latency; the
  // ring gets the delayed one, so every channel in it shares a single axis. With no delay the
  // two are literally the same buffer and the whole mechanism disappears.
  float* ring_block = delay == 0 ? in_all : ring_block_.data();

  if (NetAudioServer* net = net_.load(std::memory_order_relaxed)) {
    net->read_block(n, frames, delay, in_all, ring_block);
  } else {
    for (unsigned c = lay.net_base(); c < lay.device_base(); ++c) {
      for (size_t i = 0; i < frames; ++i) in_all[i * total + c] = 0.0f;
      if (ring_block != in_all)
        for (size_t i = 0; i < frames; ++i) ring_block[i * total + c] = 0.0f;
    }
  }

  // A device input's frames are placed where they were captured, so they are read a capture delay
  // behind, where every other column of the ring block is. Routed from that same place: a device
  // input has no live audio to pass through undelayed.
  for (unsigned c = lay.device_base(); c < total; ++c) {
    if (dev_tl[c] && n >= delay) {
      dev_tl[c]->read(n - delay, frames, ring_block + c, total, true);
    } else {
      for (size_t i = 0; i < frames; ++i) ring_block[i * total + c] = 0.0f;
    }
    if (ring_block != in_all)
      for (size_t i = 0; i < frames; ++i) in_all[i * total + c] = ring_block[i * total + c];
  }

  // Input gain lands here, upstream of the ring, so there is one version of the truth: meters,
  // spectrum, THD+N, the scope, listen streams, cross-correlation and anything routed to an
  // output all see the amplified signal. Gaining further downstream (say, only in the listen
  // path) would make the number on the meter disagree with what the operator hears.
  //
  // Clamped to full scale on the way in, because every consumer of the ring assumes |x| <= 1:
  // the envelope columns and the listen stream both convert to int16, and letting a sample past
  // 0 dBFS through would wrap into loud garbage. Clamping instead flat-tops the waveform on the
  // scope and pins the peak meter at 0.0 dBFS.
  //
  // A sender that declared its stream un-mixable is left at unity: no slider, here or in
  // alsamixer, may touch a reference stimulus.
  auto input_gain = [this](unsigned c) {
    const InputControl& in = ctl_.inputs[c];
    if (in.bypass.load(std::memory_order_relaxed)) return 1.0f;
    return in.mute.load(std::memory_order_relaxed)
               ? 0.0f
               : db_to_lin(in.gain_db.load(std::memory_order_relaxed));
  };

  for (unsigned c = 0; c < total; ++c) {
    const float g = input_gain(c);
    if (g == 1.0f) continue;  // unity: the common case, and bit-exact — do not touch the samples
    for (size_t i = 0; i < frames; ++i) {
      in_all[i * total + c] = clampf(g * in_all[i * total + c], -1.0f, 1.0f);
    }
  }

  if (delay != 0) {
    // Local audio reaches the ring through the delay line; the network channels reach it from the
    // trailing timeline read, which has not been through the gain loop above, so it gets the same
    // treatment here rather than a second version of the truth.
    apply_capture_delay(frames, in_all, ring_block);
    for (unsigned c = lay.net_base(); c < total; ++c) {
      const float g = input_gain(c);
      if (g == 1.0f) continue;
      for (size_t i = 0; i < frames; ++i)
        ring_block[i * total + c] = clampf(g * ring_block[i * total + c], -1.0f, 1.0f);
    }
  }

  ring_.write(ring_block, frames);

  SinkFeed sinks[kMaxSinks];
  for (unsigned s = 0; s < kMaxSinks; ++s) sinks[s] = sink_feed(sink_rings_[s], ctl_.sinks[s]);

  // Outputs play at n, undelayed — but capture is held back by cap_delay_frames_, so a ping
  // emitted now shows up in the ring that much later. The log records where it will APPEAR, not
  // where it was emitted, or the scope's ping markers and genie/sync would both aim a full
  // second wide of the arrival the moment a network channel is in use.
  //
  // The melody is rendered only while something plays it. It is a pure function of n, so the
  // blocks it skips cost it nothing: it picks up at the right place in the tune on its own.
  bool want_music = false;
  for (unsigned o = 0; o < lay.outputs; ++o) want_music |= routes_gen(ctl_.outputs[o], GenId::Music);
  for (unsigned s = 0; s < kMaxSinks; ++s)
    for (unsigned c = 0; c < sinks[s].speakers; ++c)
      want_music |= routes_gen(ctl_.sinks[s].outputs[c], GenId::Music);
  if (!want_music) std::fill(gen_music_.begin(), gen_music_.begin() + frames, 0.0f);
  gen_.render(n, frames, ctl_, gen_sine_.data(), gen_noise_.data(), gen_ping_.data(),
              want_music ? gen_music_.data() : nullptr, ctl_.ping_log, cap_delay_frames_);

  const float* gens[static_cast<size_t>(GenId::Count)] = {gen_sine_.data(), gen_noise_.data(),
                                                          gen_ping_.data(), gen_music_.data()};

  // A board with no engine card has no outputs of its own; out8 stays silent.
  for (unsigned o = 0; o < lay.outputs; ++o) {
    route_output<kOutputs>(ctl_.outputs[o], n, frames, in_all, total, gens, gen_,
                           identify_frames_, out8 + o);
  }

  for (unsigned s = 0; s < kMaxSinks; ++s) {
    feed_sink(sinks[s], ctl_.sinks[s].outputs.data(), n, frames, in_all, total, gens, gen_,
              identify_frames_, sink_block_.data());
  }
}

unsigned EngineCore::sync_capture_delay(bool device_inputs) {
  unsigned want = ctl_.net.delay_frames.load(std::memory_order_relaxed);
  if (device_inputs) want = std::max(want, device_delay_);
  want = std::min<unsigned>(want, static_cast<unsigned>(cap_delay_len_ ? cap_delay_len_ - 1 : 0));
  if (want != cap_delay_frames_) {
    // The capture axis just moved. Anything already frozen was measured against the old one, so
    // this is a discontinuity in exactly the way an xrun is.
    std::fill(cap_delay_.begin(), cap_delay_.end(), 0.0f);
    cap_delay_pos_ = 0;
    cap_delay_frames_ = want;
    generation_.fetch_add(1);
    ctl_.capture_delay.store(want, std::memory_order_relaxed);
    LOG_INFO("capture delay now {} frames ({:.0f} ms)", want, 1000.0 * want / rate_);
  }
  return want;
}

void EngineCore::apply_capture_delay(size_t frames, const float* live, float* ring_out) {
  const unsigned d = cap_delay_frames_;
  const unsigned local = channels().local;
  const unsigned total = channels().total();
  // Read the delayed frames out before pushing the new ones in. That order is what makes this
  // correct for any delay, including one shorter than a single block.
  for (size_t i = 0; i < frames; ++i) {
    const size_t rd = (cap_delay_pos_ + i - d) & cap_delay_mask_;
    for (unsigned c = 0; c < local; ++c)
      ring_out[i * total + c] = cap_delay_[rd * local + c];
  }
  for (size_t i = 0; i < frames; ++i) {
    const size_t wr = (cap_delay_pos_ + i) & cap_delay_mask_;
    for (unsigned c = 0; c < local; ++c)
      cap_delay_[wr * local + c] = live[i * total + c];
  }
  cap_delay_pos_ = (cap_delay_pos_ + frames) & cap_delay_mask_;
}

}  // namespace st
