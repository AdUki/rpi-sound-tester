#include "webserver.h"

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <vector>

#include "constants.h"
#include "listen_stream.h"
#include "util/dsp.h"
#include "util/log.h"
#include "util/sysinfo.h"
#include "util/wav.h"

using json = nlohmann::json;

namespace st {

namespace {

constexpr int kThreadsMin = 8;
constexpr int kThreadsMax = 64;

void send_json(httplib::Response& res, const json& j, int status = 200) {
  res.status = status;
  res.set_content(j.dump(), "application/json");
}

void send_error(httplib::Response& res, int status, const std::string& msg) {
  send_json(res, json{{"error", msg}}, status);
}

bool parse_index(const httplib::Request& req, const char* name, unsigned limit, unsigned* out) {
  auto it = req.path_params.find(name);
  if (it == req.path_params.end()) return false;
  const long v = std::strtol(it->second.c_str(), nullptr, 10);
  if (v < 0 || v >= static_cast<long>(limit)) return false;
  *out = static_cast<unsigned>(v);
  return true;
}

uint64_t query_u64(const httplib::Request& req, const char* key, uint64_t fallback) {
  if (!req.has_param(key)) return fallback;
  return std::strtoull(req.get_param_value(key).c_str(), nullptr, 10);
}

json meters_json(const AnalysisSnapshot& s) {
  json rms = json::array(), peak = json::array(), dc = json::array();
  for (unsigned c = 0; c < kInputs; ++c) {
    rms.push_back(s.meters[c].rms_db);
    peak.push_back(s.meters[c].peak_db);
    dc.push_back(s.meters[c].dc);
  }
  return json{{"type", "meters"}, {"n", s.sample}, {"rms_db", rms}, {"peak_db", peak}, {"dc", dc}};
}

json tone_json(const ToneMetrics& t) {
  return json{{"valid", t.valid},
              {"freq_hz", t.freq_hz},
              {"level_db", t.level_db},
              {"thd_n_pct", t.thd_n_pct},
              {"thd_n_db", t.thd_n_db}};
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

// Binary wave frame: u8 type=1, u64 first column's sample index, u16 column count, then
// ncols x 6 x {i16 min, i16 max}.
std::string wave_frame(uint64_t first_col, const std::vector<EnvColumn>& cols) {
  std::string out;
  out.resize(11 + cols.size() * kInputs * 4);
  char* p = out.data();
  *p++ = 1;
  const uint64_t sample = first_col * kEnvColumnFrames;
  std::memcpy(p, &sample, 8);
  p += 8;
  const uint16_t n = static_cast<uint16_t>(cols.size());
  std::memcpy(p, &n, 2);
  p += 2;
  for (const auto& c : cols) {
    for (unsigned ch = 0; ch < kInputs; ++ch) {
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

  svr.Get("/api/state", [this](const httplib::Request&, httplib::Response& res) {
    const EngineStats es = d_.engine.stats();
    const AnalysisSnapshot as = d_.analysis.snapshot();
    const CaptureStatus cs = d_.capture.status();
    // The publisher's copy, not a fresh sample: see the comment on sys_ in webserver.h.
    SysInfo si;
    {
      std::lock_guard<std::mutex> lk(sys_m_);
      si = sys_;
    }

    json sys_json = sysinfo_json(si);
    sys_json.update(json{{"sync_errors", d_.kmsg.sync_errors()},
                         {"sync_lines", d_.kmsg.recent()},
                         {"hostname", si.hostname},
                         {"ips", si.ips},
                         {"has_saved_config", d_.store.has_saved()},
                         {"data_persistent", d_.store.is_persistent()},
                         {"listen_streams", listen_streams_.load()},
                         {"loopback_offset_samples", d_.config.loopback_offset_samples}});

    json outs = json::array();
    for (unsigned i = 0; i < kOutputs; ++i) {
      const uint32_t packed = d_.ctl.outputs[i].source.load();
      const SourceType t = source_type(packed);
      const uint8_t idx = source_index(packed);
      json src{{"type", to_string(t)}};
      if (t == SourceType::Input) {
        src["index"] = idx;
      } else if (t == SourceType::Gen) {
        src["index"] = gen_name(static_cast<GenId>(idx));
      }
      outs.push_back({{"channel", i},
                      {"source", src},
                      {"gain_db", d_.ctl.outputs[i].gain_db.load()},
                      {"mute", d_.ctl.outputs[i].mute.load()},
                      {"name", d_.config.output_names[i]}});
    }

    json ins = json::array();
    for (unsigned i = 0; i < kInputs; ++i) {
      ins.push_back({{"channel", i},
                     {"name", d_.config.input_names[i]},
                     {"gain_db", d_.ctl.inputs[i].gain_db.load()},
                     {"rms_db", as.meters[i].rms_db},
                     {"peak_db", as.meters[i].peak_db},
                     {"tone", tone_json(as.tone[i])}});
    }

    json imap = json::array(), omap = json::array();
    for (unsigned i = 0; i < kInputs; ++i) imap.push_back(d_.ctl.input_map[i].load());
    for (unsigned i = 0; i < kOutputs; ++i) omap.push_back(d_.ctl.output_map[i].load());

    json j{
        {"inputs", ins},
        {"outputs", outs},
        {"generators",
         {{"sine", {{"freq_hz", d_.ctl.sine.freq_hz.load()}, {"level_db", d_.ctl.sine.level_db.load()}}},
          {"noise",
           {{"mode", d_.ctl.noise.mode.load() == static_cast<uint8_t>(NoiseMode::Pink) ? "pink" : "white"},
            {"level_db", d_.ctl.noise.level_db.load()}}},
          {"ping",
           {{"variant", ping_name(static_cast<PingVariant>(d_.ctl.ping.variant.load()))},
            {"interval_s", d_.ctl.ping.interval_s.load()},
            {"level_db", d_.ctl.ping.level_db.load()}}}}},
        {"channel_map", {{"input_map", imap}, {"output_map", omap}}},
        {"capture",
         {{"frozen", cs.frozen},
          {"freeze_sample", cs.freeze_sample},
          {"valid_start", cs.valid_start},
          {"valid_len", cs.valid_len},
          {"analyze_frames", d_.capture.analyze_frames()},
          {"generation", cs.generation}}},
        {"engine",
         {{"running", es.running},
          {"sim", es.sim},
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
         {{"env_column_frames", kEnvColumnFrames},
          {"ring_frames", kRingFrames},
          {"capture_max_frames", d_.capture.capacity_frames()},
          {"capture_config", true},
          {"xcorr_max_len", kXcorrMaxLen},
          {"listen_chunk_frames", kListenChunkFrames},
          {"input_gain_min_db", kInputGainMinDb},
          {"input_gain_max_db", kInputGainMaxDb},
          {"stream_mask", true}}},
        {"spectrum_freqs", d_.analysis.bin_freqs()},
    };
    send_json(res, j);
  });

  svr.Put("/api/inputs/:ch", [this](const httplib::Request& req, httplib::Response& res) {
    unsigned ch = 0;
    if (!parse_index(req, "ch", kInputs, &ch)) return send_error(res, 404, "no such input");
    try {
      const json j = json::parse(req.body);
      if (j.contains("gain_db")) {
        d_.ctl.inputs[ch].gain_db.store(
            std::clamp(j["gain_db"].get<float>(), kInputGainMinDb, kInputGainMaxDb));
      }
    } catch (const std::exception& e) {
      return send_error(res, 400, e.what());
    }
    send_json(res, json{{"ok", true}});
  });

  // Per-input telemetry mask. The console posts which inputs it is watching; disabled inputs are
  // dropped from the spectrum message, the widest frame on the wire. Meters and the wave frame keep
  // their fixed six-channel shape for compatibility with any console, and the console hides
  // disabled inputs itself regardless. Global and last-writer-wins — this appliance has one
  // operator; see docs/api.md.
  svr.Post("/api/stream/inputs", [this](const httplib::Request& req, httplib::Response& res) {
    try {
      const json j = json::parse(req.body);
      const auto& en = j.at("enabled");
      if (!en.is_array() || en.size() != kInputs) {
        return send_error(res, 400, "enabled must be an array of 6 booleans");
      }
      uint32_t mask = 0;
      for (unsigned c = 0; c < kInputs; ++c) {
        if (en[c].get<bool>()) mask |= (1u << c);
      }
      stream_mask_.store(mask, std::memory_order_relaxed);
    } catch (const std::exception& e) {
      return send_error(res, 400, e.what());
    }
    send_json(res, json{{"ok", true}});
  });

  svr.Put("/api/outputs/:ch", [this](const httplib::Request& req, httplib::Response& res) {
    unsigned ch = 0;
    if (!parse_index(req, "ch", kOutputs, &ch)) return send_error(res, 404, "no such output");
    try {
      const json j = json::parse(req.body);
      if (j.contains("source")) {
        const std::string type = j["source"].value("type", "silence");
        SourceType t = SourceType::Silence;
        uint8_t index = 0;
        if (type == "input") {
          const auto& iv = j["source"].at("index");
          const int i = iv.is_string() ? std::atoi(iv.get<std::string>().c_str()) : iv.get<int>();
          if (i < 0 || i >= static_cast<int>(kInputs)) return send_error(res, 400, "bad input index");
          t = SourceType::Input;
          index = static_cast<uint8_t>(i);
        } else if (type == "gen") {
          GenId g;
          if (!parse_gen(j["source"].at("index").get<std::string>(), &g)) {
            return send_error(res, 400, "bad generator name");
          }
          t = SourceType::Gen;
          index = static_cast<uint8_t>(g);
        } else if (type != "silence") {
          return send_error(res, 400, "bad source type");
        }
        d_.ctl.outputs[ch].source.store(pack_source(t, index));
      }
      if (j.contains("gain_db")) {
        d_.ctl.outputs[ch].gain_db.store(std::clamp(j["gain_db"].get<float>(), -60.0f, 0.0f));
      }
      if (j.contains("mute")) d_.ctl.outputs[ch].mute.store(j["mute"].get<bool>());
    } catch (const std::exception& e) {
      return send_error(res, 400, e.what());
    }
    send_json(res, json{{"ok", true}});
  });

  svr.Post("/api/outputs/:ch/identify", [this](const httplib::Request& req, httplib::Response& res) {
    unsigned ch = 0;
    if (!parse_index(req, "ch", kOutputs, &ch)) return send_error(res, 404, "no such output");
    d_.ctl.outputs[ch].identify_until.store(d_.ring.counter() + d_.engine.identify_frames());
    send_json(res, json{{"ok", true}});
  });

  svr.Put("/api/generators/sine", [this](const httplib::Request& req, httplib::Response& res) {
    try {
      const json j = json::parse(req.body);
      if (j.contains("freq_hz")) {
        d_.ctl.sine.freq_hz.store(std::clamp(j["freq_hz"].get<float>(), 1.0f, 40000.0f));
      }
      if (j.contains("level_db")) {
        d_.ctl.sine.level_db.store(std::clamp(j["level_db"].get<float>(), -60.0f, 0.0f));
      }
    } catch (const std::exception& e) {
      return send_error(res, 400, e.what());
    }
    send_json(res, json{{"ok", true}});
  });

  svr.Put("/api/generators/noise", [this](const httplib::Request& req, httplib::Response& res) {
    try {
      const json j = json::parse(req.body);
      if (j.contains("mode")) {
        const std::string m = j["mode"].get<std::string>();
        if (m != "white" && m != "pink") return send_error(res, 400, "mode must be white or pink");
        d_.ctl.noise.mode.store(
            static_cast<uint8_t>(m == "pink" ? NoiseMode::Pink : NoiseMode::White));
      }
      if (j.contains("level_db")) {
        d_.ctl.noise.level_db.store(std::clamp(j["level_db"].get<float>(), -60.0f, 0.0f));
      }
    } catch (const std::exception& e) {
      return send_error(res, 400, e.what());
    }
    send_json(res, json{{"ok", true}});
  });

  svr.Put("/api/generators/ping", [this](const httplib::Request& req, httplib::Response& res) {
    try {
      const json j = json::parse(req.body);
      if (j.contains("variant")) {
        PingVariant v;
        if (!parse_ping(j["variant"].get<std::string>(), &v)) {
          return send_error(res, 400, "variant must be tick, bing or bong");
        }
        d_.ctl.ping.variant.store(static_cast<uint8_t>(v));
      }
      if (j.contains("interval_s")) {
        d_.ctl.ping.interval_s.store(std::clamp(j["interval_s"].get<float>(), 0.5f, 60.0f));
      }
      if (j.contains("level_db")) {
        d_.ctl.ping.level_db.store(std::clamp(j["level_db"].get<float>(), -60.0f, 0.0f));
      }
      // Tells the generator to reschedule from the current sample.
      d_.ctl.ping.epoch.fetch_add(1);
    } catch (const std::exception& e) {
      return send_error(res, 400, e.what());
    }
    send_json(res, json{{"ok", true}});
  });

  svr.Put("/api/channel-map", [this](const httplib::Request& req, httplib::Response& res) {
    try {
      const json j = json::parse(req.body);
      const auto imap = j.at("input_map").get<std::vector<int>>();
      const auto omap = j.at("output_map").get<std::vector<int>>();
      if (imap.size() != kInputs || omap.size() != kOutputs) {
        return send_error(res, 400, "input_map needs 6 entries, output_map 8");
      }
      // Duplicate slots would make two logical channels fight over one physical slot.
      auto distinct = [](std::vector<int> v, int limit) {
        for (int x : v) {
          if (x < 0 || x >= limit) return false;
        }
        std::sort(v.begin(), v.end());
        return std::adjacent_find(v.begin(), v.end()) == v.end();
      };
      if (!distinct(imap, kTdmSlots) || !distinct(omap, kOutputs)) {
        return send_error(res, 400, "slots must be distinct and in range");
      }
      for (unsigned i = 0; i < kInputs; ++i) {
        d_.ctl.input_map[i].store(static_cast<uint8_t>(imap[i]));
      }
      for (unsigned i = 0; i < kOutputs; ++i) {
        d_.ctl.output_map[i].store(static_cast<uint8_t>(omap[i]));
      }
    } catch (const std::exception& e) {
      return send_error(res, 400, e.what());
    }
    send_json(res, json{{"ok", true}});
  });

  svr.Post("/api/capture/freeze", [this](const httplib::Request&, httplib::Response& res) {
    const CaptureStatus cs = d_.capture.freeze(d_.engine.stats().generation);
    if (!cs.frozen) return send_error(res, 503, "not enough captured audio to freeze yet");
    send_json(res, json{{"frozen", cs.frozen},
                        {"freeze_sample", cs.freeze_sample},
                        {"valid_start", cs.valid_start},
                        {"valid_len", cs.valid_len},
                        {"generation", cs.generation}});
  });

  svr.Post("/api/capture/resume", [this](const httplib::Request&, httplib::Response& res) {
    d_.capture.resume();
    send_json(res, json{{"frozen", false}});
  });

  // Sets how many recent frames the next Analyze/freeze grabs. Body: {"seconds": N} (preferred)
  // or {"frames": N}. The value is clamped to [kCaptureMinFrames, capacity]; the reply echoes
  // the clamped result so the client can show what actually took effect.
  svr.Post("/api/capture/config", [this](const httplib::Request& req, httplib::Response& res) {
    try {
      const json j = json::parse(req.body);
      const double rate = d_.engine.rate();
      uint64_t frames;
      if (j.contains("seconds")) {
        const double s = j.at("seconds").get<double>();
        if (!(s > 0.0)) return send_error(res, 400, "seconds must be > 0");
        frames = static_cast<uint64_t>(std::llround(s * rate));
      } else {
        frames = j.at("frames").get<uint64_t>();
      }
      d_.capture.set_analyze_frames(frames);
      const uint64_t now = d_.capture.analyze_frames();
      const uint64_t max = d_.capture.capacity_frames();
      send_json(res, json{{"analyze_frames", now},
                          {"analyze_seconds", now / rate},
                          {"max_frames", max},
                          {"max_seconds", max / rate}});
    } catch (const std::exception& e) {
      send_error(res, 400, e.what());
    }
  });

  svr.Get("/api/capture/status", [this](const httplib::Request&, httplib::Response& res) {
    const CaptureStatus cs = d_.capture.status();
    const uint64_t now = d_.ring.counter();
    send_json(res, json{{"frozen", cs.frozen},
                        {"freeze_sample", cs.freeze_sample},
                        {"valid_start", cs.valid_start},
                        {"valid_len", cs.valid_len},
                        {"generation", cs.generation},
                        {"live_now", now},
                        {"live_oldest", d_.ring.oldest(now)}});
  });

  svr.Get("/api/capture/window", [this](const httplib::Request& req, httplib::Response& res) {
    const unsigned ch = static_cast<unsigned>(query_u64(req, "ch", kInputs));
    if (ch >= kInputs) return send_error(res, 400, "ch must be 0..5");
    const uint64_t now = d_.ring.counter();
    const uint64_t len = query_u64(req, "len", 96000);
    const uint64_t start = query_u64(req, "start", now > len ? now - len : 0);
    const unsigned cols = static_cast<unsigned>(query_u64(req, "cols", 1024));

    const WindowResult w = d_.capture.window(ch, start, len, cols);
    if (!w.ok) return send_error(res, 400, w.error);

    json j{{"ch", ch}, {"start", w.start}, {"len", w.len}, {"raw", w.raw}};
    if (w.raw) {
      j["samples"] = w.samples;
    } else {
      j["min"] = w.mins;
      j["max"] = w.maxs;
    }
    send_json(res, j);
  });

  svr.Post("/api/capture/xcorr", [this](const httplib::Request& req, httplib::Response& res) {
    try {
      const json j = json::parse(req.body);
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
    } catch (const std::exception& e) {
      send_error(res, 400, e.what());
    }
  });

  svr.Get("/api/pings/recent", [this](const httplib::Request&, httplib::Response& res) {
    json arr = json::array();
    for (const auto& p : d_.ctl.ping_log.recent()) {
      arr.push_back({{"sample", p.sample}, {"variant", ping_name(static_cast<PingVariant>(p.variant))}});
    }
    send_json(res, arr);
  });

  svr.Post("/api/config/save", [this](const httplib::Request&, httplib::Response& res) {
    const Config cfg = Config::from_control(d_.ctl, d_.config);
    std::string err;
    if (!d_.store.save(cfg, &err)) return send_error(res, 500, err);
    d_.config = cfg;
    send_json(res, json{{"ok", true}, {"path", d_.store.saved_path()}});
  });

  svr.Post("/api/config/reset", [this](const httplib::Request&, httplib::Response& res) {
    std::string err;
    if (!d_.store.reset(&err)) return send_error(res, 500, err);
    send_json(res, json{{"ok", true}});
  });

  svr.Post("/api/system/reboot", [this](const httplib::Request&, httplib::Response& res) {
    if (!opt_.allow_reboot) return send_error(res, 403, "reboot disabled");
    send_json(res, json{{"ok", true}});
    std::thread([] {
      // Answer first, then go down: the browser needs the response before the socket dies.
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      sync();
      if (std::system("systemctl reboot") != 0) LOG_ERROR("reboot failed");
    }).detach();
  });

  // The rootfs is read-only, so pulling the plug is *usually* survivable — but /data is briefly
  // writable during a save, and a hard power cut inside that window can corrupt the SD card.
  svr.Post("/api/system/shutdown", [this](const httplib::Request&, httplib::Response& res) {
    if (!opt_.allow_reboot) return send_error(res, 403, "shutdown disabled");
    send_json(res, json{{"ok", true}});
    std::thread([] {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      sync();
      if (std::system("systemctl poweroff") != 0) LOG_ERROR("shutdown failed");
    }).detach();
  });

  // Test hook for the sync-error banner; harmless in production.
  svr.Post("/api/system/inject-kmsg", [this](const httplib::Request& req, httplib::Response& res) {
    d_.kmsg.inject(req.body.empty() ? "bcm2835-i2s: I2S SYNC error!" : req.body);
    send_json(res, json{{"ok", true}, {"sync_errors", d_.kmsg.sync_errors()}});
  });

  svr.WebSocket("/api/ws", [this](const httplib::Request&, httplib::ws::WebSocket& ws) {
    auto client = hub_.add();
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
    if (!parse_index(req, "ch", kInputs, &ch)) {
      ws.close(httplib::ws::CloseStatus::PolicyViolation, "bad channel");
      return;
    }
    StreamSlot slot(listen_streams_);
    if (!slot.acquired()) {
      ws.close(httplib::ws::CloseStatus::InternalError, "too many listeners");
      return;
    }

    LOG_INFO("listen ws opened on ch{}", ch);
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
    LOG_INFO("listen ws closed on ch{}", ch);
  });

  svr.Get("/api/inputs/:ch/stream.wav", [this](const httplib::Request& req, httplib::Response& res) {
    unsigned ch = 0;
    if (!parse_index(req, "ch", kInputs, &ch)) return send_error(res, 404, "no such input");

    auto slot = std::make_shared<StreamSlot>(listen_streams_);
    if (!slot->acquired()) return send_error(res, 503, "too many listeners");

    const unsigned rate = d_.engine.stats().rate;
    auto pacer = std::make_shared<ListenPacer>(d_.ring, ch);
    auto sent_header = std::make_shared<bool>(false);
    auto pcm = std::make_shared<std::vector<int16_t>>();

    LOG_INFO("wav stream opened on ch{}", ch);
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
        [ch](bool) { LOG_INFO("wav stream closed on ch{}", ch); });
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

  // Wave frames go out in lockstep with the thing that produces them. The analysis thread
  // appends envelope columns once per tick (kTickHz = 10 Hz, ~20 columns of 480 frames each), so
  // a publisher polling faster finds an empty ring on some ticks and emits nothing. Raising the
  // producer instead would mean running the 6-channel 8192-point FFT half again as often on a
  // Pi 3 to gain nothing — the scope's fidelity is set by the column rate (a constant
  // 200 columns/s), not by how many frames those columns are packed into.
  Task wave{t0, milliseconds(100)};      // 10 Hz — the envelope producer's cadence

  CpuSampler cpu;
  uint64_t wave_cursor = 0;

  while (running_.load()) {
    std::this_thread::sleep_until(std::min({meters.next, spectrum.next, wave.next, system.next}));
    if (!running_.load()) break;

    const auto now = clock::now();
    const bool have_clients = hub_.clients() > 0;

    // Deadlines advance even with nobody listening — otherwise they would all sit in the past
    // and sleep_until() would return instantly, spinning the thread. A deadline that has fallen
    // more than one period behind (a long stall) is snapped forward rather than firing a burst
    // of catch-up frames.
    auto fire = [&](Task& t) {
      if (now < t.next) return false;
      t.next += t.period;
      if (t.next < now) t.next = now + t.period;
      return true;
    };

    // Sampled whether or not a WS client is connected: GET /api/state serves these values to
    // HTTP-only clients, and the CPU delta needs a steady 1 Hz cadence to mean anything.
    if (fire(system)) {
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
        json j{{"type", "system"},
               {"xruns", es.xruns},
               {"generation", es.generation},
               {"sync_errors", d_.kmsg.sync_errors()},
               {"listen_streams", listen_streams_.load()},
               {"engine_running", es.running}};
        j.update(sysinfo_json(si));
        hub_.publish(std::make_shared<WsMessage>(WsMessage{j.dump(), false}));
      }
    }

    if (!have_clients) {
      // Nothing is watching the scope, so do not accumulate a backlog of envelope columns to
      // dump at whoever connects next.
      wave_cursor = d_.analysis.envelope().head();
      continue;
    }

    if (fire(meters)) {
      const AnalysisSnapshot s = d_.analysis.snapshot();
      hub_.publish(std::make_shared<WsMessage>(WsMessage{meters_json(s).dump(), false}));
    }

    if (fire(spectrum)) {
      const AnalysisSnapshot s = d_.analysis.snapshot();
      const uint32_t mask = stream_mask_.load(std::memory_order_relaxed);
      json chans = json::array();
      for (unsigned c = 0; c < kInputs; ++c) {
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

    if (fire(wave)) {
      uint64_t first = 0;
      const auto cols = d_.analysis.envelope().since(wave_cursor, &first);
      if (!cols.empty()) {
        wave_cursor = first + cols.size();
        hub_.publish(std::make_shared<WsMessage>(WsMessage{wave_frame(first, cols), true}));
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
