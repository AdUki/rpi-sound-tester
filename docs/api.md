# HTTP API

Base URL: `http://soundtester` (port 80 on the device; `--port` elsewhere, e.g. `make run` on
`:8080`). Use the device's hostname or its IP address: mDNS (`soundtester.local`) is not resolvable
everywhere a script or a sender runs. All bodies are JSON except the audio streams.

How many inputs and outputs there are depends on the board. The **engine card** paces everything:
on a Pi it is the Octo, whose 6 ADCs are inputs 0–5 (IN 1–6) and whose 8 DACs are outputs 0–7
(OUT 1–8). A board without one (the Khadas VIM3L) has a timer for a clock and no inputs or outputs
of its own. After the engine card's inputs come the 6 **network inputs** fed over the LAN (NET 1–6:
inputs 6–11 on a Pi, 0–5 on a VIM3L), then the **device inputs**: columns kept for capture devices
the daemon finds at runtime, a USB interface's inputs or the VIM3L's HDMI loopback (2 on a Pi, 4
on a VIM3L; board.json's `device_inputs`). Everything that takes an input index takes all of them.
`limits.inputs_local`, `limits.inputs_device`, `limits.inputs_total` and `limits.outputs` give the
counts; each input in `GET /api/state` has a `kind` (`local`, `net` or `device`) and the `label` the
console shows ("HDMI loopback L"). Each input also carries `active`: a network channel reads
`false` until a sender has used it, a device input until a device is on it, which is how the
console keeps unused ones out of sight. The channel exists either way — the index is fixed for the
life of the daemon, and the arrays in the meters and envelope messages always carry every
channel — so a headless client can ignore `active` entirely.

Every other playback device is a **sink** (HDMI, a Pi's 3.5 mm jack, a USB interface), found at
runtime: see Sinks below.

Every PUT field is optional — send only what changes (`PUT /api/channel-map` is the exception: it
takes both maps). Out-of-range numbers clamp to their limits. What cannot be clamped is rejected: a
bad enum, an input index that does not exist, a sink `sample_rate` that is not one of its `rates`,
a channel map whose slots repeat.

A write answers `{"ok": true}` unless its section shows another body. A failure answers
`{"error": "why"}` with **400** for a body that is not JSON, lacks a required field or holds a bad
value; **404** for an input, output, sink, channel or Bluetooth device that does not exist; **409**
when the request does not fit the current state; **503** when what it needs is not running or not
ready yet. A path under `/api/` that no endpoint answers, or the wrong method on one, is a **404**
`{"error": "no such endpoint: GET /api/status; see GET /api"}`.

### `GET /api`
This document, rendered to HTML (built from `api.md`). Also served as the static `/api.html`.

## State

### `GET /api/state`
The whole device state in one object: `version`, `unix_ms`, `daemon_uptime_s`, `inputs`, `outputs`,
`sinks`, `sources`, `devices`, `generators`, `channel_map`, `capture`, `engine`, `system`, and
`limits` (slider ranges and feature flags the console reads). `engine.backend` is `card` (the engine card), `timer` (a board without one) or
`simulator`. `limits.channel_map` and `limits.sync_watch` are `false` without an engine card: the
TDM slot map and the I2S sync watch are the Octo's. `limits.bluetooth` says Bluetooth is running.
Each input and output has a `name`, set only in `config.json` — there is no API to change it. A
network input with none takes its sender's (`thinkpad`, `thinkpad (1/2)` for half of a stereo
stream), with ` (gone)` added once that sender has disconnected (`net.state` `disconnected`), so a
name never reads like a live sender that is not there: pick NET inputs by `net.state` or with the
genie selector, not by name.

- `version`: `{"git": "8c6a898cad15-dirty", "build_time": "2026-11-19T08:42:26Z"}` — `git describe
  --always --dirty` of the tree the daemon was built from (`-dirty`: with uncommitted changes) and
  the build time in UTC. In an image build, `build_time` is the reproducible-build time (the newest
  source file's), and `git` is `"unknown"` when the build could not ask git. Compare it with the
  checkout before trusting a field documented here.
- `unix_ms`: the wall clock (ms since 1970, UTC) when the reply was built, read together with
  `engine.samples`, to line tester sample indices up with a DUT's log. `daemon_uptime_s`: seconds
  since the daemon started (`system.uptime_s` is the machine's).
- `inputs[]`: `ch`, `name`, `kind`, `label`, `active`, `gain_db`, `mute`, `bypass`, `gain_min_db`,
  `gain_max_db`, `net` (null but on a network input: its link, `{name, state, transport, rate, format, encoding}`,
  as in `GET /api/net` with `rate` the sender's own, which the dashboard card shows),
  `rms_db`, `peak_db`, `tone` (as in `GET /api/genie/sound`). One per input, `limits.inputs_total`.
  `GET /api/inputs` answers this array on its own.
- `outputs[]`: the engine card's outputs, `ch`, `label` (`"OUT 1"` for `ch` 0: what the console
  calls it; a sink's channels are named by their `position` instead), `source`, `gain_db`, `mute`,
  `name`; empty without an engine card.
- `sinks`, `sources`, `devices`: as `GET /api/sinks`, `/api/sources` and `/api/devices`.
- `capture`: as `GET /api/capture/status` without the `live_*`, `analyze_seconds` and `ring_seconds`
  fields; `analyze_frames` is what the next freeze copies.
- `engine`: `running`, `sim`, `backend`, `device` (the engine card's ALSA name), `rate`, `period`
  and `periods` (frames per period, and how many), `format`, `capture_channels` (what the card
  captures, before the slot map), `xruns`, `generation` (bumps on every xrun), `samples` (the sample
  counter), `last_error`.
- `system`: `cpu_pct`, `cpu_cores[]` (per core), `temp_c`, `uptime_s`,
  `mem` {`total_kb`, `used_kb`, `available_kb`}, `throttle` {`available`, `under_voltage`,
  `throttled`, `freq_capped`, `under_voltage_seen`, `throttled_seen`} (the Pi firmware's flags;
  `available` is false elsewhere), `sync_errors`, `hostname`, `ips`, `has_saved_config`,
  `data_persistent`, `listen_streams` (open listen streams), `loopback_offset_samples` (set in
  `config.json` only).
- `limits`: counts: `inputs_total`, `inputs_local`, `inputs_device`, `outputs`, `net_inputs`
  (running), `net_inputs_configured` (what the next start has, see `PUT /api/net`),
  `net_inputs_min`, `net_inputs_max`, `sinks_max`; gain ranges: `input_gain_min_db`, `input_gain_max_db`, `net_gain_min_db`, `net_gain_max_db`; capture:
  `capture_max_frames`, `env_column_frames` (frames per scope envelope column), `pinned_mb` (memory
  locked for the ring, the snapshot and the sinks); listening: `listen_codecs`,
  `listen_default_codec`, `listen_bitrate_kbps`, `listen_bitrate_min_kbps`,
  `listen_bitrate_max_kbps`, `opus_rate`; network input: `net_port`, `net_delay_ms`,
  `net_delay_min_ms`, `net_delay_max_ms`; feature flags: `channel_map`, `sync_watch` and `bluetooth`
  as above, `restart` (`POST /api/system/restart` is allowed); `capture_config`, `input_mute`, `telemetry_mask`, `net` and `net_forget` are always true, so a console
  can tell an older daemon by their absence.

## Inputs

### `GET /api/inputs`
`GET /api/state` `inputs[]` on its own: one object per input, `ch`, `name`, `kind`, `label`,
`active`, `net`, gains, meters and `tone`, as described there.

### `PUT /api/inputs/{ch}`
```json
{"gain_db": 12.0, "mute": false}
```
Gain and mute, applied **before the ring buffer** — so every reading (meters, spectrum, THD+N,
scope, xcorr, listen streams) is post-gain and a muted channel reads as silent everywhere.

An ADC channel, the engine card's or a device's, takes 0…+40 dB: it cannot undo clipping that
already happened in the codec, so attenuating would only hide the damage from the meters. A
**network** channel has no ADC and takes −60…0 dB, which is what gives a mixer on the sending
machine a playback volume worth the name; it never amplifies, since a digital stream at unity
already is full scale. Each input reports its own range as `gain_min_db`…`gain_max_db` in `GET
/api/state`.

A sender can put its stream beyond both (`mixer off`, below). That input reports `"bypass": true`
and plays exactly as it arrived, gain and mute ignored, until the stream ends; a stored gain is
kept and applies again afterwards.

## Routing and outputs

### `PUT /api/outputs/{0-7}`
The engine card's outputs; 404 on a board without one.
```json
{"source": {"type": "input", "index": 3}, "gain_db": -6.0, "mute": false}
{"source": {"type": "gen", "index": "ping"}}
{"source": {"type": "silence"}}
```
`type` is `silence` | `input` | `gen`. `index` is any input for `input` — a network channel routes
like any other — or `sine` | `noise` | `ping` | `music` for `gen`; a `source` without `type` is
silence. `gain_db` clamps to −60…0.

### `POST /api/outputs/{0-7}/identify`
Three 100 ms beeps on that output only, then it reverts. Tells you which physical socket it is.

### `PUT /api/channel-map`
```json
{"input_map": [0,1,2,3,4,5], "output_map": [0,1,2,3,4,5,6,7]}
```
`input_map[logical]` = the TDM slot to capture from; `output_map[logical]` = the slot to play into.
This corrects the Octo's slot rotation. Both maps are required: `input_map` is 6 distinct slots of
the 8 (0–7), `output_map` a permutation of 0–7; anything else is a 400. 404 on a board without an
engine card.

## Sinks

Every playback device other than the engine card: HDMI, a Pi's 3.5 mm jack, a USB interface. The
daemon asks ALSA for its devices at startup and every 2 s after, so one plugged in later appears on
its own; nothing about a board's devices is compiled in. Each sink plays the same sources at the
same sample index as the engine card's outputs, rendered by the same code, so a ping routed to it
is logged and measured like any other. A sink runs on its device's clock rather than the engine's;
a sample-rate converter follows the drift and holds the path's latency constant.

A sink is named by its **device id**, `<card id>,<device>` as ALSA names them (`b1,0`, `Headphones,0`,
`G12BKHADASVIM3L,0`, `Device,0` for a USB class device). Card ids do not change with probe order or
USB port, so the id, the routing saved under it and the sink's place in `sinks` stay the same
across reboots and replugs. A device that is unplugged keeps its sink (`present: false`), and plays
again with the same routing when it comes back.

One sink is not a hardware device: `bluetooth`, the Bluetooth output (*Bluetooth*), on bluez-alsa's
playback PCM. It is there whenever the board has Bluetooth (`limits.bluetooth`), is stereo, and
picks its speaker through `PUT /api/bluetooth/output`.

### `GET /api/sinks` · `GET /api/sinks/{id}`
```json
{"id": "b1,0", "label": "HDMI", "hdmi": true, "usb": false, "present": true,
 "layouts": ["mono", "stereo", "5.1", "7.1"], "rates": [32000, 44100, 48000, 96000],
 "enabled": true, "open": true, "playing": true, "device": "hw:CARD=b1,DEV=0",
 "sample_rate": 48000, "device_rate": 48000, "layout": "stereo", "speakers": 2,
 "device_channels": 2, "format": "S16_LE", "period_frames": 1024, "buffer_frames": 8192,
 "latency_ms": 112.0, "target_ms": 112.0,
 "ring_ms": 27.4, "alsa_ms": 84.6, "trim_ppm": -1.2,
 "xruns": 0, "underruns": 0, "overruns": 0, "resyncs": 0, "error": "",
 "outputs": [{"ch": 0, "position": "L", "slot": 0, "source": {"type": "gen", "index": "music"},
              "gain_db": 0, "mute": false, "name": ""}]}
```
`latency_ms` is engine to driver, averaged, and it is held at `target_ms`. It leaves out the
driver's own pipeline and the TV, receiver or interface. Both are constant, so measure them once:
route the ping generator (variant `tick`) to the sink, bring it back into an input (an HDMI audio
extractor, a loopback cable), and run `genie/sync` against a reference. Keep the ping interval at 1
s or more, so each window holds exactly one arrival. `resyncs` counts re-anchors; each is a latency
step. `error` says why a device will not open. `outputs` lists the layout's channels, each with the
PCM `slot` it is sent in. `format` is what the PCM opened in: the first of S32_LE, S24_LE, S24_3LE
and S16_LE it takes, the widest first. `period_frames` and `buffer_frames` are the device's, in its
own frames.

### `PUT /api/sinks/{id}`
```json
{"enabled": true, "sample_rate": 48000, "layout": "5.1"}
```
`layout` is one of the sink's `layouts`. An **HDMI** sink (a driver that names itself HDMI, or one
board.json marks) offers `mono` | `stereo` | `5.1` | `7.1`, in HDMI's own slot order (CEA-861: FL FR
LFE FC …). HDMI drivers tell the sink only a channel count, and the sink picks the speakers from it;
only these four counts map to one layout each. Four channels, for example, could be quad or 3.1, so
it isn't offered. Mono is sent on both L and R of a stereo stream (`device_channels` 2). 5.1 and 7.1
need a `sample_rate` of 48000 or less: the Pi carries more than two HDMI channels only up to 48 kHz.
Any other sink offers `stereo` and its own channel count (`6ch`, `8ch`), in the device's own order;
a mono device offers only `1ch`.
`sample_rate` is one of the sink's `rates`: those of 32000, 44100, 48000, 88200, 96000, 176400 and
192000 the device accepts. Use 48000 unless you know the sink takes more: an HDMI driver accepts any
rate whether or not the TV can play it. A new rate or layout restarts that sink only; the engine is
never touched. Sinks are off until switched on here, and a save keeps them on. Answers with the
sink, as `GET /api/sinks/{id}` gives it.

### `PUT /api/sinks/{id}/{ch}` · `POST /api/sinks/{id}/{ch}/identify`
Exactly as `PUT /api/outputs/{0-7}` and its identify, indexed by channel: on HDMI by **speaker**,
0 L, 1 R, 2 C, 3 LFE, 4 Ls, 5 Rs (surround; the side pair in 7.1), 6 Lb, 7 Rb (back, 7.1 only),
mono playing speaker 0; elsewhere the device's channels. All eight keep their routing whatever the
layout, so L stays L across layouts.

## Device inputs

Every capture device other than the engine card: a USB interface's inputs, the VIM3L's HDMI
loopback. Found like the sinks, each takes two adjacent device-input columns (one if it is mono, more
if it cannot do two) and keeps them for the life of the daemon, so an input unplugged and plugged
back in comes back on the same inputs. It is captured on its own clock, and a sample-rate converter
places every frame at the sample index of the instant it was captured, so a sound heard by the
engine card and by a USB microphone lands on the same index in both, to about a millisecond and a
constant the device adds (its own ADC and USB latency: calibrate it once).

For that, the capture axis is held back while any device input is bound: `net.delay_ms`, but at
least 150 ms. Every input in the ring is delayed alike, so nothing measured between two inputs
changes, and the ping log accounts for it. A device input routed to an output is heard that much
later too; it has no undelayed copy to pass through.

### `GET /api/sources`
```json
[{"id": "Device,0", "label": "USB Advanced Audio Device", "first": 8, "channels": 2,
  "present": true, "open": true, "capturing": true, "device": "hw:CARD=Device,DEV=0",
  "device_rate": 48000, "format": "S16_LE", "trim_ppm": 13.0,
  "xruns": 0, "resyncs": 0, "late": 0, "error": ""}]
```
`first` is its first input index. `late` counts blocks that arrived after the ring had read past
them (their audio is lost, not moved). Also in `GET /api/state` as `sources`.

### `GET /api/devices`
Every PCM device ALSA has, as `aplay -l` and `arecord -l` list them, and what the daemon does with it:
```json
[{"id": "Device,0", "alsa": "hw:CARD=Device,DEV=0", "card": "Device",
  "card_name": "USB Advanced Audio Device", "name": "USB Audio", "label": "USB Advanced Audio Device",
  "playback": true, "capture": true, "usb": true, "engine": false, "hidden": false,
  "sink": 1, "input": 8, "input_channels": 2, "note": ""}]
```
`engine` marks the engine card's own; `hidden` one board.json says the board wires to nothing.
Where board.json has `"scan": false` (a PC, whose cards PipeWire runs) the list holds only the
devices the daemon was named (`--sink`, `--input`: `pipewire`, say), each under its name as its id
(a hardware one under its device id).
`sink` is the sink slot its playback is on and `input` the first input its capture is on, -1 for
none; `note` says why one it could be is not (`no free sink slot`, `no free input columns`).

## Bluetooth

The Pi's own Bluetooth radio, in both A2DP roles. A speaker or headphones paired here become a
stereo **output**, a sink like any other. A phone or laptop that plays to the device
becomes a stereo **input** on two adjacent network channels (see *Network inputs*). BlueZ runs the
radio, and [bluez-alsa](https://github.com/arkq/bluez-alsa) carries the audio (SBC). The device
manages pairing itself, so nothing needs a shell. It accepts every pairing and every connection,
like a speaker with no screen, and trusts every device it has paired.

Both directions run on a clock the card does not share. The output is the sink `bluetooth`
(*Sinks*), whose converter holds its latency constant like any sink's. The input's converter holds
it the alignment delay ahead of playout, like a network sender's. Pairings are written to
`/data/bluetooth` whenever they change, so they survive a reboot without a `config/save`.

On a PC (`make pc BT=hci1`) it runs the one adapter it is given (`--bt-adapter`, an hci name or an
address) and leaves the others to the desktop: devices on them are not listed, and the pairing
agent refuses what they ask. Pairings stay where the PC's own bluetoothd keeps them.

### `GET /api/bluetooth`
```json
{"available": true, "error": "", "audio": true,
 "adapter": {"address": "B8:27:EB:50:7B:22", "name": "soundtester", "powered": true,
             "discoverable": false, "discoverable_timeout_s": 180, "pairable": true,
             "discovering": false},
 "settings": {"enabled": true, "name": "", "pairable": true, "discoverable_timeout_s": 180},
 "devices": [{"address": "5C:E9:1E:22:40:01", "name": "Pixel 7", "icon": "phone",
              "paired": true, "connected": true, "rssi": null,
              "sink": false, "source": true, "busy": "", "error": "", "playback": null,
              "capture": {"codec": "SBC", "rate": 44100, "channels": 2, "delay_ms": 15.0},
              "volume": 96,
              "player": {"status": "playing", "title": "Song", "artist": "Band", "album": "",
                         "duration_ms": 215000, "position_ms": 41200, "local": false}}],
 "request": null,
 "input": {"enabled": false, "state": "off", "address": "", "name": "", "rate": 0,
           "channels": 0, "input": -1, "frames": 0, "overruns": 0, "restarts": 0, "error": ""},
 "output": {"enabled": false, "device": "bluealsa:DEV=00:00:00:00:00:00,PROFILE=a2dp", "...": "…"}}
```
`available` is false when there is no adapter (or not the one `--bt-adapter` names) or BlueZ is
not running, and `error` says why.
`audio` is false while bluez-alsa is not running. Pairing and scanning still work without it.
`name` is empty in `settings` when the adapter takes the hostname.
`sink` means the device can take audio (a speaker), `source` that it can send audio (a phone).
`playback` and `capture` are the audio links bluez-alsa has open with it, or `null`.
`delay_ms` is the codec and radio delay the device reports, which the output's `latency_ms` leaves out.
`busy` is `pairing`, `connecting` or `disconnecting` while a request runs, and `error` holds the
last one's failure. `rssi` is reported only while scanning.
`player` is the device's AVRCP media player, present while a phone plays to this one, or `null`.
`position_ms` is carried forward between BlueZ's reports while it plays. On a speaker this device
plays to, `player` is this device's own player instead (`"local": true`, below). `volume` is the audio
link's AVRCP absolute volume, 0–127, or `null` when the device has none.
`request` is a pairing question waiting for an answer, described below.
`input.state` is one of `off`, `waiting` (no device is streaming), `streaming` or `error`.
`input.input` is the first of the input's two channel indexes, or -1. `input.frames` counts what
it has read, `overruns` the times bluez-alsa's buffer filled before it was read, and `restarts` the
times the input re-anchored (a phone that paused, or a new device).
`output` is `GET /api/sinks/bluetooth` plus the fields below, or `null` while Bluetooth is not running.

### `PUT /api/bluetooth`
```json
{"enabled": true, "name": "bench-tester", "pairable": true, "discoverable": true,
 "discoverable_timeout_s": 180}
```
`enabled` powers the radio. `discoverable` makes the device visible in a phone's scan for
`discoverable_timeout_s` seconds (0–3600, 0 means until turned off). `pairable` lets a device that
finds it pair. `name` is at most 248 bytes (400 otherwise); empty means the hostname.
`discoverable` always starts off; the rest are kept by `config/save`. Answers with the `GET` body.

### `POST /api/bluetooth/scan`
`{"on": true}` looks for devices (BR/EDR only, since A2DP needs it) and stops by itself after 60 s.
Scanning shares the radio with any audio link, so expect dropouts on one while it runs.

### `POST /api/bluetooth/devices/{address}/pair` · `/connect` · `/disconnect`
Each starts the operation and answers `{"ok": true}` straight away; watch the device's `busy` and
`error` fields for the outcome. Pairing a device also connects it. `pair` takes an optional
`{"pin": "1234"}` (at most 16 characters) for a device old enough to need one, and uses `0000` by
default. An address that is malformed or not known answers 404, and 503 means Bluetooth is not
running.

### `POST /api/bluetooth/devices/{address}/player`
`{"command": "play"}`, or `pause`, `stop`, `next` or `previous`: the remote control for a phone
that is playing to this device, and on a speaker this device plays to, the tester's own player
(below). 409 when the device has no media player, the command is unknown, or Bluetooth is not
running.

**The tester's own player.** A speaker this device plays to is offered a media player over AVRCP,
so a speaker's buttons, and the same buttons on its card, work the Bluetooth output. The track's
title is the output's signal (`Silence`, `Sine`, `Noise`, `Ping`, `Music`, or an input such as
`IN 3`), the artist is `Sound Tester`, and the album holds the signal's settings
(`440 Hz · -20 dBFS`). The status is `stopped` while the output is off, `paused` while both its
channels are muted, `playing` otherwise. `play` unmutes both channels, switching the output on if
it is off; `pause` mutes them; `stop` switches the output off; `next` and `previous` step both
channels through Silence, Sine, Noise, Ping and Music. The position counts the time the current
signal has been playing.

### `PUT /api/bluetooth/devices/{address}` · `DELETE /api/bluetooth/devices/{address}`
`PUT` takes `{"volume": 0..127}` (clamped): the AVRCP absolute volume of the device's audio link, on
a phone or a speaker. 409 when it has none; a failure after that shows in the device's `error`.
`DELETE` forgets the device: it unpairs it and removes its keys from `/data` too.

### Pairing requests · `POST /api/bluetooth/request`
Everything is accepted without asking, with one exception, which only keyboards raise. It appears
as `request` in `GET /api/bluetooth` and in the 1 Hz WS `system` message (`bt_request`):
```json
{"id": 7, "kind": "passkey", "address": "C7:3B:52:10:9A:1E", "name": "MX Keys",
 "passkey": "", "expires_s": 54}
```
`passkey` asks for the six digits the device shows: answer `{"id": 7, "accept": true,
"passkey": 123456}` (a number or a string). `display` shows a `passkey` to type on the device, and
clears itself. 409 for an `id` that is no longer waiting, a passkey that is not six digits, or
Bluetooth not running. In the WS message `bt_request` is `null` while nothing is waiting.

### `PUT /api/bluetooth/input`
`{"enabled": true}` takes the audio of the most recently connected device that streams to this
one. It lands on two adjacent network channels, named after the device (`transport: "bluetooth"`
in `GET /api/net`). Enabling it turns on the alignment delay, exactly as enabling network input
does. Answers with the `input` object of `GET /api/bluetooth`; enabling it while Bluetooth is not
running is a 503. `config/save` keeps it.

### `PUT /api/bluetooth/output`
```json
{"device": "bluealsa:DEV=F8:DF:15:0A:11:3C,PROFILE=a2dp", "enabled": true}
```
Which speaker the sink `bluetooth` plays to, and optionally switches it on or off in the same call.
Routing, rate and Identify are the sink's own (`/api/sinks/bluetooth`). The default device,
`DEV=00:00:00:00:00:00`, means the most recently connected speaker. The answer is the sink's status
plus `address` and `name` for the speaker playing, and `device_delay_ms`, the delay it reports.
The speaker sets the rate, which `device_rate` reports, so `sample_rate` is only a request.
`latency_ms` runs from the engine to the Bluetooth stack and leaves out the codec and the radio.
Those are what the speaker reports, and they are not constant, so calibrate with a ping before
trusting an absolute delay. Off by default. Both fields are optional; `device` must be a bluez-alsa
PCM (`bluealsa:…`, 400 otherwise) and is kept by `config/save`. 503 when there is no Bluetooth sink
(Bluetooth is not running).

## Generators

Generator timing comes from the same sample counter that indexes the capture ring, so generated and
captured audio line up to the sample.

### `PUT /api/generators/sine`
```json
{"freq_hz": 996.09375, "level_db": -20.0}
```
996.09375 Hz is bin-centred for the 8192-point FFT, so THD+N reads clean with no leakage.

### `PUT /api/generators/noise`
```json
{"mode": "white", "level_db": -20.0}
```
`mode` is `white` | `pink`.

### `PUT /api/generators/ping`
```json
{"variant": "tick", "interval_s": 2.0, "level_db": -20.0}
```
`variant` is `tick` | `bing` | `bong`. Use `tick` to measure delay — it is broadband, so the
correlation peak is sharp. Any change reschedules the next ping from now.

### `PUT /api/generators/music`
```json
{"level_db": -20.0}
```
A 12.8 s loop of "Ode to Joy" with a bass line, for listening rather than measuring: route it to
check a speaker, a TV or an amplifier by ear. It is computed from the sample counter, so every
output that plays it plays the same bar at the same moment. It is only rendered while routed.

### `GET /api/pings/recent`
```json
[{"sample": 1466240, "variant": "tick"}]
```
The emission sample of the last 64 pings. Use it to bracket one ping for a delay measurement.

## Capture, scope and delay

Capture and playback share one clock, so a sample index is the same instant on every channel.
`start`/`len` everywhere here are absolute indices on that counter.

### `POST /api/capture/freeze` · `POST /api/capture/resume` · `GET /api/capture/status`
Freeze copies the recent ring into a snapshot so measurements cannot shift while you work.
```json
{"frozen": true, "freeze_sample": 2897920, "valid_start": 1857536, "valid_len": 1040384, "generation": 0}
```
503 when there is not enough captured audio to freeze yet; resume answers `{"frozen": false}`.
Ask only for samples in `[valid_start, valid_start + valid_len)`. `generation` bumps on every xrun —
if it changed, the timeline has a gap. `status` also returns `live_now` (write head), `live_oldest`
(oldest readable live sample), `analyze_frames` / `analyze_seconds` (what the next freeze copies,
see `capture/config`) and `ring_seconds` (how far back the live ring reaches once it has filled):
```json
{"frozen": false, "freeze_sample": 0, "valid_start": 0, "valid_len": 0, "generation": 0,
 "live_now": 21910528, "live_oldest": 13523968, "analyze_frames": 960000, "analyze_seconds": 10.0,
 "ring_seconds": 87.36}
```

### `POST /api/capture/config`
```json
{"seconds": 20.0}
```
How much the next freeze copies. `{"seconds": N}` or `{"frames": N}`, clamped to
[4096, `limits.capture_max_frames`]. The reply says what took effect: `analyze_frames`,
`analyze_seconds`, `max_frames`, `max_seconds`, `pinned_mb`. 507 when the snapshot cannot be
allocated that large. Resets to 10 s on restart.

### `GET /api/capture/window?ch=&start=&len=&cols=`
The scope. `ch` is required; `len` defaults to 96000, `start` to the last `len` frames and `cols` to
1024. One channel per call: a `ch` list or a repeated `ch` is a 400. Answers
`{"ch", "start", "len", "cols", "raw"}` plus `min[]` and `max[]`, one pair per column, or
`samples[]` when `raw` (`len ≤ 2×cols`). So raw audio comes at most 4096 samples per call
(`cols=2048&len=4096`). Serves the frozen snapshot if frozen, else the live ring.

Every param is a number ≥ 0 (a fraction is truncated); anything else is a 400, and so is a value out of range, with the
range in the error: `ch` 0…`limits.inputs_total`−1, `len` 1…the ring, `cols` 1–2048 (refused, not
clamped), and a span outside what can be read:
`{"error": "[5, 105) is outside the live ring [13530112, 21916672)"}` (or `outside the frozen range
[…)` while frozen).

For more than a screenful of raw samples use `GET /api/capture/wav` below: one call instead of a
loop of 4096-sample windows.

### `GET /api/capture/wav?[<selector>&](start=&len= | seconds=)[&format=]`
Raw multichannel audio in one call: an interleaved WAV of the selected inputs, in the order selected.
```sh
# the DUT's stereo stream, the last 12 s, 16-bit
curl -s -m 30 -D h.txt -o cap.wav "$B/api/capture/wav?device=heston&seconds=12&format=s16"
grep -i '^x-st-' h.txt
```
```
X-ST-Start-Sample: 5739008
X-ST-Len: 1152000
X-ST-Rate: 96000
X-ST-Generation: 1
X-ST-Unix-Ms: 1763560000123
X-ST-Channels: 6,7
X-ST-Source: live
X-ST-Format: s16
```
- **Inputs** — the genie selector (*Choosing inputs* below): `ch=0,1,2` (that order is the file's
  channel order: `ch=2,0` puts IN 3 first), `device=`/`stream=`/`stream_index=`/`kind=`; none = every
  input. A sender is resolved **now**, so on a frozen snapshot it names where the sender is now, not
  where it was at the freeze.
- **The span** — exactly one of:
  - `start=&len=` — absolute sample indices (frames). On the **frozen snapshot** while frozen
    (`[valid_start, valid_start + valid_len)`), else on the **live ring** (`[live_oldest, live_now)`
    of `GET /api/capture/status`, less its oldest **2 s** once the ring has filled: the writer would
    overwrite those while the body is built, so a span starting there is a 400 naming the range that
    can be read).
  - `seconds=N` — the last N s of the **live ring**, frozen or not (fractions too): at most the
    ring's length less those 2 s.
- `format` — `f32` (default; IEEE float, the ring's values bit for bit, full scale ±1.0), `s32` or
  `s16` (integer PCM, rounded and clipped at full scale; an Octo input's 24-bit codes come back exact
  in `s32`). Plain `fmt` (tag 3 for float, 1 for PCM; no WAVE_FORMAT_EXTENSIBLE) that libsndfile,
  scipy, sox and ffmpeg read at any channel count (Python's `wave` module reads `s16`/`s32` only).

The body is the whole file with its `Content-Length`. The headers place it on the tester's axis:
`X-ST-Start-Sample` (the first frame's sample index), `X-ST-Len` (frames), `X-ST-Rate` (Hz),
`X-ST-Generation` (the snapshot's `generation` when frozen, else the engine's now: compare it with an
earlier reading, as for `capture/freeze`), `X-ST-Unix-Ms` (the wall clock at the first frame, to line
it up with a DUT's log), `X-ST-Channels` (the ring columns, in file order), `X-ST-Source` (`frozen` or
`live`) and `X-ST-Format`. The same facts, with each input's `label`, are in the file's `LIST/INFO`
comment (`ffprobe cap.wav` shows it), so a saved capture keeps them. As everywhere on the ring, a
network or device input's audio was heard `capture_delay_ms` before its sample index.

**Size:** at most **256 MB** of samples per call (`len × channels × 4`, or × 2 for `s16`): ~50 s of 14
inputs in `f32` at 96 kHz; a stereo `s16` sender's whole ring (less 2 s). The body is
built in memory before it is sent, from post-validated reads of the ring or snapshot, so a file is
never stitched from two laps of the ring. **Latency:** building 200 MB takes ~0.15 s on a desktop
and several times that on a Pi 3; then the transfer: a 96 kHz `f32` channel is 384 kB per second of
audio, so 12 s of 2 channels is 9 MB. Use `curl -m 30` (longer for a big body over Wi-Fi). One body
is built at a time; a second request waits for it (up to 10 s).

**Errors:** **400** with the valid range for a span outside it
(`[5, 105) is outside the frozen range [1719808, 2679808)`,
`seconds must be at most 34.480 (what the live ring holds now, less 2.0 s kept clear of its oldest
end)`), for both or neither of
`start+len`/`seconds`, `start` without `len`, a bad `format` or number, a malformed selector, and for
a body over the cap (`5280000 frames is 282 MB: at most 256 MB, i.e. 4793490 frames (49.932 s) of these
channels in this format …`); **404** for a sender that is not connected (as in *Choosing inputs*);
**410** when the span was overwritten while it was read (should not happen past the 2 s margin)
or the snapshot was resumed or replaced meanwhile — ask again; **429** when another export
held the build for over 10 s; **507** when the board has not the free memory for the body.

### `POST /api/capture/xcorr`
```json
{"ch_a": 0, "ch_b": 1, "start": 386560, "len": 16384}
```
→
```json
{"lag_samples": 137, "lag_ms": 1.4271, "lag_m": 0.4895, "confidence": 4.2, "peak": 0.99}
```
Cross-correlates two inputs over a window. **Freeze first**, or it is a 400; `len` is 64…2^19, and a
window in which either channel is silent is a 400 too. A **positive lag means
the signal arrives later on `ch_b`**. `lag_m` is the acoustic distance — meaningful for an air path,
not a cable.

Check `confidence` before trusting `lag_samples`: it is the winning peak over the tallest separate
rival. Above 3, trust it. Below 2 it is ambiguous — either more than one ping is in the window
(bracket a single ping) or you used a continuous tone (its delay is only known modulo the carrier —
use a ping). That holds for any steady periodic tone, a square wave or a clipped sine as much as a
sine (the test counts the whole harmonic series), and for a tone so low that the window holds only
a few of its periods (confidence 1). A ringing ping (bing, bong) bracketed with its start inside the
window reads confident; a window that opens partway through its ring-out has no onset to pin the
cycle and reads ambiguous — move the window's start before the ping.

## Genie helpers

Shortcuts that turn the primitives above into a single answer, for scripts and agents. Which one
answers which question:

| Question | Endpoint | Answers |
|---|---|---|
| Is there sound on an input, is it playing now, is it the tone I expect? | `GET /api/genie/sound` | at once (a 100 ms reading) |
| Tell me when it starts, stops, goes live, or changes (instead of sleep-and-poll) | `GET /api/genie/wait` | when the condition holds, or at `timeout_s` |
| Did it drop out, click, step or slip over the last (or next) N seconds? | `GET /api/genie/watch` | at once (`from=past`) or after N s (`from=now`) |
| What is the delay between two inputs (L/R skew, speaker vs speaker, a path's latency)? | `GET /api/genie/sync` | 0.5–3 s on a Pi |
| Give me the raw samples of these inputs over this span | `GET /api/capture/wav` (above) | at once, one WAV |

They all name inputs the same way (*Choosing inputs*, next), echo who each input is (`label`, `name`,
`kind`, `net`) so a reply can be reported as "NET 3", not as a column, and place what they read on the
tester's sample axis (`sample`, `start_sample`, sync's `snapshot`) — sound, wait and watch also on the
wall clock (`unix_ms`, `unix_ms_start`; capture/wav in `X-ST-Unix-Ms`), to line up with a DUT's log.
Every one is a GET, so a browser's address bar runs it too; in the examples `B=http://soundtester`.
Errors are JSON `{"error": "…"}` with a 4xx/5xx status; "nothing found" is a 200 with a verdict
(`met: false`, `ok: false`) and a `reason`, so check that field before using the numbers.

### Choosing inputs
Every genie endpoint (and `GET /api/spectrum`) takes the same input selector. All of it is optional;
with none, every input is returned.
- `ch=4,5` — inputs by index, in that order. A comma list and repeated `ch=4&ch=5` both work.
- `device=<prefix>` — a **connected** network sender whose `device`, `name`, `host`, `last_device`
  or `mac` starts with this, ignoring case (`device=heston` finds `heston120-501e2d676be9`).
- `stream=<name>` — a connected sender's stream (the plugin's `name`, e.g. `main`), exact. With
  `device=` too, both must match.
- `stream_index=N` — with `device`/`stream`, only member N (1-based) of each matched run: `1` is the
  left of a stereo sender, `2` the right.
- `kind=local|net|device` — only inputs of that kind (keeps idle analog inputs out of a NET hunt).
  With `ch`, it filters the list.

A sender is resolved on every request, so it is found wherever it reconnected to: never hard-code the
channels a sender had. When a sender has more than one matching run (a stale run left behind by a
reconnect, or `main` and `notif` of one host), only the runs that are `streaming` count if any is;
otherwise every connected one. All of them are returned, grouped run by run. The reply's `resolved`
says what the selector found:
```json
"resolved": {"inputs": [20, 21], "device": "heston",
             "runs": [{"inputs": [20, 21], "channel": 14, "device": "heston120-501e2d676be9",
                       "stream": "pipewire", "stream_count": 2, "state": "streaming"}]}
```
(`channel` is the run's first NET channel, as in `GET /api/net`; without `device`/`stream` it is
just `{"inputs": […]}`, plus `kind` when given.)

**400** for a malformed selector: a `ch` that is not a number or out of range, an empty `device`,
`ch` together with `device`/`stream`, `stream_index` without them, or `kind` other than `net` with
them; and, on every endpoint that takes it, for a selector that matches no inputs (`ch=6,7&kind=local`
when 6 and 7 are network inputs, `kind=device` on a board without device inputs): `the selector
matches no inputs on this board`. **404** when no connected sender matches, listing the channels a matching sender was last on:
```json
{"error": "no connected sender matches device 'heston120'", "matched": 0,
 "last_seen": [{"input": 20, "channel": 14, "last_device": "heston120-501e2d676be9",
                "last_stream": "pipewire", "state": "disconnected"}]}
```
also when the matched runs have no member `stream_index`.

### `GET /api/genie/sound[?<selector>&threshold_db=&min_rms_db=&expect_hz=&tol_hz=]`
Is there sound on an input, and is it playing now?
```json
{"sample": 1488896, "unix_ms": 1763560000123, "threshold_db": -60.0, "min_rms_db": -60.0,
 "resolved": {"inputs": [20, 21], "device": "heston", "runs": [ … ]},
 "channels": [{"ch": 20, "label": "NET 15", "name": "pipewire · heston120-501e2d676be9 (1/2)",
               "kind": "net",
               "net": {"channel": 14, "device": "heston120-501e2d676be9",
                       "last_device": "heston120-501e2d676be9", "stream": "pipewire",
                       "stream_index": 1, "stream_count": 2, "state": "streaming"},
               "sound": true, "sound_now": true, "playing": true, "last_sound_ms_ago": 0,
               "rms_db": -20.1, "peak_db": -18.0, "peak_now_db": -18.4, "threshold_db": -60.0,
               "tone": {"valid": true, "freq_hz": 996.09, "thd_n_pct": 0.0032,
                        "peak_hz": 996.09, "peak_db": -21.0, "reason": null}}]}
```
Inputs by the selector above. Per channel:
- `label`, `name`, `kind` — as in `GET /api/state` `inputs[]` (report an input by its `label`,
  "NET 15", not by `ch`). `net` is null except on a network input: who is on it (`device` is empty
  once the sender has gone; `last_device` keeps it) and the link `state` (`streaming` while audio is
  arriving, `connected` with the sender silent on the wire or stalled, `disconnected`, or `""` never
  used).
- `sound` — `peak_db > threshold_db`. `peak_db` is a **3 s hold**: the largest sample of the last
  3 s, each peak held exactly 3 s after the sample it was at (a louder one re-arms it; a softer one
  during a louder one's hold is still held its own 3 s), and every sample is metered, so a 1 ms tick
  or ping between polls counts. It stays true for 3 s after the last sound above `threshold_db`
  (to the next 100 ms reading: 3.0–3.1 s), with `rms_db` at −120.
- `sound_now` — the latest 100 ms window's own peak (`peak_now_db`, no hold) is above `threshold_db`.
- `playing` — `sound_now` and `rms_db > min_rms_db`: **the verdict for "is it playing now"**. An idle
  analog input's noise floor (peaks around −55 dB, RMS −70…−82) fails it, and so does a stopped
  input's 3 s hold.
- `last_sound_ms_ago` — how long ago a reading (its 100 ms window, or every sample since the last
  reading if that was longer) last peaked above −60 dBFS (fixed, not `threshold_db`), measured to
  `sample` (to the 100 ms reading); 0 while it does, null if never since the daemon started.
- `rms_db` (100 ms window), `peak_db` (3 s hold, as above), `peak_now_db` (100 ms, no hold); all post
  input-gain, silence at −120. `threshold_db` echoes the top-level one.
- `tone` — the tone detector, as in `GET /api/state` and `GET /api/spectrum`: an 8192-sample FFT
  refreshed at 5 Hz (so for ~200 ms after a start or stop it can disagree with `rms_db`).
  `peak_hz`/`peak_db` are always the dominant partial in 20 Hz–20 kHz. `valid` needs that partial
  above **−80 dBFS** and everything else in band (THD+N) at most **30 %** of it; then `freq_hz` is
  its frequency, else 0. `reason` says why not: `"silent"` (dominant partial ≤ −80 dBFS,
  `thd_n_pct` 0) or `"not_a_tone"` (THD+N over 30 % — music, speech, noise; `thd_n_pct` kept). It is
  null when `valid`. `thd_n_pct` is **percent**.

Params: `threshold_db` (default −60) for `sound`/`sound_now`; `min_rms_db` (default −60) for
`playing`; `expect_hz=1000` or `expect_hz=1000,1500` — one frequency for every returned channel or
one per channel in their order (a count that fits neither is a 400) — adds `tone.expect_hz` and
`tone.match` (`valid` and within `tol_hz` of it; `tol_hz` defaults to 5 and is echoed top level).
A param that is not a number is a 400. `unix_ms` is the wall clock when the engine was at `sample` (to line the reply up with a DUT's log).
A network input's audio reaches the ring `GET /api/net` `delay_ms` after it was sent, so a sender
that just started reads `playing` that much later.

**400** for a malformed selector or a param that is not a number, an `expect_hz` count that fits
neither one nor one per channel; **404** for a sender that is not connected (as in *Choosing
inputs*). **Latency:** none — it reads the latest analysis snapshot (refreshed every 100 ms) and
answers at once; `curl -m 5` is plenty.

```sh
# is the DUT playing on its sender right now?
curl -s -m 5 "$B/api/genie/sound?device=heston" | jq -c '.channels[] | {label, playing, rms_db}'
# is the 1 kHz test tone on IN 1 and IN 2?
curl -s -m 5 "$B/api/genie/sound?ch=0,1&expect_hz=1000" |
  jq -c '.channels[] | {label, match: .tone.match, freq_hz: .tone.freq_hz, thd_n_pct: .tone.thd_n_pct}'
```

To wait for a sender to start (or stop) playing, use `GET /api/genie/wait` below rather than a
loop of these.

### `GET /api/genie/wait?until=…[&<selector>&hold_ms=&timeout_s=&all=&threshold_db=&min_rms_db=&freq_hz=&tol_hz=]`
Blocks until something happens on an input, then answers: the server-side replacement for a
sleep-and-poll loop. It looks every 100 ms (the analysis refresh) at the same readings as
`genie/sound` and at `GET /api/net`, and answers as soon as the condition has held `hold_ms`
without a break, or when `timeout_s` runs out.
```json
{"met": true, "until": "sound", "after_ms": 2911, "met_sample": 1335168,
 "met_unix_ms": 1763560003235, "onset_ms": 1611, "hold_ms": 300, "timeout_s": 20.0, "all": false,
 "threshold_db": -60.0, "min_rms_db": -60.0,
 "sample": 1363968, "unix_ms": 1763560003535, "capture_delay_ms": 1000.0,
 "resolved": {"inputs": [6, 7], "stream": "main", "runs": [ … ]}, "moved_from": null,
 "channels": [{"ch": 6, "label": "NET 1", "name": "main · heston120 (1/2)", "kind": "net",
               "device": "heston120",
               "net": {"channel": 0, "device": "heston120", "last_device": "heston120",
                       "stream": "main", "stream_index": 1, "stream_count": 2,
                       "state": "streaming", "frames_received": 46041,
                       "frames_per_s": 47980.2, "last_audio_ms_ago": 17},
               "holds": true, "playing": true, "sound_now": true,
               "rms_db": -23.0, "peak_now_db": -20.0,
               "tone": {"valid": true, "freq_hz": 439.92, … }, "changed": null}]}
```
`until` (required) — per input:
- `sound` — `playing`, as in `genie/sound`: the unheld 100 ms peak above `threshold_db` and the RMS
  above `min_rms_db` (an idle analog input's noise floor does not count).
- `silence` — not `sound_now`: the unheld 100 ms peak at or below `threshold_db`. No 3 s hold, so it
  is met right after the sound stops (not 3 s later, as `sound` going false would be).
- `tone` — `tone.valid`, and within `tol_hz` (default 5) of `freq_hz` when given (any valid tone
  without it). `expect_hz=` (the name `genie/sound` and `genie/watch` use) is the same parameter
  here, one frequency; both together is a 400, and so is either, or `tol_hz`, with any other `until`.
  (`freq_hz` on `genie/sound` or `genie/watch` is a 400 naming `expect_hz`.)
- `live` — the sender is `streaming` and its `frames_received` has moved past where it stood at the
  wait's first look at that input: audio is arriving **now**, not just within the last second.
  Network inputs only: without a selector it watches every network input; a `ch` that is not one,
  or `kind` other than `net`, is a 400.
- `change` — `playing` differs from what it was on that input at the start of the wait. The reply's
  `changed` says which inputs flipped: `"on"` (started playing) or `"off"`, null for the rest. Wait
  with no selector, then start or stop the DUT: the input that flips is the one it is wired to.

Met when **any** selected input has held its condition for `hold_ms` (default 300, 0 = the first
look it holds on); `all=1` (or `any=0`) needs every selected input, and at least one. `timeout_s`
defaults to 20 and takes 0…120 (fractions too; 0 looks once). `threshold_db`/`min_rms_db` default to
−60 as in `genie/sound`.

**The selector** (as in *Choosing inputs*) is resolved again on every look, so a wait on
`device=`/`stream=` follows a sender that reconnects onto other channels; `moved_from` then lists the
inputs it started on (null otherwise), and an input new to the wait starts its own baseline and hold
there. A sender that is not connected yet is not an error: the wait waits for it (a sender that never
comes is a timeout with `last_seen`). For `silence` and `change`, a sender that leaves after it was
found (aplay ending, a killed player: it closes its PCM) is still watched on the inputs it was last
on, unless another sender has taken them: its audio drains through the ring and they go silent, so
"wait until the DUT stops" is met. The reply then has `"sender_left": true`, `resolved: null` and
those inputs in `channels`. With no selector, every input is watched (every network input
for `live`) — give one for `sound`/`silence`/`tone`, or an idle input decides it.

The reply: `met`; `after_ms`, **wall time** from the request to the look that met it (or to the
timeout); `sample`/`unix_ms` of the analysis reading it ended on (as in `genie/sound`); and per
selected input its levels then, `holds` (its condition held on that last look, for however long), and
for a network input the `net` block of `genie/sound` plus `frames_received`, `frames_per_s` and
`last_audio_ms_ago` from `GET /api/net`.

**When it happened.** `after_ms` is when the wait *answered*, not when the thing happened: it
**includes `hold_ms`** (the condition had to hold that long first) and, for `sound`, `silence`,
`tone` and `change`, **`capture_delay_ms`** — every input's audio enters the ring that long after
it was heard, the alignment delay that keeps network and device inputs on one axis with the analog
ones (`GET /api/net` `delay_ms` while the network or Bluetooth input is on, at least 150 ms while a
device input is bound, else 0). `live` reads the link, not the ring, so it has no capture delay. For
when the condition **started**:
- `met_sample` / `met_unix_ms` — the analysis reading (its ring `sample`, and that sample's wall
  time as `unix_ms` reckons it) that began the run of looks that met it: the start of the hold. With
  `all=1`, the reading on which the last of the inputs began holding. The reading is the **end** of
  its 100 ms window, so the change itself happened up to 100 ms before it.
- `onset_ms` — from the request to then, in the DUT's time: `met_unix_ms − capture_delay_ms −` the
  request's wall time (for `live`, the look's own time from the request). ≤ 0 when the condition
  already held as the request came in (for `sound` on an input already playing, about
  −`capture_delay_ms`). So "how long after I pressed play did sound arrive" is `onset_ms`, to
  ~100 ms.

All three are null unless `met`. Levels and `tone` are refreshed at 10 Hz and 5 Hz, so `after_ms` is
at most ~100 ms (tone ~200 ms) late on top of `hold_ms` and the capture delay.

**Timeout:** a 200 with `met: false` and a `reason`:
- `"no connected sender matches stream 'main'"` with `last_seen` (as the selector's 404 lists it)
  when the selector found no sender on the last look;
- `"sender not streaming: NET 1 (heston120 connected, last audio 4004 ms ago); NET 2 (…)"` when a
  selected network input's sender is connected but silent on the wire or stalled, `disconnected`, or
  absent (`"NET 3 (no sender)"`) — the link, not the DUT, is why nothing played. Only for `sound`,
  `tone` and `live`, and only when the wait was about senders (`device=`/`stream=`, or nothing but
  network inputs selected): a wait over analog and NET inputs is not blamed on an idle NET input;
- otherwise `"timed out after 20 s: sound did not hold for 300 ms on any selected input"`.

**400** for a missing or unknown `until`, a bad number, a malformed selector, or one that matches no
input on this board (`kind=device` on a board with none). **429** when 8 waits are already pending
(each holds a server thread; retry when one has answered). **503** `"the daemon is shutting down"` to
a wait cut short by a stop. A client that hangs up ends its wait (and frees its slot) within 100 ms.
Give curl at least `timeout_s` + a few seconds: `curl -m 25` for the default.

```sh
# the DUT started playing on its sender (wherever it reconnected to)
curl -s -m 30 "$B/api/genie/wait?device=heston&until=sound&timeout_s=20" | jq '{met, after_ms, reason}'
# a sender that is about to connect is live
curl -s -m 30 "$B/api/genie/wait?stream=main&until=live" | jq '{met, reason, inputs: .resolved.inputs}'
# which analog input is the DUT: start the wait, then start or stop the DUT
curl -s -m 40 "$B/api/genie/wait?kind=local&until=change&timeout_s=30" |
  jq '.channels[] | select(.changed) | {label, changed}'
# the 1 kHz test tone arrived on the left channel
curl -s -m 30 "$B/api/genie/wait?device=atron&stream_index=1&until=tone&freq_hz=1000"
```

### `GET /api/genie/watch[?<selector>&seconds=&from=&block_ms=&silence_db=&click_k=&expect_hz=&resid_dbc=&slip_deg=&level_step_db=]`
What happened on an input over a span of the ring: dropouts, clicks, level steps and, on a test
tone, phase slips and distortion, each at the sample it happened, plus the span's level statistics.
The server-side replacement for polling `genie/sound` once a second or pulling the span out with
`capture/window` for a gap/glitch/sine-fit script.
```json
{"from": "now", "seconds": 10.0, "start_sample": 7384064, "len": 960000, "analysed": 960000,
 "rate": 96000.0, "block_ms": 10.0, "unix_ms_start": 1763560000000, "t_ago_s_start": 10.04,
 "capture_delay_ms": 1000.0, "generation": 1, "generation_changed": false,
 "incomplete": false, "reason": null, "elapsed_ms": 10051, "analysis_ms": 46,
 "params": {"silence_db": -60.0, "click_k": 10.0, "expect_hz": "auto", "resid_dbc": -50.0,
            "slip_deg": 20.0, "level_step_db": 6.0},
 "resolved": {"inputs": [6, 7], "stream": "main", "runs": [ … ]},
 "channels": [{"ch": 6, "label": "NET 1", "name": "main · heston120 (1/2)", "kind": "net",
               "net": {…},
               "sound_pct": 82.9, "silent_s": 1.71,
               "rms_db": {"min": -120.0, "p10": -120.0, "median": -13.47, "max": -13.4},
               "peak_max_db": -10.46, "clipped_samples": 0,
               "counts": {"zero_run": 0, "gap": 1, "onset": 0, "offset": 0, "click": 0,
                          "level_step": 0, "phase_slip": 0, "resid": 0},
               "tone": {"expect_hz": 998.145, "source": "analysis", "blocks": 829, "bad_blocks": 2,
                        "freq_hz_median": 1000.0, "freq_hz_min": 998.004, "freq_hz_max": 1000.0,
                        "thd_n_pct_median": 0.0666, "thd_n_pct_max": 42.1,
                        "resid_dbc_median": -63.5, "resid_dbc_max": -7.5},
               "tone_note": null,
               "link": {"underruns": 10, "underrun_events": 2, "late_drops": 7, "range_drops": 0,
                        "resyncs": 1, "reconnects": 0, "device_changed": false,
                        "state": "streaming", "frames_per_s": 95275.0}}, …],
 "events": [{"kind": "gap", "ch": [6, 7], "labels": ["NET 1", "NET 2"], "sample": 7811998,
             "t_s": 4.4576, "t_ago_s": 5.58, "len_ms": 1720.792, "zeros": true, "click": true}],
 "events_truncated": false}
```
**The span** — `seconds` (default 10) of every selected input (the selector of *Choosing inputs*;
with none, every input, at most 16):
- `from=past` (default) — the last `seconds` of the ring, answered at once. Up to the ring's length
  less 3 s: ~84 s at 96 kHz with 6 network inputs, ~40 s with 16 (a 400 says the limit).
- `from=now` — the next `seconds` from the request on: it blocks until they have been captured
  (at most 60 s), then analyses them. Start it, then do what you are testing (toggle the DUT, kill a
  sender); give curl `seconds` + 10. A client that hangs up ends it.

The span is the ring's, so every input's audio in it was heard `capture_delay_ms` before (as in
`genie/wait`); `start_sample`/`unix_ms_start` place it, and every event carries its own `sample`,
`t_s` (from the span's start) and `t_ago_s` (before the reply). A span the writer overtakes while it
is read (only a very long one on a busy Pi) stops there: `incomplete: true`, `reason`, and
`analysed` (samples) short of `len`, with the figures over what was read.

**Per channel** (`label`/`name`/`kind`/`net` as in `genie/sound`), over 10 ms blocks (`block_ms`,
1–100):
- `sound_pct` — share of the span in blocks whose RMS is at or above `silence_db` (default −60
  dBFS); `silent_s` the rest, in seconds.
- `rms_db` — the blocks' RMS: `min`, `p10`, `median`, `max` (dBFS, silence −120). `peak_max_db` the
  largest sample; `clipped_samples` those at |x| ≥ 0.999.
- `counts` — events of each kind on this channel (all of them, also past the event cap).
- `tone` — **tone mode**, null when off. Each block is least-squares fitted with a sine at
  `expect_hz` (plus DC, and a linear drift of amplitude and phase, so a tone a few Hz off still
  fits). `freq_hz_median/min/max` come from the phase advance between consecutive clean blocks
  (finer than any FFT bin: a network sender's converter trim shows, e.g. 998.0 Hz at its 0.2 % limit
  while it catches up); `thd_n_pct_*` and `resid_dbc_*` are each block's residual against the tone
  (THD+N, percent; and the same in dBc); `blocks` the non-quiet blocks fitted, `bad_blocks` those
  whose residual is above `resid_dbc`. `source` says where `expect_hz` came from.
  `tone_note` says why tone mode is off with `expect_hz=auto`.
- `link` — `from=now` and a network input only: how the sender's counters (`GET /api/net`) moved
  over the request — `underruns` (engine blocks), `underrun_events`, `late_drops`, `range_drops`,
  `resyncs`, `reconnects`, plus `device_changed` (a different sender is on the channel at the end
than at the start; false when the channel is empty at the end — a sender that left reads
`state: disconnected`), the link
  `state` and `frames_per_s` at the end. The counters move one `capture_delay_ms` before the audio
  they affect reaches the ring, so a hole in the last second of the span shows here but not yet as
  an event. Null otherwise.

**Events** — one list for all channels, by sample, at most 200 (`events_truncated` past that; the
`counts` are complete). Each has `kind`, `ch` and `labels` (the inputs it was seen on: the members of
one network sender's run that show the same event within 2 ms are **one** event), `sample`, `t_s`,
`t_ago_s` and `len_ms`. Kinds:
- `gap` — quiet blocks (below `silence_db`) with level (20 dB over `silence_db`) within 30 ms on
  **both** sides, and sustained there (the median block over 0.5 s on each side that loud too): a
  dropout. The pauses of an intermittent signal (the ping/tick generator, a click track) are no
  gaps, and its bursts no onsets. `zeros: true` when it is one run of exact 0.0 samples — a network hole
  (`NetTimeline` plays an unwritten frame as exact zero), a muted input — and then `sample`
  and `len_ms` are exact to the sample; otherwise they are to the block. An analog path (or the
  simulator's noise floor) is never exactly zero, so a muted output reads `zeros: false`.
- `onset` / `offset` — the same abrupt edge on one side only: the sound started (`sample` = its first
  block, `len_ms` 0) or stopped (`len_ms` = how long it stayed quiet, to the span's end or a soft
  return). A **fade** (a track ending) has no abrupt edge and is neither: a track gap is not a
  dropout.
- `zero_run` — at least 1 ms of exact 0.0 inside audio (level within 50 ms both sides) too short to
  make a quiet block: a short network hole. Exact to the sample. Holes inside a `gap` are part of it.
- `click` — a sample whose second difference |x[n−1] − 2x[n] + x[n+1]| exceeds `click_k` (default
  **10**) times the median of its 10 ms block and of the blocks either side, and at least
  `silence_db` in absolute terms; `sample` is the worst sample of a cluster (2 ms), `len_ms` the
  cluster, `level_db` its size. The edge of a gap, a hole, a slip or a step clicks too, and is then
  reported on that event as `click: true`, not apart. At the default, 40 s of white or music-like
  noise reads no click; lower `click_k` (not below ~8) to hunt smaller ones.
- `level_step` — the RMS of two **steady** 100 ms stretches (every block but one within 1.5 dB of
  their median, quiet blocks left out) either side of a point differs by more than `level_step_db`
  (default 6): `from_db`, `to_db`, and `sample` to the block. On a tone or noise that is a volume
  jump; music with steady notes (the music generator) shows its own dynamics here too, so judge
  steps on a test signal.
- `phase_slip` — tone mode: the tone's phase jumped by more than `slip_deg` (default 20°) against its
  steady drift: samples lost or repeated. `slip_samples` (> 0: the tone jumped **ahead**, as if that
  many samples were lost; < 0: it fell behind, samples repeated) and `slip_deg`, modulo one period
  of the tone (±48 samples at 1 kHz / 96 kHz); `sample` is exact (to the block when the span had
  already left the ring for the second look). A slip across a `gap` or `zero_run` (up to 1 s) is put
  on that event as its `slip_samples`/`slip_deg`: the tone came back shifted (none: the hole kept
  its place, as a network hole does). A slip is a lasting move: the clean block before it must agree
  with the one before that, and the clean block after it with the one after that. So one block
  knocked off-phase by a disturbance (a burst at the tone's own frequency on one channel) that is
  back in place on the next is no slip, and nor is anything in the span's **last or first block or
  two**, where there is no neighbour to confirm it — a span that ends right after a disturbance does
  not invent a slip there; `from=now` a little longer (or a later `from=past`) sees it. A span that
  is not a whole number of blocks ends in a partial block, which counts for levels and clicks, for
  the tone's residual (`resid`, `bad_blocks`) only when long enough to fit, and never for its phase.
- `resid` — tone mode: blocks in a row whose residual is above `resid_dbc` (default −50 dBc) that no
  other event explains (distortion, an interferer, a wrong `expect_hz`); `resid_dbc` the worst.

**`expect_hz`** — `auto` (default): per input, the tone detector's `freq_hz` (as in `genie/sound`)
when it reads a valid tone at the span's end, else tone mode off (`tone_note` says why). `none`:
off. `1000` or `1000,1500`: one frequency for every input or one per input in order; each needs a
whole cycle per block (≥ 100 Hz at 10 ms; raise `block_ms` for a lower tone) and to be below
Nyquist. `resid_dbc` (−200…0), `slip_deg` (0…180) and `level_step_db` (0…100) tune the detectors.

`generation` is the engine's discontinuity counter at the end; `generation_changed` whether the
span holds a seam — an xrun or a capture-axis move (the network delay changed, a device input bound)
puts one in the ring, and it can read as a gap or a slip. `from=now`: the counter moved during the
span. `from=past`: the engine keeps the ring sample of its latest move, and the span holds a seam when
that is inside it (every earlier move is earlier still).

**400** for a malformed selector or param, `seconds` over the limit (the message says it), a ring
that does not hold `seconds` yet, an `expect_hz` count that fits neither one nor one per input, or
more than 16 inputs; **404** for a sender not connected (as in *Choosing inputs*). **429** when 2
watches are already running (each takes a server thread and, while it analyses, a core). **503**
`"the daemon is shutting down"` for a `from=now` cut short by a stop, and `"the engine is not
advancing: …"` for one whose span has not been captured 5 s after it should have been (a stalled
or unopenable card: see `engine.last_error` in `/api/state`). **Latency** (on top of the
wait for `from=now`): the analysis runs ~1000× real time per input on a desktop (84 s of 14 inputs
in 0.8 s; `analysis_ms` reports it) and should be 10–20× slower on a Pi 3 — 10 s of 2 inputs well
under a second, the whole ring of every input ~10 s. Use `curl -m 30` for `from=past`.

```sh
# did the DUT's stream drop out in the last 30 s?
curl -s -m 30 "$B/api/genie/watch?device=heston&seconds=30" |
  jq '{counts: [.channels[] | {label, counts}], events: [.events[] | {kind, labels, t_ago_s, len_ms}]}'
# soak a 1 kHz test tone for the next 60 s: holes, slips, THD+N
curl -s -m 75 "$B/api/genie/watch?stream=main&from=now&seconds=60&expect_hz=1000" |
  jq '.channels[] | {label, counts, tone: .tone | {freq_hz_median, thd_n_pct_max, bad_blocks}, link}'
```

### `GET /api/genie/sync[?<pair>&mode=&since=&win=&step=&min_confidence=&cur_x=&cur_y=]`
Delay between two inputs. A GET, so you can run it from a browser. Every param is optional. A positive
lag means the signal arrives later on `ch_b`.

**The pair** — one of:
- `device=<prefix>` and/or `stream=<name>` alone: **left vs right of that sender** — member 1 of its
  run is `ch_a`, member 2 `ch_b` (resolved as in *Choosing inputs* above; with several
  matching runs, the first). A one-channel sender is a 400: pair it with `a=`/`b=`.
- `a=<side>` and `b=<side>`, each an input number (`a=0`) or a sender `<device>[/<stream>]`
  (`a=heston`, `a=heston/main`, `a=/notif`): **the first channel of that sender's run** — sender
  against sender, or a sender against an analog reference.
- `ch_a=` / `ch_b=` — input numbers, as before (`ch_a` and `a` together is a 400).
- Nothing (or one side missing): the first inputs that are `playing` (as in `genie/sound`), then
  those whose 3 s held peak is above −60 dB.

`ch`, `stream_index` and `kind` are a 400 here; a sender that is not connected is a 404 with
`last_seen`, as in `genie/sound` (its `error` starts with `a: `/`b: ` for a side). The reply echoes
both sides, so you can report them by name:
```json
"ch_a": 6, "ch_b": 7,
"a": {"ch": 6, "label": "NET 1", "name": "main · heston120 (1/2)", "kind": "net",
      "device": "heston120", "stream": "main", "stream_index": 1},
"b": {"ch": 7, "label": "NET 2", …}
```
(`device`/`stream`/`stream_index` are null for an analog input), plus `resolved` (as in
`genie/sound`) with `device=`/`stream=`.

**`mode`** — what to correlate (default `auto`):
- `pings` — every ping marker in the snapshot: each window runs from one emission up to just before
  the next, so it holds exactly one arrival wherever the loopback delay puts it (the console Scope's
  method). Needs the ping generator routed into the path.
- `sliding` — **anything both inputs carry** (music, radio, a DUT's own content): windows of `win`
  samples every `step` (default: the next power of two of 0.17 s — 16384 at 96 kHz, 8192 at 48 kHz —
  and `step = win`) across the snapshot. At most **200** windows: past that, 200 evenly spaced ones.
  A window where either input's RMS is below −60 dBFS is skipped unmeasured (`n_quiet`); a measured
  one is `kept` when its `confidence ≥ min_confidence` (default 1.5) and its `peak ≥ 0.05`.
  `win` is 256…524288; a lag longer than about half a window will not be found, so raise `win` for a
  long delay (e.g. a Bluetooth speaker against an analog reference: `win=131072`).
- `auto` — `pings` when at least 3 ping readings on the pair have `confidence ≥ min_confidence`,
  otherwise `sliding`. (Ping markers are logged whether or not the ping is routed anywhere, and a
  ping window of music correlates at confidence ~1.1.) If sliding finds nothing but 1–2 pings did
  reach `min_confidence`, those (and only those) are the answer; pings below it never are.
  `mode_used` says which.

**`since=<sample>`** or **`since_ms_ago=<ms>`** (before the snapshot end): only ping markers and
windows that start at or after it — the audio after a setting changed. Echoed as `since` (a sample).

Freeze: if the capture is already frozen (`POST /api/capture/freeze`) it measures on that snapshot
and leaves it frozen; otherwise it freezes, measures and unfreezes. `frozen` in the reply says which.
The snapshot is the *Analyze buffer* (`POST /api/capture/config`, 10 s by default) ending now — one
`delay_ms` behind a network sender's wire.

```json
{"ok": true, "reason": null, "mode": "auto", "mode_used": "sliding", "trusted": true,
 "ch_a": 6, "ch_b": 7, "a": {…}, "b": {…}, "frozen": false,
 "n_markers": 5, "n_skipped": 0, "n_no_arrival": 0, "n_windows": 58, "n_quiet": 0,
 "win": 16384, "step": 16384, "min_confidence": 1.5,
 "snapshot": {"freeze_sample": 3018112, "valid_start": 2058112, "valid_len": 960000, "generation": 0},
 "measurements": [
   {"start": 2058112, "len": 16384, "lag_samples": 137, "lag_ms": 1.4271, "confidence": 4.74,
    "peak": 0.99, "kept": true}],
 "summary": {"n": 58, "lag_samples_median": 137.0, "lag_ms_median": 1.4271, "lag_m_median": 0.4895,
             "lag_samples_min": 137, "lag_samples_max": 137, "lag_samples_spread": 0,
             "lag_samples_mad": 0.0, "lag_ms_mad": 0.0, "lag_ms_stdev": 0.0,
             "lag_ms_p10": 1.4271, "lag_ms_p90": 1.4271, "confidence_median": 4.74,
             "bimodal": false}}
```
- **`ok`** — false when nothing could be measured; `reason` says why, and `summary` is just
  `{"n": 0}`. The status is still **200**: check `ok` (`jq 'select(.ok)'`) before any arithmetic on
  the summary. Reasons: `no ping markers in the snapshot (ping generator off?)`,
  `all N pings no_arrival on ch A/B: check routing`, `both channels silent (RMS below -60 dB)`,
  `ch_a (IN 5) silent …`, `the two channels are never loud in the same window`,
  `snapshot shorter than one window (…)`, `no window correlated with confidence >= 1.5 (unrelated
  signals, or a steady tone)` — any steady periodic waveform, harmonics and all; in `auto`, the
  sliding reason, then `; pings: …` (what the pings found, `no ping markers …` included) only
  when the ping generator is routed somewhere now — an output, or a channel of an enabled sink's
  layout, whose source is `gen` `ping` — so a sync over music with the ping routed nowhere is not
  sent to check a routing nobody asked for. With `mode=pings` the pings' reason is the reason, and
  `all N pings no_arrival on ch A/B` then ends `: the ping is routed to no output or sink` instead
  of `: check routing` when nothing plays it.
- `trusted` — the summary can be taken at face value. Pings: `confidence_median > 3` and
  `lag_samples_mad ≤ 2`. Sliding: `n ≥ 10` kept windows, `lag_ms_stdev < 0.5` and not `bimodal`,
  with `win` at least a quarter of the default (4096 at 96 kHz, 2048 at 48 kHz) and the stdev under
  `win`/64: a small window confines every lag to it, so its readings agree by construction, and on
  tonal music they agree on a wrong lag near 0. False whenever `!ok`.
- Counts: `n_markers` ping markers inside the snapshot (after `since`), `n_no_arrival` of them read
  as no arrival, `n_windows` sliding windows tried, `n_quiet` of them skipped for a quiet input,
  `n_skipped` entries of `measurements` that carry `skipped`. The counts of a mode that did not run
  are 0.
- `measurements` — the readings of `mode_used`. A ping entry is `{center, variant, start, len,
  lag_samples, lag_ms, lag_m, confidence, peak}`, or `{center, variant, skipped}` with `outside
  buffer`, `before since`, `too near buffer end` or the `xcorr` error, and `"no_arrival": true` when
  its `peak` is below 0.05 (the pair carries no arrival for that ping, e.g. it was emitted before
  routing). A sliding entry is `{start, len, lag_samples, lag_ms, confidence, peak, kept}` (quiet
  windows are only counted).
- `summary` — over the ping readings with an arrival, or the kept windows: `n`, median, min/max and
  `lag_samples_spread` (max−min, the reading-to-reading jitter), `lag_samples_mad`/`lag_ms_mad`
  (median absolute deviation: the jitter with outliers ignored), `lag_ms_stdev`, `lag_ms_p10`/`p90`,
  `confidence_median`, and `bimodal`: the lags are two answers, not one — over 20 % of them more
  than 5 MAD from the median, or two clusters of at least 20 % each with a gap over 5× their MAD.
  Read `confidence` per reading: below ~2 the lag is ambiguous (a repeating stimulus or a steady
  tone). Raise the *Analyze buffer* to reach further back.

With `cur_x`/`cur_y` — two sample indices bracketing **one** window — `mode` is ignored:
```json
{"ok": true, "reason": null, "ch_a": 0, "ch_b": 1, "a": {…}, "b": {…}, "frozen": true,
 "start": 1166016, "len": 16384, "lag_samples": 137, "lag_ms": 1.4271, "lag_m": 0.4895,
 "confidence": 42.0, "peak": 0.99}
```
A silent input there is `ok: false` with `reason` (200); a window outside the snapshot is a 400.

Answers 503 when there is not enough captured audio to freeze, 400 when no two inputs have sound and
no pair was given. **Latency:** one call is a freeze copy plus up to ~60 xcorrs (200 at most); on a
Pi 3 expect about 0.5–3 s (a larger `win` or a longer *Analyze buffer* costs more), so use
`curl -m 15`, not `-m 3`. One call measures at a time: a second waits for the first (up to 10 s,
then **429**), so it never loses the first's snapshot. **410** when the snapshot was resumed or
replaced while the call read it (the console's Analyze/Resume, `POST /api/capture/freeze` or
`resume`): its readings would mix two snapshots, so ask again.

```sh
# L/R skew of a sender, on whatever it plays
curl -s -m 15 "$B/api/genie/sync?device=heston&mode=sliding" |
  jq 'if .ok then {lag_ms: .summary.lag_ms_median, trusted, mad: .summary.lag_ms_mad} else .reason end'
# speaker A against speaker B, only audio from after the last change
curl -s -m 15 "$B/api/genie/sync?a=atron&b=heston&since_ms_ago=5000" | jq '{ok, reason, mode_used, trusted, summary}'
```

## Network inputs

Any Linux machine on the network can feed audio in through the `soundtester` ALSA plugin
(`alsa-plugin/` in the source tree), and it arrives as an ordinary input channel — metered, in the
scope, routable to an output, and a valid operand for `xcorr` against a real input.

A sender does not know the card's clock and never has to. It says only how far into its own stream
each packet starts; the **device** anchors the stream when its first packet arrives, adds the
**alignment delay** (`delay_ms`, 1 s by default), and places everything after it contiguously — the
way an RTP receiver decides playout. Local capture is held back by the same delay, so ring index
`n` means one instant on every channel. That delay does not appear in measurements: a delay
measured between a network channel and a real input is the true path delay. It is on while network
input or the Bluetooth input (a phone is a sender too) is enabled, and zero otherwise.

Two machines' clocks are never identical, and a sender need not even run at the card's rate. Both
differences are taken up in one place: every stream passes through an asynchronous sample-rate
converter, and its **ratio** is trimmed continuously to hold the stream the configured delay ahead
of playout. Nothing is corrected as a step anywhere, and the sender carries no model of the device.

The lead that steers it is averaged over a couple of seconds first, so the loop follows drift
rather than network jitter, and it is measured against an interpolated playout position rather
than the last completed audio block. `lead_frames` is that filtered value — the loop's own input —
and it should sit within a fraction of a millisecond of `target_lead_frames`.

If a stream does get further out than the ratio could walk back in reasonable time — a quarter of
a second — it is re-anchored instead, which is a discontinuity but a bounded one. `resyncs` counts
those, and a number that climbs means the link cannot hold the configured `delay_ms`.

Any of the usual rates and `S16_LE` / `S24_3LE` / `S32_LE` / `FLOAT_LE` are accepted; the device
converts the format and resamples the rate, so `plug:` is no longer needed for ordinary files.

A sender may also **encode** rather than send PCM, for a link that cannot carry the raw rate:
`ENCODING=vorbis` compresses on the sending machine and the device decodes. Measured on 96 kHz
S32 stereo that is 877 kB/s down to 65 kB/s. It is **lossy** — right for monitoring and for
stimulus, wrong for a reference measurement, which wants the default `pcm`. `QUALITY` is Vorbis's
own VBR scale, −0.1 to 1.0, default 0.4.

Raw Vorbis packets, not an Ogg stream: this transport already frames and orders them, which is the
job Ogg would be doing, so a container would add overhead and a second parser for nothing. The
setup packets travel once, before any audio.

A network channel's ring slot is permanent, because the ring is one pinned allocation and because
a freeze taken after the sender disconnected still has to be analysable. What is dynamic is
whether the console shows it: `active` goes true when a sender first uses the channel and stays
true for the rest of the session, or until `DELETE /api/net/channels/{n}` takes it away.

Only playback into the device is supported; there is no capture direction yet. A Bluetooth phone
lands on these channels too, through `PUT /api/bluetooth/input`, and is reported here like a
sender, with `transport: "bluetooth"` and its Bluetooth address as `peer`.

### `GET /api/net`
```json
{"enabled": true, "listening": true, "port": 4010, "delay_ms": 1000,
 "delay_frames": 96000, "rate": 96000, "n_now": 1466240, "unix_ms": 1795077781797,
 "lead_seconds": 1.0, "error": "", "inputs": 6, "inputs_configured": 6, "inputs_max": 10,
 "channels": [{"channel": 0, "input": 6, "connected": true, "connects": 1,
               "connected_since_ms": 1795077760533, "peer": "192.168.1.7:41154",
               "name": "thinkpad", "stream": "main", "last_stream": "main",
               "host": "thinkpad.lan", "device": "thinkpad",
               "last_device": "thinkpad", "mac": "3C:7C:3F:12:9A:01", "transport": "net",
               "state": "streaming", "sender_rate": 48000, "format": "S16_LE", "encoding": "pcm",
               "stream_index": 1, "stream_count": 1, "frames_received": 874496,
               "frames_per_s": 96361.1, "last_audio_ms_ago": 2,
               "late_drops": 0, "range_drops": 0, "underruns": 0, "underrun_events": 0,
               "resyncs": 0, "last_target": 1562240, "write_end": 1562496,
               "lead_frames": 96012, "lead_valid": true, "target_lead_frames": 96000,
               "lead_error_ms": 0.125, "peak": 0.79, "peak_db": -1.94}]}
```
`?connected=1` lists only the channels a sender is on (`?connected=0` only the others); anything
else is a 400. `unix_ms` is the wall clock (ms since 1970, UTC) read with `n_now`.
`input` is the index to use everywhere else — route it with
`{"source": {"type": "input", "index": 6}}`, or measure it with `?ch_a=0&ch_b=6`. `name` is what
the sender calls itself (the plugin sends its hostname), `host` its address resolved to a name when
the network can, and `device` the one to show: `name`, else `host`, else the address. `last_device`
keeps it after the sender disconnects. `stream` is which of the machine's outputs this is — the
PCM's `name` setting, empty for a PCM without one — and
`last_stream` keeps it like `last_device`; the channel's name in `GET /api/state` reads
`main · thinkpad`. `mac` is the hardware address of the sender's interface that reaches the
device, kept past the disconnect; empty from an older plugin, and for a phone (its address is
`peer`). `state` is the link: `streaming` while audio is arriving, `connected` with a sender that
has the link open and sends none, `disconnected` once it has gone, empty on a channel never used.
`sender_rate`, `format` (`S16_LE`, `S24_3LE`, `S32_LE`, `FLOAT_LE`) and `encoding` (`pcm`,
`vorbis`) are what a connected sender declared: 0 and empty before it has, and without one.
`inputs` is how many network channels
there are, `inputs_configured` how many the next start will have. `transport`
is `net`, or `bluetooth` for a phone. `lead_error_ms` is `lead_frames` against
`target_lead_frames` in milliseconds, meaningful while `lead_valid`.

**Is it live?** One read answers it: `state` is `streaming` while audio arrived in the last second;
`last_audio_ms_ago` is how long ago it last did (null with no sender on the channel, or before its
first audio); `frames_per_s` is the rate `frames_received` grew at over the last 1–1.25 s (frames
at the engine's `rate`, after the converter, so ≈ `rate` while streaming — it arrives in bursts, so
±10 % is normal — and 0 when stalled; null until the channel has been watched for a second).
`connected:true` with `frames_per_s` 0 is a sender with its link open and nothing arriving: the
link or the sender, not the DUT's audio. `connects` counts claims of the channel since the daemon
started (or the channel was forgotten), so a change between two reads means the sender reconnected;
`connected_since_ms` is the wall-clock time (ms since 1970) of the current one, null without one.

**Levels.** `peak_db` is that input's meter, as `peak_db` in `GET /api/meters` and `genie/sound`: a
3 s hold on the ring, so it is aligned with every other input and trails the sender by `delay_ms`.
`peak` is something else — the **linear** maximum |sample| since the sender connected, measured as
packets arrive (a `delay_ms` ahead of playout) after the rate converter, so it can exceed 1.0 and
never falls while the sender stays: not a live level. Use `peak_db` (or `genie/sound`) to ask "is
there sound now".

**Counters** are per channel, since the daemon started or the channel was forgotten (`DELETE
/api/net/channels/{n}`); they are not reset on reconnect. `late_drops` counts packets that arrived
after the instant they asked for. They are discarded, not slid forward: playing them late would put
the audio at the wrong place on the axis, which is the one thing this device must not do. A steady
count means the network cannot keep up with `delay_ms` — raise it. `range_drops` (packets) means the
sender aimed outside the buffer entirely. `underruns` counts **engine blocks** (`engine.period`
frames each) in which playout ran past the end of the sender's audio **while it was streaming**
(audio within the last second), and `underrun_events` how many separate times that began — the
number to watch: one per dropout. Both are judged once per run, so the channels of a stereo sender
always agree. A sender that stops sending (paused, or its PCM held open and silent) counts nothing;
neither does the time before its first packet. With the default `delay_ms` of 1000, playout only
runs dry after the sender has been silent on the wire for about a second, so a sender-side stall
shows as `late_drops` and a `resyncs` step when it resumes rather than as underruns, and a link
that stalls and then delivers its backlog counts one event (and only the blocks while late packets
were arriving). With a short `delay_ms`, any gap longer than the delay counts.

### `DELETE /api/net/channels/{n}`
Forgets a channel no sender is on: `active` goes false, its counters and `last_device` are cleared,
and a returning sender is no longer steered back to it. Its routing is left as it is, and so is the
audio already in its ring column. A sender on the channel is a 409; check `limits.net_forget`.

### Rates, formats and multi-channel senders
A sender asks for `channels` (default 1, at most 8 and at most `inputs`) and gets that many **adjacent** inputs — a
stereo source becomes NET 1 + NET 2, not NET 1 and NET 5 — so a pair reads as one source. All of its
channels travel in the same packet and through one converter, which keeps them sample-identical even
while the stream is being resampled: they cannot come apart because they are never handled apart.

`GET /api/net` reports `stream_index` and `stream_count` per channel, and a channel's name gains
`(1/2)`, `(2/2)` and so on so two cards from one machine are told apart. A mixer drives the whole
run at once, the way a card's Master does.

### Ports, and which channel a sender lands on
The device listens on the base port for *any* channel, and on **base + 1 + N for NET N+1** — so
`4010` takes whatever is free and `4013` is NET 3. Choosing a port is as deliberate as passing a
channel, so it wins over the `channel` argument in the sender's config.

With neither, a returning sender is given **the channel it used last time**, matched on its
machine — its MAC, or its address from a plugin that sends none — and its stream name. Routing set
up against NET 3 therefore keeps meaning that machine across a reconnect, or a new DHCP lease, and
a channel goes on saying whose it was while the machine is switched off. Two streams from one
machine are told apart by `name` (`main`, `notif`): each is a sender of its own and comes back to
its own run. Without a `name` a PCM is the machine's one unnamed output. The memory lasts as long
as the daemon runs.

A sender with no channel of its own takes one nobody remembers first (never used, or removed in
the console), and only then the one let go longest ago — so it does not land on the place of a
machine that is merely switched off.

**One output holds one run, never two.** An output is a machine's MAC and the PCM's `name`, or
the MAC alone for a PCM without one (two streams pinned to different channels also count as two).
When a HELLO arrives for an output whose run is still held, the device shuts the old connection
down — without asking whether it is alive: its socket may well be, with no audio on it and none
coming — and hands the **same NET inputs** to the new one, so routing and measurements carry on
against the same link. That covers a machine back from suspend, a player that opens its PCM again
without closing the last one, and a PCM whose link dropped and reconnected before the device saw
the old connection die (which it also says with the instance id in its HELLO, the only thing that
takes a run over from a plugin that sends no MAC).

Two live opens of one output do not normally get that far: the plugin holds a lock on the sending
machine (an abstract socket named after the stream name, pin and host, released by the kernel when
the PCM closes or its process dies) and the second `snd_pcm_open()` fails with `EBUSY` there and
then, as a hw PCM's does — decided locally, so it cannot disagree with what the device has seen of
the network. The takeover above is for links the sender no longer has.

Should a stream that was shut down be alive after all — the same device under two host names,
or an older plugin — its plugin
reconnects and is refused (`ST_HELLO_TAKEN`) for as long as the newer open holds the run, and its
application gets `EBUSY`, "Device or resource busy", as from a hw PCM that is already open: the
stream stops, and `snd_pcm_prepare()` fails with it until the newer open has gone.
If the old session has not let go within a second the new one is refused too and retries, rather
than being put on a second channel.

A stream that sends **no audio for 30 minutes** is disconnected and its inputs freed (they stay
remembered as its place). The device says `BYE` first, so the plugin waits until audio plays
again before it reconnects, to the same inputs; an older plugin reconnects as soon as its
application next touches the PCM.

### `PUT /api/net`
```json
{"enabled": true, "delay_ms": 1000, "port": 4010, "inputs": 8}
```
`port` is 1–65503, so a per-channel port for every network input there can be exists too. Changing it rebinds the
listener and drops any connected sender; with network input off it is only remembered, for the
next enable. Either way `config/save` keeps it; a port outside that range is a 400. `delay_ms`
clamps to `limits.net_delay_min_ms`…`limits.net_delay_max_ms` (0–2000). Answers `{"enabled",
"listening", "port", "delay_ms", "delay_frames", "inputs", "inputs_configured", "inputs_max",
"error"}`; a bind that fails still answers 200 — check `listening` and `error`.

`inputs` is how many network inputs the device has, `limits.net_inputs_min`…`net_inputs_max`
(else 400). Each is a column of the capture ring, pinned in RAM whether or not anything is sent to
it (`limits.pinned_mb` grows by roughly 37 MB per input at 96 kHz, 20 MB once the ring is shortened), so the count is fixed when the
daemon starts: a new one is written straight into the saved config (`net.inputs`) — only that
value; nothing else is saved with it — and takes effect at the next start, `POST
/api/system/restart`. `inputs_configured` is the saved value, `inputs` the running one. A save that
fails (no data partition) says why in `error`. The daemon also takes `--net-inputs N`, which wins
over the config. When the capture ring would not fit in the RAM available at startup, it is first
shortened from 87 s to 43.7 s at 96 kHz (the longest Analyze buffer a Pi can be given is shorter
than that anyway; `limits.capture_max_frames` says what is left), and only past that are fewer
inputs taken than asked, never below 6. `limits.net_inputs_max` is the most that fit. Inputs keep their gain, name and routing across a change: device inputs,
which sit after the network ones, move with them, and an output routed to a network input that is
gone falls silent.

### On the sending machine
```sh
make plugin plugin-install                       # builds and installs both plugin halves
aplay -D soundtester:192.168.1.42 tone.wav
aplay -D 'plug:"soundtester:192.168.1.42"' any.wav
aplay -D soundtester:HOST=192.168.1.42,PORT=4013 tone.wav   # pin this machine to NET 3
aplay -D soundtester:HOST=192.168.1.42,CHANNELS=2 st.wav    # stereo -> two adjacent inputs
aplay -D soundtester:HOST=192.168.1.42,NAME=notif ding.wav  # shown as "notif" on its channel
aplay -D soundtester:HOST=192.168.1.42,ENCODING=vorbis f.wav # compress on the way
alsamixer -D soundtester:192.168.1.42                       # volume and mute
```
There is a control plugin as well as a PCM one, because a PCM device with no mixer is only half a
sound card. It exposes **one** Master Playback Volume and Switch over **every** channel this
machine is streaming, so a stereo sender is turned down as one source. They are the inputs' own
`gain_db` and `mute`, so a slider moved in the web console appears in an open `alsamixer`, and the
other way about.

With no `channel` it follows the same address memory the audio side uses, and keeps following it:
alsamixer is normally open before anything plays, when that machine's run is one channel wide, and
the volume widens to cover both when its stereo stream takes NET 1+2. A pinned `CHANNEL` or port
stays where it was put, still covering the whole run that channel belongs to. A mixer with a `NAME`
drives that named stream of the machine (until it has played, whichever of the machine's it finds).
The mixer takes `HOST`, `PORT`, `CHANNEL` and `NAME`; the PCM also `CHANNELS`, `ENCODING`,
`QUALITY` and `MIXER`. `NAME` is optional, up to 32 characters, and an older device ignores it. Both take
them positional or named: `soundtester:HOST=soundtester,CHANNELS=2`.

`alsa-plugin/examples/` has a working `/etc/asound.conf` that makes the tester a machine's default
output and mixer, and a PipeWire sink.

### A PCM the mixer does not touch
`mixer off` sends a stream the device will not level: neither alsamixer nor the web console can
attenuate or mute it. For a reference stimulus, which must not be turned down by a slider left at
40% in either of the two interfaces that share the value. Define one device each way:

```
pcm.soundtester_ref {
        type soundtester
        host "soundtester"
        mixer off
}
```
```sh
aplay -D soundtester_ref sweep.wav       # never attenuated
aplay -D soundtester:MIXER=0 sweep.wav   # the same, without a second device
```
While it runs, its inputs report `"bypass": true` and the mixer's elements go **inactive**;
`amixer cset` on them answers `Operation not permitted`.

The plugin behaves like a sound card rather than a file copier. Frames leave it at a steady rate
measured against the sender's own monotonic clock — audio flows, it is never queued up and then
flushed — so the sending application sees its buffer drain as it would from hardware. On the network
they travel in bursts of whole application periods, at least 40 ms, so a slow board on Wi-Fi is not
paying for a packet per position query. The position follows that clock rather than the plugin's poll
descriptors, so a client that schedules on its own timer drives it too: PipeWire's default `tsched`
needs no `api.alsa.disable-tsched`.

**The device does not have to be there.** Opening the PCM does not connect: playback starts, keeps
time and drains with nothing listening while a background thread keeps trying. Audio starts
arriving the moment the device answers — a stream is anchored where its first packet lands — and a
device that goes away mid-stream leaves a gap rather than an error. Both transitions are reported
once on stderr.

Holding the two machines' clocks together is the **device's** job, not the plugin's, and it does it
by trimming the converter's ratio rather than by asking the sender to do anything. So the plugin
holds no jitter buffer, tracks no clock and runs no servo. `GET /api/net` reports `lead_frames`
against `target_lead_frames`, which is the loop's error term — watch it sit still.

Vorbis is a build option on the plugin (`cmake -DST_VORBIS=OFF`, or `make plugin VORBIS=0`).
Off, the plugin has no libvorbis dependency at all — which is what you want on a machine that will
only ever send PCM — and asking for `ENCODING=vorbis` there says so plainly.

## Listening

At most 12 listen streams (WS + WAV + Ogg) at once; more get 503, and a WebSocket over the limit is
closed with 1011. `{ch}` is any input, 0 to `limits.inputs_total` − 1.

### `WS /api/listen/{ch}`
Binary frames: a little-endian `uint64` start sample, then the audio. The index lets a client spot a
gap and keeps channels aligned. Codec per connection:
- **`?codec=pcm`** (default): 4096 mono **S16_LE** samples at the native rate.
- **`?codec=opus`**: one raw **Opus** packet — a 20 ms frame decimated to 48 kHz. `?bitrate=<kbps>`
  overrides. Offered only at 48/96 kHz (`limits.listen_codecs`); asked for at any other rate, the
  stream is PCM.

A channel that does not exist closes the socket with 1008.

### `GET /api/stream.ogg`
One endless Ogg/Opus stream with every input interleaved (`limits.inputs_total` channels, in input
order), from a single ring cursor so they stay sample-aligned. Channels are uncoupled (Opus family
255) — extract them, don't play them as surround:
```sh
ffmpeg -i http://soundtester/api/stream.ogg \
  -af 'pan=mono|c0=c0' in0.wav -af 'pan=mono|c0=c6' net1.wav
```
`?bitrate=<kbps>` per channel. Max 2 concurrent. Needs a 48/96 kHz rate: 501 at any other.

### `GET /api/inputs/{ch}/stream.wav`
Endless mono WAV for VLC/ffmpeg/curl. The sizes are `0xFFFFFFFF` (unknown length); players that trust
the size stop at 4 GiB — about 6.2 h at 96 kHz.

### `POST /api/listen/codec`
```json
{"codec": "pcm", "bitrate_kbps": 96}
```
Sets the default codec and Opus bitrate. Applied live (active Opus streams follow), echoed back,
saved by `config/save`. The WS wire default stays PCM — a browser opts into Opus with `?codec=opus`.

## Telemetry

The live meters and spectrum are also plain GETs, so a script can poll without a WebSocket. Both read
the same analysis snapshot as the WS feed.

### `GET /api/meters`
```json
{"type": "meters", "sample": 1488896, "rms_db": [-20.1, …], "peak_db": [-18.0, …]}
```
One value per input (`limits.inputs_total`). `rms_db` is a 100 ms window; `peak_db` a 3 s hold. Both
post input-gain. Silence sits near −120 dB.

### `GET /api/spectrum[?<selector>]`
```json
{"sample": 1488896, "bins_hz": [20.32, …, 39371.6],
 "channels": [{"ch": 0, "bins_db": [240],
               "tone": {"valid": true, "freq_hz": 996.09, "thd_n_pct": 0.0032,
                        "peak_hz": 996.09, "peak_db": -21.0, "reason": null}}]}
```
240 log-spaced bins, 20 Hz → min(Nyquist, 40 kHz), in dBFS. `bins_hz` gives each bin's center so you
can threshold by frequency directly; each bin is the loudest FFT bin it spans, so for a precise
frequency read `tone.peak_hz` (interpolated, valid tone or not). Inputs by the genie selector
(`?ch=4,5`, `?device=`, `?kind=`, see "Choosing inputs"); omit it for every input. `tone` is as in
`GET /api/genie/sound`.

### `WS /api/ws` — push only
| rate | message |
|---|---|
| 10 Hz | `{"type":"meters","sample":…,"rms_db":[…],"peak_db":[…]}`, as `GET /api/meters` |
| 5 Hz | `{"type":"spectrum","channels":[{"ch":0,"bins":[240],"tone":{…}}]}` |
| 10 Hz | binary envelope frame (below) |
| 1 Hz | `{"type":"system", …}` (below) |

Spectrum bins are quantised to 0.1 dB on the WS to save bandwidth; the GET gives full float
precision. Unlike the meters, the WS spectrum leaves out the inputs `POST /api/telemetry/inputs`
turned off.

The `system` message carries `net_active` (one bool per network channel, `active` of
`GET /api/state`), `net_connected` (one per network channel, `connected` of `GET /api/net`), `net` (one per network
channel, the `net` object of its input in `GET /api/state`), `xruns`, `generation`, `sync_errors`, `listen_streams`, `engine_running`, the
host fields of `system` in `GET /api/state` (`cpu_pct`, `cpu_cores`, `temp_c`, `uptime_s`, `mem`,
`throttle`), `sinks[]` {`id`, `present`, `enabled`, `playing`, `layout`, `latency_ms`, `trim_ppm`,
`xruns`, `resyncs`, `error`}, `sources[]` {`id`, `first`, `channels`, `present`, `capturing`,
`error`} and `bt_request` (*Bluetooth*). A sink or source that comes or goes changes those lists,
which is the cue to fetch `GET /api/state` again.

Binary envelope frame: `u8 type=2`, `u64 first_sample`, `u16 ncols`, `u8 nchan`, then
`ncols × nchan × {i16 min, i16 max}`, little-endian, `nchan` being `limits.inputs_total`. One column
is `limits.env_column_frames` frames (480 at 96 kHz, 200 columns/s).

### `POST /api/telemetry/inputs`
```json
{"enabled": [true, true, false, false, false, false, true, true, false, false, false, false, false, false]}
```
Which inputs the console is watching: one bool per input, exactly `limits.inputs_total` of them
(400 otherwise). Disabled ones are dropped from the spectrum message (the widest
frame). Global, last-writer-wins; resets to all-on at restart.

## System

### `POST /api/config/save`
Writes the live settings to `/data/config.json`: input gains and mutes, routing (every sink's
included, by device id), generators, channel map, input and output names,
`loopback_offset_samples`, the listen codec and bitrate, network input (enabled, port, delay) and
the Bluetooth settings (radio, name, pairable, timeout, the input switch and the output's device).
Bluetooth pairings are not in it: they are kept in `/data/bluetooth` as they change. `/data` is
remounted read-write for the write, then back. Answers `{"ok": true, "path": "/data/config.json"}`;
if `/data` did not mount the save is refused with a 500 (`data_persistent: false` in
`/api/state`).

### `POST /api/config/reset`
Deletes the saved file; the next boot uses the image defaults. Bluetooth pairings stay. Answers
`{"ok": true}` also when nothing was saved, 500 when the file could not be removed.

### `POST /api/system/reboot` · `POST /api/system/shutdown` · `POST /api/system/restart`
Answers `{"ok":true}`, then runs `systemctl reboot` / `poweroff`, or restarts only the daemon
(`systemctl restart soundtesterd`, which is how a new network input count takes effect). Disabled in a simulated run and
on a desktop (board.json's `desktop`, as on a PC): 403.
Shutdown exists because a power cut during a `/data` save can corrupt the card.

### `POST /api/system/inject-kmsg`
Test hook: feed a line to the kmsg watcher to exercise the I2S-sync-error banner. The body is the
line, `bcm2835-i2s: I2S SYNC error!` when empty. Answers `{"ok": true, "sync_errors": N}`.
```sh
curl -X POST http://soundtester/api/system/inject-kmsg -d 'bcm2835-i2s: I2S SYNC error!'
```
