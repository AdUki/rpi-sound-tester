#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "board_profile.h"
#include "constants.h"
#include "control.h"
#include "util/clock.h"

typedef struct _snd_pcm snd_pcm_t;

namespace st {

// What the engine opens, and the shape it asks for: the compiled-in board's clock unless main()
// says otherwise. Here rather than beside AudioEngine because the backends are built from it.
struct EngineOptions {
  bool sim = false;
  std::string device = rpi3_octo_profile().clock.capture_device;
  unsigned rate = rpi3_octo_profile().clock.rate;
  unsigned period = rpi3_octo_profile().clock.period;
  unsigned periods = rpi3_octo_profile().clock.periods;
  // Falls back to kInputs if 8 ch cannot be opened.
  unsigned capture_channels = rpi3_octo_profile().clock.capture_slots.front();
  // Simulator only: output channel c loops back into input channel c, delayed by
  // period + c*sim_stagger frames.
  unsigned sim_stagger = 0;
};

// What an open backend settled on. Read on the audio thread, or before it starts; the engine
// republishes what web handlers need through atomics of its own.
struct BackendShape {
  unsigned rate = 0;
  unsigned period = 0;             // frames per block, as the driver agreed it
  unsigned capture_channels = 0;   // slots in one capture frame
  unsigned playback_channels = 0;  // slots in one playback frame
  const char* format = "";
};

// Where the engine's blocks come from and go to: the card, or something standing in for it. The
// engine calls it from its audio thread only, a few times per block; every sample loop, the
// conversion to and from the device's format included, stays inside the calls, with its strides
// fixed at compile time.
//
// Error codes are negative, the way alsa-lib returns them; backend_strerror() puts one in words.
class AudioBackend {
 public:
  virtual ~AudioBackend() = default;

  // Opens the device and settles its shape; nothing runs until start(). False, with the reason in
  // error(), when it cannot, having closed again whatever it did open; the engine tries later.
  virtual bool open() = 0;
  // Safe to call on a backend that is not open.
  virtual void close() = 0;
  // Starts an open device. False, with the reason in error(), and the engine closes it.
  virtual bool start() = 0;
  // Waits for the next captured block and writes it into channels [0, kInputs) of `in_all`, which
  // is kTotalInputs wide and one period long. `n` is the ring index the block will be published at.
  // Returns the block's length in frames, or an error code (an xrun) for recover().
  virtual long read_block(uint64_t n, float* in_all) = 0;
  // Plays `frames` frames of `out8` (kOutputs wide), the block rendered for ring index `n`.
  // Returns `frames`, or an error code for recover().
  virtual long write_block(uint64_t n, const float* out8, size_t frames) = 0;
  // Gets the stream going again after read_block() or write_block() returned `err`. False ends the
  // stream: the engine closes the device and opens it again.
  virtual bool recover(int err) = 0;
  virtual BackendShape shape() const = 0;

  // The engine's last error, as /api/state reports it: set by the backend while it opens or
  // recovers the device and by the engine around it, both on the audio thread, and read by web
  // handlers. A bare std::string would be a racing read against a reallocating write.
  void set_error(std::string msg);
  std::string error() const;

 private:
  mutable std::mutex err_m_;
  std::string last_error_;
};

const char* backend_strerror(int err);

// ---- Interleaved S32 frames ---------------------------------------------------------------------
// Shared by every backend that moves the Octo's S32 frames, the card's and the fake the tests drive,
// so a remap is checked on the code the card runs.

// ctl.input_map and ctl.output_map as one block uses them. Read once per block, since loading an
// atomic per sample would keep the conversion loops scalar; a slot the stream does not have falls
// back to the channel's own.
struct SlotMaps {
  unsigned in[kInputs];    // ADC c is captured from slot in[c]
  unsigned out[kOutputs];  // DAC c is played into slot out[c]
};
SlotMaps snapshot_slot_maps(const Control& ctl, unsigned capture_channels);

// `frames` capture frames, `channels` slots each, into channels [0, kInputs) of `in_all`.
void s32_to_inputs(const int32_t* raw, size_t frames, unsigned channels, const SlotMaps& maps,
                   float* in_all);
// `frames` frames of `out8` into playback frames kOutputs slots wide.
void outputs_to_s32(const float* out8, size_t frames, const SlotMaps& maps, int32_t* raw);

// ---- The backends -------------------------------------------------------------------------------

// The Octo: capture and playback on one card, snd_pcm_link()ed so that they start together and
// share sample zero, both S32_LE. Capture is read in 8 TDM slots, or in the six ADCs' when the
// driver will not widen it, and each block is remapped through the slot maps both ways.
class AlsaLinkedBackend final : public AudioBackend {
 public:
  // `clock` paces the wait for a device node that has not appeared yet: the engine's, so that the
  // whole retry cadence runs on one clock.
  AlsaLinkedBackend(const Control& ctl, const EngineOptions& opt, Clock& clock);
  ~AlsaLinkedBackend() override;

  bool open() override;
  void close() override;
  bool start() override;
  long read_block(uint64_t n, float* in_all) override;
  long write_block(uint64_t n, const float* out8, size_t frames) override;
  bool recover(int err) override;
  BackendShape shape() const override;

 private:
  bool open_alsa();
  void close_alsa();
  bool configure(snd_pcm_t* pcm, unsigned channels, const char* what);
  bool prefill_and_start();
  void init_mixer();

  const Control& ctl_;
  Clock& clock_;
  // opt_.period is what the driver agreed to once it has renegotiated it, and is asked for again
  // on every reopen.
  EngineOptions opt_;
  snd_pcm_t* capture_ = nullptr;
  snd_pcm_t* playback_ = nullptr;
  unsigned cap_ch_ = 0;  // what capture opened with: 8 slots, or the fallback's 6
  // Snapshotted by read_block() and used again by the write_block() that follows it.
  SlotMaps maps_{};
  std::vector<int32_t> raw_in_;
  std::vector<int32_t> raw_out_;
};

// The simulated card: blocks paced by a clock rather than by a device, and a loopback where the
// codec would be. Output channel c comes back on input c period + c*sim_stagger frames later, under
// a small noise floor, so a delay measurement has something exact to find: with --sim-stagger 137,
// IN1->IN2 reads 137 samples and IN1->IN3 274.
class TimerBackend final : public AudioBackend {
 public:
  TimerBackend(const EngineOptions& opt, Clock& clock);

  bool open() override;  // cannot fail
  void close() override {}
  bool start() override;  // the pacing counts from now
  long read_block(uint64_t n, float* in_all) override;
  long write_block(uint64_t n, const float* out8, size_t frames) override;
  // Nothing here fails, so nothing ever needs recovering.
  bool recover(int) override { return true; }
  BackendShape shape() const override;

 private:
  const EngineOptions opt_;
  Clock& clock_;
  uint64_t block_ns_ = 0;
  uint64_t next_ns_ = 0;  // when the block after the one in flight is due
  uint64_t seed_ = 0;     // the simulated ADCs' noise
  std::vector<float> sim_delay_;
  size_t sim_delay_len_ = 0;
};

}  // namespace st
