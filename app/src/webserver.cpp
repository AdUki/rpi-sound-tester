#include "webserver.h"

#include <httplib.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <vector>

#include "capture_export.h"
#include "channel_layout.h"
#include "constants.h"
#include "genie.h"
#include "genie_select.h"
#include "genie_wait.h"
#include "genie_watch.h"
#include "listen_encoder.h"
#include "listen_stream.h"
#include "output_route.h"
#include "util/dsp.h"
#include "util/log.h"
#include "util/sysinfo.h"
#include "util/wav.h"

using json = nlohmann::json;

namespace st {

namespace {

// Input indices span the card's ADCs plus the network channels, so the bound is the run's own
// rather than spelled out: how many there are depends on the board.
std::string channel_range_msg() { return "ch must be 0.." + std::to_string(channels().total() - 1); }
std::string sync_range_msg() {
  return "ch_a/ch_b must be 0.." + std::to_string(channels().total() - 1);
}

constexpr int kThreadsMin = 8;
constexpr int kThreadsMax = 64;

void send_json(httplib::Response& res, const json& j, int status = 200) {
  res.status = status;
  res.set_content(j.dump(), "application/json");
}

void send_error(httplib::Response& res, int status, const std::string& msg) {
  send_json(res, json{{"error", msg}}, status);
}

void send_ok(httplib::Response& res) { send_json(res, json{{"ok", true}}); }

bool parse_index(const httplib::Request& req, const char* name, unsigned limit, unsigned* out) {
  auto it = req.path_params.find(name);
  if (it == req.path_params.end()) return false;
  // Strictly a number: "xyz" must not read as 0 and act on the first channel.
  char* end = nullptr;
  const long v = std::strtol(it->second.c_str(), &end, 10);
  if (it->second.empty() || *end || v < 0 || v >= static_cast<long>(limit)) return false;
  *out = static_cast<unsigned>(v);
  return true;
}

// The scaffold every mutating JSON handler shares: parse the body, turn a parse/type error
// into a 400, and answer {"ok":true} unless the handler already sent its own reply.
template <class Fn>
auto json_route(Fn fn) {
  return [fn](const httplib::Request& req, httplib::Response& res) {
    try {
      fn(json::parse(req.body), req, res);
    } catch (const std::exception& e) {
      return send_error(res, 400, e.what());
    }
    if (res.body.empty()) send_ok(res);
  };
}

// json_route for the per-channel routes: the 404 for a bad channel must win over the 400 for
// a bad body, so the index check runs before the body is parsed.
template <class Fn>
auto json_channel_route(const char* name, unsigned limit, std::string missing, Fn fn) {
  return [=](const httplib::Request& req, httplib::Response& res) {
    unsigned ch = 0;
    if (!parse_index(req, name, limit, &ch)) return send_error(res, 404, missing);
    json_route([fn, ch](const json& j, const httplib::Request& r, httplib::Response& s) {
      fn(ch, j, r, s);
    })(req, res);
  };
}

// A non-negative number, strictly: false for "abc", "-1" or "12x" rather than reading them as 0.
// A fraction is truncated ("1200.5" is 1200), as before: the console's scope sends a fractional
// start when zoomed. *out keeps its default when the param is absent.
bool query_uint(const httplib::Request& req, const char* key, uint64_t* out) {
  if (!req.has_param(key)) return true;
  const std::string v = req.get_param_value(key);
  const size_t digits = v.find_first_not_of("0123456789");
  if (v.empty() || digits == 0) return false;
  if (digits != std::string::npos &&
      (v[digits] != '.' || v.find_first_not_of("0123456789", digits + 1) != std::string::npos))
    return false;
  errno = 0;
  const uint64_t n = std::strtoull(v.c_str(), nullptr, 10);
  if (errno == ERANGE) return false;
  *out = n;
  return true;
}

uint64_t query_u64(const httplib::Request& req, const char* key, uint64_t fallback) {
  if (!req.has_param(key)) return fallback;
  return std::strtoull(req.get_param_value(key).c_str(), nullptr, 10);
}

// A listen stream's ?bitrate= override. When absent, every encode() call re-reads the live
// daemon default, so an active stream follows POST /api/listen/codec bitrate changes.
struct BitrateOverride {
  bool fixed = false;
  int kbps = 0;
};

BitrateOverride query_bitrate(const httplib::Request& req) {
  BitrateOverride b;
  b.fixed = req.has_param("bitrate");
  if (b.fixed) b.kbps = static_cast<int>(query_u64(req, "bitrate", 0));
  return b;
}

int effective_kbps(const BitrateOverride& b, const Control& ctl) {
  return b.fixed ? b.kbps : ctl.listen.bitrate_kbps.load();
}

// One output's state and its PUT body, shared by the engine card's outputs and every sink's so all
// routes accept exactly the same sources and clamp exactly alike.
json output_json(const OutputControl& oc, unsigned ch, const std::string& name) {
  const uint32_t packed = oc.source.load();
  const SourceType t = source_type(packed);
  const uint8_t idx = source_index(packed);
  json src{{"type", to_string(t)}};
  if (t == SourceType::Input) {
    src["index"] = idx;
  } else if (t == SourceType::Gen) {
    src["index"] = gen_name(static_cast<GenId>(idx));
  }
  return {{"ch", ch},
          {"source", src},
          {"gain_db", oc.gain_db.load()},
          {"mute", oc.mute.load()},
          {"name", name}};
}

// False, with the 400 already sent, when the body names a source that does not exist.
bool apply_output_put(OutputControl& oc, const json& j, httplib::Response& res) {
  if (j.contains("source")) {
    const std::string type = j["source"].value("type", "silence");
    SourceType t = SourceType::Silence;
    uint8_t index = 0;
    if (type == "input") {
      const auto& iv = j["source"].at("index");
      const int i = iv.is_string() ? std::atoi(iv.get<std::string>().c_str()) : iv.get<int>();
      if (i < 0 || i >= static_cast<int>(channels().total())) {
        send_error(res, 400, "bad input index");
        return false;
      }
      t = SourceType::Input;
      index = static_cast<uint8_t>(i);
    } else if (type == "gen") {
      GenId g;
      if (!parse_gen(j["source"].at("index").get<std::string>(), &g)) {
        send_error(res, 400, "bad generator name");
        return false;
      }
      t = SourceType::Gen;
      index = static_cast<uint8_t>(g);
    } else if (type != "silence") {
      send_error(res, 400, "bad source type");
      return false;
    }
    oc.source.store(pack_source(t, index));
  }
  if (j.contains("gain_db")) {
    oc.gain_db.store(std::clamp(j["gain_db"].get<float>(), kLevelMinDb, kLevelMaxDb));
  }
  if (j.contains("mute")) oc.mute.store(j["mute"].get<bool>());
  return true;
}

json rates_json(const std::vector<unsigned>& rates) {
  json a = json::array();
  for (unsigned r : rates) a.push_back(r);
  return a;
}

// "32000, 44100 or 48000", for the 400 a bad sample_rate gets.
std::string rates_text(const std::vector<unsigned>& rates) {
  std::string t;
  for (size_t i = 0; i < rates.size(); ++i) {
    if (i) t += i + 1 == rates.size() ? " or " : ", ";
    t += std::to_string(rates[i]);
  }
  return t;
}

json layouts_json(const std::vector<SinkLayout>& layouts) {
  json a = json::array();
  for (SinkLayout l : layouts) a.push_back(sink_layout_name(l));
  return a;
}

std::string layouts_text(const std::vector<SinkLayout>& layouts) {
  std::string t;
  for (size_t i = 0; i < layouts.size(); ++i) {
    if (i) t += i + 1 == layouts.size() ? " or " : ", ";
    t += sink_layout_name(layouts[i]);
  }
  return t;
}

// What a sink is, and how it is doing.
json sink_status_json(const Devices::Sink& k, const SinkStatus& h) {
  return {{"id", k.device.id},
          {"label", k.device.label},
          {"hdmi", k.device.hdmi},
          {"usb", k.device.usb},
          {"present", k.present},
          {"layouts", layouts_json(k.device.layouts)},
          {"rates", rates_json(k.device.rates)},
          {"enabled", h.enabled},
          {"open", h.open},
          {"playing", h.playing},
          {"device", h.device},
          {"sample_rate", h.sample_rate},
          {"device_rate", h.device_rate},
          {"layout", sink_layout_name(h.layout)},
          {"speakers", h.speakers},
          {"device_channels", h.device_channels},
          {"format", h.format},
          {"period_frames", h.period_frames},
          {"buffer_frames", h.buffer_frames},
          {"latency_ms", h.latency_ms},
          {"target_ms", h.target_ms},
          {"ring_ms", h.ring_ms},
          {"alsa_ms", h.alsa_ms},
          {"trim_ppm", h.trim_ppm},
          {"xruns", h.xruns},
          {"underruns", h.underruns},
          {"overruns", h.overruns},
          {"resyncs", h.resyncs},
          {"error", h.error}};
}

// The compact form the 1 Hz WS system frame carries.
json sink_brief_json(const Devices::Sink& k, const SinkStatus& h) {
  return {{"id", k.device.id},
          {"present", k.present},
          {"enabled", h.enabled},
          {"playing", h.playing},
          {"layout", sink_layout_name(h.layout)},
          {"latency_ms", h.latency_ms},
          {"trim_ppm", h.trim_ppm},
          {"xruns", h.xruns},
          {"resyncs", h.resyncs},
          {"error", h.error}};
}

// A sink's status plus the routing of its layout's channels, each with its position and the PCM
// slot it is sent in. The other channels keep their routing and get it back when a layout that
// has them is chosen again, but they are not listed: nothing plays them.
json sink_json(Devices& devices, const Devices::Sink& k) {
  const SinkStatus h = devices.output(k.slot).status();
  json j = sink_status_json(k, h);
  const SinkLayoutInfo& lay = sink_layout_info(h.layout);
  const SinkControl& sc = devices.output(k.slot).control();
  j["outputs"] = json::array();
  for (unsigned c = 0; c < lay.speakers; ++c) {
    json o = output_json(sc.outputs[c], c, devices.channel_name(k.slot, c));
    o["position"] = sink_speaker_name(h.layout, c);
    o["slot"] = lay.slot[c];
    j["outputs"].push_back(o);
  }
  return j;
}

// The Bluetooth output: the sink kBtSinkId as /api/sinks shows it, plus which device it is playing
// to. Its PCM name may say "the most recently connected", in which case bluez-alsa's own choice is
// looked up, so the console can name the speaker rather than print a row of zeros. Null while
// there is no such sink: Bluetooth is not running.
json bt_output_json(Devices& devices, const BtManager& bt) {
  for (const Devices::Sink& k : devices.sinks()) {
    if (k.device.id != kBtSinkId) continue;
    json j = sink_json(devices, k);
    std::string address = bt_pcm_address(k.device.alsa);
    if (address.empty()) address = bt.latest_sink();
    j["address"] = address;
    j["name"] = address.empty() ? std::string{} : bt.device_name(address);
    const BtPcmInfo p = bt.playback_of(address);
    j["device_delay_ms"] = p.present ? json(p.delay_ms) : json(nullptr);
    return j;
  }
  return nullptr;
}

json bt_pcm_json(const BtPcmInfo& p) {
  if (!p.present) return nullptr;
  return {{"codec", p.codec}, {"rate", p.rate}, {"channels", p.channels}, {"delay_ms", p.delay_ms}};
}

json bt_device_json(const BtDeviceInfo& d) {
  return {{"address", d.address},
          {"name", d.name},
          {"icon", d.icon},
          {"paired", d.paired},
          {"connected", d.connected},
          {"rssi", d.has_rssi ? json(d.rssi) : json(nullptr)},
          {"sink", d.roles.sink},
          {"source", d.roles.source},
          {"busy", d.busy},
          {"error", d.error},
          {"playback", bt_pcm_json(d.playback)},
          {"capture", bt_pcm_json(d.capture)},
          {"volume", d.volume >= 0 ? json(d.volume) : json(nullptr)},
          {"player", d.player.present ? json{{"status", d.player.status},
                                             {"title", d.player.title},
                                             {"artist", d.player.artist},
                                             {"album", d.player.album},
                                             {"duration_ms", d.player.duration_ms},
                                             {"position_ms", d.player.position_ms},
                                             {"local", d.player.local}}
                                      : json(nullptr)}};
}

// A pairing question waiting for the operator, or null. Shared by GET /api/bluetooth and the 1 Hz
// system frame, which is how a console on any tab learns to show it.
json bt_request_json(const BtStatus& s) {
  if (!s.has_request) return nullptr;
  const BtRequest& r = s.request;
  return {{"id", r.id},
          {"kind", r.kind},
          {"address", r.address},
          {"name", r.name},
          {"passkey", r.passkey},
          {"expires_s", r.expires_s}};
}

json bt_input_json(const BtInputStatus& i) {
  return {{"enabled", i.enabled},
          {"state", i.state},
          {"address", i.address},
          {"name", i.name},
          {"rate", i.rate},
          {"channels", i.channels},
          {"input", i.input},
          {"frames", i.frames},
          {"overruns", i.overruns},
          {"restarts", i.restarts},
          {"error", i.error}};
}

json device_json(const DeviceStatus& d) {
  return {{"id", d.pcm.id},
          {"alsa", d.pcm.alsa},
          {"card", d.pcm.card_id},
          {"card_name", d.pcm.card_name},
          {"name", d.pcm.pcm_name},
          {"label", d.label},
          {"playback", d.pcm.playback},
          {"capture", d.pcm.capture},
          {"usb", d.pcm.usb},
          {"engine", d.engine},
          {"hidden", d.hidden},
          {"sink", d.sink},
          {"input", d.input},
          {"input_channels", d.input_channels},
          {"note", d.note}};
}

// A device input's status: which columns it fills, and how the capture is doing.
json source_json(const Devices::Source& s) {
  const DeviceInputStatus& h = s.status;
  return {{"id", s.id},
          {"label", s.label},
          {"first", s.first},
          {"channels", s.channels},
          {"present", s.present},
          {"open", h.open},
          {"capturing", h.capturing},
          {"device", h.device},
          {"device_rate", h.device_rate},
          {"format", h.format},
          {"trim_ppm", h.trim_ppm},
          {"xruns", h.xruns},
          {"resyncs", h.resyncs},
          {"late", h.late},
          {"error", h.error}};
}

// What the console calls ring column `c`: IN n for the engine card's, NET n for the network's, and
// the device and its channel for a device input's.
std::string input_label(const ChannelLayout& layout, const std::vector<Devices::Source>& sources,
                        unsigned c) {
  if (layout.is_net(c)) return "NET " + std::to_string(c - layout.net_base() + 1);
  for (const Devices::Source& s : sources) {
    if (c < s.first || c >= s.first + s.channels) continue;
    const unsigned k = c - s.first;
    if (s.channels == 1) return s.label;
    if (s.channels == 2) return s.label + (k ? " R" : " L");
    return s.label + " " + std::to_string(k + 1);
  }
  return "IN " + std::to_string(c + 1);
}

// What a network channel is called when config.json gives it no name: its sender.
std::string net_sender_name(const NetChannelStatus& c) {
  std::string who = c.device.empty() ? c.last_device : c.device;
  // A named stream ("main", "notif") says more than the machine does, and two of them from
  // one machine would otherwise read the same.
  const std::string& stream = c.connected ? c.stream : c.last_stream;
  if (!stream.empty()) who = who.empty() ? stream : stream + " · " + who;
  if (who.empty() || c.stream_count <= 1) return who;
  // A stereo sender puts the same name on two cards; say which half each one is.
  return who + " (" + std::to_string(c.stream_index) + "/" + std::to_string(c.stream_count) + ")";
}

// The link behind a network input, for its dashboard card: who, in what state, sending what.
json net_link_json(const NetChannelStatus& c, const std::string& name) {
  return {{"name", name.empty() ? net_sender_name(c) : name},
          {"state", c.state},
          {"transport", c.transport},
          {"rate", c.rate},
          {"format", c.format},
          {"encoding", c.encoding}};
}

json meters_json(const AnalysisSnapshot& s) {
  json rms = json::array(), peak = json::array();
  for (unsigned c = 0; c < channels().total(); ++c) {
    rms.push_back(s.meters[c].rms_db);
    peak.push_back(s.meters[c].peak_db);
  }
  return json{{"type", "meters"}, {"sample", s.sample}, {"rms_db", rms}, {"peak_db", peak}};
}

json tone_json(const ToneMetrics& t) {
  const char* reason = tone_reason_name(t.reason);
  return json{{"valid", t.valid},
              {"freq_hz", t.freq_hz},
              {"thd_n_pct", t.thd_n_pct},
              {"peak_hz", t.peak_hz},
              {"peak_db", t.peak_db},
              {"reason", reason ? json(reason) : json(nullptr)}};
}

// One request's view of the inputs: what /api/state inputs[] and the genie replies call each one,
// from one copy of the names, the device sources and the NET status, so a reply agrees with itself.
struct InputInfo {
  const ChannelLayout& layout = channels();
  std::vector<std::string> names;  // config.json input_names
  std::vector<Devices::Source> sources;
  std::vector<NetChannelStatus> net;  // index = NET channel

  const NetChannelStatus* net_status(unsigned i) const {
    if (!layout.is_net(i)) return nullptr;
    const unsigned c = i - layout.net_base();
    return c < net.size() ? &net[c] : nullptr;
  }
  const std::string& configured_name(unsigned i) const {
    static const std::string none;
    return i < names.size() ? names[i] : none;
  }
  // A network channel with no name in config.json takes the sender's: the dashboard card then
  // reads "NET 1 — thinkpad.local" with nothing to configure, which is the whole point of
  // resolving the sender on this end. Once that sender has gone the name says so, or a script
  // picking inputs by name would take the channel a sender used to be on for the one it is on.
  std::string name(unsigned i) const {
    const NetChannelStatus* n = net_status(i);
    if (!configured_name(i).empty() || !n) return configured_name(i);
    std::string who = net_sender_name(*n);
    if (!who.empty() && n->state == "disconnected") who += " (gone)";
    return who;
  }
  std::string label(unsigned i) const { return input_label(layout, sources, i); }
  const char* kind(unsigned i) const { return input_kind_name(input_kind(layout, i)); }

