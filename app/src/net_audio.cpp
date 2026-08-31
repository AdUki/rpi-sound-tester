#include "net_audio.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "net_proto.h"
#include "util/dsp.h"
#include "util/log.h"

namespace st {

namespace {

// How far ahead of the reader a packet must land. One block would be the bare minimum; two keeps
// the writer clear of the reader even if the audio thread advances while the check is in flight.
constexpr uint64_t kGuardFrames = 2 * kDefaultPeriod;

// How quickly the ratio trim closes a residual offset, and how far it may stray from nominal.
// 0.2% is far more than two crystals can differ by, and small enough that the audio does not
// audibly change pitch while it is being applied.
constexpr double kAsrcTauS = 5.0;
constexpr double kAsrcTrimMax = 0.002;

// Reads exactly n bytes unless the connection dies or the server is stopping. The poll timeout is
// what lets a parked connection notice a shutdown instead of holding teardown open.
bool read_exact(int fd, void* buf, size_t n, const std::atomic<bool>& running) {
  auto* p = static_cast<uint8_t*>(buf);
  size_t got = 0;
  while (got < n) {
    if (!running.load()) return false;
    pollfd pfd{fd, POLLIN, 0};
    const int pr = poll(&pfd, 1, 250);
    if (pr < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (pr == 0) continue;
    const ssize_t r = ::recv(fd, p + got, n - got, 0);
    if (r == 0) return false;  // clean close
    if (r < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      return false;
    }
    got += static_cast<size_t>(r);
  }
  return true;
}

bool write_all(int fd, const void* buf, size_t n) {
  const auto* p = static_cast<const uint8_t*>(buf);
  size_t sent = 0;
  while (sent < n) {
    const ssize_t w = ::send(fd, p + sent, n - sent, MSG_NOSIGNAL);
    if (w <= 0) {
      if (w < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      return false;
    }
    sent += static_cast<size_t>(w);
  }
  return true;
}

bool send_msg(int fd, uint8_t type, const uint8_t* payload, uint32_t len) {
  uint8_t hdr[ST_NET_HEADER_BYTES];
  st_put_header(hdr, type, len);
  if (!write_all(fd, hdr, sizeof(hdr))) return false;
  return len == 0 || write_all(fd, payload, len);
}

// Asks the network what this address is called. Runs on the connection thread and only after the
// handshake has been answered: a resolver with nothing to talk to can block for seconds, and the
// sender is holding a timeout open waiting for its HELLO_ACK.
std::string reverse_lookup(const sockaddr_in& addr) {
  char host[NI_MAXHOST] = {0};
  const int rc = getnameinfo(reinterpret_cast<const sockaddr*>(&addr), sizeof(addr), host,
                             sizeof(host), nullptr, 0, NI_NAMEREQD);
  if (rc != 0) return {};
  std::string h(host);
  if (!h.empty() && h.back() == '.') h.pop_back();
  return h;
}

// A bench machine that is switched off mid-stream never closes its socket, and without this the
// channel it held would stay claimed until the daemon restarted. Keepalive is the only thing that
// notices, because an idle-but-open PCM legitimately sends nothing.
void enable_keepalive(int fd) {
  int on = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
#ifdef TCP_KEEPIDLE
  int idle = 20, intvl = 5, cnt = 3;  // dead peer detected in about 35 s
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
}

}  // namespace

// ---- NetTimeline ----------------------------------------------------------------------------

NetTimeline::NetTimeline(size_t frames) : frames_(frames), mask_(frames - 1), buf_(frames, 0.0f) {
  // Power of two: the index is a mask, exactly like RingBuffer.
}

NetTimeline::Write NetTimeline::write(uint64_t at, const float* samples, size_t frames,
                                      uint64_t reader_n, uint64_t guard, uint64_t max_lead) {
  if (frames == 0 || frames > frames_) return Write::OutOfRange;

  // Too late: its slot has gone past the reader, so playing it now would put the audio at the
  // wrong instant. Dropped rather than slid forward, which is what would corrupt a measurement.
  if (at < reader_n + guard) return Write::Late;
  if (at > reader_n + max_lead) return Write::OutOfRange;

  size_t idx = static_cast<size_t>(at & mask_);
  const size_t first = std::min(frames, frames_ - idx);
  memcpy(buf_.data() + idx, samples, first * sizeof(float));
  if (first < frames) memcpy(buf_.data(), samples + first, (frames - first) * sizeof(float));

  const uint64_t end = at + frames;
  uint64_t prev = write_end_.load(std::memory_order_relaxed);
  while (end > prev && !write_end_.compare_exchange_weak(prev, end, std::memory_order_release,
                                                         std::memory_order_relaxed)) {
  }
  return Write::Ok;
}

void NetTimeline::read(uint64_t n, size_t frames, float* out, size_t stride, bool clear) {
  size_t idx = static_cast<size_t>(n & mask_);
  const size_t first = std::min(frames, frames_ - idx);
  for (size_t i = 0; i < first; ++i) out[i * stride] = buf_[idx + i];
  for (size_t i = first; i < frames; ++i) out[i * stride] = buf_[i - first];

  if (!clear) return;
  // Clear behind us so an unwritten frame on the next lap reads as silence rather than as
  // whatever was here a ring ago.
  memset(buf_.data() + idx, 0, first * sizeof(float));
  if (first < frames) memset(buf_.data(), 0, (frames - first) * sizeof(float));
}

void NetTimeline::reset() {
  std::fill(buf_.begin(), buf_.end(), 0.0f);
  write_end_.store(0, std::memory_order_release);
}

// ---- NetAudioServer -------------------------------------------------------------------------

struct NetAudioServer::Channel {
  explicit Channel(size_t frames) : timeline(frames) {}

  NetTimeline timeline;
  std::atomic<bool> claimed{false};
  std::atomic<uint64_t> frames_received{0};
  std::atomic<uint64_t> late_drops{0};
  std::atomic<uint64_t> range_drops{0};
  std::atomic<uint64_t> underruns{0};
  std::atomic<uint64_t> resyncs{0};
  std::atomic<uint64_t> last_target{0};
  // The filtered lead the converter's ratio is steered by, which is also the honest thing to
  // report: a raw per-interval maximum reads about one audio block pessimistic.
  std::atomic<int64_t> lead_avg{0};
  std::atomic<bool> lead_valid{false};
  std::atomic<uint32_t> peak_milli{0};  // peak * 1000, so the meter needs no lock

  mutable std::mutex m;
  std::string peer;
  std::string name;
  std::string host;
  // Survives the disconnect on purpose: it is what makes a returning sender land back here, and
  // what lets the console still say whose channel this is while the machine is switched off.
  std::string last_ip;
  std::string last_device;
  // The run this channel belongs to, remembered past the disconnect so a returning sender is
  // offered the same block and a mixer knows how many channels its one volume covers.
  unsigned stream_base = 0;
  unsigned stream_count = 1;
  unsigned stream_index = 1;  // 1-based position within that run

  // Caller holds m.
  std::string device_locked() const {
    if (!name.empty() && name != "alsa-plugin") return name;
    if (!host.empty()) return host;
    return last_ip;
  }
};

namespace {
size_t timeline_frames(double rate) {
  size_t want = static_cast<size_t>(rate * kNetTimelineMs / 1000.0);
  size_t p = 1;
  while (p < want) p <<= 1;
  return p;
}
}  // namespace

NetAudioServer::NetAudioServer(Control& ctl, double rate, uint16_t port)
    : ctl_(ctl), rate_(rate) {
  port_ = port;
  const size_t frames = timeline_frames(rate);
  chans_.reserve(kNetInputs);
  for (unsigned c = 0; c < kNetInputs; ++c) chans_.push_back(std::make_unique<Channel>(frames));
  LOG_INFO("net: {} channels, {} frame timelines ({:.1f} s each)", kNetInputs, frames,
           frames / rate);
}

NetAudioServer::~NetAudioServer() { stop(); }

bool NetAudioServer::start(uint16_t port) {
  if (running_.load()) return true;
  port_ = port;

  // Bind on the caller's thread so start() can report a real result. Doing it inside the accept
  // thread would make listening() race every caller that checks it right after enabling.
  auto bind_one = [&](uint16_t p, int channel) -> bool {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(p);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || ::listen(fd, 8) < 0) {
      ::close(fd);
      return false;
    }
    listen_fds_.push_back(fd);
    listen_channels_.push_back(channel);
    return true;
  };

  if (!bind_one(port_, -1)) {
    set_error("bind port " + std::to_string(port_) + ": " + strerror(errno));
    return false;
  }
  // The per-channel ports are a convenience, not the service: if one is taken, say so and carry
  // on rather than refusing to accept audio at all.
  unsigned pinned = 0;
  for (unsigned c = 0; c < kNetInputs; ++c) {
    if (bind_one(static_cast<uint16_t>(port_ + 1 + c), static_cast<int>(c))) ++pinned;
  }
  if (pinned != kNetInputs) {
    LOG_WARN("net: only {} of {} per-channel ports could be bound", pinned, kNetInputs);
  }

  set_error({});
  listening_.store(true);
  running_.store(true);
  LOG_INFO("net: listening on {} (any channel) and {}..{} (NET 1..{})", port_, port_ + 1,
           port_ + kNetInputs, kNetInputs);
  accept_thread_ = std::thread([this] { accept_loop(); });
  return true;
}

void NetAudioServer::stop() {
  if (!running_.exchange(false)) return;
  for (int fd : listen_fds_) {
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
  }
  listen_fds_.clear();
  listen_channels_.clear();
  if (accept_thread_.joinable()) accept_thread_.join();
  std::vector<Conn> conns;
  {
    std::lock_guard<std::mutex> lock(conn_m_);
    conns.swap(conns_);
  }
  for (auto& c : conns)
    if (c.th.joinable()) c.th.join();
  listening_.store(false);
}

void NetAudioServer::set_error(const std::string& e) {
  std::lock_guard<std::mutex> lock(err_m_);
  last_error_ = e;
  if (!e.empty()) LOG_WARN("net: {}", e);
}

std::string NetAudioServer::last_error() const {
  std::lock_guard<std::mutex> lock(err_m_);
  return last_error_;
}

unsigned NetAudioServer::connected_count() const {
  unsigned n = 0;
  for (const auto& c : chans_)
    if (c->claimed.load()) ++n;
  return n;
}

int NetAudioServer::claim_channels(unsigned want, const std::string& ip, unsigned count) {
  if (count == 0 || count > kNetInputs) return -1;

  // Takes the whole run or nothing: a half-claimed stereo sender would leave one channel of a
  // pair stranded and claimed by no one.
  auto try_run = [&](unsigned base) -> bool {
    if (base + count > kNetInputs) return false;
    for (unsigned i = 0; i < count; ++i) {
      bool expected = false;
      if (!chans_[base + i]->claimed.compare_exchange_strong(expected, true)) {
        for (unsigned j = 0; j < i; ++j) chans_[base + j]->claimed.store(false);
        return false;
      }
    }
    return true;
  };

  // Records who has the run and how wide it is, so that a later claim from the same address can
  // find it. Done here rather than by the caller: this is the function that decides the run, and
  // splitting the decision from the record of it left the stickiness depending on a caller
  // remembering to write down what had just been handed out.
  auto took = [&](unsigned b) {
    for (unsigned i = 0; i < count; ++i) {
      Channel& c = *chans_[b + i];
      std::lock_guard<std::mutex> lock(c.m);
      c.last_ip = ip;
      c.stream_base = b;
      c.stream_count = count;
      c.stream_index = i + 1;
    }
    return static_cast<int>(b);
  };

  // An explicit `channel N` (or a per-channel port) always wins — that is how you pin two streams
  // from one machine, which the address alone cannot tell apart.
  if (want < kNetInputs) return try_run(want) ? took(want) : -1;

  // Otherwise put a returning sender back where it was, provided the run still fits there.
  for (unsigned c = 0; c < kNetInputs; ++c) {
    bool mine;
    {
      std::lock_guard<std::mutex> lock(chans_[c]->m);
      mine = chans_[c]->last_ip == ip && chans_[c]->stream_base == c;
    }
    if (mine && try_run(c)) return took(c);
  }
  for (unsigned c = 0; c + count <= kNetInputs; ++c)
    if (try_run(c)) return took(c);
  return -1;
}

void NetAudioServer::release_channels(unsigned base, unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    Channel& c = *chans_[base + i];
    c.timeline.reset();
    c.peak_milli.store(0);
    c.lead_avg.store(0);
    c.resyncs.store(0);
    c.lead_valid.store(false);
    // The bypass went with the stream; the channel is ordinary again, and its gain applies.
    ctl_.inputs[kInputs + base + i].bypass.store(false);
    {
      std::lock_guard<std::mutex> lock(c.m);
      c.last_device = c.device_locked();  // remembered for the console and for the next claim
      c.peer.clear();
      c.name.clear();
      c.host.clear();
    }
    c.claimed.store(false);
  }
}

void NetAudioServer::accept_loop() {
  while (running_.load()) {
    std::vector<pollfd> pfds;
    pfds.reserve(listen_fds_.size());
    for (int lfd : listen_fds_) pfds.push_back(pollfd{lfd, POLLIN, 0});
    const int pr = poll(pfds.data(), pfds.size(), 250);
    if (pr <= 0) continue;

    size_t which = pfds.size();
    for (size_t i = 0; i < pfds.size(); ++i) {
      if (pfds[i].revents & POLLIN) {
        which = i;
        break;
      }
    }
    if (which == pfds.size()) continue;
    const int port_channel = listen_channels_[which];

    sockaddr_in peer{};
    socklen_t plen = sizeof(peer);
    const int fd = ::accept(pfds[which].fd, reinterpret_cast<sockaddr*>(&peer), &plen);
    if (fd < 0) continue;

    // Reap the connections that have ended, so a server that has been up for weeks does not hold
    // a thread object for every client that ever came and went.
    {
      std::lock_guard<std::mutex> lock(conn_m_);
      for (size_t i = conns_.size(); i-- > 0;) {
        if (!conns_[i].done->load()) continue;
        conns_[i].th.join();
        conns_.erase(conns_.begin() + static_cast<long>(i));
      }
      auto done = std::make_shared<std::atomic<bool>>(false);
      conns_.push_back({std::thread([this, fd, peer, port_channel, done] {
                          serve(fd, peer, port_channel);
                          done->store(true);
                        }),
                        done});
    }
  }
}

void NetAudioServer::Session::reject(uint32_t code) {
  uint8_t ack[ST_HELLO_ACK_BYTES] = {0};
  st_put_u32(ack + ST_ACK_O_PROTO, kNetProtoVersion);
  st_put_u32(ack + ST_ACK_O_STATUS, code);
  st_put_u32(ack + ST_ACK_O_RATE, static_cast<uint32_t>(srv.rate_));
  send_msg(fd, ST_MSG_HELLO_ACK, ack, sizeof(ack));
}

bool NetAudioServer::Session::hello(const uint8_t* p, uint32_t len, const sockaddr_in& addr) {

      if (base >= 0 || len < ST_HELLO_FIXED) return false;
      if (st_get_u32(p + ST_HELLO_O_MAGIC) != ST_NET_MAGIC ||
          st_get_u32(p + ST_HELLO_O_PROTO) != kNetProtoVersion) {
        reject(ST_HELLO_BAD_PROTO);
        return false;
      }
      if (!srv.ctl_.net.enabled.load()) {
        reject(ST_HELLO_DISABLED);
        return false;
      }
      // 0 means "tell me yours" — the ack always carries the card's rate, and the sender's own
      // rate arrives later in FORMAT once its application has settled on one.
      const uint32_t rate = st_get_u32(p + ST_HELLO_O_RATE);
      if (rate != 0 && rate != static_cast<uint32_t>(srv.rate_)) {
        LOG_WARN("net: {} wants {} Hz, the card is at {} Hz", peer, rate, srv.rate_);
        reject(ST_HELLO_BAD_RATE);
        return false;
      }
      const uint16_t ch_count = st_get_u16(p + ST_HELLO_O_CHANNELS);
      if (st_get_u32(p + ST_HELLO_O_FORMAT) != ST_FMT_S32_LE || ch_count == 0 ||
          ch_count > kNetInputs) {
        reject(ST_HELLO_BAD_FORMAT);
        return false;
      }
      channels = ch_count;

      const uint16_t want = port_channel >= 0
                                ? static_cast<uint16_t>(port_channel)
                                : st_get_u16(p + ST_HELLO_O_WANT_CH);
      base = srv.claim_channels(want, ip, channels);
      if (base < 0) {
        reject(ST_HELLO_BUSY);
        return false;
      }

      std::string name;
      const uint32_t nlen = st_get_u32(p + ST_HELLO_O_NAME_LEN);
      if (nlen && ST_HELLO_FIXED + nlen <= len)
        name.assign(reinterpret_cast<const char*>(p + ST_HELLO_FIXED),
                    std::min<uint32_t>(nlen, ST_NET_MAX_NAME));
      for (unsigned i = 0; i < channels; ++i) {
        NetAudioServer::Channel& c = *srv.chans_[base + i];
        {
          std::lock_guard<std::mutex> lock(c.m);
          c.peer = peer;
          c.name = name;
          c.host.clear();
        }
        c.timeline.reset();
        // Mixable until this stream's FORMAT says otherwise; the last holder may have declared
        // the opposite.
        srv.ctl_.inputs[kInputs + base + i].bypass.store(false);
      }

      uint8_t ack[ST_HELLO_ACK_BYTES] = {0};
      st_put_u32(ack + ST_ACK_O_PROTO, kNetProtoVersion);
      st_put_u32(ack + ST_ACK_O_STATUS, ST_HELLO_OK);
      st_put_u32(ack + ST_ACK_O_RATE, static_cast<uint32_t>(srv.rate_));
      st_put_u32(ack + ST_ACK_O_CHANNEL, static_cast<uint32_t>(base));
      st_put_u32(ack + ST_ACK_O_MAX_LEAD, static_cast<uint32_t>(srv.rate_ * kNetTimelineMs / 2000.0));
      st_put_u32(ack + ST_ACK_O_COUNT, channels);
      if (!send_msg(fd, ST_MSG_HELLO_ACK, ack, sizeof(ack))) return false;

      LOG_INFO("net: {} ({}) took {} channel(s) from {} — input{} {}", peer,
               name.empty() ? "anonymous" : name, channels, base, channels > 1 ? "s" : "",
               kInputs + base);

      // Only now, with the handshake already answered, ask who this address belongs to. A
      // resolver with nothing to talk to blocks for seconds, and the sender is waiting.
      const std::string host = reverse_lookup(addr);
      if (!host.empty()) {
        for (unsigned i = 0; i < channels; ++i) {
          std::lock_guard<std::mutex> lock(srv.chans_[base + i]->m);
          srv.chans_[base + i]->host = host;
        }
      }
      return true;
}

bool NetAudioServer::Session::format(const uint8_t* p, uint32_t len) {

      if (len < ST_FORMAT_BYTES) return false;
      const uint32_t r = st_get_u32(p + ST_FMT_O_RATE);
      const uint32_t f = st_get_u32(p + ST_FMT_O_FORMAT);
      const uint32_t nch = st_get_u32(p + ST_FMT_O_CHANNELS);
      const uint32_t enc = st_get_u32(p + ST_FMT_O_ENCODING);
      const uint32_t sflags = st_get_u32(p + ST_FMT_O_FLAGS);
      if (r < ST_RATE_MIN || r > ST_RATE_MAX || st_fmt_bytes(f) == 0 || nch != channels ||
          (enc != ST_ENC_PCM && enc != ST_ENC_VORBIS)) {
        LOG_WARN("net: {} declared an unusable format ({} Hz, fmt {}, {} ch, enc {})", peer, r, f,
                 nch, enc);
        return false;
      }
      wire_fmt = f;
      wire_bytes = st_fmt_bytes(f);
      src_rate = r;
      encoding = enc;
      if (encoding != ST_ENC_VORBIS) vorbis.clear();
      // A rate or format change restarts the stream: the converter's history is of the old rate
      // and the output position no longer means anything.
      std::string aerr;
      if (!asrc.configure(channels, src_rate, srv.rate_, &aerr)) {
        LOG_WARN("net: {} cannot be resampled ({} Hz -> {} Hz): {}", peer, r, srv.rate_, aerr);
        return false;
      }
      out_next = 0;
      expect_pos = 0;
      lead.reset();

      // In FORMAT rather than the HELLO: it is a property of the stream, and one machine may
      // offer a volume-controlled PCM and an un-mixable one.
      const bool no_mixer = (sflags & ST_STREAM_F_NO_MIXER) != 0;
      for (unsigned i = 0; i < channels; ++i)
        srv.ctl_.inputs[kInputs + base + i].bypass.store(no_mixer);

      LOG_INFO("net: {} sends {} Hz {} x{}ch{}{}", peer, r,
               encoding == ST_ENC_VORBIS ? "vorbis" : "pcm", nch,
               r == static_cast<uint32_t>(srv.rate_) ? "" : " (resampled)",
               no_mixer ? ", mixer bypassed" : "");
      return true;
}

bool NetAudioServer::Session::codec_init(const uint8_t* p, uint32_t len) {

      if (len < ST_CODEC_FIXED) return false;
      const uint32_t count = st_get_u32(p + ST_CODEC_O_COUNT);
      if (count == 0 || count > ST_CODEC_MAX_PACKETS) return false;

      std::vector<std::vector<uint8_t>> headers;
      size_t off = ST_CODEC_FIXED;
      bool bad = false;
      for (uint32_t i = 0; i < count; ++i) {
        if (off + 4 > len) { bad = true; break; }
        const uint32_t plen = st_get_u32(p + off);
        off += 4;
        if (plen > len || off + plen > len) { bad = true; break; }
        headers.emplace_back(p + off, p + off + plen);
        off += plen;
      }
      if (bad) {
        LOG_WARN("net: {} sent a malformed codec header block", peer);
        return false;
      }

      std::string verr;
      if (!vorbis.init(headers, &verr)) {
        LOG_WARN("net: {} — {}", peer, verr);
        return false;
      }
      if (vorbis.channels() != static_cast<int>(channels)) {
        LOG_WARN("net: {} encoded {} channels but base {}", peer, vorbis.channels(), channels);
        return false;
      }
      // The encoder's own rate is what the decoder emits, so it, not the declared one, is what
      // the converter has to be told about.
      src_rate = static_cast<unsigned>(vorbis.rate());
      std::string aerr;
      if (!asrc.configure(channels, src_rate, srv.rate_, &aerr)) {
        LOG_WARN("net: {} cannot be resampled from {} Hz: {}", peer, src_rate, aerr);
        return false;
      }
      out_next = 0;
      expect_pos = 0;
      lead.reset();
      LOG_INFO("net: {} vorbis {} Hz x{}ch", peer, src_rate, vorbis.channels());
      return true;
}

bool NetAudioServer::Session::audio(const uint8_t* p, uint32_t len) {

      if (len < ST_AUDIO_FIXED) return false;
      const uint64_t pos = st_get_u64(p + ST_AUDIO_O_POS);
      const uint32_t frames = st_get_u32(p + ST_AUDIO_O_FRAMES);
      if (frames == 0 || frames > kNetMaxPacketFrames ||
          (encoding == ST_ENC_PCM &&
           ST_AUDIO_FIXED + 1ull * wire_bytes * frames * channels > len)) {
        LOG_WARN("net: {} sent a malformed audio packet ({} frames x {} ch x {} B, {} bytes)", peer,
                 frames, channels, wire_bytes, len);
        return false;
      }

      const uint64_t reader = srv.reader_n_.load();
      const uint64_t max_lead = static_cast<uint64_t>(srv.rate_ * kNetTimelineMs / 2000.0);
      const uint8_t* samples = p + ST_AUDIO_FIXED;

      // Into interleaved float, which is what the converter wants and what the rest of the path
      // works in. Whether that means unpacking a wire format or running a decoder is the only
      // difference the encoding makes anywhere below this point.
      in_il.clear();
      if (encoding == ST_ENC_VORBIS) {
        if (!vorbis.ready()) return true;  // audio before CODEC_INIT: nothing to decode it with
        size_t off = 0;
        while (off + 4 <= len - ST_AUDIO_FIXED) {
          const uint32_t plen = st_get_u32(samples + off);
          off += 4;
          if (plen > len || off + plen > len - ST_AUDIO_FIXED) break;
          std::string verr;
          if (vorbis.decode(samples + off, plen, &in_il, &verr) == 0 && !verr.empty()) {
            LOG_WARN("net: {} — {}", peer, verr);
          }
          off += plen;
        }
        if (in_il.empty()) return true;  // the decoder is still priming
      } else {
        in_il.resize(static_cast<size_t>(frames) * channels);
        for (size_t i = 0; i < in_il.size(); ++i)
          in_il[i] = st_sample_to_float(samples + 1ull * wire_bytes * i, wire_fmt);
      }
      const size_t in_frames = in_il.size() / channels;

      // Where this audio is heard is decided HERE, not by the sender. The stream is anchored the
      // moment its first packet arrives — at the reader's position plus the alignment delay — and
      // everything after it follows on contiguously. The sender never learns the card's clock.
      const uint64_t delay = srv.ctl_.net.delay_frames.load();

      // An INTERPOLATED reader position, not the raw one. srv.reader_n_ only moves when the audio
      // thread finishes a block, so measuring against it quantises the error by up to a whole
      // period — noise worth half the trim's authority, and none of it drift.
      const uint64_t reader_now = srv.ctl_.anchor.estimate(mono_ns(), srv.rate_);
      const uint64_t reader_pos = reader_now ? reader_now : reader;

      const bool sender_jumped = expect_pos != 0 && pos != expect_pos;
      // Too far out to walk back: at the trim's authority a quarter second would take two
      // minutes, and being wrong for two minutes is worse than one discontinuity now. Judged on
      // the filtered value, so a moment's jitter cannot trigger it.
      const bool adrift = out_next != 0 && lead.primed &&
                          std::fabs(lead.avg - static_cast<double>(delay)) > kNetResyncFrames;

      if (out_next == 0 || sender_jumped || adrift) {
        if (sender_jumped) {
          LOG_WARN("net: {} skipped {} frames — re-anchoring", peer,
                   static_cast<int64_t>(pos) - static_cast<int64_t>(expect_pos));
        } else if (adrift) {
          LOG_WARN("net: {} drifted {:.0f} ms past what the ratio can pull back — re-anchoring",
                   peer, 1000.0 * (lead.avg - static_cast<double>(delay)) / srv.rate_);
          for (unsigned c = 0; c < channels; ++c)
            srv.chans_[base + c]->resyncs.fetch_add(1, std::memory_order_relaxed);
        }
        asrc.reset();
        lead.reset();
        out_next = reader_pos + delay;
      }
      expect_pos = pos + frames;

      // Measured only now, and never before the anchor above: until out_next means something,
      // "lead" is out_next minus the reader with out_next still zero, which is not a small error
      // but a nonsensical one — and priming the filter with it costs a spurious resync on every
      // stream that starts.
      const double lead_now = static_cast<double>(static_cast<int64_t>(out_next) -
                                                  static_cast<int64_t>(reader_pos));

      // The one control loop. out_next runs ahead of the reader by the alignment delay when the
      // two machines agree; every way in which they do not — a crystal a hundred ppm out, a rate
      // the card does not run at — shows up here, and is taken out by nudging the converter's
      // ratio rather than by a correction applied anywhere as a step.
      const double dt_s = src_rate ? static_cast<double>(frames) / src_rate : 0.0;
      const double lead_avg = lead.update(lead_now, dt_s, kNetLeadFilterTauS);
      const double trim = asrc_trim(lead_avg, static_cast<double>(delay), srv.rate_, kAsrcTauS,
                                    kAsrcTrimMax);

      out_il.clear();
      std::string aerr;
      const size_t out_frames = asrc.process(in_il.data(), in_frames, trim, &out_il, &aerr);
      if (out_frames == 0) return true;  // the converter is still priming
      const float* src = out_il.data();
      const uint64_t write_at = out_next;
      out_next += out_frames;

      distribute(src, out_frames, write_at, reader, max_lead);

      // Report on a fixed cadence. Audio packets arrive every few milliseconds, so hanging the
      // timer off them is both simple and reliable — no extra thread, and it stops on its own the
      // moment a sender goes quiet, which is when there is nothing to report anyway.
      const uint64_t now = mono_ns();
      if (now - last_status_ns >= ST_STATUS_INTERVAL_MS * 1000000ull) {
        last_status_ns = now;
        if (!status()) return false;
      }
      return true;
}

// What the device knows and the sender cannot: where its audio actually landed relative to
// playout. Nothing on the far end steers anything with it — the device disciplines its own
// converter — but snd_pcm_delay() needs it to tell an application the truth.
void NetAudioServer::Session::distribute(const float* src, size_t frames, uint64_t at,
                                         uint64_t reader, uint64_t max_lead) {
  chan.resize(frames);
  for (unsigned c = 0; c < channels; ++c) {
    NetAudioServer::Channel& ch = *srv.chans_[base + c];
    float peak = 0.0f;
    for (size_t i = 0; i < frames; ++i) {
      const float v = src[i * channels + c];
      chan[i] = v;
      peak = std::max(peak, std::fabs(v));
    }

    switch (ch.timeline.write(at, chan.data(), frames, reader, kGuardFrames,
                              max_lead)) {
      case NetTimeline::Write::Ok: {
        ch.frames_received.fetch_add(frames, std::memory_order_relaxed);
        ch.last_target.store(at, std::memory_order_relaxed);
        ch.lead_avg.store(static_cast<int64_t>(lead.avg), std::memory_order_relaxed);
        ch.lead_valid.store(true, std::memory_order_relaxed);
        {
          // Peak HOLD, not the last packet's peak: a tick train is mostly silence, so a
          // last-packet reading answers "is anything arriving?" with a confident no.
          const uint32_t pk = static_cast<uint32_t>(peak * 1000.0f);
          uint32_t pprev = ch.peak_milli.load(std::memory_order_relaxed);
          while (pk > pprev && !ch.peak_milli.compare_exchange_weak(pprev, pk,
                                                                    std::memory_order_relaxed)) {
          }
        }
        break;
      }
      case NetTimeline::Write::Late:
        ch.late_drops.fetch_add(1, std::memory_order_relaxed);
        break;
      case NetTimeline::Write::OutOfRange:
        ch.range_drops.fetch_add(1, std::memory_order_relaxed);
        break;
    }
  }
}

bool NetAudioServer::Session::status() {
  if (base < 0) return true;
  NetAudioServer::Channel& c = *srv.chans_[base];
  const bool have = c.lead_valid.load(std::memory_order_relaxed);
  const int64_t lead_now = c.lead_avg.load(std::memory_order_relaxed);

  uint8_t st[ST_STATUS_BYTES] = {0};
  st_put_u64(st + ST_STATUS_O_N_NOW, srv.reader_n_.load());
  st_put_u64(st + ST_STATUS_O_WRITE_END, c.timeline.write_end());
  st_put_u32(st + ST_STATUS_O_LATE, static_cast<uint32_t>(c.late_drops.load()));
  st_put_u32(st + ST_STATUS_O_RANGE, static_cast<uint32_t>(c.range_drops.load()));
  st_put_u32(st + ST_STATUS_O_UNDER, static_cast<uint32_t>(c.underruns.load()));
  st_put_u32(st + ST_STATUS_O_LEAD,
             static_cast<uint32_t>(static_cast<int32_t>(have ? lead_now : 0)));
  st_put_u32(st + ST_STATUS_O_TARGET_LEAD, srv.ctl_.net.delay_frames.load());
  st_put_u32(st + ST_STATUS_O_FLAGS, have ? ST_STATUS_F_LEAD : 0u);
  return send_msg(fd, ST_MSG_STATUS, st, sizeof(st));
}

void NetAudioServer::serve(int fd, sockaddr_in addr, int port_channel) {
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  enable_keepalive(fd);

  char ipbuf[INET_ADDRSTRLEN] = {0};
  inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));

  Session s(*this);
  s.fd = fd;
  s.ip = ipbuf;
  s.peer = s.ip + ":" + std::to_string(ntohs(addr.sin_port));
  s.port_channel = port_channel;

  std::vector<uint8_t> payload;
  while (running_.load()) {
    uint8_t hdr[ST_NET_HEADER_BYTES];
    if (!read_exact(fd, hdr, sizeof(hdr), running_)) break;
    const uint8_t type = st_header_type(hdr);
    const uint32_t len = st_header_len(hdr);
    if (len > ST_NET_MAX_PAYLOAD) {
      LOG_WARN("net: {} sent an oversized {}-byte payload", s.peer, len);
      break;
    }
    payload.resize(len);
    if (len && !read_exact(fd, payload.data(), len, running_)) break;
    const uint8_t* p = payload.data();

    // A mixer is a different kind of connection and takes over the socket for its lifetime: it
    // must not consume one of the device's network inputs just to look at a volume.
    if (type == ST_MSG_CTL_HELLO) {
      if (s.base >= 0 || len < ST_CTL_HELLO_BYTES) break;
      if (st_get_u32(p + ST_CTLH_O_MAGIC) != ST_NET_MAGIC ||
          st_get_u32(p + ST_CTLH_O_PROTO) != kNetProtoVersion) {
        break;
      }
      serve_ctl(fd, s.ip, port_channel >= 0 ? static_cast<uint16_t>(port_channel)
                                            : st_get_u16(p + ST_CTLH_O_CHANNEL));
      ::close(fd);
      return;
    }

    if (type == ST_MSG_HELLO) {
      if (!s.hello(p, len, addr)) break;
      continue;
    }
    // Nothing but a handshake is legal before a channel has been granted.
    if (s.base < 0) break;

    if (type == ST_MSG_FORMAT) {
      if (!s.format(p, len)) break;
    } else if (type == ST_MSG_CODEC_INIT) {
      if (!s.codec_init(p, len)) break;
    } else if (type == ST_MSG_AUDIO) {
      if (!s.audio(p, len)) break;
    } else if (type == ST_MSG_BYE) {
      break;
    }
    // Anything else is skipped rather than fatal, so a newer sender talking to an older device
    // degrades instead of disconnecting.
  }

  if (s.base >= 0) {
    LOG_INFO("net: {} released {} channel(s) from {}", s.peer, s.channels, s.base);
    release_channels(static_cast<unsigned>(s.base), s.channels);
  }
  ::close(fd);
}

