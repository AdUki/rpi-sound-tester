#pragma once

#include <ogg/ogg.h>
#include <vorbis/codec.h>

#include <cstdint>
#include <string>
#include <vector>

namespace st {

// Decodes the Vorbis a sender may choose instead of PCM, back into the interleaved float the rest
// of the network input path works in.
//
// Raw packets, not an Ogg stream: our own framing already delivers them whole and in order, which
// is the job Ogg would be doing. The three setup packets arrive once, in CODEC_INIT.
//
// Lossy, and therefore a monitoring and stimulus path rather than a reference one — a measurement
// wants the PCM encoding. Owned by a sender's connection thread, so it may allocate.
class VorbisDecoder {
 public:
  VorbisDecoder() = default;
  ~VorbisDecoder() { clear(); }
  VorbisDecoder(const VorbisDecoder&) = delete;
  VorbisDecoder& operator=(const VorbisDecoder&) = delete;

  bool ready() const { return ready_; }
  int channels() const { return ready_ ? vi_.channels : 0; }
  long rate() const { return ready_ ? vi_.rate : 0; }

  // Feeds the setup packets, in the order the encoder produced them. Anything already decoding is
  // torn down first, so a sender may re-declare its codec mid-connection.
  bool init(const std::vector<std::vector<uint8_t>>& headers, std::string* err) {
    clear();
    if (headers.size() < 3) {
      if (err) *err = "expected three Vorbis setup packets, got " + std::to_string(headers.size());
      return false;
    }
    vorbis_info_init(&vi_);
    vorbis_comment_init(&vc_);
    info_ = true;

    for (size_t i = 0; i < headers.size(); ++i) {
      ogg_packet op{};
      op.packet = const_cast<unsigned char*>(headers[i].data());
      op.bytes = static_cast<long>(headers[i].size());
      op.b_o_s = i == 0 ? 1 : 0;   // only the identification header may claim to begin the stream
      op.packetno = static_cast<ogg_int64_t>(i);
      const int e = vorbis_synthesis_headerin(&vi_, &vc_, &op);
      if (e < 0) {
        if (err) *err = "Vorbis setup packet " + std::to_string(i) + " rejected (" +
                        std::to_string(e) + ")";
        clear();
        return false;
      }
    }
    if (vorbis_synthesis_init(&vd_, &vi_) != 0) {
      if (err) *err = "Vorbis synthesis would not start";
      clear();
      return false;
    }
    dsp_ = true;
    vorbis_block_init(&vd_, &vb_);
    block_ = true;
    ready_ = true;
    return true;
  }

  // Decodes one packet, appending interleaved float. Returns frames appended; a packet legitimately
  // yields none while the decoder primes.
  size_t decode(const uint8_t* data, size_t len, std::vector<float>* out, std::string* err) {
    if (!ready_) return 0;
    ogg_packet op{};
    op.packet = const_cast<unsigned char*>(data);
    op.bytes = static_cast<long>(len);
    op.packetno = packetno_++;

    if (vorbis_synthesis(&vb_, &op) != 0) {
      // A packet the decoder cannot make sense of is dropped rather than fatal: the stream
      // recovers on the next one, and killing the connection would be a worse answer.
      if (err) *err = "undecodable Vorbis packet";
      return 0;
    }
    vorbis_synthesis_blockin(&vd_, &vb_);

    const size_t before = out->size();
    float** pcm = nullptr;
    int n = 0;
    while ((n = vorbis_synthesis_pcmout(&vd_, &pcm)) > 0) {
      // pcmout hands back one buffer per channel; the rest of the path is interleaved.
      const size_t base = out->size();
      out->resize(base + static_cast<size_t>(n) * vi_.channels);
      for (int c = 0; c < vi_.channels; ++c) {
        const float* src = pcm[c];
        for (int i = 0; i < n; ++i) out->at(base + static_cast<size_t>(i) * vi_.channels + c) = src[i];
      }
      vorbis_synthesis_read(&vd_, n);
    }
    return (out->size() - before) / (vi_.channels ? vi_.channels : 1);
  }

  void clear() {
    if (block_) vorbis_block_clear(&vb_);
    if (dsp_) vorbis_dsp_clear(&vd_);
    if (info_) {
      vorbis_comment_clear(&vc_);
      vorbis_info_clear(&vi_);
    }
    block_ = dsp_ = info_ = ready_ = false;
    packetno_ = 3;  // the setup packets are 0..2
  }

 private:
  vorbis_info vi_{};
  vorbis_comment vc_{};
  vorbis_dsp_state vd_{};
  vorbis_block vb_{};
  bool info_ = false, dsp_ = false, block_ = false, ready_ = false;
  ogg_int64_t packetno_ = 3;
};

}  // namespace st
