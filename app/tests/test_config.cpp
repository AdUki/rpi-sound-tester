#include "config.h"

#include <stdlib.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "board_profile.h"
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
  a.rate = 48000;
  a.period = 2048;
  a.device = "hw:foo,0";
  a.outputs[0].source_type = "input";
  a.outputs[0].source_index = "3";
  a.outputs[0].gain_db = -12.0f;
  a.outputs[1].source_type = "gen";
  a.outputs[1].source_index = "ping";
  a.outputs[1].mute = true;
  a.inputs[2].gain_db = 18.5f;
  a.periods = 6;
  a.capture_channels = 6;
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

  CHECK_EQ(b.rate, 48000u);
  CHECK_EQ(b.period, 2048u);
  CHECK_EQ(b.device, std::string("hw:foo,0"));
  CHECK_EQ(b.outputs[0].source_type, std::string("input"));
  CHECK_EQ(b.outputs[0].source_index, std::string("3"));
  CHECK_EQ(b.outputs[0].gain_db, -12.0f);
  CHECK_EQ(b.outputs[1].source_type, std::string("gen"));
  CHECK_EQ(b.outputs[1].source_index, std::string("ping"));
  CHECK_EQ(b.outputs[1].mute, true);
  CHECK_EQ(b.inputs[2].gain_db, 18.5f);
  CHECK_EQ(b.inputs[0].gain_db, 0.0f);
  CHECK_EQ(b.periods, 6u);
  CHECK_EQ(b.capture_channels, 6u);
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
  c.rate = 96000;
  c.net_delay_ms = 1000;

  c.net_enabled = false;
  c.apply_to(ctl);
  CHECK_EQ(ctl.net.delay_frames.load(), 0u);
  CHECK_EQ(ctl.net.delay_ms.load(), 1000u);  // remembered, just not in force

  c.net_enabled = true;
  c.apply_to(ctl);
  CHECK_EQ(ctl.net.delay_frames.load(), 96000u);

  // Out of range clamps rather than being taken literally.
  c.net_delay_ms = 999999;
  c.apply_to(ctl);
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
  a.apply_to(ctl);

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
  c.apply_to(ctl);
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
  c.apply_to(ctl);

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
  c.apply_to(ctl);
  CHECK_EQ(ctl.net.port.load(), 4321);

  ctl.net.port.store(4555);  // what PUT /api/net does, enabled or not
  CHECK_EQ(Config::from_control(ctl, c).net_port, 4555);

  // A hand-edited file cannot ask for a port whose per-channel ports would run off the end.
  c.net_port = 70000;
  c.apply_to(ctl);
  CHECK_EQ(static_cast<int>(ctl.net.port.load()), kNetPortMax);
  c.net_port = 0;
  c.apply_to(ctl);
  CHECK_EQ(static_cast<int>(ctl.net.port.load()), kNetPortMin);
}

