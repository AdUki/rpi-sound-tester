/*
 * ALSA external PCM plugin: send audio to an rpi-sound-tester over the network.
 *
 *   aplay -D soundtester:192.168.1.42 tone.wav
 *   pcm.bench { type soundtester; host soundtester.local }
 *
 * A drop-in for `hw`: it advertises the usual rates and S16/S24/S32/float, and the device
 * converts and resamples whatever arrives.
 *
 * Frames are released against the local monotonic clock, and the position is a function of that
 * clock alone — not of anything having polled us, so a client that schedules on its own timer
 * drives it too. The device is optional: the PCM opens, paces and drains with nothing listening
 * while a connector thread keeps trying.
 *
 * The sender never learns the card's clock: a packet carries only how far into the stream it is,
 * and the device anchors that on arrival and trims its own converter to hold the lead.
 */

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "net_proto.h"
#include "plugin_common.h"
#ifdef ST_HAVE_VORBIS
#include "vorbis_enc.h"
#endif

#define FD_TIMER 0
#define FD_COUNT 1

/* Release interval: the application's own period, bounded. One timer does the job of both the
 * period interrupt and the DMA, and whatever a tick releases leaves in one burst — 20 ms is as
 * bursty as the device's alignment tolerates. */
#define TICK_MIN_NS (5 * 1000 * 1000L)
#define TICK_MAX_NS (20 * 1000 * 1000L)

/* After a stall, pay back at most this much in one go — the rest is left to the device's
 * converter. The real limit is two of the application's own periods (`catchup_ns`): a client that
 * asks for the position once per period is keeping its cadence, not stalling. */
#define MIN_CATCHUP_NS (4 * TICK_MIN_NS)

/* How long a send may wait on the far end before the link counts as gone. Bounded: this runs on
 * the application's audio path. */
#define SEND_TIMEOUT_MS 20

#define CONNECT_TIMEOUT_MS 2000
#define ACK_TIMEOUT_MS 2000
#define RETRY_MS 1000

/* Stands in for the device's answer until the HELLO_ACK brings the real one. */
#define ASSUMED_MAX_LEAD (ST_DEFAULT_RATE / 2)

#define RXBUF 8192

typedef struct {
  snd_pcm_ioplug_t io;

  char *host;
  long port;
  long want_channel;
  long channels;   /* each becomes its own input on the device */
  long encoding;   /* ST_ENC_PCM or ST_ENC_VORBIS */
  double quality;  /* Vorbis VBR quality, -0.1 .. 1.0 */
  int mixer;       /* 0: the device leaves this stream's level alone (see `mixer` in the conf) */

  int sk_fd;       /* the live link, or -1. Owned by the audio path once adopted. */
  int timer_fd;
  int wake_fd;     /* eventfd: nudges the connector to retry, or to quit */

  pthread_t link_th;
  int link_running;
  atomic_int handoff_fd;  /* connector -> audio path: a finished socket, or -1 */
  unsigned int handoff_rate;
  uint64_t handoff_max_lead;
  atomic_int want_link;   /* audio path -> connector: nothing is connected */
  atomic_int quit;

  unsigned int rate;      /* the card's rate, for reporting only; 0 until a device has answered */
  unsigned int pcm_rate;  /* what the application settled on; the device resamples the difference */
  unsigned int wire_fmt;
  unsigned int sample_bytes;
  int declared;           /* this link has been told what this stream is */

  uint64_t max_lead;      /* most the device accepts in one go */

  uint64_t last_tick_ns;
  uint64_t catchup_ns;    /* most one advance() may release, in elapsed time */
  double frac;            /* sub-frame remainder, so the average release rate is exact */

  uint64_t sent_pcm;    /* frames released on THIS link — this is what goes on the wire */
  int64_t lead_frames;  /* what the device last reported, for snd_pcm_delay() */
  snd_pcm_uframes_t hw_ptr;
  snd_pcm_uframes_t min_avail;
  snd_pcm_uframes_t boundary;
  int xrun;
  int started;

  const snd_pcm_channel_area_t *areas;

#ifdef ST_HAVE_VORBIS
  st_vorbis_enc venc;
#endif
  int venc_loaded;
  uint64_t pend_pos;      /* first frame not yet covered by a sent packet */
  uint64_t pend_frames;   /* frames fed to the encoder since the last message went out */
  uint8_t enc_buf[1 << 16];

  uint8_t rx[RXBUF];
  size_t rx_len;
  uint8_t tx[ST_AUDIO_FIXED + 4 * ST_PACKET_FRAMES * ST_MAX_STREAM_CHANNELS];
} st_pcm_t;

