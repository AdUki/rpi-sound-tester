#pragma once

#include <array>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "alsa_devices.h"
#include "board.h"
#include "config.h"
#include "device_input.h"
#include "sink_out.h"

namespace st {

class AudioEngine;

// What a slot bound to `d` opens with: `saved`'s layout and rate where the device offers them, else
// the device's defaults, and never HDMI surround above 48 kHz.
struct SinkStart {
  SinkLayout layout = kSinkLayoutDefault;
  unsigned rate = kSinkRateDefault;
};
SinkStart sink_start(const SinkDevice& d, const SinkConfig& saved);

// How many ring columns a capture device takes: stereo where it can do two, else what it must.
unsigned input_columns(const PcmCaps& caps);

// A PCM device as the API lists it: what ALSA reports, and what the daemon does with it.
struct DeviceStatus {
  PcmDevice pcm;
  std::string label;
  bool engine = false;  // the engine card's own
  bool hidden = false;  // board.json says the board wires it to nothing
  int sink = -1;        // the sink slot its playback is on
  int input = -1;       // the first ring column its capture is on
  unsigned input_channels = 0;
  std::string note;     // why it is not used, when it could be ("no free sink slot")
};

// Finds the sound cards ALSA has, at startup and every kScanIntervalMs after, and puts every
// device other than the engine card's to use: its playback on a sink slot (HDMI, a Pi's 3.5 mm
// jack, a USB interface's outputs), its capture on device-input columns (a USB interface's inputs,
// the VIM3L's HDMI loopback). Nothing about a board's devices is compiled in; board.json can only
// name one, mark it HDMI or hide it.
//
// A device keeps its slot and its columns for the life of the process, so one that is unplugged
// and plugged back in finds its routing where it left it: its threads just cannot open it in
// between. A slot's output and ring, and an input's timelines, are made on this thread when the
// device is found and handed to the engine by an atomic store, so a device found later never
// makes the audio thread allocate or wait; nothing is freed before the engine has stopped.
class Devices {
 public:
  // `engine_device` is the engine card's ALSA name, "" for none; every PCM on that card is the
  // engine's. `extra_sinks` are ALSA names to offer as sinks although no scan finds them: a
  // desktop's "default", say.
  Devices(Control& ctl, AudioEngine& engine, const Board& board, std::string engine_device,
          std::vector<std::string> extra_sinks);
  ~Devices();
  Devices(const Devices&) = delete;
  Devices& operator=(const Devices&) = delete;

  // A sink no scan finds, under an id of its own and with what it offers already decided: the
  // Bluetooth output, on a bluez-alsa PCM, which ALSA lists nowhere. It is bound by the first scan
  // like any device, and is always present. Before start().
  void add_fixed_sink(SinkDevice d);
  // Points fixed sink `id` at another ALSA name: the Bluetooth output at another speaker. A running
  // output reopens on it. False when there is no such sink.
  bool retarget(const std::string& id, const std::string& alsa);
  // The bound slot of the sink with device id `id`, or -1.
  int slot_of(const std::string& id) const;

  // Takes the saved settings of every sink, binds every device present now, and keeps looking for
  // devices that come and go until stop().
  void start(const std::map<std::string, SinkConfig>& saved);
  void stop();

  struct Sink {
    unsigned slot = 0;
    SinkDevice device;
    bool present = false;  // found by the last scan
  };
  // The bound slots, in slot order.
  std::vector<Sink> sinks() const;
  // A bound slot's output: one of sinks()'s, which is what makes it exist.
  SinkOutput& output(unsigned slot) { return *outputs_[slot]; }
  // The name config.json gives channel `ch` of slot `slot`'s device.
  std::string channel_name(unsigned slot, unsigned ch) const;

  struct Source {
    std::string id;
    std::string label;
    unsigned first = 0;  // its first ring column
    unsigned channels = 0;
    bool present = false;
    DeviceInputStatus status;
  };
  // The bound device inputs, in column order.
  std::vector<Source> sources() const;

  // Every PCM device the last scan found.
  std::vector<DeviceStatus> devices() const;
  // For a save: the settings of every sink ever configured, bound ones as they are now.
  std::map<std::string, SinkConfig> sink_configs() const;
  uint64_t pinned_bytes() const;

  static constexpr unsigned kScanIntervalMs = 2000;

 private:
  struct Input {
    std::string id, label;
    unsigned first = 0;
    bool present = false;
    std::unique_ptr<DeviceInput> in;
  };

  void loop();
  void scan();
  // Puts `d` on free slot `slot` with its saved settings. m_ held.
  void bind_sink_locked(unsigned slot, const SinkDevice& d);
  SinkDevice sink_device(const PcmDevice& pcm, unsigned pcms_on_card) const;
  std::string label(const PcmDevice& pcm, unsigned pcms_on_card) const;
  // The first of `count` adjacent free device-input columns, or -1. m_ held.
  int free_columns_locked(unsigned count) const;

  Control& ctl_;
  AudioEngine& engine_;
  const Board& board_;
  const std::string engine_card_;  // card id; "" for none
  std::vector<std::string> extra_;
  std::vector<SinkDevice> fixed_;  // under m_ once started

  mutable std::mutex m_;  // guards everything below; outputs_[s] is written once, under it
  std::array<std::unique_ptr<SinkOutput>, kMaxSinks> outputs_;
  std::array<Sink, kMaxSinks> bound_{};
  std::array<bool, kMaxSinks> used_{};
  std::vector<Input> inputs_;
  std::array<bool, kMaxInputs> column_used_{};
  std::map<std::string, SinkConfig> saved_;
  std::vector<DeviceStatus> devices_;

  std::thread thread_;
  bool running_ = false;  // under wake_m_
  std::mutex wake_m_;
  std::condition_variable wake_;
};

}  // namespace st
