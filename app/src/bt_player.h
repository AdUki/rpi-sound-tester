#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "bluetooth.h"
#include "control.h"

namespace st {

// The tester's own media player: what a speaker it plays to is told over AVRCP. The track is the
// Bluetooth output's signal, the artist the tester, and the speaker's buttons work the output the
// way the console does. All of it is read from and written to the Bluetooth sink's SinkControl, so
// the console and the speaker never disagree. Its first two channels are the speaker's L and R.
//
//   status    stopped while the output is off, paused while all its channels are muted
//   play      unmutes, switching the output on first if it is off
//   pause     mutes
//   stop      switches the output off
//   next      the next signal in Silence, Sine, Noise, Ping, Music, on both channels; previous back

inline constexpr const char* kBtTesterArtist = "Sound Tester";

// The output's signal by name, "Sine"; "Sine / Noise" when the two channels differ.
std::string bt_tester_title(const SinkControl& s);
// What the signal is set to, "440 Hz · -20 dBFS"; empty when there is nothing to say. The
// generators' settings are Control's.
std::string bt_tester_detail(const Control& c, const SinkControl& s);
// playing | paused | stopped
const char* bt_tester_status(const SinkControl& s);
// One step through the signals, +1 or -1, on both channels of the output.
void bt_tester_step(SinkControl& s, int dir);

class BtTesterPlayer {
 public:
  // `set_output` switches the Bluetooth output on or off the way the console's switch does.
  BtTesterPlayer(const Control& ctl, SinkControl& sink, std::function<void(bool)> set_output);

  // The player as of `now_ns`. The position counts while playing, holds while paused and starts
  // again from zero with a new signal or a stop. Called from one thread only (the bus thread).
  BtPlayerInfo poll(uint64_t now_ns);
  // play | pause | playpause | stop | next | previous; false for anything else.
  bool command(const std::string& cmd);

 private:
  const Control& ctl_;
  SinkControl& sink_;
  std::function<void(bool)> set_output_;
  std::string title_;
  std::string status_;
  uint64_t played_ms_ = 0;  // before the current stretch of playing
  uint64_t since_ns_ = 0;   // when that stretch began
};

}  // namespace st
