#include <CLI11.hpp>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <thread>

#include "analysis.h"
#include "audio_engine.h"
#include "capture.h"
#include "config.h"
#include "constants.h"
#include "control.h"
#include "kmsg_watch.h"
#include "ring_buffer.h"
#include "util/log.h"
#include "webserver.h"

namespace {
st::WebServer* g_server = nullptr;
std::atomic<bool> g_stopping{false};

void on_signal(int) {
  g_stopping.store(true);
  if (g_server) g_server->stop();
}

// Bound how long a stop can take. Each WebSocket connection has a reader thread parked in a
// blocking recv (see WsReadPump in webserver.cpp); a client still attached when we are asked to
// stop leaves that recv blocked until the socket's read timeout, and the clean path joins it. The
// daemon holds nothing a teardown must flush — config saves are explicit — so once a stop is
// requested, give the clean path a moment and then hard-exit. The kernel reclaims the ALSA handle,
// the mlock'd ring and the threads. Without this, a reboot from the web console would stall until
// systemd's stop timeout fired SIGKILL.
void start_shutdown_watchdog() {
  std::thread([] {
    using namespace std::chrono_literals;
    while (!g_stopping.load()) std::this_thread::sleep_for(100ms);
    std::this_thread::sleep_for(3s);
    std::_Exit(0);
  }).detach();
}
}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"soundtesterd — multichannel audio test appliance (Audio Injector Octo)"};

  bool sim = false;
  bool verbose = false;
  unsigned sim_stagger = 0;
  std::string device;
  unsigned rate = 0;
  unsigned period = 0;
  int port = 80;
  std::string www = "/usr/share/soundtester/www";
  std::string config_path = "/etc/soundtester/config.json";
  std::string data_dir = "/data";

  app.add_flag("--sim", sim, "Run without hardware: a simulated card loops each output back to its input");
  app.add_option("--sim-stagger", sim_stagger,
                 "Simulator: extra delay per input channel, in frames (channel c is delayed c x N)");
  app.add_option("--device", device, "ALSA device (default hw:audioinjectoroc,0)");
  app.add_option("--rate", rate, "Sample rate (default 96000)");
  app.add_option("--period", period, "Period size in frames (default 1024)");
  app.add_option("--port", port, "HTTP port (default 80)");
  app.add_option("--www", www, "Directory of static web files");
  app.add_option("--config", config_path, "Path to the default config");
  app.add_option("--data-dir", data_dir, "Where saved settings live (the writable partition)");
  app.add_flag("-v,--verbose", verbose, "Debug logging");
  CLI11_PARSE(app, argc, argv);

  st::init_logging(verbose);

  st::ConfigStore store(config_path, data_dir);
  st::Config cfg = store.load();

  // A tmpfs on /data means the real partition never mounted. The box still runs and stays
  // reachable, but nothing can be saved — say so at startup rather than letting the first
  // "Save as boot defaults" discover it.
  if (!store.is_persistent()) {
    LOG_ERROR("{} is a RAM filesystem: the data partition did not mount. The appliance runs "
              "normally, but settings cannot be saved as boot defaults.",
              data_dir);
  }

  // Command-line overrides win over the config file.
  if (!device.empty()) cfg.device = device;
  if (rate) cfg.rate = rate;
  if (period) cfg.period = period;

  st::Control ctl;
  cfg.apply_to(ctl);

  st::EngineOptions eopt;
  eopt.sim = sim;
  eopt.device = cfg.device;
  eopt.rate = cfg.rate;
  eopt.period = cfg.period;
  eopt.periods = cfg.periods;
  eopt.capture_channels = cfg.capture_channels;
  eopt.sim_stagger = sim_stagger;

  st::RingBuffer ring(st::kRingFrames, st::kInputs, 2ull * cfg.period);
  st::AudioEngine engine(ctl, ring, eopt);

  // A card that will not open is never fatal: the audio thread keeps retrying and the web
  // console comes up regardless, reporting the failure in /api/state. Only a thread that
  // cannot be created is fatal.
  if (!engine.start()) {
    LOG_ERROR("could not create the audio thread");
    return 1;
  }

  st::Analysis analysis(ring, engine.rate());
  analysis.start();

  st::CaptureStore capture(ring, engine.rate(), engine.period());

  st::KmsgWatch kmsg;
  kmsg.start();

  st::WebOptions wopt;
  wopt.www_dir = www;
  wopt.port = port;
  // A simulated run is a developer's workstation: its reboot/shutdown buttons must not
  // systemctl the host.
  wopt.allow_reboot = !sim;

  st::Deps deps{ctl, ring, engine, analysis, capture, kmsg, store, cfg};
  st::WebServer server(deps, wopt);
  g_server = &server;

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);
  start_shutdown_watchdog();

  const bool ok = server.start();

  LOG_INFO("shutting down");
  kmsg.stop();
  analysis.stop();
  engine.stop();
  g_server = nullptr;
  return ok ? 0 : 1;
}
