#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "constants.h"
#include "control.h"

namespace st {

struct InputConfig {
  float gain_db = 0.0f;  // digital make-up gain, 0..+40 dB
};

struct OutputConfig {
  std::string source_type = "silence";  // silence | input | gen
  std::string source_index;             // "0".."5" for input, "sine"/"noise"/"ping" for gen
  float gain_db = 0.0f;
  bool mute = false;
};

struct Config {
  unsigned rate = kDefaultRate;
  unsigned period = kDefaultPeriod;
  unsigned periods = kDefaultPeriods;
  std::string device = "hw:audioinjectoroc,0";
  unsigned capture_channels = kTdmSlots;

  std::array<InputConfig, kInputs> inputs{};
  std::array<OutputConfig, kOutputs> outputs{};
  float sine_freq_hz = 440.0f;
  float sine_level_db = -20.0f;
  std::string noise_mode = "white";
  float noise_level_db = -20.0f;
  std::string ping_variant = "tick";
  float ping_interval_s = 2.0f;
  float ping_level_db = -20.0f;

  std::array<uint8_t, kInputs> input_map{{0, 1, 2, 3, 4, 5}};
  std::array<uint8_t, kOutputs> output_map{{0, 1, 2, 3, 4, 5, 6, 7}};

  std::vector<std::string> input_names{kInputs};
  std::vector<std::string> output_names{kOutputs};

  int64_t loopback_offset_samples = 0;

  // Listen stream: the codec the console defaults to ("pcm" | "opus") and the per-channel Opus
  // bitrate. The wire default is always PCM; this only sets the console's preference and the
  // stream.ogg default.
  std::string listen_codec = "opus";
  int listen_bitrate_kbps = kListenBitrateDefaultKbps;

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
