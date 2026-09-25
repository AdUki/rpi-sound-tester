#include "config.h"

#include <stdlib.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "check.h"
#include "control.h"

using namespace st;

namespace {

std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Byte-for-byte, and where the first difference is when there is one: a 4 kB file printed whole
// twice says nothing a person can find.
bool same_bytes(const std::string& got, const std::string& want) {
  if (got == want) return true;
  size_t i = 0;
  while (i < got.size() && i < want.size() && got[i] == want[i]) ++i;
  std::cout << "  first difference at byte " << i << " of " << want.size() << ": got \""
            << got.substr(i > 40 ? i - 40 : 0, 80) << "\"\n  want \""
            << want.substr(i > 40 ? i - 40 : 0, 80) << "\"\n";
  return false;
}

void test_json_round_trip() {
  Config a;
  a.outputs[0].source_type = "input";
  a.outputs[0].source_index = "3";
  a.outputs[0].gain_db = -12.0f;
  a.outputs[1].source_type = "gen";
  a.outputs[1].source_index = "ping";
  a.outputs[1].mute = true;
  a.inputs[2].gain_db = 18.5f;
  a.sine_freq_hz = 996.09375f;
  a.sine_level_db = -12.0f;
  a.noise_mode = "pink";
  a.noise_level_db = -18.0f;
  a.ping_variant = "bong";
  a.ping_interval_s = 3.5f;
  a.ping_level_db = -24.0f;
  a.music_level_db = -9.0f;
  a.outputs[2].source_type = "gen";
  a.outputs[2].source_index = "music";
  a.input_map = {5, 4, 3, 2, 1, 0};
  a.output_map = {7, 6, 5, 4, 3, 2, 1, 0};
  a.input_names[0] = "left speaker";
  a.output_names[7] = "sub";
  a.loopback_offset_samples = 4321;
  a.listen_codec = "opus";
  a.listen_bitrate_kbps = 128;
  a.net_enabled = true;
  a.net_port = 4321;
  a.net_delay_ms = 750;

  Config b;
  std::string err;
  CHECK(Config::from_json(a.to_json(), &b, &err));
  if (!err.empty()) std::cout << "  parse error: " << err << "\n";

  CHECK_EQ(b.outputs[0].source_type, std::string("input"));
  CHECK_EQ(b.outputs[0].source_index, std::string("3"));
  CHECK_EQ(b.outputs[0].gain_db, -12.0f);
  CHECK_EQ(b.outputs[1].source_type, std::string("gen"));
  CHECK_EQ(b.outputs[1].source_index, std::string("ping"));
  CHECK_EQ(b.outputs[1].mute, true);
  CHECK_EQ(b.inputs[2].gain_db, 18.5f);
  CHECK_EQ(b.inputs[0].gain_db, 0.0f);
  CHECK_EQ(b.sine_freq_hz, 996.09375f);
  CHECK_EQ(b.sine_level_db, -12.0f);
  CHECK_EQ(b.noise_mode, std::string("pink"));
  CHECK_EQ(b.noise_level_db, -18.0f);
  CHECK_EQ(b.ping_variant, std::string("bong"));
  CHECK_EQ(b.ping_interval_s, 3.5f);
  CHECK_EQ(b.ping_level_db, -24.0f);
  CHECK_EQ(b.music_level_db, -9.0f);
  CHECK_EQ(b.outputs[2].source_index, std::string("music"));
  CHECK_EQ(b.input_map[0], 5);
  CHECK_EQ(b.output_map[0], 7);
  CHECK_EQ(b.input_names[0], std::string("left speaker"));
  CHECK_EQ(b.output_names[7], std::string("sub"));
  CHECK_EQ(b.loopback_offset_samples, 4321);
  CHECK_EQ(b.listen_codec, std::string("opus"));
  CHECK_EQ(b.listen_bitrate_kbps, 128);
  CHECK(b.net_enabled);
  CHECK_EQ(b.net_port, 4321);
  CHECK_EQ(b.net_delay_ms, 750);
}

// The capture delay is derived from enabled + delay_ms in two places — Config::apply_to and the
// live PUT handler — and must come out zero whenever network input is off, or a device with no
// remote sender would quietly shift its own capture by a second.
void test_capture_delay_is_zero_unless_network_input_is_on() {
  Control ctl;
  Config c;
  c.net_delay_ms = 1000;

  c.net_enabled = false;
  c.apply_to(ctl, 96000);
  CHECK_EQ(ctl.net.delay_frames.load(), 0u);
  CHECK_EQ(ctl.net.delay_ms.load(), 1000u);  // remembered, just not in force

  c.net_enabled = true;
  c.apply_to(ctl, 96000);
  CHECK_EQ(ctl.net.delay_frames.load(), 96000u);

  // Out of range clamps rather than being taken literally.
  c.net_delay_ms = 999999;
  c.apply_to(ctl, 96000);
  CHECK_EQ(ctl.net.delay_ms.load(), kNetDelayMaxMs);
  CHECK_EQ(ctl.net.delay_frames.load(), static_cast<uint32_t>(1ull * kNetDelayMaxMs * 96000 / 1000));

  // And it survives the snapshot back out to a Config.
  const Config back = Config::from_control(ctl, c);
  CHECK(back.net_enabled);
  CHECK_EQ(back.net_delay_ms, static_cast<int>(kNetDelayMaxMs));
}

void test_control_round_trip() {
  Config a;
  a.outputs[2].source_type = "input";
  a.outputs[2].source_index = "4";
  a.outputs[2].gain_db = -6.0f;
  a.outputs[5].source_type = "gen";
  a.outputs[5].source_index = "noise";
  a.outputs[6].source_type = "gen";
  a.outputs[6].source_index = "music";
  a.music_level_db = -14.0f;
  a.inputs[1].gain_db = 12.0f;
  a.noise_mode = "pink";
  a.ping_variant = "bing";
  a.input_map = {1, 0, 2, 3, 4, 5};
  a.listen_codec = "opus";
  a.listen_bitrate_kbps = 64;

  Control ctl;
  a.apply_to(ctl, 96000);

  CHECK_EQ(ctl.inputs[1].gain_db.load(), 12.0f);
  CHECK_EQ(ctl.listen.codec.load(), static_cast<uint8_t>(ListenCodec::Opus));
  CHECK_EQ(ctl.listen.bitrate_kbps.load(), 64);

  CHECK_EQ(source_type(ctl.outputs[2].source.load()), SourceType::Input);
  CHECK_EQ(source_index(ctl.outputs[2].source.load()), 4);
  CHECK_EQ(source_type(ctl.outputs[5].source.load()), SourceType::Gen);
  CHECK_EQ(source_index(ctl.outputs[5].source.load()), static_cast<uint8_t>(GenId::Noise));
  CHECK_EQ(source_index(ctl.outputs[6].source.load()), static_cast<uint8_t>(GenId::Music));
  CHECK_EQ(ctl.music.level_db.load(), -14.0f);
  CHECK_EQ(ctl.input_map[0].load(), 1);

  const Config b = Config::from_control(ctl, a);
  CHECK_EQ(b.inputs[1].gain_db, 12.0f);
  CHECK_EQ(b.outputs[2].source_type, std::string("input"));
  CHECK_EQ(b.outputs[2].source_index, std::string("4"));
  CHECK_EQ(b.outputs[2].gain_db, -6.0f);
  CHECK_EQ(b.outputs[5].source_type, std::string("gen"));
  CHECK_EQ(b.outputs[5].source_index, std::string("noise"));
  CHECK_EQ(b.outputs[6].source_index, std::string("music"));
  CHECK_EQ(b.music_level_db, -14.0f);
  CHECK_EQ(b.noise_mode, std::string("pink"));
  CHECK_EQ(b.ping_variant, std::string("bing"));
  CHECK_EQ(b.input_map[0], 1);
  CHECK_EQ(b.listen_codec, std::string("opus"));
  CHECK_EQ(b.listen_bitrate_kbps, 64);
}

void test_defaults_are_silent_and_identity_mapped() {
  Config c;
  Control ctl;
  c.apply_to(ctl, 96000);
  for (unsigned i = 0; i < kOutputs; ++i) {
    CHECK_EQ(source_type(ctl.outputs[i].source.load()), SourceType::Silence);
    CHECK_EQ(ctl.outputs[i].mute.load(), false);
    CHECK_EQ(ctl.output_map[i].load(), static_cast<uint8_t>(i));
  }
  for (unsigned i = 0; i < kInputs; ++i) CHECK_EQ(ctl.input_map[i].load(), static_cast<uint8_t>(i));
}

// A saved config is a file on a partition anyone with the SD card can edit, so an out-of-range
// value must be clamped on the way in, not trusted. +80 dB would be 10000x on the capture path.
void test_saved_values_are_clamped() {
  Config c;
  c.inputs[0].gain_db = 80.0f;
  c.inputs[1].gain_db = -30.0f;
  c.outputs[0].gain_db = 10.0f;
  c.sine_level_db = 10.0f;
  c.ping_interval_s = 0.01f;
  c.music_level_db = 6.0f;

  Control ctl;
  c.apply_to(ctl, 96000);

  CHECK_EQ(ctl.inputs[0].gain_db.load(), kInputGainMaxDb);
  CHECK_EQ(ctl.inputs[1].gain_db.load(), kInputGainMinDb);
  CHECK_EQ(ctl.outputs[0].gain_db.load(), kLevelMaxDb);
  CHECK_EQ(ctl.sine.level_db.load(), kLevelMaxDb);
  CHECK_EQ(ctl.ping.interval_s.load(), kPingIntervalMinS);
  CHECK_EQ(ctl.music.level_db.load(), kLevelMaxDb);
}

// A hand-edited output source may omit "index" entirely; the parse must fall back, not abort
// the daemon at boot.
void test_source_without_index_parses() {
  Config c;
  std::string err;
  CHECK(Config::from_json(R"({"outputs": [{"source": {"type": "silence"}}]})", &c, &err));
  CHECK_EQ(c.outputs[0].source_type, std::string("silence"));
  CHECK_EQ(c.outputs[0].source_index, std::string(""));
}

// The network port rides Control like every other saved setting. It used to live only in the
// server, so a port changed through the API was never saved.
void test_net_port_rides_the_control_path() {
  Config c;
  c.net_port = 4321;
  Control ctl;
  c.apply_to(ctl, 96000);
  CHECK_EQ(ctl.net.port.load(), 4321);

  ctl.net.port.store(4555);  // what PUT /api/net does, enabled or not
  CHECK_EQ(Config::from_control(ctl, c).net_port, 4555);

  // A hand-edited file cannot ask for a port whose per-channel ports would run off the end.
  c.net_port = 70000;
  c.apply_to(ctl, 96000);
  CHECK_EQ(static_cast<int>(ctl.net.port.load()), kNetPortMax);
  c.net_port = 0;
  c.apply_to(ctl, 96000);
  CHECK_EQ(static_cast<int>(ctl.net.port.load()), kNetPortMin);
}

// Sinks ride the file by device id, every channel's routing and name with them, whether or not the
// device is plugged in: Devices applies them when it finds the device.
void test_sink_round_trip() {
  Config a;
  SinkConfig& hdmi = a.sinks["b1,0"];
  hdmi.enabled = true;
  hdmi.sample_rate = 44100;
  hdmi.layout = "5.1";
  hdmi.outputs[5].source_type = "gen";
  hdmi.outputs[5].source_index = "ping";
  hdmi.outputs[1].source_type = "input";
  hdmi.outputs[1].source_index = "7";
  hdmi.outputs[1].mute = true;
  hdmi.names[1] = "tv right";
  SinkConfig& usb = a.sinks["Device,0"];
  usb.outputs[0].source_type = "gen";
  usb.outputs[0].source_index = "music";
  usb.outputs[0].gain_db = -3.0f;

  Config b;
  std::string err;
  CHECK(Config::from_json(a.to_json(), &b, &err));
  CHECK_EQ(b.sinks.size(), 2u);
  const SinkConfig& h = b.sinks["b1,0"];
  CHECK(h.enabled);
  CHECK_EQ(h.sample_rate, 44100u);
  CHECK_EQ(h.layout, std::string("5.1"));
  CHECK_EQ(h.outputs[5].source_index, std::string("ping"));
  CHECK_EQ(h.outputs[1].source_type, std::string("input"));
  CHECK_EQ(h.outputs[1].source_index, std::string("7"));
  CHECK(h.outputs[1].mute);
  CHECK_EQ(h.names[1], std::string("tv right"));
  const SinkConfig& u = b.sinks["Device,0"];
  CHECK(!u.enabled);
  CHECK_EQ(u.sample_rate, 0u);          // the device's default
  CHECK_EQ(u.layout, std::string());    // likewise
  CHECK_EQ(u.outputs[0].source_index, std::string("music"));
  CHECK_EQ(u.outputs[0].gain_db, -3.0f);

  // No sinks is none; the factory file ships none.
  Config c;
  CHECK(Config::from_json("{}", &c, &err));
  CHECK(c.sinks.empty());
}

// A file saved before sinks were found at runtime has the Pi's two under keys of their own, each
// naming its device: they load as the sinks of those devices.
void test_old_sink_keys_are_read() {
  Config c;
  std::string err;
  CHECK(Config::from_json(R"({
      "hdmi": {"enabled": true, "device": "hw:b1,0", "sample_rate": 48000, "layout": "5.1",
               "outputs": [{"source": {"type": "gen", "index": "music"}, "gain_db": -3.0,
                            "mute": false}],
               "names": ["soundbar L"]},
      "lineout": {"enabled": false, "device": "hw:Headphones,0", "sample_rate": 44100,
                  "outputs": [{"source": {"type": "input", "index": "3"}, "gain_db": 0.0,
                               "mute": true}]},
      "rate": 96000, "device": "hw:audioinjectoroc,0"})", &c, &err));
  CHECK_EQ(c.sinks.size(), 2u);
  CHECK(c.sinks["b1,0"].enabled);
  CHECK_EQ(c.sinks["b1,0"].layout, std::string("5.1"));
  CHECK_EQ(c.sinks["b1,0"].outputs[0].source_index, std::string("music"));
  CHECK_EQ(c.sinks["b1,0"].names[0], std::string("soundbar L"));
  CHECK_EQ(c.sinks["Headphones,0"].sample_rate, 44100u);
  CHECK(c.sinks["Headphones,0"].outputs[0].mute);

  // One the file also has under "sinks" is that one's.
  CHECK(Config::from_json(R"({"sinks": {"b1,0": {"layout": "7.1"}},
                              "hdmi": {"device": "hw:b1,0", "layout": "5.1"}})", &c, &err));
  CHECK_EQ(c.sinks["b1,0"].layout, std::string("7.1"));
  // A device that is not hardware has no id to be kept under.
  CHECK(Config::from_json(R"({"hdmi": {"device": "default"}})", &c, &err));
  CHECK(c.sinks.empty());
}

// ---- The Pi's file, byte for byte ---------------------------------------------------------------

// tests/data/pi-config-v1.json is this config as Config::to_json() wrote it before sinks were found
// at runtime: routes to an ADC ("3"), a network channel ("7") and the melody, a few names, and HDMI
// in 5.1. It was written once by that code and is never regenerated, so it is what every Pi that
// saved its settings has on /data. It must still load to the same routing.
//
// tests/data/pi-config-v2.json is what this code writes for it, and is pinned the same way: a
// change that alters a single byte of what a Pi reads or writes fails here rather than on a Pi.
// Like the file a save leaves on /data, neither ends in a newline.
void check_pi_config(const Config& c) {
  CHECK_EQ(c.outputs[0].source_index, std::string("3"));
  CHECK_EQ(c.outputs[0].gain_db, -6.0f);
  CHECK_EQ(c.outputs[1].source_index, std::string("7"));
  CHECK(c.outputs[1].mute);
  CHECK_EQ(c.outputs[2].source_index, std::string("music"));
  CHECK_EQ(c.input_names[0], std::string("bench mic"));
  CHECK_EQ(c.input_names[7], std::string("laptop"));
  CHECK_EQ(c.output_names[7], std::string("sub"));
  CHECK_EQ(c.sinks.size(), 2u);
  const auto hdmi = c.sinks.find("b1,0");
  const auto jack = c.sinks.find("Headphones,0");
  CHECK(hdmi != c.sinks.end() && jack != c.sinks.end());
  if (hdmi == c.sinks.end() || jack == c.sinks.end()) return;
  CHECK(hdmi->second.enabled);
  CHECK_EQ(hdmi->second.layout, std::string("5.1"));
  CHECK_EQ(hdmi->second.sample_rate, 48000u);
  CHECK_EQ(hdmi->second.outputs[kSpkL].source_index, std::string("music"));
  CHECK_EQ(hdmi->second.outputs[kSpkC].source_index, std::string("3"));
  CHECK_EQ(hdmi->second.outputs[kSpkLfe].source_index, std::string("7"));
  CHECK_EQ(hdmi->second.outputs[kSpkLfe].gain_db, -20.0f);
  CHECK_EQ(hdmi->second.names[kSpkLfe], std::string("subwoofer"));
  CHECK(!jack->second.enabled);
  CHECK_EQ(jack->second.outputs[0].source_index, std::string("3"));
  CHECK_EQ(jack->second.names[1], std::string("desk right"));
}

void test_the_pi_config_file_still_loads() {
  const std::string v1 = read_file(ST_TEST_DATA_DIR "/pi-config-v1.json");
  const std::string v2 = read_file(ST_TEST_DATA_DIR "/pi-config-v2.json");
  CHECK(!v1.empty() && !v2.empty());
  if (v1.empty() || v2.empty()) return;

  Config c;
  std::string err;
  CHECK(Config::from_json(v1, &c, &err));
  if (!err.empty()) std::cout << "  parse error: " << err << "\n";
  check_pi_config(c);
  CHECK(same_bytes(c.to_json(), v2));

  // The new file reads back to the same, and writes the same.
  Config d;
  CHECK(Config::from_json(v2, &d, &err));
  check_pi_config(d);
  CHECK(same_bytes(d.to_json(), v2));

  // And the way a Pi does both: loaded as the boot defaults, saved again.
  const char* tmp = getenv("TMPDIR");
  std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/st-config-XXXXXX";
  CHECK(mkdtemp(dir.data()) != nullptr);
  ConfigStore store(ST_TEST_DATA_DIR "/pi-config-v1.json", dir);
  const Config loaded = store.load();
  CHECK(store.save(loaded, &err));
  CHECK(same_bytes(read_file(store.saved_path()), v2));
  unlink(store.saved_path().c_str());
  rmdir(dir.c_str());
}

void test_garbage_is_rejected() {
  Config c;
  std::string err;
  CHECK(!Config::from_json("{not json", &c, &err));
  CHECK(!err.empty());
}

}  // namespace

int main() {
  test_json_round_trip();
  test_control_round_trip();
  test_defaults_are_silent_and_identity_mapped();
  test_saved_values_are_clamped();
  test_source_without_index_parses();
  test_garbage_is_rejected();
  test_capture_delay_is_zero_unless_network_input_is_on();
  test_net_port_rides_the_control_path();
  test_sink_round_trip();
  test_old_sink_keys_are_read();
  test_the_pi_config_file_still_loads();
  return report("config");
}
