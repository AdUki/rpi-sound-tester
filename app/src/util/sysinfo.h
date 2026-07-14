#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace st {

// What the Pi's firmware says about the power supply and clock capping. A board browning out
// under the HAT's load is a reason to distrust a reading, and nothing but this one firmware
// register reports that.
struct ThrottleInfo {
  bool available = false;          // false on anything that is not a Pi
  bool under_voltage = false;      // right now
  bool throttled = false;          // right now
  bool freq_capped = false;        // right now
  bool under_voltage_seen = false;  // at some point since boot
  bool throttled_seen = false;      // at some point since boot
};

struct MemInfo {
  uint64_t total_kb = 0;
  uint64_t available_kb = 0;  // what a new allocation can actually get (not just MemFree)
  uint64_t used_kb = 0;       // total - available
};

struct SysInfo {
  float cpu_pct = 0.0f;              // whole machine, all cores averaged
  std::vector<float> cpu_cores;      // per-core, in /proc/stat order
  MemInfo mem;
  ThrottleInfo throttle;
  float temp_c = 0.0f;  // negative when the platform has no thermal zone
  double uptime_s = 0.0;
  std::string hostname;
  std::vector<std::string> ips;
};

// CPU load is a *delta* between two readings of /proc/stat, so whoever samples it owns the
// previous reading — and it must therefore have exactly one owner, sampling on a steady cadence.
//
// A process-global baseline does not work: with the web console open there are two callers, the
// 1 Hz WS publisher and the console's own 5 s GET /api/state poll, and each consumes and resets
// the other's window. The gauge then reads 0.0 % (a window microseconds wide) or 100.0 % (a
// window containing no idle tick) instead of the true ~8 %.
//
// So: the 1 Hz publisher owns the sampler and everyone else reads what it published. Not
// thread-safe by design — do not share an instance across threads.
class CpuSampler {
 public:
  // Busy time as a percentage since the previous call: the aggregate in `total`, and one entry
  // per core in `cores` (in /proc/stat order). The first call has no previous reading to
  // difference against and reports 0.
  struct Load {
    float total = 0.0f;
    std::vector<float> cores;
  };
  Load sample();

 private:
  struct Times {
    uint64_t idle = 0;
    uint64_t total = 0;
  };
  Times prev_total_{};
  std::vector<Times> prev_cores_;
  bool primed_ = false;
};

// Everything except CPU load: stateless, so any thread may call it at any time. cpu_pct and
// cpu_cores are left empty — fill them in from a CpuSampler owned by the caller.
SysInfo sample_sysinfo();

}  // namespace st
