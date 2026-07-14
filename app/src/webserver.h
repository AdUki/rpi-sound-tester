#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "util/sysinfo.h"

#include "analysis.h"
#include "audio_engine.h"
#include "capture.h"
#include "config.h"
#include "control.h"
#include "kmsg_watch.h"
#include "ring_buffer.h"
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
};

struct Deps {
  Control& ctl;
  RingBuffer& ring;
  AudioEngine& engine;
  Analysis& analysis;
  CaptureStore& capture;
  KmsgWatch& kmsg;
  ConfigStore& store;
  Config& config;  // last loaded/saved config; the source of the non-live fields
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

  std::unique_ptr<httplib::Server> svr_;
  WsHub hub_;
  std::thread publisher_;
  std::atomic<bool> running_{false};
  std::atomic<unsigned> listen_streams_{0};

  // CPU load is a delta measurement with exactly one owner: the 1 Hz publisher samples it and
  // parks the whole SysInfo here, and the /api/state handler — which runs on an arbitrary web
  // worker thread — only reads the copy. If handlers sampled /proc/stat themselves they would
  // consume the publisher's baseline and both readings would be garbage (see util/sysinfo.h).
  mutable std::mutex sys_m_;
  SysInfo sys_;
};

}  // namespace st
