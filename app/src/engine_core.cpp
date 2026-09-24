#include "engine_core.h"

#include "net_audio.h"

#include <algorithm>
#include <cmath>

#include "hdmi_layout.h"
#include "output_route.h"
#include "util/dsp.h"
#include "util/log.h"

namespace st {

namespace {

// One of the SoC's own outputs, as the audio thread sees it for one block.
struct SocFeed {
  RingBuffer* ring;           // null until main() wires it in
  const HdmiLayoutInfo* lay;  // puts each speaker in its PCM slot
  unsigned speakers;          // how many are rendered: none while the sink is off
};

SocFeed soc_feed(const std::atomic<RingBuffer*>& ring, const SocControl& sc, unsigned width) {
  RingBuffer* const r = ring.load(std::memory_order_relaxed);
  const HdmiLayoutInfo& lay = hdmi_layout_info(soc_layout(sc, width));
  const bool on = r && sc.enabled.load(std::memory_order_relaxed);
  return {r, &lay, on ? lay.speakers : 0u};
}

// A sink's speakers, rendered at the same n by the same code as the DACs, each into its layout's
// PCM slot of a `Width`-wide block, then handed to the sink's thread. Written whether or not
// anything is playing them, so the handoff ring's counter never falls out of step with the capture
// ring's; that write is a memcpy and an atomic store. Slots no speaker of the layout uses stay
// silent.
template <size_t Width>
void feed_soc(const SocFeed& f, const OutputControl* outs, uint64_t n, size_t frames,
              const float* in_all, const float* const* gens, const Generators& gen,
              uint64_t identify_frames, float* block) {
  if (!f.ring) return;
  if (f.speakers < Width) std::fill(block, block + frames * Width, 0.0f);
  for (unsigned s = 0; s < f.speakers; ++s) {
    route_output<Width>(outs[s], n, frames, in_all, gens, gen, identify_frames,
                        block + f.lay->slot[s]);
  }
  f.ring->write(block, frames);
}

// Whether `ring` takes a sink's blocks as feed_soc<width> writes them. No ring always does: the
// sink is simply not wired in.
bool ring_fits(const RingBuffer* ring, unsigned width, const char* sink) {
  if (!ring || ring->channels() == width) return true;
  LOG_ERROR("{}: its handoff ring is {} channels wide, but the engine renders it {} wide — "
            "the sink gets no audio",
            sink, ring->channels(), width);
  return false;
}

}  // namespace

EngineCore::EngineCore(Control& ctl, RingBuffer& ring, Clock& clock, unsigned rate,
                       std::atomic<uint32_t>& generation)
    : ctl_(ctl), ring_(ring), clock_(clock), rate_(rate), generation_(generation) {}

bool EngineCore::set_hdmi_ring(RingBuffer* ring) {
  const bool ok = ring_fits(ring, kHdmiMaxChannels, "hdmi");
  hdmi_ring_.store(ok ? ring : nullptr, std::memory_order_relaxed);
  return ok;
}

bool EngineCore::set_lineout_ring(RingBuffer* ring) {
  const bool ok = ring_fits(ring, kLineoutChannels, "lineout");
  lineout_ring_.store(ok ? ring : nullptr, std::memory_order_relaxed);
  return ok;
}

void EngineCore::size_buffers(unsigned period) {
  // The driver may have renegotiated the period, so size the DSP buffers only once it has
  // agreed to something.
  in_.assign(static_cast<size_t>(period) * kTotalInputs, 0.0f);
  ring_block_.assign(static_cast<size_t>(period) * kTotalInputs, 0.0f);

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
    cap_delay_.assign(cap_delay_len_ * kInputs, 0.0f);
  }
  out8_.assign(static_cast<size_t>(period) * kOutputs, 0.0f);
  hdmi_block_.assign(static_cast<size_t>(period) * kHdmiMaxChannels, 0.0f);
  lineout_block_.assign(static_cast<size_t>(period) * kLineoutChannels, 0.0f);
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

  const unsigned delay = sync_capture_delay();

  // Outputs are routed from the live block, so a passthrough keeps its near-zero latency; the
  // ring gets the delayed one, so every channel in it shares a single axis. With no delay the
  // two are literally the same buffer and the whole mechanism disappears.
  float* ring_block = delay == 0 ? in_all : ring_block_.data();

