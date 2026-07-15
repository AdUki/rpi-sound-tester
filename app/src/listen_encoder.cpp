#include "listen_encoder.h"

#include <algorithm>
#include <array>
#include <cstring>

#include <ogg/ogg.h>
#include <opus.h>
#include <opus_multistream.h>

#include "util/resample.h"

namespace st {

namespace {

constexpr int kMaxMonoPacket = 4000;  // one 20 ms mono Opus frame fits well under this

int clamp_kbps(int kbps) {
  return std::clamp(kbps, kListenBitrateMinKbps, kListenBitrateMaxKbps);
}

void put_u16le(std::string* s, uint16_t v) {
  s->push_back(static_cast<char>(v & 0xff));
  s->push_back(static_cast<char>((v >> 8) & 0xff));
}

void put_u32le(std::string* s, uint32_t v) {
  for (int i = 0; i < 4; ++i) s->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

// OpusHead identification header, channel mapping family 255 (channels independent, uncoupled).
std::string make_opus_head(unsigned channels, int preskip, unsigned input_rate) {
  std::string h = "OpusHead";
  h.push_back(1);                                 // version
  h.push_back(static_cast<char>(channels));       // channel count
  put_u16le(&h, static_cast<uint16_t>(preskip));  // pre-skip
  put_u32le(&h, input_rate);                      // original input sample rate (informational)
  put_u16le(&h, 0);                               // output gain
  h.push_back(static_cast<char>(255));            // channel mapping family
  h.push_back(static_cast<char>(channels));       // stream count
  h.push_back(0);                                 // coupled stream count
  for (unsigned c = 0; c < channels; ++c) h.push_back(static_cast<char>(c));  // mapping table
  return h;
}

std::string make_opus_tags(const std::string& vendor) {
  std::string t = "OpusTags";
  put_u32le(&t, static_cast<uint32_t>(vendor.size()));
  t += vendor;
  put_u32le(&t, 0);  // user comment count
  return t;
}

void append_page(const ogg_page& og, std::string* out) {
  out->append(reinterpret_cast<const char*>(og.header), static_cast<size_t>(og.header_len));
  out->append(reinterpret_cast<const char*>(og.body), static_cast<size_t>(og.body_len));
}

void flush_pages(ogg_stream_state* os, std::string* out) {
  ogg_page og;
  while (ogg_stream_flush(os, &og)) append_page(og, out);
}

void page_out(ogg_stream_state* os, std::string* out) {
  ogg_page og;
  while (ogg_stream_pageout(os, &og)) append_page(og, out);
}

}  // namespace

// ---- OpusMonoEncoder ---------------------------------------------------------------------

struct OpusMonoEncoder::Impl {
  OpusEncoder* enc = nullptr;
  std::unique_ptr<Decimator> dec;
  std::vector<float> down;
  int cur_kbps = 0;
  ~Impl() {
    if (enc) opus_encoder_destroy(enc);
  }
};

OpusMonoEncoder::OpusMonoEncoder(unsigned rate, int bitrate_kbps, bool* ok) {
  *ok = false;
  if (!opus_rate_supported(rate)) return;
  d_ = std::make_unique<Impl>();
  const unsigned factor = rate / kOpusRate;
  in_frames_ = kOpusFrameFrames * factor;
  d_->dec = std::make_unique<Decimator>(factor);

  int err = 0;
  d_->enc = opus_encoder_create(kOpusRate, 1, OPUS_APPLICATION_AUDIO, &err);
  if (err != OPUS_OK || !d_->enc) return;

  const int kbps = clamp_kbps(bitrate_kbps);
  opus_encoder_ctl(d_->enc, OPUS_SET_BITRATE(kbps * 1000));
  opus_encoder_ctl(d_->enc, OPUS_SET_VBR(0));  // CBR: predictable rate, no silence dropping
  opus_encoder_ctl(d_->enc, OPUS_SET_DTX(0));  // never drop silent frames -> channels stay aligned
  opus_encoder_ctl(d_->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
  d_->cur_kbps = kbps;
  *ok = true;
}

OpusMonoEncoder::~OpusMonoEncoder() = default;

bool OpusMonoEncoder::encode(const float* pcm, int bitrate_kbps, std::vector<uint8_t>* packet) {
  const int kbps = clamp_kbps(bitrate_kbps);
  if (kbps != d_->cur_kbps) {
    opus_encoder_ctl(d_->enc, OPUS_SET_BITRATE(kbps * 1000));
    d_->cur_kbps = kbps;
  }
  d_->dec->process(pcm, in_frames_, &d_->down);
  packet->resize(kMaxMonoPacket);
  const opus_int32 n = opus_encode_float(d_->enc, d_->down.data(), kOpusFrameFrames, packet->data(),
                                         static_cast<opus_int32>(packet->size()));
  if (n < 0) return false;
  packet->resize(static_cast<size_t>(n));
  return true;
}

void OpusMonoEncoder::reset() {
  opus_encoder_ctl(d_->enc, OPUS_RESET_STATE);
  d_->dec->reset();
}

// ---- OpusOggMultiEncoder -----------------------------------------------------------------

struct OpusOggMultiEncoder::Impl {
  OpusMSEncoder* enc = nullptr;
  ogg_stream_state os;
  bool os_init = false;
  std::vector<std::unique_ptr<Decimator>> decs;
  std::vector<float> chan;                    // one channel, engine rate
  std::vector<std::vector<float>> downs;      // per channel, 48 kHz
  std::vector<float> down;                    // re-interleaved 48 kHz
  std::vector<unsigned char> pkt;
  int cur_kbps = 0;
  int preskip = 0;
  int64_t granule = 0;
  int64_t packetno = 2;  // 0 = OpusHead, 1 = OpusTags
  ~Impl() {
    if (enc) opus_multistream_encoder_destroy(enc);
    if (os_init) ogg_stream_clear(&os);
  }
};

OpusOggMultiEncoder::OpusOggMultiEncoder(unsigned rate, int bitrate_kbps, uint32_t serial,
                                         bool* ok) {
  *ok = false;
  if (!opus_rate_supported(rate)) return;
  d_ = std::make_unique<Impl>();
  const unsigned factor = rate / kOpusRate;
  in_frames_ = kOpusFrameFrames * factor;

  std::array<unsigned char, kInputs> mapping;
  for (unsigned c = 0; c < kInputs; ++c) mapping[c] = static_cast<unsigned char>(c);

  int err = 0;
  d_->enc = opus_multistream_encoder_create(kOpusRate, kInputs, kInputs, 0, mapping.data(),
                                            OPUS_APPLICATION_AUDIO, &err);
  if (err != OPUS_OK || !d_->enc) return;

  const int kbps = clamp_kbps(bitrate_kbps);
  opus_multistream_encoder_ctl(d_->enc, OPUS_SET_BITRATE(kbps * 1000 * static_cast<int>(kInputs)));
  opus_multistream_encoder_ctl(d_->enc, OPUS_SET_VBR(0));
  opus_multistream_encoder_ctl(d_->enc, OPUS_SET_DTX(0));
  opus_multistream_encoder_ctl(d_->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
  d_->cur_kbps = kbps;

  opus_int32 lookahead = 0;
  opus_multistream_encoder_ctl(d_->enc, OPUS_GET_LOOKAHEAD(&lookahead));
  d_->preskip = lookahead;

  for (unsigned c = 0; c < kInputs; ++c) d_->decs.push_back(std::make_unique<Decimator>(factor));
  d_->chan.resize(in_frames_);
  d_->downs.assign(kInputs, {});
  d_->down.resize(static_cast<size_t>(kOpusFrameFrames) * kInputs);
  d_->pkt.resize(static_cast<size_t>(kInputs) * 1500);

  if (ogg_stream_init(&d_->os, static_cast<int>(serial)) != 0) return;
  d_->os_init = true;
  *ok = true;
}

OpusOggMultiEncoder::~OpusOggMultiEncoder() = default;

std::string OpusOggMultiEncoder::headers() {
  std::string out;

  const std::string head = make_opus_head(kInputs, d_->preskip, kOpusRate);
  ogg_packet op{};
  op.packet = reinterpret_cast<unsigned char*>(const_cast<char*>(head.data()));
  op.bytes = static_cast<long>(head.size());
  op.b_o_s = 1;
  op.granulepos = 0;
  op.packetno = 0;
  ogg_stream_packetin(&d_->os, &op);
  flush_pages(&d_->os, &out);  // OpusHead must be alone on the first page

  const std::string tags = make_opus_tags("soundtesterd");
  ogg_packet opt{};
  opt.packet = reinterpret_cast<unsigned char*>(const_cast<char*>(tags.data()));
  opt.bytes = static_cast<long>(tags.size());
  opt.granulepos = 0;
  opt.packetno = 1;
  ogg_stream_packetin(&d_->os, &opt);
  flush_pages(&d_->os, &out);

  return out;
}

bool OpusOggMultiEncoder::encode(const float* interleaved, int bitrate_kbps, std::string* out) {
  const int kbps = clamp_kbps(bitrate_kbps);
  if (kbps != d_->cur_kbps) {
    opus_multistream_encoder_ctl(d_->enc,
                                 OPUS_SET_BITRATE(kbps * 1000 * static_cast<int>(kInputs)));
    d_->cur_kbps = kbps;
  }

  for (unsigned c = 0; c < kInputs; ++c) {
    for (unsigned i = 0; i < in_frames_; ++i) d_->chan[i] = interleaved[i * kInputs + c];
    d_->decs[c]->process(d_->chan.data(), in_frames_, &d_->downs[c]);
  }
  for (unsigned i = 0; i < kOpusFrameFrames; ++i) {
    for (unsigned c = 0; c < kInputs; ++c) d_->down[i * kInputs + c] = d_->downs[c][i];
  }

  const opus_int32 n = opus_multistream_encode_float(
      d_->enc, d_->down.data(), kOpusFrameFrames, d_->pkt.data(),
      static_cast<opus_int32>(d_->pkt.size()));
  if (n < 0) return false;

  d_->granule += kOpusFrameFrames;
  ogg_packet op{};
  op.packet = d_->pkt.data();
  op.bytes = n;
  op.granulepos = d_->granule;
  op.packetno = d_->packetno++;
  if (ogg_stream_packetin(&d_->os, &op) != 0) return false;
  page_out(&d_->os, out);
  return true;
}

void OpusOggMultiEncoder::reset() {
  opus_multistream_encoder_ctl(d_->enc, OPUS_RESET_STATE);
  for (auto& dec : d_->decs) dec->reset();
}

}  // namespace st
