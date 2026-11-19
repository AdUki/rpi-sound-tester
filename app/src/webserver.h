#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "util/sysinfo.h"

#include "analysis.h"
#include "audio_engine.h"
#include "bluetooth.h"
#include "bt_input.h"
#include "capture.h"
#include "config.h"
#include "control.h"
#include "kmsg_watch.h"
#include "net_audio.h"
#include "ring_buffer.h"
#include "devices.h"
#include "ws_hub.h"

namespace httplib {
class Server;
}

namespace st {

struct WebOptions {
  std::string www_dir = "/usr/share/soundtester/www";
  std::string host = "0.0.0.0";
  int port = 80;
  bool allow_reboot = true;
  // The most network inputs a restart may be asked for: what the ring's columns and this
  // machine's RAM leave room for.
  unsigned net_inputs_max = kMaxInputs;
  // What GET /api/state reports as `version`: the build's `git describe` and UTC build time,
  // from the generated build_info (main.cpp). "unknown" where the build could not tell.
  std::string version_git = "unknown";
  std::string build_time = "unknown";
};

struct Deps {
  Control& ctl;
  NetAudioServer& net;
  Devices& devices;
  BtManager& bt;   // the adapter, pairing and the agent; its output is the sink kBtSinkId
  BtInput& bt_in;  // a phone playing to the device, onto network channels
  RingBuffer& ring;
  AudioEngine& engine;
  Analysis& analysis;
  CaptureStore& capture;
  KmsgWatch& kmsg;
  ConfigStore& store;
  // Last loaded/saved config; the source of the non-live fields (channel names, loopback
  // offset). Handlers run on many worker threads: guard every access with config_m_.
  Config& config;
};

class WebServer {
 public:
  WebServer(Deps deps, WebOptions opt);
  ~WebServer();

  bool start();   // binds and serves on the calling thread until stop()
  void stop();

 private:
  void install_routes();
  void run_publisher();

  Deps d_;
  WebOptions opt_;
  // For daemon_uptime_s: the server is built once, at startup.
  const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();

  std::unique_ptr<httplib::Server> svr_;
  WsHub hub_;
  std::thread publisher_;
  std::atomic<bool> running_{false};
  std::atomic<unsigned> listen_streams_{0};
  // The multichannel Ogg stream runs kInputs encoders on one worker; capped separately (and well
  // below listen_streams_) so a few of them cannot starve the audio/analysis threads on a Pi.
  std::atomic<unsigned> ogg_streams_{0};
  // Monotonic Ogg bitstream serial, unique per stream.ogg connection.
  std::atomic<uint32_t> ogg_serial_{1};
  // GET /api/genie/wait requests blocking now, capped at kGenieMaxWaits.
  std::atomic<unsigned> genie_waits_{0};
  // GET /api/genie/watch requests running now, capped at kGenieMaxWatches.
  std::atomic<unsigned> genie_watches_{0};
  // Held while a GET /api/capture/wav body is built (read and encoded): one at a time, so the
  // next one's MemAvailable check sees the previous body resident.
  std::timed_mutex wav_build_m_;
  // Held by GET /api/genie/sync from its freeze to its resume: one measurement at a time, so one
  // call's resume never pulls the snapshot from under another's reads.
  std::timed_mutex sync_m_;
  // Per-input telemetry mask, bit c set = input c streamed. A console masks the inputs it is not
  // watching so the spectrum message (the widest frame) drops them. Written by POST
  // /api/telemetry/inputs, read by the publisher. Process-global: one bench, one operator.
  std::atomic<uint32_t> telemetry_mask_{0xffffffffu};

  // CPU load is a delta measurement with exactly one owner: the 1 Hz publisher samples it and
  // parks the whole SysInfo here, and the /api/state handler — which runs on an arbitrary web
  // worker thread — only reads the copy. If handlers sampled /proc/stat themselves they would
  // consume the publisher's baseline and both readings would be garbage (see util/sysinfo.h).
  mutable std::mutex sys_m_;
  SysInfo sys_;

  // Guards d_.config: the save handler replaces it while /api/state handlers on other worker
  // threads read the channel names and loopback offset out of it.
  mutable std::mutex config_m_;
  // Serialises PUT /api/sinks/{id}: two at once could each start or stop the sink's thread between
  // the other's check and its store, leaving the thread and `enabled` disagreeing.
  std::mutex sink_put_m_;

  // Held from flipping a switch that the alignment delay depends on (network input, the Bluetooth
  // input) to storing the delay it implies, so two such requests cannot interleave and leave the
  // delay computed from a state that no longer holds.
  std::mutex delay_m_;
};

}  // namespace st