/* ---- helpers -------------------------------------------------------------------------- */

static uint64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Only feeds snd_pcm_delay(). Nothing here servos on the lead; the device does that itself. */
static void apply_status(st_pcm_t *st, const uint8_t *p) {
  if (st_get_u32(p + ST_STATUS_O_FLAGS) & ST_STATUS_F_LEAD)
    st->lead_frames = (int32_t)st_get_u32(p + ST_STATUS_O_LEAD);
}

/* Runs on the audio path, so it must not block. -1 means the far end has gone. */
static int pump_rx(st_pcm_t *st) {
  for (;;) {
    if (st->rx_len == sizeof(st->rx)) st->rx_len = 0; /* desynchronised; resynchronise cheaply */
    ssize_t r = recv(st->sk_fd, st->rx + st->rx_len, sizeof(st->rx) - st->rx_len, MSG_DONTWAIT);
    if (r == 0) return -1; /* clean close */
    if (r < 0) {
      if (errno == EINTR) continue;
      return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    }
    st->rx_len += (size_t)r;

    size_t off = 0;
    while (st->rx_len - off >= ST_NET_HEADER_BYTES) {
      const uint8_t *h = st->rx + off;
      uint32_t len = st_header_len(h);
      if (len > ST_NET_MAX_PAYLOAD) {
        st->rx_len = 0;
        return 0;
      }
      if (st->rx_len - off < ST_NET_HEADER_BYTES + len) break;
      const uint8_t *pl = h + ST_NET_HEADER_BYTES;
      if (st_header_type(h) == ST_MSG_STATUS && len >= ST_STATUS_BYTES) apply_status(st, pl);
      off += ST_NET_HEADER_BYTES + len;
    }
    if (off) {
      memmove(st->rx, st->rx + off, st->rx_len - off);
      st->rx_len -= off;
    }
  }
}

/* An encoder answers no packet for some inputs, so the frames a message covers accumulate until it
 * does: stream_pos and frames must stay contiguous, or the device reads a hole that is not there. */
static int encode_and_send(st_pcm_t *st, const uint8_t *src, unsigned int frames) {
#ifndef ST_HAVE_VORBIS
  (void)st; (void)src; (void)frames;
  return -EIO;
#else
  st_vorbis_enc *v = &st->venc;
  if (!v->ready) return 0;

  float **buf = vorbis_analysis_buffer(&v->vd, (int)frames);
  if (!buf) return -EIO;
  for (unsigned int i = 0; i < frames; i++) {
    for (long c = 0; c < st->channels; c++) {
      buf[c][i] = st_sample_to_float(src + ((size_t)i * st->channels + c) * st->sample_bytes, st->wire_fmt);
    }
  }
  vorbis_analysis_wrote(&v->vd, (int)frames);
  st->pend_frames += frames;

  size_t off = ST_AUDIO_FIXED;
  unsigned packets = 0;
  while (vorbis_analysis_blockout(&v->vd, &v->vb) == 1) {
    vorbis_analysis(&v->vb, NULL);
    vorbis_bitrate_addblock(&v->vb);
    ogg_packet op;
    while (vorbis_bitrate_flushpacket(&v->vd, &op) == 1) {
      if (off + 4 + (size_t)op.bytes > sizeof(st->enc_buf)) break;  /* absurdly large; drop it */
      st_put_u32(st->enc_buf + off, (uint32_t)op.bytes);
      off += 4;
      memcpy(st->enc_buf + off, op.packet, (size_t)op.bytes);
      off += (size_t)op.bytes;
      packets++;
    }
  }
  if (packets == 0) return 0;  /* still filling a block: the frames roll into the next message */

  st_put_u64(st->enc_buf + ST_AUDIO_O_POS, st->pend_pos);
  st_put_u32(st->enc_buf + ST_AUDIO_O_FRAMES, (uint32_t)st->pend_frames);
  if (st_send_msg(st->sk_fd, ST_MSG_AUDIO, st->enc_buf, (uint32_t)off, SEND_TIMEOUT_MS) < 0)
    return -EIO;
  st->pend_pos += st->pend_frames;
  st->pend_frames = 0;
  return 0;
#endif
}

