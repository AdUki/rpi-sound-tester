#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "audio_backend.h"
#include "constants.h"
#include "control.h"

namespace st {

// A capture sample that names its device frame and slot. It has 20 significant bits, so
// s32_to_float() carries it exactly, and it is never zero, so silence cannot pass for it. Unique
// for frames below 2^16.
inline int32_t fake_capture_code(uint64_t t, unsigned slot) {
  const int32_t v = static_cast<int32_t>(((slot + 1u) << 24) | ((t & 0xffffu) << 8));
  return (t & 1) ? -v : v;
}

// A card made of a script, for stepping the engine a block at a time with no device and no audio
// thread. It captures S32 frames `capture_channels` slots wide, slot s of device frame t holding
// capture(t, s), and keeps every S32 playback frame the engine writes, in order. The slot maps and
// the conversion either side are the shared ones AlsaLinkedBackend runs, so a remap is checked on
// the card's own code. Nothing waits: a read has its block ready at once.
//
// Failures are scripted too: the first `fail_opens` opens and `fail_starts` starts fail, read
// number `xrun_on_read` and write number `xrun_on_write` (counting from 1) return -EPIPE instead,
// and recover() fails while `recover_fails` is set.
//
// `period` is the one the engine asks for, and what shape() reports until the card is open. A card
// whose driver renegotiates it sets `settle_period`, which an open that succeeds takes on, as
// AlsaLinkedBackend's configure() takes on what the driver agreed to.
class FakeBackend final : public AudioBackend {
 public:
  explicit FakeBackend(const Control& ctl, unsigned capture_channels = kTdmSlots,
                       unsigned period = rpi3_octo_profile().clock.period,
                       unsigned rate = rpi3_octo_profile().clock.rate)
      : ctl_(ctl), cap_ch_(capture_channels), period_(period), rate_(rate) {}

  bool open() override {
    ++opens;
    error_at_open.push_back(error());
    if (opens <= fail_opens) {
      set_error("fake: no card");
      return false;
    }
    if (settle_period) period_ = settle_period;
    raw_in_.assign(static_cast<size_t>(period_) * cap_ch_, 0);
    raw_out_.assign(static_cast<size_t>(period_) * kOutputs, 0);
    return true;
  }

  void close() override { ++closes; }

  bool start() override {
    ++starts;
    if (starts <= fail_starts) {
      set_error("fake: will not start");
      return false;
    }
    return true;
  }

  long read_block(uint64_t n, float* in_all) override {
    ++reads;
    read_n.push_back(n);
    if (on_read) on_read();
    if (reads == xrun_on_read) return -EPIPE;
    for (size_t i = 0; i < period_; ++i)
      for (unsigned s = 0; s < cap_ch_; ++s) raw_in_[i * cap_ch_ + s] = capture(captured + i, s);
    captured += period_;
    maps_ = snapshot_slot_maps(ctl_, cap_ch_);
    s32_to_inputs(raw_in_.data(), period_, cap_ch_, maps_, in_all);
    return static_cast<long>(period_);
  }

  long write_block(uint64_t n, const float* out8, size_t frames) override {
    ++writes;
    write_n.push_back(n);
    if (writes == xrun_on_write) return -EPIPE;
    outputs_to_s32(out8, frames, maps_, raw_out_.data());
    played.insert(played.end(), raw_out_.begin(), raw_out_.begin() + frames * kOutputs);
    return static_cast<long>(frames);
  }

  bool recover(int err) override {
    recover_errs.push_back(err);
    return !recover_fails;
  }

  BackendShape shape() const override { return {rate_, period_, cap_ch_, kOutputs, "S32_LE"}; }

  // Slot `slot` of the `frame`th playback frame written.
  int32_t played_at(uint64_t frame, unsigned slot) const {
    return played[static_cast<size_t>(frame) * kOutputs + slot];
  }

  // The script.
  std::function<int32_t(uint64_t, unsigned)> capture = fake_capture_code;
  std::function<void()> on_read;  // the first thing every read does
  unsigned fail_opens = 0;
  unsigned fail_starts = 0;
  unsigned xrun_on_read = 0;
  unsigned xrun_on_write = 0;
  bool recover_fails = false;
  unsigned settle_period = 0;  // 0: the card keeps the period it was asked for

  // What the engine did with it.
  unsigned opens = 0, closes = 0, starts = 0, reads = 0, writes = 0;
  uint64_t captured = 0;                  // device frames delivered so far
  std::vector<uint64_t> read_n, write_n;  // the n each call was handed
  std::vector<int> recover_errs;
  std::vector<std::string> error_at_open;  // what /api/state said as each open began
  std::vector<int32_t> played;            // kOutputs slots per frame

 private:
  const Control& ctl_;
  const unsigned cap_ch_;
  unsigned period_;
  const unsigned rate_;
  SlotMaps maps_{};
  std::vector<int32_t> raw_in_, raw_out_;
};

}  // namespace st
