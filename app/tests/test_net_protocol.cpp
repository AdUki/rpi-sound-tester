#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "constants.h"
#include "net_proto.h"

using namespace st;

namespace {

// The wire helpers are the one piece of this feature compiled into two different languages, so
// they are worth pinning down explicitly rather than trusting to symmetry.
void test_little_endian_round_trip() {
  uint8_t b[8];

  st_put_u16(b, 0x1234);
  CHECK_EQ(b[0], static_cast<uint8_t>(0x34));
  CHECK_EQ(b[1], static_cast<uint8_t>(0x12));
  CHECK_EQ(st_get_u16(b), static_cast<uint16_t>(0x1234));

  st_put_u32(b, 0xdeadbeefu);
  CHECK_EQ(b[0], static_cast<uint8_t>(0xef));
  CHECK_EQ(b[3], static_cast<uint8_t>(0xde));
  CHECK_EQ(st_get_u32(b), 0xdeadbeefu);

  const uint64_t big = 0x0123456789abcdefull;
  st_put_u64(b, big);
  CHECK_EQ(b[0], static_cast<uint8_t>(0xef));
  CHECK_EQ(b[7], static_cast<uint8_t>(0x01));
  CHECK_EQ(st_get_u64(b), big);

  // Extremes, where a sign slip would show up.
  st_put_u64(b, ~0ull);
  CHECK_EQ(st_get_u64(b), ~0ull);
  st_put_u32(b, 0x80000000u);
  CHECK_EQ(st_get_u32(b), 0x80000000u);
}

void test_header_round_trip() {
  uint8_t h[ST_NET_HEADER_BYTES];
  st_put_header(h, ST_MSG_AUDIO, 1234);
  CHECK_EQ(st_header_type(h), static_cast<uint8_t>(ST_MSG_AUDIO));
  CHECK_EQ(st_header_len(h), 1234u);

  st_put_header(h, ST_MSG_HELLO, 0);
  CHECK_EQ(st_header_type(h), static_cast<uint8_t>(ST_MSG_HELLO));
  CHECK_EQ(st_header_len(h), 0u);
}

void test_audio_packet_layout() {
  std::vector<uint8_t> pkt(ST_AUDIO_FIXED + 4 * 3);
  st_put_u64(pkt.data() + ST_AUDIO_O_POS, 1234567890123ull);
  st_put_u32(pkt.data() + ST_AUDIO_O_FRAMES, 3);
  for (int i = 0; i < 3; ++i)
    st_put_u32(pkt.data() + ST_AUDIO_FIXED + 4 * i, static_cast<uint32_t>(1000 + i));

  CHECK_EQ(st_get_u64(pkt.data() + ST_AUDIO_O_POS), 1234567890123ull);
  CHECK_EQ(st_get_u32(pkt.data() + ST_AUDIO_O_FRAMES), 3u);
  for (int i = 0; i < 3; ++i)
    CHECK_EQ(st_get_u32(pkt.data() + ST_AUDIO_FIXED + 4 * i), static_cast<uint32_t>(1000 + i));
}

// Each field must round-trip and none may sit on top of another — the layouts are hand-numbered
// offsets shared between two languages, so an overlap is a plausible mistake with no compiler to
// catch it.
void test_hello_ack_fields_do_not_overlap() {
  uint8_t ack[ST_HELLO_ACK_BYTES];
  std::memset(ack, 0, sizeof(ack));
  st_put_u32(ack + ST_ACK_O_PROTO, 4);
  st_put_u32(ack + ST_ACK_O_STATUS, ST_HELLO_OK);
  st_put_u32(ack + ST_ACK_O_RATE, 96000);
  st_put_u32(ack + ST_ACK_O_CHANNEL, 1);
  st_put_u32(ack + ST_ACK_O_MAX_LEAD, 288000);
  st_put_u32(ack + ST_ACK_O_COUNT, 2);

  CHECK_EQ(st_get_u32(ack + ST_ACK_O_PROTO), 4u);
  CHECK_EQ(st_get_u32(ack + ST_ACK_O_STATUS), ST_HELLO_OK);
  CHECK_EQ(st_get_u32(ack + ST_ACK_O_RATE), 96000u);
  CHECK_EQ(st_get_u32(ack + ST_ACK_O_CHANNEL), 1u);
  CHECK_EQ(st_get_u32(ack + ST_ACK_O_MAX_LEAD), 288000u);
  CHECK_EQ(st_get_u32(ack + ST_ACK_O_COUNT), 2u);
  // Every field within the declared size, so the struct and the constant cannot drift apart.
  CHECK(ST_ACK_O_COUNT + 4 <= ST_HELLO_ACK_BYTES);
}

// The three layouts that grew a flags field. An overlap here would show up as a volume control
// that mutes, or a stream that bypasses when it should not.
void test_mixer_layouts_carry_their_flags() {
  uint8_t ack[ST_CTL_ACK_BYTES];
  std::memset(ack, 0, sizeof(ack));
  st_put_u32(ack + ST_CTLA_O_GAIN, static_cast<uint32_t>(-1200));
  st_put_u32(ack + ST_CTLA_O_MUTE, 1);
  st_put_u32(ack + ST_CTLA_O_COUNT, 2);
  st_put_u32(ack + ST_CTLA_O_FLAGS, ST_MIX_ST_BYPASS);
  CHECK_EQ(static_cast<int32_t>(st_get_u32(ack + ST_CTLA_O_GAIN)), -1200);
  CHECK_EQ(st_get_u32(ack + ST_CTLA_O_MUTE), 1u);
  CHECK_EQ(st_get_u32(ack + ST_CTLA_O_COUNT), 2u);
  CHECK_EQ(st_get_u32(ack + ST_CTLA_O_FLAGS), ST_MIX_ST_BYPASS);
  CHECK(ST_CTLA_O_FLAGS + 4 <= ST_CTL_ACK_BYTES);

  uint8_t mix[ST_MIX_BYTES];
  std::memset(mix, 0, sizeof(mix));
  st_put_u32(mix + ST_MIX_O_GAIN, static_cast<uint32_t>(-600));
  st_put_u32(mix + ST_MIX_O_MUTE, 0);
  st_put_u32(mix + ST_MIX_O_FLAGS, ST_MIX_ST_BYPASS);
  CHECK_EQ(static_cast<int32_t>(st_get_u32(mix + ST_MIX_O_GAIN)), -600);
  CHECK_EQ(st_get_u32(mix + ST_MIX_O_MUTE), 0u);
  CHECK_EQ(st_get_u32(mix + ST_MIX_O_FLAGS), ST_MIX_ST_BYPASS);
  CHECK(ST_MIX_O_FLAGS + 4 <= ST_MIX_BYTES);

  uint8_t fmt[ST_FORMAT_BYTES];
  std::memset(fmt, 0, sizeof(fmt));
  st_put_u32(fmt + ST_FMT_O_RATE, 48000);
  st_put_u32(fmt + ST_FMT_O_ENCODING, ST_ENC_VORBIS);
  st_put_u32(fmt + ST_FMT_O_FLAGS, ST_STREAM_F_NO_MIXER);
  CHECK_EQ(st_get_u32(fmt + ST_FMT_O_RATE), 48000u);
  CHECK_EQ(st_get_u32(fmt + ST_FMT_O_ENCODING), ST_ENC_VORBIS);
  CHECK_EQ(st_get_u32(fmt + ST_FMT_O_FLAGS), ST_STREAM_F_NO_MIXER);
  CHECK(ST_FMT_O_FLAGS + 4 <= ST_FORMAT_BYTES);

  // The two flag sets travel in different messages and must not be read as one another.
  CHECK(ST_MIX_ST_BYPASS != 0u && ST_STREAM_F_NO_MIXER != 0u);
}

// The C++ constants and the C macros the plugin compiles against must agree, or the two ends of
// the wire disagree about the port, the rate or the packet size.
void test_shared_constants_agree() {
  CHECK_EQ(static_cast<unsigned>(kNetPort), static_cast<unsigned>(ST_DEFAULT_PORT));
  CHECK_EQ(kNetProtoVersion, ST_NET_PROTO_VERSION);
  CHECK_EQ(kNetPacketFrames, ST_PACKET_FRAMES);
  CHECK_EQ(kDefaultRate, ST_DEFAULT_RATE);
  // A full-size packet must fit the receiver's cap, or every large packet is refused.
  CHECK(ST_AUDIO_FIXED + 4u * kNetMaxPacketFrames <= ST_NET_MAX_PAYLOAD);
  // Two readers plus the sender's lead have to fit in the timeline.
  CHECK(kNetTimelineMs > 2 * kNetDelayMaxMs);
}

}  // namespace

int main() {
  test_little_endian_round_trip();
  test_header_round_trip();
  test_audio_packet_layout();
  test_hello_ack_fields_do_not_overlap();
  test_mixer_layouts_carry_their_flags();
  test_shared_constants_agree();
  return report("net_protocol");
}
