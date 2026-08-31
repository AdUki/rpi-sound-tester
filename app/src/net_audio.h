#pragma once

#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "constants.h"
#include "control.h"
#include "util/asrc.h"
#include "vorbis_decode.h"

namespace st {

// One network input channel's timeline: a ring indexed by ABSOLUTE sample number rather than by
// arrival order. A packet is placed at the index its sender asked for, which is what makes the
// whole feature work — out-of-order and duplicate packets land correctly by construction, a lost
// packet leaves a hole rather than shifting everything after it, and there is no jitter-buffer
// state machine to get wrong. The timeline *is* the jitter buffer.
//
// Exactly one writer (that channel's connection thread) and one reader (the audio thread), and
// they are kept apart in the index space rather than by a lock: a write is only accepted well
// ahead of where the reader is, so the two never touch the same frames.
class NetTimeline {
 public:
  enum class Write { Ok, Late, OutOfRange };

  explicit NetTimeline(size_t frames);

  // Connection thread. `reader_n` is where the audio thread currently is; `guard` is how far
  // ahead of it a packet must land to be safe from the reader, and `max_lead` how far ahead is
  // still plausible rather than a bug or a hostile sender.
  Write write(uint64_t at, const float* samples, size_t frames, uint64_t reader_n, uint64_t guard,
              uint64_t max_lead);

  // Audio thread only. Copies [n, n+frames) into `out` at `stride`. With `clear`, also zeroes
  // the region it just consumed — which is what makes "never arrived" mean silence: nothing has
  // to track which individual frames are present, because unwritten frames are already zero.
  // Only the trailing of the two readers clears, or the other would find the data gone.
  void read(uint64_t n, size_t frames, float* out, size_t stride, bool clear);

  // Highest (target + frames) ever accepted. The reader compares against it to notice that it
  // has run past the end of what the sender has supplied.
  uint64_t write_end() const { return write_end_.load(std::memory_order_acquire); }
  void reset();

 private:
  const size_t frames_;
  const size_t mask_;
  std::vector<float> buf_;
  std::atomic<uint64_t> write_end_{0};
};

// Smooths the measured lead before it is allowed to steer the converter.
//
// The raw measurement is noisy for two reasons that have nothing to do with the clocks it is
// meant to be tracking: packets arrive with whatever jitter the network adds, and the reader
// position they are measured against only advances once per audio block. Feeding that straight
// into the trim makes the loop chase noise — at a five-second time constant, one block of
// quantisation alone is worth half the trim's whole authority.
//
// An exponential moving average, with its coefficient derived from the actual interval between
// packets so the time constant is in seconds rather than in packets. Packet sizes and rates vary;
// a fixed coefficient would mean a different filter for every sender.
struct LeadFilter {
  double avg = 0.0;
  bool primed = false;

  void reset() {
    avg = 0.0;
    primed = false;
  }

