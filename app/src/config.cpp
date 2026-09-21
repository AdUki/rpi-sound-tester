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

// One output's routing, shared by the Octo's outputs, HDMI and the line out so they cannot come to
// disagree on the file format, the clamps or what an unknown source falls back to.
json output_to_json(const OutputConfig& o) {
  return {{"source", {{"type", o.source_type}, {"index", o.source_index}}},
          {"gain_db", o.gain_db},
          {"mute", o.mute}};
}

OutputConfig output_from_json(const json& o) {
  OutputConfig c;
  if (o.contains("source")) {
    const json& src = o["source"];
    c.source_type = src.value("type", std::string("silence"));
    if (src.contains("index")) {
      const auto& idx = src["index"];
      c.source_index = idx.is_string() ? idx.get<std::string>() : std::to_string(idx.get<int>());
    }
  }
  c.gain_db = o.value("gain_db", 0.0f);
  c.mute = o.value("mute", false);
  return c;
}

void apply_output(const OutputConfig& o, OutputControl& oc) {
  SourceType type = SourceType::Silence;
  uint8_t index = 0;
  if (o.source_type == "input") {
    type = SourceType::Input;
    index = static_cast<uint8_t>(std::clamp(std::atoi(o.source_index.c_str()), 0,
                                            static_cast<int>(kTotalInputs) - 1));
  } else if (o.source_type == "gen") {
    GenId g = GenId::Sine;
    if (parse_gen(o.source_index, &g)) {
      type = SourceType::Gen;
      index = static_cast<uint8_t>(g);
    }
  }
  oc.source.store(pack_source(type, index));
  oc.gain_db.store(std::clamp(o.gain_db, kLevelMinDb, kLevelMaxDb));
  oc.mute.store(o.mute);
}

OutputConfig output_from_control(const OutputControl& oc) {
  OutputConfig c;
  const uint32_t packed = oc.source.load();
  const SourceType t = source_type(packed);
  const uint8_t idx = source_index(packed);
  c.source_type = to_string(t);
  if (t == SourceType::Input) {
    c.source_index = std::to_string(idx);
  } else if (t == SourceType::Gen) {
    c.source_index = gen_name(static_cast<GenId>(idx));
  }
  c.gain_db = oc.gain_db.load();
  c.mute = oc.mute.load();
  return c;
}

// One of the SoC's own outputs. Only HDMI carries a layout; the line out is stereo whatever a
// hand-edited file says.
json soc_to_json(const SocConfig& c, bool layouts) {
  json outs = json::array();
  for (const auto& o : c.outputs) outs.push_back(output_to_json(o));
  json j{{"enabled", c.enabled}, {"device", c.device}, {"sample_rate", c.sample_rate}};
  if (layouts) j["layout"] = c.layout;
  j["outputs"] = outs;
  j["names"] = c.names;
  return j;
}

void soc_from_json(const json& j, bool layouts, SocConfig* c) {
  c->enabled = j.value("enabled", c->enabled);
  c->device = j.value("device", c->device);
  c->sample_rate = j.value("sample_rate", c->sample_rate);
  if (layouts) c->layout = j.value("layout", c->layout);
  if (j.contains("outputs")) {
    const auto& arr = j.at("outputs");
    for (size_t i = 0; i < arr.size() && i < c->outputs.size(); ++i)
      c->outputs[i] = output_from_json(arr[i]);
  }
  if (j.contains("names")) c->names = j.at("names").get<std::vector<std::string>>();
}

// Not clamped: a layout that does not exist is stereo, and a rate the sink cannot use — at all, or
// for this layout — falls back to the one every sink accepts.
void soc_sanitise(SocConfig* c) {
  c->names.resize(c->outputs.size());
  HdmiLayout layout = kHdmiLayoutDefault;
  if (!parse_hdmi_layout(c->layout, &layout)) c->layout = hdmi_layout_name(layout);
  if (!soc_rate_ok(c->sample_rate) || !hdmi_layout_rate_ok(layout, c->sample_rate))
    c->sample_rate = kSocRateDefault;
}

void apply_soc(const SocConfig& c, bool layouts, OutputControl* outs, SocControl& sc) {
  for (size_t i = 0; i < c.outputs.size(); ++i) apply_output(c.outputs[i], outs[i]);
  sc.enabled.store(c.enabled);
  HdmiLayout layout = kHdmiLayoutDefault;
  if (layouts) parse_hdmi_layout(c.layout, &layout);
  sc.layout.store(static_cast<uint8_t>(layout));
}

