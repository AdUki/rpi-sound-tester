#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "constants.h"
#include "control.h"
#include "generators.h"
#include "ring_buffer.h"
#include "util/clock.h"

namespace st {

class NetAudioServer;

// Everything the engine does with a block once a backend has captured it: the network channels
// read in, input gain, the capture-axis delay, the ring write, the generators and every output and
// sink rendered at the block's n. It owns no thread and no device. AudioEngine calls it once per
// block from its audio thread, and a test can call it with no audio thread at all.
class EngineCore {
 public:
  // `generation` is the engine's discontinuity counter, bumped here when the capture axis moves,
  // just as an xrun bumps it. `clock` stamps the anchor each block publishes.
  EngineCore(Control& ctl, RingBuffer& ring, Clock& clock, unsigned rate,
             std::atomic<uint32_t>& generation);

  // Wired in by main() before the engine starts: the core reads its network channels each block.
  void set_net(NetAudioServer* net) { net_.store(net, std::memory_order_relaxed); }
  // Also before start(): the rings the HDMI speakers and the line out's pair are handed over
  // through. Each is written every block, its sink on or off, so its counter stays equal to the
  // capture ring's and an index in it means the same sample as everywhere else.
  //
  // A sink's block is rendered at a width fixed when the engine is compiled, kHdmiMaxChannels or
  // kLineoutChannels, and copied into the ring at the ring's own width, so the two must be the
  // same. A ring of any other width is refused with an error in the log, and the sink gets no
  // audio: a wider one would have the audio thread read past the end of its block, a narrower one
  // would hand the sink misframed audio. False when refused.
  bool set_hdmi_ring(RingBuffer* ring);
  bool set_lineout_ring(RingBuffer* ring);

  // Sizes every block buffer for `period` frames, and starts the capture delay line and the
  // generators afresh. Allocates, so never while a stream runs.
  void size_buffers(unsigned period);

  // Publishes one captured block to the ring and produces the output block that sits on the same
  // sample axis, whichever backend captured it.
  // `in_all` is kTotalInputs wide: the backend fills channels [0, kInputs) with card audio and
  // process_block fills [kInputs, kTotalInputs) from the network timelines. It is modified in
  // place — input gain is applied to it before anything else reads it.
  void process_block(uint64_t n, size_t frames, float* in_all, float* out8);

  // The block buffers size_buffers() made, for the engine to hand its backend: in() is
  // kTotalInputs wide, out8() kOutputs.
  float* in() { return in_.data(); }
  float* out8() { return out8_.data(); }
  uint64_t identify_frames() const { return identify_frames_; }

 private:
  friend struct EngineTestAccess;

  // Picks up a change to ctl_.net.delay_frames and returns the delay now in force.
  unsigned sync_capture_delay();
  // Writes the local channels of `ring_out` from `live` delayed by cap_delay_frames_, so that
  // ring index n means the same real-world instant on an ADC channel as on a network channel.
  void apply_capture_delay(size_t frames, const float* live, float* ring_out);

  Control& ctl_;
  RingBuffer& ring_;
  Clock& clock_;
  const unsigned rate_;
  std::atomic<uint32_t>& generation_;
  Generators gen_;

  uint64_t identify_frames_ = 0;

  // Set once before start(). The engine does not own it; a null pointer just means the network
  // channels stay silent.
  std::atomic<NetAudioServer*> net_{nullptr};
  std::atomic<RingBuffer*> hdmi_ring_{nullptr};
  std::atomic<RingBuffer*> lineout_ring_{nullptr};

  // Two views of the same block, on the two axes this device has. `in_` is LIVE — what the card
  // just captured and what the network sender wants heard now — and is what outputs are routed
  // from, so a passthrough keeps its near-zero latency. `ring_block_` is the same audio on the
  // CAPTURE axis, every channel delayed alike, and is the only thing the ring ever sees.
  // With no delay configured the two are the same buffer and none of this costs anything.
  std::vector<float> in_;
  std::vector<float> ring_block_;
  std::vector<float> cap_delay_;   // kInputs wide, power-of-two frames, mask-indexed
  size_t cap_delay_len_ = 0;
  size_t cap_delay_mask_ = 0;
  size_t cap_delay_pos_ = 0;
  unsigned cap_delay_frames_ = 0;  // currently in force; a change resets the line
  std::vector<float> out8_;
  std::vector<float> hdmi_block_;     // kHdmiMaxChannels wide
  std::vector<float> lineout_block_;  // kLineoutChannels wide
  std::vector<float> gen_sine_, gen_noise_, gen_ping_, gen_music_;
};

}  // namespace st