  // label, name, kind, and for a network input who is on it: the genie replies' header per input.
  json genie_json(unsigned i) const {
    json j{{"label", label(i)}, {"name", name(i)}, {"kind", kind(i)}, {"net", nullptr}};
    if (const NetChannelStatus* n = net_status(i)) {
      j["net"] = {{"channel", n->channel},
                  {"device", n->device},
                  {"last_device", n->last_device},
                  {"stream", n->stream},
                  {"stream_index", n->stream_index},
                  {"stream_count", n->stream_count},
                  {"state", n->state}};
    }
    return j;
  }
};

// The shared genie input selector from a query: ch (comma list or repeated), device, stream,
// stream_index, kind. False, with the 400 already sent, on a malformed one.
bool parse_selector(const httplib::Request& req, InputSelector* sel, httplib::Response& res) {
  if (req.has_param("ch")) {
    std::vector<std::string> vals;
    for (size_t i = 0; i < req.get_param_value_count("ch"); ++i)
      vals.push_back(req.get_param_value("ch", i));
    std::string err;
    if (!parse_ch_list(vals, channels().total(), &sel->ch, &err)) {
      send_error(res, 400, err);
      return false;
    }
  }
  for (const char* key : {"device", "stream"}) {
    if (!req.has_param(key)) continue;
    if (req.get_param_value_count(key) > 1) {
      send_error(res, 400, std::string(key) + " takes one value");
      return false;
    }
    const std::string v = req.get_param_value(key);
    if (v.empty()) {
      send_error(res, 400, std::string(key) + " must not be empty");
      return false;
    }
    (std::string(key) == "device" ? sel->device : sel->stream) = v;
  }
  if (req.has_param("stream_index")) {
    const std::string v = req.get_param_value("stream_index");
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (v.empty() || *end || n < 1 || n > static_cast<long>(kMaxInputs)) {
      send_error(res, 400, "stream_index must be 1.." + std::to_string(kMaxInputs));
      return false;
    }
    sel->stream_index = static_cast<unsigned>(n);
  }
  if (req.has_param("kind")) {
    InputKind k;
    if (!parse_input_kind(req.get_param_value("kind"), &k)) {
      send_error(res, 400, "kind must be local, net or device");
      return false;
    }
    sel->kind = k;
  }
  return true;
}

// A failed resolution: 400 for a contradictory selector, 404 when no connected sender matches,
// with the channels it was last seen on.
void send_selection_error(httplib::Response& res, const InputSelection& s) {
  json j{{"error", s.error}};
  if (s.status == 404) {
    j["matched"] = 0;
    json seen = json::array();
    for (const LastSeen& l : s.last_seen) {
      seen.push_back({{"input", l.input},
                      {"channel", l.channel},
                      {"last_device", l.last_device},
                      {"last_stream", l.last_stream},
                      {"state", l.state}});
    }
    j["last_seen"] = seen;
  }
  send_json(res, j, s.status);
}

// What a selector resolved to, echoed in the reply so a caller sees where the sender is now.
json resolved_json(const InputSelector& sel, const InputSelection& s) {
  json j{{"inputs", s.inputs}};
  if (sel.kind) j["kind"] = input_kind_name(*sel.kind);
  if (!sel.by_sender()) return j;
  if (!sel.device.empty()) j["device"] = sel.device;
  if (!sel.stream.empty()) j["stream"] = sel.stream;
  if (sel.stream_index) j["stream_index"] = sel.stream_index;
  json runs = json::array();
  for (const SelectedRun& r : s.runs) {
    runs.push_back({{"inputs", r.inputs},
                    {"channel", r.base},
                    {"device", r.device},
                    {"stream", r.stream},
                    {"stream_count", r.stream_count},
                    {"state", r.state}});
  }
  j["runs"] = runs;
  return j;
}

// A finite number, strictly: unlike query_f64, "abc" or "-60dB" is an error, not 0. *out keeps
// its default when the param is absent.
bool query_number(const httplib::Request& req, const char* key, double* out) {
  if (!req.has_param(key)) return true;
  const std::string v = req.get_param_value(key);
  char* end = nullptr;
  const double d = std::strtod(v.c_str(), &end);
  if (v.empty() || *end || !std::isfinite(d)) return false;
  *out = d;
  return true;
}

// A comma list of doubles (expect_hz=1000,1500). False on an empty item or a non-number.
bool parse_f64_list(const std::string& v, std::vector<double>* out) {
  out->clear();
  size_t pos = 0;
  while (true) {
    const size_t comma = v.find(',', pos);
    const std::string item = v.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    char* end = nullptr;
    const double d = std::strtod(item.c_str(), &end);
    if (item.empty() || *end || !std::isfinite(d)) return false;
    out->push_back(d);
    if (comma == std::string::npos) return true;
    pos = comma + 1;
  }
}

int64_t unix_ms_now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// The wall clock when the engine was at `sample`, given where it is now (`now`, ring.counter()):
// what lines a genie reply's sample up with a DUT's log.
int64_t unix_ms_at(uint64_t sample, uint64_t now, double rate) {
  return unix_ms_now() -
         (rate > 0 && now > sample ? std::llround(1000.0 * (now - sample) / rate) : 0);
}

// The five fields shared by /api/state, /api/capture/freeze and /api/capture/status.
json capture_json(const CaptureStatus& cs) {
  return json{{"frozen", cs.frozen},
              {"freeze_sample", cs.freeze_sample},
              {"valid_start", cs.valid_start},
              {"valid_len", cs.valid_len},
              {"generation", cs.generation}};
}

// The host-health fields, identical in GET /api/state and the 1 Hz WS system message, so that
// the console does not have to know two shapes for the same information.
json sysinfo_json(const SysInfo& si) {
  return json{
      {"cpu_pct", si.cpu_pct},
      {"cpu_cores", si.cpu_cores},
      {"temp_c", si.temp_c},
      {"uptime_s", si.uptime_s},
      {"mem",
       {{"total_kb", si.mem.total_kb},
        {"used_kb", si.mem.used_kb},
        {"available_kb", si.mem.available_kb}}},
      {"throttle",
       {{"available", si.throttle.available},
        {"under_voltage", si.throttle.under_voltage},
        {"throttled", si.throttle.throttled},
        {"freq_capped", si.throttle.freq_capped},
        {"under_voltage_seen", si.throttle.under_voltage_seen},
        {"throttled_seen", si.throttle.throttled_seen}}}};
}

// The listen and telemetry handlers below only ever *send*. A send-only httplib WebSocket loop
// never observes the browser's Close frame: read() is what processes it (flips is_open(), echoes
// it so the browser can tear the TCP down) and what answers heartbeat pings — and nothing here
// reads. So after a clean browser ws.close() the socket stays "open" from the server's side, and
// the loop keeps streaming (a listen socket burning ~190 kB/s and one of the device's twelve
// slots) until a *write* finally fails. That write does not fail on a clean close: the browser
// holds the TCP connection open, draining, while it waits for the closing handshake we never
// complete. So the audio keeps coming long after the user stopped listening.
//
// This pump gives the connection its reader: one side thread that just drains inbound frames, so
// read() runs and the Close is seen within a chunk. It is the *only* reader, so it never races the
// paced send (the sole writer) on the stream. A parked reader at shutdown is not joined out here —
// its blocking recv only unwinds on the socket's 5-minute timeout — so main() hard-exits a couple
// of seconds after a stop is requested rather than stalling the reboot on this join.
class WsReadPump {
 public:
  explicit WsReadPump(httplib::ws::WebSocket& ws) {
    thread_ = std::thread([&ws] {
      std::string msg;
      while (ws.read(msg) != httplib::ws::Fail) { /* clients send nothing but the closing frame */ }
    });
  }
  ~WsReadPump() { if (thread_.joinable()) thread_.join(); }
  WsReadPump(const WsReadPump&) = delete;
  WsReadPump& operator=(const WsReadPump&) = delete;