  if (NetAudioServer* net = net_.load(std::memory_order_relaxed)) {
    net->read_block(n, frames, delay, in_all, ring_block);
  } else {
    for (unsigned c = kInputs; c < kTotalInputs; ++c) {
      for (size_t i = 0; i < frames; ++i) in_all[i * kTotalInputs + c] = 0.0f;
      if (ring_block != in_all)
        for (size_t i = 0; i < frames; ++i) ring_block[i * kTotalInputs + c] = 0.0f;
    }
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

  for (unsigned c = 0; c < kTotalInputs; ++c) {
    const float g = input_gain(c);
    if (g == 1.0f) continue;  // unity: the common case, and bit-exact — do not touch the samples
    for (size_t i = 0; i < frames; ++i) {
      in_all[i * kTotalInputs + c] = clampf(g * in_all[i * kTotalInputs + c], -1.0f, 1.0f);
    }
  }

  if (delay != 0) {
    // Local audio reaches the ring through the delay line; the network channels reach it from the
    // trailing timeline read, which has not been through the gain loop above, so it gets the same
    // treatment here rather than a second version of the truth.
    apply_capture_delay(frames, in_all, ring_block);
    for (unsigned c = kInputs; c < kTotalInputs; ++c) {
      const float g = input_gain(c);
      if (g == 1.0f) continue;
      for (size_t i = 0; i < frames; ++i)
        ring_block[i * kTotalInputs + c] = clampf(g * ring_block[i * kTotalInputs + c], -1.0f, 1.0f);
    }
  }

  ring_.write(ring_block, frames);

  const SocFeed hdmi = soc_feed(hdmi_ring_, ctl_.hdmi, kHdmiMaxChannels);
  const SocFeed lineout = soc_feed(lineout_ring_, ctl_.lineout, kLineoutChannels);

  // Outputs play at n, undelayed — but capture is held back by cap_delay_frames_, so a ping
  // emitted now shows up in the ring that much later. The log records where it will APPEAR, not
  // where it was emitted, or the scope's ping markers and genie/sync would both aim a full
  // second wide of the arrival the moment a network channel is in use.
  //
  // The melody is rendered only while something plays it. It is a pure function of n, so the
  // blocks it skips cost it nothing: it picks up at the right place in the tune on its own.
  bool want_music = false;
  for (const OutputControl& oc : ctl_.outputs) want_music |= routes_gen(oc, GenId::Music);
  for (unsigned s = 0; s < hdmi.speakers; ++s)
    want_music |= routes_gen(ctl_.hdmi_outputs[s], GenId::Music);
  for (unsigned s = 0; s < lineout.speakers; ++s)
    want_music |= routes_gen(ctl_.lineout_outputs[s], GenId::Music);
  if (!want_music) std::fill(gen_music_.begin(), gen_music_.begin() + frames, 0.0f);
  gen_.render(n, frames, ctl_, gen_sine_.data(), gen_noise_.data(), gen_ping_.data(),
              want_music ? gen_music_.data() : nullptr, ctl_.ping_log, cap_delay_frames_);

  const float* gens[static_cast<size_t>(GenId::Count)] = {gen_sine_.data(), gen_noise_.data(),
                                                          gen_ping_.data(), gen_music_.data()};

  for (unsigned o = 0; o < kOutputs; ++o) {
    route_output<kOutputs>(ctl_.outputs[o], n, frames, in_all, gens, gen_, identify_frames_,
                           out8 + o);
  }

  feed_soc<kHdmiMaxChannels>(hdmi, ctl_.hdmi_outputs.data(), n, frames, in_all, gens, gen_,
                             identify_frames_, hdmi_block_.data());
  feed_soc<kLineoutChannels>(lineout, ctl_.lineout_outputs.data(), n, frames, in_all, gens, gen_,
                             identify_frames_, lineout_block_.data());
}

unsigned EngineCore::sync_capture_delay() {
  const unsigned want =
      std::min<unsigned>(ctl_.net.delay_frames.load(std::memory_order_relaxed),
                         static_cast<unsigned>(cap_delay_len_ ? cap_delay_len_ - 1 : 0));
  if (want != cap_delay_frames_) {
    // The capture axis just moved. Anything already frozen was measured against the old one, so
    // this is a discontinuity in exactly the way an xrun is.
    std::fill(cap_delay_.begin(), cap_delay_.end(), 0.0f);
    cap_delay_pos_ = 0;
    cap_delay_frames_ = want;
    generation_.fetch_add(1);
    LOG_INFO("capture delay now {} frames ({:.0f} ms)", want, 1000.0 * want / rate_);
  }
  return want;
}

void EngineCore::apply_capture_delay(size_t frames, const float* live, float* ring_out) {
  const unsigned d = cap_delay_frames_;
  // Read the delayed frames out before pushing the new ones in. That order is what makes this
  // correct for any delay, including one shorter than a single block.
  for (size_t i = 0; i < frames; ++i) {
    const size_t rd = (cap_delay_pos_ + i - d) & cap_delay_mask_;
    for (unsigned c = 0; c < kInputs; ++c)
      ring_out[i * kTotalInputs + c] = cap_delay_[rd * kInputs + c];
  }
  for (size_t i = 0; i < frames; ++i) {
    const size_t wr = (cap_delay_pos_ + i) & cap_delay_mask_;
    for (unsigned c = 0; c < kInputs; ++c)
      cap_delay_[wr * kInputs + c] = live[i * kTotalInputs + c];
  }
  cap_delay_pos_ = (cap_delay_pos_ + frames) & cap_delay_mask_;
}

}  // namespace st
