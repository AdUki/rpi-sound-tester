#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "control.h"

namespace st {

class BtManager;
class NetAudioServer;

struct BtInputStatus {
  bool enabled = false;
  std::string state = "off";  // off | waiting | streaming | error
  std::string address;        // the device being recorded
  std::string name;
  unsigned rate = 0;          // what it sends at; the converter takes it to the card's
  unsigned channels = 0;
  int input = -1;             // the first of its input channels, or -1
  uint64_t frames = 0;        // received since the device was taken
  uint64_t overruns = 0;      // this thread fell behind bluez-alsa's buffer
  uint64_t restarts = 0;      // the stream resumed after a pause or a stall, and was re-anchored
  std::string error;
};

// The Bluetooth input: a phone, a laptop, anything playing to the device over A2DP, read from
// bluez-alsa's capture PCM and written to two adjacent network channels.
//
// It is a network sender in every way but the transport. It claims a run of channels the way a
// HELLO does, names them after the device, and goes through the same Feed: the stream is anchored
// the alignment delay ahead of playout when its first audio arrives, and a converter whose ratio
// is trimmed to hold that lead takes up both the device's rate (44.1 or 48 kHz) and the drift
// between its clock and the card's. The radio's jitter, which is large, is what the delay absorbs.
//
// Follows whichever device most recently started streaming here; with none, it waits. Like every
// other path into or out of the device, a failure is a status line and a retry, never fatal.
class BtInput {
 public:
  BtInput(Control& ctl, NetAudioServer& net, const BtManager& bt);
  ~BtInput();
  BtInput(const BtInput&) = delete;
  BtInput& operator=(const BtInput&) = delete;

  // Both idempotent.
  void start();
  void stop();
  bool running() const { return running_.load(); }

  BtInputStatus status() const;

 private:
  void run();
  // Records one device until its PCM goes away, another device takes over, or the input is
  // stopped. False when it could not even start, so the caller waits before trying again.
  bool record(const std::string& address, const std::string& name, unsigned rate,
              unsigned channels);
  void set_state(const std::string& state, const std::string& error);
  void sleep_ms(unsigned ms) const;

  Control& ctl_;
  NetAudioServer& net_;
  const BtManager& bt_;

  std::mutex life_m_;
  std::thread thread_;
  std::atomic<bool> running_{false};

  mutable std::mutex m_;
  BtInputStatus st_;
};

}  // namespace st
