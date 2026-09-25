// What the image says about a board (board.json), how a device is named, and which layouts a
// device found at runtime is offered.
#include "board.h"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "alsa_devices.h"
#include "channel_layout.h"
#include "check.h"
#include "devices.h"
#include "sink_layout.h"
#include "sink_out.h"

using namespace st;

namespace {

std::string write_temp(const std::string& text) {
  char path[] = "/tmp/st_board_XXXXXX";
  const int fd = mkstemp(path);
  if (fd >= 0) close(fd);
  std::ofstream(path) << text;
  return path;
}

// Every board file the images ship parses, and says what its board is.
void test_the_shipped_boards() {
  Board pi, vim3l;
  std::string err;
  CHECK(load_board(ST_BOARDS_DIR "/rpi3-octo.json", &pi, &err));
  CHECK(load_board(ST_BOARDS_DIR "/vim3l.json", &vim3l, &err));
  CHECK_EQ(err, std::string());

  CHECK_EQ(pi.engine_device, std::string("hw:audioinjectoroc,0"));
  CHECK_EQ(pi.rate, 96000u);
  CHECK_EQ(pi.period, 1024u);
  CHECK_EQ(pi.capture_channels, 8u);
  CHECK(pi.hint("b1,0") && pi.hint("b1,0")->label == "HDMI");

  // No engine card: a timer paces it, and the HDMI output is a sink like any other.
  CHECK(vim3l.engine_device.empty());
  CHECK_EQ(vim3l.rate, 48000u);
  // Room for the HDMI loopback and a stereo USB interface; the Pi's default, one stereo device.
  CHECK_EQ(vim3l.device_inputs, 4u);
  CHECK_EQ(pi.device_inputs, 2u);
  const DeviceHint* hdmi = vim3l.hint("G12BKHADASVIM3L,0");
  CHECK(hdmi && hdmi->hdmi && !hdmi->hidden);
  CHECK(vim3l.hint("G12BKHADASVIM3L,1") && vim3l.hint("G12BKHADASVIM3L,1")->hidden);
  CHECK(vim3l.hint("Device,0") == nullptr);
}

// No board file is a desktop: no engine card, no hints. A broken one is an error.
void test_missing_and_broken_files() {
  Board b;
  b.rate = 1;
  CHECK(load_board("/nonexistent/board.json", &b, nullptr));
  CHECK(b.engine_device.empty());
  CHECK_EQ(b.rate, 48000u);
  CHECK(b.devices.empty());

  const std::string bad = write_temp("{ \"rate\": ");
  std::string err;
  CHECK(!load_board(bad, &b, &err));
  CHECK(!err.empty());
  std::remove(bad.c_str());
}

// Every spelling of a hardware device ALSA accepts names the same device id.
void test_device_ids() {
  CHECK_EQ(pcm_device_id("hw:b1,0"), std::string("b1,0"));
  CHECK_EQ(pcm_device_id("hw:CARD=b1,DEV=0"), std::string("b1,0"));
  CHECK_EQ(pcm_device_id("hw:Headphones"), std::string("Headphones,0"));
  CHECK_EQ(pcm_device_id("plughw:CARD=Device,DEV=1"), std::string("Device,1"));
  CHECK_EQ(pcm_device_id("hw:DEV=3,CARD=G12BKHADASVIM3L"), std::string("G12BKHADASVIM3L,3"));
  CHECK_EQ(pcm_device_id("default"), std::string());
  CHECK_EQ(pcm_device_id("hw:"), std::string());
  CHECK_EQ(pcm_card_id("b1,0"), std::string("b1"));
  CHECK_EQ(pcm_card_id(pcm_device_id("hw:audioinjectoroc,0")), std::string("audioinjectoroc"));
}

void test_hdmi_is_recognised_by_name() {
  PcmDevice d;
  d.card_name = "bcm2835 HDMI 1";
  CHECK(pcm_looks_hdmi(d));
  d.card_name = "HDA Intel PCH";
  d.pcm_name = "HDMI 0";
  CHECK(pcm_looks_hdmi(d));
  d.card_name = "G12B-KHADAS-VIM3L";
  d.pcm_name = "fe.dai-link-0 (*)";
  CHECK(!pcm_looks_hdmi(d));  // board.json says so instead
}

bool has(const std::vector<SinkLayout>& v, SinkLayout l) {
  return std::find(v.begin(), v.end(), l) != v.end();
}

// HDMI gets its speaker layouts; anything else stereo and its own width, in its own order.
void test_layouts_offered() {
  const auto hdmi = sink_layouts(true, 1, 8);
  CHECK_EQ(hdmi.size(), 4u);
  CHECK(has(hdmi, SinkLayout::Mono) && has(hdmi, SinkLayout::Stereo));
  CHECK(has(hdmi, SinkLayout::S51) && has(hdmi, SinkLayout::S71));

  const auto usb = sink_layouts(false, 1, 2);
  CHECK_EQ(usb.size(), 1u);
  CHECK(has(usb, SinkLayout::Stereo));

  const auto wide = sink_layouts(false, 2, 10);  // a 10-out interface: 8 is all a sink has
  CHECK_EQ(wide.size(), 2u);
  CHECK(has(wide, SinkLayout::Stereo) && has(wide, SinkLayout::Ch8));
  CHECK_EQ(std::string(sink_speaker_name(SinkLayout::Ch8, 2)), std::string("3"));
  CHECK_EQ(std::string(sink_speaker_name(SinkLayout::S51, 2)), std::string("C"));

  const auto mono = sink_layouts(false, 1, 1);
  CHECK_EQ(mono.size(), 1u);
  CHECK(has(mono, SinkLayout::Ch1));

  // A device that reports something unusable is still offered stereo, and its open says why.
  CHECK(has(sink_layouts(false, 12, 16), SinkLayout::Stereo));
}

// A saved sink's layout and rate stand where the device offers them; anything else falls back to
// the device's defaults, and HDMI surround never goes above 48 kHz.
void test_saved_settings_meet_the_device() {
  SinkDevice hdmi;
  hdmi.hdmi = true;
  hdmi.layouts = sink_layouts(true, 1, 8);
  hdmi.rates = {44100, 48000, 96000};
  SinkDevice usb;
  usb.layouts = sink_layouts(false, 2, 2);
  usb.rates = {44100, 48000};

  SinkConfig c;  // nothing saved
  CHECK_EQ(sink_start(hdmi, c).layout, SinkLayout::Stereo);
  CHECK_EQ(sink_start(hdmi, c).rate, 48000u);

  c.layout = "5.1";
  c.sample_rate = 44100;
  CHECK_EQ(sink_start(hdmi, c).layout, SinkLayout::S51);
  CHECK_EQ(sink_start(hdmi, c).rate, 44100u);
  // The same file on a stereo USB device: its own defaults.
  CHECK_EQ(sink_start(usb, c).layout, SinkLayout::Stereo);
  CHECK_EQ(sink_start(usb, c).rate, 44100u);

  c.layout = "7.1";
  c.sample_rate = 96000;
  CHECK_EQ(sink_start(hdmi, c).layout, SinkLayout::S71);
  CHECK_EQ(sink_start(hdmi, c).rate, 48000u);
  c.layout = "stereo";
  CHECK_EQ(sink_start(hdmi, c).rate, 96000u);
  CHECK_EQ(sink_start(usb, c).rate, 48000u);  // not one of its rates
  c.layout = "bogus";
  CHECK_EQ(sink_start(hdmi, c).layout, SinkLayout::Stereo);
}

// What each PCM format puts on the wire, and mono on both sides; and what comes back off it.
void test_formats() {
  const float in[2] = {1.0f, -0.5f};
  uint8_t out[16] = {};
  pcm_from_float(in, 1, 2, 2, PcmFormat::S16_LE, out);
  CHECK_EQ(static_cast<int16_t>(out[0] | out[1] << 8), 32767);
  pcm_from_float(in, 1, 2, 2, PcmFormat::S24_3LE, out);
  const int32_t l24 = out[0] | out[1] << 8 | out[2] << 16;
  const int32_t r24 = static_cast<int32_t>((out[3] | out[4] << 8 | out[5] << 16) << 8) >> 8;
  CHECK_EQ(l24, 0x7fffff);
  CHECK_EQ(r24, -0x400000);
  pcm_from_float(in + 1, 1, 1, 2, PcmFormat::S32_LE, out);  // mono: both sides
  int32_t a = 0, b = 0;
  std::memcpy(&a, out, 4);
  std::memcpy(&b, out + 4, 4);
  CHECK_EQ(a, b);
  CHECK(a < -0x3fffff00 && a > -0x40000100);

  // Every format reads back what it was written to within two of its last bit: written at
  // full scale 2^n-1 (clamped), read at 2^n.
  const float wave[5] = {0.0f, 0.25f, -0.25f, 0.999f, -1.0f};
  for (const PcmFormat f : kPcmFormats) {
    uint8_t pcm[5 * 4] = {};
    float back[5] = {};
    pcm_from_float(wave, 5, 1, 1, f, pcm);
    pcm_to_float(pcm, 5, f, back);
    const float lsb = f == PcmFormat::S16_LE ? 1.0f / 32768 : 1.0f / 8388608;
    bool close = true;
    for (int i = 0; i < 5; ++i) close &= std::fabs(back[i] - wave[i]) <= 2 * lsb;
    CHECK(close);
  }
  // S24_LE's padding byte is not part of the sample.
  const uint8_t padded[4] = {0x00, 0x00, 0x40, 0xff};
  float v = 0.0f;
  pcm_to_float(padded, 1, PcmFormat::S24_LE, &v);
  CHECK_EQ(v, 0.5f);
}

// A capture device takes two columns where it can do two, one if it has only one, and what it
// must if it cannot go below more.
void test_input_columns() {
  PcmCaps c;
  c.min_channels = 1;
  c.max_channels = 2;
  CHECK_EQ(input_columns(c), 2u);
  c.max_channels = 8;
  CHECK_EQ(input_columns(c), 2u);
  c.max_channels = 1;
  CHECK_EQ(input_columns(c), 1u);
  c.min_channels = 4;
  c.max_channels = 4;
  CHECK_EQ(input_columns(c), 4u);
}

// The device-input columns come after the network's, and a device input is an ADC: amplify-only.
void test_device_columns() {
  ChannelLayout l;
  l.local = 6;
  l.device = 2;
  CHECK_EQ(l.total(), 14u);
  CHECK_EQ(l.device_base(), 12u);
  CHECK(l.is_net(6) && l.is_net(11) && !l.is_net(12));
  CHECK(l.is_device(12) && l.is_device(13) && !l.is_device(14) && !l.is_device(11));
  CHECK_EQ(l.gain_min_db(12), kInputGainMinDb);
  CHECK_EQ(l.gain_min_db(6), kNetGainMinDb);
}

}  // namespace

int main() {
  test_the_shipped_boards();
  test_missing_and_broken_files();
  test_device_ids();
  test_hdmi_is_recognised_by_name();
  test_layouts_offered();
  test_saved_settings_meet_the_device();
  test_formats();
  test_input_columns();
  test_device_columns();
  return report("board");
}
