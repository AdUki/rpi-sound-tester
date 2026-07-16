#include "kmsg_watch.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <vector>

#include "util/log.h"

namespace st {

namespace {

bool is_sync_error(const std::string& line) {
  std::string low = line;
  std::transform(low.begin(), low.end(), low.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return low.find("i2s") != std::string::npos && low.find("sync error") != std::string::npos;
}

}  // namespace

KmsgWatch::~KmsgWatch() { stop(); }

void KmsgWatch::start() {
  running_.store(true);
  thread_ = std::thread([this] { run(); });
}

void KmsgWatch::stop() {
  running_.store(false);
  if (thread_.joinable()) thread_.join();
}

void KmsgWatch::process_line(const std::string& line) {
  if (!is_sync_error(line)) return;
  sync_errors_.fetch_add(1);
  LOG_WARN("I2S SYNC error seen — channel mapping may have rotated: {}", line);
}

void KmsgWatch::run() {
  const int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    LOG_WARN("cannot open /dev/kmsg ({}) — I2S sync errors will not be detected",
             strerror(errno));
    return;
  }
  // Only new messages are interesting; skip whatever is already in the buffer.
  lseek(fd, 0, SEEK_END);

  std::vector<char> buf(8192);
  while (running_.load()) {
    pollfd p{fd, POLLIN, 0};
    const int rc = poll(&p, 1, 250);
    if (rc <= 0) continue;

    for (;;) {
      const ssize_t n = read(fd, buf.data(), buf.size());
      if (n <= 0) break;

      // Each read returns one record: "prio,seq,time,flag;message".
      std::string rec(buf.data(), static_cast<size_t>(n));
      const size_t semi = rec.find(';');
      std::string msg = semi == std::string::npos ? rec : rec.substr(semi + 1);
      while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
      if (!msg.empty()) process_line(msg);
    }
  }
  close(fd);
}

}  // namespace st
