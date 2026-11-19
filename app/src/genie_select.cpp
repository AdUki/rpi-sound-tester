#include "genie_select.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <map>

namespace st {

namespace {

bool iprefix(const std::string& s, const std::string& prefix) {
  if (prefix.empty() || s.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i])))
      return false;
  }
  return true;
}

// A live sender, by what it calls itself or where it comes from.
bool live_matches(const InputSelector& sel, const NetChannelStatus& c) {
  if (!sel.device.empty() && !iprefix(c.device, sel.device) && !iprefix(c.name, sel.device) &&
      !iprefix(c.host, sel.device) && !iprefix(c.last_device, sel.device) &&
      !iprefix(c.mac, sel.device))
    return false;
  if (!sel.stream.empty() && c.stream != sel.stream) return false;
  return true;
}

// A channel's former sender, by what it is remembered as.
bool past_matches(const InputSelector& sel, const NetChannelStatus& c) {
  if (!sel.device.empty() && !iprefix(c.last_device, sel.device) && !iprefix(c.mac, sel.device))
    return false;
  if (!sel.stream.empty() && c.last_stream != sel.stream) return false;
  return true;
}

std::string quoted_selector(const InputSelector& sel) {
  std::string s;
  if (!sel.device.empty()) s = "device '" + sel.device + "'";
  if (!sel.stream.empty()) s += (s.empty() ? "" : " and ") + std::string("stream '") + sel.stream + "'";
  return s;
}

InputSelection fail(int status, std::string msg) {
  InputSelection r;
  r.status = status;
  r.error = std::move(msg);
  return r;
}

}  // namespace

const char* input_kind_name(InputKind k) {
  switch (k) {
    case InputKind::Net: return "net";
    case InputKind::Device: return "device";
    case InputKind::Local: break;
  }
  return "local";
}

bool parse_input_kind(const std::string& s, InputKind* out) {
  if (s == "local") *out = InputKind::Local;
  else if (s == "net") *out = InputKind::Net;
  else if (s == "device") *out = InputKind::Device;
  else return false;
  return true;
}

InputKind input_kind(const ChannelLayout& layout, unsigned c) {
  return layout.is_net(c) ? InputKind::Net
         : layout.is_device(c) ? InputKind::Device
                               : InputKind::Local;
}

bool parse_ch_list(const std::vector<std::string>& values, unsigned total,
                   std::vector<unsigned>* out, std::string* err) {
  out->clear();
  const std::string range = "ch must be 0.." + std::to_string(total ? total - 1 : 0);
  for (const std::string& v : values) {
    size_t pos = 0;
    while (true) {
      const size_t comma = v.find(',', pos);
      const std::string item = v.substr(pos, comma == std::string::npos ? std::string::npos
                                                                         : comma - pos);
      if (item.empty() || !std::all_of(item.begin(), item.end(),
                                       [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); })) {
        *err = "ch takes input numbers, a comma list or repeated ch= (got '" + v + "'); " + range;
        return false;
      }
      errno = 0;
      const unsigned long n = std::strtoul(item.c_str(), nullptr, 10);
      if (errno != 0 || n >= total) {
        *err = range;
        return false;
      }
      const unsigned c = static_cast<unsigned>(n);
      if (std::find(out->begin(), out->end(), c) == out->end()) out->push_back(c);
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
  }
  if (out->empty()) {
    *err = range;
    return false;
  }
  return true;
}

bool parse_sync_side(const std::string& v, unsigned total, InputSelector* out, std::string* err) {
  *out = InputSelector{};
  if (v.empty()) {
    *err = "must not be empty: an input number or <device>[/<stream>]";
    return false;
  }
  if (std::all_of(v.begin(), v.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
    return parse_ch_list({v}, total, &out->ch, err);
  const size_t slash = v.find('/');
  out->device = v.substr(0, slash);
  if (slash != std::string::npos) out->stream = v.substr(slash + 1);
  if (!out->by_sender()) {
    *err = "names neither a device nor a stream: an input number or <device>[/<stream>]";
    return false;
  }
  return true;
}

InputSelection resolve_inputs(const InputSelector& sel, const ChannelLayout& layout,
                              const std::vector<NetChannelStatus>& net) {
  if (sel.by_sender() && !sel.ch.empty())
    return fail(400, "ch and device/stream are two ways to name inputs: pass one");
  if (sel.stream_index && !sel.by_sender())
    return fail(400, "stream_index picks a member of a sender's run: it needs device= or stream=");
  if (sel.by_sender() && sel.kind && *sel.kind != InputKind::Net)
    return fail(400, "device/stream name network senders: kind must be net or absent");

  InputSelection r;
  if (!sel.by_sender()) {
    if (!sel.ch.empty()) {
      for (unsigned c : sel.ch)
        if (!sel.kind || input_kind(layout, c) == *sel.kind) r.inputs.push_back(c);
    } else {
      for (unsigned c = 0; c < layout.total(); ++c)
        if (!sel.kind || input_kind(layout, c) == *sel.kind) r.inputs.push_back(c);
    }
    return r;
  }

  // Group the connected channels into runs by their first member.
  const unsigned n_net = std::min<unsigned>(layout.net, static_cast<unsigned>(net.size()));
  std::map<unsigned, SelectedRun> runs;          // by base
  std::map<unsigned, std::vector<std::pair<unsigned, unsigned>>> members;  // base -> (index, ch)
  for (unsigned c = 0; c < n_net; ++c) {
    const NetChannelStatus& s = net[c];
    if (!s.connected) continue;
    const unsigned idx = std::max(1u, s.stream_index);
    const unsigned base = idx - 1 <= c ? c - (idx - 1) : c;
    // The run's identity is its first member's; a member that disagrees is not part of it.
    const NetChannelStatus& head = net[base];
    if (!head.connected || head.peer != s.peer) continue;
    if (!live_matches(sel, head)) continue;
    SelectedRun& run = runs[base];
    if (run.state.empty()) {
      run.base = base;
      run.device = head.device;
      run.stream = head.stream;
      run.stream_count = std::max(1u, head.stream_count);
      run.state = "connected";
    }
    if (s.state == "streaming") run.state = "streaming";
    members[base].push_back({idx, c});
  }

  if (runs.empty()) {
    InputSelection f = fail(404, "no connected sender matches " + quoted_selector(sel));
    for (unsigned c = 0; c < n_net; ++c) {
      const NetChannelStatus& s = net[c];
      if (s.connected || s.state.empty() || !past_matches(sel, s)) continue;
      f.last_seen.push_back({c, layout.net_base() + c, s.last_device, s.last_stream, s.state});
    }
    return f;
  }

  const bool any_streaming = std::any_of(runs.begin(), runs.end(), [](const auto& kv) {
    return kv.second.state == "streaming";
  });
  for (auto& [base, run] : runs) {
    if (any_streaming && run.state != "streaming") continue;
    auto& m = members[base];
    std::sort(m.begin(), m.end());
    for (const auto& [idx, c] : m) {
      if (sel.stream_index && idx != sel.stream_index) continue;
      run.inputs.push_back(layout.net_base() + c);
    }
    if (run.inputs.empty()) continue;
    r.inputs.insert(r.inputs.end(), run.inputs.begin(), run.inputs.end());
    r.runs.push_back(std::move(run));
  }
  if (r.runs.empty()) {
    return fail(404, "no connected run matching " + quoted_selector(sel) + " has a stream_index " +
                         std::to_string(sel.stream_index));
  }
  return r;
}

}  // namespace st