void NetAudioServer::read_block(uint64_t n, size_t frames, uint64_t delay, float* live,
                                float* ring) {
  // Publish the PLAYOUT position, not the trailing one: a packet is late once its slot has gone
  // past the point where it would have been emitted, whatever the ring is still catching up on.
  reader_n_.store(n, std::memory_order_release);

  for (unsigned c = 0; c < kNetInputs; ++c) {
    Channel& ch = *chans_[c];
    float* lp = live + (kInputs + c);
    float* rp = ring + (kInputs + c);

    if (!ch.claimed.load(std::memory_order_relaxed)) {
      for (size_t i = 0; i < frames; ++i) lp[i * kTotalInputs] = 0.0f;
      if (ring != live)
        for (size_t i = 0; i < frames; ++i) rp[i * kTotalInputs] = 0.0f;
      continue;
    }

    // Only once the sender has actually supplied something: the gap between claiming a channel
    // and the first packet landing is the playout lead working as intended, not an underrun.
    const uint64_t end = ch.timeline.write_end();
    if (end != 0 && n + frames > end) {
      ch.underruns.fetch_add(1, std::memory_order_relaxed);
    }

    // The live read must come first and must not clear, or the trailing read would find the
    // frames already gone.
    ch.timeline.read(n, frames, lp, kTotalInputs, delay == 0);
    if (ring == live) continue;

    if (n < delay) {
      for (size_t i = 0; i < frames; ++i) rp[i * kTotalInputs] = 0.0f;
    } else {
      ch.timeline.read(n - delay, frames, rp, kTotalInputs, true);
    }
  }
}

