#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "board_profile.h"
#include "constants.h"
#include "control.h"

namespace st {

struct InputConfig {
  // Make-up gain: 0..+40 dB on an ADC channel, and down to -60 dB on a network one, which has no
  // ADC to have clipped and so nothing to hide by attenuating.
  float gain_db = 0.0f;
  bool mute = false;
};

struct OutputConfig {
  std::string source_type = "silence";  // silence | input | gen
  std::string source_index;             // "0".."11" for input, a gen_name() for gen
  float gain_db = 0.0f;
  bool mute = false;
};

// One of the SoC's own outputs: HDMI or the line out. `device` is an ALSA name, and
// `sample_rate` the rate its PCM is opened at (never "rate": the image recipe patches every "rate"
// key in config.json to the card's). `outputs` and `names` hold one entry per speaker the sink can
// have, playing or not.
struct SocConfig {
  SocConfig(std::string dev, unsigned width)
      : device(std::move(dev)), outputs(width), names(width) {}

  bool enabled = false;
  std::string device;
  unsigned sample_rate = kSocRateDefault;
  std::string layout = hdmi_layout_name(kHdmiLayoutDefault);  // mono | stereo | 5.1 | 7.1
  std::vector<OutputConfig> outputs;
  std::vector<std::string> names;
};

struct Config {
  // The compiled-in board's clock, until a file says otherwise.
  unsigned rate = rpi3_octo_profile().clock.rate;
  unsigned period = rpi3_octo_profile().clock.period;
  unsigned periods = rpi3_octo_profile().clock.periods;
  std::string device = rpi3_octo_profile().clock.capture_device;
  unsigned capture_channels = rpi3_octo_profile().clock.capture_slots.front();

  std::array<InputConfig, kTotalInputs> inputs{};
  std::array<OutputConfig, kOutputs> outputs{};
  float sine_freq_hz = 440.0f;
  float sine_level_db = -20.0f;
  std::string noise_mode = "white";
  float noise_level_db = -20.0f;
  std::string ping_variant = "tick";
  float ping_interval_s = 2.0f;
  float ping_level_db = -20.0f;
  float music_level_db = -20.0f;

  std::array<uint8_t, kInputs> input_map{{0, 1, 2, 3, 4, 5}};
  std::array<uint8_t, kOutputs> output_map{{0, 1, 2, 3, 4, 5, 6, 7}};

  std::vector<std::string> input_names{kTotalInputs};
  std::vector<std::string> output_names{kOutputs};

  int64_t loopback_offset_samples = 0;

  // Listen stream: the codec the console defaults to ("pcm" | "opus") and the per-channel Opus
  // bitrate. The wire default is always PCM; this only sets the console's preference and the
  // stream.ogg default.
  std::string listen_codec = "opus";
  int listen_bitrate_kbps = kListenBitrateDefaultKbps;

  // Network audio input. `net_delay_ms` is how far local capture is held back so that a network
  // channel and an ADC channel that heard the same thing land on the same sample index. It only
  // takes effect while net_enabled, so a device with no remote sender behaves as it always did.
  bool net_enabled = false;
  int net_port = kNetPort;
  int net_delay_ms = static_cast<int>(kNetDelayDefaultMs);

  // On the devices the compiled-in board names for them. Only HDMI has a layout: the line out is
  // always stereo, and neither reads nor writes one.
  SocConfig hdmi{rpi3_octo_profile().sink("hdmi")->device, kHdmiMaxChannels};
  SocConfig lineout{rpi3_octo_profile().sink("lineout")->device, kLineoutChannels};

  std::string to_json() const;
  static bool from_json(const std::string& text, Config* out, std::string* err);

  void apply_to(Control& ctl) const;
  static Config from_control(const Control& ctl, const Config& base);
};

// Boot order: the saved copy on the data partition wins, otherwise the read-only defaults.
class ConfigStore {
 public:
  ConfigStore(std::string defaults_path, std::string data_dir);

  Config load();
  bool save(const Config& cfg, std::string* err);
  bool reset(std::string* err);

  const std::string& saved_path() const { return saved_path_; }
  bool has_saved() const;

  // False when the data directory is a RAM filesystem — i.e. the real /data partition failed to
  // mount and soundtester-sshkeys.sh put a tmpfs there so that ssh would still work. Saving to
  // it would appear to succeed and then evaporate at the next power cycle, so save() refuses,
  // and /api/state publishes this so the console can say why.
  bool is_persistent() const;

 private:
  // The data partition is mounted read-only; only a save may briefly flip it.
  bool remount(bool writable, std::string* err) const;
  bool is_mountpoint() const;

  std::string defaults_path_;
  std::string data_dir_;
  std::string saved_path_;
};

}  // namespace st
