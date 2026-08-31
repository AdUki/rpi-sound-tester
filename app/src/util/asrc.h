#pragma once

#include <samplerate.h>

#include <cstddef>
#include <string>
#include <vector>

namespace st {

// Asynchronous sample-rate converter for the network input path, wrapping libsamplerate.
//
// Two machines' crystals are never quite the same, and a sender need not even be running at the
// card's rate — so the conversion ratio is not a constant. `src_process` takes the ratio per call
// and interpolates it across the block, which is what makes a slowly-trimmed ratio a clock
// discipline rather than a source of zipper noise. That is the whole reason for a library here
// rather than a fixed-ratio kernel: the ratio is the control input.
//
// Multi-channel in ONE converter state, on interleaved audio. libsamplerate then advances every
// channel by the same fractional position by construction, so a stereo sender's two halves cannot
// come out of the converter misaligned — which one state per channel could not promise.
//
// Not for the audio thread: a sender's connection thread owns one, so it may allocate.
class Asrc {
 public:
  Asrc() = default;
  ~Asrc() { destroy(); }
  Asrc(const Asrc&) = delete;
  Asrc& operator=(const Asrc&) = delete;

  // SRC_SINC_FASTEST is ~97 dB SNR: at parity with this device's analog path rather than below
  // it, and roughly half the cost of MEDIUM. That matters because every stream is converted, and
  // six of them on a Pi 3 is the case that has to fit — measured, MEDIUM put four channels at 44%
  // of the whole machine. Raise it to SRC_SINC_MEDIUM_QUALITY on a board with cores to spare.
  static constexpr int kQuality = SRC_SINC_FASTEST;

  // Reconfiguring with the same shape keeps the state, so a repeated FORMAT is not a glitch.
  bool configure(unsigned channels, double in_rate, double out_rate, std::string* err) {
    if (channels == channels_ && in_rate == in_rate_ && out_rate == out_rate_) return true;
    destroy();
    channels_ = channels;
    in_rate_ = in_rate;
    out_rate_ = out_rate;
    nominal_ = out_rate / in_rate;
    int e = 0;
    state_ = src_new(kQuality, static_cast<int>(channels), &e);
    if (!state_) {
      if (err) *err = src_strerror(e);
      return false;
    }
    return true;
  }

  // Whether the nominal ratio is 1. The converter runs anyway: it is the only thing holding the
  // two machines' clocks together, and a converter that switched itself off at matched rates
  // would leave nothing to take up the drift between two crystals that are never quite equal.
  bool unity() const { return in_rate_ == out_rate_; }
  double nominal_ratio() const { return nominal_; }

  void reset() {
    if (state_) src_reset(state_);
  }

  // Converts `frames` interleaved frames, appending interleaved output. `trim` multiplies the
  // nominal ratio: 1.0 runs free, and small deviations are how accumulated clock drift is taken
  // out without a step anywhere. Returns frames appended, or 0 with *err set on failure.
  size_t process(const float* in, size_t frames, double trim, std::vector<float>* out,
                 std::string* err) {
    if (!state_) return 0;

    const double ratio = nominal_ * trim;
    const size_t before = out->size();
    size_t used = 0;
    while (used < frames) {
      // Room for this pass plus slack: src_process stops early if the output fills, and a tight
      // buffer would just make more passes.
      const size_t room = static_cast<size_t>((frames - used) * ratio) + 64;
      scratch_.resize(room * channels_);

      SRC_DATA d{};
      d.data_in = const_cast<float*>(in + used * channels_);
      d.input_frames = static_cast<long>(frames - used);
      d.data_out = scratch_.data();
      d.output_frames = static_cast<long>(room);
      d.src_ratio = ratio;
      d.end_of_input = 0;

      const int e = src_process(state_, &d);
      if (e != 0) {
        if (err) *err = src_strerror(e);
        return 0;
      }
      if (d.input_frames_used == 0 && d.output_frames_gen == 0) break;  // needs more input
      out->insert(out->end(), scratch_.begin(),
                  scratch_.begin() + d.output_frames_gen * static_cast<long>(channels_));
      used += static_cast<size_t>(d.input_frames_used);
    }
    return (out->size() - before) / channels_;
  }

 private:
  void destroy() {
    if (state_) {
      src_delete(state_);
      state_ = nullptr;
    }
  }

  SRC_STATE* state_ = nullptr;
  unsigned channels_ = 0;
  double in_rate_ = 0.0, out_rate_ = 0.0, nominal_ = 1.0;
  std::vector<float> scratch_;
};

}  // namespace st
