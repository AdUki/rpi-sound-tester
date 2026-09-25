#pragma once

#include <array>
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
class NetTimeline;

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
  // The ring sink slot `slot`'s channels are handed over through: before start(), or from any
  // thread while the engine runs, once, when a device is found for the slot. From the next block
  // on it is written every block at the block's n, its sink on or off, so its counter is the
  // capture ring's and an index in it means the same sample as everywhere else. The ring must
  // outlive the engine.
  //
  // A sink's block is rendered kMaxSinkWidth wide and copied into the ring at the ring's own
  // width, so the two must be the same. A ring of any other width is refused with an error in the
  // log, and the sink gets no audio: a wider one would have the audio thread read past the end of
  // its block, a narrower one would hand the sink misframed audio. False when refused.
  bool set_sink_ring(unsigned slot, RingBuffer* ring);
  // The timeline device-input column `column` is read from, wired in the same way when its device
  // is found. The timeline must outlive the engine.
  void set_input_timeline(unsigned column, NetTimeline* timeline);

  // Sizes every block buffer for `period` frames, and starts the capture delay line and the
  // generators afresh. Allocates, so never while a stream runs.
  void size_buffers(unsigned period);

  // Publishes one captured block to the ring and produces the output block that sits on the same
  // sample axis, whichever backend captured it.
  // `in_all` is channels().total() wide: the backend fills the local channels with card audio and
  // process_block fills the network ones from their timelines. It is modified in place — input
  // gain is applied to it before anything else reads it.
  void process_block(uint64_t n, size_t frames, float* in_all, float* out8);

  // The block buffers size_buffers() made, for the engine to hand its backend: in() is
  // channels().total() wide, out8() kOutputs.
  float* in() { return in_.data(); }
  float* out8() { return out8_.data(); }
  uint64_t identify_frames() const { return identify_frames_; }

 private:
  friend struct EngineTestAccess;

  // Picks up a change to ctl_.net.delay_frames, or to whether any device input is bound, and
  // returns the delay now in force.
  unsigned sync_capture_delay(bool device_inputs);
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
  std::array<std::atomic<RingBuffer*>, kMaxSinks> sink_rings_{};
  std::array<std::atomic<NetTimeline*>, kMaxInputs> input_timelines_{};  // by ring column
  const unsigned device_delay_;  // kDeviceInputDelayMs, in frames

  // Two views of the same block, on the two axes this device has. `in_` is LIVE — what the card
  // just captured and what the network sender wants heard now — and is what outputs are routed
  // from, so a passthrough keeps its near-zero latency. `ring_block_` is the same audio on the
  // CAPTURE axis, every channel delayed alike, and is the only thing the ring ever sees.
  // With no delay configured the two are the same buffer and none of this costs anything.
  std::vector<float> in_;
  std::vector<float> ring_block_;
  std::vector<float> cap_delay_;   // the local channels, power-of-two frames, mask-indexed
  size_t cap_delay_len_ = 0;
  size_t cap_delay_mask_ = 0;
  size_t cap_delay_pos_ = 0;
  unsigned cap_delay_frames_ = 0;  // currently in force; a change resets the line
  std::vector<float> out8_;
  std::vector<float> sink_block_;  // kMaxSinkWidth wide, one sink at a time
  std::vector<float> gen_sine_, gen_noise_, gen_ping_, gen_music_;
};

}  // namespace st
