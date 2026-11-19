#pragma once

// GET /api/genie/wait: block until a condition holds on the selected inputs, instead of a script's
// sleep-and-poll loop. The handler feeds one tick per analysis refresh (~100 ms) of what each
// selected input reads now; WaitTracker decides when the condition has held long enough.
//
// Pure: no HTTP, no daemon state, no clock of its own (the caller passes the time), so tests drive
// it with synthetic ticks.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "constants.h"

namespace st {

enum class WaitUntil { Sound, Silence, Tone, Live, Change };

// "sound", "silence", "tone", "live", "change".
const char* wait_until_name(WaitUntil u);
bool parse_wait_until(const std::string& s, WaitUntil* out);

struct WaitCondition {
  WaitUntil until = WaitUntil::Sound;
  double threshold_db = kGenieSoundThresholdDb;  // on the unheld 100 ms peak
  double min_rms_db = kGeniePlayingRmsDb;        // sound (and change's `playing`) also needs this RMS
  std::optional<double> freq_hz;                 // tone: empty = any valid tone
  double tol_hz = kGenieToneTolHz;
  uint64_t hold_ms = kGenieWaitHoldMs;           // how long it must hold without a break
  bool all = false;                              // every selected input, not just one
};

// One selected input at one tick.
struct WaitSample {
  unsigned ch = 0;
  float rms_db = -120.0f;
  float peak_now_db = -120.0f;  // the latest 100 ms window's peak: no hold
  bool tone_valid = false;
  float tone_hz = 0.0f;
  // A network input's sender: its state is "streaming" and how many frames it has delivered.
  bool streaming = false;
  uint64_t frames_received = 0;
};

// genie/sound's `playing`: the unheld peak above threshold_db and the RMS above min_rms_db.
bool wait_playing(const WaitCondition& c, const WaitSample& s);

// Per input, whether the condition holds on it right now:
//   sound    playing
//   silence  !sound_now (the unheld peak at or below threshold_db: no 3 s hold tail)
//   tone     a valid tone, within tol_hz of freq_hz when given
//   live     streaming and frames_received past where it stood at the input's first tick, so a
//            sender that stopped just before the request (still "streaming" for up to 1 s) is not live;
//            only network inputs can be
//   change   playing differs from what it was at the input's first tick
// and the whole: some input (all: every input of the tick, and at least one) has held its
// condition for hold_ms without a break. Inputs are keyed by ring column; one that a re-resolved
// selector brings in mid-wait starts its baseline and its hold at its first tick, and one that
// drops out of the selection for a tick starts its hold over when it comes back.
class WaitTracker {
 public:
  explicit WaitTracker(const WaitCondition& c) : c_(c) {}

  // One tick at t_ms (any monotonic ms clock) with the inputs selected now, read from the
  // analysis reading at `sample` (its ring sample counter). True once met.
  bool update(uint64_t t_ms, const std::vector<WaitSample>& now, uint64_t sample = 0);

  // Once met: where the run of ticks that met it started — the first tick of the hold, as its
  // t_ms and its sample. With any, the input that met it (the earliest-started of those that did
  // on that tick); with all, the latest-started input: when every one was holding.
  struct Onset {
    uint64_t t_ms = 0;
    uint64_t sample = 0;
  };
  const std::optional<Onset>& met_onset() const { return met_; }

  // As of the last update, for an input of that tick: its condition holds now, and for change the
  // direction it flipped (+1 started playing, -1 stopped, 0 no flip).
  bool holds(unsigned ch) const;
  int changed(unsigned ch) const;

 private:
  struct State {
    bool base_playing = false;
    uint64_t base_frames = 0;
    std::optional<uint64_t> since_ms;  // when the condition last became true
    uint64_t since_sample = 0;         // ...and the sample of that tick's reading
    int changed = 0;
    uint64_t last_tick = 0;
  };
  WaitCondition c_;
  uint64_t tick_ = 0;
  std::map<unsigned, State> st_;
  std::optional<Onset> met_;
};

// Why a wait that timed out may have found nothing, from its last tick's network inputs: the
// senders that were not streaming. Empty when that explains nothing: no network input among them,
// all of them streaming, a condition a stopped sender cannot fail (silence) or that is about the
// inputs, not a link (change), or a wait not about senders (`about_senders` false: no sender
// selector and not only network inputs, e.g. an analog DUT hunt with idle NET inputs).
struct WaitNetInput {
  std::string label;   // "NET 3"
  std::string device;  // who is (or was) on it; empty if nobody
  std::string state;   // NetChannelStatus::state
  std::optional<uint64_t> last_audio_ms_ago;
};
std::string wait_net_reason(WaitUntil u, const std::vector<WaitNetInput>& net,
                            bool about_senders = true);

}  // namespace st