static int send_pcm(st_pcm_t *st, const uint8_t *src, unsigned int frames) {
  const size_t frame_bytes = (size_t)st->sample_bytes * (size_t)st->channels;
  st_put_u64(st->tx + ST_AUDIO_O_POS, st->sent_pcm);
  st_put_u32(st->tx + ST_AUDIO_O_FRAMES, frames);
  memcpy(st->tx + ST_AUDIO_FIXED, src, (size_t)frames * frame_bytes);
  return st_send_msg(st->sk_fd, ST_MSG_AUDIO, st->tx,
                     (uint32_t)(ST_AUDIO_FIXED + (size_t)frames * frame_bytes), SEND_TIMEOUT_MS);
}

/* ---- the link -------------------------------------------------------------------------- */

static void link_wake(st_pcm_t *st) {
  const uint64_t one = 1;
  if (st->wake_fd >= 0) {
    ssize_t w = write(st->wake_fd, &one, sizeof(one));
    (void)w;
  }
}

/* Interruptible: a close has to be noticed at once, not a second later. */
static void link_nap(st_pcm_t *st, int ms) {
  struct pollfd pfd = {st->wake_fd, POLLIN, 0};
  if (poll(&pfd, 1, ms) > 0) {
    uint64_t v;
    while (read(st->wake_fd, &v, sizeof(v)) > 0) {
    }
  }
}

/* Connector thread. Returns a handshaken socket, or fills `why` for the caller to report — a retry
 * loop must not repeat itself. */
static int link_connect(st_pcm_t *st, unsigned int *rate, uint64_t *max_lead, char *why,
                        size_t why_n) {
  int fd = st_connect(st->host, st->port, NULL, CONNECT_TIMEOUT_MS, st->wake_fd);
  if (fd < 0) {
    snprintf(why, why_n, "%s:%ld is not answering", st->host, st->port);
    return fd;
  }

  /* Names this machine in the device's console; it falls back to a reverse lookup, then the
   * address. */
  char name[ST_NET_MAX_NAME];
  if (gethostname(name, sizeof(name)) != 0 || name[0] == '\0')
    snprintf(name, sizeof(name), "alsa-plugin");
  name[sizeof(name) - 1] = '\0';
  uint32_t nlen = (uint32_t)strlen(name);

  uint8_t hello[ST_HELLO_FIXED + ST_NET_MAX_NAME];
  memset(hello, 0, sizeof(hello));
  st_put_u32(hello + ST_HELLO_O_MAGIC, ST_NET_MAGIC);
  st_put_u32(hello + ST_HELLO_O_PROTO, ST_NET_PROTO_VERSION);
  /* Rate 0 means "tell me yours"; ours is declared later, in FORMAT. */
  st_put_u32(hello + ST_HELLO_O_RATE, 0);
  st_put_u32(hello + ST_HELLO_O_FORMAT, ST_FMT_S32_LE);
  st_put_u16(hello + ST_HELLO_O_CHANNELS, (uint16_t)st->channels);
  st_put_u16(hello + ST_HELLO_O_WANT_CH,
             st->want_channel < 0 ? ST_HELLO_ANY_CHANNEL : (uint16_t)st->want_channel);
  st_put_u32(hello + ST_HELLO_O_NAME_LEN, nlen);
  memcpy(hello + ST_HELLO_FIXED, name, nlen);

  uint8_t ack[ST_HELLO_ACK_BYTES];
  uint32_t len = 0;
  int type = -1;
  if (st_send_msg(fd, ST_MSG_HELLO, hello, ST_HELLO_FIXED + nlen, ACK_TIMEOUT_MS) == 0)
    type = st_read_msg(fd, ack, sizeof(ack), &len, ACK_TIMEOUT_MS);
  if (type != ST_MSG_HELLO_ACK || len != ST_HELLO_ACK_BYTES) {
    snprintf(why, why_n, "%s did not complete the handshake", st->host);
    close(fd);
    return -EIO;
  }

  uint32_t status = st_get_u32(ack + ST_ACK_O_STATUS);
  if (status == ST_HELLO_OK) {
    *rate = st_get_u32(ack + ST_ACK_O_RATE);
    *max_lead = st_get_u32(ack + ST_ACK_O_MAX_LEAD);
    return fd;
  }
  if (status == ST_HELLO_BUSY)
    snprintf(why, why_n, "every network input on %s is already in use", st->host);
  else if (status == ST_HELLO_DISABLED)
    snprintf(why, why_n, "network input is switched off on %s (enable it in Configuration)",
             st->host);
  else
    snprintf(why, why_n, "%s refused the connection (status %u)", st->host, status);
  close(fd);
  return -EIO;
}