// The HDMI block rides the same file and the same Config<->Control path as everything else.
void test_hdmi_round_trip() {
  Config a;
  a.hdmi.enabled = true;
  a.hdmi.device = "hw:ALSA,1";
  a.hdmi.sample_rate = 44100;
  a.hdmi.layout = "5.1";
  a.hdmi.outputs[5].source_type = "gen";
  a.hdmi.outputs[5].source_index = "ping";
  a.hdmi.outputs[0].source_type = "gen";
  a.hdmi.outputs[0].source_index = "music";
  a.hdmi.outputs[0].gain_db = -3.0f;
  a.hdmi.outputs[1].source_type = "input";
  a.hdmi.outputs[1].source_index = "7";
  a.hdmi.outputs[1].mute = true;
  a.hdmi.names[1] = "tv right";

  Config b;
  std::string err;
  CHECK(Config::from_json(a.to_json(), &b, &err));
  CHECK(b.hdmi.enabled);
  CHECK_EQ(b.hdmi.device, std::string("hw:ALSA,1"));
  CHECK_EQ(b.hdmi.sample_rate, 44100u);
  CHECK_EQ(b.hdmi.layout, std::string("5.1"));
  CHECK_EQ(b.hdmi.outputs[5].source_index, std::string("ping"));
  CHECK_EQ(b.hdmi.outputs[0].source_index, std::string("music"));
  CHECK_EQ(b.hdmi.outputs[0].gain_db, -3.0f);
  CHECK_EQ(b.hdmi.outputs[1].source_type, std::string("input"));
  CHECK_EQ(b.hdmi.outputs[1].source_index, std::string("7"));
  CHECK(b.hdmi.outputs[1].mute);
  CHECK_EQ(b.hdmi.names[1], std::string("tv right"));

  Control ctl;
  b.apply_to(ctl);
  CHECK(ctl.hdmi.enabled.load());
  CHECK_EQ(soc_layout(ctl.hdmi, kHdmiMaxChannels), HdmiLayout::S51);
  CHECK_EQ(source_index(ctl.hdmi_outputs[5].source.load()), static_cast<uint8_t>(GenId::Ping));
  CHECK_EQ(source_type(ctl.hdmi_outputs[0].source.load()), SourceType::Gen);
  CHECK_EQ(source_index(ctl.hdmi_outputs[0].source.load()), static_cast<uint8_t>(GenId::Music));
  CHECK_EQ(source_index(ctl.hdmi_outputs[1].source.load()), 7);
  // The Octo's outputs and the line out are untouched by HDMI.
  CHECK_EQ(source_type(ctl.lineout_outputs[1].source.load()), SourceType::Silence);
  CHECK_EQ(source_type(ctl.outputs[0].source.load()), SourceType::Silence);

  ctl.hdmi.enabled.store(false);
  ctl.hdmi.layout.store(static_cast<uint8_t>(HdmiLayout::S71));
  ctl.hdmi_outputs[1].gain_db.store(-12.0f);
  const Config c = Config::from_control(ctl, b);
  CHECK(!c.hdmi.enabled);
  CHECK_EQ(c.hdmi.layout, std::string("7.1"));
  CHECK_EQ(c.hdmi.outputs[1].gain_db, -12.0f);
  CHECK_EQ(c.hdmi.outputs[0].source_index, std::string("music"));
  CHECK_EQ(c.hdmi.device, std::string("hw:ALSA,1"));  // not live: carried over from the base
}

// A config written before HDMI existed must load with HDMI off, and a rate the HDMI path cannot
// use falls back to the one every sink accepts rather than being passed to the driver.
void test_hdmi_defaults_and_bad_values() {
  Config c;
  std::string err;
  CHECK(Config::from_json(R"({"rate": 96000})", &c, &err));
  CHECK(!c.hdmi.enabled);
  CHECK_EQ(c.hdmi.device, rpi3_octo_profile().sink("hdmi")->device);
  CHECK_EQ(c.hdmi.sample_rate, kSocRateDefault);
  CHECK_EQ(c.hdmi.names.size(), static_cast<size_t>(kHdmiMaxChannels));
  CHECK_EQ(c.hdmi.layout, std::string("stereo"));

  CHECK(Config::from_json(R"({"hdmi": {"sample_rate": 22050,
      "outputs": [{"source": {"type": "gen", "index": "nope"}, "gain_db": 12.0}]}})", &c, &err));
  CHECK_EQ(c.hdmi.sample_rate, kSocRateDefault);
  Control ctl;
  c.apply_to(ctl);

  // A layout that does not exist is stereo, and surround asked for at a rate the Pi cannot carry
  // it at comes back at 48 kHz rather than reaching the firmware.
  Config odd;
  CHECK(Config::from_json(R"({"hdmi": {"layout": "5"}})", &odd, &err));
  CHECK_EQ(odd.hdmi.layout, std::string("stereo"));
  CHECK(Config::from_json(R"({"hdmi": {"layout": "7.1", "sample_rate": 96000}})", &odd, &err));
  CHECK_EQ(odd.hdmi.layout, std::string("7.1"));
  CHECK_EQ(odd.hdmi.sample_rate, 48000u);
  CHECK(Config::from_json(R"({"hdmi": {"layout": "stereo", "sample_rate": 96000}})", &odd, &err));
  CHECK_EQ(odd.hdmi.sample_rate, 96000u);
  // Every HDMI audio rate is offered for stereo, and only those.
  CHECK(Config::from_json(R"({"hdmi": {"layout": "stereo", "sample_rate": 192000}})", &odd, &err));
  CHECK_EQ(odd.hdmi.sample_rate, 192000u);
  CHECK(Config::from_json(R"({"hdmi": {"layout": "5.1", "sample_rate": 32000}})", &odd, &err));
  CHECK_EQ(odd.hdmi.sample_rate, 32000u);
  CHECK(Config::from_json(R"({"hdmi": {"layout": "5.1", "sample_rate": 88200}})", &odd, &err));
  CHECK_EQ(odd.hdmi.sample_rate, 48000u);
  for (unsigned r : {32000u, 44100u, 48000u, 88200u, 96000u, 176400u, 192000u})
    CHECK(soc_rate_ok(r));
  for (unsigned r : {0u, 8000u, 22050u, 64000u, 384000u}) CHECK(!soc_rate_ok(r));
  odd.hdmi.layout = "bogus";
  odd.apply_to(ctl);
  CHECK_EQ(soc_layout(ctl.hdmi, kHdmiMaxChannels), HdmiLayout::Stereo);
  c.apply_to(ctl);
  CHECK_EQ(source_type(ctl.hdmi_outputs[0].source.load()), SourceType::Silence);
  CHECK_EQ(ctl.hdmi_outputs[0].gain_db.load(), kLevelMaxDb);

  // The shipped defaults parse, and ship HDMI off.
  CHECK(!Config{}.hdmi.enabled);
}

