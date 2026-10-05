#include "bt_player.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "channel_layout.h"

namespace st {

namespace {

// The order next and previous walk in. Inputs are not in it: a speaker's button cannot know which
// one is plugged into anything.
constexpr uint32_t kCycle[] = {
    pack_source(SourceType::Silence, 0),
    pack_source(SourceType::Gen, static_cast<uint8_t>(GenId::Sine)),
    pack_source(SourceType::Gen, static_cast<uint8_t>(GenId::Noise)),
    pack_source(SourceType::Gen, static_cast<uint8_t>(GenId::Ping)),
    pack_source(SourceType::Gen, static_cast<uint8_t>(GenId::Music)),
};
constexpr int kCycleLen = static_cast<int>(sizeof(kCycle) / sizeof(kCycle[0]));

// The console's names for the ring's columns: the engine card's ADCs, then the network channels.
// A device input is named by its device, which this cannot see; its column number will do.
std::string input_name(unsigned i) {
  const ChannelLayout& lay = channels();
  if (lay.is_net(i)) return "NET " + std::to_string(i - lay.net_base() + 1);
  return "IN " + std::to_string(i + 1);
}

std::string source_name(uint32_t s) {
  switch (source_type(s)) {
    case SourceType::Input: return input_name(source_index(s));
    case SourceType::Gen:
      switch (static_cast<GenId>(source_index(s))) {
        case GenId::Sine: return "Sine";
        case GenId::Noise: return "Noise";
        case GenId::Ping: return "Ping";
        case GenId::Music: return "Music";
        default: return "?";
      }
    default: return "Silence";
  }
}

std::string level(float db) {
  char b[24];
  std::snprintf(b, sizeof b, "%g dBFS", std::round(db * 10.0f) / 10.0f);
  return b;
}

std::string source_detail(const Control& c, uint32_t s) {
  if (source_type(s) != SourceType::Gen) return {};
  char b[64];
  switch (static_cast<GenId>(source_index(s))) {
    case GenId::Sine:
      std::snprintf(b, sizeof b, "%g Hz · ", std::round(c.sine.freq_hz.load() * 10.0f) / 10.0f);
      return b + level(c.sine.level_db.load());
    case GenId::Noise:
      return std::string(static_cast<NoiseMode>(c.noise.mode.load()) == NoiseMode::Pink ? "pink"
                                                                                       : "white") +
             " · " + level(c.noise.level_db.load());
    case GenId::Ping:
      std::snprintf(b, sizeof b, "%s every %g s · ",
                    ping_name(static_cast<PingVariant>(c.ping.variant.load())),
                    static_cast<double>(c.ping.interval_s.load()));
      return b + level(c.ping.level_db.load());
    case GenId::Music: return level(c.music.level_db.load());
    default: return {};
  }
}

// The speaker's two channels: a stereo sink's L and R.
constexpr unsigned kWidth = kBtChannels;

bool same_source(const SinkControl& s) {
  const uint32_t s0 = s.outputs[0].source.load();
  for (unsigned c = 1; c < kWidth; ++c)
    if (s.outputs[c].source.load() != s0) return false;
  return true;
}

}  // namespace

std::string bt_tester_title(const SinkControl& s) {
  if (same_source(s)) return source_name(s.outputs[0].source.load());
  std::string t;
  for (unsigned c = 0; c < kWidth; ++c)
    t += (t.empty() ? "" : " / ") + source_name(s.outputs[c].source.load());
  return t;
}

std::string bt_tester_detail(const Control& c, const SinkControl& s) {
  return same_source(s) ? source_detail(c, s.outputs[0].source.load()) : std::string();
}

const char* bt_tester_status(const SinkControl& s) {
  if (!s.enabled.load()) return "stopped";
  for (unsigned c = 0; c < kWidth; ++c)
    if (!s.outputs[c].mute.load()) return "playing";
  return "paused";
}

void bt_tester_step(SinkControl& s, int dir) {
  const uint32_t cur = s.outputs[0].source.load();
  int at = -1;
  for (int i = 0; i < kCycleLen; ++i)
    if (kCycle[i] == cur) at = i;
  // From an input, next starts the cycle at its beginning and previous at its end.
  if (at < 0) at = dir > 0 ? -1 : 0;
  const int to = ((at + (dir > 0 ? 1 : -1)) % kCycleLen + kCycleLen) % kCycleLen;
  for (unsigned c = 0; c < kWidth; ++c) s.outputs[c].source.store(kCycle[to]);
}

BtTesterPlayer::BtTesterPlayer(const Control& ctl, SinkControl& sink,
                               std::function<void(bool)> set_output)
    : ctl_(ctl), sink_(sink), set_output_(std::move(set_output)) {}

BtPlayerInfo BtTesterPlayer::poll(uint64_t now_ns) {
  BtPlayerInfo p;
  p.present = true;
  p.local = true;
  p.title = bt_tester_title(sink_);
  p.artist = kBtTesterArtist;
  p.album = bt_tester_detail(ctl_, sink_);
  p.status = bt_tester_status(sink_);

  const bool was_playing = status_ == "playing";
  if (p.title != title_ || p.status == "stopped") {
    played_ms_ = 0;
    since_ns_ = now_ns;
  } else if (p.status != status_) {
    if (was_playing) played_ms_ += (now_ns - since_ns_) / 1000000ull;
    since_ns_ = now_ns;
  }
  title_ = p.title;
  status_ = p.status;
  const uint64_t ms = played_ms_ + (p.status == "playing" ? (now_ns - since_ns_) / 1000000ull : 0);
  p.position_ms = static_cast<uint32_t>(std::min<uint64_t>(ms, UINT32_MAX));
  return p;
}

bool BtTesterPlayer::command(const std::string& cmd) {
  auto mute = [this](bool m) {
    for (unsigned c = 0; c < kWidth; ++c) sink_.outputs[c].mute.store(m);
  };
  if (cmd == "playpause") return command(bt_tester_status(sink_) == std::string("playing") ? "pause"
                                                                                          : "play");
  if (cmd == "play") {
    if (!sink_.enabled.load() && set_output_) set_output_(true);
    mute(false);
  } else if (cmd == "pause") {
    mute(true);
  } else if (cmd == "stop") {
    if (sink_.enabled.load() && set_output_) set_output_(false);
  } else if (cmd == "next" || cmd == "previous") {
    bt_tester_step(sink_, cmd == "next" ? 1 : -1);
  } else {
    return false;
  }
  return true;
}

}  // namespace st
