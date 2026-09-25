#include "alsa_devices.h"

#include <alsa/asoundlib.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "constants.h"

namespace st {

namespace {

bool has_hdmi(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s.find("hdmi") != std::string::npos;
}

// The id of card number `card`, or "".
std::string card_id_of(int card) {
  snd_ctl_t* ctl = nullptr;
  const std::string hw = "hw:" + std::to_string(card);
  if (snd_ctl_open(&ctl, hw.c_str(), 0) < 0) return {};
  snd_ctl_card_info_t* info;
  snd_ctl_card_info_alloca(&info);
  std::string id;
  if (snd_ctl_card_info(ctl, info) == 0) id = snd_ctl_card_info_get_id(info);
  snd_ctl_close(ctl);
  return id;
}

}  // namespace

std::vector<PcmDevice> scan_pcm_devices() {
  std::vector<PcmDevice> out;
  int card = -1;
  while (snd_card_next(&card) == 0 && card >= 0) {
    snd_ctl_t* ctl = nullptr;
    const std::string hw = "hw:" + std::to_string(card);
    if (snd_ctl_open(&ctl, hw.c_str(), 0) < 0) continue;
    snd_ctl_card_info_t* info;
    snd_ctl_card_info_alloca(&info);
    if (snd_ctl_card_info(ctl, info) < 0) {
      snd_ctl_close(ctl);
      continue;
    }
    const std::string card_id = snd_ctl_card_info_get_id(info);
    const std::string card_name = snd_ctl_card_info_get_name(info);
    const bool usb = access(("/proc/asound/card" + std::to_string(card) + "/usbid").c_str(), F_OK) == 0;

    int dev = -1;
    while (snd_ctl_pcm_next_device(ctl, &dev) == 0 && dev >= 0) {
      PcmDevice d;
      d.card_id = card_id;
      d.card_name = card_name;
      d.device = static_cast<unsigned>(dev);
      d.id = card_id + "," + std::to_string(dev);
      d.alsa = "hw:CARD=" + card_id + ",DEV=" + std::to_string(dev);
      d.usb = usb;
      for (const snd_pcm_stream_t stream : {SND_PCM_STREAM_PLAYBACK, SND_PCM_STREAM_CAPTURE}) {
        snd_pcm_info_t* pi;
        snd_pcm_info_alloca(&pi);
        snd_pcm_info_set_device(pi, static_cast<unsigned>(dev));
        snd_pcm_info_set_subdevice(pi, 0);
        snd_pcm_info_set_stream(pi, stream);
        if (snd_ctl_pcm_info(ctl, pi) < 0) continue;
        (stream == SND_PCM_STREAM_PLAYBACK ? d.playback : d.capture) = true;
        if (d.pcm_name.empty()) d.pcm_name = snd_pcm_info_get_name(pi);
      }
      if (d.playback || d.capture) out.push_back(d);
    }
    snd_ctl_close(ctl);
  }
  return out;
}

std::string pcm_device_id(const std::string& alsa_name) {
  std::string s;
  if (alsa_name.rfind("plughw:", 0) == 0) {
    s = alsa_name.substr(7);
  } else if (alsa_name.rfind("hw:", 0) == 0) {
    s = alsa_name.substr(3);
  } else {
    return {};
  }
  // "CARD=x,DEV=n", "x,n" or "x", in any mix.
  std::string card, dev = "0";
  size_t pos = 0;
  for (int field = 0; pos <= s.size(); ++field) {
    const size_t comma = s.find(',', pos);
    std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    if (tok.compare(0, 5, "CARD=") == 0) {
      card = tok.substr(5);
    } else if (tok.compare(0, 4, "DEV=") == 0) {
      dev = tok.substr(4);
    } else if (field == 0) {
      card = tok;
    } else if (field == 1) {
      dev = tok;
    }
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  if (card.empty() || dev.empty()) return {};
  if (std::all_of(card.begin(), card.end(), [](unsigned char c) { return std::isdigit(c); })) {
    card = card_id_of(std::atoi(card.c_str()));
    if (card.empty()) return {};
  }
  return card + "," + dev;
}

std::string pcm_card_id(const std::string& device_id) {
  return device_id.substr(0, device_id.find(','));
}

bool pcm_looks_hdmi(const PcmDevice& d) {
  return has_hdmi(d.card_name) || has_hdmi(d.pcm_name) || has_hdmi(d.card_id);
}

PcmCaps probe_pcm(const std::string& alsa_name, bool capture) {
  PcmCaps caps;
  caps.rates.assign(std::begin(kSinkRates), std::end(kSinkRates));
  snd_pcm_t* pcm = nullptr;
  if (snd_pcm_open(&pcm, alsa_name.c_str(),
                   capture ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0)
    return caps;
  snd_pcm_hw_params_t* hw;
  snd_pcm_hw_params_alloca(&hw);
  if (snd_pcm_hw_params_any(pcm, hw) >= 0) {
    unsigned lo = 0, hi = 0;
    if (snd_pcm_hw_params_get_channels_min(hw, &lo) >= 0 &&
        snd_pcm_hw_params_get_channels_max(hw, &hi) >= 0 && lo >= 1 && hi >= lo) {
      caps.min_channels = lo;
      caps.max_channels = hi;
    }
    caps.rates.clear();
    for (unsigned r : kSinkRates)
      if (snd_pcm_hw_params_test_rate(pcm, hw, r, 0) == 0) caps.rates.push_back(r);
    if (caps.rates.empty()) caps.rates.assign(std::begin(kSinkRates), std::end(kSinkRates));
    caps.probed = true;
  }
  snd_pcm_close(pcm);
  return caps;
}

}  // namespace st