// The line out rides the same file and path as HDMI, but has no layout: it is stereo whatever a
// file or a stray control value says, and the file does not carry one.
void test_lineout_round_trip() {
  Config a;
  a.lineout.enabled = true;
  a.lineout.device = "hw:ALSA,0";
  a.lineout.sample_rate = 44100;
  a.lineout.outputs[0].source_type = "gen";
  a.lineout.outputs[0].source_index = "music";
  a.lineout.outputs[1].source_type = "input";
  a.lineout.outputs[1].source_index = "3";
  a.lineout.outputs[1].gain_db = -6.0f;
  a.lineout.names[0] = "desk left";

  const std::string text = a.to_json();
  CHECK(text.find("\"lineout\"") != std::string::npos);
  Config b;
  std::string err;
  CHECK(Config::from_json(text, &b, &err));
  CHECK(b.lineout.enabled);
  CHECK_EQ(b.lineout.device, std::string("hw:ALSA,0"));
  CHECK_EQ(b.lineout.sample_rate, 44100u);
  CHECK_EQ(b.lineout.outputs.size(), static_cast<size_t>(kLineoutChannels));
  CHECK_EQ(b.lineout.outputs[0].source_index, std::string("music"));
  CHECK_EQ(b.lineout.outputs[1].gain_db, -6.0f);
  CHECK_EQ(b.lineout.names[0], std::string("desk left"));
  CHECK_EQ(b.lineout.names.size(), static_cast<size_t>(kLineoutChannels));

  Control ctl;
  b.apply_to(ctl);
  CHECK(ctl.lineout.enabled.load());
  CHECK(!ctl.hdmi.enabled.load());
  CHECK_EQ(source_index(ctl.lineout_outputs[0].source.load()), static_cast<uint8_t>(GenId::Music));
  CHECK_EQ(source_index(ctl.lineout_outputs[1].source.load()), 3);
  CHECK_EQ(source_type(ctl.hdmi_outputs[0].source.load()), SourceType::Silence);
  CHECK_EQ(soc_layout(ctl.lineout, kLineoutChannels), HdmiLayout::Stereo);

  ctl.lineout.enabled.store(false);
  ctl.lineout_outputs[0].mute.store(true);
  const Config c = Config::from_control(ctl, b);
  CHECK(!c.lineout.enabled);
  CHECK(c.lineout.outputs[0].mute);
  CHECK_EQ(c.lineout.device, std::string("hw:ALSA,0"));  // not live: carried over from the base

  // A layout in the file is not the line out's to have.
  Config odd;
  CHECK(Config::from_json(R"({"lineout": {"layout": "7.1", "sample_rate": 22050,
      "names": ["a", "b", "c"]}})", &odd, &err));
  CHECK_EQ(odd.lineout.layout, std::string("stereo"));
  CHECK_EQ(odd.lineout.sample_rate, kSocRateDefault);
  CHECK_EQ(odd.lineout.names.size(), static_cast<size_t>(kLineoutChannels));
  odd.apply_to(ctl);
  CHECK_EQ(ctl.lineout.layout.load(), static_cast<uint8_t>(HdmiLayout::Stereo));

  // Defaults: off, on the jack's own card.
  CHECK(!Config{}.lineout.enabled);
  CHECK_EQ(Config{}.lineout.device, rpi3_octo_profile().sink("lineout")->device);
}