static void *link_thread(void *arg) {
  st_pcm_t *st = (st_pcm_t *)arg;
  int said = 0;     /* this outage has been reported */
  int ever_up = 0;  /* the first connection of all is silent; a recovery is not */

  while (!atomic_load(&st->quit)) {
    if (!atomic_load(&st->want_link)) {
      link_nap(st, RETRY_MS);
      continue;
    }

    unsigned int rate = 0;
    uint64_t max_lead = 0;
    char why[160];
    why[0] = '\0';
    int fd = link_connect(st, &rate, &max_lead, why, sizeof(why));
    if (atomic_load(&st->quit)) {
      if (fd >= 0) close(fd);
      break;
    }
    if (fd < 0) {
      if (!said) {
        SNDERR("soundtester: %s — the stream plays on, still trying", why);
        said = 1;
      }
      link_nap(st, RETRY_MS);
      continue;
    }
    if (said || ever_up) SNDERR("soundtester: connected to %s:%ld", st->host, st->port);
    said = 0;
    ever_up = 1;

    /* Read by the audio path only after the release store below hands it the socket. */
    st->handoff_rate = rate;
    st->handoff_max_lead = max_lead;
    atomic_store(&st->want_link, 0);
    atomic_store_explicit(&st->handoff_fd, fd, memory_order_release);
  }
  return NULL;
}

/* The link is gone; the stream is not. A network fault must never reach the application as an
 * xrun. */
static void link_drop(st_pcm_t *st) {
  if (st->sk_fd >= 0) {
    SNDERR("soundtester: lost the connection to %s — the stream plays on", st->host);
    close(st->sk_fd);
    st->sk_fd = -1;
  }
  st->declared = 0;
  st->lead_frames = 0;
  st->rx_len = 0;
  atomic_store(&st->want_link, 1);
  link_wake(st);
}

#ifdef ST_HAVE_VORBIS
static int vorbis_restart(st_pcm_t *st) {
  const char *err =
      st_vorbis_start(&st->venc, (int)st->channels, (long)st->pcm_rate, (float)st->quality);
  if (err) {
    SNDERR("soundtester: %s", err);
    return -EINVAL;
  }
  st->venc_loaded = 1;
  return 0;
}

/* The setup packets, once per connection and before any audio: a device that joined mid-stream has
 * never seen them. */
static int send_codec_init(st_pcm_t *st) {
  ogg_packet oh, oc, ob;
  if (vorbis_analysis_headerout(&st->venc.vd, &st->venc.vc, &oh, &oc, &ob) != 0) {
    SNDERR("soundtester: libvorbis would not produce its setup packets");
    return -EIO;
  }
  const ogg_packet *hdrs[3] = {&oh, &oc, &ob};
  size_t off = ST_CODEC_FIXED;
  st_put_u32(st->enc_buf + ST_CODEC_O_COUNT, 3);
  for (int i = 0; i < 3; i++) {
    if (off + 4 + (size_t)hdrs[i]->bytes > sizeof(st->enc_buf)) return -EIO;
    st_put_u32(st->enc_buf + off, (uint32_t)hdrs[i]->bytes);
    off += 4;
    memcpy(st->enc_buf + off, hdrs[i]->packet, (size_t)hdrs[i]->bytes);
    off += (size_t)hdrs[i]->bytes;
  }
  return st_send_msg(st->sk_fd, ST_MSG_CODEC_INIT, st->enc_buf, (uint32_t)off, SEND_TIMEOUT_MS);
}
#endif

/* Tells a freshly connected device what this stream is. hw_params settles the format long after
 * the socket could have opened, so it cannot ride in the HELLO, and a reconnection says it again. */