int NetAudioServer::ctl_channel_for(const std::string& ip, uint16_t want) const {
  if (want < kNetInputs) return static_cast<int>(want);
  // No channel asked for: control whichever one this machine is streaming on, or last streamed
  // on. That is the same address memory the audio side uses to hand a channel back, so a mixer
  // and its PCM end up on the same channel without either being told which.
  for (unsigned c = 0; c < kNetInputs; ++c) {
    std::lock_guard<std::mutex> lock(chans_[c]->m);
    if (chans_[c]->last_ip == ip) return static_cast<int>(c);
  }
  return 0;  // nothing known about this machine yet; the ack says which channel it got
}

NetAudioServer::MixRun NetAudioServer::mixer_run(const std::string& ip, uint16_t want) const {
  const int ch = ctl_channel_for(ip, want);
  MixRun r;
  {
    std::lock_guard<std::mutex> lock(chans_[ch]->m);
    r.base = chans_[ch]->stream_base;
    r.count = chans_[ch]->stream_count;
  }
  // A channel that is not inside the run it names has never been part of one — an untouched
  // channel still carries the {0, 1} its Channel was built with — so it is a run of its own.
  const unsigned c = static_cast<unsigned>(ch);
  if (r.count == 0 || r.base + r.count > kNetInputs || c < r.base || c >= r.base + r.count) {
    r.base = c;
    r.count = 1;
  }
  return r;
}

