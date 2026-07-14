#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
  std::vector<std::string> recent() const;

  // Test hook (also used by the acceptance check): pretend this line came from the kernel.
  void inject(const std::string& line);

 private:
  void run();

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<uint32_t> sync_errors_{0};

  mutable std::mutex m_;
  std::vector<std::string> recent_;
};

}  // namespace st