static int declare(st_pcm_t *st) {
  uint8_t f[ST_FORMAT_BYTES];
  memset(f, 0, sizeof(f));
  st_put_u32(f + ST_FMT_O_RATE, st->pcm_rate);
  st_put_u32(f + ST_FMT_O_FORMAT, st->wire_fmt);
  st_put_u32(f + ST_FMT_O_CHANNELS, (uint32_t)st->channels);
  st_put_u32(f + ST_FMT_O_ENCODING, (uint32_t)st->encoding);
  st_put_u32(f + ST_FMT_O_FLAGS, st->mixer ? 0u : ST_STREAM_F_NO_MIXER);
  if (st_send_msg(st->sk_fd, ST_MSG_FORMAT, f, sizeof(f), SEND_TIMEOUT_MS) < 0) return -EIO;

#ifdef ST_HAVE_VORBIS
  if (st->encoding == ST_ENC_VORBIS) {
    /* A new connection is a new Vorbis stream: headers have to describe the packets that follow. */
    if (vorbis_restart(st) < 0 || send_codec_init(st) < 0) return -EIO;
  }
#endif
  st->pend_pos = 0;
  st->pend_frames = 0;
  st->declared = 1;
  return 0;
}

/* Adopts whatever the connector has finished with, and makes sure the far end has been told what
 * this stream is. `sent_pcm` restarts at 0 per connection: the device anchors a stream where its
 * first packet lands, so a link that comes up mid-file needs no catching up. */
static void link_service(st_pcm_t *st) {
  const int fd = atomic_exchange_explicit(&st->handoff_fd, -1, memory_order_acquire);
  if (fd >= 0) {
    st->sk_fd = fd;
    st->rate = st->handoff_rate;
    st->max_lead = st->handoff_max_lead ? st->handoff_max_lead : ASSUMED_MAX_LEAD;
    st->sent_pcm = 0;
    st->lead_frames = 0;
    st->rx_len = 0;
    st->declared = 0;
  }
  if (st->sk_fd >= 0 && !st->declared && st->pcm_rate && declare(st) < 0) link_drop(st);
}

static void link_stop(st_pcm_t *st) {
  if (st->link_running) {
    atomic_store(&st->quit, 1);
    link_wake(st);
    pthread_join(st->link_th, NULL);
    st->link_running = 0;
  }
  const int fd = atomic_exchange(&st->handoff_fd, -1);
  if (fd >= 0) close(fd);
}

/* ---- the clock: release as many frames as it has advanced ------------------------------- */

/* hw_ptr is a function of the monotonic clock and of nothing else — in particular not of anything
 * having polled us. Every callback that reports a position calls this first. */
static void advance(st_pcm_t *st, uint64_t now) {
  snd_pcm_ioplug_t *io = &st->io;

  link_service(st);
  if (st->sk_fd >= 0 && pump_rx(st) < 0) link_drop(st);

  if (!st->started || !st->areas) {
    st->last_tick_ns = now;
    return;
  }

  uint64_t dt = now > st->last_tick_ns ? now - st->last_tick_ns : 0;
  st->last_tick_ns = now;
  if (dt > st->catchup_ns) dt = st->catchup_ns;

  /* Elapsed time in, frames out, at the application's own rate. */
  double due = (double)dt * 1e-9 * (double)st->pcm_rate + st->frac;
  if (due < 0.0) due = 0.0;
  unsigned long want = (unsigned long)due;
  st->frac = due - (double)want;
  if (want == 0) return;
  if (want > st->max_lead) want = st->max_lead; /* belt and braces; dt is already capped */

  snd_pcm_uframes_t avail = snd_pcm_ioplug_hw_avail(io, st->hw_ptr, io->appl_ptr);
  if (avail < want) {
    /* Not in DRAINING: a shrinking buffer there is the stream ending, not a fault. */
    if (io->state == SND_PCM_STATE_RUNNING) st->xrun = 1;
    want = avail;
    if (want == 0) return;
  }

  const snd_pcm_channel_area_t *a = st->areas;
  while (want > 0) {
    unsigned int chunk = want > ST_PACKET_FRAMES ? ST_PACKET_FRAMES : (unsigned int)want;
    snd_pcm_uframes_t off = st->hw_ptr % io->buffer_size;
    if (off + chunk > io->buffer_size) chunk = (unsigned int)(io->buffer_size - off);

    /* areas[0].step is bits per FRAME for an interleaved buffer. */
    const uint8_t *src = (const uint8_t *)a->addr + (a->first + a->step * off) / 8;

    if (st->sk_fd >= 0) {
      const int err = st->encoding == ST_ENC_VORBIS ? encode_and_send(st, src, chunk)
                                                    : send_pcm(st, src, chunk);
      if (err < 0) link_drop(st);
    }

    st->sent_pcm += chunk;
    st->hw_ptr += chunk;
    want -= chunk;
  }
}

