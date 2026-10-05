#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "constants.h"

typedef struct sd_bus sd_bus;
typedef struct sd_bus_message sd_bus_message;
typedef struct sd_bus_slot sd_bus_slot;

namespace st {

// ---- The parts that are decisions rather than D-Bus traffic, exposed so they can be tested ------

// "AA:BB:CC:DD:EE:FF", hex in either case. What the API accepts in a URL.
bool bt_address_ok(const std::string& a);
std::string bt_address_upper(const std::string& a);

// BlueZ's object for a device on an adapter: /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF.
std::string bt_device_path(const std::string& adapter_path, const std::string& address);

// The bluez-alsa PCM that plays to, or captures from, one device; and back. An address of all
// zeros, or a PCM name with none, means bluez-alsa's "most recently connected", reported as "".
std::string bt_pcm_name(const std::string& address);
std::string bt_pcm_address(const std::string& pcm);

// What an A2DP device can do, from the service UUIDs it advertises: a sink takes audio (a
// speaker), a source sends it (a phone). A laptop can be both.
struct BtRoles {
  bool sink = false;
  bool source = false;
};
BtRoles bt_roles(const std::vector<std::string>& uuids);

// What a device asked the agent, and what the agent does about it.
enum class BtAsk {
  Confirm,    // numeric comparison: both sides show a six-digit code
  Authorize,  // a just-works pairing the other side started
  Pin,        // a legacy device wants a PIN
  Passkey,    // type the code the other side shows
  Service,    // a device wants to use a profile
};
enum class BtVerdict { Accept, Reject, Ask };

// The whole of the agent's policy: a bench instrument says yes to everything, the way a speaker
// with no screen does — every pairing, whoever started it, and every profile. The one exception
// is a passkey to type, which cannot be guessed; it is shown in the console instead.
BtVerdict bt_agent_policy(BtAsk ask);

// ---- State as the console sees it --------------------------------------------------------------

// One bluez-alsa audio link with a device.
struct BtPcmInfo {
  bool present = false;
  std::string codec;      // "SBC", ...
  unsigned rate = 0;
  unsigned channels = 0;
  double delay_ms = 0.0;  // codec and radio delay, as the device reports it
  uint32_t sequence = 0;  // larger = connected more recently
};

// A device's AVRCP media player: what a phone playing to us says it is playing, and the remote
// control for it.
struct BtPlayerInfo {
  bool present = false;
  std::string path;
  std::string status;  // playing | paused | stopped | forward-seek | reverse-seek | error
  std::string title;
  std::string artist;
  std::string album;
  uint32_t duration_ms = 0;
  uint32_t position_ms = 0;  // extrapolated while playing: BlueZ reports it only on changes
  // The tester's own player (bt_player.h), shown on a speaker it plays to: its buttons work the
  // Bluetooth output rather than the device.
  bool local = false;
};

// What the player buttons send. The values are MediaPlayer1's method names.
bool bt_player_command(const std::string& in, std::string* method);

struct BtDeviceInfo {
  std::string path;
  std::string address;
  std::string name;  // BlueZ's alias, which is the device's own name until someone changes it
  std::string icon;  // freedesktop icon name: audio-card, audio-headphones, phone, computer, ...
  bool paired = false;
  bool trusted = false;
  bool connected = false;
  bool has_rssi = false;  // only while scanning
  int rssi = 0;
  BtRoles roles;
  std::string busy;   // "pairing" | "connecting" | "disconnecting" while one runs
  std::string error;  // the last request's failure, kept until the next one
  BtPcmInfo playback;  // the device is our speaker
  BtPcmInfo capture;   // the device is streaming to us
  BtPlayerInfo player;  // its AVRCP media player, when it has one
  // AVRCP absolute volume of its audio link, 0..127, -1 when the link has none.
  int volume = -1;
  std::string transport_path;
};

struct BtAdapterInfo {
  std::string path;
  std::string address;
  std::string name;
  bool powered = false;
  bool discoverable = false;
  bool pairable = false;
  bool discovering = false;
  unsigned discoverable_timeout_s = 0;
};

// What the operator configured. BlueZ holds the live values; these are what the manager keeps it
// at, and what a save writes.
struct BtSettings {
  bool enabled = true;
  std::string name;  // empty: the hostname
  bool pairable = true;
  unsigned discoverable_timeout_s = kBtDiscoverableDefaultS;
};

// A pairing question waiting for the operator.
struct BtRequest {
  uint64_t id = 0;
  std::string kind;  // confirm | authorize | pin | passkey | display (see docs/api.md)
  std::string address;
  std::string name;
  std::string passkey;  // shown with confirm and display, zero-padded to six digits
  unsigned expires_s = 0;
};

struct BtStatus {
  bool running = false;    // the manager was started at all
  bool available = false;  // BlueZ answers and has an adapter
  bool audio = false;      // bluez-alsa answers
  std::string error;       // why not available, or the last adapter-level failure
  BtAdapterInfo adapter;
  BtSettings settings;
  std::vector<BtDeviceInfo> devices;
  bool has_request = false;
  BtRequest request;
};

// Runs the Pi's Bluetooth over BlueZ's D-Bus API: keeps the adapter at the configured settings,
// scans, pairs, connects and forgets devices, and is the pairing agent, so that every question a
// pairing asks reaches the web console instead of a terminal nobody has open. bluez-alsa's PCMs
// are read alongside, so the console can say which devices are carrying audio.
//
// One thread owns the bus connection: sd-bus is not thread-safe, and an agent that answers late
// costs a pairing. Web handlers queue a request and return; a long operation (pairing can take a
// minute while someone finds their phone) reports its outcome on the device, not to the caller.
// Like the card, nothing here is fatal: no bus, no BlueZ or no adapter is a status line, retried.
class BtManager {
 public:
  explicit BtManager(BtSettings settings);
  ~BtManager();
  BtManager(const BtManager&) = delete;
  BtManager& operator=(const BtManager&) = delete;