// A file that leaves the clock out gets the board's.
void test_defaults_are_the_boards() {
  const BoardProfile& board = rpi3_octo_profile();
  Config c;
  std::string err;
  CHECK(Config::from_json("{}", &c, &err));
  CHECK_EQ(c.rate, board.clock.rate);
  CHECK_EQ(c.period, board.clock.period);
  CHECK_EQ(c.periods, board.clock.periods);
  CHECK_EQ(c.device, board.clock.capture_device);
  CHECK_EQ(c.capture_channels, board.clock.capture_slots.front());
  CHECK_EQ(c.hdmi.device, board.sink("hdmi")->device);
  CHECK_EQ(c.lineout.device, board.sink("lineout")->device);
  CHECK_EQ(c.hdmi.outputs.size(), static_cast<size_t>(board.sink("hdmi")->width));
  CHECK_EQ(c.lineout.outputs.size(), static_cast<size_t>(board.sink("lineout")->width));
}

// ---- The Pi's file, byte for byte ---------------------------------------------------------------

// tests/data/pi-config-v1.json is this config as Config::to_json() wrote it before there were board
// profiles: routes to an ADC ("3"), a network channel ("7") and the melody, a few names, and HDMI
// in 5.1. It was written once by that code and is never regenerated from this one, so a change
// that alters a single byte of what a Pi reads or writes fails here rather than on a Pi. Like the
// file a save leaves on /data, it does not end in a newline.
Config pi_config_v1() {
  Config c;
  c.outputs[0].source_type = "input";
  c.outputs[0].source_index = "3";
  c.outputs[0].gain_db = -6.0f;
  c.outputs[1].source_type = "input";
  c.outputs[1].source_index = "7";
  c.outputs[1].mute = true;
  c.outputs[2].source_type = "gen";
  c.outputs[2].source_index = "music";
  c.outputs[2].gain_db = -12.0f;
  c.input_names[0] = "bench mic";
  c.input_names[3] = "DUT left";
  c.input_names[7] = "laptop";
  c.output_names[0] = "to DUT";
  c.output_names[7] = "sub";
  c.hdmi.enabled = true;
  c.hdmi.layout = "5.1";
  c.hdmi.sample_rate = 48000;
  c.hdmi.outputs[kSpkL].source_type = "gen";
  c.hdmi.outputs[kSpkL].source_index = "music";
  c.hdmi.outputs[kSpkR].source_type = "gen";
  c.hdmi.outputs[kSpkR].source_index = "music";
  c.hdmi.outputs[kSpkC].source_type = "input";
  c.hdmi.outputs[kSpkC].source_index = "3";
  c.hdmi.outputs[kSpkLfe].source_type = "input";
  c.hdmi.outputs[kSpkLfe].source_index = "7";
  c.hdmi.outputs[kSpkLfe].gain_db = -20.0f;
  c.hdmi.names[kSpkL] = "soundbar L";
  c.hdmi.names[kSpkLfe] = "subwoofer";
  c.lineout.outputs[0].source_type = "input";
  c.lineout.outputs[0].source_index = "3";
  c.lineout.names[1] = "desk right";
  return c;
}

void test_the_pi_config_file_is_unchanged() {
  const std::string golden = read_file(ST_TEST_DATA_DIR "/pi-config-v1.json");
  CHECK(!golden.empty());
  if (golden.empty()) return;

  // What the daemon writes for it.
  CHECK(same_bytes(pi_config_v1().to_json(), golden));

  // What it reads from it, written back out.
  Config c;
  std::string err;
  CHECK(Config::from_json(golden, &c, &err));
  if (!err.empty()) std::cout << "  parse error: " << err << "\n";
  CHECK(same_bytes(c.to_json(), golden));
  CHECK_EQ(c.outputs[1].source_index, std::string("7"));
  CHECK_EQ(c.hdmi.layout, std::string("5.1"));

  // And the way a Pi does both: loaded as the factory defaults, saved as the boot defaults.
  const char* tmp = getenv("TMPDIR");
  std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/st-config-XXXXXX";
  CHECK(mkdtemp(dir.data()) != nullptr);
  ConfigStore store(ST_TEST_DATA_DIR "/pi-config-v1.json", dir);
  const Config loaded = store.load();
  CHECK(store.save(loaded, &err));
  CHECK(same_bytes(read_file(store.saved_path()), golden));
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
  test_hdmi_round_trip();
  test_hdmi_defaults_and_bad_values();
  test_lineout_round_trip();
  test_defaults_are_the_boards();
  test_the_pi_config_file_is_unchanged();
  return report("config");
}