/* ---- ioplug callbacks ------------------------------------------------------------------ */

static snd_pcm_sframes_t st_pointer(snd_pcm_ioplug_t *io) {
  st_pcm_t *st = io->private_data;
  advance(st, mono_ns());
  if (st->xrun) return -EPIPE;
  return (snd_pcm_sframes_t)(st->boundary ? st->hw_ptr % st->boundary : st->hw_ptr);
}

static int st_start(snd_pcm_ioplug_t *io) {
  st_pcm_t *st = io->private_data;
  long tick = (long)(1000000000ll * (long long)io->period_size / (long long)st->pcm_rate);
  if (tick < TICK_MIN_NS) tick = TICK_MIN_NS;
  if (tick > TICK_MAX_NS) tick = TICK_MAX_NS;

  struct itimerspec its;
  memset(&its, 0, sizeof(its));
  its.it_interval.tv_nsec = tick;
  its.it_value.tv_nsec = tick;
  if (timerfd_settime(st->timer_fd, 0, &its, NULL) < 0) return -errno;

  /* Pace from now, or the first tick bills us for however long prepare() took. */
  st->last_tick_ns = mono_ns();
  st->frac = 0.0;
  st->started = 1;
  return 0;
}

static int st_stop(snd_pcm_ioplug_t *io) {
  st_pcm_t *st = io->private_data;
  struct itimerspec its;
  memset(&its, 0, sizeof(its));
  timerfd_settime(st->timer_fd, 0, &its, NULL); /* must be safe on a never-started stream */
  st->started = 0;
  return 0;
}

static int st_prepare(snd_pcm_ioplug_t *io) {
  st_pcm_t *st = io->private_data;
  st->areas = snd_pcm_ioplug_mmap_areas(io);
  st->hw_ptr = 0;
  st->xrun = 0;
  st->started = 0;
  st->frac = 0.0;
  st->last_tick_ns = mono_ns();
  st->declared = 0; /* a new stream, even over a socket that was already open */

  switch (io->format) {
    case SND_PCM_FORMAT_S16_LE: st->wire_fmt = ST_FMT_S16_LE; break;
    case SND_PCM_FORMAT_S24_3LE: st->wire_fmt = ST_FMT_S24_3LE; break;
    case SND_PCM_FORMAT_FLOAT_LE: st->wire_fmt = ST_FMT_FLOAT_LE; break;
    default: st->wire_fmt = ST_FMT_S32_LE; break;
  }
  st->sample_bytes = st_fmt_bytes(st->wire_fmt);
  st->pcm_rate = io->rate ? io->rate : ST_DEFAULT_RATE;
  st->catchup_ns = 2ull * io->period_size * 1000000000ull / st->pcm_rate;
  if (st->catchup_ns < (uint64_t)MIN_CATCHUP_NS) st->catchup_ns = (uint64_t)MIN_CATCHUP_NS;

#ifdef ST_HAVE_VORBIS
  /* Also started here, so a combination libvorbis will not encode is refused while there is still
   * an open() to fail. */
  if (st->encoding == ST_ENC_VORBIS && vorbis_restart(st) < 0) return -EINVAL;
#endif

  link_service(st);
  return 0;
}

static int st_sw_params(snd_pcm_ioplug_t *io, snd_pcm_sw_params_t *params) {
  st_pcm_t *st = io->private_data;
  snd_pcm_sw_params_get_avail_min(params, &st->min_avail);
  snd_pcm_sw_params_get_boundary(params, &st->boundary);
  return 0;
}

static int st_poll_descriptors_count(snd_pcm_ioplug_t *io ATTRIBUTE_UNUSED) { return FD_COUNT; }

/* The timer only. The socket comes and goes with the link, and an fd set an application has
 * already collected cannot change under it; what arrives on it is drained on the next tick. */
