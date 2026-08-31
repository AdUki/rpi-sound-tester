#ifndef ST_NET_PROTO_H
#define ST_NET_PROTO_H

// Wire format for network audio input.
//
// Deliberately plain C: this header is compiled into BOTH the C++ daemon and the C ALSA plugin
// (alsa-plugin/pcm_soundtester.c), so there is exactly one definition of the protocol and no
// second copy to drift out of sync. Keep it free of C++ and of struct-packing tricks — every
// field is read and written through the explicit little-endian helpers below, so alignment and
// padding never enter into it.
//
// The idea in one line: every audio packet names the absolute sample index it is to be heard at,
// on the tester's own capture/playback counter. The receiver drops it into a per-channel timeline
// at exactly that index, so reordering and duplication sort themselves out, loss is a hole, and a
// packet that misses its slot is dropped and counted rather than silently shifting everything
// after it.

#include <stdint.h>
#include <string.h>

#define ST_NET_MAGIC 0x53544e31u /* "STN1" */
#define ST_NET_HEADER_BYTES 8
#define ST_NET_PROTO_VERSION 5u

/* Shared defaults. The C++ side mirrors these into constants.h rather than restating them, so
   there is one number for the plugin and the daemon to disagree about: none. */
#define ST_DEFAULT_PORT 4010
#define ST_DEFAULT_RATE 96000u
#define ST_PACKET_FRAMES 256u

/* Most channels one sender may take. A stereo source asks for 2 and gets two adjacent network
   inputs; the device caps this at however many it actually has. */
#define ST_MAX_STREAM_CHANNELS 8u

/* Message types. 0x08..0x0b carry the mixer, which is a separate connection from the audio one:
   an ALSA control device is opened independently of the PCM, and it must not consume one of the
   device's network input slots just to look at a volume. 0x0c..0x0f carry per-stream setup that
   is only known after the PCM's hw_params. 0x10.. is left for a future capture direction
   (arecord -D soundtester:) so adding it later is not a breaking change. */
#define ST_MSG_HELLO 1u     /* C->S */
#define ST_MSG_HELLO_ACK 2u /* S->C */
/* 3 and 4 were a round-trip clock probe, retired when the device took over anchoring. The numbers
   stay spoken for so a future message cannot quietly reuse them. */
#define ST_MSG_AUDIO 5u     /* C->S */
#define ST_MSG_STATUS 6u    /* S->C */
#define ST_MSG_BYE 7u       /* both  */
#define ST_MSG_CTL_HELLO 8u /* C->S */
#define ST_MSG_CTL_ACK 9u   /* S->C */
#define ST_MSG_SET_MIX 10u  /* C->S */
#define ST_MSG_MIX 11u      /* S->C: current values, pushed whenever they change */
#define ST_MSG_FORMAT 12u   /* C->S: rate, sample format and encoding, after the app's hw_params */
#define ST_MSG_CODEC_INIT 13u /* C->S: a codec's setup packets, before any audio */

/* HELLO_ACK status codes. */
#define ST_HELLO_OK 0u
#define ST_HELLO_BUSY 1u        /* every network channel is already claimed */
#define ST_HELLO_BAD_RATE 2u    /* the tester is not running at the rate the client asked for */
#define ST_HELLO_BAD_FORMAT 3u
#define ST_HELLO_BAD_PROTO 4u
#define ST_HELLO_DISABLED 5u /* network input is switched off on the device */

/* Sample formats on the wire. The device works in float internally and has to convert whatever
   arrives, so carrying the sender's own format costs nothing and saves converting twice — and
   S16 halves the bandwidth of a stream that was 16-bit to begin with. */
#define ST_FMT_S32_LE 0u
#define ST_FMT_S16_LE 1u
#define ST_FMT_S24_3LE 2u
#define ST_FMT_FLOAT_LE 3u

/* Bytes per sample, or 0 for a format this build does not know. */
static inline unsigned st_fmt_bytes(unsigned fmt) {
  switch (fmt) {
    case ST_FMT_S16_LE: return 2;
    case ST_FMT_S24_3LE: return 3;
    case ST_FMT_S32_LE:
    case ST_FMT_FLOAT_LE: return 4;
    default: return 0;
  }
}

