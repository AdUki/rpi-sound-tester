#pragma once

// Which inputs a genie request is about. A NET sender's channels move whenever it reconnects (a
// DUT reboot, a PipeWire restart, a tester restart), so a script that names ring columns measures
// whatever took them over. The selector names the sender instead and is resolved per request
// against NetAudioServer::status(); plain `ch=` still works for the engine card's inputs and for
// anyone who already knows the columns.
//
// Pure: no HTTP, no daemon state beyond what the caller passes in, so tests drive it with
// synthetic NetChannelStatus lists.

#include <optional>
#include <string>
#include <vector>

#include "channel_layout.h"
#include "net_audio.h"

namespace st {

enum class InputKind { Local, Net, Device };

// "local", "net", "device": what /api/state inputs[].kind says.
const char* input_kind_name(InputKind k);
bool parse_input_kind(const std::string& s, InputKind* out);
InputKind input_kind(const ChannelLayout& layout, unsigned c);

struct InputSelector {
  std::vector<unsigned> ch;   // explicit ring columns, in the caller's order; empty = not given
  std::string device;         // case-insensitive prefix of a live sender's device/name/host/mac
  std::string stream;         // exact name of a live sender's stream (the PCM's `name`)
  unsigned stream_index = 0;  // 1-based member of each matched run; 0 = every member
  std::optional<InputKind> kind;

  bool by_sender() const { return !device.empty() || !stream.empty(); }
};

// The query's ch values: repeated `ch=` and comma lists (`ch=4,5&ch=7`) both count, in order,
// duplicates dropped. False with *err set on an empty item, a non-number or one out of range.
bool parse_ch_list(const std::vector<std::string>& values, unsigned total,
                   std::vector<unsigned>* out, std::string* err);

// One sender's run of NET channels: stream_index 1..stream_count on consecutive channels.
struct SelectedRun {
  unsigned base = 0;             // NET channel (0-based) of the run's first member
  std::vector<unsigned> inputs;  // ring columns of the selected members, in stream order
  std::string device;
  std::string stream;
  std::string state;             // "streaming" or "connected"
  unsigned stream_count = 1;
};

// A channel that once carried a matching sender but has none now: what a 404 lists so the caller
// can tell "never connected" from "went away".
struct LastSeen {
  unsigned channel = 0;          // NET channel
  unsigned input = 0;            // ring column
  std::string last_device;
  std::string last_stream;
  std::string state;
};

struct InputSelection {
  int status = 200;              // 200, or the HTTP status of `error` (400 or 404)
  std::string error;
  std::vector<unsigned> inputs;  // flat, in run order when the selector named a sender
  std::vector<SelectedRun> runs; // only when the selector named a sender
  std::vector<LastSeen> last_seen;  // on a sender 404
  bool ok() const { return status == 200; }
};

// One side of a genie/sync pair (`a=` / `b=`): a ring column ("4"), or a sender as
// "<device>[/<stream>]" — "heston", "heston/main", or "/main" for the stream alone. Device names,
// hosts and MACs carry no '/'. Fills *out (ch, or device/stream); false with *err on an empty
// value, an empty device-and-stream ("/") or a column out of range.
bool parse_sync_side(const std::string& v, unsigned total, InputSelector* out, std::string* err);

// Resolves a selector against the layout and the NET status (index = NET channel).
//   - nothing given: every input, optionally only those of `kind`
//   - ch: those columns in that order, filtered by `kind` when also given
//   - device/stream (both given = both must match): the connected runs that match, the
//     streaming ones only when any is streaming (a stale run can carry the live one's name);
//     stream_index then picks one member of each. Nothing connected matches: 404 with last_seen.
//   - ch with device/stream, stream_index without them, or kind other than net with them: 400.
InputSelection resolve_inputs(const InputSelector& sel, const ChannelLayout& layout,
                              const std::vector<NetChannelStatus>& net);

}  // namespace st
