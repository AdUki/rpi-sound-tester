#pragma once

#include "constants.h"

namespace st {

// The ring's columns, decided once at startup and fixed for the life of the process: the engine
// card's own inputs first, then the network channels, then the device inputs.
//
//   [0, local)                   the engine card's ADCs: the Octo's six, or none on a board whose
//                                engine is paced by a timer
//   [local, local + net)         network inputs, NET 1 at `local`
//   [local + net, total)         device inputs: capture devices found at runtime (a USB interface),
//                                each taking the columns it needs from here as it is found
//
// A column never moves while the daemon runs: routes, freezes, listen streams and the plugin's
// per-channel ports all name one by its index.
struct ChannelLayout {
  unsigned local = kInputs;    // the engine card's inputs
  unsigned outputs = kOutputs; // the engine card's outputs
  unsigned net = kNetInputs;
  unsigned device = 0;         // columns for device inputs

  unsigned total() const { return local + net + device; }
  unsigned net_base() const { return local; }
  unsigned device_base() const { return local + net; }
  bool is_net(unsigned c) const { return c >= local && c < local + net; }
  bool is_device(unsigned c) const { return c >= device_base() && c < total(); }

  // A network channel may be attenuated as well as amplified; an ADC's make-up gain only
  // amplifies (see kInputGainMinDb), and a device input is an ADC too.
  float gain_min_db(unsigned c) const { return is_net(c) ? kNetGainMinDb : kInputGainMinDb; }
};

namespace detail {
inline ChannelLayout g_channels;
}

// The process's layout. main() sets it before any thread starts, and nothing changes it after;
// until then, and in a test that does not set one, it is the Octo's.
inline const ChannelLayout& channels() { return detail::g_channels; }
inline void set_channels(const ChannelLayout& layout) { detail::g_channels = layout; }

}  // namespace st