 private:
  std::thread thread_;
};

// Binary envelope frame:
// [u8 type=2][u64 first sample][u16 ncols][u8 nchan] then ncols x nchan x {i16 min, i16 max}.
//
// The channel count is on the wire, and the type byte moved from 1 to 2, because the frame used
// to be a fixed six channels wide. A cached copy of app.js parsing the old layout would not fail
// — it would silently read every column at the wrong offset and draw plausible nonsense.
std::string envelope_frame(uint64_t first_col, unsigned col_frames,
                           const std::vector<EnvColumn>& cols) {
  std::string out;
  const unsigned total = channels().total();
  out.resize(12 + cols.size() * total * 4);
  char* p = out.data();
  *p++ = 2;
  const uint64_t sample = first_col * col_frames;
  std::memcpy(p, &sample, 8);
  p += 8;
  const uint16_t n = static_cast<uint16_t>(cols.size());
  std::memcpy(p, &n, 2);
  p += 2;
  *p++ = static_cast<char>(total);
  for (const auto& c : cols) {
    for (unsigned ch = 0; ch < total; ++ch) {
      std::memcpy(p, &c.min[ch], 2);
      p += 2;
      std::memcpy(p, &c.max[ch], 2);
      p += 2;
    }
  }
  return out;
}

}  // namespace

WebServer::WebServer(Deps deps, WebOptions opt) : d_(deps), opt_(std::move(opt)) {
  svr_ = std::make_unique<httplib::Server>();
}

WebServer::~WebServer() { stop(); }

void WebServer::install_routes() {
  httplib::Server& svr = *svr_;

  svr.set_mount_point("/", opt_.www_dir);
  // The console is replaced on disk (deploy-www, a reflash) while browsers still hold the last
  // copy, and a cached app.js against a newer index.html, or a newer daemon, breaks the page. With
  // no-cache every load revalidates against the ETag httplib sends: a 304 when nothing changed.
  svr.set_file_request_handler([](const httplib::Request&, httplib::Response& res) {
    res.set_header("Cache-Control", "no-cache");
  });
  // An /api path no route took (a guessed name, or the wrong method) answers JSON like every other
  // API error, not httplib's empty 404 that a `| jq` turns into a parse error. A handler's own 404
  // already has a body and is left alone, and so is anything outside /api.
  svr.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
    using HR = httplib::Server::HandlerResponse;
    if (res.status != 404 || !res.body.empty() || req.path.rfind("/api/", 0) != 0)
      return HR::Unhandled;
    send_error(res, 404, "no such endpoint: " + req.method + " " + req.path + "; see GET /api");
    return HR::Handled;
  });

  // The API index: GET /api serves docs/api.md rendered to HTML (www/api.html, produced at build
  // time by the md2html tool). Read from disk per request, like the rest of www, so a deploy-www
  // refresh takes effect without a restart. Also reachable as the static /api.html.
  auto api_index = [this](const httplib::Request&, httplib::Response& res) {
    std::ifstream f(opt_.www_dir + "/api.html", std::ios::binary);
    if (!f) return send_error(res, 404, "api.html not found");
    std::stringstream ss;
    ss << f.rdbuf();
    res.set_content(ss.str(), "text/html; charset=utf-8");
  };
  svr.Get("/api", api_index);
  svr.Get("/api/", api_index);

  // One copy of everything a reply names an input by, taken once per request.
  auto input_info = [this] {
    InputInfo info;
    {
      std::lock_guard<std::mutex> lk(config_m_);
      info.names = d_.config.input_names;
    }
    info.sources = d_.devices.sources();
    info.net = d_.net.status();
    return info;
  };

  // GET /api/state inputs[], which GET /api/inputs also answers on its own.
  auto inputs_json = [this](const InputInfo& info, const AnalysisSnapshot& as) {
    const ChannelLayout& layout = info.layout;
    // A device input's columns are shown once a device is on them, and stay, like a network
    // channel's: audio it captured before it was unplugged is still in the ring.
    auto input_active = [&](unsigned i) {
      if (layout.is_net(i)) return d_.net.channel_in_use(i - layout.net_base());
      if (!layout.is_device(i)) return true;
      for (const Devices::Source& s : info.sources)
        if (i >= s.first && i < s.first + s.channels) return true;
      return false;
    };
    json ins = json::array();
    for (unsigned i = 0; i < layout.total(); ++i) {
      const NetChannelStatus* n = info.net_status(i);
      ins.push_back({{"ch", i},
                     {"name", info.name(i)},
                     {"net", n ? net_link_json(*n, info.configured_name(i)) : json(nullptr)},
                     {"kind", info.kind(i)},
                     {"label", info.label(i)},
                     {"active", input_active(i)},
                     {"gain_db", d_.ctl.inputs[i].gain_db.load()},
                     {"mute", d_.ctl.inputs[i].mute.load()},
                     // The sender on this channel asked for its stream to be left alone: gain and
                     // mute are ignored here and in alsamixer until it disconnects.
                     {"bypass", d_.ctl.inputs[i].bypass.load()},
                     {"gain_min_db", layout.gain_min_db(i)},
                     {"gain_max_db", layout.gain_max_db(i)},
                     {"rms_db", as.meters[i].rms_db},
                     {"peak_db", as.meters[i].peak_db},
                     {"tone", tone_json(as.tone[i])}});
    }
    return ins;
  };

  svr.Get("/api/inputs", [this, input_info, inputs_json](const httplib::Request&,
                                                         httplib::Response& res) {
    const InputInfo info = input_info();
    send_json(res, inputs_json(info, d_.analysis.snapshot()));
  });

  svr.Get("/api/state", [this, input_info, inputs_json](const httplib::Request&,
                                                       httplib::Response& res) {
    const int64_t unix_ms = unix_ms_now();
    const EngineStats es = d_.engine.stats();
    const AnalysisSnapshot as = d_.analysis.snapshot();
    const CaptureStatus cs = d_.capture.status();
    // The publisher's copy, not a fresh sample: see the comment on sys_ in webserver.h.
    SysInfo si;
    {
      std::lock_guard<std::mutex> lk(sys_m_);
      si = sys_;
    }
    // Copies, because a concurrent /api/config/save may replace d_.config.
    std::vector<std::string> out_names;
    int64_t loopback_offset = 0;
    {
      std::lock_guard<std::mutex> lk(config_m_);
      out_names = d_.config.output_names;
      loopback_offset = d_.config.loopback_offset_samples;
    }
    const ChannelLayout& layout = channels();

    json sys_json = sysinfo_json(si);
    sys_json.update(json{{"sync_errors", d_.kmsg.sync_errors()},
                         {"hostname", si.hostname},
                         {"ips", si.ips},
                         {"has_saved_config", d_.store.has_saved()},
                         {"data_persistent", d_.store.is_persistent()},
                         {"listen_streams", listen_streams_.load()},
                         {"loopback_offset_samples", loopback_offset}});

    json outs = json::array();
    for (unsigned i = 0; i < layout.outputs; ++i) {
      json o = output_json(d_.ctl.outputs[i], i, out_names[i]);
      o["label"] = "OUT " + std::to_string(i + 1);  // as the console names it
      outs.push_back(std::move(o));
    }


    const InputInfo info = input_info();
    const std::vector<Devices::Source>& sources = info.sources;
    const json ins = inputs_json(info, as);

    json imap = json::array(), omap = json::array();
    for (unsigned i = 0; i < kInputs; ++i) imap.push_back(d_.ctl.input_map[i].load());
    for (unsigned i = 0; i < kOutputs; ++i) omap.push_back(d_.ctl.output_map[i].load());

    json cap = capture_json(cs);
    cap["analyze_frames"] = d_.capture.analyze_frames();

    json sinks = json::array();
    for (const Devices::Sink& k : d_.devices.sinks()) sinks.push_back(sink_json(d_.devices, k));
    json devs = json::array();
    for (const DeviceStatus& d : d_.devices.devices()) devs.push_back(device_json(d));
    json srcs = json::array();
    for (const Devices::Source& s : sources) srcs.push_back(source_json(s));

    json j{
        {"version", {{"git", opt_.version_git}, {"build_time", opt_.build_time}}},
        {"unix_ms", unix_ms},
        {"daemon_uptime_s",
         std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count()},
        {"inputs", ins},
        {"outputs", outs},
        {"sinks", sinks},
        {"sources", srcs},
        {"devices", devs},
        {"generators",
         {{"sine", {{"freq_hz", d_.ctl.sine.freq_hz.load()}, {"level_db", d_.ctl.sine.level_db.load()}}},
          {"noise",
           {{"mode", noise_name(static_cast<NoiseMode>(d_.ctl.noise.mode.load()))},
            {"level_db", d_.ctl.noise.level_db.load()}}},
          {"ping",
           {{"variant", ping_name(static_cast<PingVariant>(d_.ctl.ping.variant.load()))},
            {"interval_s", d_.ctl.ping.interval_s.load()},
            {"level_db", d_.ctl.ping.level_db.load()}}},
          {"music", {{"level_db", d_.ctl.music.level_db.load()}}}}},
        {"channel_map", {{"input_map", imap}, {"output_map", omap}}},
        {"capture", cap},
        {"engine",
         {{"running", es.running},
          {"sim", es.sim},
          {"backend", es.backend},
          {"device", es.device},
          {"rate", es.rate},
          {"period", es.period},
          {"periods", es.periods},
          {"format", es.format},
          {"capture_channels", es.capture_channels},
          {"xruns", es.xruns},
          {"generation", es.generation},
          {"samples", es.samples},
          {"last_error", es.last_error}}},
        {"system", sys_json},
        {"limits",
         {{"env_column_frames", d_.analysis.env_column_frames()},
          {"capture_max_frames", d_.capture.max_frames()},
          {"capture_config", true},
          {"input_gain_min_db", kInputGainMinDb},
          {"input_gain_max_db", kInputGainMaxDb},
          {"net_gain_min_db", kNetGainMinDb},
          {"net_gain_max_db", kNetGainMaxDb},
          {"input_mute", true},
          {"telemetry_mask", true},
          {"listen_codecs",
           opus_rate_supported(es.rate) ? json::array({"pcm", "opus"}) : json::array({"pcm"})},
          {"listen_default_codec", codec_name(static_cast<ListenCodec>(d_.ctl.listen.codec.load()))},
          {"listen_bitrate_kbps", d_.ctl.listen.bitrate_kbps.load()},
          {"listen_bitrate_min_kbps", kListenBitrateMinKbps},
          {"listen_bitrate_max_kbps", kListenBitrateMaxKbps},
          {"opus_rate", kOpusRate},
          {"inputs_total", layout.total()},
          {"inputs_local", layout.local},
          {"inputs_device", layout.device},
          {"outputs", layout.outputs},
          // The TDM slot map and the I2S sync watch are the engine card's.
          {"channel_map", layout.local > 0},
          {"sync_watch", layout.local > 0},
          {"sinks_max", kMaxSinks},
          {"net_inputs", layout.net},
          {"net_inputs_configured", d_.ctl.net.inputs.load()},
          {"net_inputs_min", kNetInputsMin},
          {"net_inputs_max", opt_.net_inputs_max},
          {"restart", opt_.allow_reboot},
          {"net_port", d_.net.port()},
          {"net_delay_ms", d_.ctl.net.delay_ms.load()},
          {"net_delay_min_ms", kNetDelayMinMs},
          {"net_delay_max_ms", kNetDelayMaxMs},
          {"net", true},
          {"net_forget", true},
          {"bluetooth", d_.bt.status().running},
          {"pinned_mb", (d_.ring.pinned_bytes() + d_.capture.pinned_bytes() +
                         d_.devices.pinned_bytes()) /
                            (1024 * 1024)}}},
    };
    send_json(res, j);
  });

  // The 10 Hz WS "meters" message as a synchronous GET, for headless clients that just want the
  // current per-input level without opening a WebSocket ("which channel has sound"). Same shape as
  // the WS frame (type/sample/rms_db/peak_db) so one parser handles both. rms_db is a 100 ms
  // window; peak_db carries a 3 s hold. Both are post input-gain — see docs/api.md.
  svr.Get("/api/meters", [this](const httplib::Request&, httplib::Response& res) {
    send_json(res, meters_json(d_.analysis.snapshot()));
  });

  // The WS "spectrum" message as a synchronous GET. Adds `bins_hz` — the center frequency of each
  // of the 240 log-spaced bins — which the WS frame omits (the console reconstructs it), so a
  // script can threshold by frequency directly. Full float precision here (one-shot), unlike the
  // WS frame's 0.1 dB quantisation. Inputs by the genie selector (`?ch=4,5`, `?device=`, `?kind=`);
  // none for every channel.
  svr.Get("/api/spectrum", [this](const httplib::Request& req, httplib::Response& res) {
    InputSelector sel;
    if (!parse_selector(req, &sel, res)) return;
    const InputSelection pick = resolve_inputs(sel, channels(), d_.net.status());
    if (!pick.ok()) return send_selection_error(res, pick);
    if (pick.inputs.empty()) return send_error(res, 400, "the selector matches no inputs on this board");
    const AnalysisSnapshot s = d_.analysis.snapshot();
    json chans = json::array();
    for (const unsigned c : pick.inputs) {
      chans.push_back({{"ch", c}, {"bins_db", s.spectrum[c]}, {"tone", tone_json(s.tone[c])}});
    }
    send_json(res, json{{"sample", s.sample},
                        {"bins_hz", d_.analysis.bin_hz()},
                        {"channels", chans}});
  });

  svr.Put("/api/inputs/:ch", json_channel_route("ch", channels().total(), "no such input",
      [this](unsigned ch, const json& j, const httplib::Request&, httplib::Response&) {
    if (j.contains("gain_db")) {
      d_.ctl.inputs[ch].gain_db.store(
          std::clamp(j["gain_db"].get<float>(), channels().gain_min_db(ch), channels().gain_max_db(ch)));
    }
    if (j.contains("mute")) d_.ctl.inputs[ch].mute.store(j["mute"].get<bool>());
  }));

  // Per-input telemetry mask. The console posts which inputs it is watching; disabled inputs are
  // dropped from the spectrum message, the widest frame on the wire. Meters and the envelope frame
  // always carry every input, and the console hides disabled inputs itself regardless. Global and last-writer-wins — this appliance has one
  // operator; see docs/api.md.
  svr.Post("/api/telemetry/inputs", json_route([this](const json& j, const httplib::Request&,
                                                      httplib::Response& res) {
    const auto& en = j.at("enabled");
    if (!en.is_array() || en.size() != channels().total()) {
      return send_error(res, 400, "enabled must be an array of " +
                                      std::to_string(channels().total()) + " booleans");
    }
    uint32_t mask = 0;
    for (unsigned c = 0; c < channels().total(); ++c) {
      if (en[c].get<bool>()) mask |= (1u << c);
    }
    telemetry_mask_.store(mask, std::memory_order_relaxed);
  }));

  // Default listen codec + per-channel Opus bitrate. Body: {"codec": "pcm"|"opus"} and/or
  // {"bitrate_kbps": N}. The bitrate is clamped and applied live (active Opus streams follow it);
  // the reply echoes what took effect. The codec is only a default: the browser opts into Opus
  // per connection with ?codec=opus, so this never changes the raw-PCM wire default.
  svr.Post("/api/listen/codec", json_route([this](const json& j, const httplib::Request&,
                                                  httplib::Response& res) {
    if (j.contains("codec")) {
      ListenCodec c;
      if (!parse_codec(j.at("codec").get<std::string>(), &c)) {
        return send_error(res, 400, "codec must be pcm or opus");
      }
      d_.ctl.listen.codec.store(static_cast<uint8_t>(c));
    }
    if (j.contains("bitrate_kbps")) {
      const int br = std::clamp(j.at("bitrate_kbps").get<int>(), kListenBitrateMinKbps,
                                kListenBitrateMaxKbps);
      d_.ctl.listen.bitrate_kbps.store(br);
    }
    send_json(res, json{{"codec", codec_name(static_cast<ListenCodec>(d_.ctl.listen.codec.load()))},
                        {"bitrate_kbps", d_.ctl.listen.bitrate_kbps.load()}});
  }));

  svr.Put("/api/outputs/:ch", json_channel_route("ch", channels().outputs, "no such output",
      [this](unsigned ch, const json& j, const httplib::Request&, httplib::Response& res) {
    apply_output_put(d_.ctl.outputs[ch], j, res);
  }));

  svr.Post("/api/outputs/:ch/identify", [this](const httplib::Request& req, httplib::Response& res) {
    unsigned ch = 0;
    if (!parse_index(req, "ch", channels().outputs, &ch))
      return send_error(res, 404, "no such output");
    d_.ctl.outputs[ch].identify_until.store(d_.ring.counter() + d_.engine.identify_frames());
    send_json(res, json{{"ok", true}});
  });

  // ---- Sinks: every playback device other than the engine card ------------------------------
  //
  // Every channel routes exactly like the engine card's outputs — same sources, same clamps, same
  // Identify — and plays at the same n. Only the link itself (on/off, rate, layout) is configured
  // here. A sink is named by its device id ("b1,0", "Device,0"); GET /api/state lists them.
  auto find_sink = [this](const httplib::Request& req, Devices::Sink* out) {
    const auto it = req.path_params.find("id");
    if (it == req.path_params.end()) return false;
    for (const Devices::Sink& k : d_.devices.sinks()) {
      if (k.device.id == it->second) {
        *out = k;
        return true;
      }
    }
    return false;
  };
  const std::string no_sink = "no such sink";

  svr.Get("/api/sinks", [this](const httplib::Request&, httplib::Response& res) {
    json a = json::array();
    for (const Devices::Sink& k : d_.devices.sinks()) a.push_back(sink_json(d_.devices, k));
    send_json(res, a);
  });

  svr.Get("/api/sinks/:id", [this, find_sink, no_sink](const httplib::Request& req,
                                                       httplib::Response& res) {
    Devices::Sink k;
    if (!find_sink(req, &k)) return send_error(res, 404, no_sink);
    send_json(res, sink_json(d_.devices, k));
  });

  // Body: {"enabled": bool, "sample_rate": 48000, "layout": "5.1"}. A new rate or layout restarts
  // the sink's thread; the engine is not touched either way.
  svr.Put("/api/sinks/:id", [this, find_sink, no_sink](const httplib::Request& req,
                                                       httplib::Response& res) {
    Devices::Sink k;
    if (!find_sink(req, &k)) return send_error(res, 404, no_sink);
    std::lock_guard<std::mutex> put_lk(sink_put_m_);
    json_route([this, k](const json& j, const httplib::Request&, httplib::Response& res) {
      SinkOutput& out = d_.devices.output(k.slot);
      SinkControl& sc = out.control();
      const SinkDevice& dev = k.device;
      // Everything is checked before anything is applied, so a rejected body changes nothing.
      unsigned sample_rate = out.sample_rate();
      const SinkLayout layout_was = sink_layout(sc);
      SinkLayout layout = layout_was;
      if (j.contains("sample_rate")) {
        sample_rate = j["sample_rate"].get<unsigned>();
        if (!dev.offers_rate(sample_rate))
          return send_error(res, 400, "sample_rate must be " + rates_text(dev.rates));
      }
      if (j.contains("layout")) {
        if (!parse_sink_layout(j["layout"].get<std::string>(), &layout) || !dev.offers(layout))
          return send_error(res, 400, "layout must be " + layouts_text(dev.layouts));
      }
      if (!surround_rate_ok(layout, sample_rate)) {
        return send_error(res, 400, std::string(sink_layout_name(layout)) +
                                        " needs a sample_rate of 48000 or less: HDMI carries "
                                        "more than two channels only up to 48 kHz");
      }

      // Stored before any restart, so one reopen picks up everything in this request.
      sc.layout.store(static_cast<uint8_t>(layout));
      if (sample_rate != out.sample_rate()) {
        out.set_sample_rate(sample_rate);
      } else if (layout != layout_was) {
        out.restart();
      }
      if (j.contains("enabled")) {
        const bool on = j["enabled"].get<bool>();
        // The audio thread starts rendering the channels before the sink's thread goes looking
        // for them, and stops only after that thread is gone.
        if (on) {
          sc.enabled.store(true);
          out.start();
        } else {
          out.stop();
          sc.enabled.store(false);
        }
      }
      send_json(res, sink_json(d_.devices, k));
    })(req, res);
  });

  // Indexed by channel: an HDMI sink's speakers are 0 L, 1 R, 2 C, 3 LFE, 4 Ls, 5 Rs, 6 Lb, 7 Rb;
  // any other device's channels are its own. Any of them takes a route, played or not, so a layout
  // can be set up before it is switched on.
  svr.Put("/api/sinks/:id/:ch", [this, find_sink, no_sink](const httplib::Request& req,
                                                           httplib::Response& res) {
    Devices::Sink k;
    if (!find_sink(req, &k)) return send_error(res, 404, no_sink);
    SinkControl& sc = d_.devices.output(k.slot).control();
    json_channel_route("ch", kMaxSinkWidth, "no such sink channel",
        [&sc](unsigned ch, const json& j, const httplib::Request&, httplib::Response& r) {
      apply_output_put(sc.outputs[ch], j, r);
    })(req, res);
  });

  svr.Post("/api/sinks/:id/:ch/identify", [this, find_sink, no_sink](const httplib::Request& req,
                                                                     httplib::Response& res) {
    Devices::Sink k;
    if (!find_sink(req, &k)) return send_error(res, 404, no_sink);
    unsigned ch = 0;
    if (!parse_index(req, "ch", kMaxSinkWidth, &ch))
      return send_error(res, 404, "no such sink channel");
    d_.devices.output(k.slot).control().outputs[ch].identify_until.store(
        d_.ring.counter() + d_.engine.identify_frames());
    send_json(res, json{{"ok", true}});
  });

  // Every PCM device ALSA has, as `aplay -l` and `arecord -l` would list them, and what the
  // daemon does with each.
  svr.Get("/api/devices", [this](const httplib::Request&, httplib::Response& res) {
    json a = json::array();
    for (const DeviceStatus& d : d_.devices.devices()) a.push_back(device_json(d));
    send_json(res, a);
  });

  svr.Get("/api/sources", [this](const httplib::Request&, httplib::Response& res) {
    json a = json::array();
    for (const Devices::Source& s : d_.devices.sources()) a.push_back(source_json(s));
    send_json(res, a);
  });

  // ---- Bluetooth: the adapter, pairing, and the input ----------------------------------------
  //
  // The output is the sink kBtSinkId, routed through /api/sinks like any other. Everything here
  // returns at once: the manager's own thread does the D-Bus work, and anything slow (a pairing can
  // wait a minute for someone to tap their phone) reports its outcome on the device in the next GET.
  auto bt_state = [this] {
    const BtStatus b = d_.bt.status();
    json devices = json::array();
    for (const BtDeviceInfo& d : b.devices) devices.push_back(bt_device_json(d));
    return json{{"available", b.available},
                {"error", b.error},
                {"audio", b.audio},
                {"adapter",
                 {{"address", b.adapter.address},
                  {"name", b.adapter.name},
                  {"powered", b.adapter.powered},
                  {"discoverable", b.adapter.discoverable},
                  {"discoverable_timeout_s", b.adapter.discoverable_timeout_s},
                  {"pairable", b.adapter.pairable},
                  {"discovering", b.adapter.discovering}}},
                {"settings",
                 {{"enabled", b.settings.enabled},
                  {"name", b.settings.name},
                  {"pairable", b.settings.pairable},
                  {"discoverable_timeout_s", b.settings.discoverable_timeout_s}}},
                {"devices", devices},
                {"request", bt_request_json(b)},
                {"input", bt_input_json(d_.bt_in.status())},
                {"output", bt_output_json(d_.devices, d_.bt)}};
  };

  svr.Get("/api/bluetooth", [bt_state](const httplib::Request&, httplib::Response& res) {
    send_json(res, bt_state());
  });

  // Body: {"enabled", "name", "pairable", "discoverable", "discoverable_timeout_s"}, every field
  // optional. All but `discoverable` are settings a save keeps.
  svr.Put("/api/bluetooth", json_route([this, bt_state](const json& j, const httplib::Request&,
                                                        httplib::Response& res) {
    BtSettings s = d_.bt.settings();
    // Every field is read before anything is applied, so a rejected body changes nothing.
    const bool set_disc = j.contains("discoverable");
    const bool disc = set_disc && j["discoverable"].get<bool>();
    if (j.contains("name")) {
      s.name = j["name"].get<std::string>();
      // BlueZ refuses a longer alias, and an empty one means "the hostname" here.
      if (s.name.size() > 248) return send_error(res, 400, "name is at most 248 bytes");
    }
    if (j.contains("enabled")) s.enabled = j["enabled"].get<bool>();
    if (j.contains("pairable")) s.pairable = j["pairable"].get<bool>();
    if (j.contains("discoverable_timeout_s")) {
      s.discoverable_timeout_s = static_cast<unsigned>(std::clamp<long long>(
          j["discoverable_timeout_s"].get<long long>(), 0, kBtDiscoverableMaxS));
    }
    d_.bt.apply(s);
    {
      std::lock_guard<std::mutex> lk(config_m_);
      BtConfig& c = d_.config.bluetooth;
      c.enabled = s.enabled;
      c.name = s.name;
      c.pairable = s.pairable;
      c.discoverable_timeout_s = s.discoverable_timeout_s;
    }
    if (set_disc) d_.bt.set_discoverable(disc);
    send_json(res, bt_state());
  }));

  svr.Post("/api/bluetooth/scan", json_route([this](const json& j, const httplib::Request&,
                                                    httplib::Response&) {
    d_.bt.scan(j.at("on").get<bool>());
  }));

  // Pair, connect, disconnect, forget and trust, by address. A 404 for an address that is not
  // well-formed or not known, so a typo reads as what it is.
  auto bt_device_route = [this](BtManager::Action action) {
    return [this, action](const httplib::Request& req, httplib::Response& res) {
      const auto it = req.path_params.find("addr");
      if (it == req.path_params.end() || !bt_address_ok(it->second)) {
        return send_error(res, 404, "no such device");
      }
      std::string pin;
      if (action == BtManager::Action::Pair && !req.body.empty()) {
        try {
          const json j = json::parse(req.body);
          pin = j.value("pin", std::string{});
        } catch (const std::exception& e) {
          return send_error(res, 400, e.what());
        }
        if (pin.size() > 16) return send_error(res, 400, "a PIN is at most 16 characters");
      }
      std::string err;
      if (!d_.bt.device_action(it->second, action, pin, &err)) {
        return send_error(res, err == "no such device" ? 404 : 503, err);
      }
      send_ok(res);
    };
  };
  svr.Post("/api/bluetooth/devices/:addr/pair", bt_device_route(BtManager::Action::Pair));
  svr.Post("/api/bluetooth/devices/:addr/connect", bt_device_route(BtManager::Action::Connect));
  svr.Post("/api/bluetooth/devices/:addr/disconnect",
           bt_device_route(BtManager::Action::Disconnect));
  svr.Delete("/api/bluetooth/devices/:addr", bt_device_route(BtManager::Action::Remove));

  svr.Put("/api/bluetooth/devices/:addr", [this](const httplib::Request& req,
                                                 httplib::Response& res) {
    const auto it = req.path_params.find("addr");
    if (it == req.path_params.end() || !bt_address_ok(it->second)) {
      return send_error(res, 404, "no such device");
    }
    json_route([this, addr = it->second](const json& j, const httplib::Request&,
                                         httplib::Response& r) {
      std::string err;
      if (j.contains("volume") && !d_.bt.set_volume(addr, j["volume"].get<int>(), &err)) {
        send_error(r, err == "no such device" ? 404 : 409, err);
      }
    })(req, res);
  });

  // Body: {"command": "play" | "pause" | "stop" | "next" | "previous"}, to the device's AVRCP
  // media player: the remote control for a phone that is playing to this one.
  svr.Post("/api/bluetooth/devices/:addr/player", [this](const httplib::Request& req,
                                                         httplib::Response& res) {
    const auto it = req.path_params.find("addr");
    if (it == req.path_params.end() || !bt_address_ok(it->second)) {
      return send_error(res, 404, "no such device");
    }
    json_route([this, addr = it->second](const json& j, const httplib::Request&,
                                         httplib::Response& r) {
      std::string err;
      if (!d_.bt.player_command(addr, j.at("command").get<std::string>(), &err)) {
        send_error(r, err == "no such device" ? 404 : 409, err);
      }
    })(req, res);
  });

  // Body: {"id": N, "accept": bool} plus "pin" or "passkey" for those kinds of request.
  svr.Post("/api/bluetooth/request", json_route([this](const json& j, const httplib::Request&,
                                                       httplib::Response& res) {
    std::string value;
    if (j.contains("pin")) value = j["pin"].get<std::string>();
    if (j.contains("passkey")) {
      const json& p = j["passkey"];
      value = p.is_string() ? p.get<std::string>() : std::to_string(p.get<long long>());
    }
    std::string err;
    if (!d_.bt.answer(j.at("id").get<uint64_t>(), j.value("accept", false), value, &err)) {
      send_error(res, 409, err);
    }
  }));

  // Body: {"device": "bluealsa:DEV=AA:BB:CC:DD:EE:FF,PROFILE=a2dp", "enabled": bool}, both
  // optional: which speaker the Bluetooth sink plays to, and a switch for it in the same request, so
  // "Play here" is one call. Its routing, rate and the rest are /api/sinks/bluetooth's.
  svr.Put("/api/bluetooth/output", json_route([this](const json& j, const httplib::Request&,
                                                     httplib::Response& res) {
    const int slot = d_.devices.slot_of(kBtSinkId);
    if (slot < 0) return send_error(res, 503, d_.bt.status().error);
    std::string device;
    if (j.contains("device")) {
      device = j["device"].get<std::string>();
      if (device.rfind("bluealsa:", 0) != 0)
        return send_error(res, 400, "device must be a bluez-alsa PCM (bluealsa:DEV=...)");
    }
    std::lock_guard<std::mutex> put_lk(sink_put_m_);
    if (!device.empty()) {
      d_.devices.retarget(kBtSinkId, device);
      // Not live state, so it rides the config rather than Control; a later save keeps it.
      std::lock_guard<std::mutex> lk(config_m_);
      d_.config.bluetooth.output_device = device;
    }
    if (j.contains("enabled")) {
      SinkOutput& out = d_.devices.output(static_cast<unsigned>(slot));
      // As PUT /api/sinks/{id}: rendering starts before the sink's thread, and stops after it.
      if (j["enabled"].get<bool>()) {
        out.control().enabled.store(true);
        out.start();
      } else {
        out.stop();
        out.control().enabled.store(false);
      }
    }
    send_json(res, bt_output_json(d_.devices, d_.bt));
  }));

  // Body: {"enabled": bool}. Turning it on also turns on the alignment delay, as network input
  // does: a phone is a sender on the network channels in every way but the transport.
  svr.Put("/api/bluetooth/input", json_route([this](const json& j, const httplib::Request&,
                                                    httplib::Response& res) {
    if (j.contains("enabled")) {
      const bool on = j["enabled"].get<bool>();
      if (on && !d_.bt.status().running) return send_error(res, 503, d_.bt.status().error);
      std::lock_guard<std::mutex> lk(delay_m_);
      // The delay is on before the reader starts and off only after it has stopped, so the
      // input never writes against a playout position with no room in front of it.
      if (on) {
        d_.ctl.net.bt_input.store(true);
        d_.ctl.net.delay_frames.store(net_delay_frames(d_.ctl.net, d_.engine.rate()));
        d_.bt_in.start();
      } else {
        d_.bt_in.stop();
        d_.ctl.net.bt_input.store(false);
        d_.ctl.net.delay_frames.store(net_delay_frames(d_.ctl.net, d_.engine.rate()));
      }
    }
    send_json(res, bt_input_json(d_.bt_in.status()));
  }));

  svr.Put("/api/generators/sine", json_route([this](const json& j, const httplib::Request&,
                                                    httplib::Response&) {
    if (j.contains("freq_hz")) {
      d_.ctl.sine.freq_hz.store(std::clamp(j["freq_hz"].get<float>(), kSineFreqMinHz, kSineFreqMaxHz));
    }
    if (j.contains("level_db")) {
      d_.ctl.sine.level_db.store(std::clamp(j["level_db"].get<float>(), kLevelMinDb, kLevelMaxDb));
    }
  }));

  svr.Put("/api/generators/noise", json_route([this](const json& j, const httplib::Request&,
                                                     httplib::Response& res) {
    if (j.contains("mode")) {
      NoiseMode m;
      if (!parse_noise(j["mode"].get<std::string>(), &m)) {
        return send_error(res, 400, "mode must be white or pink");
      }
      d_.ctl.noise.mode.store(static_cast<uint8_t>(m));
    }
    if (j.contains("level_db")) {
      d_.ctl.noise.level_db.store(std::clamp(j["level_db"].get<float>(), kLevelMinDb, kLevelMaxDb));
    }
  }));

  svr.Put("/api/generators/ping", json_route([this](const json& j, const httplib::Request&,
                                                    httplib::Response& res) {
    if (j.contains("variant")) {
      PingVariant v;
      if (!parse_ping(j["variant"].get<std::string>(), &v)) {
        return send_error(res, 400, "variant must be tick, bing or bong");
      }
      d_.ctl.ping.variant.store(static_cast<uint8_t>(v));
    }
    if (j.contains("interval_s")) {
      d_.ctl.ping.interval_s.store(
          std::clamp(j["interval_s"].get<float>(), kPingIntervalMinS, kPingIntervalMaxS));
    }
    if (j.contains("level_db")) {
      d_.ctl.ping.level_db.store(std::clamp(j["level_db"].get<float>(), kLevelMinDb, kLevelMaxDb));
    }
    // Tells the generator to reschedule from the current sample.
    d_.ctl.ping.epoch.fetch_add(1);
  }));

  svr.Put("/api/generators/music", json_route([this](const json& j, const httplib::Request&,
                                                     httplib::Response&) {
    if (j.contains("level_db")) {
      d_.ctl.music.level_db.store(std::clamp(j["level_db"].get<float>(), kLevelMinDb, kLevelMaxDb));
    }
  }));

  svr.Put("/api/channel-map", json_route([this](const json& j, const httplib::Request&,
                                                httplib::Response& res) {
    if (!channels().local) return send_error(res, 404, "no engine card, so no TDM slots to map");
    const auto imap = j.at("input_map").get<std::vector<int>>();
    const auto omap = j.at("output_map").get<std::vector<int>>();
    if (imap.size() != kInputs || omap.size() != kOutputs) {
      return send_error(res, 400, "input_map needs 6 entries, output_map 8");
    }
    if (!is_slot_permutation(imap, kTdmSlots) || !is_slot_permutation(omap, kOutputs)) {
      return send_error(res, 400, "slots must be distinct and in range");
    }
    for (unsigned i = 0; i < kInputs; ++i) {
      d_.ctl.input_map[i].store(static_cast<uint8_t>(imap[i]));
    }
    for (unsigned i = 0; i < kOutputs; ++i) {
      d_.ctl.output_map[i].store(static_cast<uint8_t>(omap[i]));
    }
  }));

  // ---- network audio input --------------------------------------------------------------
  //
  // A network channel is an input like any other once it is here: it is metered, it appears in
  // the scope, it can be routed to an output with {"type":"input","index":N}, and it is a legal
  // xcorr operand against an ADC channel. This endpoint is only about the link itself.
  svr.Get("/api/net", [this](const httplib::Request& req, httplib::Response& res) {
    // ?connected=1: only the channels a sender is on (or =0: only the others), so 16 empty rows
    // do not bury the two that matter.
    int want_connected = -1;
    if (req.has_param("connected")) {
      const std::string v = req.get_param_value("connected");
      if (v == "1" || v == "true") want_connected = 1;
      else if (v == "0" || v == "false") want_connected = 0;
      else return send_error(res, 400, "connected must be 1 or 0");
    }
    const double rate = d_.engine.rate();
    const AnalysisSnapshot as = d_.analysis.snapshot();
    const unsigned net_base = channels().net_base();
    json chans = json::array();
    for (const NetChannelStatus& c : d_.net.status()) {
      if (want_connected >= 0 && c.connected != (want_connected == 1)) continue;
      const unsigned input = net_base + c.channel;
      chans.push_back({{"channel", c.channel},
                       {"input", input},
                       {"connected", c.connected},
                       {"connects", c.connects},
                       {"connected_since_ms", c.connected_since_unix_ms
                                                  ? json(c.connected_since_unix_ms)
                                                  : json(nullptr)},
                       {"peer", c.peer},
                       {"name", c.name},
                       {"stream", c.stream},
                       {"last_stream", c.last_stream},
                       {"host", c.host},
                       {"device", c.device},
                       {"last_device", c.last_device},
                       {"mac", c.mac},
                       {"transport", c.transport},
                       {"state", c.state},
                       {"sender_rate", c.rate},
                       {"format", c.format},
                       {"encoding", c.encoding},
                       {"stream_index", c.stream_index},
                       {"stream_count", c.stream_count},
                       {"frames_received", c.frames_received},
                       {"frames_per_s", c.frames_per_s ? json(*c.frames_per_s) : json(nullptr)},
                       {"last_audio_ms_ago",
                        c.last_audio_ms_ago ? json(*c.last_audio_ms_ago) : json(nullptr)},
                       {"late_drops", c.late_drops},
                       {"range_drops", c.range_drops},
                       {"underruns", c.underruns},
                       {"underrun_events", c.underrun_events},
                       {"resyncs", c.resyncs},
                       {"last_target", c.last_target},
                       {"write_end", c.write_end},
                       {"lead_frames", c.lead_frames},
                       {"lead_valid", c.lead_valid},
                       {"target_lead_frames", c.target_lead_frames},
                       {"lead_error_ms", c.lead_valid && rate > 0
                            ? 1000.0 * (c.lead_frames - static_cast<int64_t>(c.target_lead_frames)) / rate
                            : 0.0},
                       {"peak", c.peak},
                       {"peak_db", as.meters[input].peak_db}});
    }
    const uint64_t delay = d_.ctl.net.delay_frames.load();
    send_json(res, json{{"enabled", d_.ctl.net.enabled.load()},
                        {"listening", d_.net.listening()},
                        {"port", d_.net.port()},
                        {"delay_ms", d_.ctl.net.delay_ms.load()},
                        {"delay_frames", delay},
                        {"rate", d_.engine.rate()},
                        {"n_now", d_.ring.counter()},
                        {"unix_ms", unix_ms_now()},
                        {"lead_seconds", delay / rate},
                        {"error", d_.net.last_error()},
                        {"inputs", d_.net.count()},
                        {"inputs_configured", d_.ctl.net.inputs.load()},
                        {"inputs_max", opt_.net_inputs_max},
                        {"channels", chans}});
  });

  // Body: {"enabled": bool, "delay_ms": N, "port": N}. Changing the port rebinds the listener,
  // which drops any connected sender — there is no way to move a listening socket without that.
  svr.Put("/api/net", json_route([this](const json& j, const httplib::Request&,
                                        httplib::Response& res) {
    const double rate = d_.engine.rate();

    // Checked before anything is applied, so a rejected body changes nothing.
    unsigned want_inputs = d_.ctl.net.inputs.load();
    if (j.contains("inputs")) {
      const int n = j["inputs"].get<int>();
      if (n < static_cast<int>(kNetInputsMin) || n > static_cast<int>(opt_.net_inputs_max)) {
        return send_error(res, 400, "inputs must be " + std::to_string(kNetInputsMin) + ".." +
                                        std::to_string(opt_.net_inputs_max));
      }
      want_inputs = static_cast<unsigned>(n);
    }
    int want_port = d_.net.port();
    if (j.contains("port")) {
      want_port = j["port"].get<int>();
      if (want_port < kNetPortMin || want_port > kNetPortMax) {
        return send_error(res, 400, "port must be " + std::to_string(kNetPortMin) + ".." +
                                        std::to_string(kNetPortMax));
      }
    }

    if (j.contains("delay_ms")) {
      const unsigned ms = static_cast<unsigned>(std::clamp<int>(
          j["delay_ms"].get<int>(), static_cast<int>(kNetDelayMinMs),
          static_cast<int>(kNetDelayMaxMs)));
      d_.ctl.net.delay_ms.store(ms);
    }
    std::lock_guard<std::mutex> lk(delay_m_);
    if (j.contains("enabled")) d_.ctl.net.enabled.store(j["enabled"].get<bool>());

    // The count only means anything at the next start, which reads the saved config: so it goes
    // straight there, and nothing else of the saved config changes with it.
    std::string inputs_error;
    if (want_inputs != d_.ctl.net.inputs.load()) {
      Config saved = d_.store.load();
      saved.remap_columns(channels());
      saved.net_inputs = want_inputs;
      if (d_.store.save(saved, &inputs_error)) {
        d_.ctl.net.inputs.store(want_inputs);
        LOG_INFO("net: {} network inputs from the next start", want_inputs);
      }
    }

    // Derived in exactly one expression, shared with Config::apply_to and the Bluetooth input, so
    // the paths cannot drift: zero unless something that lands on a network channel is on.
    const bool on = d_.ctl.net.enabled.load();
    d_.ctl.net.delay_frames.store(net_delay_frames(d_.ctl.net, rate));

    if (!on) {
      d_.net.stop();
      // Remembered while off: enabling later binds here, and a save keeps it.
      d_.net.set_port(static_cast<uint16_t>(want_port));
    } else if (!d_.net.listening() || d_.net.port() != want_port) {
      d_.net.stop();
      d_.net.start(static_cast<uint16_t>(want_port));  // becomes the configured port either way
    }

    send_json(res, json{{"enabled", on},
                        {"listening", d_.net.listening()},
                        {"port", d_.net.port()},
                        {"delay_ms", d_.ctl.net.delay_ms.load()},
                        {"delay_frames", d_.ctl.net.delay_frames.load()},
                        {"inputs", d_.net.count()},
                        {"inputs_configured", d_.ctl.net.inputs.load()},
                        {"inputs_max", opt_.net_inputs_max},
                        {"error", !inputs_error.empty() ? inputs_error : d_.net.last_error()}});
  }));

  // Takes a channel nobody is sending on off the console. Its ring column keeps whatever audio
  // was in it; it is only no longer shown, and no longer kept for the machine that used it.
  svr.Delete("/api/net/channels/:n", [this](const httplib::Request& req,
                                           httplib::Response& res) {
    unsigned c;
    if (!parse_index(req, "n", d_.net.count(), &c)) return send_error(res, 404, "no such channel");
    if (!d_.net.forget_channel(c)) return send_error(res, 409, "a sender is on this channel");
    send_ok(res);
  });

  svr.Post("/api/capture/freeze", [this](const httplib::Request&, httplib::Response& res) {
    const CaptureStatus cs = d_.capture.freeze(d_.engine.stats().generation);
    if (!cs.frozen) return send_error(res, 503, "not enough captured audio to freeze yet");
    send_json(res, capture_json(cs));
  });

  svr.Post("/api/capture/resume", [this](const httplib::Request&, httplib::Response& res) {
    d_.capture.resume();
    send_json(res, json{{"frozen", false}});
  });

  // Sets how many recent frames the next Analyze/freeze grabs. Body: {"seconds": N} (preferred)
  // or {"frames": N}. The value is clamped to [kCaptureMinFrames, max_frames]; the reply echoes
  // the clamped result so the client can show what actually took effect.
  svr.Post("/api/capture/config", json_route([this](const json& j, const httplib::Request&,
                                                    httplib::Response& res) {
    const double rate = d_.engine.rate();
    uint64_t frames;
    if (j.contains("seconds")) {
      const double s = j.at("seconds").get<double>();
      if (!(s > 0.0)) return send_error(res, 400, "seconds must be > 0");
      frames = static_cast<uint64_t>(std::llround(s * rate));
    } else {
      frames = j.at("frames").get<uint64_t>();
    }
    // The snapshot buffer is resized to match, so an over-large request can fail on memory
    // rather than silently pinning more than the board has. Refuse it visibly.
    std::string err;
    if (!d_.capture.set_analyze_frames(frames, &err)) return send_error(res, 507, err);
    const uint64_t now = d_.capture.analyze_frames();
    const uint64_t max = d_.capture.max_frames();
    send_json(res, json{{"analyze_frames", now},
                        {"analyze_seconds", now / rate},
                        {"max_frames", max},
                        {"max_seconds", max / rate},
                        {"pinned_mb", d_.capture.pinned_bytes() / (1024 * 1024)}});
  }));

  svr.Get("/api/capture/status", [this](const httplib::Request&, httplib::Response& res) {
    const uint64_t now = d_.ring.counter();
    json j = capture_json(d_.capture.status());
    j["live_now"] = now;
    j["live_oldest"] = d_.ring.oldest(now);
    // How much a freeze copies, and how far back the live ring reaches: what an Analyze can hold.
    const double rate = d_.engine.rate();
    const uint64_t analyze = d_.capture.analyze_frames();
    j["analyze_frames"] = analyze;
    j["analyze_seconds"] = rate > 0 ? analyze / rate : 0.0;
    j["ring_seconds"] = rate > 0 ? d_.ring.span() / rate : 0.0;
    send_json(res, j);
  });

  svr.Get("/api/capture/window", [this](const httplib::Request& req, httplib::Response& res) {
    // One channel per call: a list would otherwise silently read only its first entry.
    if (req.get_param_value_count("ch") > 1 ||
        req.get_param_value("ch").find(',') != std::string::npos)
      return send_error(res, 400, "capture/window reads one channel: pass a single ch");
    uint64_t ch = channels().total();
    if (!query_uint(req, "ch", &ch) || ch >= channels().total())
      return send_error(res, 400, channel_range_msg());
    const uint64_t now = d_.ring.counter();
    uint64_t len = 96000, cols = 1024;
    if (!query_uint(req, "len", &len)) return send_error(res, 400, "len must be a number of frames");
    if (!query_uint(req, "cols", &cols)) return send_error(res, 400, "cols must be a number");
    uint64_t start = now > len ? now - len : 0;
    if (!query_uint(req, "start", &start))
      return send_error(res, 400, "start must be a sample index (a number >= 0)");
    cols = std::min<uint64_t>(cols, UINT32_MAX);  // still over the maximum, so still refused

    const WindowResult w =
        d_.capture.window(static_cast<unsigned>(ch), start, len, static_cast<unsigned>(cols));
    if (!w.ok) return send_error(res, 400, w.error);

    json j{{"ch", ch}, {"start", w.start}, {"len", w.len}, {"cols", w.cols}, {"raw", w.raw}};
    if (w.raw) {
      j["samples"] = w.samples;
    } else {
      j["min"] = w.mins;
      j["max"] = w.maxs;
    }
    send_json(res, j);
  });

  // Raw multichannel audio in one call: an interleaved WAV of the selected inputs (the genie
  // selector, in its order) over start+len (the frozen snapshot while frozen, else the live ring)
  // or the live ring's last `seconds`. Built in memory from post-validated chunks before a byte is
  // sent, so a span the writer laps is a 410, not a file with stitched audio, and no lock is held
  // across network I/O. See docs/api.md, "GET /api/capture/wav".
  svr.Get("/api/capture/wav", [this, input_info](const httplib::Request& req,
                                                 httplib::Response& res) {
    InputSelector sel;
    if (!parse_selector(req, &sel, res)) return;
    WavFormat fmt = WavFormat::F32;
    if (req.has_param("format") && !parse_wav_format(req.get_param_value("format"), &fmt))
      return send_error(res, 400, "format must be f32, s32 or s16");
    ExportQuery q;
    for (const char* key : {"start", "len"}) {
      if (!req.has_param(key)) continue;
      uint64_t v = 0;
      if (!query_uint(req, key, &v))
        return send_error(res, 400, std::string(key) + " must be a number >= 0 (samples)");
      (std::string(key) == "start" ? q.start : q.len) = v;
    }
    if (req.has_param("seconds")) {
      double sec = 0;
      if (!query_number(req, "seconds", &sec)) return send_error(res, 400, "seconds must be a number > 0");
      q.seconds = sec;
    }

    const InputInfo info = input_info();
    const InputSelection pick = resolve_inputs(sel, info.layout, info.net);
    if (!pick.ok()) return send_selection_error(res, pick);
    if (pick.inputs.empty()) return send_error(res, 400, "the selector matches no inputs on this board");

    // The span is resolved once the build is ours, so a wait for another export cannot age it.
    std::unique_lock<std::timed_mutex> build(wav_build_m_, std::defer_lock);
    if (!build.try_lock_for(std::chrono::seconds(kWavBuildWaitS)))
      return send_error(res, 429, "another capture/wav is still being built: retry in a moment");
    const unsigned rate = d_.engine.stats().rate;
    const uint64_t frame_bytes = pick.inputs.size() * wav_sample_bytes(fmt);
    CaptureStatus cs;
    // Resolved against the ring as it is now: once to size the body, and again after it is
    // allocated (zero-filling 256 MB takes a Pi 3 a while), so the reads start right after.
    auto resolve_now = [&] {
      cs = d_.capture.status();
      const uint64_t head = d_.ring.counter();
      ExportBuffers bufs;
      bufs.frozen = cs.frozen;
      bufs.frozen_begin = cs.valid_start;
      bufs.frozen_end = cs.valid_start + cs.valid_len;
      bufs.live_begin = d_.ring.oldest(head);
      bufs.live_end = head;
      // What the writer overwrites within the margin: nothing until the ring has filled.
      const uint64_t margin = static_cast<uint64_t>(kWavLiveMarginS * rate);
      bufs.live_margin = d_.ring.oldest(head + margin) - bufs.live_begin;
      return resolve_export_span(q, bufs, rate, frame_bytes, kWavMaxBytes);
    };
    ExportSpan sp = resolve_now();
    if (sp.status != 200) return send_error(res, sp.status, sp.error);

    // The body is resident until the client has it: refuse one the box cannot hold now, as
    // capture/config does, rather than reach for the OOM killer.
    const unsigned total = d_.ring.channels();
    const uint64_t data_bytes = sp.len * frame_bytes;
    const uint64_t need = data_bytes + kWavChunkFrames * total * sizeof(float);
    const uint64_t avail = meminfo_bytes("MemAvailable:");
    if (avail != 0 && need + 64ull * 1024 * 1024 > avail) {
      return send_error(res, 507,
                        "not enough free memory for a " + std::to_string(data_bytes / (1024 * 1024)) +
                            " MB body (" + std::to_string(avail / (1024 * 1024)) +
                            " MB available): ask for a shorter span or fewer channels");
    }

    std::string chans_csv, labels;
    for (size_t k = 0; k < pick.inputs.size(); ++k) {
      chans_csv += (k ? "," : "") + std::to_string(pick.inputs[k]);
      labels += (k ? "; " : "") + info.label(pick.inputs[k]);
    }
    const char* source = nullptr;

    // ---- Read and encode, chunk by chunk ------------------------------------------------------
    auto data = std::make_shared<std::string>();
    std::vector<float> buf;
    try {
      buf.resize(kWavChunkFrames * total);
      data->resize(static_cast<size_t>(data_bytes));
    } catch (const std::bad_alloc&) {
      return send_error(res, 507, "not enough memory for the body");
    }
    sp = resolve_now();  // same len (it never depends on the head), a fresh start for `seconds`
    if (sp.status != 200) return send_error(res, sp.status, sp.error);
    source = sp.from_snapshot ? "frozen" : "live";
    for (uint64_t off = 0; off < sp.len;) {
      const size_t n = static_cast<size_t>(std::min<uint64_t>(kWavChunkFrames, sp.len - off));
      const bool ok = sp.from_snapshot
                          ? d_.capture.snapshot_frames(cs.freeze_sample, sp.start + off, n, buf.data())
                          : d_.ring.read_interleaved(sp.start + off, n, buf.data());
      if (!ok) {
        const std::string why =
            sp.from_snapshot
                ? "the frozen snapshot was resumed or replaced while it was read"
                : "the live ring overwrote sample " + std::to_string(sp.start + off) +
                      " while it was read (the span was too near its oldest sample)";
        return send_error(res, 410, why + ": ask again");
      }
      wav_encode(buf.data(), n, total, pick.inputs, fmt, data->data() + off * frame_bytes);
      off += n;
    }
    const uint32_t gen = sp.from_snapshot ? cs.generation : d_.engine.stats().generation;
    const int64_t unix_ms = unix_ms_at(sp.start, d_.ring.counter(), rate);
    // The same facts as the X-ST-* headers, inside the file, so a saved capture keeps them.
    auto hdr = std::make_shared<const std::string>(wav_header(
        rate, static_cast<unsigned>(pick.inputs.size()), fmt, sp.len,
        "soundtester capture: start_sample=" + std::to_string(sp.start) +
            " len=" + std::to_string(sp.len) + " rate=" + std::to_string(rate) +
            " unix_ms=" + std::to_string(unix_ms) + " generation=" + std::to_string(gen) +
            " source=" + source + " format=" + wav_format_name(fmt) + " ch=" + chans_csv +
            " labels=" + labels));

    res.set_header("X-ST-Start-Sample", std::to_string(sp.start));
    res.set_header("X-ST-Len", std::to_string(sp.len));
    res.set_header("X-ST-Rate", std::to_string(rate));
    res.set_header("X-ST-Generation", std::to_string(gen));
    res.set_header("X-ST-Unix-Ms", std::to_string(unix_ms));
    res.set_header("X-ST-Channels", chans_csv);
    res.set_header("X-ST-Source", source);
    res.set_header("X-ST-Format", wav_format_name(fmt));
    res.set_header("Content-Disposition",
                   "attachment; filename=\"capture-" + std::to_string(sp.start) + ".wav\"");
    // Header then samples, 1 MB per write; both buffers live until httplib is done with them.
    res.set_content_provider(
        hdr->size() + data->size(), "audio/wav",
        [hdr, data](size_t offset, size_t length, httplib::DataSink& sink) {
          const std::string& part = offset < hdr->size() ? *hdr : *data;
          const size_t at = offset < hdr->size() ? offset : offset - hdr->size();
          return sink.write(part.data() + at, std::min<size_t>({length, part.size() - at, 1u << 20}));
        });
  });

  svr.Post("/api/capture/xcorr", json_route([this](const json& j, const httplib::Request&,
                                                   httplib::Response& res) {
    const unsigned a = j.at("ch_a").get<unsigned>();
    const unsigned b = j.at("ch_b").get<unsigned>();
    const uint64_t start = j.at("start").get<uint64_t>();
    const uint64_t len = j.at("len").get<uint64_t>();
    const XcorrResult r = d_.capture.xcorr(a, b, start, len);
    if (!r.ok) return send_error(res, 400, r.error);

    const double c = 343.0;  // speed of sound, m/s
    send_json(res, json{{"lag_samples", r.lag_samples},
                        {"lag_ms", r.lag_ms},
                        {"lag_m", r.lag_ms * c / 1000.0},
                        {"confidence", r.confidence},
                        {"peak", r.peak}});
  }));

  svr.Get("/api/pings/recent", [this](const httplib::Request&, httplib::Response& res) {
    json arr = json::array();
    for (const auto& p : d_.ctl.ping_log.recent()) {
      arr.push_back({{"sample", p.sample}, {"variant", ping_name(static_cast<PingVariant>(p.variant))}});
    }
    send_json(res, arr);
  });

  // ---- "Genie" helpers -------------------------------------------------------------------
  // Convenience endpoints that fold the raw telemetry/measurement primitives above into one
  // direct answer, for headless/scripted use. They add no capability the other endpoints lack:
  // sound is a thin wrapper over the analysis snapshot, sync over the freeze+xcorr path. See
  // docs/api.md, "Genie helpers".

  // "Is there sound on a channel?" Inputs by the shared selector (ch list, or device/stream of a
  // NET sender, or kind); every input when none is given. Per channel:
  //   sound      peak_db (a 3 s hold, so it catches a tick/ping between polls) > threshold_db
  //   sound_now  the latest 100 ms window's own peak > threshold_db: no hold, no tail
  //   playing    sound_now && rms_db > min_rms_db: "is the DUT playing", which an idle analog
  //              input's noise floor and a stopped input's hold both fail
  // plus who the input is (label/name/kind/net) and the tone detector. expect_hz (one per returned
  // channel, or one for all) and tol_hz add tone.match. See docs/api.md, "Genie helpers".
  svr.Get("/api/genie/sound", [this, input_info](const httplib::Request& req,
                                                 httplib::Response& res) {
    InputSelector sel;
    if (!parse_selector(req, &sel, res)) return;
    double threshold = kGenieSoundThresholdDb, min_rms = kGeniePlayingRmsDb, tol = kGenieToneTolHz;
    if (!query_number(req, "threshold_db", &threshold) || !query_number(req, "min_rms_db", &min_rms))
      return send_error(res, 400, "threshold_db and min_rms_db are numbers in dB");
    if (!query_number(req, "tol_hz", &tol) || tol < 0)
      return send_error(res, 400, "tol_hz must be a number >= 0");
    if (req.has_param("freq_hz"))
      return send_error(res, 400, "genie/sound takes the expected frequency as expect_hz");
    std::vector<double> expect;
    const bool want_match = req.has_param("expect_hz");
    if (want_match && (!parse_f64_list(req.get_param_value("expect_hz"), &expect) ||
                       std::any_of(expect.begin(), expect.end(), [](double f) { return f <= 0; })))
      return send_error(res, 400, "expect_hz takes positive frequencies in Hz, comma-separated");

    const InputInfo info = input_info();
    const InputSelection pick = resolve_inputs(sel, info.layout, info.net);
    if (!pick.ok()) return send_selection_error(res, pick);
    if (pick.inputs.empty()) return send_error(res, 400, "the selector matches no inputs on this board");
    if (want_match && expect.size() != 1 && expect.size() != pick.inputs.size()) {
      return send_error(res, 400,
                        "expect_hz has " + std::to_string(expect.size()) + " values for " +
                            std::to_string(pick.inputs.size()) +
                            " channels: give one for all or one per channel, in order");
    }

    const AnalysisSnapshot s = d_.analysis.snapshot();
    const double rate = d_.engine.rate();
    const uint64_t now = d_.ring.counter();
    json chans = json::array();
    for (size_t k = 0; k < pick.inputs.size(); ++k) {
      const unsigned c = pick.inputs[k];
      const ChannelMeters& m = s.meters[c];
      const bool sound_now = m.peak_now_db > threshold;
      json tone = tone_json(s.tone[c]);
      if (want_match) {
        const double f = expect.size() == 1 ? expect[0] : expect[k];
        tone["expect_hz"] = f;
        tone["match"] = s.tone[c].valid && std::fabs(s.tone[c].freq_hz - f) <= tol;
      }
      json j{{"ch", c}};
      j.update(info.genie_json(c));
      j.update({{"sound", m.peak_db > threshold},
                {"sound_now", sound_now},
                {"playing", sound_now && m.rms_db > min_rms},
                {"last_sound_ms_ago",
                 m.loud_seen && rate > 0
                     ? json(std::llround(1000.0 * static_cast<double>(s.sample - m.last_loud) / rate))
                     : json(nullptr)},
                {"rms_db", m.rms_db},
                {"peak_db", m.peak_db},
                {"peak_now_db", m.peak_now_db},
                {"threshold_db", threshold},
                {"tone", tone}});
      chans.push_back(std::move(j));
    }
    json out{{"sample", s.sample},
             {"unix_ms", unix_ms_at(s.sample, now, rate)},
             {"threshold_db", threshold},
             {"min_rms_db", min_rms},
             {"resolved", resolved_json(sel, pick)},
             {"channels", chans}};
    if (want_match) out["tol_hz"] = tol;
    send_json(res, out);
  });

  // "Wait until something happens on an input" instead of a sleep-and-poll loop: blocks until
  // `until` has held hold_ms on any selected input (all=1: every one), or timeout_s passes. Looks
  // every analysis refresh (100 ms) at the analysis snapshot and the NET status, re-resolving a
  // device/stream selector each time so it follows a sender that moves. A timeout is a 200 with
  // met:false and a reason. At most kGenieMaxWaits at once (429 beyond): each parks a worker.
  // The condition itself is WaitTracker's (genie_wait.h); see docs/api.md, "Genie helpers".
  svr.Get("/api/genie/wait", [this, input_info](const httplib::Request& req,
                                                httplib::Response& res) {
    // ---- Parameters -------------------------------------------------------------------------
    WaitCondition cond;
    if (!req.has_param("until") || !parse_wait_until(req.get_param_value("until"), &cond.until))
      return send_error(res, 400, "until must be sound, silence, tone, live or change");
    InputSelector sel;
    if (!parse_selector(req, &sel, res)) return;
    if (!query_number(req, "threshold_db", &cond.threshold_db) ||
        !query_number(req, "min_rms_db", &cond.min_rms_db))
      return send_error(res, 400, "threshold_db and min_rms_db are numbers in dB");
    // expect_hz is the name genie/sound and genie/watch use: the same thing here (one value).
    if (req.has_param("freq_hz") && req.has_param("expect_hz"))
      return send_error(res, 400, "freq_hz and expect_hz are one parameter here: pass one");
    const char* freq_key = req.has_param("expect_hz") ? "expect_hz" : "freq_hz";
    if ((req.has_param(freq_key) || req.has_param("tol_hz")) && cond.until != WaitUntil::Tone)
      return send_error(res, 400, std::string(freq_key) + " and tol_hz are for until=tone");
    if (req.has_param(freq_key)) {
      double f = 0;
      if (!query_number(req, freq_key, &f) || f <= 0)
        return send_error(res, 400, std::string(freq_key) + " must be one frequency in Hz > 0");
      cond.freq_hz = f;
    }
    if (!query_number(req, "tol_hz", &cond.tol_hz) || cond.tol_hz < 0)
      return send_error(res, 400, "tol_hz must be a number >= 0");
    const uint64_t max_ms = static_cast<uint64_t>(kGenieWaitMaxTimeoutS * 1000);
    if (!query_uint(req, "hold_ms", &cond.hold_ms) || cond.hold_ms > max_ms)
      return send_error(res, 400, "hold_ms must be 0.." + std::to_string(max_ms));
    double timeout_s = kGenieWaitTimeoutS;
    if (!query_number(req, "timeout_s", &timeout_s) || timeout_s < 0 ||
        timeout_s > kGenieWaitMaxTimeoutS)
      return send_error(res, 400, "timeout_s must be 0.." +
                                      std::to_string(static_cast<int>(kGenieWaitMaxTimeoutS)));
    for (const char* key : {"all", "any"}) {
      if (!req.has_param(key)) continue;
      const std::string v = req.get_param_value(key);
      if (v != "0" && v != "1" && v != "true" && v != "false")
        return send_error(res, 400, std::string(key) + " must be 0 or 1");
    }
    auto flag = [&req](const char* key) {
      const std::string v = req.get_param_value(key);
      return v == "1" || v == "true";
    };
    const bool all_given = req.has_param("all"), any_given = req.has_param("any");
    if (all_given && any_given && flag("all") == flag("any"))
      return send_error(res, 400, "all and any contradict each other: pass one");
    cond.all = all_given ? flag("all") : any_given && !flag("any");

    // Only a network input has a sender to be live: with no selector, the network inputs.
    if (cond.until == WaitUntil::Live) {
      if (sel.kind && *sel.kind != InputKind::Net)
        return send_error(res, 400, "until=live is about network senders: kind must be net");
      for (unsigned c : sel.ch) {
        if (!channels().is_net(c))
          return send_error(res, 400, "until=live is about network senders: input " +
                                          std::to_string(c) + " is not a network input");
      }
      if (sel.ch.empty() && !sel.by_sender()) sel.kind = InputKind::Net;
    }

    // A selector that cannot resolve on any tick is the caller's mistake: say so now. One that
    // names a sender not connected yet (404) is what a wait is for.
    {
      const InputInfo info = input_info();
      const InputSelection pick = resolve_inputs(sel, info.layout, info.net);
      if (pick.status == 400) return send_selection_error(res, pick);
      if (pick.ok() && pick.inputs.empty())
        return send_error(res, 400, "the selector matches no inputs on this board");
    }

    if (genie_waits_.fetch_add(1) >= kGenieMaxWaits) {
      genie_waits_.fetch_sub(1);
      return send_error(res, 429,
                        "already " + std::to_string(kGenieMaxWaits) +
                            " genie/wait requests pending: retry when one has answered");
    }
    struct Slot {
      std::atomic<unsigned>& n;
      ~Slot() { n.fetch_sub(1); }
    } slot{genie_waits_};

    // ---- The wait ---------------------------------------------------------------------------
    using namespace std::chrono;
    const auto t0 = steady_clock::now();
    const int64_t t0_unix_ms = unix_ms_now();
    const uint64_t timeout_ms = static_cast<uint64_t>(std::llround(timeout_s * 1000));
    WaitTracker tracker(cond);
    std::optional<InputInfo> last;  // the last tick's (InputInfo holds a reference: no assignment)
    InputSelection pick;
    AnalysisSnapshot s;
    std::optional<std::vector<unsigned>> first_inputs;
    // A sender that stops by closing its PCM (aplay ending, a killed player) is no longer
    // connected, so its selector stops resolving: for silence and change, keep evaluating the
    // inputs it was last on (its audio drains through the ring, then they read silent), unless
    // another sender has taken them.
    const bool keep_after_leave =
        sel.by_sender() && (cond.until == WaitUntil::Silence || cond.until == WaitUntil::Change);
    std::vector<unsigned> resolved_inputs, eval;
    bool sender_left = false;
    bool met = false;
    uint64_t elapsed = 0;
    for (;;) {
      last.emplace(input_info());
      const InputInfo& info = *last;
      pick = resolve_inputs(sel, info.layout, info.net);
      s = d_.analysis.snapshot();
      std::vector<WaitSample> now;
      eval.clear();
      sender_left = false;
      if (pick.ok()) {
        if (!first_inputs) first_inputs = pick.inputs;
        resolved_inputs = pick.inputs;
        eval = pick.inputs;
      } else if (keep_after_leave && pick.status == 404 && !resolved_inputs.empty()) {
        for (unsigned c : resolved_inputs) {
          const NetChannelStatus* n = info.net_status(c);
          if (!n || !n->connected) eval.push_back(c);
        }
        sender_left = !eval.empty();
      }
      {
        for (unsigned c : eval) {
          WaitSample w;
          w.ch = c;
          w.rms_db = s.meters[c].rms_db;
          w.peak_now_db = s.meters[c].peak_now_db;
          w.tone_valid = s.tone[c].valid;
          w.tone_hz = s.tone[c].freq_hz;
          if (const NetChannelStatus* n = info.net_status(c)) {
            w.streaming = n->state == "streaming";
            w.frames_received = n->frames_received;
          }
          now.push_back(w);
        }
      }
      elapsed = static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now() - t0).count());
      if (tracker.update(elapsed, now, s.sample)) {
        met = true;
        break;
      }
      if (elapsed >= timeout_ms) break;
      // A daemon stopping must not wait out anyone's timeout (the worker pool joins this
      // thread), and a client that hung up has nobody to answer.
      if (!running_.load()) return send_error(res, 503, "the daemon is shutting down");
      if (req.is_connection_closed()) return;
      std::this_thread::sleep_for(milliseconds(std::min<uint64_t>(kGenieWaitTickMs,
                                                                   timeout_ms - elapsed)));
    }

    // ---- The reply --------------------------------------------------------------------------
    const InputInfo& info = *last;
    const double rate = d_.engine.rate();
    json chans = json::array();
    std::vector<WaitNetInput> net_inputs;
    {
      for (unsigned c : eval) {
        const ChannelMeters& m = s.meters[c];
        const NetChannelStatus* n = info.net_status(c);
        json j{{"ch", c}};
        j.update(info.genie_json(c));
        if (n) {
          j["net"]["frames_received"] = n->frames_received;
          j["net"]["frames_per_s"] = n->frames_per_s ? json(*n->frames_per_s) : json(nullptr);
          j["net"]["last_audio_ms_ago"] =
              n->last_audio_ms_ago ? json(*n->last_audio_ms_ago) : json(nullptr);
          net_inputs.push_back({info.label(c), n->device.empty() ? n->last_device : n->device,
                                n->state, n->last_audio_ms_ago});
        }
        WaitSample w;
        w.rms_db = m.rms_db;
        w.peak_now_db = m.peak_now_db;
        const int flip = tracker.changed(c);
        j.update({{"device", n ? json(n->device) : json(nullptr)},
                  {"holds", tracker.holds(c)},
                  {"playing", wait_playing(cond, w)},
                  {"sound_now", m.peak_now_db > cond.threshold_db},
                  {"rms_db", m.rms_db},
                  {"peak_now_db", m.peak_now_db},
                  {"tone", tone_json(s.tone[c])},
                  {"changed", flip > 0 ? json("on") : flip < 0 ? json("off") : json(nullptr)}});
        chans.push_back(std::move(j));
      }
    }
    // Where the condition started holding: the first reading of the run that met it. The ring's
    // audio was heard capture_delay before it reached the ring, so onset_ms (from the request) takes
    // that off; `live` reads the link, not the ring, so its onset is the look's own time.
    const double capture_delay_ms = rate > 0 ? 1000.0 * d_.ctl.capture_delay.load() / rate : 0.0;
    json met_sample = nullptr, met_unix_ms = nullptr, onset_ms = nullptr;
    if (met && tracker.met_onset()) {
      const WaitTracker::Onset& o = *tracker.met_onset();
      const int64_t at = unix_ms_at(o.sample, d_.ring.counter(), rate);
      met_sample = o.sample;
      met_unix_ms = at;
      onset_ms = cond.until == WaitUntil::Live
                     ? static_cast<int64_t>(o.t_ms)
                     : at - std::llround(capture_delay_ms) - t0_unix_ms;
    }
    json out{{"met", met},
             {"until", wait_until_name(cond.until)},
             {"after_ms", elapsed},
             {"met_sample", met_sample},
             {"met_unix_ms", met_unix_ms},
             {"onset_ms", onset_ms},
             {"hold_ms", cond.hold_ms},
             {"timeout_s", timeout_s},
             {"all", cond.all},
             {"threshold_db", cond.threshold_db},
             {"min_rms_db", cond.min_rms_db},
             {"sample", s.sample},
             {"unix_ms", unix_ms_at(s.sample, d_.ring.counter(), rate)},
             {"capture_delay_ms", capture_delay_ms},
             {"resolved", pick.ok() ? resolved_json(sel, pick) : json(nullptr)},
             {"moved_from", first_inputs && pick.ok() && *first_inputs != pick.inputs
                                ? json(*first_inputs)
                                : json(nullptr)},
             {"channels", chans}};
    if (sender_left) out["sender_left"] = true;
    if (cond.until == WaitUntil::Tone) {
      out["freq_hz"] = cond.freq_hz ? json(*cond.freq_hz) : json(nullptr);
      out["tol_hz"] = cond.tol_hz;
    }
    if (!met) {
      std::string reason;
      if (!pick.ok() && !sender_left) {
        reason = pick.error;
        json seen = json::array();
        for (const LastSeen& l : pick.last_seen) {
          seen.push_back({{"input", l.input},
                          {"channel", l.channel},
                          {"last_device", l.last_device},
                          {"last_stream", l.last_stream},
                          {"state", l.state}});
        }
        out["last_seen"] = seen;
      } else {
        // The link explains a timeout only when the wait was about senders: a sender selector,
        // or nothing but network inputs (an analog DUT hunt has idle NET inputs too).
        reason = wait_net_reason(cond.until, net_inputs,
                                 sel.by_sender() || net_inputs.size() == eval.size());
      }
      if (reason.empty()) {
        char hold[32];
        std::snprintf(hold, sizeof hold, "%g", timeout_s);
        reason = std::string("timed out after ") + hold + " s: " + wait_until_name(cond.until) +
                 " did not hold for " + std::to_string(cond.hold_ms) + " ms on " +
                 (cond.all ? "every" : "any") + " selected input";
      }
      out["reason"] = reason;
    }
    send_json(res, out);
  });

  // "What happened on an input over a span": dropouts, clicks, level steps and, on a test tone,
  // phase slips and distortion, each at its sample, plus the span's level statistics. from=past
  // analyses the last `seconds` of the ring and answers at once; from=now waits for `seconds` to
  // be captured first (at most 60). The analysis is WatchAnalyzer's (genie_watch.h), fed straight
  // from the ring in post-validated chunks. At most kGenieMaxWatches at once (429 beyond). See
  // docs/api.md, "Genie helpers".
  svr.Get("/api/genie/watch", [this, input_info](const httplib::Request& req,
                                                 httplib::Response& res) {
    using namespace std::chrono;
    const auto t_req = steady_clock::now();
    // ---- Parameters -------------------------------------------------------------------------
    InputSelector sel;
    if (!parse_selector(req, &sel, res)) return;
    const std::string from = req.has_param("from") ? req.get_param_value("from") : "past";
    if (from != "past" && from != "now") return send_error(res, 400, "from must be past or now");
    const bool now_mode = from == "now";
    double seconds = kGenieWatchSeconds, block_ms = kGenieWatchBlockMs;
    WatchParams wp;
    if (!query_number(req, "seconds", &seconds) || seconds <= 0)
      return send_error(res, 400, "seconds must be a number > 0");
    if (!query_number(req, "block_ms", &block_ms) || block_ms < 1 || block_ms > 100)
      return send_error(res, 400, "block_ms must be 1..100");
    if (!query_number(req, "silence_db", &wp.silence_db) || wp.silence_db < -150 || wp.silence_db > 0)
      return send_error(res, 400, "silence_db must be -150..0 (dBFS)");
    if (!query_number(req, "click_k", &wp.click_k) || wp.click_k < 2 || wp.click_k > 1000)
      return send_error(res, 400, "click_k must be 2..1000");
    if (!query_number(req, "resid_dbc", &wp.resid_dbc) || wp.resid_dbc < -200 || wp.resid_dbc > 0)
      return send_error(res, 400, "resid_dbc must be -200..0 (dB against the tone)");
    if (!query_number(req, "slip_deg", &wp.slip_deg) || wp.slip_deg <= 0 || wp.slip_deg > 180)
      return send_error(res, 400, "slip_deg must be > 0 and <= 180");
    if (!query_number(req, "level_step_db", &wp.level_step_db) || wp.level_step_db <= 0 ||
        wp.level_step_db > 100)
      return send_error(res, 400, "level_step_db must be > 0 and <= 100");
    if (req.has_param("freq_hz"))
      return send_error(res, 400, "genie/watch takes the expected frequency as expect_hz");
    const std::string expect = req.has_param("expect_hz") ? req.get_param_value("expect_hz") : "auto";
    std::vector<double> expect_hz;  // empty: auto or none
    const double rate = d_.engine.rate();
    wp.rate = rate;
    wp.block = std::max(16u, static_cast<unsigned>(std::llround(block_ms * rate / 1000.0)));
    // A tone needs a whole cycle in a block to be fitted, and must be below Nyquist.
    const double min_hz = rate / wp.block, max_hz = rate / 2 * 0.95;
    if (expect != "auto" && expect != "none") {
      if (!parse_f64_list(expect, &expect_hz) ||
          std::any_of(expect_hz.begin(), expect_hz.end(),
                      [&](double f) { return f < min_hz || f > max_hz; })) {
        char msg[200];
        std::snprintf(msg, sizeof msg,
                      "expect_hz is auto, none, or frequencies in Hz, comma-separated, each "
                      "%.0f..%.0f (one cycle per block_ms up to Nyquist)",
                      std::ceil(min_hz), std::floor(max_hz));
        return send_error(res, 400, msg);
      }
    }

    InputInfo info0 = input_info();
    const InputSelection pick = resolve_inputs(sel, info0.layout, info0.net);
    if (!pick.ok()) return send_selection_error(res, pick);
    if (pick.inputs.empty()) return send_error(res, 400, "the selector matches no inputs on this board");
    if (pick.inputs.size() > kGenieWatchMaxInputs)
      return send_error(res, 400, std::to_string(pick.inputs.size()) + " inputs selected: at most " +
                                      std::to_string(kGenieWatchMaxInputs) +
                                      " per watch (narrow it with ch=, device= or kind=)");
    if (!expect_hz.empty() && expect_hz.size() != 1 && expect_hz.size() != pick.inputs.size())
      return send_error(res, 400, "expect_hz has " + std::to_string(expect_hz.size()) + " values for " +
                                      std::to_string(pick.inputs.size()) +
                                      " channels: give one for all or one per channel, in order");

    // The span: never nearer than kGenieWatchMarginS to the ring's oldest sample, so it is still
    // there while it is read.
    const double ring_s = static_cast<double>(d_.ring.span()) / rate;
    const double max_s = std::floor((now_mode ? std::min(kGenieWatchNowMaxS, ring_s - kGenieWatchMarginS)
                                              : ring_s - kGenieWatchMarginS) * 10) / 10;
    if (seconds > max_s) {
      char msg[160];
      std::snprintf(msg, sizeof msg, "seconds must be at most %.1f for from=%s (the ring holds %.1f s)",
                    max_s, from.c_str(), ring_s);
      return send_error(res, 400, msg);
    }
    const uint64_t len = static_cast<uint64_t>(std::llround(seconds * rate));
    if (len < wp.block) return send_error(res, 400, "seconds is shorter than one block");

    if (genie_watches_.fetch_add(1) >= kGenieMaxWatches) {
      genie_watches_.fetch_sub(1);
      return send_error(res, 429,
                        "already " + std::to_string(kGenieMaxWatches) +
                            " genie/watch requests running: retry when one has answered");
    }
    struct Slot {
      std::atomic<unsigned>& n;
      ~Slot() { n.fetch_sub(1); }
    } slot{genie_watches_};

    // ---- The span ---------------------------------------------------------------------------
    uint64_t start = 0;
    std::vector<NetChannelStatus> net0;
    const EngineStats es0 = d_.engine.stats();
    const uint32_t gen0 = es0.generation;
    // from=past: whether a seam (an xrun, a capture-axis move) lies inside the span, from where
    // the last one was — read before the head, so it is at or before the span's end.
    bool seam_in_past = false;
    if (!now_mode) {
      const uint64_t head = d_.ring.counter();
      if (head < len) {
        char msg[120];
        std::snprintf(msg, sizeof msg, "the ring holds only %.1f s so far", head / rate);
        return send_error(res, 400, msg);
      }
      start = head - len;
      seam_in_past = es0.generation_at > start && es0.generation_at < head;
    } else {
      start = d_.ring.counter();
      net0 = info0.net;
      // A stalled engine (a card that cannot be reopened) never gets there: give up 5 s late.
      const auto deadline = steady_clock::now() + milliseconds(static_cast<int64_t>(seconds * 1000)) +
                            std::chrono::seconds(5);
      while (d_.ring.counter() < start + len) {
        if (!running_.load()) return send_error(res, 503, "the daemon is shutting down");
        if (req.is_connection_closed()) return;
        if (steady_clock::now() > deadline)
          return send_error(res, 503,
                            "the engine is not advancing: " +
                                std::to_string(d_.ring.counter() - start) + " of " +
                                std::to_string(len) +
                                " samples captured (see /api/state engine.last_error)");
        const uint64_t left = start + len - d_.ring.counter();
        std::this_thread::sleep_for(milliseconds(std::clamp<uint64_t>(
            static_cast<uint64_t>(1000.0 * static_cast<double>(left) / rate) + 1, 1, 100)));
      }
    }
    const uint32_t gen1 = d_.engine.stats().generation;
    const InputInfo info = input_info();  // who is on each input now, at the span's end

    // expect_hz per input: given, or auto from the tone detector now (the span's end).
    const AnalysisSnapshot snap = d_.analysis.snapshot();
    const size_t ni = pick.inputs.size();
    std::vector<WatchAnalyzer> an;
    std::vector<std::string> tone_note(ni), tone_src(ni);
    an.reserve(ni);
    for (size_t k = 0; k < ni; ++k) {
      const unsigned c = pick.inputs[k];
      WatchParams p = wp;
      if (!expect_hz.empty()) {
        p.tone_hz = expect_hz.size() == 1 ? expect_hz[0] : expect_hz[k];
        tone_src[k] = "expect_hz";
      } else if (expect == "auto") {
        const ToneMetrics& t = snap.tone[c];
        if (!t.valid) {
          const char* why = tone_reason_name(t.reason);
          tone_note[k] = std::string("auto: no valid tone at the span's end") +
                         (why ? std::string(" (") + why + ")" : "");
        } else if (t.freq_hz < min_hz || t.freq_hz > max_hz) {
          char msg[120];
          std::snprintf(msg, sizeof msg, "auto: the tone (%.1f Hz) is below one cycle per block: raise block_ms",
                        t.freq_hz);
          tone_note[k] = msg;
        } else {
          p.tone_hz = t.freq_hz;
          tone_src[k] = "analysis";
        }
      }
      an.emplace_back(p);
    }

    // ---- The analysis: every selected column out of each post-validated chunk ---------------
    const auto t_an = steady_clock::now();
    const unsigned total = d_.ring.channels();
    std::vector<float> buf(kGenieWatchChunkFrames * total), col(kGenieWatchChunkFrames);
    bool incomplete = false;
    std::string reason;
    uint64_t analysed = 0;
    for (uint64_t off = 0; off < len;) {
      const size_t n = static_cast<size_t>(std::min<uint64_t>(kGenieWatchChunkFrames, len - off));
      if (!d_.ring.read_interleaved(start + off, n, buf.data())) {
        incomplete = true;
        char msg[160];
        std::snprintf(msg, sizeof msg,
                      "ring overwritten at sample %llu: the span left the ring while it was read; "
                      "%.2f s of %.2f analysed",
                      static_cast<unsigned long long>(start + off), off / rate, len / rate);
        reason = msg;
        break;
      }
      for (size_t k = 0; k < ni; ++k) {
        const unsigned c = pick.inputs[k];
        for (size_t i = 0; i < n; ++i) col[i] = buf[i * total + c];
        an[k].feed(col.data(), n);
      }
      off += n;
      analysed = off;
    }
    std::vector<WatchChannelResult> results;
    results.reserve(ni);
    for (size_t k = 0; k < ni; ++k) {
      const unsigned c = pick.inputs[k];
      results.push_back(an[k].finish([this, start, c](uint64_t off, size_t n, float* out) {
        return d_.ring.read_channel(start + off, n, c, out);
      }));
    }
    const auto t_done = steady_clock::now();

    // ---- The reply --------------------------------------------------------------------------
    const uint64_t head = d_.ring.counter();
    // To `per` steps per unit (100: 0.01), as the nearest double prints: 440.001, not 440.00100000000003.
    auto rnd = [](double v, double per) { return std::round(v * per) / per; };
    auto opt = [&](const std::optional<double>& v, double q) {
      return v ? json(rnd(*v, q)) : json(nullptr);
    };
    std::vector<WatchChannelEvents> evs;
    json chans = json::array();
    for (size_t k = 0; k < ni; ++k) {
      const unsigned c = pick.inputs[k];
      const WatchChannelResult& r = results[k];
      const NetChannelStatus* n = info.net_status(c);
      // One sender's run of channels shares its holes: events merge across its members.
      const int group = n && !n->state.empty() && n->stream_count > 1
                            ? static_cast<int>(n->channel + 1 - n->stream_index)
                            : -1;
      evs.push_back({c, group, &r.events});
      json counts = json::object();
      for (size_t i = 0; i < kWatchKinds; ++i)
        counts[watch_kind_name(static_cast<WatchKind>(i))] = r.counts[i];
      json tone = nullptr;
      if (r.tone) {
        const WatchToneStats& t = *r.tone;
        tone = {{"expect_hz", rnd(t.expect_hz, 1000)},
                {"source", tone_src[k]},
                {"blocks", t.blocks},
                {"bad_blocks", t.bad_blocks},
                {"freq_hz_median", opt(t.freq_hz_median, 1000)},
                {"freq_hz_min", opt(t.freq_hz_min, 1000)},
                {"freq_hz_max", opt(t.freq_hz_max, 1000)},
                {"thd_n_pct_median", opt(t.thd_n_pct_median, 10000)},
                {"thd_n_pct_max", opt(t.thd_n_pct_max, 10000)},
                {"resid_dbc_median", opt(t.resid_dbc_median, 100)},
                {"resid_dbc_max", opt(t.resid_dbc_max, 100)}};
      }
      json link = nullptr;
      if (now_mode && n && info.layout.is_net(c)) {
        const unsigned nc = c - info.layout.net_base();
        if (nc < net0.size()) {
          const NetChannelStatus& a = net0[nc];
          auto delta = [](uint64_t x0, uint64_t x1) { return x1 >= x0 ? x1 - x0 : x1; };
          link = {{"underruns", delta(a.underruns, n->underruns)},
                  {"underrun_events", delta(a.underrun_events, n->underrun_events)},
                  {"late_drops", delta(a.late_drops, n->late_drops)},
                  {"range_drops", delta(a.range_drops, n->range_drops)},
                  {"resyncs", delta(a.resyncs, n->resyncs)},
                  {"reconnects", delta(a.connects, n->connects)},
                  // A sender that only left is `state`, not someone else.
                  {"device_changed", !n->device.empty() && a.device != n->device},
                  {"state", n->state},
                  {"frames_per_s", n->frames_per_s ? json(rnd(*n->frames_per_s, 10)) : json(nullptr)}};
        }
      }
      json j{{"ch", c}};
      j.update(info.genie_json(c));
      j.update({{"sound_pct", rnd(r.sound_pct, 100)},
                {"silent_s", rnd(r.silent_s, 1000)},
                {"rms_db",
                 {{"min", rnd(r.rms_db_min, 100)},
                  {"p10", rnd(r.rms_db_p10, 100)},
                  {"median", rnd(r.rms_db_median, 100)},
                  {"max", rnd(r.rms_db_max, 100)}}},
                {"peak_max_db", rnd(r.peak_max_db, 100)},
                {"clipped_samples", r.clipped_samples},
                {"counts", counts},
                {"tone", tone},
                {"tone_note", tone_note[k].empty() ? json(nullptr) : json(tone_note[k])},
                {"link", link}});
      chans.push_back(std::move(j));
    }
    const auto merged = merge_watch_events(
        evs, static_cast<uint64_t>(std::llround(kGenieWatchMergeMs * rate / 1000.0)));
    json events = json::array();
    for (size_t i = 0; i < merged.size() && i < kGenieWatchMaxEvents; ++i) {
      const WatchEvent& e = merged[i].ev;
      const uint64_t sample = start + e.offset;
      json labels = json::array();
      for (unsigned c : merged[i].ch) labels.push_back(info.label(c));
      json j{{"kind", watch_kind_name(e.kind)},
             {"ch", merged[i].ch},
             {"labels", labels},
             {"sample", sample},
             {"t_s", rnd(static_cast<double>(e.offset) / rate, 10000)},
             {"t_ago_s", rnd(static_cast<double>(head - sample) / rate, 1000)},
             {"len_ms", rnd(1000.0 * static_cast<double>(e.len) / rate, 1000)}};
      if (e.slip_samples) {
        j["slip_samples"] = rnd(*e.slip_samples, 100);
        j["slip_deg"] = rnd(*e.slip_deg, 10);
      }
      if (e.from_db) {
        j["from_db"] = rnd(*e.from_db, 100);
        j["to_db"] = rnd(*e.to_db, 100);
      }
      if (e.level_db) j["level_db"] = rnd(*e.level_db, 100);
      if (e.resid_dbc) j["resid_dbc"] = rnd(*e.resid_dbc, 100);
      if (e.kind == WatchKind::Gap) j["zeros"] = e.zeros;
      if (e.click) j["click"] = true;
      events.push_back(std::move(j));
    }
    bool truncated = merged.size() > kGenieWatchMaxEvents;
    for (const WatchChannelResult& r : results) truncated |= r.events_truncated;

    json out{{"from", from},
             {"seconds", rnd(static_cast<double>(len) / rate, 10000)},
             {"start_sample", start},
             {"len", len},
             {"analysed", analysed},
             {"rate", rate},
             {"block_ms", rnd(1000.0 * wp.block / rate, 1000)},
             {"unix_ms_start", unix_ms_at(start, head, rate)},
             {"t_ago_s_start", rnd(static_cast<double>(head - start) / rate, 1000)},
             {"capture_delay_ms", rate > 0 ? 1000.0 * d_.ctl.capture_delay.load() / rate : 0.0},
             {"generation", gen1},
             {"generation_changed", now_mode ? gen1 != gen0 : seam_in_past},
             {"incomplete", incomplete},
             {"reason", incomplete ? json(reason) : json(nullptr)},
             {"elapsed_ms", duration_cast<milliseconds>(t_done - t_req).count()},
             {"analysis_ms", duration_cast<milliseconds>(t_done - t_an).count()},
             {"params",
              {{"silence_db", wp.silence_db},
               {"click_k", wp.click_k},
               {"expect_hz", expect_hz.empty() ? json(expect) : json(expect_hz)},
               {"resid_dbc", wp.resid_dbc},
               {"slip_deg", wp.slip_deg},
               {"level_step_db", wp.level_step_db}}},
             {"resolved", resolved_json(sel, pick)},
             {"channels", chans},
             {"events", events},
             {"events_truncated", truncated}};
    send_json(res, out);
  });

  // "Measure delay/sync between two inputs." A GET, so it runs straight from a browser address
  // bar. Every query param is optional:
  //   ch_a, ch_b       the pair by ring column; a=, b= the same by column or by sender
  //                    ("<device>[/<stream>]", its first channel); device=/stream= alone = the L
  //                    and R of that sender's run. Default: the first two inputs playing now.
  //   cur_x, cur_y     two sample indices bracketing ONE window to measure
  //   mode             pings (every ping marker), sliding (windows over the whole snapshot, for
  //                    music or any DUT-played content), auto (pings when >= 3 arrive, else sliding)
  //   win, step, min_confidence   the sliding windows; since / since_ms_ago  only audio after it
  // Freeze handling follows intent: if the capture is already frozen (you sent POST
  // /api/capture/freeze) it measures on that snapshot and leaves it frozen; otherwise it freezes,
  // measures and unfreezes. A positive lag means the signal arrives later on ch_b. A measurement
  // that finds nothing is a 200 with ok:false and a reason, never an empty summary to guess at.
  svr.Get("/api/genie/sync", [this, input_info](const httplib::Request& req,
                                                httplib::Response& res) {
    // The snapshot was resumed or replaced (the console's Analyze/Resume, POST
    // /api/capture/resume) while this call read it: its readings would mix two snapshots or none.
    auto send_snapshot_lost = [](httplib::Response& r) {
      send_error(r, 410, std::string(CaptureStore::kSnapshotLost) +
                             " during the measurement (Analyze/Resume, or capture/freeze/resume): "
                             "ask again");
    };
    // ---- Parameters -------------------------------------------------------------------------
    const std::string mode = req.has_param("mode") ? req.get_param_value("mode") : "auto";
    if (mode != "auto" && mode != "pings" && mode != "sliding")
      return send_error(res, 400, "mode must be auto, pings or sliding");
    const double rate = d_.engine.rate();
    uint64_t win = sliding_default_window(rate), step = 0, since = 0;
    if (!query_uint(req, "win", &win) || win < kSyncMinWindow || win > kXcorrMaxLen)
      return send_error(res, 400, "win must be " + std::to_string(kSyncMinWindow) + ".." +
                                      std::to_string(kXcorrMaxLen) + " samples");
    step = win;
    if (!query_uint(req, "step", &step) || step < 1)
      return send_error(res, 400, "step must be a number of samples >= 1");
    double min_conf = kSyncMinConfidence, since_ms_ago = -1.0;
    if (!query_number(req, "min_confidence", &min_conf) || min_conf < 0)
      return send_error(res, 400, "min_confidence must be a number >= 0");
    if (req.has_param("since") && req.has_param("since_ms_ago"))
      return send_error(res, 400, "since and since_ms_ago are two ways to say one thing: pass one");
    if (!query_uint(req, "since", &since))
      return send_error(res, 400, "since must be a sample index");
    if (!query_number(req, "since_ms_ago", &since_ms_ago) ||
        (req.has_param("since_ms_ago") && since_ms_ago < 0))
      return send_error(res, 400, "since_ms_ago must be a number of ms >= 0");

    // ---- The pair -----------------------------------------------------------------------------
    const InputInfo info = input_info();
    InputSelector run_sel;
    if (!parse_selector(req, &run_sel, res)) return;
    if (!run_sel.ch.empty() || run_sel.stream_index || run_sel.kind)
      return send_error(res, 400,
                        "genie/sync takes its pair as ch_a/ch_b, a/b or device/stream "
                        "(not ch, stream_index or kind)");
    const unsigned total = channels().total();
    int a = -1, b = -1;
    json resolved = nullptr;
    if (run_sel.by_sender()) {
      // device=/stream=: the left and right of that sender's run.
      for (const char* k : {"ch_a", "ch_b", "a", "b"})
        if (req.has_param(k))
          return send_error(res, 400, std::string("device/stream measures the L/R of one sender: ") +
                                          "pass it without " + k);
      const InputSelection s = resolve_inputs(run_sel, info.layout, info.net);
      if (!s.ok()) return send_selection_error(res, s);
      const SelectedRun& run = s.runs.front();
      if (run.inputs.size() < 2) {
        return send_error(res, 400, "the sender on " + info.label(run.inputs.front()) +
                                        " has one channel: pass a= and b= to pair it with another");
      }
      a = static_cast<int>(run.inputs[0]);
      b = static_cast<int>(run.inputs[1]);
      resolved = resolved_json(run_sel, s);
    } else {
      // Each side by ch_x (a column) or x (a column or a sender); both is a contradiction.
      const std::string range_msg = sync_range_msg();
      auto side = [&](const char* ch_key, const char* key, int* out) -> bool {
        if (req.has_param(ch_key) && req.has_param(key)) {
          send_error(res, 400, std::string(ch_key) + " and " + key + " name the same input: pass one");
          return false;
        }
        if (req.has_param(ch_key)) {
          uint64_t v = 0;
          if (!query_uint(req, ch_key, &v) || v >= total) {
            send_error(res, 400, range_msg);
            return false;
          }
          *out = static_cast<int>(v);
          return true;
        }
        if (!req.has_param(key)) return true;
        InputSelector sel;
        std::string err;
        if (!parse_sync_side(req.get_param_value(key), total, &sel, &err)) {
          send_error(res, 400, std::string(key) + ": " + err);
          return false;
        }
        if (!sel.ch.empty()) {
          *out = static_cast<int>(sel.ch.front());
          return true;
        }
        InputSelection s = resolve_inputs(sel, info.layout, info.net);
        if (!s.ok()) {
          s.error = std::string(key) + ": " + s.error;
          send_selection_error(res, s);
          return false;
        }
        *out = static_cast<int>(s.inputs.front());
        return true;
      };
      if (!side("ch_a", "a", &a) || !side("ch_b", "b", &b)) return;

      // Default candidates: the inputs playing now first (genie/sound's `playing`), then the rest
      // whose held peak reads sound — a ping every few seconds can fall between 100 ms windows,
      // and an idle analog input's noise floor must not beat a real signal to ch_a.
      const AnalysisSnapshot as = d_.analysis.snapshot();
      std::vector<unsigned> sounding;
      for (unsigned ch = 0; ch < total; ++ch) {
        const ChannelMeters& m = as.meters[ch];
        if (m.peak_now_db > kGenieSoundThresholdDb && m.rms_db > kGeniePlayingRmsDb)
          sounding.push_back(ch);
      }
      for (unsigned ch = 0; ch < total; ++ch) {
        if (as.meters[ch].peak_db > kGenieSoundThresholdDb &&
            std::find(sounding.begin(), sounding.end(), ch) == sounding.end())
          sounding.push_back(ch);
      }
      auto fill = [&](int* out, int other) {
        for (const unsigned ch : sounding) {
          if (*out >= 0) return;
          if (static_cast<int>(ch) != other) *out = static_cast<int>(ch);
        }
      };
      fill(&a, b);
      fill(&b, a);
      if (a < 0 || b < 0) return send_error(res, 400, "need two inputs with sound, or pass ch_a and ch_b");
    }

    // Who each side is: enough to report it by name without another call.
    auto side_json = [&](unsigned c) {
      const NetChannelStatus* n = info.net_status(c);
      return json{{"ch", c},
                  {"label", info.label(c)},
                  {"name", info.name(c)},
                  {"kind", info.kind(c)},
                  {"device", n ? json(n->device) : json(nullptr)},
                  {"stream", n ? json(n->stream) : json(nullptr)},
                  {"stream_index", n ? json(n->stream_index) : json(nullptr)}};
    };

    // One sync at a time: a second call that joined the first's freeze would lose it when the
    // first resumes. The console's Analyze/Resume can still replace the snapshot meanwhile;
    // every read below is pinned to `cs`, and a measurement that lost it is a 410.
    std::unique_lock<std::timed_mutex> serial(sync_m_, std::defer_lock);
    if (!serial.try_lock_for(std::chrono::seconds(kSyncWaitS)))
      return send_error(res, 429, "another genie/sync is still measuring: retry in a moment");
    // Keep an existing freeze; otherwise freeze for this call only.
    const CaptureStatus pre = d_.capture.status();
    const bool was_frozen = pre.frozen;
    const CaptureStatus cs = was_frozen ? pre : d_.capture.freeze(d_.engine.stats().generation);
    if (!cs.frozen) return send_error(res, 503, "not enough captured audio to freeze yet");

    const double c = 343.0;  // speed of sound, m/s
    json out{{"ch_a", a}, {"ch_b", b}, {"a", side_json(a)}, {"b", side_json(b)},
             {"frozen", was_frozen}};
    if (!resolved.is_null()) out["resolved"] = resolved;

    // Two cursors -> one window between them.
    if (req.has_param("cur_x") && req.has_param("cur_y")) {
      const uint64_t x = query_u64(req, "cur_x", 0), y = query_u64(req, "cur_y", 0);
      const uint64_t start = std::min(x, y), len = std::max(x, y) - std::min(x, y);
      const XcorrResult r = d_.capture.xcorr(a, b, start, len, cs.freeze_sample);
      if (r.error == CaptureStore::kSnapshotLost) return send_snapshot_lost(res);
      if (!was_frozen) d_.capture.resume();
      // A silent channel is a measurement that found nothing; a bad window is the caller's error.
      if (!r.ok && r.error != "one of the channels is silent") return send_error(res, 400, r.error);
      out.update({{"ok", r.ok}, {"reason", r.ok ? json(nullptr) : json(r.error)},
                  {"start", start}, {"len", len}});
      if (!r.ok) return send_json(res, out);
      out.update({{"lag_samples", r.lag_samples},
                  {"lag_ms", r.lag_ms},
                  {"lag_m", r.lag_ms * c / 1000.0},
                  {"confidence", r.confidence},
                  {"peak", r.peak}});
      return send_json(res, out);
    }

    const uint64_t snap_end = cs.valid_start + cs.valid_len;
    const bool has_since = req.has_param("since") || req.has_param("since_ms_ago");
    if (since_ms_ago >= 0) {
      const uint64_t back = static_cast<uint64_t>(std::llround(since_ms_ago * rate / 1000.0));
      since = snap_end > back ? snap_end - back : 0;
    }
    const uint64_t begin = std::max(cs.valid_start, since);
    const std::string after = has_since ? " after since" : "";

    struct Run {
      json meas = json::array();
      std::vector<SyncMeasurement> good;
      std::string reason;  // set when good is empty
      size_t skipped = 0;  // measurements entries carrying `skipped`
    };
    size_t n_markers = 0, n_no_arrival = 0, n_windows = 0, n_quiet = 0;
    const std::string pair_name = "ch " + std::to_string(a) + "/" + std::to_string(b);

    // Whether anything plays the ping now: an engine output, or a channel of an enabled sink's
    // layout, whose source is the ping generator. Its markers are logged either way, so without
    // this a sync over music would blame "no_arrival" pings nobody was sent.
    bool ping_routed = false;
    for (unsigned o = 0; o < channels().outputs; ++o)
      ping_routed |= routes_gen(d_.ctl.outputs[o], GenId::Ping);
    for (unsigned k = 0; k < kMaxSinks; ++k) {
      const SinkControl& sc = d_.ctl.sinks[k];
      if (!sc.enabled.load(std::memory_order_relaxed)) continue;
      const unsigned speakers = sink_layout_info(sink_layout(sc)).speakers;
      for (unsigned c = 0; c < speakers; ++c) ping_routed |= routes_gen(sc.outputs[c], GenId::Ping);
    }

    // Every ping marker in the frozen buffer. Bracket each ping the way the console's Scope
    // auto-measure does (app.js labelablePings): the window runs from this emission up to just
    // before the next, so it holds exactly one ping *arrival* wherever the loopback delay lands
    // it. A fixed window centred on the emission would miss the arrival once loopback exceeds it —
    // the cause of the large bogus lags a narrow window reports.
    auto run_pings = [&] {
      Run p;
      const auto pings = d_.ctl.ping_log.recent();  // chronological (oldest -> newest)
      const uint64_t interval =
          static_cast<uint64_t>(std::llround(d_.ctl.ping.interval_s.load() * rate));
      for (size_t i = 0; i < pings.size(); ++i) {
        const uint64_t sample = pings[i].sample;
        json e{{"center", sample}, {"variant", ping_name(static_cast<PingVariant>(pings[i].variant))}};
        auto skip = [&](const std::string& why) {
          e["skipped"] = why;
          ++p.skipped;
          p.meas.push_back(e);
        };
        if (sample < cs.valid_start || sample >= snap_end) {
          skip("outside buffer");
          continue;
        }
        if (sample < begin) {
          skip("before since");
          continue;
        }
        ++n_markers;
        const uint64_t gap = (i + 1 < pings.size()) ? pings[i + 1].sample - sample : interval;
        const uint64_t natural = std::min<uint64_t>(
            std::max<uint64_t>(1024, gap > kPingWinGuard ? gap - kPingWinGuard : 1024), kXcorrMaxLen);
        const uint64_t room = snap_end - sample;
        if (room < std::min<uint64_t>(natural, kPingMinWindow)) {
          skip("too near buffer end");
          continue;
        }
        const uint64_t len = std::min(natural, room);
        const XcorrResult r = d_.capture.xcorr(a, b, sample, len, cs.freeze_sample);
        if (!r.ok) {
          skip(r.error);
          continue;
        }
        e.update({{"start", sample},
                  {"len", len},
                  {"lag_samples", r.lag_samples},
                  {"lag_ms", r.lag_ms},
                  {"lag_m", r.lag_ms * c / 1000.0},
                  {"confidence", r.confidence},
                  {"peak", r.peak}});
        // Below the peak floor the correlation is noise, not a captured arrival on this pair —
        // flag it and keep it out of the summary (the console shows these as a dash).
        if (r.peak < kPingMinPeak) {
          e["no_arrival"] = true;
          ++n_no_arrival;
        } else {
          p.good.push_back({r.lag_samples, r.confidence});
        }
        p.meas.push_back(e);
      }
      if (n_markers == 0) {
        p.reason = "no ping markers in the snapshot" + after + " (ping generator off?)";
      } else if (p.good.empty() && n_no_arrival == n_markers) {
        p.reason = "all " + std::to_string(n_no_arrival) + " pings no_arrival on " + pair_name +
                   (ping_routed ? ": check routing" : ": the ping is routed to no output or sink");
      } else if (p.good.empty()) {
        p.reason = "no ping arrived on " + pair_name + " (" + std::to_string(n_no_arrival) +
                   " no_arrival, " + std::to_string(n_markers - n_no_arrival) +
                   " skipped near the buffer end or refused)";
      }
      return p;
    };

    // Windows over the whole snapshot (from since): any content both channels carry, not only
    // the tester's pings. A window with either channel quiet is skipped unmeasured; a measured one
    // counts when its correlation is unambiguous (confidence) and real (peak).
    auto run_sliding = [&] {
      Run s;
      const std::vector<uint64_t> starts =
          sliding_window_starts(begin, snap_end, win, step, kSyncMaxWindows);
      n_windows = starts.size();
      size_t quiet_a = 0, quiet_b = 0;
      for (const uint64_t start : starts) {
        double ra = -120.0, rb = -120.0;
        // False only when the snapshot went away (the windows lie inside it): never "quiet".
        if (!d_.capture.rms_db(a, start, win, &ra, cs.freeze_sample) ||
            !d_.capture.rms_db(b, start, win, &rb, cs.freeze_sample))
          break;
        if (ra < kSyncQuietDb) ++quiet_a;
        if (rb < kSyncQuietDb) ++quiet_b;
        if (ra < kSyncQuietDb || rb < kSyncQuietDb) {
          ++n_quiet;
          continue;
        }
        const XcorrResult r = d_.capture.xcorr(a, b, start, win, cs.freeze_sample);
        if (!r.ok) {
          ++s.skipped;
          s.meas.push_back({{"start", start}, {"len", win}, {"skipped", r.error}});
          continue;
        }
        const bool kept = r.confidence >= min_conf && r.peak >= kPingMinPeak;
        s.meas.push_back({{"start", start},
                          {"len", win},
                          {"lag_samples", r.lag_samples},
                          {"lag_ms", r.lag_ms},
                          {"confidence", r.confidence},
                          {"peak", r.peak},
                          {"kept", kept}});
        if (kept) s.good.push_back({r.lag_samples, r.confidence});
      }
      const size_t n = starts.size();
      if (n == 0) {
        s.reason = "snapshot" + after + " shorter than one window (" +
                   std::to_string(snap_end > begin ? snap_end - begin : 0) + " < " +
                   std::to_string(win) + " samples)";
      } else if (n_quiet == n && quiet_a == n && quiet_b == n) {
        s.reason = "both channels silent (RMS below " + std::to_string(int(kSyncQuietDb)) + " dB)";
      } else if (n_quiet == n && (quiet_a == n || quiet_b == n)) {
        s.reason = std::string(quiet_a == n ? "ch_a" : "ch_b") + " (" +
                   info.label(quiet_a == n ? a : b) + ") silent (RMS below " +
                   std::to_string(int(kSyncQuietDb)) + " dB)";
      } else if (n_quiet == n) {
        s.reason = "the two channels are never loud in the same window";
      } else if (s.good.empty()) {
        s.reason = "no window correlated with confidence >= " + json(min_conf).dump() +
                   " (unrelated signals, or a steady tone)";
      }
      return s;
    };

    Run r;
    std::string mode_used = mode;
    if (mode == "pings") {
      r = run_pings();
    } else if (mode == "sliding") {
      r = run_sliding();
    } else {
      r = run_pings();
      mode_used = "pings";
      // Ping markers are logged whether or not the ping is routed anywhere, and a 1 s window of
      // music still "arrives" (peak) without a ping in it: only unambiguous readings count as the
      // pair carrying the pings. Music correlates at confidence ~1.1 over a ping window; a ping
      // reads hundreds; noise reads ~5 and is measured right either way.
      const size_t clean = static_cast<size_t>(std::count_if(
          r.good.begin(), r.good.end(), [&](const SyncMeasurement& m) { return m.confidence >= min_conf; }));
      if (clean < kSyncAutoMinPings) {
        Run s = run_sliding();
        if (!s.good.empty() || clean == 0) {
          // What the pings found, only when something plays them: a sync over music with the
          // ping routed nowhere has its markers logged all the same, and "all N pings no_arrival"
          // would send the caller to check a routing nobody asked for.
          if (s.good.empty() && ping_routed) {
            if (r.reason.empty())  // pings arrived, but none unambiguous
              r.reason = std::to_string(r.good.size()) + " arrived on " + pair_name +
                         ", none with confidence >= " + json(min_conf).dump();
            s.reason += "; pings: " + r.reason;
          }
          r = std::move(s);
          mode_used = "sliding";
        } else {
          // Sliding found nothing, 1-2 clean pings did: they are the answer, and only they.
          r.good.erase(std::remove_if(r.good.begin(), r.good.end(),
                                      [&](const SyncMeasurement& m) { return m.confidence < min_conf; }),
                       r.good.end());
        }
      }
    }
    // Pinned reads fail only once the snapshot is no longer `cs`, and it never comes back: so
    // this one check says whether every reading above came from it.
    const CaptureStatus post = d_.capture.status();
    if (!post.frozen || post.freeze_sample != cs.freeze_sample) return send_snapshot_lost(res);
    if (!was_frozen) d_.capture.resume();

    const bool ok = !r.good.empty();
    const SyncSummary sum = summarize_sync(r.good);
    json summary{{"n", sum.n}};
    bool trusted = false;
    if (ok) {
      const double ms = 1000.0 / rate;
      summary.update({{"lag_samples_median", sum.lag_samples_median},
                      {"lag_ms_median", sum.lag_samples_median * ms},
                      {"lag_m_median", sum.lag_samples_median * ms * c / 1000.0},
                      {"lag_samples_min", sum.lag_samples_min},
                      {"lag_samples_max", sum.lag_samples_max},
                      {"lag_samples_spread", sum.lag_samples_spread},
                      {"lag_samples_mad", sum.lag_samples_mad},
                      {"lag_ms_mad", sum.lag_samples_mad * ms},
                      {"lag_ms_stdev", sum.lag_samples_stdev * ms},
                      {"lag_ms_p10", sum.lag_samples_p10 * ms},
                      {"lag_ms_p90", sum.lag_samples_p90 * ms},
                      {"confidence_median", sum.confidence_median},
                      {"bimodal", sum.bimodal}});
      trusted = mode_used == "pings" ? sync_trusted_pings(sum) : sync_trusted_sliding(sum, rate, win);
    }
    out.update({{"ok", ok},
                {"reason", ok ? json(nullptr) : json(r.reason)},
                {"mode", mode},
                {"mode_used", mode_used},
                {"trusted", trusted},
                {"n_markers", n_markers},
                {"n_skipped", r.skipped},
                {"n_no_arrival", n_no_arrival},
                {"n_windows", n_windows},
                {"n_quiet", n_quiet},
                {"snapshot",
                 {{"freeze_sample", cs.freeze_sample},
                  {"valid_start", cs.valid_start},
                  {"valid_len", cs.valid_len},
                  {"generation", cs.generation}}},
                {"measurements", r.meas},
                {"summary", summary}});
    if (has_since) out["since"] = since;
    if (mode_used == "sliding")
      out.update({{"win", win}, {"step", step}, {"min_confidence", min_conf}});
    send_json(res, out);
  });

  svr.Post("/api/config/save", [this](const httplib::Request&, httplib::Response& res) {
    Config base;
    {
      std::lock_guard<std::mutex> lk(config_m_);
      base = d_.config;
    }
    Config cfg = Config::from_control(d_.ctl, base);
    cfg.sinks = d_.devices.sink_configs();
    std::string err;
    if (!d_.store.save(cfg, &err)) return send_error(res, 500, err);
    {
      std::lock_guard<std::mutex> lk(config_m_);
      d_.config = cfg;
    }
    send_json(res, json{{"ok", true}, {"path", d_.store.saved_path()}});
  });

  svr.Post("/api/config/reset", [this](const httplib::Request&, httplib::Response& res) {
    std::string err;
    if (!d_.store.reset(&err)) return send_error(res, 500, err);
    send_json(res, json{{"ok", true}});
  });

  // The rootfs is read-only, so pulling the plug is *usually* survivable — but /data is briefly
  // writable during a save, and a hard power cut inside that window can corrupt the SD card,
  // which is why a shutdown route exists at all. Disabled in a simulated run: those buttons
  // must not systemctl a developer's workstation.
  auto power_route = [this, &svr](const char* path, const char* what, const char* cmd) {
    svr.Post(path, [this, what, cmd](const httplib::Request&, httplib::Response& res) {
      if (!opt_.allow_reboot) return send_error(res, 403, std::string(what) + " disabled");
      send_ok(res);
      std::thread([what, cmd] {
        // Answer first, then go down: the browser needs the response before the socket dies.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        sync();
        if (std::system(cmd) != 0) LOG_ERROR("{} failed", what);
      }).detach();
    });
  };
  power_route("/api/system/reboot", "reboot", "systemctl reboot");
  power_route("/api/system/shutdown", "shutdown", "systemctl poweroff");
  // Only the daemon: systemd starts it again (Restart=always), with what the board was booted with
  // and the saved config — which is how a new network input count takes effect.
  power_route("/api/system/restart", "restart", "systemctl --no-block restart soundtesterd");

  // Test hook for the sync-error banner; harmless in production.
  svr.Post("/api/system/inject-kmsg", [this](const httplib::Request& req, httplib::Response& res) {
    d_.kmsg.process_line(req.body.empty() ? "bcm2835-i2s: I2S SYNC error!" : req.body);
    send_json(res, json{{"ok", true}, {"sync_errors", d_.kmsg.sync_errors()}});
  });

  svr.WebSocket("/api/ws", [this](const httplib::Request&, httplib::ws::WebSocket& ws) {
    auto client = hub_.add();
    WsReadPump pump(ws);   // notice a closed tab instead of streaming telemetry to it forever
    LOG_INFO("telemetry client connected ({} total)", hub_.clients());
    while (running_.load() && ws.is_open()) {
      WsMessagePtr m = hub_.wait(client, 250);
      if (!m) continue;
      const bool ok = m->binary ? ws.send(m->data.data(), m->data.size()) : ws.send(m->data);
      if (!ok) break;
    }
    hub_.remove(client);
    LOG_INFO("telemetry client gone ({} left)", hub_.clients());
  });

  svr.WebSocket("/api/listen/:ch", [this](const httplib::Request& req, httplib::ws::WebSocket& ws) {
    unsigned ch = 0;
    if (!parse_index(req, "ch", channels().total(), &ch)) {
      ws.close(httplib::ws::CloseStatus::PolicyViolation, "bad channel");
      return;
    }
    StreamSlot slot(listen_streams_);
    if (!slot.acquired()) {
      ws.close(httplib::ws::CloseStatus::InternalError, "too many listeners");
      return;
    }

    // Notice a clean browser close promptly (see WsReadPump) — otherwise the stream keeps
    // pushing audio and holding this slot until a write eventually fails. Declared after `slot`
    // so its reader is joined before the slot is released.
    WsReadPump pump(ws);

    // Codec is negotiated per connection: ?codec=opus, with an optional ?bitrate=kbps override.
    // The wire default stays PCM, so any client that does not ask keeps the raw S16 format byte
    // for byte — an un-updated console never receives Opus bytes into PCM-parsing code.
    const bool want_opus = req.has_param("codec") && req.get_param_value("codec") == "opus";
    const unsigned rate = d_.engine.stats().rate;

    if (want_opus && opus_rate_supported(rate)) {
      const BitrateOverride br = query_bitrate(req);
      bool ok = false;
      OpusMonoEncoder enc(rate, effective_kbps(br, d_.ctl), &ok);
      if (!ok) {
        ws.close(httplib::ws::CloseStatus::InternalError, "opus init failed");
        return;
      }
      LOG_DEBUG("listen ws opened on ch{} (opus)", ch);
      ListenPacer pacer(d_.ring, ch, enc.in_frames());
      const float* pcm = nullptr;
      uint64_t start = 0;
      bool skipped = false;
      std::vector<uint8_t> packet;
      std::string frame;
      while (running_.load() && ws.is_open()) {
        if (!pacer.next_float(running_, &pcm, &start, &skipped)) break;
        if (skipped) enc.reset();  // clean restart across a skip-to-live discontinuity
        if (!enc.encode(pcm, effective_kbps(br, d_.ctl), &packet)) break;
        // u64 starting sample index (native ring rate), then one raw Opus packet.
        frame.resize(8 + packet.size());
        std::memcpy(frame.data(), &start, 8);
        std::memcpy(frame.data() + 8, packet.data(), packet.size());
        if (!ws.send(frame.data(), frame.size())) break;
      }
      LOG_DEBUG("listen ws closed on ch{} (opus)", ch);
      return;
    }

    LOG_DEBUG("listen ws opened on ch{}", ch);
    ListenPacer pacer(d_.ring, ch);
    std::vector<int16_t> pcm;
    uint64_t start = 0;
    std::string frame;

    while (running_.load() && ws.is_open()) {
      if (!pacer.next(running_, &pcm, &start)) break;
      // u64 starting sample index, then S16_LE mono; the index lets the client notice gaps.
      frame.resize(8 + pcm.size() * 2);
      std::memcpy(frame.data(), &start, 8);
      std::memcpy(frame.data() + 8, pcm.data(), pcm.size() * 2);
      if (!ws.send(frame.data(), frame.size())) break;
    }
    LOG_DEBUG("listen ws closed on ch{}", ch);
  });

  svr.Get("/api/inputs/:ch/stream.wav", [this](const httplib::Request& req, httplib::Response& res) {
    unsigned ch = 0;
    if (!parse_index(req, "ch", channels().total(), &ch)) return send_error(res, 404, "no such input");

    auto slot = std::make_shared<StreamSlot>(listen_streams_);
    if (!slot->acquired()) return send_error(res, 503, "too many listeners");

    const unsigned rate = d_.engine.stats().rate;
    auto pacer = std::make_shared<ListenPacer>(d_.ring, ch);
    auto sent_header = std::make_shared<bool>(false);
    auto pcm = std::make_shared<std::vector<int16_t>>();

    LOG_DEBUG("wav stream opened on ch{}", ch);
    res.set_chunked_content_provider(
        "audio/wav",
        [this, ch, rate, pacer, sent_header, pcm, slot](size_t, httplib::DataSink& sink) {
          if (!running_.load()) {
            sink.done();
            return false;
          }
          if (!*sent_header) {
            const std::string h = endless_wav_header(rate);
            sink.write(h.data(), h.size());
            *sent_header = true;
            return true;
          }
          uint64_t start = 0;
          if (!pacer->next(running_, pcm.get(), &start)) {
            sink.done();
            return false;
          }
          return sink.write(reinterpret_cast<const char*>(pcm->data()), pcm->size() * 2);
        },
        [ch](bool) { LOG_DEBUG("wav stream closed on ch{}", ch); });
  });

  // One multichannel Ogg/Opus stream carrying every input, read from a single ring cursor so all
  // channels share one granulepos clock — the only shape that keeps channels sample-aligned for a
  // generic external player (extract with ffmpeg; see docs/api.md). Family 255, uncoupled.
  svr.Get("/api/stream.ogg", [this](const httplib::Request& req, httplib::Response& res) {
    const unsigned rate = d_.engine.stats().rate;
    if (!opus_rate_supported(rate)) {
      return send_error(res, 501, "opus is not available at this sample rate");
    }

    auto slot = std::make_shared<StreamSlot>(listen_streams_);
    if (!slot->acquired()) return send_error(res, 503, "too many listeners");
    auto ogg_slot =
        std::make_shared<StreamSlot>(ogg_streams_, kMaxOggStreams, "multichannel");
    if (!ogg_slot->acquired()) return send_error(res, 503, "too many multichannel streams");

    const BitrateOverride br = query_bitrate(req);

    bool ok = false;
    auto enc = std::make_shared<OpusOggMultiEncoder>(rate, effective_kbps(br, d_.ctl),
                                                     ogg_serial_.fetch_add(1), &ok);
    if (!ok) return send_error(res, 500, "opus init failed");
    auto pacer = std::make_shared<MultiListenPacer>(d_.ring, enc->in_frames());
    auto sent_header = std::make_shared<bool>(false);

    LOG_DEBUG("ogg stream opened ({} ch)", channels().total());
    res.set_chunked_content_provider(
        "audio/ogg",
        [this, enc, pacer, sent_header, slot, ogg_slot, br](size_t, httplib::DataSink& sink) {
          if (!running_.load()) {
            sink.done();
            return false;
          }
          if (!*sent_header) {
            const std::string h = enc->headers();
            *sent_header = true;
            return sink.write(h.data(), h.size());
          }
          const float* pcm = nullptr;
          uint64_t start = 0;
          bool skipped = false;
          if (!pacer->next_float(running_, &pcm, &start, &skipped)) {
            sink.done();
            return false;
          }
          if (skipped) enc->reset();
          std::string pages;
          if (!enc->encode(pcm, effective_kbps(br, d_.ctl), &pages)) {
            sink.done();
            return false;
          }
          if (pages.empty()) return true;  // Ogg is still filling a page; next_float paces us
          return sink.write(pages.data(), pages.size());
        },
        [](bool) { LOG_DEBUG("ogg stream closed"); });
  });
}

