#include "genie_wait.h"

#include <cmath>

namespace st {

namespace {

constexpr struct {
  WaitUntil u;
  const char* name;
} kUntil[] = {{WaitUntil::Sound, "sound"},
              {WaitUntil::Silence, "silence"},
              {WaitUntil::Tone, "tone"},
              {WaitUntil::Live, "live"},
              {WaitUntil::Change, "change"}};

}  // namespace

const char* wait_until_name(WaitUntil u) {
  for (const auto& e : kUntil)
    if (e.u == u) return e.name;
  return "?";
}

bool parse_wait_until(const std::string& s, WaitUntil* out) {
  for (const auto& e : kUntil) {
    if (s == e.name) {
      *out = e.u;
      return true;
    }
  }
  return false;
}

bool wait_playing(const WaitCondition& c, const WaitSample& s) {
  return s.peak_now_db > c.threshold_db && s.rms_db > c.min_rms_db;
}

bool WaitTracker::update(uint64_t t_ms, const std::vector<WaitSample>& now, uint64_t sample) {
  bool any_held = false, all_held = !now.empty();
  std::optional<Onset> any_onset, all_onset;
  ++tick_;
  for (const WaitSample& s : now) {
    const bool playing = wait_playing(c_, s);
    auto [it, first] = st_.try_emplace(s.ch);
    State& st = it->second;
    if (first) {
      st.base_playing = playing;
      st.base_frames = s.frames_received;
    } else if (st.last_tick + 1 != tick_) {
      st.since_ms.reset();  // out of the selection for a while: its hold starts over
    }
    st.last_tick = tick_;

    bool cond = false;
    st.changed = 0;
    switch (c_.until) {
      case WaitUntil::Sound:
        cond = playing;
        break;
      case WaitUntil::Silence:
        cond = !(s.peak_now_db > c_.threshold_db);
        break;
      case WaitUntil::Tone:
        cond = s.tone_valid && (!c_.freq_hz || std::fabs(s.tone_hz - *c_.freq_hz) <= c_.tol_hz);
        break;
      case WaitUntil::Live:
        // A counter that went backwards is a channel forgotten and claimed afresh: start over.
        if (s.frames_received < st.base_frames) st.base_frames = s.frames_received;
        cond = s.streaming && s.frames_received > st.base_frames;
        break;
      case WaitUntil::Change:
        cond = playing != st.base_playing;
        if (cond) st.changed = playing ? 1 : -1;
        break;
    }

    if (!cond) {
      st.since_ms.reset();
    } else if (!st.since_ms) {
      st.since_ms = t_ms;
      st.since_sample = sample;
    }
    const bool held = st.since_ms && t_ms - *st.since_ms >= c_.hold_ms;
    any_held = any_held || held;
    all_held = all_held && held;
    if (held) {
      const Onset o{*st.since_ms, st.since_sample};
      if (!any_onset || o.t_ms < any_onset->t_ms) any_onset = o;
      if (!all_onset || o.t_ms > all_onset->t_ms) all_onset = o;
    }
  }
  const bool met = c_.all ? all_held : any_held;
  if (met) met_ = c_.all ? all_onset : any_onset;
  return met;
}

bool WaitTracker::holds(unsigned ch) const {
  const auto it = st_.find(ch);
  return it != st_.end() && it->second.since_ms.has_value();
}

int WaitTracker::changed(unsigned ch) const {
  const auto it = st_.find(ch);
  return it == st_.end() ? 0 : it->second.changed;
}

std::string wait_net_reason(WaitUntil u, const std::vector<WaitNetInput>& net, bool about_senders) {
  if (!about_senders || u == WaitUntil::Silence || u == WaitUntil::Change) return "";
  std::string list;
  for (const WaitNetInput& n : net) {
    if (n.state == "streaming") continue;
    if (!list.empty()) list += "; ";
    list += n.label + " (";
    if (n.state.empty() || n.state == "disconnected") {
      list += n.device.empty() ? "no sender" : n.device + " disconnected";
    } else {
      list += (n.device.empty() ? std::string("sender") : n.device) + " " + n.state;
      list += n.last_audio_ms_ago
                  ? ", last audio " + std::to_string(*n.last_audio_ms_ago) + " ms ago"
                  : ", no audio yet";
    }
    list += ")";
  }
  return list.empty() ? "" : "sender not streaming: " + list;
}

}  // namespace st