void soc_from_control(const OutputControl* outs, const SocControl& sc, SocConfig* c) {
  for (size_t i = 0; i < c->outputs.size(); ++i) c->outputs[i] = output_from_control(outs[i]);
  c->enabled = sc.enabled.load();
  c->layout = hdmi_layout_name(soc_layout(sc, static_cast<unsigned>(c->outputs.size())));
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
  for (const auto& i : inputs) j["inputs"].push_back({{"gain_db", i.gain_db}, {"mute", i.mute}});

  j["outputs"] = json::array();
  for (const auto& o : outputs) j["outputs"].push_back(output_to_json(o));

  j["generators"]["sine"] = {{"freq_hz", sine_freq_hz}, {"level_db", sine_level_db}};
  j["generators"]["noise"] = {{"mode", noise_mode}, {"level_db", noise_level_db}};
  j["generators"]["ping"] = {
      {"variant", ping_variant}, {"interval_s", ping_interval_s}, {"level_db", ping_level_db}};
  j["generators"]["music"] = {{"level_db", music_level_db}};

  j["input_map"] = input_map;
  j["output_map"] = output_map;
  j["input_names"] = input_names;
  j["output_names"] = output_names;
  j["loopback_offset_samples"] = loopback_offset_samples;
  j["listen"] = {{"codec", listen_codec}, {"bitrate_kbps", listen_bitrate_kbps}};
  j["net"] = {{"enabled", net_enabled}, {"port", net_port}, {"delay_ms", net_delay_ms}};
  j["hdmi"] = soc_to_json(hdmi, true);
  j["lineout"] = soc_to_json(lineout, false);
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
      for (size_t i = 0; i < arr.size() && i < kTotalInputs; ++i) {
        c.inputs[i].gain_db = arr[i].value("gain_db", 0.0f);
        c.inputs[i].mute = arr[i].value("mute", false);
      }
    }

    if (j.contains("outputs")) {
      const auto& arr = j.at("outputs");
      for (size_t i = 0; i < arr.size() && i < kOutputs; ++i)
        c.outputs[i] = output_from_json(arr[i]);
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
      if (g.contains("music")) c.music_level_db = g["music"].value("level_db", c.music_level_db);
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
    if (j.contains("net")) {
      c.net_enabled = j["net"].value("enabled", c.net_enabled);
      c.net_port = j["net"].value("port", c.net_port);
      c.net_delay_ms = j["net"].value("delay_ms", c.net_delay_ms);
    }
    if (j.contains("listen")) {
      c.listen_codec = j["listen"].value("codec", c.listen_codec);
      c.listen_bitrate_kbps = j["listen"].value("bitrate_kbps", c.listen_bitrate_kbps);
    }
    if (j.contains("hdmi")) soc_from_json(j.at("hdmi"), true, &c.hdmi);
    if (j.contains("lineout")) soc_from_json(j.at("lineout"), false, &c.lineout);
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    return false;
  }

  c.input_names.resize(kTotalInputs);
  c.output_names.resize(kOutputs);
  soc_sanitise(&c.hdmi);
  soc_sanitise(&c.lineout);
  *out = c;
  return true;
}

