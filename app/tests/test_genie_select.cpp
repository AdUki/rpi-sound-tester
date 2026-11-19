// The genie input selector: ch lists, kind filters, and NET senders by name, resolved against
// synthetic NetAudioServer::status() lists.
#include "genie_select.h"

#include <string>
#include <vector>

#include "check.h"

using namespace st;

namespace {

// IN 1-6, NET 1-8 on columns 6..13, two device columns 14..15.
ChannelLayout layout() {
  ChannelLayout l;
  l.local = 6;
  l.net = 8;
  l.device = 2;
  return l;
}

std::vector<NetChannelStatus> idle_net() {
  std::vector<NetChannelStatus> v(8);
  for (unsigned c = 0; c < v.size(); ++c) v[c].channel = c;
  return v;
}

// A connected run of `count` channels from `base`, as claim_channels() leaves it.
void put_run(std::vector<NetChannelStatus>& net, unsigned base, unsigned count,
             const std::string& device, const std::string& stream, const std::string& state,
             const std::string& peer, const std::string& host = "", const std::string& mac = "") {
  for (unsigned i = 0; i < count; ++i) {
    NetChannelStatus& s = net[base + i];
    s.connected = true;
    s.device = device;
    s.last_device = device;
    s.name = device;
    s.stream = stream;
    s.last_stream = stream;
    s.host = host;
    s.mac = mac;
    s.peer = peer;
    s.state = state;
    s.stream_index = i + 1;
    s.stream_count = count;
  }
}

// A channel a sender has left: only what is remembered.
void put_gone(std::vector<NetChannelStatus>& net, unsigned c, const std::string& device,
              const std::string& stream) {
  NetChannelStatus& s = net[c];
  s.connected = false;
  s.device.clear();
  s.stream.clear();
  s.last_device = device;
  s.last_stream = stream;
  s.state = "disconnected";
}

std::vector<unsigned> v(std::initializer_list<unsigned> l) { return l; }

void test_ch_lists() {
  std::vector<unsigned> out;
  std::string err;
  CHECK(parse_ch_list({"4,5"}, 16, &out, &err));
  CHECK(out == v({4, 5}));
  CHECK(parse_ch_list({"7", "2", "7,3"}, 16, &out, &err));  // repeated ch=, order kept, dupes dropped
  CHECK(out == v({7, 2, 3}));
  CHECK(parse_ch_list({"15"}, 16, &out, &err));
  CHECK(!parse_ch_list({"16"}, 16, &out, &err));
  CHECK_EQ(err, std::string("ch must be 0..15"));
  CHECK(!parse_ch_list({"4,"}, 16, &out, &err));
  CHECK(!parse_ch_list({""}, 16, &out, &err));
  CHECK(!parse_ch_list({"-1"}, 16, &out, &err));
  CHECK(!parse_ch_list({"x"}, 16, &out, &err));
  CHECK(!parse_ch_list({"4 5"}, 16, &out, &err));
  CHECK(!parse_ch_list({"99999999999999999999"}, 16, &out, &err));
  CHECK(!parse_ch_list({}, 16, &out, &err));
}

void test_kinds() {
  const ChannelLayout l = layout();
  CHECK_EQ(std::string(input_kind_name(input_kind(l, 0))), std::string("local"));
  CHECK_EQ(std::string(input_kind_name(input_kind(l, 6))), std::string("net"));
  CHECK_EQ(std::string(input_kind_name(input_kind(l, 13))), std::string("net"));
  CHECK_EQ(std::string(input_kind_name(input_kind(l, 14))), std::string("device"));
  InputKind k;
  CHECK(parse_input_kind("device", &k) && k == InputKind::Device);
  CHECK(!parse_input_kind("NET", &k));
}

void test_no_selector_is_every_input() {
  const InputSelection r = resolve_inputs({}, layout(), idle_net());
  CHECK(r.ok());
  CHECK_EQ(r.inputs.size(), size_t{16});
  CHECK(r.runs.empty());
}

void test_kind_filters() {
  InputSelector sel;
  sel.kind = InputKind::Net;
  InputSelection r = resolve_inputs(sel, layout(), idle_net());
  CHECK(r.inputs == v({6, 7, 8, 9, 10, 11, 12, 13}));
  sel.kind = InputKind::Device;
  CHECK(resolve_inputs(sel, layout(), idle_net()).inputs == v({14, 15}));
  // With ch, kind filters the list and keeps its order.
  sel.kind = InputKind::Local;
  sel.ch = {7, 2, 0};
  CHECK(resolve_inputs(sel, layout(), idle_net()).inputs == v({2, 0}));
}

void test_ch_order_kept() {
  InputSelector sel;
  sel.ch = {9, 1};
  const InputSelection r = resolve_inputs(sel, layout(), idle_net());
  CHECK(r.ok());
  CHECK(r.inputs == v({9, 1}));
}

void test_contradictions_are_400() {
  InputSelector sel;
  sel.ch = {6};
  sel.device = "heston";
  CHECK_EQ(resolve_inputs(sel, layout(), idle_net()).status, 400);
  InputSelector idx;
  idx.stream_index = 1;
  CHECK_EQ(resolve_inputs(idx, layout(), idle_net()).status, 400);
  InputSelector k;
  k.device = "heston";
  k.kind = InputKind::Local;
  CHECK_EQ(resolve_inputs(k, layout(), idle_net()).status, 400);
  k.kind = InputKind::Net;  // net is what device= names anyway
  CHECK_EQ(resolve_inputs(k, layout(), idle_net()).status, 404);
}

void test_device_prefix_any_case() {
  auto net = idle_net();
  put_run(net, 2, 2, "heston120-501e2d676be9", "pipewire", "streaming", "10.0.0.5:4000",
          "heston120.lan", "50:1e:2d:67:6b:e9");
  for (const char* q : {"heston", "HESTON120", "heston120-501e", "heston120.lan", "50:1E:2D"}) {
    InputSelector sel;
    sel.device = q;
    const InputSelection r = resolve_inputs(sel, layout(), net);
    CHECK(r.ok());
    CHECK(r.inputs == v({8, 9}));
    CHECK_EQ(r.runs.size(), size_t{1});
    if (r.runs.size() == 1) {
      CHECK_EQ(r.runs[0].base, 2u);
      CHECK_EQ(r.runs[0].stream_count, 2u);
      CHECK_EQ(r.runs[0].state, std::string("streaming"));
    }
  }
  InputSelector miss;
  miss.device = "501e";  // a prefix, not a substring
  CHECK_EQ(resolve_inputs(miss, layout(), net).status, 404);
}

// A stale run (connected, frames frozen) with the live one's name must not win.
void test_streaming_beats_connected() {
  auto net = idle_net();
  put_run(net, 0, 2, "atron-aa", "main", "connected", "10.0.0.7:5000");
  put_run(net, 4, 2, "atron-aa", "main", "streaming", "10.0.0.7:5002");
  InputSelector sel;
  sel.device = "atron";
  const InputSelection r = resolve_inputs(sel, layout(), net);
  CHECK(r.ok());
  CHECK(r.inputs == v({10, 11}));
  CHECK_EQ(r.runs.size(), size_t{1});

  // With nothing streaming, every connected match is returned.
  net[4].state = net[5].state = "connected";
  const InputSelection both = resolve_inputs(sel, layout(), net);
  CHECK(both.inputs == v({6, 7, 10, 11}));
  CHECK_EQ(both.runs.size(), size_t{2});
}

// main and notif of one host: two runs, grouped, in channel order; stream= picks one exactly.
void test_streams() {
  auto net = idle_net();
  put_run(net, 3, 2, "heston", "notif", "streaming", "10.0.0.5:1");
  put_run(net, 0, 2, "heston", "main", "streaming", "10.0.0.5:2");
  InputSelector sel;
  sel.device = "heston";
  InputSelection r = resolve_inputs(sel, layout(), net);
  CHECK(r.inputs == v({6, 7, 9, 10}));
  CHECK_EQ(r.runs.size(), size_t{2});
  if (r.runs.size() == 2) {
    CHECK_EQ(r.runs[0].stream, std::string("main"));
    CHECK(r.runs[1].inputs == v({9, 10}));
  }

  InputSelector st;
  st.stream = "notif";
  CHECK(resolve_inputs(st, layout(), net).inputs == v({9, 10}));
  st.stream = "Notif";  // exact
  CHECK_EQ(resolve_inputs(st, layout(), net).status, 404);
  st.stream = "notif";
  st.device = "other";  // both must match
  CHECK_EQ(resolve_inputs(st, layout(), net).status, 404);
  st.device = "hes";
  CHECK(resolve_inputs(st, layout(), net).inputs == v({9, 10}));

  st.stream_index = 2;
  CHECK(resolve_inputs(st, layout(), net).inputs == v({10}));
  sel.stream_index = 1;
  CHECK(resolve_inputs(sel, layout(), net).inputs == v({6, 9}));
  sel.stream_index = 3;
  const InputSelection none = resolve_inputs(sel, layout(), net);
  CHECK_EQ(none.status, 404);
  CHECK(none.error.find("stream_index 3") != std::string::npos);
}

void test_gone_sender_is_404_with_last_seen() {
  auto net = idle_net();
  put_gone(net, 5, "heston120-501e", "pipewire");
  put_gone(net, 6, "heston120-501e", "pipewire");
  put_gone(net, 1, "atron-aa", "main");
  put_run(net, 2, 1, "laptop", "", "streaming", "10.0.0.9:1");
  InputSelector sel;
  sel.device = "heston";
  const InputSelection r = resolve_inputs(sel, layout(), net);
  CHECK_EQ(r.status, 404);
  CHECK(r.error.find("heston") != std::string::npos);
  CHECK(r.inputs.empty());
  CHECK_EQ(r.last_seen.size(), size_t{2});
  if (r.last_seen.size() == 2) {
    CHECK_EQ(r.last_seen[0].channel, 5u);
    CHECK_EQ(r.last_seen[0].input, 11u);
    CHECK_EQ(r.last_seen[0].last_stream, std::string("pipewire"));
    CHECK_EQ(r.last_seen[0].state, std::string("disconnected"));
  }
  InputSelector by_stream;
  by_stream.stream = "main";
  const InputSelection s = resolve_inputs(by_stream, layout(), net);
  CHECK_EQ(s.status, 404);
  CHECK_EQ(s.last_seen.size(), size_t{1});
  // Never-used channels are not "seen".
  InputSelector nobody;
  nobody.device = "zzz";
  CHECK(resolve_inputs(nobody, layout(), net).last_seen.empty());
}

// A channel whose stream_index points at a base held by another connection is not that run's.
void test_member_of_another_peer_is_ignored() {
  auto net = idle_net();
  put_run(net, 0, 2, "a", "", "streaming", "10.0.0.1:1");
  net[1].peer = "10.0.0.2:1";  // torn: base 0 is someone else's
  InputSelector sel;
  sel.device = "a";
  CHECK(resolve_inputs(sel, layout(), net).inputs == v({6}));
}

// A shorter status list (or a layout with fewer NET channels) never reads past either.
void test_short_status() {
  auto net = idle_net();
  put_run(net, 0, 1, "a", "", "streaming", "p");
  net.resize(1);
  InputSelector sel;
  sel.device = "a";
  CHECK(resolve_inputs(sel, layout(), net).inputs == v({6}));
}

void test_sync_side() {
  InputSelector sel;
  std::string err;
  CHECK(parse_sync_side("4", 16, &sel, &err));
  CHECK(sel.ch == v({4}));
  CHECK(!sel.by_sender());
  CHECK(!parse_sync_side("16", 16, &sel, &err));  // out of range
  CHECK(!parse_sync_side("", 16, &sel, &err));
  CHECK(!parse_sync_side("/", 16, &sel, &err));

  CHECK(parse_sync_side("heston", 16, &sel, &err));
  CHECK_EQ(sel.device, std::string("heston"));
  CHECK(sel.stream.empty() && sel.ch.empty());
  CHECK(parse_sync_side("heston/main", 16, &sel, &err));
  CHECK_EQ(sel.device, std::string("heston"));
  CHECK_EQ(sel.stream, std::string("main"));
  CHECK(parse_sync_side("/notif", 16, &sel, &err));
  CHECK(sel.device.empty());
  CHECK_EQ(sel.stream, std::string("notif"));
  CHECK(parse_sync_side("4a", 16, &sel, &err));  // not all digits: a device prefix
  CHECK_EQ(sel.device, std::string("4a"));

  // A side resolves like any selector: its first input is member 1 of the first matched run.
  auto net = idle_net();
  put_run(net, 2, 2, "heston120", "main", "streaming", "p1");
  CHECK(parse_sync_side("hest/main", 16, &sel, &err));
  const InputSelection r = resolve_inputs(sel, layout(), net);
  CHECK(r.ok());
  CHECK(r.inputs == v({8, 9}));
}

}  // namespace

int main() {
  test_ch_lists();
  test_kinds();
  test_no_selector_is_every_input();
  test_kind_filters();
  test_ch_order_kept();
  test_contradictions_are_400();
  test_device_prefix_any_case();
  test_streaming_beats_connected();
  test_streams();
  test_gone_sender_is_404_with_last_seen();
  test_member_of_another_peer_is_ignored();
  test_short_status();
  test_sync_side();
  return report("genie_select");
}
