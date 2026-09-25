#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "constants.h"

namespace st {

// The channel layouts a sink offers, and where each of its channels goes in the PCM.
//
// An HDMI sink offers speaker layouts: mono, stereo, 5.1 and 7.1. Only layouts a sink places
// cleanly: the Pi's firmware audio driver sends the firmware a channel COUNT and nothing else (the
// per-stream channel map, "audioserv 3", was never merged into the Raspberry Pi kernel:
// raspberrypi/linux#1257), so the firmware picks the speaker layout from the count. For 1, 2, 6
// and 8 channels there is one layout anyone means; for 3, 4, 5 and 7 there is not (4 is quad or
// 3.1, 5 is 5.0 or 4.1), and a mode that might land on the wrong speakers is not offered at all.
//
// HDMI routing is indexed by SPEAKER, not by PCM slot, so that L is L in every layout and a
// correction to the slot table below moves no one's routing. The slot order is HDMI's own
// (CEA-861: FL FR LFE FC RL RR RLC RRC), not ALSA's (FL FR RL RR FC LFE SL SR): HDMI drivers pass
// the PCM's channels through to the sink as they are. Confirmed for 5.1 on a soundbar; 7.1's back
// pair (RLC RRC) is the same standard's, not yet heard. If a sink shows otherwise, `slot` is the
// one place to change. A desktop sound server (PipeWire, PulseAudio) reads the PCM in ALSA's order,
// so a laptop test through one shows C/LFE and the surround pair swapped: that is the laptop, not
// this table.
//
// Any other sink offers stereo and its own channel count, channels numbered as the device numbers
// them: nothing here knows which speaker a USB interface's third output feeds.
enum class SinkLayout : uint8_t {
  Mono = 0, Stereo, S51, S71,        // HDMI's
  Ch1, Ch3, Ch4, Ch5, Ch6, Ch7, Ch8,  // a device's own channels, in its own order
  Count
};

// Speaker positions in HDMI's layouts, in the order the console lists them and the API indexes
// them.
enum HdmiSpeaker : uint8_t {
  kSpkL = 0,   // front left (the mono channel, in mono)
  kSpkR = 1,   // front right
  kSpkC = 2,   // centre
  kSpkLfe = 3,
  kSpkLs = 4,  // surround left: the side pair in 7.1, the only surround pair in 5.1
  kSpkRs = 5,
  kSpkLb = 6,  // back left, 7.1 only
  kSpkRb = 7,
};

struct SinkLayoutInfo {
  const char* name;       // what the API and config say
  unsigned speakers;      // how many are routed: channels [0, speakers)
  unsigned pcm_channels;  // how wide the PCM is opened
  uint8_t slot[kMaxSinkWidth];  // channel -> PCM slot
  bool hdmi;              // offered by HDMI sinks only
};

inline constexpr SinkLayoutInfo kSinkLayouts[] = {
    // Mono is one speaker sent to both sides of a stereo PCM: a sink given a one-channel stream
    // may play it from the left speaker alone. The sink's thread does the duplicating.
    {"mono", 1, 2, {0}, true},
    {"stereo", 2, 2, {0, 1}, false},
    //                L  R  C  LFE Ls Rs
    {"5.1", 6, 6, {0, 1, 3, 2, 4, 5}, true},
    //                L  R  C  LFE Ls Rs Lb Rb
    {"7.1", 8, 8, {0, 1, 3, 2, 4, 5, 6, 7}, true},
    {"1ch", 1, 1, {0}, false},
    {"3ch", 3, 3, {0, 1, 2}, false},
    {"4ch", 4, 4, {0, 1, 2, 3}, false},
    {"5ch", 5, 5, {0, 1, 2, 3, 4}, false},
    {"6ch", 6, 6, {0, 1, 2, 3, 4, 5}, false},
    {"7ch", 7, 7, {0, 1, 2, 3, 4, 5, 6}, false},
    {"8ch", 8, 8, {0, 1, 2, 3, 4, 5, 6, 7}, false},
};
static_assert(sizeof(kSinkLayouts) / sizeof(kSinkLayouts[0]) ==
                  static_cast<size_t>(SinkLayout::Count),
              "one table row per layout");

inline constexpr SinkLayout kSinkLayoutDefault = SinkLayout::Stereo;

inline const SinkLayoutInfo& sink_layout_info(SinkLayout l) {
  const auto i = static_cast<size_t>(l);
  return kSinkLayouts[i < static_cast<size_t>(SinkLayout::Count) ? i : 1];
}

inline const char* sink_layout_name(SinkLayout l) { return sink_layout_info(l).name; }

inline bool parse_sink_layout(const std::string& s, SinkLayout* out) {
  for (uint8_t i = 0; i < static_cast<uint8_t>(SinkLayout::Count); ++i) {
    if (s == kSinkLayouts[i].name) {
      *out = static_cast<SinkLayout>(i);
      return true;
    }
  }
  return false;
}

// The layouts a device offers: HDMI's speaker layouts on an HDMI sink, else stereo and the
// device's own channel count, each only if the device can be opened that wide. Never empty: a
// device that reports nothing usable is offered stereo, and its open says why it fails.
inline std::vector<SinkLayout> sink_layouts(bool hdmi, unsigned min_ch, unsigned max_ch) {
  std::vector<SinkLayout> v;
  for (uint8_t i = 0; i < static_cast<uint8_t>(SinkLayout::Count); ++i) {
    const auto l = static_cast<SinkLayout>(i);
    const SinkLayoutInfo& info = sink_layout_info(l);
    if (info.pcm_channels < min_ch || info.pcm_channels > max_ch) continue;
    if (hdmi ? (info.hdmi || l == SinkLayout::Stereo)
             : (l == SinkLayout::Stereo ||
                (!info.hdmi && info.pcm_channels == std::min(max_ch, kMaxSinkWidth))))
      v.push_back(l);
  }
  if (v.empty()) v.push_back(SinkLayout::Stereo);
  return v;
}

// The short name a channel goes by in a layout: a speaker on HDMI, a number otherwise. Mono's one
// speaker is not "L".
inline const char* sink_speaker_name(SinkLayout l, unsigned ch) {
  static constexpr const char* kSpeakers[kMaxSinkWidth] = {"L",  "R",  "C",  "LFE",
                                                           "Ls", "Rs", "Lb", "Rb"};
  static constexpr const char* kNumbers[kMaxSinkWidth] = {"1", "2", "3", "4", "5", "6", "7", "8"};
  if (ch >= kMaxSinkWidth) return "?";
  if (l == SinkLayout::Mono) return "M";
  if (l == SinkLayout::Stereo || sink_layout_info(l).hdmi) return kSpeakers[ch];
  return kNumbers[ch];
}

// The Pi's HDMI audio carries 8 channels at up to 48 kHz and only stereo above that
// (raspberrypi/linux#3933: "8 channel 48kHz PCM, 2 channel 192kHz PCM"), so HDMI surround is
// refused at 96 kHz rather than handed to a driver that would resample or corrupt it.
inline constexpr bool surround_rate_ok(SinkLayout l, unsigned rate) {
  return rate <= 48000 || !(l == SinkLayout::S51 || l == SinkLayout::S71);
}

}  // namespace st
