#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "util/log.h"

using json = nlohmann::json;

namespace st {

namespace {

std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

std::string Config::to_json() const {
  json j;
  j["rate"] = rate;
  j["period"] = period;
  j["periods"] = periods;
  j["device"] = device;
  j["capture_channels"] = capture_channels;

  j["inputs"] = json::array();
  for (const auto& i : inputs) j["inputs"].push_back({{"gain_db", i.gain_db}});

  j["outputs"] = json::array();
  for (const auto& o : outputs) {
    j["outputs"].push_back({{"source", {{"type", o.source_type}, {"index", o.source_index}}},
                            {"gain_db", o.gain_db},
                            {"mute", o.mute}});
  }

  j["generators"]["sine"] = {{"freq_hz", sine_freq_hz}, {"level_db", sine_level_db}};
  j["generators"]["noise"] = {{"mode", noise_mode}, {"level_db", noise_level_db}};
  j["generators"]["ping"] = {
      {"variant", ping_variant}, {"interval_s", ping_interval_s}, {"level_db", ping_level_db}};

  j["input_map"] = input_map;
  j["output_map"] = output_map;
  j["input_names"] = input_names;
  j["output_names"] = output_names;
  j["loopback_offset_samples"] = loopback_offset_samples;
  j["listen"] = {{"codec", listen_codec}, {"bitrate_kbps", listen_bitrate_kbps}};
  return j.dump(2);
}

bool Config::from_json(const std::string& text, Config* out, std::string* err) {
  Config c;
  try {
    const json j = json::parse(text);
    c.rate = j.value("rate", c.rate);
    c.period = j.value("period", c.period);
    c.periods = j.value("periods", c.periods);
    c.device = j.value("device", c.device);
    c.capture_channels = j.value("capture_channels", c.capture_channels);

    if (j.contains("inputs")) {
      const auto& arr = j.at("inputs");
      for (size_t i = 0; i < arr.size() && i < kInputs; ++i) {
        c.inputs[i].gain_db = arr[i].value("gain_db", 0.0f);
      }
    }

    if (j.contains("outputs")) {
      const auto& arr = j.at("outputs");
      for (size_t i = 0; i < arr.size() && i < kOutputs; ++i) {
        const auto& o = arr[i];
        if (o.contains("source")) {
          c.outputs[i].source_type = o["source"].value("type", std::string("silence"));
          const auto& idx = o["source"]["index"];
          c.outputs[i].source_index = idx.is_string() ? idx.get<std::string>()
                                                      : std::to_string(idx.get<int>());
        }
        c.outputs[i].gain_db = o.value("gain_db", 0.0f);
        c.outputs[i].mute = o.value("mute", false);
      }
    }

    if (j.contains("generators")) {
      const auto& g = j.at("generators");
      if (g.contains("sine")) {
        c.sine_freq_hz = g["sine"].value("freq_hz", c.sine_freq_hz);
        c.sine_level_db = g["sine"].value("level_db", c.sine_level_db);
      }
      if (g.contains("noise")) {
        c.noise_mode = g["noise"].value("mode", c.noise_mode);
        c.noise_level_db = g["noise"].value("level_db", c.noise_level_db);
      }
      if (g.contains("ping")) {
        c.ping_variant = g["ping"].value("variant", c.ping_variant);
        c.ping_interval_s = g["ping"].value("interval_s", c.ping_interval_s);
        c.ping_level_db = g["ping"].value("level_db", c.ping_level_db);
      }
    }

    if (j.contains("input_map")) {
      const auto v = j.at("input_map").get<std::vector<uint8_t>>();
      for (size_t i = 0; i < v.size() && i < kInputs; ++i) c.input_map[i] = v[i];
    }
    if (j.contains("output_map")) {
      const auto v = j.at("output_map").get<std::vector<uint8_t>>();
      for (size_t i = 0; i < v.size() && i < kOutputs; ++i) c.output_map[i] = v[i];
    }
    if (j.contains("input_names")) c.input_names = j.at("input_names").get<std::vector<std::string>>();
    if (j.contains("output_names")) c.output_names = j.at("output_names").get<std::vector<std::string>>();
    c.loopback_offset_samples = j.value("loopback_offset_samples", c.loopback_offset_samples);
    if (j.contains("listen")) {
      c.listen_codec = j["listen"].value("codec", c.listen_codec);
      c.listen_bitrate_kbps = j["listen"].value("bitrate_kbps", c.listen_bitrate_kbps);
    }
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    return false;
  }

  c.input_names.resize(kInputs);
  c.output_names.resize(kOutputs);
  *out = c;
  return true;
}

void Config::apply_to(Control& ctl) const {
  for (unsigned i = 0; i < kInputs; ++i) {
    ctl.inputs[i].gain_db.store(std::clamp(inputs[i].gain_db, kInputGainMinDb, kInputGainMaxDb));
  }

  for (unsigned i = 0; i < kOutputs; ++i) {
    const OutputConfig& o = outputs[i];
    SourceType type = SourceType::Silence;
    uint8_t index = 0;
    if (o.source_type == "input") {
      type = SourceType::Input;
      index = static_cast<uint8_t>(std::clamp(std::atoi(o.source_index.c_str()), 0,
                                              static_cast<int>(kInputs) - 1));
    } else if (o.source_type == "gen") {
      GenId g = GenId::Sine;
      if (parse_gen(o.source_index, &g)) {
        type = SourceType::Gen;
        index = static_cast<uint8_t>(g);
      }
    }
    ctl.outputs[i].source.store(pack_source(type, index));
    ctl.outputs[i].gain_db.store(std::clamp(o.gain_db, -60.0f, 0.0f));
    ctl.outputs[i].mute.store(o.mute);
  }

  ctl.sine.freq_hz.store(std::clamp(sine_freq_hz, 1.0f, 40000.0f));
  ctl.sine.level_db.store(std::clamp(sine_level_db, -60.0f, 0.0f));

  ctl.noise.mode.store(static_cast<uint8_t>(noise_mode == "pink" ? NoiseMode::Pink
                                                                 : NoiseMode::White));
  ctl.noise.level_db.store(std::clamp(noise_level_db, -60.0f, 0.0f));

  PingVariant pv = PingVariant::Tick;
  parse_ping(ping_variant, &pv);
  ctl.ping.variant.store(static_cast<uint8_t>(pv));
  ctl.ping.interval_s.store(std::clamp(ping_interval_s, 0.5f, 60.0f));
  ctl.ping.level_db.store(std::clamp(ping_level_db, -60.0f, 0.0f));
  ctl.ping.epoch.fetch_add(1);

  ctl.listen.codec.store(
      static_cast<uint8_t>(listen_codec == "opus" ? ListenCodec::Opus : ListenCodec::Pcm));
  ctl.listen.bitrate_kbps.store(
      std::clamp(listen_bitrate_kbps, kListenBitrateMinKbps, kListenBitrateMaxKbps));

  // The audio loop writes each physical slot exactly once, via this map. A duplicate entry
  // would leave some slot never written (stale audio) and let two logical channels fight
  // over another, so a map that is not a permutation is rejected wholesale.
  auto is_permutation = [](const uint8_t* v, size_t n, unsigned limit) {
    bool seen[kTdmSlots] = {false};
    for (size_t i = 0; i < n; ++i) {
      if (v[i] >= limit || seen[v[i]]) return false;
      seen[v[i]] = true;
    }
    return true;
  };

  if (is_permutation(input_map.data(), kInputs, kTdmSlots)) {
    for (unsigned i = 0; i < kInputs; ++i) ctl.input_map[i].store(input_map[i]);
  } else {
    LOG_WARN("input_map is not a permutation — falling back to identity");
    for (unsigned i = 0; i < kInputs; ++i) ctl.input_map[i].store(static_cast<uint8_t>(i));
  }

  if (is_permutation(output_map.data(), kOutputs, kOutputs)) {
    for (unsigned i = 0; i < kOutputs; ++i) ctl.output_map[i].store(output_map[i]);
  } else {
    LOG_WARN("output_map is not a permutation — falling back to identity");
    for (unsigned i = 0; i < kOutputs; ++i) ctl.output_map[i].store(static_cast<uint8_t>(i));
  }
}

Config Config::from_control(const Control& ctl, const Config& base) {
  Config c = base;
  for (unsigned i = 0; i < kInputs; ++i) c.inputs[i].gain_db = ctl.inputs[i].gain_db.load();

  for (unsigned i = 0; i < kOutputs; ++i) {
    const uint32_t packed = ctl.outputs[i].source.load();
    const SourceType t = source_type(packed);
    const uint8_t idx = source_index(packed);
    c.outputs[i].source_type = to_string(t);
    if (t == SourceType::Input) {
      c.outputs[i].source_index = std::to_string(idx);
    } else if (t == SourceType::Gen) {
      c.outputs[i].source_index = gen_name(static_cast<GenId>(idx));
    } else {
      c.outputs[i].source_index = "";
    }
    c.outputs[i].gain_db = ctl.outputs[i].gain_db.load();
    c.outputs[i].mute = ctl.outputs[i].mute.load();
  }

  c.sine_freq_hz = ctl.sine.freq_hz.load();
  c.sine_level_db = ctl.sine.level_db.load();
  c.noise_mode = ctl.noise.mode.load() == static_cast<uint8_t>(NoiseMode::Pink) ? "pink" : "white";
  c.noise_level_db = ctl.noise.level_db.load();
  c.ping_variant = ping_name(static_cast<PingVariant>(ctl.ping.variant.load()));
  c.ping_interval_s = ctl.ping.interval_s.load();
  c.ping_level_db = ctl.ping.level_db.load();

  for (unsigned i = 0; i < kInputs; ++i) c.input_map[i] = ctl.input_map[i].load();
  for (unsigned i = 0; i < kOutputs; ++i) c.output_map[i] = ctl.output_map[i].load();

  c.listen_codec =
      ctl.listen.codec.load() == static_cast<uint8_t>(ListenCodec::Opus) ? "opus" : "pcm";
  c.listen_bitrate_kbps = ctl.listen.bitrate_kbps.load();
  return c;
}

ConfigStore::ConfigStore(std::string defaults_path, std::string data_dir)
    : defaults_path_(std::move(defaults_path)), data_dir_(std::move(data_dir)) {
  saved_path_ = data_dir_ + "/config.json";
}

bool ConfigStore::has_saved() const {
  struct stat st{};
  return stat(saved_path_.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool ConfigStore::is_mountpoint() const {
  struct stat a{}, b{};
  if (stat(data_dir_.c_str(), &a) != 0) return false;
  if (stat((data_dir_ + "/..").c_str(), &b) != 0) return false;
  return a.st_dev != b.st_dev;
}

bool ConfigStore::is_persistent() const {
  // The /data entry in fstab carries nofail, so a missing or unreadable data partition does not
  // stop the boot, and soundtester-sshkeys.sh then mounts a tmpfs at /data so that sshd still
  // gets host keys. That leaves a /data that looks completely normal and is RAM: a save would
  // report success and be gone at the next power cycle. So look for a RAM filesystem mounted
  // *at* the data directory. A plain directory on a dev box is not a mountpoint at all, so this
  // stays out of the way there.
  constexpr decltype(statfs::f_type) kTmpfsMagic = 0x01021994;
  constexpr decltype(statfs::f_type) kRamfsMagic = 0x858458f6;

  if (!is_mountpoint()) return true;
  struct statfs sf{};
  if (statfs(data_dir_.c_str(), &sf) != 0) return true;  // cannot tell — do not block the save
  return sf.f_type != kTmpfsMagic && sf.f_type != kRamfsMagic;
}

bool ConfigStore::remount(bool writable, std::string* err) const {
  // On a dev box the data directory is a plain directory, not a mount: nothing to flip.
  if (!is_mountpoint()) return true;

  const unsigned long flags =
      static_cast<unsigned long>(MS_REMOUNT) | (writable ? 0UL : static_cast<unsigned long>(MS_RDONLY));
  for (int attempt = 0; attempt < 5; ++attempt) {
    if (mount(nullptr, data_dir_.c_str(), nullptr, flags, nullptr) == 0) return true;
    if (errno != EBUSY) break;
    usleep(100 * 1000);
  }
  if (err) {
    *err = std::string("remount ") + (writable ? "rw" : "ro") + " " + data_dir_ + ": " +
           strerror(errno);
  }
  return false;
}

Config ConfigStore::load() {
  Config cfg;
  std::string err;

  const std::string saved = read_file(saved_path_);
  if (!saved.empty()) {
    if (Config::from_json(saved, &cfg, &err)) {
      LOG_INFO("loaded saved config from {}", saved_path_);
      return cfg;
    }
    LOG_WARN("ignoring invalid saved config {}: {}", saved_path_, err);
  }

  const std::string defaults = read_file(defaults_path_);
  if (!defaults.empty()) {
    if (Config::from_json(defaults, &cfg, &err)) {
      LOG_INFO("loaded defaults from {}", defaults_path_);
      return cfg;
    }
    LOG_WARN("invalid defaults {}: {} — using built-ins", defaults_path_, err);
  } else {
    LOG_WARN("no config at {} — using built-ins", defaults_path_);
  }
  return Config{};
}

bool ConfigStore::save(const Config& cfg, std::string* err) {
  if (!is_persistent()) {
    if (err) {
      *err = "the data partition is not mounted (" + data_dir_ +
             " is a RAM fallback) — settings cannot be saved as boot defaults";
    }
    LOG_ERROR("refusing to save: {} is a tmpfs, so nothing written there would survive a reboot",
              data_dir_);
    return false;
  }
  if (!remount(true, err)) return false;

  bool ok = false;
  const std::string tmp = saved_path_ + ".tmp";
  const std::string text = cfg.to_json();

  // Durability order matters: the data must be on the medium before the rename publishes
  // it, and the rename itself must be on the medium before power can be pulled.
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    if (err) *err = std::string("open ") + tmp + ": " + strerror(errno);
  } else {
    ssize_t written = write(fd, text.data(), text.size());
    if (written != static_cast<ssize_t>(text.size())) {
      if (err) *err = std::string("write: ") + strerror(errno);
      close(fd);
    } else if (fsync(fd) != 0) {
      if (err) *err = std::string("fsync: ") + strerror(errno);
      close(fd);
    } else {
      close(fd);
      if (rename(tmp.c_str(), saved_path_.c_str()) != 0) {
        if (err) *err = std::string("rename: ") + strerror(errno);
      } else {
        int dir = open(data_dir_.c_str(), O_RDONLY | O_DIRECTORY);
        if (dir >= 0) {
          fsync(dir);
          close(dir);
        }
        ok = true;
      }
    }
  }

  if (!ok) unlink(tmp.c_str());

  std::string rerr;
  if (!remount(false, &rerr)) LOG_ERROR("{} — data partition left writable!", rerr);

  if (ok) LOG_INFO("saved boot defaults to {}", saved_path_);
  return ok;
}

bool ConfigStore::reset(std::string* err) {
  if (!has_saved()) return true;
  if (!remount(true, err)) return false;

  bool ok = true;
  if (unlink(saved_path_.c_str()) != 0 && errno != ENOENT) {
    if (err) *err = std::string("unlink: ") + strerror(errno);
    ok = false;
  }

  std::string rerr;
  if (!remount(false, &rerr)) LOG_ERROR("{} — data partition left writable!", rerr);

  if (ok) LOG_INFO("removed {} — next boot uses the image defaults", saved_path_);
  return ok;
}

}  // namespace st
