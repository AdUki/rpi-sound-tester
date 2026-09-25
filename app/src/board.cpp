#include "board.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

namespace st {

const DeviceHint* Board::hint(const std::string& id) const {
  const auto it = devices.find(id);
  return it == devices.end() ? nullptr : &it->second;
}

bool load_board(const std::string& path, Board* out, std::string* err) {
  Board b;
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    *out = b;
    return true;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  try {
    const json j = json::parse(ss.str());
    if (j.contains("engine")) {
      const json& e = j.at("engine");
      b.engine_device = e.value("device", b.engine_device);
      b.capture_channels = e.value("capture_channels", b.capture_channels);
    }
    b.rate = j.value("rate", b.rate);
    b.period = j.value("period", b.period);
    b.periods = j.value("periods", b.periods);
    b.device_inputs = j.value("device_inputs", b.device_inputs);
    if (j.contains("devices")) {
      for (const auto& [id, d] : j.at("devices").items()) {
        DeviceHint h;
        h.label = d.value("label", h.label);
        h.hdmi = d.value("hdmi", h.hdmi);
        h.hidden = d.value("hidden", h.hidden);
        b.devices[id] = h;
      }
    }
  } catch (const std::exception& e) {
    if (err) *err = path + ": " + e.what();
    return false;
  }
  *out = b;
  return true;
}

}  // namespace st