  // First measurement after an anchor is taken as-is: there is nothing to average it with, and
  // starting from zero would spend the first seconds pretending the stream was badly out.
  double update(double lead, double dt_s, double tau_s) {
    if (!primed) {
      avg = lead;
      primed = true;
      return avg;
    }
    const double a = dt_s <= 0.0 ? 0.0 : dt_s / (tau_s + dt_s);
    avg += a * (lead - avg);
    return avg;
  }
};

// The trim to hold `lead` at `target`, as a multiplier on the converter's nominal ratio.
// Proportional, and clamped: `max_dev` is far more than two crystals can differ by, and small
// enough that the correction is never audible as a pitch step.
inline double asrc_trim(double lead, double target, double rate, double tau_s, double max_dev) {
  const double err = target - lead;
  const double trim = 1.0 + err / (rate * tau_s);
  return trim < 1.0 - max_dev ? 1.0 - max_dev : (trim > 1.0 + max_dev ? 1.0 + max_dev : trim);
}

struct NetChannelStatus {
  unsigned channel = 0;
  bool connected = false;
  std::string peer;   // ip:port of the live connection
  std::string name;   // what the sender called itself in its HELLO
  std::string host;   // reverse-resolved hostname, when the network can supply one
  // The one string worth showing: the sender's own name, else its hostname, else its address.
  std::string device;
  // Whose it was last, even after it disconnected — so an operator can see that NET 3 is "the
  // one the laptop uses" while the laptop is off.
  std::string last_device;
  // Where this channel sits in its sender's stream: 1-based index and total. Both 1 for the
  // ordinary mono case; a stereo sender's pair reads 1/2 and 2/2.
  unsigned stream_index = 1;
  unsigned stream_count = 1;
  uint64_t frames_received = 0;
  uint64_t late_drops = 0;
  uint64_t range_drops = 0;
  uint64_t underruns = 0;
  uint64_t resyncs = 0;
  uint64_t last_target = 0;
  uint64_t write_end = 0;
  // How far ahead of playout the sender's packets are landing, and where the device wants that
  // to be. The gap between them is what the sender's release-rate servo is closing.
  int64_t lead_frames = 0;
  bool lead_valid = false;
  uint64_t target_lead_frames = 0;
  float peak = 0.0f;
};

// Accepts connections from the soundtester ALSA plugin and turns them into input channels.
//
// Threading mirrors the listen streams: one accept thread, then one thread parked per connection
// for its whole life. Nothing here ever blocks the audio thread — read_block() only copies out of
// preallocated timelines.
class NetAudioServer {
 public:
  // `port` is the configured port, remembered even while the server is stopped so that
  // enabling it later binds where the operator asked rather than falling back to a default.
  NetAudioServer(Control& ctl, double rate, uint16_t port);
  ~NetAudioServer();

  NetAudioServer(const NetAudioServer&) = delete;
  NetAudioServer& operator=(const NetAudioServer&) = delete;

  // Binds and starts accepting. A failure to bind is reported, not fatal — the same philosophy
  // as a card that will not open: the web console has to stay up to explain why.
  bool start(uint16_t port);
  void stop();

  // Audio thread. Fills the network channels of two blocks that sit on two different axes:
  //
  //   `live`  — the timeline at n. This is what gets ROUTED to an output, so it is emitted at
  //             exactly the instant the sender asked for.
  //   `ring`  — the timeline at n - delay. This is what enters the ring, and it is delayed for
  //             the same reason local capture is: ring index n has to mean one instant on every
  //             channel. A network sample emitted at real time T is heard at T, and a local mic
  //             hearing it lands at T + delay — so the network channel must land there too, or
  //             an xcorr between the two would read a whole delay too high.
  void read_block(uint64_t n, size_t frames, uint64_t delay, float* live, float* ring);

  std::vector<NetChannelStatus> status() const;

  // Claims `count` ADJACENT channels for `ip` and returns the first, or -1 if the run will not
  // fit. Adjacent because a stereo sender's two channels should read as one source — NET 3+4, not
  // NET 3 and NET 6 — and all-or-nothing, because a half-claimed run would strand one channel of
  // a pair owned by nobody. `want` < kNetInputs pins the run explicitly; anything else prefers
  // wherever this address was last time, so routing survives a reconnect.
  int claim_channels(unsigned want, const std::string& ip, unsigned count);
  void release_channels(unsigned base, unsigned count);

  // The run of channels one mixer connection drives: the whole of whatever `ip` is streaming.
  //
  // Call it again rather than caching the answer. alsamixer is normally opened before anything
  // plays, when the address owns one channel; a mixer still holding that answer once a stereo
  // sender has taken NET 1+2 would turn down only the left. `want` < kNetInputs pins a channel
  // explicitly (a `channel` argument or a per-channel port); anything else follows the sender.
  struct MixRun {
    unsigned base = 0;
    unsigned count = 1;
  };
  MixRun mixer_run(const std::string& ip, uint16_t want) const;

