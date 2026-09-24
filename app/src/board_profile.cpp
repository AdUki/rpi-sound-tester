#include "board_profile.h"

namespace st {

const SinkProfile* BoardProfile::sink(const std::string& sink_id) const {
  for (const SinkProfile& s : sinks)
    if (s.id == sink_id) return &s;
  return nullptr;
}

const BoardProfile& rpi3_octo_profile() {
  static const BoardProfile profile = [] {
    BoardProfile b;
    b.id = "rpi3-octo";
    b.label = "Audio Injector Octo";

    // The Octo's CS42448: 6 ADCs and 8 DACs on one card, capture and playback linked, S32_LE in
    // 8-slot TDM frames. The machine driver raises capture's channels_max to 8 only while a stream
    // runs, so capture asks for the 8 slots and falls back to the 6 ADCs' when it cannot have them.
    b.clock.kind = ClockKind::LinkedDuplex;
    b.clock.capture_device = "hw:audioinjectoroc,0";
    b.clock.playback_device = "hw:audioinjectoroc,0";
    b.clock.rate = 96000;
    b.clock.period = 1024;
    b.clock.periods = 4;
    b.clock.capture_slots = {8, 6};

    // The firmware driver calls the first HDMI port's card "b1" and the 3.5 mm jack's "Headphones"
    // once snd_bcm2835.enable_compat_alsa=0 is on the kernel command line; without it both are
    // devices of one card called "ALSA", HDMI 1 and the jack 0.
    //
    // HDMI carries 8 channels at up to 48 kHz and only stereo above that (raspberrypi/linux#3933:
    // "8 channel 48kHz PCM, 2 channel 192kHz PCM"). The jack is stereo and nothing else.
    SinkProfile hdmi;
    hdmi.id = "hdmi";
    hdmi.device = "hw:b1,0";
    hdmi.width = 8;
    hdmi.layouts = {HdmiLayout::Mono, HdmiLayout::Stereo, HdmiLayout::S51, HdmiLayout::S71};
    hdmi.surround_max_rate = 48000;
    hdmi.where_hint =
        "the Pi's HDMI audio needs dtparam=audio=on in config.txt; it is hw:b1,0 with "
        "snd_bcm2835.enable_compat_alsa=0 on the kernel command line, hw:ALSA,1 without";

    SinkProfile lineout;
    lineout.id = "lineout";
    lineout.device = "hw:Headphones,0";
    lineout.width = 2;
    lineout.where_hint =
        "the Pi's 3.5 mm jack needs dtparam=audio=on in config.txt; it is hw:Headphones,0 with "
        "snd_bcm2835.enable_compat_alsa=0 on the kernel command line, hw:ALSA,0 without";

    b.sinks = {hdmi, lineout};

    // The CS42448's ADC volumes top out at +24 dB, not at unity: see init_mixer().
    b.mixer = MixerPolicy::AllSimple0dB;

    // Slot rotation after an "I2S SYNC error" is the Octo's own failure, and remapping the TDM
    // slots is what an operator does about it.
    b.capabilities.channel_map = 8;
    b.capabilities.sync_watch = true;
    return b;
  }();
  return profile;
}

}  // namespace st
