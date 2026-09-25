#pragma once

#include <map>
#include <string>

namespace st {

// What the image says about a device the daemon will find on this board: the rest of what it
// knows about a device comes from ALSA when it is found.
struct DeviceHint {
  std::string label;    // what the console calls it, instead of the driver's name for the card
  bool hdmi = false;    // offer HDMI's speaker layouts: needed only where the driver's names do
                        // not say HDMI
  bool hidden = false;  // not offered at all: a PCM the board wires to nothing
};

// The board, as /etc/soundtester/board.json describes it: the part of the setup that belongs to
// the image rather than to the operator, so it is never saved over.
//
// The engine card is the one exception to finding every device at runtime. Its capture paces the
// engine and its playback is snd_pcm_link()ed to it, so it is decided before anything starts: the
// Octo on a Pi. A board without one leaves engine_device empty, a timer paces the engine, and
// every device it has is a sink.
struct Board {
  std::string engine_device;
  unsigned capture_channels = 8;  // TDM slots in one capture frame on the engine card
  unsigned rate = 48000;
  unsigned period = 1024;
  unsigned periods = 4;
  // Ring columns kept for capture devices found at runtime (a USB interface's inputs), each costing
  // a ring's worth of pinned RAM whether or not anything is plugged in.
  unsigned device_inputs = 2;
  std::map<std::string, DeviceHint> devices;  // by device id: "<card id>,<device>"

  // The hint for device `id`, or nullptr.
  const DeviceHint* hint(const std::string& id) const;
};

// Reads a board file. A missing file is a board with no engine card and no hints — a desktop — and
// only a file that exists but cannot be parsed is an error.
bool load_board(const std::string& path, Board* out, std::string* err);

}  // namespace st