#define ST_NET_MAX_NAME 64u

/* ---- little-endian primitives ------------------------------------------------------------ */

static inline void st_put_u16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
}
static inline void st_put_u32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}
static inline void st_put_u64(uint8_t* p, uint64_t v) {
  st_put_u32(p, (uint32_t)(v & 0xffffffffu));
  st_put_u32(p + 4, (uint32_t)((v >> 32) & 0xffffffffu));
}
static inline uint16_t st_get_u16(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t st_get_u32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t st_get_u64(const uint8_t* p) {
  return (uint64_t)st_get_u32(p) | ((uint64_t)st_get_u32(p + 4) << 32);
}

/* One sample of a wire format as a float in [-1, 1).
   Shared, because both ends need exactly this mapping: the device to unpack what arrives, and the
   sender to hand float to an encoder. Two copies of it would be two chances to get S24's sign
   extension wrong in only one of them. */
static inline float st_sample_to_float(const uint8_t *p, unsigned fmt) {
  switch (fmt) {
    case ST_FMT_S16_LE:
      return (float)(int16_t)st_get_u16(p) * (1.0f / 32768.0f);
    case ST_FMT_S24_3LE: {
      /* Sign-extended by landing the three bytes in the TOP of an int32 and letting the shift
         carry the sign, rather than by masking and testing a bit. */
      int32_t v = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24));
      return (float)v * (1.0f / 2147483648.0f);
    }
    case ST_FMT_FLOAT_LE: {
      float f;
      uint32_t bits = st_get_u32(p);
      memcpy(&f, &bits, sizeof(f));
      return f;
    }
    default:
      return (float)(int32_t)st_get_u32(p) * (1.0f / 2147483648.0f);
  }
}

/* ---- framing ----------------------------------------------------------------------------- */

/* [u8 type][u8 flags][u16 reserved][u32 payload_len] */
static inline void st_put_header(uint8_t* p, uint8_t type, uint32_t payload_len) {
  p[0] = type;
  p[1] = 0;
  st_put_u16(p + 2, 0);
  st_put_u32(p + 4, payload_len);
}
static inline uint8_t st_header_type(const uint8_t* p) { return p[0]; }
static inline uint32_t st_header_len(const uint8_t* p) { return st_get_u32(p + 4); }

/* ---- payload layouts --------------------------------------------------------------------- */
/* Offsets are spelled out rather than expressed as structs so that the two languages, and any
   future third reader of this protocol, cannot disagree about padding. */

/* HELLO: magic, proto, rate, format, channels, want_channel, name_len, name[] */
#define ST_HELLO_FIXED 24u
#define ST_HELLO_O_MAGIC 0u
#define ST_HELLO_O_PROTO 4u
#define ST_HELLO_O_RATE 8u
#define ST_HELLO_O_FORMAT 12u
#define ST_HELLO_O_CHANNELS 16u  /* u16 */
#define ST_HELLO_O_WANT_CH 18u   /* u16; 0xffff = "any free channel" */
#define ST_HELLO_O_NAME_LEN 20u  /* u32, name bytes follow */
#define ST_HELLO_ANY_CHANNEL 0xffffu

/* HELLO_ACK: proto, status, rate, granted_channel, max_lead_frames, granted_channels.
   `granted_channel` is the FIRST of `granted_channels` adjacent inputs — a multi-channel sender
   always lands on a contiguous run, so NET 3+4 reads as one stereo source rather than two
   unrelated ones. It carried the device's clock too, until the device took over deciding playout
   and the sender stopped having any use for it. */
#define ST_HELLO_ACK_BYTES 24u
#define ST_ACK_O_PROTO 0u
#define ST_ACK_O_STATUS 4u
#define ST_ACK_O_RATE 8u
#define ST_ACK_O_CHANNEL 12u
#define ST_ACK_O_MAX_LEAD 16u
#define ST_ACK_O_COUNT 20u /* how many adjacent channels were granted */


