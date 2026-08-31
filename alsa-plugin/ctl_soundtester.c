/*
 * ALSA external control plugin: the mixer for an rpi-sound-tester network input.
 *
 *   alsamixer -D soundtester
 *   amixer -D soundtester set Master 60%
 *
 * One volume and one mute for the WHOLE of what this machine is streaming, not one per channel.
 * The values live on the device, so alsamixer and the web console read and write the same thing;
 * it pushes ST_MSG_MIX whenever anything moves.
 *
 * With no `channel` the device picks by address and keeps re-resolving, so the volume widens with
 * a stereo stream that starts later. A stream from a PCM with `mixer off` is not volume-controlled
 * at all, and both elements go INACTIVE.
 */

#include <alsa/asoundlib.h>
#include <alsa/control_external.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net_proto.h"
#include "plugin_common.h"

#define ELEM_VOLUME 0
#define ELEM_SWITCH 1
#define ELEM_COUNT 2

/* 0..100 is what every ALSA mixer drives; the ends map onto the device's gain limits, linear in
 * dB between. */
#define VOL_MAX 100

typedef struct {
  snd_ctl_ext_t ext;
  int fd;
  unsigned int channel;
  int32_t gain_min_cdb;
  int32_t gain_max_cdb;
  int32_t gain_cdb;
  int mute;
  int bypass;                /* `mixer off` on the sender: the elements go inactive */
  unsigned int pending;      /* element bits whose VALUE the device changed under us */
  unsigned int pending_info; /* element bits whose ATTRIBUTES changed (active <-> inactive) */
} st_ctl_t;

/* ---- wire helpers (the same framing the PCM plugin uses) -------------------------------- */

static long cdb_to_vol(const st_ctl_t *c, int32_t cdb) {
  const int32_t span = c->gain_max_cdb - c->gain_min_cdb;
  if (span <= 0) return VOL_MAX;
  long v = ((long)(cdb - c->gain_min_cdb) * VOL_MAX + span / 2) / span;
  return v < 0 ? 0 : (v > VOL_MAX ? VOL_MAX : v);
}

static int32_t vol_to_cdb(const st_ctl_t *c, long vol) {
  if (vol < 0) vol = 0;
  if (vol > VOL_MAX) vol = VOL_MAX;
  return c->gain_min_cdb + (int32_t)((vol * (c->gain_max_cdb - c->gain_min_cdb)) / VOL_MAX);
}

/* Runs on the mixer's poll path, so it must not block. */
static void pump(st_ctl_t *c) {
  for (;;) {
    struct pollfd pfd = {c->fd, POLLIN, 0};
    if (poll(&pfd, 1, 0) <= 0) return;
    uint8_t pl[64];
    uint32_t len = 0;
    if (st_read_msg(c->fd, pl, sizeof(pl), &len, 100) != ST_MSG_MIX) continue;
    if (len < ST_MIX_BYTES) continue;
    int32_t g = (int32_t)st_get_u32(pl + ST_MIX_O_GAIN);
    int m = st_get_u32(pl + ST_MIX_O_MUTE) != 0;
    int b = (st_get_u32(pl + ST_MIX_O_FLAGS) & ST_MIX_ST_BYPASS) != 0;
    if (g != c->gain_cdb) c->pending |= 1u << ELEM_VOLUME;
    if (m != c->mute) c->pending |= 1u << ELEM_SWITCH;
    /* Both elements: they are one control to the user. */
    if (b != c->bypass) c->pending_info |= (1u << ELEM_VOLUME) | (1u << ELEM_SWITCH);
    c->gain_cdb = g;
    c->mute = m;
    c->bypass = b;
  }
}

static int push(st_ctl_t *c, unsigned int mask) {
  uint8_t p[ST_SET_MIX_BYTES];
  memset(p, 0, sizeof(p));
  st_put_u32(p + ST_SETMIX_O_MASK, mask);
  st_put_u32(p + ST_SETMIX_O_GAIN, (uint32_t)c->gain_cdb);
  st_put_u32(p + ST_SETMIX_O_MUTE, c->mute ? 1u : 0u);
  return st_send_msg(c->fd, ST_MSG_SET_MIX, p, sizeof(p), -1) < 0 ? -EIO : 0;
}

/* ---- ctl_ext callbacks ------------------------------------------------------------------ */

static void st_close(snd_ctl_ext_t *ext) {
  st_ctl_t *c = ext->private_data;
  if (c->fd >= 0) {
    st_send_msg(c->fd, ST_MSG_BYE, NULL, 0, 1000);
    close(c->fd);
  }
  free(c);
}

static int st_elem_count(snd_ctl_ext_t *ext ATTRIBUTE_UNUSED) { return ELEM_COUNT; }

