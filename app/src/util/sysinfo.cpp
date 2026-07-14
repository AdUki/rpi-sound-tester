#include "util/sysinfo.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace st {

namespace {

float sample_temp() {
  std::ifstream f("/sys/class/thermal/thermal_zone0/temp");
  if (!f) return -1.0f;
  long milli = 0;
  f >> milli;
  return static_cast<float>(milli) / 1000.0f;
}

double sample_uptime() {
  std::ifstream f("/proc/uptime");
  if (!f) return 0.0;
  double up = 0.0;
  f >> up;
  return up;
}

MemInfo sample_mem() {
  MemInfo m;
  std::ifstream f("/proc/meminfo");
  if (!f) return m;
  std::string key;
  uint64_t value = 0;
  std::string unit;
  while (f >> key >> value >> unit) {
    if (key == "MemTotal:") m.total_kb = value;
    // MemAvailable, not MemFree: the kernel's own estimate of what a new allocation can get,
    // which counts the reclaimable page cache. MemFree on a box that has been running a while
    // looks alarmingly small and means nothing.
    else if (key == "MemAvailable:") m.available_kb = value;
    if (m.total_kb && m.available_kb) break;
  }
  if (m.total_kb >= m.available_kb) m.used_kb = m.total_kb - m.available_kb;
  return m;
}

ThrottleInfo sample_throttle() {
  ThrottleInfo t;
  // The Pi's VideoCore firmware publishes one bitmask here. There is no other way to learn that
  // the 5 V rail is sagging — Linux itself has no idea.
  std::ifstream f("/sys/devices/platform/soc/soc:firmware/get_throttled");
  if (!f) return t;
  std::string hex;
  f >> hex;
  if (hex.empty()) return t;

  const unsigned long bits = std::strtoul(hex.c_str(), nullptr, 16);
  t.available = true;
  t.under_voltage = bits & (1u << 0);
  t.freq_capped = bits & (1u << 1);
  t.throttled = bits & (1u << 2);
  t.under_voltage_seen = bits & (1u << 16);
  t.throttled_seen = bits & (1u << 18);
  return t;
}

}  // namespace

CpuSampler::Load CpuSampler::sample() {
  Load out;
  std::ifstream f("/proc/stat");
  if (!f) return out;

  // /proc/stat leads with the aggregate "cpu" line, then one "cpuN" line per core.
  std::vector<Times> now_cores;
  Times now_total{};
  bool have_total = false;

  std::string line;
  while (std::getline(f, line)) {
    if (line.compare(0, 3, "cpu") != 0) break;  // the cpu* lines come first; stop at the rest
    std::istringstream ls(line);
    std::string tag;
    uint64_t user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    ls >> tag >> user >> nice >> sys >> idle >> iowait >> irq >> softirq >> steal;

    Times t;
    t.idle = idle + iowait;
    t.total = user + nice + sys + t.idle + irq + softirq + steal;

    if (tag == "cpu") {
      now_total = t;
      have_total = true;
    } else {
      now_cores.push_back(t);
    }
  }
  if (!have_total) return out;

  auto pct = [](const Times& now, const Times& prev) -> float {
    const uint64_t dt = now.total - prev.total;
    const uint64_t di = now.idle - prev.idle;
    if (dt == 0) return 0.0f;  // two reads inside one jiffy: no window to measure over
    return 100.0f * static_cast<float>(dt - di) / static_cast<float>(dt);
  };

  const bool had_baseline = primed_ && prev_cores_.size() == now_cores.size();
  if (had_baseline) {
    out.total = pct(now_total, prev_total_);
    out.cores.reserve(now_cores.size());
    for (size_t i = 0; i < now_cores.size(); ++i) out.cores.push_back(pct(now_cores[i], prev_cores_[i]));
  } else {
    // First call (or the core count changed): no baseline to difference against. Report zeros
    // of the right shape rather than a plausible-looking number computed from the
    // uptime-since-boot totals.
    out.cores.assign(now_cores.size(), 0.0f);
  }

  prev_total_ = now_total;
  prev_cores_ = std::move(now_cores);
  primed_ = true;
  return out;
}

SysInfo sample_sysinfo() {
  SysInfo s;
  s.temp_c = sample_temp();
  s.uptime_s = sample_uptime();
  s.mem = sample_mem();
  s.throttle = sample_throttle();

  char host[256] = {0};
  if (gethostname(host, sizeof(host) - 1) == 0) s.hostname = host;

  ifaddrs* ifa = nullptr;
  if (getifaddrs(&ifa) == 0) {
    for (ifaddrs* i = ifa; i; i = i->ifa_next) {
      if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
      if (i->ifa_flags & IFF_LOOPBACK) continue;
      char ip[INET_ADDRSTRLEN] = {0};
      auto* addr = reinterpret_cast<sockaddr_in*>(i->ifa_addr);
      if (inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip))) {
        s.ips.push_back(std::string(i->ifa_name) + " " + ip);
      }
    }
    freeifaddrs(ifa);
  }
  return s;
}

}  // namespace st
