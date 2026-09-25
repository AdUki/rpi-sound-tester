#pragma once

#include <string>
#include <vector>

namespace st {

// One PCM device, as `aplay -l` / `arecord -l` list it.
struct PcmDevice {
  // "<card id>,<device>": what config.json and the API call it. Card ids come from the driver
  // ("b1", "Headphones", "Device" for a USB class device), so unlike card numbers they do not
  // change with probe order or when a USB device is plugged into another port.
  std::string id;
  std::string alsa;       // "hw:CARD=<card id>,DEV=<device>": what it is opened as
  std::string card_id;
  std::string card_name;  // the driver's name for the card: "bcm2835 HDMI 1"
  std::string pcm_name;   // the driver's name for the device: "USB Audio"
  unsigned device = 0;
  bool playback = false;
  bool capture = false;
  bool usb = false;
};

// Every PCM device of every card ALSA has right now.
std::vector<PcmDevice> scan_pcm_devices();

// The device id an ALSA name opens: "hw:b1,0", "hw:CARD=b1,DEV=0" and "plughw:b1" are all "b1,0".
// A card given by number is looked up, since only its id is stable. "" for a name that is not a
// hardware device (default, a plugin chain), or a card that does not exist.
std::string pcm_device_id(const std::string& alsa_name);
// The card id part of a device id: "b1" of "b1,0".
std::string pcm_card_id(const std::string& device_id);

// Whether a driver calls a device HDMI (the Pi's "bcm2835 HDMI 1", a PC's "HDMI 0", vc4-hdmi). A
// driver that does not say so needs a board hint.
bool pcm_looks_hdmi(const PcmDevice& d);

// What a device accepts in one direction, found by opening it. `probed` is false when it could
// not be opened (busy, gone): the defaults then stand, and opening it for real says what is wrong.
struct PcmCaps {
  bool probed = false;
  unsigned min_channels = 1;
  unsigned max_channels = 2;
  std::vector<unsigned> rates;  // of kSinkRates; all of them when not probed
};
PcmCaps probe_pcm(const std::string& alsa_name, bool capture);

}  // namespace st
