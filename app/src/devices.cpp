#include "devices.h"

#include <algorithm>
#include <chrono>

#include "audio_engine.h"
#include "channel_layout.h"
#include "util/log.h"

namespace st {

SinkStart sink_start(const SinkDevice& d, const SinkConfig& saved) {
  SinkStart st;
  st.layout = d.default_layout();
  if (SinkLayout l; parse_sink_layout(saved.layout, &l) && d.offers(l)) st.layout = l;
  st.rate = saved.sample_rate && d.offers_rate(saved.sample_rate) ? saved.sample_rate
                                                                  : d.default_rate();
  if (!surround_rate_ok(st.layout, st.rate)) st.rate = kSinkRateDefault;
  return st;
}

unsigned input_columns(const PcmCaps& caps) {
  return std::max(caps.min_channels, std::min(2u, caps.max_channels));
}

Devices::Devices(Control& ctl, AudioEngine& engine, const Board& board,
                 std::string engine_device, std::vector<std::string> extra_sinks)
    : ctl_(ctl),
      engine_(engine),
      board_(board),
      engine_card_(pcm_card_id(pcm_device_id(engine_device))) {
  // A hardware device named here is one the scan finds anyway, and two slots on one PCM would
  // leave one of them unable to open it.
  for (std::string& name : extra_sinks)
    if (pcm_device_id(name).empty()) extra_.push_back(std::move(name));
}

Devices::~Devices() { stop(); }

void Devices::start(const std::map<std::string, SinkConfig>& saved) {
  {
    std::lock_guard<std::mutex> lk(m_);
    saved_ = saved;
  }
  scan();
  std::lock_guard<std::mutex> lk(wake_m_);
  if (running_) return;
  running_ = true;
  thread_ = std::thread([this] { loop(); });
}

void Devices::stop() {
  {
    std::lock_guard<std::mutex> lk(wake_m_);
    running_ = false;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lk(m_);
  for (auto& o : outputs_)
    if (o) o->stop();
  for (Input& in : inputs_) in.in->stop();
}

void Devices::loop() {
  std::unique_lock<std::mutex> lk(wake_m_);
  while (running_) {
    wake_.wait_for(lk, std::chrono::milliseconds(kScanIntervalMs), [this] { return !running_; });
    if (!running_) break;
    lk.unlock();
    scan();
    lk.lock();
  }
}

std::string Devices::label(const PcmDevice& pcm, unsigned pcms_on_card) const {
  if (const DeviceHint* hint = board_.hint(pcm.id); hint && !hint->label.empty()) return hint->label;
  if (pcms_on_card > 1 && !pcm.pcm_name.empty()) return pcm.card_name + ": " + pcm.pcm_name;
  return pcm.card_name.empty() ? pcm.id : pcm.card_name;
}

SinkDevice Devices::sink_device(const PcmDevice& pcm, unsigned pcms_on_card) const {
  const DeviceHint* hint = board_.hint(pcm.id);
  SinkDevice d;
  d.id = pcm.id;
  d.alsa = pcm.alsa;
  d.usb = pcm.usb;
  d.hdmi = (hint && hint->hdmi) || pcm_looks_hdmi(pcm);
  d.label = label(pcm, pcms_on_card);
  // Opens the device for a moment, so only for one no sink is playing on. HDMI offers its speaker
  // layouts whatever the probe says: what an HDMI driver accepts follows the display attached at
  // the time, and one that cannot take a layout says so when it is opened.
  const PcmCaps caps = probe_pcm(pcm.alsa, false);
  d.layouts = d.hdmi ? sink_layouts(true, 1, kMaxSinkWidth)
                     : sink_layouts(false, caps.min_channels, caps.max_channels);
  d.rates = caps.rates;
  d.probed = caps.probed;
  return d;
}

int Devices::free_columns_locked(unsigned count) const {
  const ChannelLayout& lay = channels();
  for (unsigned c = lay.device_base(); c + count <= lay.total(); ++c) {
    bool free = true;
    for (unsigned k = 0; k < count && free; ++k) free = !column_used_[c + k];
    if (free) return static_cast<int>(c);
  }
  return -1;
}

void Devices::scan() {
  std::vector<PcmDevice> found = scan_pcm_devices();
  for (const std::string& name : extra_) {
    PcmDevice d;
    d.id = d.alsa = d.card_name = name;
    d.playback = true;
    found.push_back(d);
  }
  std::map<std::string, unsigned> per_card;
  for (const PcmDevice& d : found) ++per_card[d.card_id];

  auto is_engine = [this](const PcmDevice& d) {
    return !engine_card_.empty() && d.card_id == engine_card_;
  };
  auto is_hidden = [this](const PcmDevice& d) {
    const DeviceHint* h = board_.hint(d.id);
    return h && h->hidden;
  };

  // What is new, and which bound sinks are worth probing again. Probed outside the lock: opening a
  // device can take a moment, and the web server reads the tables meanwhile.
  std::vector<std::string> sink_ids, input_ids;
  std::vector<std::pair<unsigned, PcmDevice>> reprobe;
  {
    std::lock_guard<std::mutex> lk(m_);
    for (unsigned s = 0; s < kMaxSinks; ++s)
      if (used_[s]) sink_ids.push_back(bound_[s].device.id);
    for (const Input& in : inputs_) input_ids.push_back(in.id);
    for (const PcmDevice& d : found)
      for (unsigned s = 0; s < kMaxSinks; ++s)
        if (used_[s] && bound_[s].device.id == d.id && !bound_[s].device.probed &&
            !outputs_[s]->running())
          reprobe.emplace_back(s, d);
  }
  auto known = [](const std::vector<std::string>& ids, const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
  };
  std::vector<SinkDevice> fresh_sinks;
  std::vector<std::pair<PcmDevice, PcmCaps>> fresh_inputs;
  for (const PcmDevice& d : found) {
    if (is_engine(d) || is_hidden(d)) continue;
    if (d.playback && !known(sink_ids, d.id))
      fresh_sinks.push_back(sink_device(d, per_card[d.card_id]));
    if (d.capture && !known(input_ids, d.id)) fresh_inputs.emplace_back(d, probe_pcm(d.alsa, true));
  }
  std::vector<std::pair<unsigned, SinkDevice>> reprobed;
  for (const auto& [s, d] : reprobe) {
    SinkDevice sd = sink_device(d, per_card[d.card_id]);
    if (sd.probed) reprobed.emplace_back(s, sd);
  }

  std::lock_guard<std::mutex> lk(m_);
  for (const SinkDevice& d : fresh_sinks) {
    unsigned s = 0;
    while (s < kMaxSinks && used_[s]) ++s;
    if (s == kMaxSinks) break;
    bind_sink_locked(s, d);
  }
  for (const auto& [s, d] : reprobed) {
    // Its first probe could not open it; now it has said what it takes. Nothing plays on it.
    if (outputs_[s]->running()) continue;
    const auto it = saved_.find(d.id);
    const SinkStart start = sink_start(d, it == saved_.end() ? SinkConfig{} : it->second);
    bound_[s].device = d;
    ctl_.sinks[s].layout.store(static_cast<uint8_t>(start.layout));
    outputs_[s]->bind(d, start.rate);
  }
  for (const auto& [d, caps] : fresh_inputs) {
    const unsigned count = input_columns(caps);
    const int first = free_columns_locked(count);
    if (first < 0) continue;
    Input in;
    in.id = d.id;
    in.label = label(d, per_card[d.card_id]);
    in.first = static_cast<unsigned>(first);
    in.present = true;
    in.in = std::make_unique<DeviceInput>(d.id, d.alsa, count, ctl_, engine_);
    for (unsigned c = 0; c < count; ++c) {
      column_used_[in.first + c] = true;
      engine_.set_input_timeline(in.first + c, &in.in->timeline(c));
    }
    in.in->start();
    LOG_INFO("input: {} ({}, {}) on columns {}-{}", in.id, in.label, d.alsa, in.first,
             in.first + count - 1);
    inputs_.push_back(std::move(in));
  }

  std::array<bool, kMaxSinks> seen{};
  std::vector<bool> seen_input(inputs_.size(), false);
  devices_.clear();
  for (const PcmDevice& d : found) {
    DeviceStatus st;
    st.pcm = d;
    st.label = label(d, per_card[d.card_id]);
    st.engine = is_engine(d);
    st.hidden = is_hidden(d);
    if (!st.engine && !st.hidden) {
      for (unsigned s = 0; s < kMaxSinks; ++s) {
        if (used_[s] && bound_[s].device.id == d.id) {
          st.sink = static_cast<int>(s);
          seen[s] = true;
        }
      }
      for (size_t i = 0; i < inputs_.size(); ++i) {
        if (inputs_[i].id == d.id) {
          st.input = static_cast<int>(inputs_[i].first);
          st.input_channels = inputs_[i].in->channels();
          seen_input[i] = true;
        }
      }
      if (d.playback && st.sink < 0) st.note = "no free sink slot";
      if (d.capture && st.input < 0) st.note = "no free input columns";
    }
    devices_.push_back(st);
  }
  for (unsigned s = 0; s < kMaxSinks; ++s) {
    if (!used_[s] || bound_[s].present == seen[s]) continue;
    bound_[s].present = seen[s];
    LOG_INFO("sink {}: {} ({}) {}", s, bound_[s].device.id, bound_[s].device.label,
             seen[s] ? "is back" : "is gone");
  }
  for (size_t i = 0; i < inputs_.size(); ++i) {
    if (inputs_[i].present == seen_input[i]) continue;
    inputs_[i].present = seen_input[i];
    LOG_INFO("input: {} ({}) {}", inputs_[i].id, inputs_[i].label,
             seen_input[i] ? "is back" : "is gone");
  }
}

void Devices::bind_sink_locked(unsigned slot, const SinkDevice& d) {
  const auto it = saved_.find(d.id);
  const SinkConfig cfg = it == saved_.end() ? SinkConfig{} : it->second;
  SinkControl& sc = ctl_.sinks[slot];
  for (unsigned c = 0; c < kMaxSinkWidth; ++c) apply_output(cfg.outputs[c], sc.outputs[c]);
  const SinkStart start = sink_start(d, cfg);
  sc.layout.store(static_cast<uint8_t>(start.layout));

  used_[slot] = true;
  bound_[slot] = {slot, d, true};
  outputs_[slot] = std::make_unique<SinkOutput>(slot, sc, ctl_, engine_);
  outputs_[slot]->bind(d, start.rate);
  engine_.set_sink_ring(slot, &outputs_[slot]->ring());
  LOG_INFO("sink {}: {} ({}, {}){}", slot, d.id, d.label, d.alsa, cfg.enabled ? ", on" : "");
  // The audio thread renders the channels before the sink's thread goes looking for them.
  if (cfg.enabled) {
    sc.enabled.store(true);
    outputs_[slot]->start();
  }
}

std::vector<Devices::Sink> Devices::sinks() const {
  std::lock_guard<std::mutex> lk(m_);
  std::vector<Sink> v;
  for (unsigned s = 0; s < kMaxSinks; ++s)
    if (used_[s]) v.push_back(bound_[s]);
  return v;
}

std::vector<Devices::Source> Devices::sources() const {
  std::lock_guard<std::mutex> lk(m_);
  std::vector<Source> v;
  for (const Input& in : inputs_)
    v.push_back({in.id, in.label, in.first, in.in->channels(), in.present, in.in->status()});
  std::sort(v.begin(), v.end(), [](const Source& a, const Source& b) { return a.first < b.first; });
  return v;
}

std::string Devices::channel_name(unsigned slot, unsigned ch) const {
  std::lock_guard<std::mutex> lk(m_);
  if (slot >= kMaxSinks || !used_[slot] || ch >= kMaxSinkWidth) return {};
  const auto it = saved_.find(bound_[slot].device.id);
  return it == saved_.end() ? std::string() : it->second.names[ch];
}

std::vector<DeviceStatus> Devices::devices() const {
  std::lock_guard<std::mutex> lk(m_);
  return devices_;
}

std::map<std::string, SinkConfig> Devices::sink_configs() const {
  std::lock_guard<std::mutex> lk(m_);
  std::map<std::string, SinkConfig> out = saved_;
  for (unsigned s = 0; s < kMaxSinks; ++s) {
    if (!used_[s]) continue;
    const SinkControl& sc = ctl_.sinks[s];
    const bool had = out.count(bound_[s].device.id) != 0;
    SinkConfig& c = out[bound_[s].device.id];
    c.enabled = sc.enabled.load();
    // A device that could not yet be asked what it takes runs on guesses: keep what was saved.
    if (bound_[s].device.probed || !had) {
      c.layout = sink_layout_name(sink_layout(sc));
      c.sample_rate = outputs_[s]->sample_rate();
    }
    for (unsigned ch = 0; ch < kMaxSinkWidth; ++ch)
      c.outputs[ch] = output_from_control(sc.outputs[ch]);
  }
  return out;
}

uint64_t Devices::pinned_bytes() const {
  std::lock_guard<std::mutex> lk(m_);
  uint64_t b = 0;
  for (const auto& o : outputs_)
    if (o) b += o->pinned_bytes();
  return b;
}

}  // namespace st
