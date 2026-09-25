#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "control.h"
#include "net_audio.h"
#include "pcm_format.h"
#include "util/asrc.h"
#include "util/clock.h"

namespace st {

class AudioEngine;

struct DeviceInputStatus {
  bool open = false;       // the PCM is open
  bool capturing = false;  // anchored and delivering audio onto the engine's axis
  std::string device;      // the ALSA name it is opened as
  unsigned channels = 0;   // how many it captures: its columns
  unsigned device_rate = 0;
  const char* format = "";
  uint64_t xruns = 0;    // the driver's buffer overflowed
  uint64_t resyncs = 0;  // strayed too far to trim back, so it was re-anchored
  uint64_t late = 0;     // blocks that arrived after the engine had read past them
  double trim_ppm = 0.0;
  std::string error;
};

// Captures one ALSA device other than the engine card (a USB interface, the VIM3L's HDMI
// loopback) onto the engine's sample axis, on its own thread and its own clock.
//
// Every frame is placed at the ring index of the instant it was captured: the engine's position
// now, less what is still queued in the driver. A converter trimmed the way the network input's is
// holds that placement against the drift between the two clocks. The frames go into one timeline
// per channel (net_audio.h), which the audio thread reads a capture delay behind, so a sound heard
// by the engine card and by this device lands on the same index in both.
//
// Like a sink, a device that will not open is never fatal: the thread retries it forever, and one
// that is unplugged is simply one that will not open until it is back.
class DeviceInput {
 public:
  DeviceInput(std::string id, std::string alsa, unsigned channels, Control& ctl,
              const AudioEngine& engine);
  ~DeviceInput();
  DeviceInput(const DeviceInput&) = delete;
  DeviceInput& operator=(const DeviceInput&) = delete;

  // The timeline channel `c` is delivered into, for the audio thread to read.
  NetTimeline& timeline(unsigned c) { return *timelines_[c]; }
  unsigned channels() const { return channels_; }

  void start();
  void stop();
  DeviceInputStatus status() const;

 private:
  void run();
  bool open_pcm();
  void close_pcm();
  bool stream();
  void set_error(std::string msg);

  const std::string id_;
  const std::string alsa_;
  const unsigned channels_;
  Control& ctl_;
  const AudioEngine& engine_;
  Clock& clock_;
  std::vector<std::unique_ptr<NetTimeline>> timelines_;

  std::thread thread_;
  std::atomic<bool> running_{false};
  mutable std::mutex m_;  // guards error_
  std::string error_;

  // Thread-owned.
  snd_pcm_t* pcm_ = nullptr;
  PcmFormat format_ = PcmFormat::S32_LE;
  unsigned dev_rate_ = 0;
  size_t period_ = 0;  // device frames per read
  double rate_ = 0.0;  // engine rate
  Asrc asrc_;
  LeadFilter filter_;
  std::vector<uint8_t> raw_;
  std::vector<float> in_, out_, chan_;

  // Published for status().
  std::atomic<bool> open_{false};
  std::atomic<bool> capturing_{false};
  std::atomic<unsigned> dev_rate_pub_{0};
  std::atomic<const char*> format_pub_{""};
  std::atomic<uint64_t> xruns_{0};
  std::atomic<uint64_t> resyncs_{0};
  std::atomic<uint64_t> late_{0};
  std::atomic<float> trim_ppm_{0.0f};
};

}  // namespace st
