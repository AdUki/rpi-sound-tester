// GET /api/genie/wait's condition: per-input verdicts, the hold, any/all, the change baseline and
// a selection that moves, driven with synthetic ticks.
#include "genie_wait.h"

#include <string>
#include <vector>

#include "check.h"

using namespace st;

namespace {

WaitSample quiet(unsigned ch) {
  WaitSample s;
  s.ch = ch;
  return s;
}

// Playing music: loud peak and RMS, no tone.
WaitSample loud(unsigned ch) {
  WaitSample s;
  s.ch = ch;
  s.rms_db = -20.0f;
  s.peak_now_db = -14.0f;
  return s;
}

// An idle analog input: the noise floor's peaks clear -60 dB, its RMS does not.
WaitSample noise_floor(unsigned ch) {
  WaitSample s;
  s.ch = ch;
  s.rms_db = -75.0f;
  s.peak_now_db = -56.0f;
  return s;
}

WaitSample tone(unsigned ch, float hz) {
  WaitSample s = loud(ch);
  s.tone_valid = true;
  s.tone_hz = hz;
  return s;
}

WaitSample net(unsigned ch, bool streaming, uint64_t frames) {
  WaitSample s = quiet(ch);
  s.streaming = streaming;
  s.frames_received = frames;
  return s;
}

WaitCondition until(WaitUntil u, uint64_t hold_ms = 300, bool all = false) {
  WaitCondition c;
  c.until = u;
  c.hold_ms = hold_ms;
  c.all = all;
  return c;
}

void test_names() {
  for (WaitUntil u : {WaitUntil::Sound, WaitUntil::Silence, WaitUntil::Tone, WaitUntil::Live,
                      WaitUntil::Change}) {
    WaitUntil back = WaitUntil::Sound;
    CHECK(parse_wait_until(wait_until_name(u), &back));
    CHECK(back == u);
  }
  WaitUntil x;
  CHECK(!parse_wait_until("Sound", &x));
  CHECK(!parse_wait_until("", &x));
  CHECK(!parse_wait_until("playing", &x));
}

void test_playing() {
  const WaitCondition c = until(WaitUntil::Sound);
  CHECK(wait_playing(c, loud(0)));
  CHECK(!wait_playing(c, quiet(0)));
  CHECK(!wait_playing(c, noise_floor(0)));  // the peak alone is not playing
  WaitCondition lax = c;
  lax.min_rms_db = -90;
  CHECK(wait_playing(lax, noise_floor(0)));
}

// The condition has to hold hold_ms without a break, counted from the tick it first held.
void test_hold() {
  WaitTracker t(until(WaitUntil::Sound, 300));
  CHECK(!t.update(0, {quiet(4)}));
  CHECK(!t.update(100, {loud(4)}));
  CHECK(t.holds(4));
  CHECK(!t.update(200, {loud(4)}));
  CHECK(!t.update(300, {loud(4)}));
  CHECK(t.update(400, {loud(4)}));

  // A break starts the hold over.
  WaitTracker b(until(WaitUntil::Sound, 300));
  CHECK(!b.update(0, {loud(4)}));
  CHECK(!b.update(200, {loud(4)}));
  CHECK(!b.update(300, {quiet(4)}));
  CHECK(!b.holds(4));
  CHECK(!b.update(400, {loud(4)}));
  CHECK(!b.update(600, {loud(4)}));
  CHECK(b.update(700, {loud(4)}));

  // hold_ms 0: the first tick it holds on.
  WaitTracker z(until(WaitUntil::Sound, 0));
  CHECK(z.update(0, {loud(4)}));
}

void test_silence() {
  // The unheld peak: an input that just stopped is silent on the next tick, no 3 s tail.
  WaitTracker t(until(WaitUntil::Silence, 200));
  CHECK(!t.update(0, {loud(2)}));
  CHECK(!t.update(100, {quiet(2)}));
  CHECK(!t.update(200, {quiet(2)}));
  CHECK(t.update(300, {quiet(2)}));

  // An idle analog input's noise floor peaks above -60 dB: not silence at the default threshold,
  // silence at a lower one.
  WaitTracker n(until(WaitUntil::Silence, 0));
  CHECK(!n.update(0, {noise_floor(2)}));
  WaitCondition c = until(WaitUntil::Silence, 0);
  c.threshold_db = -50;
  WaitTracker n2(c);
  CHECK(n2.update(0, {noise_floor(2)}));
}

void test_tone() {
  WaitCondition c = until(WaitUntil::Tone, 0);
  c.freq_hz = 1000.0;
  c.tol_hz = 5.0;
  WaitTracker t(c);
  CHECK(!t.update(0, {loud(0)}));            // music: no valid tone
  CHECK(!t.update(100, {tone(0, 440.0f)}));  // the wrong tone
  CHECK(!t.update(200, {tone(0, 1006.0f)}));
  CHECK(t.update(300, {tone(0, 1004.5f)}));

  // No freq_hz: any valid tone.
  WaitTracker any(until(WaitUntil::Tone, 0));
  CHECK(!any.update(0, {loud(0)}));
  CHECK(any.update(100, {tone(0, 440.0f)}));
}

// Live: streaming and frames moving past where they stood at the first tick, so a sender that
// stopped right before the request (its state still "streaming" for up to 1 s) does not count.
void test_live() {
  WaitTracker t(until(WaitUntil::Live, 0));
  CHECK(!t.update(0, {net(8, true, 1000)}));
  CHECK(!t.update(100, {net(8, true, 1000)}));  // frozen
  CHECK(t.update(200, {net(8, true, 5800)}));

  WaitTracker s(until(WaitUntil::Live, 0));
  CHECK(!s.update(0, {net(8, false, 0)}));
  CHECK(!s.update(100, {net(8, false, 4800)}));  // frames, but the state says not streaming
  CHECK(s.update(200, {net(8, true, 9600)}));

  // The counter went backwards (the channel forgotten, claimed afresh): new baseline.
  WaitTracker r(until(WaitUntil::Live, 0));
  CHECK(!r.update(0, {net(8, true, 50000)}));
  CHECK(!r.update(100, {net(8, true, 100)}));
  CHECK(r.update(200, {net(8, true, 4900)}));

  // A local input is never live.
  WaitTracker l(until(WaitUntil::Live, 0));
  CHECK(!l.update(0, {loud(0)}));
  CHECK(!l.update(100, {loud(0)}));
}

// Change: playing against each input's own first tick, reported with its direction.
void test_change() {
  WaitTracker t(until(WaitUntil::Change, 200));
  CHECK(!t.update(0, {loud(0), quiet(1), noise_floor(2)}));
  CHECK(!t.update(100, {loud(0), quiet(1), noise_floor(2)}));
  CHECK_EQ(t.changed(0), 0);
  // IN 2 starts playing, IN 1 stops.
  CHECK(!t.update(200, {quiet(0), loud(1), noise_floor(2)}));
  CHECK_EQ(t.changed(0), -1);
  CHECK_EQ(t.changed(1), 1);
  CHECK_EQ(t.changed(2), 0);
  CHECK(!t.update(300, {quiet(0), loud(1), noise_floor(2)}));
  CHECK(t.update(400, {quiet(0), loud(1), noise_floor(2)}));
  CHECK(t.holds(0));
  CHECK(t.holds(1));
  CHECK(!t.holds(2));

  // A flip that flips back before hold_ms is no change.
  WaitTracker b(until(WaitUntil::Change, 300));
  CHECK(!b.update(0, {quiet(5)}));
  CHECK(!b.update(100, {loud(5)}));
  CHECK(!b.update(200, {quiet(5)}));
  CHECK_EQ(b.changed(5), 0);
  CHECK(!b.update(300, {quiet(5)}));
  CHECK(!b.update(1000, {quiet(5)}));

  // The noise floor's peaks are not playing, so they never read as a change.
  WaitTracker n(until(WaitUntil::Change, 0));
  CHECK(!n.update(0, {quiet(3)}));
  CHECK(!n.update(100, {noise_floor(3)}));
}

void test_any_all() {
  WaitTracker any(until(WaitUntil::Sound, 100));
  CHECK(!any.update(0, {loud(8), quiet(9)}));
  CHECK(any.update(100, {loud(8), quiet(9)}));

  WaitTracker all(until(WaitUntil::Sound, 100, true));
  CHECK(!all.update(0, {loud(8), quiet(9)}));
  CHECK(!all.update(100, {loud(8), quiet(9)}));
  CHECK(!all.update(200, {loud(8), loud(9)}));  // 9's hold starts here
  CHECK(all.update(300, {loud(8), loud(9)}));

  // Nothing selected (a sender not connected yet) is never met, all or any.
  WaitTracker e(until(WaitUntil::Silence, 0, true));
  CHECK(!e.update(0, {}));
  WaitTracker e2(until(WaitUntil::Silence, 0));
  CHECK(!e2.update(0, {}));
}

// A re-resolved selector: the sender moves from columns 8/9 to 12/13 mid-wait.
void test_moved_selection() {
  WaitTracker t(until(WaitUntil::Sound, 200));
  CHECK(!t.update(0, {loud(8), loud(9)}));
  CHECK(!t.update(100, {loud(8), loud(9)}));
  // Gone for a tick (reconnecting), then on new columns: their hold starts there.
  CHECK(!t.update(200, {}));
  CHECK(!t.update(300, {loud(12), loud(13)}));
  CHECK(!t.update(400, {loud(12), loud(13)}));
  CHECK(t.update(500, {loud(12), loud(13)}));

  // An input that leaves the selection and comes back starts its hold over, not where it left.
  WaitTracker r(until(WaitUntil::Sound, 300));
  CHECK(!r.update(0, {loud(8)}));
  CHECK(!r.update(100, {loud(8)}));
  CHECK(!r.update(200, {}));
  CHECK(!r.update(300, {loud(8)}));  // would be "held 300 ms" counting from tick 0
  CHECK(!r.update(500, {loud(8)}));
  CHECK(r.update(600, {loud(8)}));

  // change: a column new to the wait takes its baseline at its first tick.
  WaitTracker c(until(WaitUntil::Change, 0));
  CHECK(!c.update(0, {quiet(8)}));
  CHECK(!c.update(100, {loud(12)}));  // first seen playing: its baseline, not a change
  CHECK(c.update(200, {quiet(12)}));
  CHECK_EQ(c.changed(12), -1);
}

// Where the condition started holding: the first tick of the run that met it, by its time and its
// analysis reading's sample; not the tick it was met on, and not an earlier run that broke.
void test_met_onset() {
  WaitTracker t(until(WaitUntil::Sound, 300));
  CHECK(!t.met_onset());
  CHECK(!t.update(0, {quiet(4)}, 1000));
  CHECK(!t.update(100, {loud(4)}, 5800));   // a run that breaks
  CHECK(!t.update(200, {quiet(4)}, 10600));
  CHECK(!t.update(300, {loud(4)}, 15400));  // the run that meets it
  CHECK(!t.update(400, {loud(4)}, 20200));
  CHECK(!t.update(500, {loud(4)}, 25000));
  CHECK(!t.met_onset());
  CHECK(t.update(600, {loud(4)}, 29800));
  CHECK(t.met_onset().has_value());
  CHECK_EQ(t.met_onset()->t_ms, 300u);
  CHECK_EQ(t.met_onset()->sample, 15400u);

  // hold_ms 0: met on the tick it starts, which is then its own onset.
  WaitTracker z(until(WaitUntil::Silence, 0));
  CHECK(z.update(700, {quiet(1)}, 42));
  CHECK_EQ(z.met_onset()->t_ms, 700u);
  CHECK_EQ(z.met_onset()->sample, 42u);

  // any: the earliest-started input of those that met it on that tick.
  WaitTracker any(until(WaitUntil::Sound, 200));
  CHECK(!any.update(0, {quiet(8), quiet(9)}, 100));
  CHECK(!any.update(100, {quiet(8), loud(9)}, 200));
  CHECK(!any.update(200, {loud(8), loud(9)}, 300));
  CHECK(any.update(300, {loud(8), loud(9)}, 400));
  CHECK_EQ(any.met_onset()->t_ms, 100u);
  CHECK_EQ(any.met_onset()->sample, 200u);

  // all: when every input was holding, i.e. the latest-started one.
  WaitTracker all(until(WaitUntil::Sound, 100, true));
  CHECK(!all.update(0, {loud(8), quiet(9)}, 100));
  CHECK(!all.update(100, {loud(8), quiet(9)}, 200));
  CHECK(!all.update(200, {loud(8), loud(9)}, 300));
  CHECK(all.update(300, {loud(8), loud(9)}, 400));
  CHECK_EQ(all.met_onset()->t_ms, 200u);
  CHECK_EQ(all.met_onset()->sample, 300u);
}

void test_net_reason() {
  CHECK_EQ(wait_net_reason(WaitUntil::Sound, {}), std::string());
  CHECK_EQ(wait_net_reason(WaitUntil::Sound, {{"NET 1", "heston", "streaming", 3}}), std::string());
  CHECK_EQ(wait_net_reason(WaitUntil::Sound, {{"NET 1", "heston", "connected", 4200}}),
           std::string("sender not streaming: NET 1 (heston connected, last audio 4200 ms ago)"));
  CHECK_EQ(wait_net_reason(WaitUntil::Live, {{"NET 2", "atron", "connected", std::nullopt}}),
           std::string("sender not streaming: NET 2 (atron connected, no audio yet)"));
  CHECK_EQ(wait_net_reason(WaitUntil::Tone, {{"NET 1", "heston", "streaming", 3},
                                             {"NET 3", "", "", std::nullopt},
                                             {"NET 4", "amp", "disconnected", std::nullopt}}),
           std::string("sender not streaming: NET 3 (no sender); NET 4 (amp disconnected)"));
  // Silence cannot be kept from happening by a sender that stopped.
  CHECK_EQ(wait_net_reason(WaitUntil::Silence, {{"NET 1", "heston", "connected", 4200}}),
           std::string());
  // A wait not about senders (no selector: analog and NET inputs) is not blamed on idle NET inputs,
  // nor is `change`, which is about the inputs.
  CHECK_EQ(wait_net_reason(WaitUntil::Sound, {{"NET 1", "", "", std::nullopt}}, false), std::string());
  CHECK_EQ(wait_net_reason(WaitUntil::Change, {{"NET 1", "heston", "connected", 4200}}), std::string());
}

}  // namespace

int main() {
  test_names();
  test_playing();
  test_hold();
  test_silence();
  test_tone();
  test_live();
  test_change();
  test_any_all();
  test_moved_selection();
  test_met_onset();
  test_net_reason();
  return report("genie_wait");
}