static int st_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int space) {
  st_pcm_t *st = io->private_data;
  if (space < FD_COUNT) return -EINVAL;
  pfd[FD_TIMER].fd = st->timer_fd;
  pfd[FD_TIMER].events = POLLIN;
  return FD_COUNT;
}

static int st_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int nfds,
                           unsigned short *revents) {
  st_pcm_t *st = io->private_data;
  if (nfds < FD_COUNT) return -EINVAL;
  *revents = 0;

  if (pfd[FD_TIMER].revents & POLLIN) {
    uint64_t ticks;
    while (read(st->timer_fd, &ticks, sizeof(ticks)) > 0) {
    }
  }
  advance(st, mono_ns());

  /* A tick says nothing about the PCM having room: POLLOUT only once avail_min is free. */
  if (snd_pcm_ioplug_avail(io, st->hw_ptr, io->appl_ptr) >= st->min_avail) *revents |= POLLOUT;
  return 0;
}

static int st_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp) {
  st_pcm_t *st = io->private_data;
  advance(st, mono_ns());
  /* Written but not yet sent, plus what the device says is ahead of playout — the latter on the
   * card's axis, so scale it into the application's frames. */
  snd_pcm_sframes_t queued = (snd_pcm_sframes_t)snd_pcm_ioplug_hw_avail(io, st->hw_ptr, io->appl_ptr);
  double flight = st->lead_frames > 0 ? (double)st->lead_frames : 0.0;
  if (st->rate) flight = flight * st->pcm_rate / st->rate;
  *delayp = queued + (snd_pcm_sframes_t)flight;
  return 0;
}

static int st_close(snd_pcm_ioplug_t *io) {
  st_pcm_t *st = io->private_data;
  link_stop(st);
#ifdef ST_HAVE_VORBIS
  if (st->venc_loaded) st_vorbis_stop(&st->venc);
#endif
  if (st->sk_fd >= 0) {
    st_send_msg(st->sk_fd, ST_MSG_BYE, NULL, 0, SEND_TIMEOUT_MS);
    close(st->sk_fd);
  }
  if (st->timer_fd >= 0) close(st->timer_fd);
  if (st->wake_fd >= 0) close(st->wake_fd);
  free(st->host);
  free(st);
  return 0;
}

static const snd_pcm_ioplug_callback_t st_callback = {
    .start = st_start,
    .stop = st_stop,
    .pointer = st_pointer,
    .close = st_close,
    .sw_params = st_sw_params,
    .prepare = st_prepare,
    .poll_descriptors_count = st_poll_descriptors_count,
    .poll_descriptors = st_poll_descriptors,
    .poll_revents = st_poll_revents,
    .delay = st_delay,
};

/* ---- open ------------------------------------------------------------------------------ */

static int set_hw_constraints(st_pcm_t *st) {
  snd_pcm_ioplug_t *io = &st->io;
  static const unsigned int accesses[] = {SND_PCM_ACCESS_RW_INTERLEAVED,
                                          SND_PCM_ACCESS_MMAP_INTERLEAVED};
  /* Every format the device can unpack, so nothing has to convert before us. */
  static const unsigned int formats[] = {SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_S24_3LE,
                                         SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_FLOAT_LE};
  /* The device resamples, so any of the usual rates is fine. */
  static const unsigned int rates[] = {8000,  11025, 16000, 22050, 32000,  44100,
                                       48000, 64000, 88200, 96000, 176400, 192000};
  int err;
  if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS,
                                           sizeof(accesses) / sizeof(accesses[0]), accesses)) < 0)
    return err;
  if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT,
                                           sizeof(formats) / sizeof(formats[0]), formats)) < 0)
    return err;
  if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_RATE,
                                           sizeof(rates) / sizeof(rates[0]), rates)) < 0)
    return err;
  if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_CHANNELS,
                                             (unsigned)st->channels, (unsigned)st->channels)) < 0)
    return err;
  if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 256, 65536)) < 0)
    return err;
  if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS, 2, 64)) < 0)
    return err;
  return 0;
}