void WebServer::run_publisher() {
  using clock = std::chrono::steady_clock;
  using std::chrono::milliseconds;

  // Each stream has an absolute deadline that advances by exactly one period when it fires, and
  // the loop sleeps until the nearest one. Advancing by the period rather than re-anchoring to
  // the wake time (next = now + period) keeps each rate exact: re-anchoring would fold every
  // overshoot into the following interval, so every stream would run slow.
  struct Task {
    clock::time_point next;
    milliseconds period;
  };
  const auto t0 = clock::now();
  Task meters{t0, milliseconds(100)};    // 10 Hz
  Task spectrum{t0, milliseconds(200)};  //  5 Hz
  Task system{t0, milliseconds(1000)};   //  1 Hz

  // Envelope frames go out in lockstep with the thing that produces them. The analysis thread
  // appends envelope columns once per tick (kTickHz = 10 Hz, ~20 columns of env_column_frames), so
  // a publisher polling faster finds an empty ring on some ticks and emits nothing. Raising the
  // producer instead would mean running the 6-channel 8192-point FFT half again as often on a
  // Pi 3 to gain nothing — the scope's fidelity is set by the column rate (a constant
  // 200 columns/s), not by how many frames those columns are packed into.
  Task envelope{t0, milliseconds(100)};  // 10 Hz — the envelope producer's cadence

  CpuSampler cpu;
  uint64_t env_cursor = 0;

  while (running_.load()) {
    std::this_thread::sleep_until(
        std::min({meters.next, spectrum.next, envelope.next, system.next}));
    if (!running_.load()) break;

    const auto now = clock::now();
    const bool have_clients = hub_.clients() > 0;

    // A deadline that has fallen more than one period behind (a long stall) is snapped
    // forward rather than firing a burst of catch-up frames.
    auto fire = [&](Task& t) {
      if (now < t.next) return false;
      t.next += t.period;
      if (t.next < now) t.next = now + t.period;
      return true;
    };

    // Every deadline advances even with nobody listening — otherwise they would all sit in
    // the past and sleep_until() would return instantly, spinning the thread. Whether the
    // fired work is *published* is gated on have_clients below.
    const bool fire_system = fire(system);
    const bool fire_meters = fire(meters);
    const bool fire_spectrum = fire(spectrum);
    const bool fire_envelope = fire(envelope);

    // Sampled whether or not a WS client is connected: GET /api/state serves these values to
    // HTTP-only clients, and the CPU delta needs a steady 1 Hz cadence to mean anything.
    if (fire_system) {
      SysInfo si = sample_sysinfo();
      const CpuSampler::Load load = cpu.sample();
      si.cpu_pct = load.total;
      si.cpu_cores = load.cores;
      {
        std::lock_guard<std::mutex> lk(sys_m_);
        sys_ = si;
      }
      if (have_clients) {
        const EngineStats es = d_.engine.stats();
        json net_active = json::array();
        json net_connected = json::array();
        json net_links = json::array();
        std::vector<std::string> in_names;
        {
          std::lock_guard<std::mutex> lk(config_m_);
          in_names = d_.config.input_names;
        }
        const unsigned net_base = channels().net_base();
        for (const NetChannelStatus& c : d_.net.status()) {
          net_active.push_back(d_.net.channel_in_use(c.channel));
          net_connected.push_back(c.connected);
          net_links.push_back(net_link_json(
              c, net_base + c.channel < in_names.size() ? in_names[net_base + c.channel]
                                                        : std::string{}));
        }
        json j{{"type", "system"},
               {"net_active", net_active},
               {"net_connected", net_connected},
               {"net", net_links},
               {"xruns", es.xruns},
               {"generation", es.generation},
               {"sync_errors", d_.kmsg.sync_errors()},
               {"listen_streams", listen_streams_.load()},
               {"engine_running", es.running}};
        json sinks = json::array();
        for (const Devices::Sink& k : d_.devices.sinks())
          sinks.push_back(sink_brief_json(k, d_.devices.output(k.slot).status()));
        j["sinks"] = sinks;
        json srcs = json::array();
        for (const Devices::Source& s : d_.devices.sources())
          srcs.push_back({{"id", s.id}, {"first", s.first}, {"channels", s.channels},
                          {"present", s.present}, {"capturing", s.status.capturing},
                          {"error", s.status.error}});
        j["sources"] = srcs;
        j["bt_request"] = bt_request_json(d_.bt.status());
        j.update(sysinfo_json(si));
        hub_.publish(std::make_shared<WsMessage>(WsMessage{j.dump(), false}));
      }
    }

    if (!have_clients) {
      // Nothing is watching the scope, so do not accumulate a backlog of envelope columns to
      // dump at whoever connects next.
      env_cursor = d_.analysis.envelope().head();
      continue;
    }

    if (fire_meters) {
      const AnalysisSnapshot s = d_.analysis.snapshot();
      hub_.publish(std::make_shared<WsMessage>(WsMessage{meters_json(s).dump(), false}));
    }

    if (fire_spectrum) {
      const AnalysisSnapshot s = d_.analysis.snapshot();
      const uint32_t mask = telemetry_mask_.load(std::memory_order_relaxed);
      json chans = json::array();
      for (unsigned c = 0; c < channels().total(); ++c) {
        if (!(mask & (1u << c))) continue;   // console is not watching this input — skip its bins
        // Quantised to 0.1 dB, which is far finer than the display can resolve and far coarser
        // than a float's shortest round-trip decimal. Raw floats serialise every bin in full
        // ("-88.61194610595703"): 27.9 kB per spectrum message, against ~7 kB for this.
        std::vector<double> bins;
        bins.reserve(s.spectrum[c].size());
        for (const float v : s.spectrum[c]) bins.push_back(std::round(v * 10.0) / 10.0);
        chans.push_back({{"ch", c}, {"bins", bins}, {"tone", tone_json(s.tone[c])}});
      }
      const json j{{"type", "spectrum"}, {"channels", chans}};
      hub_.publish(std::make_shared<WsMessage>(WsMessage{j.dump(), false}));
    }

    if (fire_envelope) {
      uint64_t first = 0;
      const auto cols = d_.analysis.envelope().since(env_cursor, &first);
      if (!cols.empty()) {
        env_cursor = first + cols.size();
        hub_.publish(std::make_shared<WsMessage>(
            WsMessage{envelope_frame(first, d_.analysis.env_column_frames(), cols), true}));
      }
    }
  }
}

