#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hdmi_layout.h"

namespace st {

// What the daemon has to know about the board it runs on: which card drives the sample clock and
// how, which of the SoC's own outputs there are and where to find them, what the mixer is set to,
// and what the console may offer. Everything board-specific is described here once, instead of in
// the defaults of each class that uses it.
//
// Only one board is described so far, and it is compiled in: the Pi with the Octo. Every value in
// it is what the daemon used before there were profiles, so that the Pi's /api/state, config.json
// and logs are the same byte for byte.
//
// Not every field is read yet. The defaults of Config and EngineOptions, --help, and each sink's
// name, device, width and hint come from here; the fields marked "declared" below describe the Pi
// as it is, and nothing acts on them until the backend and the mixer are driven by the profile.

// What paces the engine's blocks.
enum class ClockKind : uint8_t {
  // The card's capture: playback is snd_pcm_link()ed to it on the same card, so that the two start
  // together and share sample zero.
  LinkedDuplex,
  // A timer: the simulator, or a board with no card whose clock the engine can follow.
  Timer,
};

struct ClockProfile {
  ClockKind kind = ClockKind::LinkedDuplex;  // declared: --sim alone picks the timer
  // One ALSA name per direction. On a linked card both are the same device, and the engine opens
  // capture and playback on capture_device alone, so playback_device is declared.
  std::string capture_device;
  std::string playback_device;
  unsigned rate = 0;
  // Frames per block, as the engine asks for them unless the config says otherwise. The network
  // write guard is two of these whatever the config says.
  unsigned period = 0;
  unsigned periods = 0;  // blocks in the PCM's buffer
  // Slots in one capture frame, in the order they are tried: the engine asks for the first, and
  // falls back to the next when the driver will not open it. Only the first is read; the fallback
  // is still AlsaLinkedBackend's own kInputs.
  std::vector<unsigned> capture_slots;
};

// One of the SoC's own outputs.
struct SinkProfile {
  std::string id;      // what the API, the config file and the log call it
  std::string device;  // the ALSA name it opens unless the config names another
  // The most PCM slots it can have, and so how wide its handoff ring is. The engine renders each
  // sink at a width fixed when it is compiled, kHdmiMaxChannels or kLineoutChannels, and refuses
  // a ring of any other width (EngineCore::set_hdmi_ring), so a profile that gets this wrong
  // leaves the sink silent.
  unsigned width = 0;
  // Declared: the speaker layouts it offers. None means stereo and nothing else, with no layout in
  // the config or the API at all. The API still offers hdmi_layout.h's whole table on HDMI.
  std::vector<HdmiLayout> layouts;
  // Declared: the highest rate it carries more than two channels at; 0 when it never does.
  // hdmi_layout_rate_ok() still holds its own 48 kHz.
  unsigned surround_max_rate = 0;
  // Why the device might not exist, appended to an open error that says it does not.
  std::string where_hint;
};

// Declared: the engine sets every volume to 0 dB on each open whatever this says.
enum class MixerPolicy : uint8_t {
  // Every playback and capture volume on the card set to 0 dB, each time the card opens.
  AllSimple0dB,
  // The mixer is left as the driver set it.
  None,
};

// Declared: what the console may offer on this board. It offers the channel map and the sync
// watch everywhere today.
struct BoardCapabilities {
  unsigned channel_map = 0;  // TDM slots the channel map can remap; 0 when there is none
  bool sync_watch = false;   // whether the kernel log is watched for I2S sync errors
};

struct BoardProfile {
  std::string id;     // declared: nothing is told which board it runs on yet
  std::string label;  // how --help names the hardware
  ClockProfile clock;
  std::vector<SinkProfile> sinks;
  MixerPolicy mixer = MixerPolicy::None;
  BoardCapabilities capabilities;

  // The sink called `sink_id`, or nullptr when the board has none.
  const SinkProfile* sink(const std::string& sink_id) const;
};

// The Raspberry Pi with the Audio Injector Octo, compiled in. Until profiles are read from files it
// is the only board there is, and every default that names a device, a rate or a period is taken
// from it.
const BoardProfile& rpi3_octo_profile();

}  // namespace st
