#pragma once

#include <cstdint>
#include <string>

#include "constants.h"

namespace st {

// The speaker layouts the HDMI output offers, and where each speaker's channel goes in the PCM.
//
// Only layouts the Pi can place cleanly. The firmware audio driver sends the firmware a channel
// COUNT and nothing else (the per-stream channel map, "audioserv 3", was never merged into the
// Raspberry Pi kernel: raspberrypi/linux#1257), so the firmware picks the speaker layout from the
// count. For 1, 2, 6 and 8 channels there is one layout anyone means; for 3, 4, 5 and 7 there is
// not (4 is quad or 3.1, 5 is 5.0 or 4.1), and a mode that might land on the wrong speakers is not
// offered at all.
//
// Routing is indexed by SPEAKER, not by PCM slot, so that L is L in every layout and a correction
// to the slot table below moves no one's routing. The slot order is HDMI's own (CEA-861: FL FR LFE
// FC RL RR RLC RRC), not ALSA's (FL FR RL RR FC LFE SL SR): the firmware passes the PCM's channels
// through to the sink as they are. Confirmed for 5.1 on a soundbar; 7.1's back pair (RLC RRC) is
// the same standard's, not yet heard. If a sink shows otherwise, `slot` is the one place to change.
// A desktop sound server (PipeWire, PulseAudio) reads the PCM in ALSA's order, so a laptop test
// through one shows C/LFE and the surround pair swapped: that is the laptop, not this table.
enum class HdmiLayout : uint8_t { Mono = 0, Stereo = 1, S51 = 2, S71 = 3, Count = 4 };

// Speaker positions, in the order the console lists them and the API indexes them.
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

struct HdmiLayoutInfo {
  const char* name;       // what the API and config say
  unsigned speakers;      // how many are routed: speakers [0, speakers) of HdmiSpeaker
  unsigned pcm_channels;  // how wide the PCM is opened
  uint8_t slot[kHdmiMaxChannels];  // speaker -> PCM slot
};

inline constexpr HdmiLayoutInfo kHdmiLayouts[] = {
    // Mono is one speaker sent to both sides of a stereo PCM: a sink given a one-channel stream
    // may play it from the left speaker alone. The HDMI thread does the duplicating.
    {"mono", 1, 2, {0}},
    {"stereo", 2, 2, {0, 1}},
    //                L  R  C  LFE Ls Rs
    {"5.1", 6, 6, {0, 1, 3, 2, 4, 5}},
    //                L  R  C  LFE Ls Rs Lb Rb
    {"7.1", 8, 8, {0, 1, 3, 2, 4, 5, 6, 7}},
};
static_assert(sizeof(kHdmiLayouts) / sizeof(kHdmiLayouts[0]) ==
                  static_cast<size_t>(HdmiLayout::Count),
              "one table row per layout");

inline constexpr HdmiLayout kHdmiLayoutDefault = HdmiLayout::Stereo;

inline const HdmiLayoutInfo& hdmi_layout_info(HdmiLayout l) {
  const auto i = static_cast<size_t>(l);
  return kHdmiLayouts[i < static_cast<size_t>(HdmiLayout::Count) ? i : 1];
}

inline const char* hdmi_layout_name(HdmiLayout l) { return hdmi_layout_info(l).name; }

inline bool parse_hdmi_layout(const std::string& s, HdmiLayout* out) {
  for (uint8_t i = 0; i < static_cast<uint8_t>(HdmiLayout::Count); ++i) {
    if (s == kHdmiLayouts[i].name) {
      *out = static_cast<HdmiLayout>(i);
      return true;
    }
  }
  return false;
}

// The short name a speaker goes by in a layout. Mono's one speaker is not "L".
inline const char* hdmi_speaker_name(HdmiLayout l, unsigned speaker) {
  static constexpr const char* kNames[kHdmiMaxChannels] = {"L",  "R",  "C",  "LFE",
                                                           "Ls", "Rs", "Lb", "Rb"};
  if (l == HdmiLayout::Mono) return "M";
  return speaker < kHdmiMaxChannels ? kNames[speaker] : "?";
}

// The Pi's HDMI audio carries 8 channels at up to 48 kHz and only stereo above that
// (raspberrypi/linux#3933: "8 channel 48kHz PCM, 2 channel 192kHz PCM"), so surround is refused
// at 96 kHz rather than handed to a firmware that would resample or corrupt it.
inline constexpr bool hdmi_layout_rate_ok(HdmiLayout l, unsigned rate) {
  return rate <= 48000 || l == HdmiLayout::Mono || l == HdmiLayout::Stereo;
}

}  // namespace st
