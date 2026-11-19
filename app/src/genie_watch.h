#pragma once

// GET /api/genie/watch: what happened on an input over a span of the ring. Dropouts (exact-zero
// holes, quiet gaps), clicks, level steps and, on a test tone, phase slips and distortion, each
// at the sample it happened, plus the span's level statistics. Replaces the gapscan / sine-fit /
// glitch scripts that each pulled the span out 4096 samples at a time.
//
// Pure: no HTTP, no ring, no daemon state. The handler feeds one channel's span in chunks (any
// size) and hands finish() a way to read a few blocks of it again; tests feed synthetic signals.
// O(1) work per sample plus a per-block median, so 40 s x 2 channels x 96 kHz is well under a
// second on a Pi 3.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace st {

struct WatchParams {
  double rate = 96000.0;
  unsigned block = 960;          // samples per block (block_ms at rate); >= 16
  double silence_db = -60.0;     // a block whose RMS is below this is quiet
  double click_k = 10.0;         // a click's |second difference| over this x the local median
  std::optional<double> tone_hz; // tone mode: the expected frequency (>= one cycle per block)
  double resid_dbc = -50.0;      // a tone block whose fit residual is above this is bad
  double slip_deg = 20.0;        // a phase jump over this is a slip
  double level_step_db = 6.0;    // a change between two steady 100 ms stretches over this
};

enum class WatchKind : unsigned {
  ZeroRun,    // >= 1 ms of exact 0.0 inside audio (a network hole, an unwritten frame)
  Gap,        // quiet blocks with level right before and right after them (a dropout)
  Onset,      // quiet -> level, abruptly, with no level before (a start, or a gap's far side)
  Offset,     // level -> quiet, abruptly, with no level after
  Click,      // a second-difference outlier against the local median
  LevelStep,  // a jump between two steady 100 ms stretches
  PhaseSlip,  // tone mode: the tone's phase jumped (samples lost or repeated)
  Resid,      // tone mode: blocks that do not fit the tone, with no other event to explain them
  Count
};
inline constexpr size_t kWatchKinds = static_cast<size_t>(WatchKind::Count);

// "zero_run", "gap", "onset", "offset", "click", "level_step", "phase_slip", "resid".
const char* watch_kind_name(WatchKind k);

struct WatchEvent {
  WatchKind kind = WatchKind::Click;
  uint64_t offset = 0;  // first sample, from the span's start
  uint64_t len = 0;     // samples; 0 for a point (a phase slip)
  // phase_slip, and a gap / zero_run across which the tone came back shifted: > 0 the tone jumped
  // ahead (as if that many samples were lost), < 0 it fell behind (samples repeated or inserted).
  // Modulo one period of the tone.
  std::optional<double> slip_samples;
  std::optional<double> slip_deg;
  std::optional<double> from_db, to_db;  // level_step: the 100 ms RMS on either side
  std::optional<double> level_db;        // click: its |second difference| in dBFS
  std::optional<double> resid_dbc;       // resid: the worst block's
  bool zeros = false;  // gap: made of exact zeros (a network hole, a muted path), sample-exact
  bool click = false;  // its edge also read as a click (folded in here, not reported apart)
};

struct WatchToneStats {
  double expect_hz = 0;
  size_t blocks = 0;      // blocks the tone was fitted on (not quiet)
  size_t bad_blocks = 0;  // of those, residual above resid_dbc
  // From the phase advance between consecutive clean blocks; empty with fewer than two.
  std::optional<double> freq_hz_median, freq_hz_min, freq_hz_max;
  std::optional<double> thd_n_pct_median, thd_n_pct_max;  // the fit residual against the tone
  std::optional<double> resid_dbc_median, resid_dbc_max;
};