void NetAudioServer::serve_ctl(int fd, const std::string& ip, uint16_t want) {
  MixRun run = mixer_run(ip, want);

  // The run's first channel is the reference. Everything below reads and writes through `run`,
  // which moves under it.
  auto gain_db = [&] { return ctl_.inputs[kInputs + run.base].gain_db.load(); };
  auto gain_cdb = [&] { return static_cast<int32_t>(std::lround(gain_db() * 100.0f)); };
  auto muted = [&] { return ctl_.inputs[kInputs + run.base].mute.load(); };
  auto bypassed = [&] { return ctl_.inputs[kInputs + run.base].bypass.load(); };
  auto flags = [&] { return bypassed() ? ST_MIX_ST_BYPASS : 0u; };
  auto set_all = [&](bool set_gain, float db, bool set_mute, bool m) {
    for (unsigned i = 0; i < run.count; ++i) {
      if (set_gain) ctl_.inputs[kInputs + run.base + i].gain_db.store(db);
      if (set_mute) ctl_.inputs[kInputs + run.base + i].mute.store(m);
    }
  };

  uint8_t ack[ST_CTL_ACK_BYTES] = {0};
  st_put_u32(ack + ST_CTLA_O_PROTO, kNetProtoVersion);
  st_put_u32(ack + ST_CTLA_O_STATUS, ST_HELLO_OK);
  st_put_u32(ack + ST_CTLA_O_CHANNEL, run.base);
  st_put_u32(ack + ST_CTLA_O_GAIN_MIN, static_cast<uint32_t>(static_cast<int32_t>(kNetGainMinDb * 100)));
  st_put_u32(ack + ST_CTLA_O_GAIN_MAX, static_cast<uint32_t>(static_cast<int32_t>(0)));
  st_put_u32(ack + ST_CTLA_O_GAIN, static_cast<uint32_t>(gain_cdb()));
  st_put_u32(ack + ST_CTLA_O_MUTE, muted() ? 1u : 0u);
  st_put_u32(ack + ST_CTLA_O_COUNT, run.count);
  st_put_u32(ack + ST_CTLA_O_FLAGS, flags());
  if (!send_msg(fd, ST_MSG_CTL_ACK, ack, sizeof(ack))) return;
  LOG_INFO("net: mixer from {} attached to {} channel(s) from {} — input {}", ip, run.count,
           run.base, kInputs + run.base);

  int32_t sent_gain = gain_cdb();
  bool sent_mute = muted();
  uint32_t sent_flags = flags();

  while (running_.load()) {
    pollfd pfd{fd, POLLIN, 0};
    const int pr = poll(&pfd, 1, ST_MIX_POLL_MS);
    if (pr < 0 && errno != EINTR) break;

    if (pr > 0 && (pfd.revents & POLLIN)) {
      uint8_t hdr[ST_NET_HEADER_BYTES];
      if (!read_exact(fd, hdr, sizeof(hdr), running_)) break;
      const uint32_t len = st_header_len(hdr);
      if (len > ST_NET_MAX_PAYLOAD) break;
      std::vector<uint8_t> pl(len);
      if (len && !read_exact(fd, pl.data(), len, running_)) break;

      if (st_header_type(hdr) == ST_MSG_BYE) break;
      // An un-mixable stream ignores the write rather than storing a value that does not apply;
      // the next push snaps the mixer back.
      if (st_header_type(hdr) == ST_MSG_SET_MIX && len >= ST_SET_MIX_BYTES && !bypassed()) {
        const uint32_t mask = st_get_u32(pl.data() + ST_SETMIX_O_MASK);
        const auto cdb = static_cast<int32_t>(st_get_u32(pl.data() + ST_SETMIX_O_GAIN));
        set_all(mask & ST_MIX_F_GAIN, std::clamp(cdb / 100.0f, kNetGainMinDb, kInputGainMaxDb),
                mask & ST_MIX_F_MUTE, st_get_u32(pl.data() + ST_SETMIX_O_MUTE) != 0);
      }
    }

    // Follow the sender: a mixer opened before anything played attached to one channel, and once
    // that machine takes a stereo run the one volume has to cover both. The value it is showing
    // goes to the channels that just joined, or the pair comes apart at the moment it starts
    // playing — the mixer's -20 dB on NET 1 while NET 2 runs at full scale.
    const MixRun now = mixer_run(ip, want);
    if (now.base != run.base || now.count != run.count) {
      const float db = gain_db();
      const bool m = muted();
      run = now;
      if (!bypassed()) set_all(true, db, true, m);
      LOG_INFO("net: mixer from {} now drives {} channel(s) from {} — input {}", ip, run.count,
               run.base, kInputs + run.base);
    }

    // Push whatever the value is now, whoever changed it. This is what lets a slider moved in the
    // web console show up in an open alsamixer, and it costs one comparison per 100 ms.
    const int32_t g = gain_cdb();
    const bool m = muted();
    const uint32_t f = flags();
    if (g != sent_gain || m != sent_mute || f != sent_flags) {
      uint8_t mix[ST_MIX_BYTES] = {0};
      st_put_u32(mix + ST_MIX_O_GAIN, static_cast<uint32_t>(g));
      st_put_u32(mix + ST_MIX_O_MUTE, m ? 1u : 0u);
      st_put_u32(mix + ST_MIX_O_FLAGS, f);
      if (!send_msg(fd, ST_MSG_MIX, mix, sizeof(mix))) break;
      sent_gain = g;
      sent_mute = m;
      sent_flags = f;
    }
  }
  LOG_INFO("net: mixer from {} detached from channel {}", ip, run.base);
}

