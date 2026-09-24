// The compiled-in board. Its values are what the daemon used before there were profiles, and the
// parts of the daemon that are still sized at compile time have to agree with it. Some of its
// fields are read and some are only declared so far (board_profile.h says which); the two are
// tested apart, so that a pinned value is never mistaken for behaviour the daemon has.
#include "board_profile.h"

#include <string>

#include "audio_backend.h"
#include "check.h"
#include "constants.h"
#include "hdmi_layout.h"

using namespace st;

namespace {

// What the daemon reads: the clock Config and EngineOptions start from, the name --help gives the
// hardware, and each sink's name, device, handoff ring width and missing-device hint.
void test_what_is_read_is_the_pi_as_it_was() {
  const BoardProfile& b = rpi3_octo_profile();
  CHECK_EQ(b.label, std::string("Audio Injector Octo"));
  CHECK_EQ(b.clock.capture_device, std::string("hw:audioinjectoroc,0"));
  CHECK_EQ(b.clock.rate, 96000u);
  CHECK_EQ(b.clock.period, 1024u);
  CHECK_EQ(b.clock.periods, 4u);
  CHECK(!b.clock.capture_slots.empty());
  if (!b.clock.capture_slots.empty()) CHECK_EQ(b.clock.capture_slots.front(), kTdmSlots);

  CHECK_EQ(b.sinks.size(), 2u);
  const SinkProfile* hdmi = b.sink("hdmi");
  const SinkProfile* lineout = b.sink("lineout");
  CHECK(hdmi && lineout);
  CHECK(b.sink("usb") == nullptr);
  CHECK(b.sink("") == nullptr);
  if (!hdmi || !lineout) return;

  // Each exactly as wide as the engine renders it, or the engine would refuse its ring.
  CHECK_EQ(hdmi->device, std::string("hw:b1,0"));
  CHECK_EQ(hdmi->width, kHdmiMaxChannels);
  CHECK_EQ(hdmi->where_hint,
           std::string("the Pi's HDMI audio needs dtparam=audio=on in config.txt; it is hw:b1,0 "
                       "with snd_bcm2835.enable_compat_alsa=0 on the kernel command line, "
                       "hw:ALSA,1 without"));

  CHECK_EQ(lineout->device, std::string("hw:Headphones,0"));
  CHECK_EQ(lineout->width, kLineoutChannels);
  CHECK_EQ(lineout->where_hint,
           std::string("the Pi's 3.5 mm jack needs dtparam=audio=on in config.txt; it is "
                       "hw:Headphones,0 with snd_bcm2835.enable_compat_alsa=0 on the kernel "
                       "command line, hw:ALSA,0 without"));
}

// What is only declared. Nothing acts on these yet: the engine picks its clock from --sim, falls
// back to kInputs capture slots on its own, sets the mixer to 0 dB whatever the policy, and the
// console offers the channel map and the sync watch everywhere. They are pinned as the Pi's own
// values, the ones a profile-driven daemon has to start from, and not as anything it does now.
void test_what_is_declared_is_the_pi_as_it_is() {
  const BoardProfile& b = rpi3_octo_profile();
  CHECK_EQ(b.id, std::string("rpi3-octo"));
  CHECK(b.clock.kind == ClockKind::LinkedDuplex);
  CHECK_EQ(b.clock.playback_device, b.clock.capture_device);
  CHECK_EQ(b.clock.capture_slots.size(), 2u);
  if (b.clock.capture_slots.size() == 2) {
    // Every TDM slot first, then only the ADCs'.
    CHECK_EQ(b.clock.capture_slots[0], kTdmSlots);
    CHECK_EQ(b.clock.capture_slots[1], kInputs);
  }
  CHECK(b.mixer == MixerPolicy::AllSimple0dB);
  CHECK_EQ(b.capabilities.channel_map, kTdmSlots);
  CHECK(b.capabilities.sync_watch);

  const SinkProfile* hdmi = b.sink("hdmi");
  const SinkProfile* lineout = b.sink("lineout");
  if (!hdmi || !lineout) return;
  CHECK_EQ(hdmi->surround_max_rate, 48000u);
  CHECK(lineout->layouts.empty());  // stereo, with no layout to set
  CHECK_EQ(lineout->surround_max_rate, 0u);
}

// Also declared, but it has to agree with what is used in its place today: HDMI offers every layout
// the table has, in the table's order, as the API does, and the rate it stops carrying surround at
// is the one hdmi_layout_rate_ok() refuses above with a literal of its own.
void test_hdmi_matches_the_layout_table() {
  const SinkProfile* hdmi = rpi3_octo_profile().sink("hdmi");
  CHECK(hdmi != nullptr);
  if (!hdmi) return;
  CHECK_EQ(hdmi->layouts.size(), static_cast<size_t>(HdmiLayout::Count));
  for (size_t i = 0; i < hdmi->layouts.size(); ++i)
    CHECK_EQ(hdmi->layouts[i], static_cast<HdmiLayout>(i));

  const unsigned top = hdmi->surround_max_rate;
  for (HdmiLayout l : hdmi->layouts) {
    const bool surround = hdmi_layout_info(l).pcm_channels > 2;
    CHECK(hdmi_layout_rate_ok(l, top));
    CHECK_EQ(hdmi_layout_rate_ok(l, top + 1), !surround);
  }
}

// What an engine left alone opens is the board's clock.
void test_the_engine_asks_for_the_boards_clock() {
  const ClockProfile& clock = rpi3_octo_profile().clock;
  const EngineOptions o;
  CHECK(!o.sim);
  CHECK_EQ(o.device, clock.capture_device);
  CHECK_EQ(o.rate, clock.rate);
  CHECK_EQ(o.period, clock.period);
  CHECK_EQ(o.periods, clock.periods);
  CHECK_EQ(o.capture_channels, clock.capture_slots.front());
}

// One profile, however often it is asked for: the sinks keep pointers into its strings.
void test_the_profile_is_one_object() {
  CHECK(&rpi3_octo_profile() == &rpi3_octo_profile());
  CHECK(rpi3_octo_profile().sink("hdmi") == rpi3_octo_profile().sink("hdmi"));
}

}  // namespace

int main() {
  test_what_is_read_is_the_pi_as_it_was();
  test_what_is_declared_is_the_pi_as_it_is();
  test_hdmi_matches_the_layout_table();
  test_the_engine_asks_for_the_boards_clock();
  test_the_profile_is_one_object();
  return report("board_profile");
}