static int st_elem_list(snd_ctl_ext_t *ext ATTRIBUTE_UNUSED, unsigned int offset,
                        snd_ctl_elem_id_t *id) {
  snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
  /* The names alsamixer and every desktop mixer look for; anything else shows up unlabelled. */
  snd_ctl_elem_id_set_name(id, offset == ELEM_VOLUME ? "Master Playback Volume"
                                                     : "Master Playback Switch");
  return 0;
}

static snd_ctl_ext_key_t st_find_elem(snd_ctl_ext_t *ext ATTRIBUTE_UNUSED,
                                      const snd_ctl_elem_id_t *id) {
  const char *name = snd_ctl_elem_id_get_name(id);
  if (!strcmp(name, "Master Playback Volume")) return ELEM_VOLUME;
  if (!strcmp(name, "Master Playback Switch")) return ELEM_SWITCH;
  return SND_CTL_EXT_KEY_NOT_FOUND;
}

static int st_get_attribute(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, int *type,
                            unsigned int *acc, unsigned int *count) {
  st_ctl_t *c = ext->private_data;
  *type = key == ELEM_VOLUME ? SND_CTL_ELEM_TYPE_INTEGER : SND_CTL_ELEM_TYPE_BOOLEAN;
  *acc = c->bypass ? (SND_CTL_EXT_ACCESS_READ | SND_CTL_EXT_ACCESS_INACTIVE)
                   : SND_CTL_EXT_ACCESS_READWRITE;
  *count = 1;  /* one value, however many channels the device drives with it */
  return 0;
}

static int st_get_integer_info(snd_ctl_ext_t *ext ATTRIBUTE_UNUSED,
                               snd_ctl_ext_key_t key ATTRIBUTE_UNUSED, long *imin, long *imax,
                               long *istep) {
  *imin = 0;
  *imax = VOL_MAX;
  *istep = 0;
  return 0;
}

static int st_read_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value) {
  st_ctl_t *c = ext->private_data;
  pump(c);
  *value = key == ELEM_VOLUME ? cdb_to_vol(c, c->gain_cdb) : (c->mute ? 0 : 1);
  return 0;
}

static int st_write_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value) {
  st_ctl_t *c = ext->private_data;
  if (c->bypass) return -EPERM;
  if (key == ELEM_VOLUME) {
    int32_t cdb = vol_to_cdb(c, *value);
    if (cdb == c->gain_cdb) return 0;
    c->gain_cdb = cdb;
    if (push(c, ST_MIX_F_GAIN) < 0) return -EIO;
  } else {
    int m = *value ? 0 : 1;
    if (m == c->mute) return 0;
    c->mute = m;
    if (push(c, ST_MIX_F_MUTE) < 0) return -EIO;
  }
  return 1; /* changed */
}

/* The socket is the event source: a value moved in the web console arrives here as a MIX. */
static int st_read_event(snd_ctl_ext_t *ext, snd_ctl_elem_id_t *id, unsigned int *event_mask) {
  st_ctl_t *c = ext->private_data;
  pump(c);
  const unsigned int any = c->pending | c->pending_info;
  if (!any) return -EAGAIN;
  const unsigned int which = (any & (1u << ELEM_VOLUME)) ? ELEM_VOLUME : ELEM_SWITCH;
  *event_mask = 0;
  if (c->pending & (1u << which)) *event_mask |= SND_CTL_EVENT_MASK_VALUE;
  /* INFO, not VALUE: a mixer only re-reads an element's attributes when told they moved. */
  if (c->pending_info & (1u << which)) *event_mask |= SND_CTL_EVENT_MASK_INFO;
  c->pending &= ~(1u << which);
  c->pending_info &= ~(1u << which);
  st_elem_list(ext, which, id);
  snd_ctl_elem_id_set_numid(id, 0);
  return 0;
}

static int st_poll_descriptors_count(snd_ctl_ext_t *ext ATTRIBUTE_UNUSED) { return 1; }

static int st_poll_descriptors(snd_ctl_ext_t *ext, struct pollfd *pfd, unsigned int space) {
  st_ctl_t *c = ext->private_data;
  if (space < 1) return -EINVAL;
  pfd[0].fd = c->fd;
  pfd[0].events = POLLIN;
  return 1;
}

static int st_poll_revents(snd_ctl_ext_t *ext, struct pollfd *pfd, unsigned int nfds,
                           unsigned short *revents) {
  st_ctl_t *c = ext->private_data;
  if (nfds < 1) return -EINVAL;
  *revents = 0;
  if (pfd[0].revents & POLLIN) {
    pump(c);
    if (c->pending || c->pending_info) *revents = POLLIN;
  }
  return 0;
}

