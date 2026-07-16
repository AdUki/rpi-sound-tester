#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace st {

// The Octo's TDM slot alignment can rotate after an "I2S SYNC error", silently moving a
// channel's audio to a different slot. The kernel says so on /dev/kmsg; the UI turns that
// into a "verify your channel mapping" banner.
class KmsgWatch {
 public:
  ~KmsgWatch();

  void start();
  void stop();

  uint32_t sync_errors() const { return sync_errors_.load(); }

  // Classifies one kernel line; run() feeds it every /dev/kmsg record. Public so that
  // POST /api/system/inject-kmsg (and the acceptance check) can feed synthetic lines.
  void process_line(const std::string& line);

 private:
  void run();

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<uint32_t> sync_errors_{0};
};

}  // namespace st
