// The parts of the Bluetooth manager that are decisions rather than D-Bus traffic: what an address
// looks like, how it becomes a BlueZ object and a bluez-alsa PCM and back, what a device can do,
// what the agent answers, and which player commands reach the bus.
// The traffic itself is exercised against tools/fake-bluez, which a unit test cannot stand up.
#include <string>
#include <vector>

#include "bluetooth.h"
#include "bt_player.h"
#include "check.h"
#include "channel_layout.h"
#include "sink_out.h"

using namespace st;

namespace {

void test_addresses() {
  CHECK(bt_address_ok("5C:E9:1E:22:40:01"));
  CHECK(bt_address_ok("5c:e9:1e:22:40:01"));
  CHECK(!bt_address_ok("5C:E9:1E:22:40"));
  CHECK(!bt_address_ok("5C-E9-1E-22-40-01"));
  CHECK(!bt_address_ok("5C:E9:1E:22:40:0G"));
  CHECK(!bt_address_ok(""));
  CHECK(!bt_address_ok("../../../etc/pass"));
  CHECK_EQ(bt_address_upper("5c:e9:1e:22:40:0a"), std::string("5C:E9:1E:22:40:0A"));
}

void test_device_paths() {
  CHECK_EQ(bt_device_path("/org/bluez/hci0", "5c:e9:1e:22:40:01"),
           std::string("/org/bluez/hci0/dev_5C_E9_1E_22_40_01"));
}

// A board runs its only radio, or hci0 when a dongle joins it. A desktop names the one that is the
// tester's, by hci name or by address, and gets nothing at all when that one is not there: never
// the desktop's own.
void test_adapter_choice() {
  const std::vector<BtAdapterId> one = {{"/org/bluez/hci0", "B8:27:EB:50:7B:22"}};
  const std::vector<BtAdapterId> two = {{"/org/bluez/hci1", "00:1B:DC:08:4B:CC"},
                                        {"/org/bluez/hci0", "44:A3:BB:36:5E:2E"}};
  CHECK_EQ(bt_pick_adapter(one, ""), std::string("/org/bluez/hci0"));
  CHECK_EQ(bt_pick_adapter(two, ""), std::string("/org/bluez/hci0"));
  CHECK_EQ(bt_pick_adapter({}, ""), std::string());
  CHECK_EQ(bt_pick_adapter(two, "hci1"), std::string("/org/bluez/hci1"));
  CHECK_EQ(bt_pick_adapter(two, "00:1b:dc:08:4b:cc"), std::string("/org/bluez/hci1"));
  CHECK_EQ(bt_pick_adapter(two, "44:A3:BB:36:5E:2E"), std::string("/org/bluez/hci0"));
  CHECK_EQ(bt_pick_adapter(one, "hci1"), std::string());
  CHECK_EQ(bt_pick_adapter(one, "00:1B:DC:08:4B:CC"), std::string());
  CHECK_EQ(bt_pick_adapter(two, "hci"), std::string());
}

// The output's device is a PCM name, and the console needs the speaker behind it. An address of
// zeros is bluez-alsa's "most recently connected", which has no address to report.
void test_pcm_names_round_trip() {
  CHECK_EQ(bt_pcm_name("f8:df:15:0a:11:3c"), std::string("bluealsa:DEV=F8:DF:15:0A:11:3C,PROFILE=a2dp"));
  CHECK_EQ(bt_pcm_name(""), std::string(kBtDefaultDevice));
  CHECK_EQ(bt_pcm_address(bt_pcm_name("F8:DF:15:0A:11:3C")), std::string("F8:DF:15:0A:11:3C"));
  CHECK_EQ(bt_pcm_address(kBtDefaultDevice), std::string());
  CHECK_EQ(bt_pcm_address("bluealsa:F8:DF:15:0A:11:3C"), std::string("F8:DF:15:0A:11:3C"));
  CHECK_EQ(bt_pcm_address("bluealsa:PROFILE=a2dp,DEV=f8:df:15:0a:11:3c"),
           std::string("F8:DF:15:0A:11:3C"));
  CHECK_EQ(bt_pcm_address("hw:Headphones,0"), std::string());
  CHECK_EQ(bt_pcm_address("bluealsa:DEV=nonsense"), std::string());
}

void test_roles_come_from_the_uuids() {
  const BtRoles speaker = bt_roles({"0000110B-0000-1000-8000-00805F9B34FB",
                                    "0000110c-0000-1000-8000-00805f9b34fb"});
  CHECK(speaker.sink);
  CHECK(!speaker.source);
  const BtRoles phone = bt_roles({"0000110a-0000-1000-8000-00805f9b34fb",
                                  "0000111f-0000-1000-8000-00805f9b34fb"});
  CHECK(phone.source);
  CHECK(!phone.sink);
  const BtRoles keyboard = bt_roles({"00001124-0000-1000-8000-00805f9b34fb"});
  CHECK(!keyboard.sink && !keyboard.source);
  CHECK(!bt_roles({}).sink);
}

// A bench instrument says yes to everything, the way a speaker with no screen does. The only
// thing it cannot answer by itself is a passkey it would have to read off the other device.
void test_the_agent_policy() {
  CHECK_EQ(bt_agent_policy(BtAsk::Confirm), BtVerdict::Accept);
  CHECK_EQ(bt_agent_policy(BtAsk::Authorize), BtVerdict::Accept);
  CHECK_EQ(bt_agent_policy(BtAsk::Pin), BtVerdict::Accept);
  CHECK_EQ(bt_agent_policy(BtAsk::Service), BtVerdict::Accept);
  CHECK_EQ(bt_agent_policy(BtAsk::Passkey), BtVerdict::Ask);
}

// The player buttons map onto MediaPlayer1's methods, and nothing else gets through to the bus.
void test_player_commands() {
  std::string m;
  CHECK(bt_player_command("play", &m) && m == "Play");
  CHECK(bt_player_command("pause", &m) && m == "Pause");
  CHECK(bt_player_command("stop", &m) && m == "Stop");
  CHECK(bt_player_command("next", &m) && m == "Next");
  CHECK(bt_player_command("previous", &m) && m == "Previous");
  CHECK(!bt_player_command("Play", &m));
  CHECK(!bt_player_command("FastForward", &m));
  CHECK(!bt_player_command("", &m));
}

// The tester's own player: its track is the output's signal, and a speaker's buttons work the
// output the way the console does.
void test_the_tester_player() {
  Control ctl;
  SinkControl& c = ctl.sinks[3];  // whichever slot the Bluetooth sink was bound to
  bool output_switched = false;
  BtTesterPlayer p(ctl, c, [&](bool on) {
    output_switched = true;
    c.enabled.store(on);
  });
  const uint64_t s = 1000000000ull;

  BtPlayerInfo i = p.poll(1 * s);
  CHECK(i.present && i.local);
  CHECK_EQ(i.title, std::string("Silence"));
  CHECK_EQ(i.artist, std::string("Sound Tester"));
  CHECK_EQ(i.status, std::string("stopped"));  // the output is off

  // Play switches the output on; next walks the signals on both channels.
  CHECK(p.command("play"));
  CHECK(output_switched && c.enabled.load());
  CHECK(p.command("next"));
  CHECK_EQ(bt_tester_title(c), std::string("Sine"));
  CHECK_EQ(c.outputs[1].source.load(), pack_source(SourceType::Gen, 0));
  CHECK_EQ(bt_tester_detail(ctl, c), std::string("440 Hz · -20 dBFS"));
  i = p.poll(2 * s);
  CHECK_EQ(i.status, std::string("playing"));
  CHECK_EQ(i.position_ms, 0u);
  CHECK_EQ(p.poll(5 * s).position_ms, 3000u);

  // Pause mutes, and holds the position; play unmutes and counts on.
  CHECK(p.command("pause"));
  CHECK(c.outputs[0].mute.load() && c.outputs[1].mute.load());
  i = p.poll(6 * s);
  CHECK_EQ(i.status, std::string("paused"));
  CHECK_EQ(p.poll(9 * s).position_ms, 4000u);
  CHECK(p.command("playpause"));
  CHECK(!c.outputs[0].mute.load());
  CHECK_EQ(p.poll(10 * s).position_ms, 4000u);
  CHECK_EQ(p.poll(11 * s).position_ms, 5000u);

  // A channel muted from the console alone is still playing; both muted is paused.
  c.outputs[0].mute.store(true);
  CHECK_EQ(std::string(bt_tester_status(c)), std::string("playing"));
  c.outputs[1].mute.store(true);
  CHECK_EQ(std::string(bt_tester_status(c)), std::string("paused"));
  c.outputs[0].mute.store(false);
  c.outputs[1].mute.store(false);

  // Round the cycle and back; a new signal starts the position again.
  CHECK(p.command("next"));
  CHECK_EQ(p.poll(12 * s).title, std::string("Noise"));
  CHECK_EQ(p.poll(12 * s).position_ms, 0u);
  CHECK(p.command("next") && p.command("next"));
  CHECK_EQ(bt_tester_title(c), std::string("Music"));
  CHECK(p.command("next"));
  CHECK_EQ(bt_tester_title(c), std::string("Silence"));
  CHECK(p.command("previous"));
  CHECK_EQ(bt_tester_title(c), std::string("Music"));

  // From an input, next starts at the beginning and previous at the end.
  c.outputs[0].source.store(pack_source(SourceType::Input, 2));
  c.outputs[1].source.store(pack_source(SourceType::Input, channels().net_base()));
  CHECK_EQ(bt_tester_title(c), std::string("IN 3 / NET 1"));
  CHECK_EQ(bt_tester_detail(ctl, c), std::string());
  CHECK(p.command("next"));
  CHECK_EQ(bt_tester_title(c), std::string("Silence"));
  c.outputs[0].source.store(pack_source(SourceType::Input, 2));
  CHECK(p.command("previous"));
  CHECK_EQ(bt_tester_title(c), std::string("Music"));

  // Stop switches the output off, which is what stopped means.
  CHECK(p.command("stop"));
  CHECK(!c.enabled.load());
  CHECK_EQ(p.poll(20 * s).status, std::string("stopped"));
  CHECK(!p.command("eject"));
}

// A speaker's reported codec delay is not the output's to hold; see SinkDevice::local_queue. Only
// the Bluetooth sink sets it: every device the scan finds holds snd_pcm_delay().
void test_the_bluetooth_sink() {
  CHECK(!SinkDevice{}.local_queue);
}

}  // namespace

int main() {
  test_addresses();
  test_device_paths();
  test_adapter_choice();
  test_pcm_names_round_trip();
  test_roles_come_from_the_uuids();
  test_the_agent_policy();
  test_player_commands();
  test_the_tester_player();
  test_the_bluetooth_sink();
  return report("bluetooth");
}
