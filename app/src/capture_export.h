#pragma once

// GET /api/capture/wav: which span of which buffer an export reads, and whether it fits the body
// cap. Pure (no ring, no HTTP), so the range rules and their error messages are unit-tested.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace st {

struct ExportQuery {
  std::optional<uint64_t> start;  // absolute sample index
  std::optional<uint64_t> len;    // frames
  std::optional<double> seconds;  // the last N s of the live ring
};

// What can be read right now: the frozen snapshot [frozen_begin, frozen_end) when `frozen`, and
// the live ring [live_begin, live_end) always. `live_margin` frames past live_begin are refused:
// the writer laps the oldest samples while the body is allocated and read, so a span starting
// there would be a 410 every time (the reply's error names the reduced range).
struct ExportBuffers {
  bool frozen = false;
  uint64_t frozen_begin = 0, frozen_end = 0;
  uint64_t live_begin = 0, live_end = 0;
  uint64_t live_margin = 0;
};

struct ExportSpan {
  int status = 200;  // 200, or 400 with `error`
  std::string error;
  bool from_snapshot = false;  // true: read the frozen snapshot, else the live ring
  uint64_t start = 0;
  uint64_t len = 0;
};

// start+len address the frozen snapshot while frozen, else the live ring (as capture/window does);
// seconds is always the live ring's last N s, frozen or not. Either start+len or seconds, never
// both, never neither. `frame_bytes` (channels x sample bytes) and `max_data_bytes` bound the span:
// past that a 400 says how many frames (and seconds) the request may have.
inline ExportSpan resolve_export_span(const ExportQuery& q, const ExportBuffers& b, double rate,
                                      uint64_t frame_bytes, uint64_t max_data_bytes) {
  ExportSpan s;
  auto fail = [&s](std::string msg) {
    s.status = 400;
    s.error = std::move(msg);
    return s;
  };
  const bool by_range = q.start || q.len;
  if (by_range && q.seconds) return fail("give start and len, or seconds, not both");
  if (!by_range && !q.seconds)
    return fail("give start and len (absolute sample indices), or seconds (the last N s of the live ring)");
  if (by_range && !(q.start && q.len)) return fail("start and len go together");

  const uint64_t max_frames = frame_bytes ? max_data_bytes / frame_bytes : 0;
  const uint64_t live_lo =
      b.live_begin + std::min<uint64_t>(b.live_margin, b.live_end - b.live_begin);
  char msg[256];
  if (q.seconds) {
    const double sec = *q.seconds;
    const uint64_t have = b.live_end - live_lo;
    if (!(sec > 0) || !std::isfinite(sec)) return fail("seconds must be a number > 0");
    const double have_s = std::floor(static_cast<double>(have) / rate * 1000) / 1000;
    const uint64_t len = static_cast<uint64_t>(std::llround(sec * rate));
    if (len == 0) return fail("seconds is shorter than one sample");
    if (len > have) {
      if (live_lo > b.live_begin)
        std::snprintf(msg, sizeof msg,
                      "seconds must be at most %.3f (what the live ring holds now, less %.1f s kept "
                      "clear of its oldest end)",
                      have_s, static_cast<double>(live_lo - b.live_begin) / rate);
      else
        std::snprintf(msg, sizeof msg, "seconds must be at most %.3f (what the live ring holds now)",
                      have_s);
      return fail(msg);
    }
    s.start = b.live_end - len;
    s.len = len;
  } else {
    s.from_snapshot = b.frozen;
    const uint64_t lo = b.frozen ? b.frozen_begin : live_lo;
    const uint64_t hi = b.frozen ? b.frozen_end : b.live_end;
    const char* what = b.frozen ? "frozen range" : "live ring";
    if (*q.len == 0) return fail("len must be >= 1");
    if (*q.start > UINT64_MAX - *q.len) {
      std::snprintf(msg, sizeof msg, "start + len is past the end of any range (the %s is [%llu, %llu))",
                    what, static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi));
      return fail(msg);
    }
    if (*q.start < lo || *q.start + *q.len > hi) {
      std::snprintf(msg, sizeof msg, "[%llu, %llu) is outside the %s [%llu, %llu)",
                    static_cast<unsigned long long>(*q.start),
                    static_cast<unsigned long long>(*q.start + *q.len), what,
                    static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi));
      return fail(msg);
    }
    s.start = *q.start;
    s.len = *q.len;
  }
  if (s.len > max_frames) {
    std::snprintf(msg, sizeof msg,
                  "%llu frames is %.0f MB: at most %llu MB, i.e. %llu frames (%.3f s) of these "
                  "channels in this format (fewer channels, s16, or a shorter span)",
                  static_cast<unsigned long long>(s.len),
                  static_cast<double>(s.len) * frame_bytes / (1024.0 * 1024.0),
                  static_cast<unsigned long long>(max_data_bytes / (1024 * 1024)),
                  static_cast<unsigned long long>(max_frames), static_cast<double>(max_frames) / rate);
    return fail(msg);
  }
  return s;
}

}  // namespace st