void Config::apply_to(Control& ctl) const {
  for (unsigned i = 0; i < kTotalInputs; ++i) {
    ctl.inputs[i].gain_db.store(
        std::clamp(inputs[i].gain_db, input_gain_min_db(i), kInputGainMaxDb));
    ctl.inputs[i].mute.store(inputs[i].mute);
  }

  for (unsigned i = 0; i < kOutputs; ++i) apply_output(outputs[i], ctl.outputs[i]);
  apply_soc(hdmi, true, ctl.hdmi_outputs.data(), ctl.hdmi);
  apply_soc(lineout, false, ctl.lineout_outputs.data(), ctl.lineout);

  ctl.sine.freq_hz.store(std::clamp(sine_freq_hz, kSineFreqMinHz, kSineFreqMaxHz));
  ctl.sine.level_db.store(std::clamp(sine_level_db, kLevelMinDb, kLevelMaxDb));

  NoiseMode nm = NoiseMode::White;
  parse_noise(noise_mode, &nm);
  ctl.noise.mode.store(static_cast<uint8_t>(nm));
  ctl.noise.level_db.store(std::clamp(noise_level_db, kLevelMinDb, kLevelMaxDb));

  PingVariant pv = PingVariant::Tick;
  parse_ping(ping_variant, &pv);
  ctl.ping.variant.store(static_cast<uint8_t>(pv));
  ctl.ping.interval_s.store(std::clamp(ping_interval_s, kPingIntervalMinS, kPingIntervalMaxS));
  ctl.ping.level_db.store(std::clamp(ping_level_db, kLevelMinDb, kLevelMaxDb));
  ctl.ping.epoch.fetch_add(1);
  ctl.music.level_db.store(std::clamp(music_level_db, kLevelMinDb, kLevelMaxDb));

  ListenCodec lc = ListenCodec::Opus;
  parse_codec(listen_codec, &lc);
  ctl.listen.codec.store(static_cast<uint8_t>(lc));
  ctl.listen.bitrate_kbps.store(
      std::clamp(listen_bitrate_kbps, kListenBitrateMinKbps, kListenBitrateMaxKbps));

  // The delay is derived once, here and in the live PUT handler, so the audio thread reads a
  // frame count instead of recomputing one per block — and so it is unambiguously zero whenever
  // network input is off.
  const unsigned dms = static_cast<unsigned>(std::clamp(
      net_delay_ms, static_cast<int>(kNetDelayMinMs), static_cast<int>(kNetDelayMaxMs)));
  ctl.net.enabled.store(net_enabled);
  ctl.net.port.store(static_cast<uint16_t>(std::clamp(net_port, kNetPortMin, kNetPortMax)));
  ctl.net.delay_ms.store(dms);
  ctl.net.delay_frames.store(net_enabled ? static_cast<uint32_t>(1ull * dms * rate / 1000) : 0);

  // A saved map that is not a permutation (see is_slot_permutation) is rejected wholesale.
  if (is_slot_permutation(input_map, kTdmSlots)) {
    for (unsigned i = 0; i < kInputs; ++i) ctl.input_map[i].store(input_map[i]);
  } else {
    LOG_WARN("input_map is not a permutation — falling back to identity");
    for (unsigned i = 0; i < kInputs; ++i) ctl.input_map[i].store(static_cast<uint8_t>(i));
  }

  if (is_slot_permutation(output_map, kOutputs)) {
    for (unsigned i = 0; i < kOutputs; ++i) ctl.output_map[i].store(output_map[i]);
  } else {
    LOG_WARN("output_map is not a permutation — falling back to identity");
    for (unsigned i = 0; i < kOutputs; ++i) ctl.output_map[i].store(static_cast<uint8_t>(i));
  }
}

Config Config::from_control(const Control& ctl, const Config& base) {
  Config c = base;
  for (unsigned i = 0; i < kTotalInputs; ++i) {
    c.inputs[i].gain_db = ctl.inputs[i].gain_db.load();
    c.inputs[i].mute = ctl.inputs[i].mute.load();
  }

  for (unsigned i = 0; i < kOutputs; ++i) c.outputs[i] = output_from_control(ctl.outputs[i]);
  soc_from_control(ctl.hdmi_outputs.data(), ctl.hdmi, &c.hdmi);
  soc_from_control(ctl.lineout_outputs.data(), ctl.lineout, &c.lineout);

  c.sine_freq_hz = ctl.sine.freq_hz.load();
  c.sine_level_db = ctl.sine.level_db.load();
  c.noise_mode = noise_name(static_cast<NoiseMode>(ctl.noise.mode.load()));
  c.noise_level_db = ctl.noise.level_db.load();
  c.ping_variant = ping_name(static_cast<PingVariant>(ctl.ping.variant.load()));
  c.ping_interval_s = ctl.ping.interval_s.load();
  c.ping_level_db = ctl.ping.level_db.load();
  c.music_level_db = ctl.music.level_db.load();

  for (unsigned i = 0; i < kInputs; ++i) c.input_map[i] = ctl.input_map[i].load();
  for (unsigned i = 0; i < kOutputs; ++i) c.output_map[i] = ctl.output_map[i].load();

  c.listen_codec = codec_name(static_cast<ListenCodec>(ctl.listen.codec.load()));
  c.listen_bitrate_kbps = ctl.listen.bitrate_kbps.load();
  c.net_enabled = ctl.net.enabled.load();
  c.net_port = ctl.net.port.load();
  c.net_delay_ms = static_cast<int>(ctl.net.delay_ms.load());
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