  // Both idempotent. A manager never started reports `reason` as its error: the simulator does
  // not take over the Bluetooth of the machine it is run on.
  bool start();
  void stop();
  void set_not_running_reason(std::string reason);

  BtStatus status() const;
  BtSettings settings() const;

  // All of these return at once; the bus thread carries them out.
  void apply(const BtSettings& s);
  void set_discoverable(bool on);
  void scan(bool on);

  enum class Action { Pair, Connect, Disconnect, Remove };
  // False, with a reason, when there is no such device (a 404) or the manager is not running.
  bool device_action(const std::string& address, Action action, const std::string& pin,
                     std::string* err);
  // The device's absolute volume, 0..127, over AVRCP.
  bool set_volume(const std::string& address, int volume, std::string* err);
  // play | pause | stop | next | previous, to the device's media player.
  bool player_command(const std::string& address, const std::string& command, std::string* err);
  // Answers the request `id`. `value` is the PIN or the passkey for those kinds.
  bool answer(uint64_t id, bool accept, const std::string& value, std::string* err);

  // The device that most recently started streaming to this one, which is what the Bluetooth input
  // records. False while none is.
  bool capture_source(std::string* address, std::string* name, unsigned* rate,
                      unsigned* channels) const;
  // The device bluez-alsa's "most recently connected" playback PCM is, which is where an output
  // pointed at DEV=00:00:00:00:00:00 actually plays. Empty while none is.
  std::string latest_sink() const;
  // A device's name by address, for labels; empty when unknown.
  std::string device_name(const std::string& address) const;
  // A device's playback link, for the output's status: the delay it reports.
  BtPcmInfo playback_of(const std::string& address) const;

  // The tester's own media player, which BlueZ offers over AVRCP to whatever it plays to. `poll`
  // is called on the bus thread every tick, and a speaker's buttons (or the console's, on a speaker's
  // card) reach `command`, also on the bus thread. Set before start().
  struct LocalPlayer {
    std::function<BtPlayerInfo(uint64_t now_ns)> poll;
    std::function<bool(const std::string& cmd)> command;
  };
  void set_local_player(LocalPlayer p);

