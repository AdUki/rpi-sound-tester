#ifndef ST_PLUGIN_COMMON_H
#define ST_PLUGIN_COMMON_H

/* What both halves of the plugin share: message framing, the socket, and .conf field readers. */

#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net_proto.h"

/* ---- whole protocol messages ------------------------------------------------------------- */

/* A short write is normal on a socket, not an error. These sockets are non-blocking, so a full
 * send buffer is a wait for POLLOUT: `timeout_ms` bounds each wait, negative waits for ever. */
static inline int st_write_all(int fd, const void *buf, size_t n, int timeout_ms) {
  const uint8_t *p = (const uint8_t *)buf;
  size_t sent = 0;
  while (sent < n) {
    ssize_t w = send(fd, p + sent, n - sent, MSG_NOSIGNAL);
    if (w > 0) {
      sent += (size_t)w;
      continue;
    }
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd pfd = {fd, POLLOUT, 0};
      int pr = poll(&pfd, 1, timeout_ms);
      if (pr == 0) return -ETIMEDOUT;
      if (pr < 0 && errno != EINTR) return -EIO;
      continue;
    }
    return -EIO;
  }
  return 0;
}

static inline int st_send_msg(int fd, uint8_t type, const uint8_t *payload, uint32_t len,
                              int timeout_ms) {
  uint8_t hdr[ST_NET_HEADER_BYTES];
  st_put_header(hdr, type, len);
  if (st_write_all(fd, hdr, sizeof(hdr), timeout_ms) < 0) return -EIO;
  if (len && st_write_all(fd, payload, len, timeout_ms) < 0) return -EIO;
  return 0;
}

/* -ETIMEDOUT rather than blocking forever, so a handshake gives up on a silent daemon. */
static inline int st_read_exact(int fd, void *buf, size_t n, int timeout_ms) {
  uint8_t *p = (uint8_t *)buf;
  size_t got = 0;
  while (got < n) {
    struct pollfd pfd = {fd, POLLIN, 0};
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) {
      if (errno == EINTR) continue;
      return -EIO;
    }
    if (pr == 0) return -ETIMEDOUT;
    ssize_t r = recv(fd, p + got, n - got, 0);
    if (r == 0) return -EIO; /* clean close */
    if (r < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      return -EIO;
    }
    got += (size_t)r;
  }
  return 0;
}

/* Reads one message into `buf`, discarding a payload longer than it. Returns the type. */
static inline int st_read_msg(int fd, void *buf, size_t cap, uint32_t *len_out, int timeout_ms) {
  uint8_t hdr[ST_NET_HEADER_BYTES];
  if (st_read_exact(fd, hdr, sizeof(hdr), timeout_ms) < 0) return -EIO;
  uint32_t len = st_header_len(hdr);
  if (len > ST_NET_MAX_PAYLOAD) return -EIO;
  uint32_t take = len > cap ? (uint32_t)cap : len;
  if (take && st_read_exact(fd, buf, take, timeout_ms) < 0) return -EIO;
  for (uint32_t left = len - take; left > 0;) {
    uint8_t skip[64];
    uint32_t n = left > sizeof(skip) ? (uint32_t)sizeof(skip) : left;
    if (st_read_exact(fd, skip, n, timeout_ms) < 0) return -EIO;
    left -= n;
  }
  if (len_out) *len_out = take;
  return (int)st_header_type(hdr);
}

/* ---- the socket ---------------------------------------------------------------------------- */

/* `tag` names the caller in the errors the user sees, or is NULL to fail silently — a retry loop
 * must not log the same line every second. `timeout_ms` bounds the attempt rather than leaving it
 * to the kernel's minutes-long one, and a readable `abort_fd` (-1 for none) cuts it short.
 * The socket comes back NON-BLOCKING; everything here polls. */
static inline int st_connect(const char *host, long port, const char *tag, int timeout_ms,
                             int abort_fd) {
  char portstr[16];
  snprintf(portstr, sizeof(portstr), "%ld", port);

  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  int gai = getaddrinfo(host, portstr, &hints, &res);
  if (gai != 0) {
    if (tag) SNDERR("%s: cannot resolve %s: %s", tag, host, gai_strerror(gai));
    return -EINVAL;
  }

  int fd = -1;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    if (errno == EINPROGRESS) {
      struct pollfd pfd[2] = {{fd, POLLOUT, 0}, {abort_fd, POLLIN, 0}};
      const int nfd = abort_fd >= 0 ? 2 : 1;
      int pr = poll(pfd, nfd, timeout_ms);
      if (pr > 0 && (pfd[0].revents & (POLLOUT | POLLERR | POLLHUP))) {
        int soerr = 0;
        socklen_t slen = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) == 0 && soerr == 0) break;
      }
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) {
    if (tag) SNDERR("%s: cannot connect to %s:%ld", tag, host, port);
    return -EIO;
  }

  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  return fd;
}

/* ---- .conf fields -------------------------------------------------------------------------- */

/* Each returns 1 if it consumed the node, 0 if `id` is some other field; a wrong type is reported
 * by name, or the user gets a bare "Invalid argument". */
static inline int st_cfg_str(const char *id, const char *want, snd_config_t *n, const char *tag,
                             const char **out, int *err) {
  if (strcmp(id, want)) return 0;
  if (snd_config_get_string(n, out) < 0) {
    SNDERR("%s: %s must be a string", tag, want);
    *err = -EINVAL;
  }
  return 1;
}

static inline int st_cfg_int(const char *id, const char *want, snd_config_t *n, const char *tag,
                             long *out, int *err) {
  if (strcmp(id, want)) return 0;
  if (snd_config_get_integer(n, out) < 0) {
    SNDERR("%s: %s must be an integer", tag, want);
    *err = -EINVAL;
  }
  return 1;
}

/* on/off, true/false, yes/no, 1/0 — `mixer off` and `MIXER=0` are the same thing. */
static inline int st_cfg_bool(const char *id, const char *want, snd_config_t *n, const char *tag,
                              int *out, int *err) {
  if (strcmp(id, want)) return 0;
  const int v = snd_config_get_bool(n);
  if (v < 0) {
    SNDERR("%s: %s must be on or off", tag, want);
    *err = -EINVAL;
  } else {
    *out = v;
  }
  return 1;
}

static inline int st_cfg_real(const char *id, const char *want, snd_config_t *n, const char *tag,
                              double *out, int *err) {
  if (strcmp(id, want)) return 0;
  if (snd_config_get_ireal(n, out) < 0) {
    SNDERR("%s: %s must be a number", tag, want);
    *err = -EINVAL;
  }
  return 1;
}

/* alsa-lib puts these in every device node. Skipping "hint" is load-bearing: without it every
 * device that advertises itself in `aplay -L` fails to open with "Unknown field hint". */
static inline int st_cfg_boilerplate(const char *id) {
  return !strcmp(id, "comment") || !strcmp(id, "type") || !strcmp(id, "hint");
}

#endif /* ST_PLUGIN_COMMON_H */