bool NetAudioServer::channel_in_use(unsigned c) const {
  if (c >= kNetInputs) return false;
  const Channel& ch = *chans_[c];
  return ch.claimed.load(std::memory_order_relaxed) ||
         ch.frames_received.load(std::memory_order_relaxed) != 0;
}

std::vector<NetChannelStatus> NetAudioServer::status() const {
  std::vector<NetChannelStatus> out;
  out.reserve(kNetInputs);
  for (unsigned c = 0; c < kNetInputs; ++c) {
    const Channel& ch = *chans_[c];
    NetChannelStatus s;
    s.channel = c;
    s.connected = ch.claimed.load();
    s.frames_received = ch.frames_received.load();
    s.late_drops = ch.late_drops.load();
    s.range_drops = ch.range_drops.load();
    s.underruns = ch.underruns.load();
    s.resyncs = ch.resyncs.load();
    s.last_target = ch.last_target.load();
    s.write_end = ch.timeline.write_end();
    s.lead_frames = ch.lead_avg.load();
    s.lead_valid = ch.lead_valid.load();
    s.target_lead_frames = ctl_.net.delay_frames.load();
    s.peak = ch.peak_milli.load() / 1000.0f;
    {
      std::lock_guard<std::mutex> lock(ch.m);
      s.peer = ch.peer;
      s.name = ch.name;
      s.host = ch.host;
      // Only while something is actually on the channel. The fallback inside device_locked() is
      // the peer address, which after a disconnect would shadow last_device — the remembered
      // hostname, which is the more useful of the two.
      s.device = s.connected ? ch.device_locked() : std::string{};
      s.stream_index = ch.stream_index;
      s.stream_count = ch.stream_count;
      s.last_device = ch.last_device;
    }
    out.push_back(std::move(s));
  }
  return out;
}

}  // namespace st
