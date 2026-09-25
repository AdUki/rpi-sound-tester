#include <CLI11.hpp>
#include <alsa/asoundlib.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "analysis.h"
#include "audio_engine.h"
#include "board.h"
#include "capture.h"
#include "channel_layout.h"
#include "config.h"
#include "constants.h"
#include "control.h"
#include "devices.h"
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

// alsa-lib prints its own line to stderr for every failed lookup, and a device that is not there
// yet (the Octo early in boot) or any more (a USB interface unplugged) is looked up every few
// seconds for as long as that lasts. The daemon says once, in its own words, what failed; alsa-lib's
// version is for --verbose.
void alsa_lib_message(const char* file, int line, const char* function, int err, const char* fmt,
                      ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  LOG_DEBUG("alsa-lib {}:{} {}: {}{}{}", file ? file : "?", line, function ? function : "?", msg,
            err ? ": " : "", err ? snd_strerror(err) : "");
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
  CLI::App app{"soundtesterd — multichannel audio test appliance"};

  bool sim = false;
  bool verbose = false;
  unsigned sim_stagger = 0;
  std::string board_path = "/etc/soundtester/board.json";
  std::string device;
  unsigned rate = 0;
  unsigned period = 0;
  int port = 80;
  int net_port = 0;  // 0 = whatever the config says
  std::vector<std::string> extra_sinks;
  std::string www = "/usr/share/soundtester/www";
  std::string config_path = "/etc/soundtester/config.json";
  std::string data_dir = "/data";

  app.add_option("--board", board_path,
                 "What the image says about the board: its engine card, rate and period, and "
                 "names for its devices (default /etc/soundtester/board.json; none is a desktop)");
  app.add_flag("--sim", sim, "Run without hardware: a simulated card loops each output back to its input");
  app.add_option("--sim-stagger", sim_stagger,
                 "Simulator: extra delay per input channel, in frames (channel c is delayed c x N)");
  app.add_option("--device", device,
                 "The engine card: capture paces the engine, playback is linked to it (default "
                 "board.json's; none means a timer paces it)");
  app.add_option("--rate", rate, "Sample rate (default board.json's)");
  app.add_option("--period", period, "Period size in frames (default board.json's)");
  app.add_option("--port", port, "HTTP port (default 80)");
  app.add_option("--net-port", net_port, "TCP port for network audio input (default 4010)");
  app.add_option("--sink", extra_sinks,
                 "Also offer this ALSA device as a sink, e.g. default to hear it through a "
                 "desktop's speakers (repeatable; hardware devices are found without it)");
  app.add_option("--www", www, "Directory of static web files");
  app.add_option("--config", config_path, "Path to the default config");
  app.add_option("--data-dir", data_dir, "Where saved settings live (the writable partition)");
  app.add_flag("-v,--verbose", verbose, "Debug logging");
  CLI11_PARSE(app, argc, argv);

  st::init_logging(verbose);
  snd_lib_error_set_handler(alsa_lib_message);

  st::Board board;
  std::string berr;
  if (!st::load_board(board_path, &board, &berr)) {
    // A board file the image ships and cannot read is a broken image, not a desktop.
    LOG_ERROR("{}", berr);
    return 1;
  }
  // No file is a desktop. On a board it is a daemon copied without the /etc files that came with
  // it: the engine card goes unused, so say so where it will be seen.
  if (!sim && board.engine_device.empty() && device.empty() && access(board_path.c_str(), F_OK) != 0)
    LOG_WARN("no {}: running with no engine card, as on a desktop", board_path);

  // Command-line overrides win over the board file.
  st::EngineOptions eopt;
  eopt.sim = sim;
  eopt.device = device.empty() ? board.engine_device : device;
  eopt.rate = rate ? rate : board.rate;
  eopt.period = period ? period : board.period;
  eopt.periods = board.periods;
  eopt.capture_channels = board.capture_channels;
  eopt.sim_stagger = sim_stagger;

  // The ring's columns, fixed from here on: the engine card's own channels, if it has one (the
  // simulator is an Octo), then the network inputs.
  st::ChannelLayout layout;
  if (eopt.timer() && !sim) {
    layout.local = 0;
    layout.outputs = 0;
  }
  layout.device = std::min(board.device_inputs, st::kMaxInputs - layout.local - layout.net);
  st::set_channels(layout);
  LOG_INFO("board: {}; {} Hz, period {}; {} local inputs, {} outputs, {} network inputs, {} for "
           "device inputs",
           sim ? std::string("simulated Octo")
               : eopt.device.empty() ? std::string("no engine card") : eopt.device,
           eopt.rate, eopt.period, layout.local, layout.outputs, layout.net, layout.device);

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

  if (net_port > 0) cfg.net_port = net_port;

  st::Control ctl;
  cfg.apply_to(ctl, eopt.rate);

  st::RingBuffer ring(st::kRingFrames, layout.total(), 2ull * eopt.period);
  st::AudioEngine engine(ctl, ring, eopt);

  // Wired in before start(), so the audio thread never sees a half-constructed server. A bind
  // failure is reported through /api/net, not fatal — same reasoning as a card that will not
  // open: taking the console down removes the only way to find out what went wrong.
  st::NetAudioServer net(ctl, engine.rate(), st::kNetGuardPeriod, engine.clock());
  engine.set_net(&net);
  if (cfg.net_enabled) net.start(ctl.net.port.load());

  // Sinks are found at runtime, and each hands the engine its ring when it is: see Devices.
  st::Devices devices(ctl, engine, board, sim ? std::string() : eopt.device, extra_sinks);

  // A card that will not open is never fatal: the audio thread keeps retrying and the web
  // console comes up regardless, reporting the failure in /api/state. Only a thread that
  // cannot be created is fatal.
  if (!engine.start()) {
    LOG_ERROR("could not create the audio thread");
    return 1;
  }

  devices.start(cfg.sinks);

  st::Analysis analysis(ring, engine.rate());
  analysis.start();

  st::CaptureStore capture(ring, engine.rate(), engine.period());

  // I2S sync errors are the engine card's: nothing to watch for without one.
  st::KmsgWatch kmsg;
  if (layout.local) kmsg.start();

  st::WebOptions wopt;
  wopt.www_dir = www;
  wopt.port = port;
  // A simulated run is a developer's workstation: its reboot/shutdown buttons must not
  // systemctl the host.
  wopt.allow_reboot = !sim;

  st::Deps deps{ctl, net, devices, ring, engine, analysis, capture, kmsg, store, cfg};
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
  devices.stop();
  engine.stop();
  g_server = nullptr;
  return ok ? 0 : 1;
}