struct WatchChannelResult {
  uint64_t samples = 0;
  size_t blocks = 0;
  double sound_pct = 0;  // blocks at or above silence_db
  double silent_s = 0;   // blocks below it
  double rms_db_min = -120, rms_db_p10 = -120, rms_db_median = -120, rms_db_max = -120;
  double peak_max_db = -120;
  uint64_t clipped_samples = 0;  // |x| >= 0.999
  std::optional<WatchToneStats> tone;
  std::array<uint64_t, kWatchKinds> counts{};
  std::vector<WatchEvent> events;  // by offset; at most kWatchMaxChannelEvents
  bool events_truncated = false;
};

inline constexpr size_t kWatchMaxChannelEvents = 1000;

// Reads n samples of the span from `offset` (from its start) into out; false when they are gone.
using WatchReread = std::function<bool(uint64_t offset, size_t n, float* out)>;

class WatchAnalyzer {
 public:
  explicit WatchAnalyzer(const WatchParams& p);

  // The span's next samples, in order, in chunks of any size.
  void feed(const float* x, size_t n);

  // Everything fed so far. `reread` (may be empty, or fail) locates a phase slip to the sample;
  // without it a slip is placed to the block.
  WatchChannelResult finish(const WatchReread& reread = {});

 private:
  struct Block {
    float rms_db = -120.0f;
    float peak = 0.0f;
    // Tone mode: x ~ amp * sin(w n + phase) + dc at the block's centre (n from the span's start;
    // the fit also takes a linear drift of both quadratures, so a tone a few Hz off expect_hz, or
    // a slow fade, still fits), and the residual's power against the tone's (amp^2 / 2).
    // solved = the fit was well-posed.
    bool solved = false;
    double amp = 0, phase = 0, dc = 0, resid_ratio = 1;
  };
  struct ClickCand {
    uint64_t first = 0, last = 0, at = 0;
    float peak = 0;
  };

  void end_block();
  void end_d2_block();
  void end_zero_run(uint64_t end);
  void start_tone_block();
  double centre(size_t b) const { return static_cast<double>(b) * p_.block + 0.5 * (p_.block - 1); }

  WatchParams p_;
  double w_ = 0;  // tone: radians per sample
  uint64_t zmin_ = 0, ctol_ = 0;
  uint64_t pos_ = 0;  // samples fed

  // The block being accumulated.
  unsigned bn_ = 0;
  double sumsq_ = 0;
  float peak_ = 0;
  // The tone fit's normal equations over basis {sin, cos, t sin, t cos, 1} (t from the block's
  // centre): the upper triangle of the Gram matrix and the projections of x.
  double gram_[15] = {}, proj_[5] = {};
  double t_ = 0;
  double rs_ = 0, rc_ = 1, ws_ = 0, wc_ = 1;  // the tone's sin/cos at this sample, its step

  std::vector<Block> blocks_;
  uint64_t clipped_ = 0;

  // Second differences: d2 at sample i is known once i + 1 has arrived.
  float x1_ = 0, x2_ = 0;
  std::vector<float> d2_;          // |d2| of the current d2 block
  uint64_t d2_start_ = 0;          // sample of d2_[0]
  std::vector<float> d2_median_;   // per block
  std::vector<float> scratch_;
  std::vector<ClickCand> clicks_;  // candidates against their own block's median
  std::optional<ClickCand> open_click_;

  // Exact-zero runs of at least zmin_.
  bool in_zero_ = false;
  uint64_t zstart_ = 0;
  std::vector<std::pair<uint64_t, uint64_t>> zero_runs_;  // offset, len
};

// One channel's events, for the cross-channel merge: `group` ties the members of one network
// sender's run (their events within tol samples are one event); -1 = no group.
struct WatchChannelEvents {
  unsigned ch = 0;
  int group = -1;
  const std::vector<WatchEvent>* events = nullptr;
};

struct WatchMergedEvent {
  WatchEvent ev;
  std::vector<unsigned> ch;
};

// Every channel's events in one list by offset, those of one group of the same kind within tol
// samples of each other merged (channels listed, the earliest offset, the longest len).
std::vector<WatchMergedEvent> merge_watch_events(const std::vector<WatchChannelEvents>& chans,
                                                 uint64_t tol);

}  // namespace st