static const snd_ctl_ext_callback_t st_callback = {
    .close = st_close,
    .elem_count = st_elem_count,
    .elem_list = st_elem_list,
    .find_elem = st_find_elem,
    .get_attribute = st_get_attribute,
    .get_integer_info = st_get_integer_info,
    .read_integer = st_read_integer,
    .write_integer = st_write_integer,
    .read_event = st_read_event,
    .poll_descriptors_count = st_poll_descriptors_count,
    .poll_descriptors = st_poll_descriptors,
    .poll_revents = st_poll_revents,
};

/* ---- open ------------------------------------------------------------------------------- */

static int connect_ctl(st_ctl_t *c, const char *host, long port, long channel) {
  int fd = st_connect(host, port, "soundtester ctl", 3000, -1);
  if (fd < 0) return fd;
  c->fd = fd;

  uint8_t hello[ST_CTL_HELLO_BYTES];
  memset(hello, 0, sizeof(hello));
  st_put_u32(hello + ST_CTLH_O_MAGIC, ST_NET_MAGIC);
  st_put_u32(hello + ST_CTLH_O_PROTO, ST_NET_PROTO_VERSION);
  st_put_u16(hello + ST_CTLH_O_CHANNEL, channel < 0 ? ST_CTL_ANY_CHANNEL : (uint16_t)channel);
  if (st_send_msg(fd, ST_MSG_CTL_HELLO, hello, sizeof(hello), 3000) < 0) return -EIO;

  uint8_t ack[ST_CTL_ACK_BYTES];
  uint32_t len = 0;
  int type = st_read_msg(fd, ack, sizeof(ack), &len, 3000);
  if (type < 0) {
    SNDERR("soundtester ctl: no reply from %s:%ld", host, port);
    return -EIO;
  }
  if (type != ST_MSG_CTL_ACK || len != ST_CTL_ACK_BYTES) return -EIO;
  if (st_get_u32(ack + ST_CTLA_O_STATUS) != ST_HELLO_OK) {
    SNDERR("soundtester ctl: %s refused the mixer connection", host);
    return -EIO;
  }

  c->channel = st_get_u32(ack + ST_CTLA_O_CHANNEL);
  c->gain_min_cdb = (int32_t)st_get_u32(ack + ST_CTLA_O_GAIN_MIN);
  c->gain_max_cdb = (int32_t)st_get_u32(ack + ST_CTLA_O_GAIN_MAX);
  c->gain_cdb = (int32_t)st_get_u32(ack + ST_CTLA_O_GAIN);
  c->mute = st_get_u32(ack + ST_CTLA_O_MUTE) != 0;
  c->bypass = (st_get_u32(ack + ST_CTLA_O_FLAGS) & ST_MIX_ST_BYPASS) != 0;
  return 0;
}

SND_CTL_PLUGIN_DEFINE_FUNC(soundtester) {
  static const char *const T = "soundtester ctl";
  snd_config_iterator_t i, next;
  const char *host = "localhost";
  long port = ST_DEFAULT_PORT;
  long channel = -1;
  st_ctl_t *c;
  int err;

  snd_config_for_each(i, next, conf) {
    snd_config_t *n = snd_config_iterator_entry(i);
    const char *id;
    if (snd_config_get_id(n, &id) < 0) continue;
    if (st_cfg_boilerplate(id)) continue;

    err = 0;
    if (st_cfg_str(id, "host", n, T, &host, &err) || st_cfg_int(id, "port", n, T, &port, &err) ||
        st_cfg_int(id, "channel", n, T, &channel, &err)) {
      if (err < 0) return err;
    } else {
      SNDERR("%s: unknown field %s", T, id);
      return -EINVAL;
    }
  }

  c = calloc(1, sizeof(*c));
  if (!c) return -ENOMEM;
  c->fd = -1;
  if ((err = connect_ctl(c, host, port, channel)) < 0) {
    if (c->fd >= 0) close(c->fd);
    free(c);
    return err;
  }

  c->ext.version = SND_CTL_EXT_VERSION;
  c->ext.card_idx = 0;
  strncpy(c->ext.id, "soundtester", sizeof(c->ext.id) - 1);
  strncpy(c->ext.driver, "soundtester", sizeof(c->ext.driver) - 1);
  /* Named after the device: the channel it drives is not fixed for its lifetime. */
  snprintf(c->ext.name, sizeof(c->ext.name), "%s", host);
  snprintf(c->ext.longname, sizeof(c->ext.longname), "rpi-sound-tester network input at %s", host);
  snprintf(c->ext.mixername, sizeof(c->ext.mixername), "soundtester");
  c->ext.poll_fd = -1; /* poll_descriptors() supplies it, so events work */
  c->ext.callback = &st_callback;
  c->ext.private_data = c;

  if ((err = snd_ctl_ext_create(&c->ext, name, mode)) < 0) {
    close(c->fd);
    free(c);
    return err;
  }
  *handlep = c->ext.handle;
  return 0;
}

SND_CTL_PLUGIN_SYMBOL(soundtester);