/* AUDIO: stream_pos, frames, then the audio itself.

   Under ST_ENC_PCM that is frames * channels samples of the declared format, interleaved. Under
   ST_ENC_VORBIS it is [u32 length][bytes] per encoded packet, however many the encoder produced
   for those frames — none is legal, and the sender then rolls the frames into its next message so
   stream_pos and frames stay contiguous either way.
   The channel count and format are per connection and are not repeated per packet.

   `stream_pos` counts frames since this stream started, in the SENDER's own rate. It is not a
   position on the card's axis and the sender does not know one: the device anchors the stream on
   arrival and adds the alignment delay, the way an RTP receiver does. That is why there is no
   clock exchange in this protocol — the sender never has to learn the card's clock, and the
   difference between the two crystals is taken out by the converter's ratio instead.

   The field exists only so the device can notice that the SENDER skipped: a jump means an xrun or
   a stop at the far end, which should leave a hole rather than being silently closed up. Ordering
   and duplication are TCP's problem, and TCP has already solved them.

   All channels travel in one packet rather than a stream each, so a sender's left and right
   cannot come apart: same packet, same converter, same fractional position. */
#define ST_AUDIO_FIXED 12u
#define ST_AUDIO_O_POS 0u    /* u64, frames since the stream started, in the sender's rate */
#define ST_AUDIO_O_FRAMES 8u /* u32 */

/* STATUS, sent by the device about ten times a second. This is the sync feedback loop: the
   device is the only party that can see where a packet actually landed relative to playout, so it
   measures the lead and the sender servos its release rate to hold it. The sender therefore needs
   no clock tracking of its own beyond one initial anchor, and — crucially — never has to make a
   step correction, which is what a burst is.

   `lead` is (target index of the least-delayed packet in the interval) - (playout position when
   it arrived), in frames. Taking the largest lead over the interval rather than the average is
   min-RTT filtering by another name: the least-delayed packet is the one that carries the
   cleanest measurement of the true offset. Valid only when ST_STATUS_F_LEAD is set. */
#define ST_STATUS_BYTES 40u
#define ST_STATUS_O_N_NOW 0u
#define ST_STATUS_O_WRITE_END 8u
#define ST_STATUS_O_LATE 16u
#define ST_STATUS_O_RANGE 20u
#define ST_STATUS_O_UNDER 24u
#define ST_STATUS_O_LEAD 28u        /* int32, frames, signed */
#define ST_STATUS_O_TARGET_LEAD 32u /* uint32, frames: what the device wants `lead` to be */
#define ST_STATUS_O_FLAGS 36u
#define ST_STATUS_F_LEAD 1u /* a packet arrived this interval, so `lead` means something */

/* How often the device reports. Also the sender's servo update rate. */
#define ST_STATUS_INTERVAL_MS 100u

/* ---- mixer -------------------------------------------------------------------------------
   Volume and mute live on the DEVICE, not in the plugin. The PCM and CTL plugins are separate
   objects with no way to share state, and putting it on the device means alsamixer on the sending
   machine and the web console are looking at, and changing, the same value.

   Levels are in hundredths of a dB, the unit ALSA itself uses for dB scales. */

/* CTL_HELLO: magic, proto, channel, pad. Channel 0xffff asks for whichever channel this address
   is using (or last used), so a mixer needs no more configuration than the PCM did. */
#define ST_CTL_HELLO_BYTES 12u
#define ST_CTLH_O_MAGIC 0u
#define ST_CTLH_O_PROTO 4u
#define ST_CTLH_O_CHANNEL 8u /* u16 */
#define ST_CTL_ANY_CHANNEL 0xffffu

/* CTL_ACK: proto, status, channel, gain_min, gain_max, gain, mute, count, flags.
   One volume covers a sender's whole run, so the ack says how many channels that is. The run is
   not fixed for the mixer's lifetime — the sender may take a wider one, or a different one,
   minutes later — so the device re-resolves it and pushes a MIX when it moves. */
