#include "config.h"

#include <iostream>

#include "check.h"
#include "control.h"

using namespace st;

namespace {

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
  a.sine_freq_hz = 996.09375f;
  a.noise_mode = "pink";
  a.ping_variant = "bong";
  a.ping_interval_s = 3.5f;
  a.input_map = {5, 4, 3, 2, 1, 0};
  a.output_map = {7, 6, 5, 4, 3, 2, 1, 0};
  a.input_names[0] = "left speaker";
  a.loopback_offset_samples = 4321;

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
  CHECK_EQ(b.sine_freq_hz, 996.09375f);
  CHECK_EQ(b.noise_mode, std::string("pink"));
  CHECK_EQ(b.ping_variant, std::string("bong"));
  CHECK_EQ(b.ping_interval_s, 3.5f);
  CHECK_EQ(b.input_map[0], 5);
  CHECK_EQ(b.output_map[0], 7);
  CHECK_EQ(b.input_names[0], std::string("left speaker"));
  CHECK_EQ(b.loopback_offset_samples, 4321);
}

void test_control_round_trip() {
  Config a;
  a.outputs[2].source_type = "input";
  a.outputs[2].source_index = "4";
  a.outputs[2].gain_db = -6.0f;
  a.outputs[5].source_type = "gen";
  a.outputs[5].source_index = "noise";
  a.inputs[1].gain_db = 12.0f;
  a.noise_mode = "pink";
  a.ping_variant = "bing";
  a.input_map = {1, 0, 2, 3, 4, 5};

  Control ctl;
  a.apply_to(ctl);

  CHECK_EQ(ctl.inputs[1].gain_db.load(), 12.0f);

  CHECK_EQ(source_type(ctl.outputs[2].source.load()), SourceType::Input);
  CHECK_EQ(source_index(ctl.outputs[2].source.load()), 4);
  CHECK_EQ(source_type(ctl.outputs[5].source.load()), SourceType::Gen);
  CHECK_EQ(source_index(ctl.outputs[5].source.load()), static_cast<uint8_t>(GenId::Noise));
  CHECK_EQ(ctl.input_map[0].load(), 1);

  const Config b = Config::from_control(ctl, a);
  CHECK_EQ(b.inputs[1].gain_db, 12.0f);
  CHECK_EQ(b.outputs[2].source_type, std::string("input"));
  CHECK_EQ(b.outputs[2].source_index, std::string("4"));
  CHECK_EQ(b.outputs[2].gain_db, -6.0f);
  CHECK_EQ(b.outputs[5].source_type, std::string("gen"));
  CHECK_EQ(b.outputs[5].source_index, std::string("noise"));
  CHECK_EQ(b.noise_mode, std::string("pink"));
  CHECK_EQ(b.ping_variant, std::string("bing"));
  CHECK_EQ(b.input_map[0], 1);
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
// gain must be clamped on the way in, not trusted. +80 dB would be 10000x on the capture path.
void test_input_gain_is_clamped() {
  Config c;
  c.inputs[0].gain_db = 80.0f;
  c.inputs[1].gain_db = -30.0f;

  Control ctl;
  c.apply_to(ctl);

  CHECK_EQ(ctl.inputs[0].gain_db.load(), kInputGainMaxDb);
  CHECK_EQ(ctl.inputs[1].gain_db.load(), kInputGainMinDb);
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
  test_input_gain_is_clamped();
  test_garbage_is_rejected();
  return report("config");
}
