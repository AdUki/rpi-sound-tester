#include "bluetooth.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <nlohmann/json.hpp>

#include "control.h"
#include "util/log.h"

using json = nlohmann::json;

namespace st {

namespace {

constexpr const char* kBluez = "org.bluez";
constexpr const char* kBluealsa = "org.bluealsa";
constexpr const char* kAgentPath = "/org/soundtester/agent";
constexpr const char* kAdapterIface = "org.bluez.Adapter1";
constexpr const char* kDeviceIface = "org.bluez.Device1";
constexpr const char* kPcmIface = "org.bluealsa.PCM1";
// The tester's own media player (bt_player.h), as BlueZ's Media1.RegisterPlayer wants one: an MPRIS
// player object, whose properties BlueZ follows through PropertiesChanged and whose methods it calls
// when a speaker's button is pressed.
constexpr const char* kPlayerPath = "/org/soundtester/player";
constexpr const char* kMprisIface = "org.mpris.MediaPlayer2.Player";
// The console's player position is extrapolated from this often while playing.
constexpr uint64_t kPlayerPublishNs = 500 * 1000000ull;

// What the agent tells BlueZ it can do. With a screen and a yes/no — the console — every phone
// pairs by numeric comparison, and a speaker with no screen still pairs by just-works.
constexpr const char* kAgentCapability = "DisplayYesNo";

// Calls that should be quick get this long: BlueZ answers a property write in milliseconds, and
// a bus thread stuck for the default 25 s is an agent that misses its pairings.
constexpr uint64_t kCallTimeoutUs = 5 * 1000000ull;
// Coalesces a burst of signals (a scan reports devices one inquiry result at a time) into one read.
constexpr uint64_t kRefreshDebounceNs = 150 * 1000000ull;
// ...and reads anyway now and then, so a missed signal cannot leave the console stale for long.
constexpr uint64_t kRefreshPeriodNs = 3 * 1000000000ull;
// BlueZ writes a new bond to its storage as the pairing completes; copy it a moment later.
constexpr uint64_t kPersistDelayNs = 2 * 1000000000ull;
// How long a web handler waits for the bus thread to take an answer.
constexpr auto kAnswerWait = std::chrono::seconds(6);  // past kCallTimeoutUs: a call in flight finishes first

constexpr const char* kA2dpSourceUuid = "0000110a-0000-1000-8000-00805f9b34fb";
constexpr const char* kA2dpSinkUuid = "0000110b-0000-1000-8000-00805f9b34fb";

uint64_t s_to_ns(unsigned s) { return static_cast<uint64_t>(s) * 1000000000ull; }

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Reads the next complete value of `m` into JSON: numbers, strings and booleans as themselves,
// arrays as arrays, dictionaries as objects (keyed by their key's text), structs as arrays and
// variants as what they hold. One reader for every reply this file parses, so that BlueZ's
// a{oa{sa{sv}}} is a few lookups rather than a nest of container calls. Returns 0 at the end of
// the enclosing container, 1 after a value, and a negative errno on a malformed message.
int read_json(sd_bus_message* m, json* out) {
  char type = 0;
  const char* contents = nullptr;
  int r = sd_bus_message_peek_type(m, &type, &contents);
  if (r <= 0) return r;

  switch (type) {
    case SD_BUS_TYPE_BYTE: {
      uint8_t v = 0;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v;
      break;
    }
    case SD_BUS_TYPE_BOOLEAN: {
      int v = 0;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v != 0;
      break;
    }
    case SD_BUS_TYPE_INT16: {
      int16_t v = 0;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v;
      break;
    }
    case SD_BUS_TYPE_UINT16: {
      uint16_t v = 0;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v;
      break;
    }
    case SD_BUS_TYPE_INT32: {
      int32_t v = 0;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v;
      break;
    }
    case SD_BUS_TYPE_UINT32: {
      uint32_t v = 0;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v;
      break;
    }
    case SD_BUS_TYPE_INT64: {
      int64_t v = 0;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v;
      break;
    }
    case SD_BUS_TYPE_UINT64: {
      uint64_t v = 0;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v;
      break;
    }
    case SD_BUS_TYPE_DOUBLE: {
      double v = 0;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v;
      break;
    }
    case SD_BUS_TYPE_STRING:
    case SD_BUS_TYPE_OBJECT_PATH:
    case SD_BUS_TYPE_SIGNATURE: {
      const char* v = nullptr;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = v ? v : "";
      break;
    }
    case SD_BUS_TYPE_UNIX_FD: {
      int v = -1;
      r = sd_bus_message_read_basic(m, type, &v);
      *out = nullptr;
      break;
    }
    case SD_BUS_TYPE_VARIANT: {
      if ((r = sd_bus_message_enter_container(m, type, contents)) < 0) return r;
      if ((r = read_json(m, out)) < 0) return r;
      r = sd_bus_message_exit_container(m);
      break;
    }
    case SD_BUS_TYPE_ARRAY: {
      if ((r = sd_bus_message_enter_container(m, type, contents)) < 0) return r;
      if (contents[0] == SD_BUS_TYPE_DICT_ENTRY_BEGIN) {
        *out = json::object();
        const std::string entry(contents + 1, strlen(contents) - 2);
        while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, entry.c_str())) > 0) {
          json k, v;
          if ((r = read_json(m, &k)) < 0 || (r = read_json(m, &v)) < 0) return r;
          (*out)[k.is_string() ? k.get<std::string>() : k.dump()] = std::move(v);
          if ((r = sd_bus_message_exit_container(m)) < 0) return r;
        }
      } else {
        *out = json::array();
        for (;;) {
          json v;
          if ((r = read_json(m, &v)) <= 0) break;
          out->push_back(std::move(v));
        }
      }
      if (r < 0) return r;
      r = sd_bus_message_exit_container(m);
      break;
    }
    case SD_BUS_TYPE_STRUCT: {
      if ((r = sd_bus_message_enter_container(m, type, contents)) < 0) return r;
      *out = json::array();
      for (;;) {
        json v;
        if ((r = read_json(m, &v)) <= 0) break;
        out->push_back(std::move(v));
      }
      if (r < 0) return r;
      r = sd_bus_message_exit_container(m);
      break;
    }
    default:
      return -EINVAL;
  }
  return r < 0 ? r : 1;
}

std::string str_of(const json& o, const char* key) {
  const auto it = o.find(key);
  return it != o.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool bool_of(const json& o, const char* key) {
  const auto it = o.find(key);
  return it != o.end() && it->is_boolean() && it->get<bool>();
}

long long num_of(const json& o, const char* key, long long fallback = 0) {
  const auto it = o.find(key);
  return it != o.end() && it->is_number() ? it->get<long long>() : fallback;
}

// The operator reads these in the console, next to the device; the D-Bus name goes to the log.
std::string friendly(const sd_bus_error* e) {
  if (!e || !e->name) return "failed";
  const std::string n = e->name;
  auto is = [&](const char* suffix) { return n == std::string("org.bluez.Error.") + suffix; };
  if (is("AuthenticationFailed")) {
    return "authentication failed — a wrong PIN, or the device has forgotten this pairing "
           "(forget it here too and pair again)";
  }
  if (is("AuthenticationRejected") || is("AuthenticationCanceled")) return "the pairing was declined";
  if (is("AuthenticationTimeout")) return "the other device did not answer in time";
  if (is("ConnectionAttemptFailed")) return "could not reach the device — is it on and in range?";
  if (is("InProgress")) return "already in progress";
  if (is("AlreadyConnected")) return "already connected";
  if (is("NotReady")) return "Bluetooth is off";
  if (is("NotAvailable") || is("NotSupported")) {
    return "the device has no profile in common with this one (no A2DP)";
  }
  // A failed connection is a generic Failed with the reason in the message, in BlueZ's own words.
  const std::string msg = e->message ? e->message : "";
  if (msg == "br-connection-page-timeout" || msg == "Page Timeout" ||
      msg == "br-connection-create-socket") {
    return "could not reach the device — is it on and in range?";
  }
  if (msg == "br-connection-profile-unavailable") {
    return "the device has no profile in common with this one (no A2DP)";
  }
  if (msg == "br-connection-refused" || msg == "br-connection-canceled") {
    return "the device refused the connection";
  }
  if (msg == "Not Powered") return "Bluetooth is off";
  if (n == "org.freedesktop.DBus.Error.NoReply" || n == "org.freedesktop.DBus.Error.Timeout") {
    return "no answer in time";
  }
  return e->message && *e->message ? e->message : n;
}

std::string address_from_path(const std::string& path) {
  const size_t at = path.rfind("/dev_");
  if (at == std::string::npos) return {};
  std::string a = path.substr(at + 5);
  std::replace(a.begin(), a.end(), '_', ':');
  return a;
}

std::string hostname() {
  char h[256] = {0};
  if (gethostname(h, sizeof(h) - 1) != 0 || !h[0]) return "soundtester";
  return h;
}

// MPRIS spells the states with a capital; BlueZ compares them without caring.
const char* mpris_status(const std::string& s) {
  return s == "playing" ? "Playing" : s == "paused" ? "Paused" : "Stopped";
}

// MPRIS metadata, a{sv}. No mpris:length: a test signal has no end.
int append_metadata(sd_bus_message* m, const BtPlayerInfo& p) {
  int r = sd_bus_message_open_container(m, 'a', "{sv}");
  if (r >= 0) r = sd_bus_message_append(m, "{sv}", "mpris:trackid", "o", "/org/soundtester/player/track");
  if (r >= 0) r = sd_bus_message_append(m, "{sv}", "xesam:title", "s", p.title.c_str());
  if (r >= 0) r = sd_bus_message_append(m, "{sv}", "xesam:artist", "as", 1, p.artist.c_str());
  if (r >= 0) r = sd_bus_message_append(m, "{sv}", "xesam:album", "s", p.album.c_str());
  if (r >= 0) r = sd_bus_message_close_container(m);
  return r;
}

bool is_error(const sd_bus_error* e, const char* name) {
  return e && e->name && strcmp(e->name, name) == 0;
}

}  // namespace