#define ST_CTL_ACK_BYTES 36u
#define ST_CTLA_O_PROTO 0u
#define ST_CTLA_O_STATUS 4u
#define ST_CTLA_O_CHANNEL 8u
#define ST_CTLA_O_GAIN_MIN 12u /* i32, dB x 100 */
#define ST_CTLA_O_GAIN_MAX 16u /* i32, dB x 100 */
#define ST_CTLA_O_GAIN 20u     /* i32, dB x 100 */
#define ST_CTLA_O_MUTE 24u
#define ST_CTLA_O_COUNT 28u
#define ST_CTLA_O_FLAGS 32u /* ST_MIX_ST_*, the same set every MIX carries */

/* SET_MIX: mask, gain, mute. The mask says which fields to act on, so a mixer can move the
   volume without also asserting a mute state it may not have been told about yet. */
#define ST_SET_MIX_BYTES 12u
#define ST_MIX_F_GAIN 1u
#define ST_MIX_F_MUTE 2u
#define ST_SETMIX_O_MASK 0u
#define ST_SETMIX_O_GAIN 4u /* i32, dB x 100 */
#define ST_SETMIX_O_MUTE 8u

/* MIX: gain, mute, flags */
#define ST_MIX_BYTES 12u
#define ST_MIX_O_GAIN 0u /* i32, dB x 100 */
#define ST_MIX_O_MUTE 4u
#define ST_MIX_O_FLAGS 8u

/* State flags, carried by CTL_ACK and by every MIX. Not the SET_MIX mask above: those say which
   fields a mixer is asserting, these say what the device is doing. */
#define ST_MIX_ST_BYPASS 1u /* the run plays at unity: its sender declared the stream un-mixable */

/* How often the device looks for a change to push to a connected mixer. */
#define ST_MIX_POLL_MS 100u

/* FORMAT: rate, format, channels, encoding, flags. Sent once the application has settled the
   PCM's hw_params and before any audio, because a device is opened long before its format is
   known — the HELLO cannot carry it. A sender whose rate differs from the card's is resampled on
   the device: the card's clock is not negotiable, and the sending machine should not have to
   care. */
#define ST_FORMAT_BYTES 20u
#define ST_FMT_O_RATE 0u
#define ST_FMT_O_FORMAT 4u
#define ST_FMT_O_CHANNELS 8u
#define ST_FMT_O_ENCODING 12u
#define ST_FMT_O_FLAGS 16u

/* Stream flags, declared in FORMAT and in force for the life of the stream.
   NO_MIXER makes the stream un-mixable: its channels play exactly as they arrive, and neither
   alsamixer nor the web console can attenuate or mute them. Per PCM, so an .asoundrc can carry one
   device that is volume-controlled and one that is not. */
#define ST_STREAM_F_NO_MIXER 1u

/* How the samples in an AUDIO packet are carried.
   PCM is the default and the only lossless option; it is what a measurement wants. Vorbis exists
   for links that cannot carry 2.3 MB/s — the sender encodes and the device decodes — and is lossy,
   so it belongs to monitoring and stimulus rather than to a reference measurement.

   Raw Vorbis packets, not an Ogg stream. Ogg exists to frame and order packets over a transport
   that does neither; this one is framed and ordered already, so a container would only add
   overhead and a second parser. The setup packets Ogg would carry in its first pages travel once,
   in CODEC_INIT. */
#define ST_ENC_PCM 0u
#define ST_ENC_VORBIS 1u

/* CODEC_INIT: packet count, then [u32 length][bytes] for each. */
#define ST_CODEC_O_COUNT 0u
#define ST_CODEC_FIXED 4u
#define ST_CODEC_MAX_PACKETS 8u

/* Rates a sender may declare. The device resamples anything that is not its own. */
#define ST_RATE_MIN 8000u
#define ST_RATE_MAX 192000u

/* Largest payload the receiver will ever accept, so a bad length field cannot make it allocate
   or block forever waiting for bytes that are not coming. */
#define ST_NET_MAX_PAYLOAD (ST_AUDIO_FIXED + 4u * 4096u * ST_MAX_STREAM_CHANNELS)

#endif /* ST_NET_PROTO_H */