  // Called on the bus thread a couple of seconds after the set of paired devices changes, so the
  // keys BlueZ has just written can be copied somewhere that survives a reboot.
  void on_bonds_changed(std::function<void()> fn);

 private:
  struct Pending;  // an agent request held open while the operator decides
  struct DevState {
    std::string busy;
    std::string error;
  };

  void run();
  bool open_bus();
  void close_bus();
  void post(std::function<void()> fn);
  void drain_commands();
  void tick();
  void refresh();
  void reconcile_adapter();
  void register_agent();
  void register_player();
  // Polls the local player, tells BlueZ what changed and puts it on the devices it plays to.
  void tick_player(uint64_t now_ns);
  void stamp_local_player(std::vector<BtDeviceInfo>& devs) const;
  void publish();
  void expire_request(uint64_t now_ns);
  void clear_request(bool reply_rejected);
  void set_error(std::string e);

  // Asynchronous Device1 methods; the reply lands in on_reply().
  void call_device(const std::string& path, const char* method, const char* busy,
                   unsigned timeout_s);
  // A quick method call whose failure is only worth showing on the device.
  void call_quiet(const std::string& dev_path, const std::string& path, const char* iface,
                  const char* method);
  void on_reply(const std::string& path, const std::string& method, sd_bus_message* reply);
  void set_device_state(const std::string& path, std::string busy, std::string error);

  bool set_bool(const std::string& path, const char* iface, const char* prop, bool v,
                std::string* err);
  void start_scan();
  void stop_scan();
  void schedule_persist();
  // After a pairing question is accepted. The set of paired devices may not change at all — a
  // phone re-pairing a device it has forgotten gets a new key under the same address — and the key
  // is written whenever the other side finishes, so copy once soon and once again later.
  void schedule_persist_after_pairing();

  // Agent methods, on the bus thread.
  int agent_call(sd_bus_message* m, const char* member);
  void hold_request(sd_bus_message* m, BtAsk ask, const std::string& kind,
                    const std::string& dev_path, const std::string& passkey);
  const BtDeviceInfo* find_path(const std::string& path) const;

  // The sd-bus callbacks, which need the members above; defined in bluetooth.cpp.
  friend struct BtBus;

  // Shared with web handlers.
  mutable std::mutex m_;
  BtStatus status_;
  BtSettings settings_;
  std::string not_running_reason_;
  std::deque<std::function<void()>> commands_;
  std::function<void()> bonds_hook_;
  uint64_t request_deadline_ns_ = 0;

  std::mutex life_m_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<int> wake_fd_{-1};

  // Bus thread only.
  sd_bus* bus_ = nullptr;
  sd_bus_slot* agent_slot_ = nullptr;
  sd_bus_slot* player_slot_ = nullptr;
  LocalPlayer local_;
  BtPlayerInfo local_info_;
  bool player_registered_ = false;
  std::string player_error_;
  uint64_t local_published_ns_ = 0;
  std::vector<sd_bus_slot*> match_slots_;
  std::string bluez_owner_;  // BlueZ's unique name: the only caller the agent answers
  bool agent_registered_ = false;
  bool dirty_ = true;
  bool settings_dirty_ = true;
  uint64_t last_refresh_ns_ = 0;
  uint64_t scan_until_ns_ = 0;
  uint64_t persist_at_ns_ = 0;
  uint64_t persist_late_ns_ = 0;  // a second copy, for a key that lands after the first
  std::string adapter_path_;  // the adapter last reconciled; a new one is reconciled again
  std::vector<BtDeviceInfo> devices_;
  BtAdapterInfo adapter_;
  bool available_ = false;
  bool audio_ = false;
  std::string error_;
  std::set<std::string> paired_;  // addresses, for noticing a change
  bool paired_known_ = false;
  std::map<std::string, DevState> dev_state_;          // by device path
  std::map<std::string, std::string> started_here_;    // device path -> the PIN to offer
  // Position as BlueZ last reported it, and when that was seen: by player path.
  std::map<std::string, std::pair<uint32_t, uint64_t>> positions_;
  Pending* pending_ = nullptr;
  uint64_t next_request_id_ = 1;
};

}  // namespace st