// ---- Decisions ----------------------------------------------------------------------------------

bool bt_address_ok(const std::string& a) {
  if (a.size() != 17) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (i % 3 == 2) {
      if (a[i] != ':') return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(a[i]))) {
      return false;
    }
  }
  return true;
}

std::string bt_address_upper(const std::string& a) {
  std::string u = a;
  for (char& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return u;
}

std::string bt_pick_adapter(const std::vector<BtAdapterId>& adapters, const std::string& want) {
  if (want.empty()) {
    std::string path;
    for (const BtAdapterId& a : adapters)
      if (path.empty() || a.path == "/org/bluez/hci0") path = a.path;
    return path;
  }
  const bool by_address = bt_address_ok(want);
  for (const BtAdapterId& a : adapters) {
    if (by_address ? bt_address_upper(a.address) == bt_address_upper(want)
                   : a.path == "/org/bluez/" + want)
      return a.path;
  }
  return {};
}

std::string bt_device_path(const std::string& adapter_path, const std::string& address) {
  std::string a = bt_address_upper(address);
  std::replace(a.begin(), a.end(), ':', '_');
  return adapter_path + "/dev_" + a;
}

std::string bt_pcm_name(const std::string& address) {
  const std::string a = address.empty() ? "00:00:00:00:00:00" : bt_address_upper(address);
  return "bluealsa:DEV=" + a + ",PROFILE=a2dp";
}

std::string bt_pcm_address(const std::string& pcm) {
  // DEV=... anywhere in the argument list, or the address as the first positional argument.
  std::string a;
  const size_t dev = pcm.find("DEV=");
  if (dev != std::string::npos) {
    a = pcm.substr(dev + 4, 17);
  } else if (pcm.rfind("bluealsa:", 0) == 0) {
    a = pcm.substr(9, 17);
  }
  if (!bt_address_ok(a) || a == "00:00:00:00:00:00") return {};
  return bt_address_upper(a);
}

BtRoles bt_roles(const std::vector<std::string>& uuids) {
  BtRoles r;
  for (const std::string& u : uuids) {
    const std::string l = lower(u);
    if (l == kA2dpSinkUuid) r.sink = true;
    if (l == kA2dpSourceUuid) r.source = true;
  }
  return r;
}

BtVerdict bt_agent_policy(BtAsk ask) {
  return ask == BtAsk::Passkey ? BtVerdict::Ask : BtVerdict::Accept;
}

bool bt_player_command(const std::string& in, std::string* method) {
  static const char* const kMap[][2] = {{"play", "Play"},   {"pause", "Pause"},
                                        {"stop", "Stop"},   {"next", "Next"},
                                        {"previous", "Previous"}};
  for (const auto& m : kMap) {
    if (in == m[0]) {
      *method = m[1];
      return true;
    }
  }
  return false;
}

// ---- The agent's open requests and the sd-bus callbacks -----------------------------------------

struct BtManager::Pending {
  sd_bus_message* msg = nullptr;  // held open until answered; null for a display-only request
  BtAsk ask = BtAsk::Confirm;
  BtRequest req;
  std::string dev_path;
  uint64_t deadline_ns = 0;
};

struct BtBus {
  struct AsyncOp {
    BtManager* self;
    std::string path;
    std::string method;
  };

  static int signal(sd_bus_message*, void* self, sd_bus_error*) {
    static_cast<BtManager*>(self)->dirty_ = true;
    return 0;
  }

  // BlueZ restarting drops our agent with it, and a fresh one starts from its own settings.
  static int owner(sd_bus_message* m, void* self, sd_bus_error*) {
    auto* b = static_cast<BtManager*>(self);
    const char *name = nullptr, *was = nullptr, *now = nullptr;
    if (sd_bus_message_read(m, "sss", &name, &was, &now) < 0 || !name) return 0;
    if (strcmp(name, kBluez) == 0) {
      b->bluez_owner_ = now ? now : "";
      b->agent_registered_ = false;
      b->player_registered_ = false;
      b->settings_dirty_ = true;
      b->clear_request(false);
      if (b->bluez_owner_.empty()) LOG_WARN("bluetooth: BlueZ has left the bus");
      b->dirty_ = true;
    } else if (strcmp(name, kBluealsa) == 0) {
      b->dirty_ = true;
    }
    return 0;
  }

  static int agent(sd_bus_message* m, void* self, sd_bus_error*) {
    return static_cast<BtManager*>(self)->agent_call(m, sd_bus_message_get_member(m));
  }

  static int async(sd_bus_message* m, void* data, sd_bus_error*) {
    auto* op = static_cast<AsyncOp*>(data);
    op->self->on_reply(op->path, op->method, m);
    return 0;
  }

  // A speaker's button, or busctl. Answered first, then carried out: switching the output on or
  // off takes a moment, and BlueZ is waiting.
  static int player(sd_bus_message* m, void* self, sd_bus_error*) {
    auto* b = static_cast<BtManager*>(self);
    const std::string cmd = lower(sd_bus_message_get_member(m));
    const int r = sd_bus_reply_method_return(m, "");
    const char* who = sd_bus_message_get_sender(m);
    LOG_INFO("bluetooth: player {} from {}", cmd, who ? who : "?");
    if (b->local_.command) b->local_.command(cmd);
    b->tick_player(mono_ns());
    return r;
  }

  static int prop(sd_bus*, const char*, const char*, const char* property, sd_bus_message* reply,
                  void* self, sd_bus_error*) {
    const BtPlayerInfo& p = static_cast<BtManager*>(self)->local_info_;
    if (strcmp(property, "PlaybackStatus") == 0)
      return sd_bus_message_append(reply, "s", mpris_status(p.status));
    if (strcmp(property, "Metadata") == 0) return append_metadata(reply, p);
    if (strcmp(property, "Position") == 0)
      return sd_bus_message_append(reply, "x", static_cast<int64_t>(p.position_ms) * 1000);
    if (strcmp(property, "Rate") == 0) return sd_bus_message_append(reply, "d", 1.0);
    return sd_bus_message_append(reply, "b", 1);  // the Can* ones: it can do all of them
  }

  static const sd_bus_vtable* player_vtable() {
    static const sd_bus_vtable v[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("Play", "", "", player, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Pause", "", "", player, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("PlayPause", "", "", player, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Stop", "", "", player, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Next", "", "", player, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Previous", "", "", player, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_PROPERTY("PlaybackStatus", "s", prop, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("Metadata", "a{sv}", prop, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        // BlueZ counts the position on by itself while playing; it is told where it restarts.
        SD_BUS_PROPERTY("Position", "x", prop, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("Rate", "d", prop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CanGoNext", "b", prop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CanGoPrevious", "b", prop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CanPlay", "b", prop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CanPause", "b", prop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CanControl", "b", prop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_VTABLE_END};
    return v;
  }

  static const sd_bus_vtable* agent_vtable() {
    static const sd_bus_vtable v[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("Release", "", "", agent, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("RequestPinCode", "o", "s", agent, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("DisplayPinCode", "os", "", agent, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("RequestPasskey", "o", "u", agent, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("DisplayPasskey", "ouq", "", agent, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("RequestConfirmation", "ou", "", agent, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("RequestAuthorization", "o", "", agent, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("AuthorizeService", "os", "", agent, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Cancel", "", "", agent, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_VTABLE_END};
    return v;
  }
};

// ---- Lifecycle -----------------------------------------------------------------------------------

BtManager::BtManager(BtSettings settings) : settings_(std::move(settings)) {
  status_.settings = settings_;
}

BtManager::~BtManager() { stop(); }

bool BtManager::start() {
  std::lock_guard<std::mutex> life(life_m_);
  if (running_.load()) return true;
  wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (wake_fd_ < 0) {
    LOG_ERROR("bluetooth: eventfd: {}", strerror(errno));
    return false;
  }
  running_.store(true);
  {
    std::lock_guard<std::mutex> lk(m_);
    status_.running = true;
  }
  thread_ = std::thread([this] { run(); });
  return true;
}

void BtManager::stop() {
  std::lock_guard<std::mutex> life(life_m_);
  if (!running_.exchange(false)) return;
  if (wake_fd_ >= 0) eventfd_write(wake_fd_, 1);
  if (thread_.joinable()) thread_.join();
  const int fd = wake_fd_.exchange(-1);
  if (fd >= 0) close(fd);
  std::lock_guard<std::mutex> lk(m_);
  status_.running = false;
}

void BtManager::set_not_running_reason(std::string reason) {
  std::lock_guard<std::mutex> lk(m_);
  not_running_reason_ = std::move(reason);
}

void BtManager::set_adapter(std::string want) { want_adapter_ = std::move(want); }

void BtManager::post(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lk(m_);
    commands_.push_back(std::move(fn));
  }
  const int fd = wake_fd_.load();
  if (fd >= 0) eventfd_write(fd, 1);
}

void BtManager::drain_commands() {
  std::deque<std::function<void()>> todo;
  {
    std::lock_guard<std::mutex> lk(m_);
    todo.swap(commands_);
  }
  for (auto& fn : todo) fn();
}

void BtManager::run() {
  std::string logged;  // a failure is logged once, not every few seconds for as long as it lasts
  while (running_.load()) {
    if (!bus_ && !open_bus()) {
      if (error_ != logged) {
        LOG_WARN("bluetooth: {} — retrying every {} s", error_, kReopenDelayS);
        logged = error_;
      }
      drain_commands();  // each sees no bus and gives up, rather than piling up for later
      pollfd pfd{wake_fd_, POLLIN, 0};
      poll(&pfd, 1, static_cast<int>(kReopenDelayS * 1000));
      eventfd_t v;
      eventfd_read(wake_fd_, &v);
      continue;
    }
    logged.clear();

    drain_commands();
    for (;;) {
      const int r = sd_bus_process(bus_, nullptr);
      if (r < 0) {
        LOG_WARN("bluetooth: lost the system bus: {}", strerror(-r));
        close_bus();
        break;
      }
      if (r == 0) break;
    }
    if (!bus_) continue;
    tick();

    pollfd pfd[2] = {{sd_bus_get_fd(bus_), static_cast<short>(sd_bus_get_events(bus_)), 0},
                     {wake_fd_, POLLIN, 0}};
    int timeout_ms = 100;  // the timers in tick() are this coarse, and need be no finer
    uint64_t until = 0;
    if (sd_bus_get_timeout(bus_, &until) >= 0 && until != UINT64_MAX) {
      timespec ts{};
      clock_gettime(CLOCK_MONOTONIC, &ts);
      const uint64_t now_us = static_cast<uint64_t>(ts.tv_sec) * 1000000ull + ts.tv_nsec / 1000;
      const uint64_t left_ms = until > now_us ? (until - now_us + 999) / 1000 : 0;
      timeout_ms = static_cast<int>(std::min<uint64_t>(left_ms, 100));
    }
    if (poll(pfd, 2, timeout_ms) > 0 && (pfd[1].revents & POLLIN)) {
      eventfd_t v;
      eventfd_read(wake_fd_, &v);
    }
  }
  if (bus_) {
    if (scan_until_ns_) stop_scan();
    close_bus();
  }
}

bool BtManager::open_bus() {
  int r = sd_bus_open_system(&bus_);
  if (r < 0) {
    bus_ = nullptr;
    set_error(std::string("cannot reach the system D-Bus: ") + strerror(-r));
    return false;
  }
  sd_bus_set_method_call_timeout(bus_, kCallTimeoutUs);

  r = sd_bus_add_object_vtable(bus_, &agent_slot_, kAgentPath, "org.bluez.Agent1",
                               BtBus::agent_vtable(), this);
  if (r < 0) LOG_WARN("bluetooth: cannot export the pairing agent: {}", strerror(-r));
  if (local_.poll) {
    local_info_ = local_.poll(mono_ns());
    r = sd_bus_add_object_vtable(bus_, &player_slot_, kPlayerPath, kMprisIface,
                                 BtBus::player_vtable(), this);
    if (r < 0) LOG_WARN("bluetooth: cannot export the media player: {}", strerror(-r));
  }

  auto match = [this](const char* sender, const char* path, const char* iface, const char* member,
                      sd_bus_message_handler_t fn) {
    sd_bus_slot* slot = nullptr;
    if (sd_bus_match_signal(bus_, &slot, sender, path, iface, member, fn, this) >= 0)
      match_slots_.push_back(slot);
  };
  // Everything either service says is a reason to look again; the reads are cheap and the
  // debounce in tick() keeps a scan's flood of them to a few re-reads a second.
  match(kBluez, nullptr, nullptr, nullptr, BtBus::signal);
  match(kBluealsa, nullptr, nullptr, nullptr, BtBus::signal);
  match("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "NameOwnerChanged", BtBus::owner);

  agent_registered_ = false;
  settings_dirty_ = true;
  dirty_ = true;
  adapter_path_.clear();
  bluez_owner_.clear();
  return true;
}

void BtManager::close_bus() {
  clear_request(false);
  // Calls still out are dropped with the bus, replies and all: none of those pairings is running.
  started_here_.clear();
  for (sd_bus_slot* s : match_slots_) sd_bus_slot_unref(s);
  match_slots_.clear();
  if (agent_slot_) agent_slot_ = sd_bus_slot_unref(agent_slot_);
  if (player_slot_) player_slot_ = sd_bus_slot_unref(player_slot_);
  player_registered_ = false;
  if (bus_) bus_ = sd_bus_flush_close_unref(bus_);
  agent_registered_ = false;
  available_ = false;
  audio_ = false;
  devices_.clear();
  publish();
}

void BtManager::set_error(std::string e) { error_ = std::move(e); }

// ---- The periodic part -------------------------------------------------------------------------

void BtManager::tick() {
  const uint64_t now = mono_ns();
  if ((dirty_ && now - last_refresh_ns_ >= kRefreshDebounceNs) ||
      now - last_refresh_ns_ >= kRefreshPeriodNs) {
    dirty_ = false;
    last_refresh_ns_ = now;
    refresh();
  }
  expire_request(now);
  tick_player(now);
  if (scan_until_ns_ && now >= scan_until_ns_) {
    LOG_INFO("bluetooth: scan stopped after {} s", kBtScanS);
    stop_scan();
  }
  if ((persist_at_ns_ && now >= persist_at_ns_) || (persist_late_ns_ && now >= persist_late_ns_)) {
    if (persist_at_ns_ && now >= persist_at_ns_) persist_at_ns_ = 0;
    else persist_late_ns_ = 0;
    std::function<void()> hook;
    {
      std::lock_guard<std::mutex> lk(m_);
      hook = bonds_hook_;
    }
    if (hook) hook();
  }
}

void BtManager::refresh() {
  if (bluez_owner_.empty()) {
    sd_bus_error e = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    if (sd_bus_call_method(bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "GetNameOwner", &e, &reply, "s", kBluez) >= 0) {
      const char* owner = nullptr;
      if (sd_bus_message_read(reply, "s", &owner) >= 0 && owner) bluez_owner_ = owner;
    }
    sd_bus_message_unref(reply);
    sd_bus_error_free(&e);
  }

  auto managed = [this](const char* service, const char* path, json* out, sd_bus_error* e) {
    sd_bus_message* reply = nullptr;
    const int r = sd_bus_call_method(bus_, service, path, "org.freedesktop.DBus.ObjectManager",
                                     "GetManagedObjects", e, &reply, "");
    const bool ok = r >= 0 && read_json(reply, out) > 0 && out->is_object();
    sd_bus_message_unref(reply);
    return ok;
  };

  json objs;
  sd_bus_error e = SD_BUS_ERROR_NULL;
  if (!managed(kBluez, "/", &objs, &e)) {
    available_ = false;
    devices_.clear();
    adapter_ = BtAdapterInfo{};
    if (is_error(&e, SD_BUS_ERROR_SERVICE_UNKNOWN) || is_error(&e, SD_BUS_ERROR_NAME_HAS_NO_OWNER)) {
      set_error("BlueZ is not running (bluetooth.service)");
    } else {
      set_error(std::string("BlueZ: ") + (e.message ? e.message : "no answer"));
    }
    sd_bus_error_free(&e);
    publish();
    return;
  }
  sd_bus_error_free(&e);
  if (!agent_registered_) register_agent();

  std::vector<BtAdapterId> adapters;
  for (auto it = objs.begin(); it != objs.end(); ++it)
    if (it->contains(kAdapterIface))
      adapters.push_back({it.key(), str_of((*it)[kAdapterIface], "Address")});
  const std::string apath = bt_pick_adapter(adapters, want_adapter_);
  if (apath.empty()) {
    available_ = false;
    devices_.clear();
    adapter_ = BtAdapterInfo{};
    if (!want_adapter_.empty()) {
      std::string have;
      for (const BtAdapterId& a : adapters)
        have += (have.empty() ? "" : ", ") + a.path.substr(a.path.rfind('/') + 1) + " " + a.address;
      set_error("no Bluetooth adapter " + want_adapter_ + " (BlueZ has " +
                (have.empty() ? std::string("none") : have) + ")");
    } else {
      set_error("no Bluetooth adapter: the radio needs dtparam=krnbt=on in config.txt and its "
                "firmware in /lib/firmware/brcm (dmesg | grep -i blue)");
    }
    publish();
    return;
  }

  const json& ap = objs[apath][kAdapterIface];
  BtAdapterInfo a;
  a.path = apath;
  a.address = str_of(ap, "Address");
  a.name = str_of(ap, "Alias");
  a.powered = bool_of(ap, "Powered");
  a.discoverable = bool_of(ap, "Discoverable");
  a.pairable = bool_of(ap, "Pairable");
  a.discovering = bool_of(ap, "Discovering");
  a.discoverable_timeout_s = static_cast<unsigned>(num_of(ap, "DiscoverableTimeout"));
  if (!available_) LOG_INFO("bluetooth: adapter {} ({})", a.address, apath);
  adapter_ = a;
  available_ = true;
  if (error_.rfind("BlueZ", 0) == 0 || error_.rfind("no Bluetooth", 0) == 0 ||
      error_.rfind("cannot reach", 0) == 0) {
    set_error({});
  }

  std::vector<BtDeviceInfo> devs;
  const std::string prefix = apath + "/";
  for (auto it = objs.begin(); it != objs.end(); ++it) {
    if (it.key().rfind(prefix, 0) != 0 || !it->contains(kDeviceIface)) continue;
    const json& dp = (*it)[kDeviceIface];
    BtDeviceInfo d;
    d.path = it.key();
    d.address = bt_address_upper(str_of(dp, "Address"));
    d.name = str_of(dp, "Alias");
    if (d.name.empty()) d.name = str_of(dp, "Name");
    d.icon = str_of(dp, "Icon");
    d.paired = bool_of(dp, "Paired");
    d.trusted = bool_of(dp, "Trusted");
    d.connected = bool_of(dp, "Connected");
    if (dp.contains("RSSI") && dp["RSSI"].is_number()) {
      d.has_rssi = true;
      d.rssi = dp["RSSI"].get<int>();
    }
    std::vector<std::string> uuids;
    if (dp.contains("UUIDs") && dp["UUIDs"].is_array())
      for (const auto& u : dp["UUIDs"])
        if (u.is_string()) uuids.push_back(u.get<std::string>());
    d.roles = bt_roles(uuids);
    devs.push_back(std::move(d));
  }

  // AVRCP: a device's media player and its audio link's volume are objects under the device's
  // own path. BlueZ reports a player's position only when it changes (a seek, a new track, play
  // or pause), so it is carried forward here from when that was seen.
  const uint64_t now_ns = mono_ns();
  std::map<std::string, std::pair<uint32_t, uint64_t>> positions;
  for (auto it = objs.begin(); it != objs.end(); ++it) {
    for (BtDeviceInfo& d : devs) {
      if (it.key().rfind(d.path + "/", 0) != 0) continue;
      if (it->contains("org.bluez.MediaPlayer1")) {
        const json& pp = (*it)["org.bluez.MediaPlayer1"];
        BtPlayerInfo& p = d.player;
        p.present = true;
        p.path = it.key();
        p.status = str_of(pp, "Status");
        if (pp.contains("Track") && pp["Track"].is_object()) {
          const json& t = pp["Track"];
          p.title = str_of(t, "Title");
          p.artist = str_of(t, "Artist");
          p.album = str_of(t, "Album");
          // Unknown is all ones, in either width a device happens to use.
          const long long dur = num_of(t, "Duration");
          p.duration_ms = dur > 0 && dur < 0x7fffffff ? static_cast<uint32_t>(dur) : 0;
        }
        const uint32_t pos = static_cast<uint32_t>(num_of(pp, "Position"));
        const auto was = positions_.find(p.path);
        const uint64_t seen = was != positions_.end() && was->second.first == pos
                                  ? was->second.second : now_ns;
        positions[p.path] = {pos, seen};
        uint64_t at = pos;
        if (p.status == "playing") at += (now_ns - seen) / 1000000ull;
        if (p.duration_ms && at > p.duration_ms) at = p.duration_ms;
        p.position_ms = static_cast<uint32_t>(at);
      }
      if (it->contains("org.bluez.MediaTransport1")) {
        const json& tp = (*it)["org.bluez.MediaTransport1"];
        d.transport_path = it.key();
        if (tp.contains("Volume")) d.volume = static_cast<int>(num_of(tp, "Volume"));
      }
    }
  }
  positions_ = std::move(positions);

  // bluez-alsa's PCMs say which of those devices are carrying audio, and in which direction. It is
  // optional: without it pairing still works, there is just nothing to play to.
  json pcms;
  sd_bus_error pe = SD_BUS_ERROR_NULL;
  const bool had_audio = audio_;
  audio_ = managed(kBluealsa, "/org/bluealsa", &pcms, &pe);
  sd_bus_error_free(&pe);
  if (audio_ != had_audio) {
    LOG_INFO("bluetooth: bluez-alsa {}", audio_ ? "is up" : "is not running — no audio links");
  }
  if (audio_) {
    for (auto it = pcms.begin(); it != pcms.end(); ++it) {
      if (!it->contains(kPcmIface)) continue;
      const json& pp = (*it)[kPcmIface];
      if (str_of(pp, "Transport").rfind("A2DP", 0) != 0) continue;
      const std::string dev = str_of(pp, "Device");
      auto d = std::find_if(devs.begin(), devs.end(), [&](const BtDeviceInfo& x) {
        return x.path == dev;
      });
      if (d == devs.end()) continue;
      // bluez-alsa names the direction from its own side: a "sink" PCM is one it takes audio into,
      // to send to a speaker, so for us it is playback.
      BtPcmInfo& p = str_of(pp, "Mode") == "sink" ? d->playback : d->capture;
      p.present = true;
      p.codec = str_of(pp, "Codec");
      p.rate = static_cast<unsigned>(num_of(pp, "Sampling"));
      p.channels = static_cast<unsigned>(num_of(pp, "Channels"));
      p.delay_ms = static_cast<double>(num_of(pp, "Delay")) / 10.0;
      p.sequence = static_cast<uint32_t>(num_of(pp, "Sequence"));
    }
  }

  // Every paired device is trusted, so it reconnects without asking anyone. Done when it pairs,
  // however it came to.
  std::set<std::string> paired;
  for (const BtDeviceInfo& d : devs) {
    if (!d.paired) continue;
    paired.insert(d.address);
    if (paired_known_ && !paired_.count(d.address) && !d.trusted) {
      std::string err;
      if (!set_bool(d.path, kDeviceIface, "Trusted", true, &err))
        LOG_WARN("bluetooth: cannot trust {}: {}", d.address, err);
    }
  }
  if (paired_known_ && paired != paired_) {
    LOG_INFO("bluetooth: {} paired device(s)", paired.size());
    schedule_persist();
  }
  paired_ = std::move(paired);
  paired_known_ = true;

  // A pairing in progress whose prompt is only for show is over once the device is paired.
  if (pending_ && !pending_->msg) {
    const BtDeviceInfo* d = nullptr;
    for (const BtDeviceInfo& x : devs)
      if (x.path == pending_->dev_path) d = &x;
    if (d && d->paired) clear_request(false);
  }

  for (auto it = dev_state_.begin(); it != dev_state_.end();) {
    const bool gone = std::none_of(devs.begin(), devs.end(),
                                   [&](const BtDeviceInfo& x) { return x.path == it->first; });
    it = gone ? dev_state_.erase(it) : std::next(it);
  }
  for (BtDeviceInfo& d : devs) {
    const auto st = dev_state_.find(d.path);
    if (st == dev_state_.end()) continue;
    // A connection that failed earlier is old news once the device is connected, however it got
    // there: the phone itself usually reconnects.
    if (d.connected && st->second.busy.empty() && !st->second.error.empty() &&
        st->second.error.find("volume") == std::string::npos) {
      st->second.error.clear();
    }
    d.busy = st->second.busy;
    d.error = st->second.error;
  }

  // Paired first, then whatever is connected, then by name: the devices that matter at the top,
  // and an order that does not shuffle as a scan adds more.
  std::stable_sort(devs.begin(), devs.end(), [](const BtDeviceInfo& x, const BtDeviceInfo& y) {
    if (x.paired != y.paired) return x.paired;
    if (x.connected != y.connected) return x.connected;
    return lower(x.name.empty() ? x.address : x.name) < lower(y.name.empty() ? y.address : y.name);
  });
  stamp_local_player(devs);
  devices_ = std::move(devs);

  if (apath != adapter_path_) {
    adapter_path_ = apath;
    settings_dirty_ = true;
    player_registered_ = false;
  }
  if (settings_dirty_) reconcile_adapter();
  if (!player_registered_ && player_slot_) register_player();
  publish();
}

// Keeps BlueZ at what the operator configured. Done when the adapter appears (BlueZ restarted, or
// started after us) and when the settings change, not on every read: someone using bluetoothctl on
// a dev image should not have every change undone a second later.
void BtManager::reconcile_adapter() {
  settings_dirty_ = false;
  BtSettings s;
  {
    std::lock_guard<std::mutex> lk(m_);
    s = settings_;
  }
  std::string err;
  bool ok = true;
  auto fail = [&](const char* what) {
    ok = false;
    const std::string e = std::string("cannot set ") + what + ": " + err;
    if (e != error_) LOG_WARN("bluetooth: {}", e);
    set_error(e);
  };
  if (adapter_.powered != s.enabled) {
    if (!set_bool(adapter_.path, kAdapterIface, "Powered", s.enabled, &err)) fail("Powered");
    else LOG_INFO("bluetooth: radio {}", s.enabled ? "on" : "off");
  }
  const std::string alias = s.name.empty() ? hostname() : s.name;
  if (adapter_.name != alias) {
    sd_bus_error e = SD_BUS_ERROR_NULL;
    if (sd_bus_set_property(bus_, kBluez, adapter_.path.c_str(), kAdapterIface, "Alias", &e, "s",
                            alias.c_str()) < 0) {
      err = friendly(&e);
      fail("the name");
    }
    sd_bus_error_free(&e);
  }
  if (adapter_.pairable != s.pairable &&
      !set_bool(adapter_.path, kAdapterIface, "Pairable", s.pairable, &err)) {
    fail("Pairable");
  }
  if (adapter_.discoverable_timeout_s != s.discoverable_timeout_s) {
    sd_bus_error e = SD_BUS_ERROR_NULL;
    if (sd_bus_set_property(bus_, kBluez, adapter_.path.c_str(), kAdapterIface,
                            "DiscoverableTimeout", &e, "u", s.discoverable_timeout_s) < 0) {
      err = friendly(&e);
      fail("DiscoverableTimeout");
    }
    sd_bus_error_free(&e);
  }
  // A refusal is usually BlueZ being busy with the same thing — powering up as it starts, say — so
  // try again on the next read rather than leave the adapter wherever it ended up. Once everything
  // took, an old refusal is no longer true.
  if (!ok) settings_dirty_ = true;
  else if (error_.rfind("cannot set ", 0) == 0) set_error({});
  dirty_ = true;  // read back what BlueZ made of it
}

void BtManager::register_agent() {
  sd_bus_error e = SD_BUS_ERROR_NULL;
  int r = sd_bus_call_method(bus_, kBluez, "/org/bluez", "org.bluez.AgentManager1",
                             "RegisterAgent", &e, nullptr, "os", kAgentPath, kAgentCapability);
  if (r < 0 && !is_error(&e, "org.bluez.Error.AlreadyExists")) {
    LOG_WARN("bluetooth: cannot register the pairing agent: {}", friendly(&e));
    sd_bus_error_free(&e);
    return;
  }
  sd_bus_error_free(&e);
  // The default agent is the one BlueZ asks about pairings nobody started from a client: a phone
  // pairing with us. Without this those would be refused.
  r = sd_bus_call_method(bus_, kBluez, "/org/bluez", "org.bluez.AgentManager1",
                         "RequestDefaultAgent", &e, nullptr, "o", kAgentPath);
  if (r < 0) LOG_WARN("bluetooth: not the default agent: {}", friendly(&e));
  sd_bus_error_free(&e);
  agent_registered_ = true;
  LOG_INFO("bluetooth: pairing agent registered ({})", kAgentCapability);
}

// BlueZ offers the player to every AVRCP controller on the adapter: a speaker's play, pause and
// skip buttons. It drops it by itself when we leave the bus, and BlueZ restarting drops it too.
void BtManager::register_player() {
  sd_bus_message* m = nullptr;
  sd_bus_error e = SD_BUS_ERROR_NULL;
  const BtPlayerInfo& p = local_info_;
  int r = sd_bus_message_new_method_call(bus_, &m, kBluez, adapter_.path.c_str(), "org.bluez.Media1",
                                         "RegisterPlayer");
  if (r >= 0) r = sd_bus_message_append(m, "o", kPlayerPath);
  if (r >= 0) r = sd_bus_message_open_container(m, 'a', "{sv}");
  if (r >= 0) r = sd_bus_message_append(m, "{sv}", "PlaybackStatus", "s", mpris_status(p.status));
  if (r >= 0) r = sd_bus_message_append(m, "{sv}", "Position", "x",
                                        static_cast<int64_t>(p.position_ms) * 1000);
  if (r >= 0) r = sd_bus_message_open_container(m, 'e', "sv");
  if (r >= 0) r = sd_bus_message_append(m, "s", "Metadata");
  if (r >= 0) r = sd_bus_message_open_container(m, 'v', "a{sv}");
  if (r >= 0) r = append_metadata(m, p);
  if (r >= 0) r = sd_bus_message_close_container(m);
  if (r >= 0) r = sd_bus_message_close_container(m);
  for (const char* can : {"CanGoNext", "CanGoPrevious", "CanPlay", "CanPause", "CanControl"})
    if (r >= 0) r = sd_bus_message_append(m, "{sv}", can, "b", 1);
  if (r >= 0) r = sd_bus_message_close_container(m);
  if (r >= 0) r = sd_bus_call(bus_, m, 0, &e, nullptr);
  if (r >= 0 || is_error(&e, "org.bluez.Error.AlreadyExists")) {
    player_registered_ = true;
    LOG_INFO("bluetooth: media player registered: \"{}\" by {}", p.title, p.artist);
  } else if (!player_registered_) {
    // Retried on the next read; logged once per failure.
    const std::string err = std::string("cannot register the media player: ") +
                            (e.message ? friendly(&e) : strerror(-r));
    if (err != player_error_) LOG_WARN("bluetooth: {}", err);
    player_error_ = err;
  }
  if (player_registered_) player_error_.clear();
  sd_bus_error_free(&e);
  sd_bus_message_unref(m);
}

void BtManager::tick_player(uint64_t now_ns) {
  if (!local_.poll || !bus_) return;
  BtPlayerInfo p = local_.poll(now_ns);
  const bool changed = p.status != local_info_.status || p.title != local_info_.title ||
                       p.album != local_info_.album;
  // A new track or a restart moves the position somewhere BlueZ's own count cannot know.
  const bool jumped = changed || p.position_ms < local_info_.position_ms;
  local_info_ = p;
  if (jumped && player_slot_ && player_registered_) {
    sd_bus_emit_properties_changed(bus_, kPlayerPath, kMprisIface, "PlaybackStatus", "Metadata",
                                   "Position", nullptr);
  }
  if (changed || (p.status == "playing" && now_ns - local_published_ns_ >= kPlayerPublishNs)) {
    local_published_ns_ = now_ns;
    stamp_local_player(devices_);
    publish();
  }
}

// On a device we play to, and not one playing to us, the player shown is ours: a speaker's own
// player object is an empty one.
void BtManager::stamp_local_player(std::vector<BtDeviceInfo>& devs) const {
  if (!local_.poll) return;
  for (BtDeviceInfo& d : devs)
    if (d.playback.present && !d.capture.present) d.player = local_info_;
}

bool BtManager::set_bool(const std::string& path, const char* iface, const char* prop, bool v,
                         std::string* err) {
  if (!bus_) {
    if (err) *err = "no bus";
    return false;
  }
  sd_bus_error e = SD_BUS_ERROR_NULL;
  const int r = sd_bus_set_property(bus_, kBluez, path.c_str(), iface, prop, &e, "b",
                                    static_cast<int>(v));
  if (r < 0 && err) *err = friendly(&e);
  sd_bus_error_free(&e);
  dirty_ = true;
  return r >= 0;
}

void BtManager::publish() {
  std::lock_guard<std::mutex> lk(m_);
  status_.available = available_;
  status_.audio = audio_;
  status_.error = error_;
  status_.adapter = adapter_;
  status_.devices = devices_;
  status_.has_request = pending_ != nullptr;
  if (pending_) status_.request = pending_->req;
  request_deadline_ns_ = pending_ ? pending_->deadline_ns : 0;
}

void BtManager::schedule_persist() { persist_at_ns_ = mono_ns() + kPersistDelayNs; }

void BtManager::schedule_persist_after_pairing() {
  schedule_persist();
  persist_late_ns_ = mono_ns() + 10 * kPersistDelayNs;
}

// ---- Scanning ------------------------------------------------------------------------------------

void BtManager::start_scan() {
  if (!bus_ || !available_) return;
  sd_bus_error e = SD_BUS_ERROR_NULL;
  // BR/EDR only: A2DP does not run over LE, and an LE scan in a busy room lists every beacon,
  // watch and phone advertisement in range under a random address.
  sd_bus_call_method(bus_, kBluez, adapter_.path.c_str(), kAdapterIface, "SetDiscoveryFilter", &e,
                     nullptr, "a{sv}", 1, "Transport", "s", "bredr");
  sd_bus_error_free(&e);
  const int r = sd_bus_call_method(bus_, kBluez, adapter_.path.c_str(), kAdapterIface,
                                   "StartDiscovery", &e, nullptr, "");
  if (r < 0 && !is_error(&e, "org.bluez.Error.InProgress")) {
    set_error("cannot scan: " + friendly(&e));
    LOG_WARN("bluetooth: {}", error_);
  } else {
    scan_until_ns_ = mono_ns() + s_to_ns(kBtScanS);
    LOG_INFO("bluetooth: scanning");
  }
  sd_bus_error_free(&e);
  dirty_ = true;
}

void BtManager::stop_scan() {
  scan_until_ns_ = 0;
  if (!bus_ || !available_) return;
  sd_bus_error e = SD_BUS_ERROR_NULL;
  sd_bus_call_method(bus_, kBluez, adapter_.path.c_str(), kAdapterIface, "StopDiscovery", &e,
                     nullptr, "");
  sd_bus_error_free(&e);
  dirty_ = true;
}

// ---- Device operations ---------------------------------------------------------------------------

void BtManager::set_device_state(const std::string& path, std::string busy, std::string error) {
  DevState& s = dev_state_[path];
  s.busy = std::move(busy);
  s.error = std::move(error);
  for (BtDeviceInfo& d : devices_) {
    if (d.path != path) continue;
    d.busy = s.busy;
    d.error = s.error;
  }
  publish();
}

void BtManager::call_device(const std::string& path, const char* method, const char* busy,
                            unsigned timeout_s) {
  if (!bus_) return;
  sd_bus_message* m = nullptr;
  int r = sd_bus_message_new_method_call(bus_, &m, kBluez, path.c_str(), kDeviceIface, method);
  auto* op = new BtBus::AsyncOp{this, path, method};
  sd_bus_slot* slot = nullptr;
  if (r >= 0) r = sd_bus_call_async(bus_, &slot, m, BtBus::async, op, s_to_ns(timeout_s) / 1000);
  sd_bus_message_unref(m);
  if (r < 0) {
    delete op;
    if (std::string(method) == "Pair") started_here_.erase(path);
    set_device_state(path, "", std::string(method) + ": " + strerror(-r));
    return;
  }
  // The slot owns the op from here: freed when the reply has been handled, and equally when the
  // bus is closed with the call still out, which never runs the callback.
  sd_bus_slot_set_destroy_callback(slot, [](void* p) { delete static_cast<BtBus::AsyncOp*>(p); });
  sd_bus_slot_set_floating(slot, 1);
  sd_bus_slot_unref(slot);
  LOG_INFO("bluetooth: {} {}", method, address_from_path(path));
  set_device_state(path, busy, "");
}

void BtManager::call_quiet(const std::string& dev_path, const std::string& path,
                          const char* iface, const char* method) {
  if (!bus_) return;
  sd_bus_error e = SD_BUS_ERROR_NULL;
  if (sd_bus_call_method(bus_, kBluez, path.c_str(), iface, method, &e, nullptr, "") < 0) {
    set_device_state(dev_path, "", std::string(method) + ": " + friendly(&e));
  } else {
    LOG_INFO("bluetooth: {} {}", method, address_from_path(dev_path));
  }
  sd_bus_error_free(&e);
  dirty_ = true;
}

void BtManager::on_reply(const std::string& path, const std::string& method,
                         sd_bus_message* reply) {
  const sd_bus_error* e = sd_bus_message_get_error(reply);
  const std::string addr = address_from_path(path);
  if (method == "Pair") {
    started_here_.erase(path);
    // A device that was already paired is simply connected.
    if (!e || is_error(e, "org.bluez.Error.AlreadyExists")) {
      LOG_INFO("bluetooth: paired {}", addr);
      std::string err;
      if (!set_bool(path, kDeviceIface, "Trusted", true, &err))
        LOG_WARN("bluetooth: cannot trust {}: {}", addr, err);
      schedule_persist();
      call_device(path, "Connect", "connecting", kBtConnectTimeoutS);
      return;
    }
    if (pending_ && pending_->dev_path == path) clear_request(false);
  }
  if (e) LOG_WARN("bluetooth: {} {} failed: {} ({})", method, addr, friendly(e), e->name);
  else LOG_INFO("bluetooth: {} {} done", method, addr);
  set_device_state(path, "", e ? friendly(e) : "");
  dirty_ = true;
}

const BtDeviceInfo* BtManager::find_path(const std::string& path) const {
  for (const BtDeviceInfo& d : devices_)
    if (d.path == path) return &d;
  return nullptr;
}

// ---- The agent -----------------------------------------------------------------------------------

int BtManager::agent_call(sd_bus_message* m, const char* member) {
  // Only BlueZ may ask. The system bus's policy already keeps other users from calling an object
  // root exports; this keeps another root process from pairing a device by impersonating BlueZ.
  const char* sender = sd_bus_message_get_sender(m);
  if (!sender || bluez_owner_.empty() || bluez_owner_ != sender) {
    return sd_bus_reply_method_errorf(m, "org.bluez.Error.Rejected", "not BlueZ");
  }
  const std::string what = member ? member : "";

  if (what == "Release") {
    agent_registered_ = false;
    return sd_bus_reply_method_return(m, "");
  }
  if (what == "Cancel") {
    // BlueZ has given up on the request already; there is no one left to answer.
    if (pending_) LOG_INFO("bluetooth: {} cancelled the pairing", pending_->req.name);
    clear_request(false);
    return sd_bus_reply_method_return(m, "");
  }

  const char* dev = nullptr;
  if (sd_bus_message_read(m, "o", &dev) < 0 || !dev) {
    return sd_bus_reply_method_errorf(m, "org.bluez.Error.Rejected", "bad arguments");
  }
  const std::string path = dev;
  // The agent is BlueZ's default for every adapter, and on a desktop the others are the desktop's
  // own: what pairs with those is not the tester's to say yes to.
  if (adapter_.path.empty() || path.rfind(adapter_.path + "/", 0) != 0) {
    LOG_INFO("bluetooth: {} {} refused: not on this tester's adapter", what, path);
    return sd_bus_reply_method_errorf(m, "org.bluez.Error.Rejected", "not this tester's adapter");
  }
  const auto started = started_here_.find(path);
  const bool here = started != started_here_.end();

  if (what == "DisplayPinCode" || what == "DisplayPasskey") {
    std::string shown;
    if (what == "DisplayPinCode") {
      const char* pin = nullptr;
      sd_bus_message_read(m, "s", &pin);
      shown = pin ? pin : "";
    } else {
      uint32_t key = 0;
      uint16_t entered = 0;
      sd_bus_message_read(m, "uq", &key, &entered);
      char buf[8];
      snprintf(buf, sizeof(buf), "%06u", key % 1000000u);
      shown = buf;
    }
    // Answered at once — there is nothing to say back — but kept on screen until the pairing
    // finishes, since that is the whole point of displaying it.
    const int r = sd_bus_reply_method_return(m, "");
    if (!pending_ || pending_->msg || pending_->dev_path != path || pending_->req.passkey != shown)
      hold_request(nullptr, BtAsk::Passkey, "display", path, shown);
    return r;
  }

  BtAsk ask = BtAsk::Confirm;
  std::string kind = "confirm";
  std::string passkey;
  if (what == "RequestConfirmation") {
    uint32_t key = 0;
    sd_bus_message_read(m, "u", &key);
    char buf[8];
    snprintf(buf, sizeof(buf), "%06u", key % 1000000u);
    passkey = buf;
  } else if (what == "RequestAuthorization") {
    ask = BtAsk::Authorize;
    kind = "authorize";
  } else if (what == "AuthorizeService") {
    ask = BtAsk::Service;
    kind = "authorize";
  } else if (what == "RequestPinCode") {
    ask = BtAsk::Pin;
    kind = "pin";
  } else if (what == "RequestPasskey") {
    ask = BtAsk::Passkey;
    kind = "passkey";
  } else {
    return sd_bus_reply_method_errorf(m, "org.freedesktop.DBus.Error.UnknownMethod", "%s",
                                      what.c_str());
  }

  switch (bt_agent_policy(ask)) {
    case BtVerdict::Accept: {
      LOG_INFO("bluetooth: {} {} — accepted", what, address_from_path(path));
      if (ask != BtAsk::Service) schedule_persist_after_pairing();
      if (ask == BtAsk::Pin) {
        const std::string pin = here && !started->second.empty() ? started->second : "0000";
        return sd_bus_reply_method_return(m, "s", pin.c_str());
      }
      return sd_bus_reply_method_return(m, "");
    }
    case BtVerdict::Reject:
      return sd_bus_reply_method_errorf(m, "org.bluez.Error.Rejected", "refused");
    case BtVerdict::Ask:
      break;
  }
  hold_request(m, ask, kind, path, passkey);
  return 1;
}

void BtManager::hold_request(sd_bus_message* m, BtAsk ask, const std::string& kind,
                             const std::string& dev_path, const std::string& passkey) {
  // BlueZ asks one thing at a time; a new question means the old one is moot.
  clear_request(true);
  auto* p = new Pending;
  p->msg = m ? sd_bus_message_ref(m) : nullptr;
  p->ask = ask;
  p->dev_path = dev_path;
  p->deadline_ns = mono_ns() + s_to_ns(kBtRequestTimeoutS);
  p->req.id = next_request_id_++;
  p->req.kind = kind;
  p->req.passkey = passkey;
  const BtDeviceInfo* d = find_path(dev_path);
  p->req.address = d ? d->address : address_from_path(dev_path);
  p->req.name = d ? d->name : std::string{};
  if (p->req.name.empty() && bus_) {
    // A device that has never been scanned is not in the last read; its name is still known.
    char* alias = nullptr;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    if (sd_bus_get_property_string(bus_, kBluez, dev_path.c_str(), kDeviceIface, "Alias", &e,
                                   &alias) >= 0 && alias) {
      p->req.name = alias;
    }
    free(alias);
    sd_bus_error_free(&e);
  }
  if (p->req.name.empty()) p->req.name = p->req.address;
  pending_ = p;
  if (m) {
    LOG_INFO("bluetooth: {} ({}) asks to {} — waiting for an answer in the console", p->req.name,
             p->req.address, kind);
  }
  publish();
}

void BtManager::clear_request(bool reply_rejected) {
  if (!pending_) return;
  if (pending_->msg) {
    if (reply_rejected) {
      sd_bus_reply_method_errorf(pending_->msg, "org.bluez.Error.Canceled", "superseded");
    }
    sd_bus_message_unref(pending_->msg);
  }
  delete pending_;
  pending_ = nullptr;
  publish();
}

void BtManager::expire_request(uint64_t now_ns) {
  if (!pending_ || now_ns < pending_->deadline_ns) return;
  if (pending_->msg) {
    LOG_INFO("bluetooth: nobody answered {} in time — declined", pending_->req.name);
    sd_bus_reply_method_errorf(pending_->msg, "org.bluez.Error.Canceled",
                               "nobody answered in the console");
  }
  clear_request(false);
}

// ---- Called from web handlers --------------------------------------------------------------------

BtStatus BtManager::status() const {
  std::lock_guard<std::mutex> lk(m_);
  BtStatus s = status_;
  s.settings = settings_;
  if (!s.running) {
    s.available = false;
    s.error = not_running_reason_.empty() ? "Bluetooth is not running" : not_running_reason_;
  }
  if (s.has_request) {
    const uint64_t now = mono_ns();
    s.request.expires_s = request_deadline_ns_ > now
                              ? static_cast<unsigned>((request_deadline_ns_ - now) / 1000000000ull)
                              : 0;
  }
  return s;
}

void BtManager::set_local_player(LocalPlayer p) {
  std::lock_guard<std::mutex> life(life_m_);
  local_ = std::move(p);
}

BtSettings BtManager::settings() const {
  std::lock_guard<std::mutex> lk(m_);
  return settings_;
}

void BtManager::apply(const BtSettings& s) {
  {
    std::lock_guard<std::mutex> lk(m_);
    settings_ = s;
    settings_.discoverable_timeout_s = std::min(settings_.discoverable_timeout_s, kBtDiscoverableMaxS);
  }
  post([this] {
    settings_dirty_ = true;
    dirty_ = true;
    last_refresh_ns_ = 0;
  });
}

void BtManager::set_discoverable(bool on) {
  post([this, on] {
    if (!bus_ || !available_) return;
    if (on && !adapter_.powered) {
      set_error("turn Bluetooth on before making it discoverable");
      publish();
      return;
    }
    // The timeout first: BlueZ starts counting when Discoverable goes on, with the value in force.
    reconcile_adapter();
    std::string err;
    if (!set_bool(adapter_.path, kAdapterIface, "Discoverable", on, &err)) {
      set_error("cannot make it discoverable: " + err);
      publish();
    } else {
      LOG_INFO("bluetooth: discoverable {}", on ? "on" : "off");
    }
  });
}

void BtManager::scan(bool on) {
  post([this, on] {
    if (on) start_scan();
    else stop_scan();
  });
}

bool BtManager::device_action(const std::string& address, Action action, const std::string& pin,
                              std::string* err) {
  if (!running_.load()) {
    if (err) *err = "Bluetooth is not running";
    return false;
  }
  const std::string addr = bt_address_upper(address);
  std::string path;
  {
    std::lock_guard<std::mutex> lk(m_);
    for (BtDeviceInfo& d : status_.devices) {
      if (d.address != addr) continue;
      path = d.path;
      // Shown at once, so a console that reads the state straight back sees its request running.
      if (action == Action::Pair) d.busy = "pairing";
      if (action == Action::Connect) d.busy = "connecting";
      if (action == Action::Disconnect) d.busy = "disconnecting";
      d.error.clear();
    }
  }
  if (path.empty()) {
    if (err) *err = "no such device";
    return false;
  }
  post([this, path, action, pin] {
    if (!bus_) return;
    switch (action) {
      case Action::Pair:
        started_here_[path] = pin;
        call_device(path, "Pair", "pairing", kBtPairTimeoutS);
        break;
      case Action::Connect:
        call_device(path, "Connect", "connecting", kBtConnectTimeoutS);
        break;
      case Action::Disconnect:
        call_device(path, "Disconnect", "disconnecting", kBtConnectTimeoutS);
        break;
      case Action::Remove: {
        sd_bus_error e = SD_BUS_ERROR_NULL;
        if (sd_bus_call_method(bus_, kBluez, adapter_.path.c_str(), kAdapterIface, "RemoveDevice",
                               &e, nullptr, "o", path.c_str()) < 0) {
          set_device_state(path, "", "cannot forget it: " + friendly(&e));
        } else {
          LOG_INFO("bluetooth: forgot {}", address_from_path(path));
          dev_state_.erase(path);
          schedule_persist();
        }
        sd_bus_error_free(&e);
        dirty_ = true;
        break;
      }
    }
  });
  return true;
}

// The device's path and one of its parts, looked up by address under the lock, for a handler that
// has to say 404 before it posts anything.
static bool find_device(const std::vector<BtDeviceInfo>& devs, const std::string& address,
                        BtDeviceInfo* out) {
  const std::string addr = bt_address_upper(address);
  for (const BtDeviceInfo& d : devs) {
    if (d.address != addr) continue;
    *out = d;
    return true;
  }
  return false;
}

bool BtManager::set_volume(const std::string& address, int volume, std::string* err) {
  if (!running_.load()) {
    if (err) *err = "Bluetooth is not running";
    return false;
  }
  BtDeviceInfo d;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!find_device(status_.devices, address, &d)) {
      if (err) *err = "no such device";
      return false;
    }
  }
  if (d.transport_path.empty() || d.volume < 0) {
    if (err) *err = "no volume: the device has no audio link, or no absolute volume";
    return false;
  }
  const uint16_t v = static_cast<uint16_t>(std::clamp(volume, 0, 127));
  post([this, d, v] {
    if (!bus_) return;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    if (sd_bus_set_property(bus_, kBluez, d.transport_path.c_str(), "org.bluez.MediaTransport1",
                            "Volume", &e, "q", v) < 0) {
      // BlueZ only tells a device about a volume change it has asked to be told about; a phone
      // playing to us often has not, and BlueZ says so as ENOENT in an Internal error.
      const std::string why = e.message && strstr(e.message, "(-2)")
                                  ? "the device does not take its volume from this side"
                                  : friendly(&e);
      set_device_state(d.path, "", "cannot set the volume: " + why);
    }
    sd_bus_error_free(&e);
    dirty_ = true;
  });
  return true;
}

bool BtManager::player_command(const std::string& address, const std::string& command,
                               std::string* err) {
  if (!running_.load()) {
    if (err) *err = "Bluetooth is not running";
    return false;
  }
  std::string method;
  if (!bt_player_command(command, &method)) {
    if (err) *err = "command must be play, pause, stop, next or previous";
    return false;
  }
  BtDeviceInfo d;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!find_device(status_.devices, address, &d)) {
      if (err) *err = "no such device";
      return false;
    }
  }
  if (!d.player.present) {
    if (err) *err = "no media player: the device is not playing to this one";
    return false;
  }
  if (d.player.local) {
    post([this, command] {
      if (local_.command) local_.command(command);
      tick_player(mono_ns());
    });
    return true;
  }
  post([this, d, method] { call_quiet(d.path, d.player.path, "org.bluez.MediaPlayer1", method.c_str()); });
  return true;
}

bool BtManager::answer(uint64_t id, bool accept, const std::string& value, std::string* err) {
  if (!running_.load()) {
    if (err) *err = "Bluetooth is not running";
    return false;
  }
  auto done = std::make_shared<std::promise<std::string>>();
  std::future<std::string> result = done->get_future();
  post([this, id, accept, value, done] {
    if (!pending_ || pending_->req.id != id) {
      done->set_value("no such request — it was answered, withdrawn or has expired");
      return;
    }
    Pending& p = *pending_;
    if (!p.msg) {  // a code on display: nothing to say back, only to stop showing it
      clear_request(false);
      done->set_value({});
      return;
    }
    int r = 0;
    if (!accept) {
      LOG_INFO("bluetooth: {} declined in the console", p.req.name);
      r = sd_bus_reply_method_errorf(p.msg, "org.bluez.Error.Rejected", "declined in the console");
    } else if (p.req.kind == "pin") {
      if (value.empty() || value.size() > 16) {
        done->set_value("a PIN is 1 to 16 characters");
        return;
      }
      r = sd_bus_reply_method_return(p.msg, "s", value.c_str());
    } else if (p.req.kind == "passkey") {
      char* end = nullptr;
      const unsigned long v = strtoul(value.c_str(), &end, 10);
      if (value.empty() || *end || v > 999999ul) {
        done->set_value("a passkey is six digits");
        return;
      }
      r = sd_bus_reply_method_return(p.msg, "u", static_cast<uint32_t>(v));
    } else {
      r = sd_bus_reply_method_return(p.msg, "");
    }
    if (accept) {
      LOG_INFO("bluetooth: {} accepted in the console", p.req.name);
      schedule_persist_after_pairing();
    }
    clear_request(false);
    done->set_value(r < 0 ? std::string("BlueZ is gone: ") + strerror(-r) : std::string{});
  });
  if (result.wait_for(kAnswerWait) != std::future_status::ready) {
    if (err) *err = "Bluetooth is not answering";
    return false;
  }
  const std::string e = result.get();
  if (!e.empty()) {
    if (err) *err = e;
    return false;
  }
  return true;
}

bool BtManager::capture_source(std::string* address, std::string* name, unsigned* rate,
                               unsigned* channels) const {
  std::lock_guard<std::mutex> lk(m_);
  const BtDeviceInfo* best = nullptr;
  for (const BtDeviceInfo& d : status_.devices) {
    if (!d.capture.present) continue;
    if (!best || d.capture.sequence > best->capture.sequence) best = &d;
  }
  if (!best || !status_.running) return false;
  *address = best->address;
  *name = best->name;
  *rate = best->capture.rate;
  *channels = best->capture.channels;
  return true;
}

std::string BtManager::latest_sink() const {
  std::lock_guard<std::mutex> lk(m_);
  const BtDeviceInfo* best = nullptr;
  for (const BtDeviceInfo& d : status_.devices) {
    if (!d.playback.present) continue;
    if (!best || d.playback.sequence > best->playback.sequence) best = &d;
  }
  return best ? best->address : std::string{};
}

std::string BtManager::device_name(const std::string& address) const {
  const std::string addr = bt_address_upper(address);
  std::lock_guard<std::mutex> lk(m_);
  for (const BtDeviceInfo& d : status_.devices)
    if (d.address == addr) return d.name;
  return {};
}

BtPcmInfo BtManager::playback_of(const std::string& address) const {
  const std::string addr = bt_address_upper(address);
  std::lock_guard<std::mutex> lk(m_);
  for (const BtDeviceInfo& d : status_.devices)
    if (d.address == addr) return d.playback;
  return {};
}

void BtManager::on_bonds_changed(std::function<void()> fn) {
  std::lock_guard<std::mutex> lk(m_);
  bonds_hook_ = std::move(fn);
}

}  // namespace st