SND_PCM_PLUGIN_DEFINE_FUNC(soundtester) {
  static const char *const T = "soundtester";
  snd_config_iterator_t i, next;
  const char *host = "localhost";
  long port = ST_DEFAULT_PORT;
  long channel = -1;
  long channels = 1;
  long encoding = ST_ENC_PCM;
  double quality = 0.4; /* Vorbis VBR: roughly 128 kbps at 44.1 kHz stereo */
  int mixer = 1;
  st_pcm_t *st;
  int err;

  const char *enc = NULL;
  snd_config_for_each(i, next, conf) {
    snd_config_t *n = snd_config_iterator_entry(i);
    const char *id;
    if (snd_config_get_id(n, &id) < 0) continue;
    if (st_cfg_boilerplate(id)) continue;

    err = 0;
    if (st_cfg_str(id, "host", n, T, &host, &err) || st_cfg_int(id, "port", n, T, &port, &err) ||
        st_cfg_int(id, "channel", n, T, &channel, &err) ||
        st_cfg_int(id, "channels", n, T, &channels, &err) ||
        st_cfg_real(id, "quality", n, T, &quality, &err) ||
        st_cfg_bool(id, "mixer", n, T, &mixer, &err) ||
        st_cfg_str(id, "encoding", n, T, &enc, &err)) {
      if (err < 0) return err;
    } else {
      SNDERR("%s: unknown field %s", T, id);
      return -EINVAL;
    }
  }

  if (channels < 1 || channels > (long)ST_MAX_STREAM_CHANNELS) {
    SNDERR("%s: channels must be 1..%u", T, ST_MAX_STREAM_CHANNELS);
    return -EINVAL;
  }
  if (enc && strcmp(enc, "pcm")) {
    if (strcmp(enc, "vorbis")) {
      SNDERR("%s: encoding must be pcm or vorbis, not %s", T, enc);
      return -EINVAL;
    }
#ifdef ST_HAVE_VORBIS
    encoding = ST_ENC_VORBIS;
#else
    SNDERR("%s: this plugin was built without Vorbis support (rebuild with cmake -DST_VORBIS=ON)", T);
    return -ENOSYS;
#endif
  }

  if (stream != SND_PCM_STREAM_PLAYBACK) {
    SNDERR("soundtester: capture is not supported yet — this is a playback-only device");
    return -EINVAL;
  }

  st = calloc(1, sizeof(*st));
  if (!st) return -ENOMEM;
  st->sk_fd = -1;
  st->timer_fd = -1;
  st->wake_fd = -1;
  st->want_channel = channel;
  st->channels = channels;
  st->encoding = encoding;
  st->quality = quality;
  st->mixer = mixer;
  st->port = port;
  st->max_lead = ASSUMED_MAX_LEAD;
  atomic_init(&st->handoff_fd, -1);
  atomic_init(&st->want_link, 1);
  atomic_init(&st->quit, 0);
  st->host = strdup(host);
  if (!st->host) {
    free(st);
    return -ENOMEM;
  }

  st->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  st->wake_fd = eventfd(0, EFD_NONBLOCK);
  if (st->timer_fd < 0 || st->wake_fd < 0) {
    err = -errno;
    goto err_out;
  }

  st->io.version = SND_PCM_IOPLUG_VERSION;
  st->io.name = "rpi-sound-tester network input";
  st->io.callback = &st_callback;
  st->io.private_data = st;
  /* Without BOUNDARY_WA a hw_ptr advance of exactly buffer_size is indistinguishable from none. */
  st->io.flags = SND_PCM_IOPLUG_FLAG_BOUNDARY_WA;
  st->io.mmap_rw = 1;

  if ((err = snd_pcm_ioplug_create(&st->io, name, stream, mode)) < 0) goto err_out;
  if ((err = set_hw_constraints(st)) < 0) {
    snd_pcm_ioplug_delete(&st->io);
    return err; /* delete() called close(), which freed st */
  }

  /* Last, so every path that can still fail unwinds without a thread to stop. */
  if (pthread_create(&st->link_th, NULL, link_thread, st) == 0)
    st->link_running = 1;
  else
    SNDERR("%s: no connector thread — this stream will play to nowhere", T);

  *pcmp = st->io.pcm;
  return 0;

err_out:
  /* Before ioplug_create succeeds nothing will ever call close() for us. */
  if (st->timer_fd >= 0) close(st->timer_fd);
  if (st->wake_fd >= 0) close(st->wake_fd);
  free(st->host);
  free(st);
  return err;
}

SND_PCM_PLUGIN_SYMBOL(soundtester);
