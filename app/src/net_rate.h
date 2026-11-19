#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace st {

// How fast a monotonic counter (a network channel's frames_received) is moving, measured between
// now and a mark of it taken at least kMinWindowNs earlier. Not a thread of its own: whoever reads
// the rate also marks (GET /api/net, the net server's accept loop every 250 ms), so a window is
// 1–1.25 s while anything is watching and only longer when nothing has asked for a while.
//
// Nothing on the audio thread: the counter is read where it already is, by the status reader.
class RateMeter {
 public:
  static constexpr uint64_t kSpacingNs = 250000000ull;    // marks at least this far apart
  static constexpr uint64_t kMinWindowNs = 1000000000ull;  // a rate spans at least this

  // Records (ns, count) when the last mark is kSpacingNs old, and drops the marks no rate needs
  // any more. A counter that went backwards (a forgotten channel) starts over; a mark older than
  // the newest (two markers racing) is ignored, never taken for a reset.
  void mark(uint64_t ns, uint64_t count) {
    if (!marks_.empty() && ns < marks_.back().ns) return;
    if (!marks_.empty() && count < marks_.back().count) marks_.clear();
    if (marks_.empty() || ns - marks_.back().ns >= kSpacingNs) marks_.push_back({ns, count});
    // The newest mark already kMinWindowNs old is the base of every rate from here on: anything
    // older than it is never read again.
    while (marks_.size() >= 2 && ns - marks_[1].ns >= kMinWindowNs) marks_.pop_front();
  }

  // Per second, over [the newest mark at least kMinWindowNs old, ns]. Empty until there is one,
  // or when the counter has gone backwards since.
  std::optional<double> per_second(uint64_t ns, uint64_t count) const {
    for (size_t i = marks_.size(); i-- > 0;) {
      const Mark& m = marks_[i];
      if (ns < m.ns || ns - m.ns < kMinWindowNs) continue;
      if (count < m.count) return std::nullopt;
      return static_cast<double>(count - m.count) * 1e9 / static_cast<double>(ns - m.ns);
    }
    return std::nullopt;
  }

  size_t marks() const { return marks_.size(); }

 private:
  struct Mark {
    uint64_t ns;
    uint64_t count;
  };
  std::deque<Mark> marks_;
};

}  // namespace st