  // Whether a network channel is worth showing. Sticky for the session on purpose: a sender that
  // has been and gone leaves audio in the ring, and a freeze taken after it disconnected has to
  // stay analysable. A channel nobody has ever used stays out of the way entirely.
  bool channel_in_use(unsigned c) const;
  uint16_t port() const { return port_; }
  bool listening() const { return listening_.load(); }
  std::string last_error() const;
  unsigned connected_count() const;

 private:
  struct Channel;

  void accept_loop();

  // Everything one connected sender needs, with a method per message type.
  struct Session {
    explicit Session(NetAudioServer& s) : srv(s) {}
    NetAudioServer& srv;
    int fd = -1;
    std::string ip, peer;
    int port_channel = -1;   // channel implied by the port the sender chose, or -1

    int base = -1;           // first channel of the claimed run, or -1
    unsigned channels = 1;
    unsigned wire_fmt = 0;   // ST_FMT_*
    unsigned wire_bytes = 4;
    unsigned encoding = 0;   // ST_ENC_*
    unsigned src_rate = 0;   // the sender's own rate, once FORMAT has said

    VorbisDecoder vorbis;
    Asrc asrc;
    LeadFilter lead;
    uint64_t out_next = 0;   // device index for the next converted frame; 0 = unanchored
    uint64_t expect_pos = 0; // where the sender's next packet should start, in its own frames
    uint64_t last_status_ns = 0;

    std::vector<float> in_il, out_il, chan;

    // Each returns false to close the connection. The protocol is small enough that "this message
    // was wrong" and "hang up" are the same answer.
    bool hello(const uint8_t* p, uint32_t len, const sockaddr_in& addr);
    bool format(const uint8_t* p, uint32_t len);
    bool codec_init(const uint8_t* p, uint32_t len);
    bool audio(const uint8_t* p, uint32_t len);
    bool status();
    void reject(uint32_t code);
    // Splits converted interleaved audio across the run's timelines and keeps each channel's
    // counters. Separate from audio() because it is the only part that touches the channels the
    // audio thread reads, and it reads better without the decode and the control loop above it.
    void distribute(const float* src, size_t frames, uint64_t at, uint64_t reader,
                    uint64_t max_lead);
  };

  // `port_channel` is the channel implied by the port the sender chose, or -1 for the base port,
  // which means "any". Choosing a port is as deliberate as passing a channel argument, so it wins
  // over the one in the HELLO.
  void serve(int fd, sockaddr_in peer, int port_channel);
  // A mixer connection. Deliberately not a claim on the channel: opening alsamixer must not cost
  // a network input slot, and closing it must not take the audio down with it.
  void serve_ctl(int fd, const std::string& ip, uint16_t want);
  int ctl_channel_for(const std::string& ip, uint16_t want) const;
  void set_error(const std::string& e);

  Control& ctl_;
  const double rate_;

  std::vector<std::unique_ptr<Channel>> chans_;
  std::atomic<uint64_t> reader_n_{0};

  // The base port takes any channel; base + 1 + c is a direct line to channel c, so a sender can
  // be pinned by the one setting its .asoundrc already had. One socket each, all in one poll.
  std::vector<int> listen_fds_;
  std::vector<int> listen_channels_;  // parallel to listen_fds_; -1 = any
  uint16_t port_ = 0;  // the configured base port, whether or not it is bound
  std::atomic<bool> running_{false};
  std::atomic<bool> listening_{false};
  std::thread accept_thread_;

  // A finished thread is still joinable until it is joined, so "has it ended?" needs its own
  // flag — testing joinable() would reap nothing and leak a thread object per client.
  struct Conn {
    std::thread th;
    std::shared_ptr<std::atomic<bool>> done;
  };
  mutable std::mutex conn_m_;
  std::vector<Conn> conns_;

  mutable std::mutex err_m_;
  std::string last_error_;
};

}  // namespace st
