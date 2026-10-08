#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

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

// A sink: one playback device other than the engine card, by its device id in Config::sinks.
// `sample_rate` is the rate its PCM is opened at (never "rate": older images patched every "rate"
// key in config.json) and 0, like an empty `layout`, means the device's default. `outputs` and
// `names` hold one entry per channel a sink can have, played or not.
struct SinkConfig {
  bool enabled = false;
  unsigned sample_rate = 0;
  std::string layout;
  std::array<OutputConfig, kMaxSinkWidth> outputs{};
  std::array<std::string, kMaxSinkWidth> names{};
};

// Bluetooth. The adapter's settings belong to the Bluetooth manager rather than to Control, since
// the audio thread reads none of them; a save takes them from here, where the PUT handler keeps
// them current. `input` is the one live switch (NetControl::bt_input). The output is the sink
// kBtSinkId, configured in `sinks` like any other; `output_device` is only which speaker it plays to,
// a bluez-alsa PCM name. Pairings are not here at all: BlueZ keeps them, and they are mirrored to the
// data partition as they change (ConfigStore::save_dir).
struct BtConfig {
  bool enabled = true;  // the radio is powered
  std::string name;     // what other devices see it as; empty means the hostname
  bool pairable = true;
  unsigned discoverable_timeout_s = kBtDiscoverableDefaultS;
  bool input = false;
  std::string output_device = kBtDefaultDevice;
};

struct Config {
  std::array<InputConfig, kMaxInputs> inputs{};
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

  std::vector<std::string> input_names{kMaxInputs};
  std::vector<std::string> output_names{kOutputs};

  int64_t loopback_offset_samples = 0;

  // Listen stream: the codec the console defaults to ("pcm" | "opus") and the per-channel Opus
  // bitrate. The wire default is always PCM; this only sets the console's preference and the
  // stream.ogg default.
  std::string listen_codec = "opus";
  int listen_bitrate_kbps = kListenBitrateDefaultKbps;

  // Network audio input. `net_delay_ms` is how far local capture is held back so that a network
  // channel and an ADC channel that heard the same thing land on the same sample index. It only
  // takes effect while net_enabled, so a device with network input off captures with no delay.
  // On by default, so a sender can play to a freshly booted device with nothing to switch on.
  bool net_enabled = true;
  int net_port = kNetPort;
  int net_delay_ms = static_cast<int>(kNetDelayDefaultMs);

  // Every sink ever configured, by device id ("b1,0"), whether or not it is plugged in now: a USB
  // interface gets its routing back when it returns.
  std::map<std::string, SinkConfig> sinks;

  BtConfig bluetooth;

  std::string to_json() const;
  static bool from_json(const std::string& text, Config* out, std::string* err);

  // `rate` is the engine's: the network delay is held in frames of it. Sinks are applied by
  // Devices, as their devices are found.
  void apply_to(Control& ctl, unsigned rate) const;
  // Everything but the sinks, which Devices holds: see Devices::sink_configs().
  static Config from_control(const Control& ctl, const Config& base);
};

// One output's routing onto its control and back, shared by the engine card's outputs and every
// sink's so they clamp alike.
void apply_output(const OutputConfig& o, OutputControl& oc);
OutputConfig output_from_control(const OutputControl& oc);

// Boot order: the saved copy on the data partition wins, otherwise the read-only defaults.
class ConfigStore {
 public:
  ConfigStore(std::string defaults_path, std::string data_dir);

  Config load();
  bool save(const Config& cfg, std::string* err);
  bool reset(std::string* err);

  // Replaces `<data dir>/<name>` with a copy of the directory `src`: how BlueZ's pairings, which it
  // keeps on the RAM-backed /var/lib, survive a reboot. The same rules as save(): refused on a RAM
  // fallback, and the partition is writable only for as long as the copy takes.
  bool save_dir(const std::string& src, const std::string& name, std::string* err);

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
  // Held from a remount read-write to the remount back: a save from the console and the pairings
  // being copied from the Bluetooth thread must not remount the partition under each other.
  mutable std::mutex write_m_;
};

}  // namespace st