bool WebServer::start() {
  running_.store(true);

  // Prime the host-health snapshot so the very first GET /api/state — which can arrive before
  // the publisher's first 1 Hz tick — carries a hostname, addresses and memory rather than an
  // empty object. CPU load stays 0 until there are two readings to difference.
  {
    std::lock_guard<std::mutex> lk(sys_m_);
    sys_ = sample_sysinfo();
  }

  install_routes();

  svr_->new_task_queue = [] { return new httplib::ThreadPool(kThreadsMin, kThreadsMax); };
  // Long-lived streams must not be reaped by an idle timeout.
  svr_->set_read_timeout(3600, 0);
  svr_->set_write_timeout(3600, 0);

  publisher_ = std::thread([this] { run_publisher(); });

  LOG_INFO("serving http://{}:{} from {}", opt_.host, opt_.port, opt_.www_dir);
  const bool ok = svr_->listen(opt_.host.c_str(), opt_.port);
  if (!ok) LOG_ERROR("cannot bind {}:{}", opt_.host, opt_.port);

  running_.store(false);
  hub_.shutdown();
  if (publisher_.joinable()) publisher_.join();
  return ok;
}

void WebServer::stop() {
  running_.store(false);
  hub_.shutdown();
  if (svr_) svr_->stop();
  if (publisher_.joinable()) publisher_.join();
}

}  // namespace st
