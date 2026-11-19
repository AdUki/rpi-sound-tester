# HTTP API

Base URL `http://soundtester` (port 80; `--port` elsewhere, `:8080` for `make run`). Use the hostname
or IP: mDNS (`soundtester.local`) does not resolve everywhere. Bodies are JSON except audio streams.

**Inputs**, in index order: the **engine card**'s (`kind` `local`: on a Pi the Octo's 6 ADCs, IN 1–6
= inputs 0–5; its 8 DACs are outputs 0–7, OUT 1–8), then **network** inputs (`net`, NET 1–6 by
default: inputs 6–11 on a Pi, 0–5 on a VIM3L), then **device** inputs (`device`: columns for capture
devices found at runtime; board.json `device_inputs`, 2 on a Pi, 4 on a VIM3L). The engine card paces
everything; a board without one (VIM3L) runs on a timer and has no local inputs or outputs. Any input
index works everywhere. Counts: `limits.inputs_local`, `inputs_device`, `inputs_total`, `outputs`.
Indexes are fixed for the daemon's life and meter/envelope arrays carry every input; `active` (false
on a network channel no sender has used, a device input with no device) only hides inputs in the
console. Every other playback device is a **sink**.

**Writes.** PUT fields are optional (send what changes; `PUT /api/channel-map` needs both maps).
Out-of-range numbers clamp; what cannot clamp is rejected (bad enum, unknown input, sink
`sample_rate` not in its `rates`, repeated map slots). Success is `{"ok": true}` unless shown
otherwise. Errors are `{"error": "why"}`: **400** body not JSON, missing field, bad value; **404**
unknown input, output, sink, channel or Bluetooth device, and any unanswered `/api/` path or method
(`no such endpoint: GET /api/status; see GET /api`); **409** does not fit the current state; **503**
what it needs is not running or not ready.

### `GET /api`
This document as HTML (built from `api.md`); also static `/api.html`.

## State

### `GET /api/state`
Keys: `version`, `unix_ms`, `daemon_uptime_s`, `inputs`, `outputs`, `sinks`, `sources`, `devices`,
`generators`, `channel_map`, `capture`, `engine`, `system`, `limits`.
- `version` — `{"git": "8c6a898cad15-dirty", "build_time": "2026-11-19T08:42:26Z"}`: `git describe
  --always --dirty` of the built tree (`"unknown"` if the build could not ask git) and UTC build time
  (image builds: the newest source file's). Compare with the checkout before trusting this document.
- `unix_ms` — wall clock (ms since 1970, UTC) at reply time; pair with `engine.samples` to align
  sample indices with a DUT's log. `daemon_uptime_s` (the machine's is `system.uptime_s`).
- `inputs[]` — `ch`, `name`, `kind`, `label` (console name, "HDMI loopback L"), `active`, `gain_db`,
  `mute`, `bypass`, `gain_min_db`, `gain_max_db`, `rms_db`, `peak_db`, `tone` (as in `genie/sound`),
  `net` (null except on a network input: `{name, state, transport, rate, format, encoding}` as in
  `GET /api/net`, `rate` = the sender's).
- `outputs[]` — engine card outputs (empty without one): `ch`, `label` (`OUT 1` for ch 0; sink
  channels are named by `position`), `source`, `gain_db`, `mute`, `name`.
- `sinks`, `sources`, `devices` — as their GETs. `capture` — `capture/status` without `live_*`,
  `analyze_seconds`, `ring_seconds`.
- `engine` — `running`, `sim`, `backend` (`card` | `timer` | `simulator`), `device` (ALSA name),
  `rate`, `period`, `periods`, `format`, `capture_channels` (before the slot map), `xruns`,
  `generation` (bumps on every xrun), `samples` (the sample counter), `last_error`.
- `system` — `cpu_pct`, `cpu_cores[]`, `temp_c`, `uptime_s`, `mem` {`total_kb`, `used_kb`,
  `available_kb`}, `throttle` {`available` (false off a Pi), `under_voltage`, `throttled`,
  `freq_capped`, `under_voltage_seen`, `throttled_seen`}, `sync_errors`, `hostname`, `ips`,
  `has_saved_config`, `data_persistent`, `listen_streams` (open listen streams),
  `loopback_offset_samples` (config.json only).
- `limits`:

| group | fields |
|---|---|
| counts | `inputs_total`, `inputs_local`, `inputs_device`, `outputs`, `net_inputs` (running), `net_inputs_configured` (next start), `net_inputs_min`, `net_inputs_max`, `sinks_max` |
| gains | `input_gain_min_db`, `input_gain_max_db`, `net_gain_min_db`, `net_gain_max_db` |
| capture | `capture_max_frames`, `env_column_frames` (frames per envelope column), `pinned_mb` (locked for ring, snapshot, sinks) |
| listening | `listen_codecs`, `listen_default_codec`, `listen_bitrate_kbps`, `listen_bitrate_min_kbps`, `listen_bitrate_max_kbps`, `opus_rate` |
| network | `net_port`, `net_delay_ms`, `net_delay_min_ms`, `net_delay_max_ms` |
| flags | `channel_map`, `sync_watch` (both false without an engine card), `bluetooth` (running), `restart` (`POST /api/system/restart` allowed); always true: `capture_config`, `input_mute`, `telemetry_mask`, `net`, `net_forget` |

**Names** are set only in `config.json`. A network input without one takes its sender's (`thinkpad`,
`thinkpad (1/2)` for half a stereo stream), with ` (gone)` once disconnected: select NET inputs by
`net.state` or the genie selector, not by name.

## Inputs

### `GET /api/inputs`
`inputs[]` of `GET /api/state`.

### `PUT /api/inputs/{ch}`
`{"gain_db": 12.0, "mute": false}`, applied **before the ring**: meters, spectrum, THD+N, scope,
xcorr and listen streams are all post-gain; muted reads silent everywhere. Range (`gain_min_db`…
`gain_max_db`): ADC inputs (engine card, devices) 0…+40 dB (attenuation would only hide codec
clipping); network inputs −60…0 dB (a mixer volume; unity is already full scale). A `mixer off`
stream (*A PCM the mixer does not touch*) sets `"bypass": true` and ignores gain and mute until it
ends; the stored gain is kept.

## Routing and outputs

### `PUT /api/outputs/{0-7}`
Engine card outputs; 404 without one.
```json
{"source": {"type": "input", "index": 3}, "gain_db": -6.0, "mute": false}
{"source": {"type": "gen", "index": "ping"}}
```
`type`: `silence` (also when missing) | `input` (`index` any input, network included) | `gen`
(`index` `sine` | `noise` | `ping` | `music`). `gain_db` −60…0.

### `POST /api/outputs/{0-7}/identify`
Three 100 ms beeps on that output only, then it reverts.

### `PUT /api/channel-map`
`{"input_map": [0,1,2,3,4,5], "output_map": [0,1,2,3,4,5,6,7]}` — TDM slot per logical input/output,
to correct the Octo's slot rotation. Both required: `input_map` 6 distinct slots of 0–7, `output_map`
a permutation of 0–7, else 400. 404 without an engine card.

## Sinks

Every playback device but the engine card (HDMI, the Pi's jack, USB), from an ALSA scan at startup
and every 2 s. A sink plays the same sources at the same sample index as the engine outputs (pings on
it are logged and measured alike), on its own clock through an ASRC that holds the latency constant.

Id = `<card id>,<device>` (`b1,0`, `Headphones,0`, `G12BKHADASVIM3L,0`, `Device,0` for USB class
devices), stable across reboots, probe order and USB ports, as are its saved routing and place in
`sinks`. An unplugged device keeps its sink (`present: false`) and routing. The sink `bluetooth` is
the Bluetooth output (bluez-alsa playback, stereo), present whenever `limits.bluetooth`; its speaker
is set with `PUT /api/bluetooth/output`.

### `GET /api/sinks` · `GET /api/sinks/{id}`
```json
{"id": "b1,0", "label": "HDMI", "hdmi": true, "usb": false, "present": true,
 "layouts": ["mono", "stereo", "5.1", "7.1"], "rates": [32000, 44100, 48000, 96000],
 "enabled": true, "open": true, "playing": true, "device": "hw:CARD=b1,DEV=0",
 "sample_rate": 48000, "device_rate": 48000, "layout": "stereo", "speakers": 2,
 "device_channels": 2, "format": "S16_LE", "period_frames": 1024, "buffer_frames": 8192,
 "latency_ms": 112.0, "target_ms": 112.0, "ring_ms": 27.4, "alsa_ms": 84.6, "trim_ppm": -1.2,
 "xruns": 0, "underruns": 0, "overruns": 0, "resyncs": 0, "error": "",
 "outputs": [{"ch": 0, "position": "L", "slot": 0, "source": {"type": "gen", "index": "music"},
              "gain_db": 0, "mute": false, "name": ""}]}
```
- `latency_ms` — engine to driver, averaged, held at `target_ms`. Excludes the driver's pipeline and
  the TV/receiver (constant: measure once with ping `tick` looped back into an input and `genie/sync`
  against a reference, interval ≥ 1 s so each window holds one arrival).
- `resyncs` — re-anchors (each a latency step). `error` — why it will not open. `outputs` — the
  layout's channels with their PCM `slot`. `format` — first of S32_LE, S24_LE, S24_3LE, S16_LE the
  PCM takes. `period_frames`, `buffer_frames` — in device frames.

### `PUT /api/sinks/{id}`
`{"enabled": true, "sample_rate": 48000, "layout": "5.1"}` → the sink as `GET` gives it. Sinks start
off; a save keeps them on. A rate or layout change restarts that sink only.
- `layout` — one of `layouts`. **HDMI** (driver says HDMI, or board.json `hdmi`): `mono` | `stereo`
  | `5.1` | `7.1` in CEA-861 order (FL FR LFE FC …) — the only counts that map to one layout (4 could
  be quad or 3.1). Mono goes out on L and R (`device_channels` 2). 5.1/7.1 need `sample_rate` ≤ 48000
  (Pi HDMI limit). Others: `stereo` and their own count (`6ch`, `8ch`) in device order; mono devices
  `1ch` only.
- `sample_rate` — one of `rates` (of 32000, 44100, 48000, 88200, 96000, 176400, 192000, those the
  device accepts). Prefer 48000: HDMI drivers accept rates the TV cannot play.

### `PUT /api/sinks/{id}/{ch}` · `POST /api/sinks/{id}/{ch}/identify`
As the engine outputs, per channel. HDMI channels are **speakers**: 0 L, 1 R, 2 C, 3 LFE, 4 Ls, 5 Rs
(side pair in 7.1), 6 Lb, 7 Rb (7.1 only); mono plays speaker 0. All eight keep their routing across
layouts.

## Device inputs

Capture devices other than the engine card (USB inputs, the VIM3L's HDMI loopback), scanned like
sinks. Each takes two adjacent device-input columns (one if mono, more if it cannot do two), kept for
the daemon's life (a replug returns to the same inputs). An ASRC places each frame at the sample
index it was captured at, so it lines up with the engine card to ~1 ms plus a device constant (ADC +
USB latency; calibrate once). For that, while any device input is bound, the whole capture axis is
delayed by `net.delay_ms`, at least 150 ms: inter-input measurements are unaffected and the ping log
accounts for it, but a device input routed to an output is heard that much later.

### `GET /api/sources`
```json
[{"id": "Device,0", "label": "USB Advanced Audio Device", "first": 8, "channels": 2,
  "present": true, "open": true, "capturing": true, "device": "hw:CARD=Device,DEV=0",
  "device_rate": 48000, "format": "S16_LE", "trim_ppm": 13.0,
  "xruns": 0, "resyncs": 0, "late": 0, "error": ""}]
```
`first` — first input index. `late` — blocks that arrived after the ring read past them (lost).

### `GET /api/devices`
Every ALSA PCM (`aplay -l`, `arecord -l`) and what the daemon does with it:
```json
[{"id": "Device,0", "alsa": "hw:CARD=Device,DEV=0", "card": "Device",
  "card_name": "USB Advanced Audio Device", "name": "USB Audio", "label": "USB Advanced Audio Device",
  "playback": true, "capture": true, "usb": true, "engine": false, "hidden": false,
  "sink": 1, "input": 8, "input_channels": 2, "note": ""}]
```
`engine` — the engine card's; `hidden` — board.json says it is wired to nothing; `sink` — sink slot,
`input` — first input, -1 for none; `note` — why not used (`no free sink slot`, `no free input
columns`). With board.json `"scan": false` (a PC) only the `--sink`/`--input` devices are listed,
under their names (a hardware one under its device id).

## Bluetooth

A2DP both ways (SBC; BlueZ + bluez-alsa): a paired speaker is a stereo **output** (sink `bluetooth`);
a phone playing to the tester is a stereo **input** on two adjacent network channels, aligned like a
network sender. The tester accepts every pairing and connection and trusts every paired device.
Pairings are saved to `/data/bluetooth` on change (no `config/save` needed). On a PC (`make pc
BT=hci1`) only the `--bt-adapter` (hci name or address) adapter is used: other adapters' devices are
not listed and their agent requests refused; pairings stay with the PC's bluetoothd.

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
- `available` — false with no (matching `--bt-adapter`) adapter or no BlueZ, `error` says why.
  `audio` — bluez-alsa running (pairing and scanning work without it). `settings.name` empty = the
  hostname.
- Devices: `sink` — takes audio (speaker); `source` — sends it (phone); `playback`/`capture` — open
  bluez-alsa links or null; `delay_ms` — reported codec+radio delay (not in `latency_ms`); `busy` —
  `pairing` | `connecting` | `disconnecting`, with `error` the last failure; `rssi` — only while
  scanning; `volume` — AVRCP absolute volume 0–127 or null; `player` — a phone's AVRCP player while it
  plays, or null (`position_ms` extrapolated between BlueZ reports); on a speaker the tester plays to,
  the tester's own player (`"local": true`).
- `request` — a pending pairing question (*Pairing requests*).
- `input` — `state` `off` | `waiting` | `streaming` | `error`; `input` first channel or -1; `frames`
  read; `overruns` (bluez-alsa buffer full); `restarts` (re-anchors: phone paused, new device).
- `output` — `GET /api/sinks/bluetooth` plus `PUT /api/bluetooth/output`'s extra fields; null while
  Bluetooth is down.

### `PUT /api/bluetooth`
`{"enabled": true, "name": "bench-tester", "pairable": true, "discoverable": true,
"discoverable_timeout_s": 180}` → the `GET` body. `enabled` powers the radio; `discoverable` for
`discoverable_timeout_s` (0–3600, 0 = until off; always starts off); `name` ≤ 248 bytes (else 400),
empty = hostname. All but `discoverable` are kept by `config/save`.

### `POST /api/bluetooth/scan`
`{"on": true}` — BR/EDR scan, stops itself after 60 s. Can cause dropouts on audio links meanwhile.

### `POST /api/bluetooth/devices/{address}/pair` · `/connect` · `/disconnect`
Answer `{"ok": true}` at once; the outcome shows in the device's `busy`/`error`. Pairing also
connects; optional `{"pin": "1234"}` (≤ 16 chars, default `0000`) for legacy devices. 404 malformed or
unknown address; 503 Bluetooth not running.

### `POST /api/bluetooth/devices/{address}/player`
`{"command": "play"}` | `pause` | `stop` | `next` | `previous` — to a phone's player, or on a speaker
to the tester's own player. 409: no player, unknown command, or Bluetooth not running.

**The tester's own player** (AVRCP, offered to speakers it plays to, so their buttons and the console
card's work the output): title = the output's signal (`Silence`, `Sine`, `Noise`, `Ping`, `Music`, or
an input like `IN 3`), artist `Sound Tester`, album = signal settings (`440 Hz · -20 dBFS`), position
= time on the current signal. Status `stopped` (output off), `paused` (both channels muted), else
`playing`. `play` unmutes both (switching the output on), `pause` mutes, `stop` switches off,
`next`/`previous` cycle both channels through Silence, Sine, Noise, Ping, Music.

### `PUT /api/bluetooth/devices/{address}` · `DELETE /api/bluetooth/devices/{address}`
`PUT {"volume": 0..127}` (clamped) — AVRCP absolute volume (phone or speaker); 409 if it has none,
later failures in `error`. `DELETE` unpairs and removes its keys from `/data`.

### Pairing requests · `POST /api/bluetooth/request`
Only keyboards ask. Shown as `request` here and `bt_request` in the WS `system` message (null when
none):
```json
{"id": 7, "kind": "passkey", "address": "C7:3B:52:10:9A:1E", "name": "MX Keys",
 "passkey": "", "expires_s": 54}
```
`passkey`: type the 6 digits the device shows — `{"id": 7, "accept": true, "passkey": 123456}`
(number or string). `display`: shows `passkey` to type on the device; clears itself. 409: `id` no
longer waiting, not six digits, or Bluetooth not running.

### `PUT /api/bluetooth/input`
`{"enabled": true}` — audio of the most recently connected streaming device, on two adjacent network
channels named after it (`transport: "bluetooth"` in `GET /api/net`). Turns on the alignment delay.
Answers `GET`'s `input`; 503 when Bluetooth is not running. Kept by `config/save`.

### `PUT /api/bluetooth/output`
`{"device": "bluealsa:DEV=F8:DF:15:0A:11:3C,PROFILE=a2dp", "enabled": true}` (both optional; off by
default) — the speaker of sink `bluetooth`. `device` must be `bluealsa:…` (else 400), kept by
`config/save`; the default `DEV=00:00:00:00:00:00` = most recently connected speaker. Routing, rate
and identify go through `/api/sinks/bluetooth`. Answers the sink status plus `address`, `name` and
`device_delay_ms` (the speaker's reported delay). The speaker picks the rate (`device_rate`;
`sample_rate` is a request). `latency_ms` excludes codec and radio, which vary: calibrate with a ping
before trusting absolute delays. 503 without Bluetooth.

## Generators

Driven by the ring's sample counter: generated and captured audio align to the sample.

### `PUT /api/generators/sine`
`{"freq_hz": 996.09375, "level_db": -20.0}` — 996.09375 Hz is bin-centred for the 8192-point FFT (no
leakage in THD+N).

### `PUT /api/generators/noise`
`{"mode": "white", "level_db": -20.0}` — `white` | `pink`.

### `PUT /api/generators/ping`
`{"variant": "tick", "interval_s": 2.0, "level_db": -20.0}` — `tick` | `bing` | `bong`; use `tick`
(broadband, sharp correlation peak) for delays. Any change reschedules the next ping from now.

### `PUT /api/generators/music`
`{"level_db": -20.0}` — 12.8 s "Ode to Joy" loop with bass, for listening tests. Same bar on every
output at the same moment; rendered only while routed.

### `GET /api/pings/recent`
`[{"sample": 1466240, "variant": "tick"}]` — emission samples of the last 64 pings.

## Capture, scope and delay

`start`/`len` are absolute indices on the shared sample counter.

### `POST /api/capture/freeze` · `POST /api/capture/resume` · `GET /api/capture/status`
Freeze copies the recent ring into a snapshot (503 if not enough audio yet). Resume answers
`{"frozen": false}`. `status` (freeze answers its first five fields):
```json
{"frozen": false, "freeze_sample": 0, "valid_start": 0, "valid_len": 0, "generation": 0,
 "live_now": 21910528, "live_oldest": 13523968, "analyze_frames": 960000, "analyze_seconds": 10.0,
 "ring_seconds": 87.36}
```
Snapshot reads must stay in `[valid_start, valid_start + valid_len)`. A changed `generation` (bumps
per xrun) means a gap. `live_now` — write head; `live_oldest` — oldest readable; `analyze_*` — size
of the next freeze; `ring_seconds` — live ring length once filled.

### `POST /api/capture/config`
`{"seconds": 20.0}` or `{"frames": N}` — next freeze size, clamped to [4096,
`limits.capture_max_frames`]; resets to 10 s on restart. Answers `analyze_frames`,
`analyze_seconds`, `max_frames`, `max_seconds`, `pinned_mb`. 507 if it cannot be allocated.

### `GET /api/capture/window?ch=&start=&len=&cols=`
Scope data for one channel (snapshot if frozen, else live ring). `ch` required (0…`inputs_total`−1;
lists/repeats are 400); `len` 1…ring (96000); `start` (last `len` frames); `cols` 1–2048, not clamped
(1024). Answers `{ch, start, len, cols, raw}` plus `min[]`/`max[]` per column, or `samples[]` when
`raw` (`len ≤ 2×cols`; so ≤ 4096 samples per call — use `capture/wav` for more). Params must be
numbers ≥ 0 (fractions truncated); otherwise, out of range or an unreadable span is a 400 naming the
range: `[5, 105) is outside the live ring [13530112, 21916672)` (`frozen range` while frozen).

### `GET /api/capture/wav?[<selector>&](start=&len= | seconds=)[&format=]`
Interleaved WAV of the selected inputs.
```sh
curl -s -m 30 -D h.txt -o cap.wav "$B/api/capture/wav?device=heston&seconds=12&format=s16"
```
- Inputs — the selector (*Choosing inputs*), none = all; `ch=2,0` order is file order. Senders
  resolve to where they are **now**, even on a frozen snapshot.
- Span, exactly one of: `start=&len=` (frames; frozen: the valid range; live: `[live_oldest,
  live_now)` minus its oldest **2 s** once the ring is full, which the writer may overwrite during
  the build) or `seconds=N` (last N s of the **live ring** even when frozen; ≤ ring − 2 s).
- `format` — `f32` (default; ring values bit-exact, ±1.0 full scale), `s32`, `s16` (rounded, clipped;
  Octo 24-bit codes exact in `s32`). Plain `fmt` chunk (tag 3/1, no WAVE_FORMAT_EXTENSIBLE): fine for
  libsndfile, scipy, sox, ffmpeg; Python's `wave` reads only `s16`/`s32`.

Headers (also in the `LIST/INFO` chunk with each input's `label`; `ffprobe` shows it):
`X-ST-Start-Sample` (first frame's index), `X-ST-Len` (frames), `X-ST-Rate`, `X-ST-Generation`
(snapshot's when frozen, else the engine's: compare with an earlier reading), `X-ST-Unix-Ms` (wall
clock at the first frame), `X-ST-Channels` (ring columns in file order, `6,7`), `X-ST-Source`
(`frozen` | `live`), `X-ST-Format`. Network/device audio was heard `capture_delay_ms` before its index.

Limits: ≤ **256 MB** of samples (`len × channels × 4`, × 2 for `s16`: ~50 s of 14 `f32` inputs at
96 kHz). Built in memory from post-validated reads (never stitched across ring laps): 200 MB takes
~0.15 s on a desktop, several times that on a Pi 3, plus transfer (384 kB/s per 96 kHz `f32`
channel). Use `curl -m 30` or more. One build at a time; others wait up to 10 s.

Errors: **400** span outside the valid range (`seconds must be at most 34.480 (what the live ring
holds now, less 2.0 s kept clear of its oldest end)`), both/neither of `start+len`/`seconds`, `start`
without `len`, bad `format`/number/selector, or over the cap (`5280000 frames is 282 MB: at most 256
MB, i.e. 4793490 frames (49.932 s) …`); **404** sender not connected; **410** span overwritten
during the read, or snapshot resumed/replaced — retry; **429** another export held the build > 10 s;
**507** not enough free memory.

### `POST /api/capture/xcorr`
`{"ch_a": 0, "ch_b": 1, "start": 386560, "len": 16384}` → `{"lag_samples": 137, "lag_ms": 1.4271,
"lag_m": 0.4895, "confidence": 4.2, "peak": 0.99}`. **Freeze first** (else 400); `len` 64…2^19; 400
if either channel is silent. **Positive lag = later on `ch_b`.** `lag_m` — acoustic distance (air
paths only).

`confidence` = winning peak / tallest rival: > 3 trust, < 2 ambiguous — several pings in the window
(bracket one) or a steady periodic signal (any waveform; delay known only modulo its period: use a
ping); a tone with only a few periods in the window reads ~1. A ringing ping (`bing`, `bong`) needs
its onset inside the window.

## Genie helpers

| Question | Endpoint | Answers |
|---|---|---|
| Is there sound, is it playing now, is it the expected tone? | `GET /api/genie/sound` | at once |
| Tell me when it starts, stops, goes live or changes | `GET /api/genie/wait` | when met, or at `timeout_s` |
| Did it drop out, click, step or slip over the last/next N s? | `GET /api/genie/watch` | at once (`from=past`) or after N s (`from=now`) |
| Delay between two inputs (L/R skew, speaker vs speaker, path latency) | `GET /api/genie/sync` | 0.5–3 s on a Pi |
| Raw samples of these inputs | `GET /api/capture/wav` | at once |

Common rules:
- Inputs by the selector below; replies echo `label`, `name`, `kind`, `net` per input (report "NET
  3", not a column) and place readings on the sample axis (`sample`, `start_sample`, `snapshot`) and,
  for sound/wait/watch, the wall clock (`unix_ms`, `unix_ms_start`).
- GET only (works from a browser). Examples use `B=http://soundtester`.
- "Nothing found" is a **200** with `met: false` / `ok: false` and a `reason`: check it first.
- **Capture delay**: audio enters the ring `capture_delay_ms` after it was heard — `GET /api/net`
  `delay_ms` while network or Bluetooth input is on, ≥ 150 ms with a device input bound, else 0. A
  sender that just started reads `playing` that much later.

### Choosing inputs
Taken by the genie endpoints, `capture/wav` and `GET /api/spectrum`; all optional (none = every
input):
- `ch=4,5` — by index, in that order (or `ch=4&ch=5`).
- `device=<prefix>` — a **connected** sender whose `device`, `name`, `host`, `last_device` or `mac`
  starts with it, case-insensitive (`heston` → `heston120-501e2d676be9`).
- `stream=<name>` — the plugin's stream `name` (`main`), exact; ANDed with `device`.
- `stream_index=N` — with `device`/`stream`: member N (1-based) of each run (1 = left).
- `kind=local|net|device` — filter (also filters `ch`).

Resolved per request, so it follows reconnects (never hard-code a sender's channels). If several runs
match (a stale run, `main` + `notif`), only `streaming` ones count if any, else all connected ones;
all are returned, run by run:
```json
"resolved": {"inputs": [20, 21], "device": "heston",
             "runs": [{"inputs": [20, 21], "channel": 14, "device": "heston120-501e2d676be9",
                       "stream": "pipewire", "stream_count": 2, "state": "streaming"}]}
```
(`channel` = first NET channel; without `device`/`stream` just `inputs`, plus `kind` if given.)

**400**: non-numeric or out-of-range `ch`, empty `device`, `ch` with `device`/`stream`, `stream_index`
without them, `kind` other than `net` with them, or no input matches (`the selector matches no inputs
on this board`). **404**: no connected sender matches, or no member `stream_index`:
```json
{"error": "no connected sender matches device 'heston120'", "matched": 0,
 "last_seen": [{"input": 20, "channel": 14, "last_device": "heston120-501e2d676be9",
                "last_stream": "pipewire", "state": "disconnected"}]}
```

### `GET /api/genie/sound[?<selector>&threshold_db=&min_rms_db=&expect_hz=&tol_hz=]`
```json
{"sample": 1488896, "unix_ms": 1763560000123, "threshold_db": -60.0, "min_rms_db": -60.0,
 "resolved": {…},
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
`unix_ms` — wall clock at `sample`. Per channel (levels post-gain, silence −120):
- `net` — sender on a network input: `device` (empty once gone; `last_device` keeps it), `state`
  `streaming` | `connected` (silent on the wire or stalled) | `disconnected` | `""` (never used).
- `peak_db` — **3 s hold**: each peak held exactly 3 s after its sample; every sample metered, so a
  1 ms tick between polls counts. `sound` = `peak_db > threshold_db` (stays true 3.0–3.1 s after the
  sound, with `rms_db` at −120).
- `peak_now_db`, `rms_db` — the latest 100 ms window. `sound_now` = `peak_now_db > threshold_db`.
- `playing` = `sound_now` and `rms_db > min_rms_db` — **the "playing now" verdict**; an idle analog
  input's noise floor (peaks ~−55, RMS −70…−82) fails it.
- `last_sound_ms_ago` — since a reading (every sample counts) last peaked above −60 dBFS (fixed, not
  `threshold_db`), to 100 ms; 0 while it does, null if never.
- `tone` — 8192-sample FFT at 5 Hz (lags levels ~200 ms). `peak_hz`/`peak_db` — dominant partial in
  20 Hz–20 kHz. `valid` if it is above **−80 dBFS** and the rest (THD+N) ≤ **30 %**; then `freq_hz`,
  else 0 and `reason` `"silent"` (`thd_n_pct` 0) or `"not_a_tone"` (music, speech, noise).
  `thd_n_pct` is **percent**.

Params: `threshold_db` (−60), `min_rms_db` (−60), `expect_hz=1000` or `1000,1500` (all channels, or
one per channel) adds `tone.expect_hz` and `tone.match` (valid and within `tol_hz`, default 5,
echoed). **400** bad selector, non-numeric param, `expect_hz` count mismatch; **404** sender not
connected. Answers at once from the 100 ms analysis snapshot.
```sh
curl -s -m 5 "$B/api/genie/sound?device=heston" | jq -c '.channels[] | {label, playing, rms_db}'
curl -s -m 5 "$B/api/genie/sound?ch=0,1&expect_hz=1000" | jq -c '.channels[] | {label, m: .tone.match}'
```

### `GET /api/genie/wait?until=…[&<selector>&hold_ms=&timeout_s=&all=&threshold_db=&min_rms_db=&freq_hz=&tol_hz=]`
Blocks until a condition has held `hold_ms` (checked every 100 ms on the `genie/sound` readings and
`GET /api/net`), or `timeout_s`.
```json
{"met": true, "until": "sound", "after_ms": 2911, "met_sample": 1335168,
 "met_unix_ms": 1763560003235, "onset_ms": 1611, "hold_ms": 300, "timeout_s": 20.0, "all": false,
 "threshold_db": -60.0, "min_rms_db": -60.0, "sample": 1363968, "unix_ms": 1763560003535,
 "capture_delay_ms": 1000.0, "resolved": {…}, "moved_from": null,
 "channels": [{"ch": 6, "label": "NET 1", "name": "main · heston120 (1/2)", "kind": "net",
               "device": "heston120", "net": {…, "frames_received": 46041,
                       "frames_per_s": 47980.2, "last_audio_ms_ago": 17},
               "holds": true, "playing": true, "sound_now": true, "rms_db": -23.0,
               "peak_now_db": -20.0, "tone": {…}, "changed": null}]}
```
`until` (required):
- `sound` — `playing`.
- `silence` — not `sound_now` (no 3 s hold: met right after the sound stops).
- `tone` — `tone.valid`, within `tol_hz` (5) of `freq_hz` if given. `expect_hz` is an alias (both:
  400); either, or `tol_hz`, with another `until` is a 400 (`freq_hz` elsewhere is a 400 naming
  `expect_hz`).
- `live` — sender `streaming` and `frames_received` grew since the wait began: audio arriving
  **now**. Network inputs only (default: all of them; another `ch` or `kind` is a 400).
- `change` — `playing` flipped since the wait began; `changed` per input is `"on"`, `"off"` or null.
  Wait with no selector and toggle the DUT to find its input.

Params: `hold_ms` (300; 0 = first look), `all=1`/`any=0` (every selected input, ≥ 1, must hold;
default any), `timeout_s` (20; 0…120, 0 = look once), `threshold_db`/`min_rms_db` (−60). With no
selector every input is watched — give one for `sound`/`silence`/`tone` or an idle input decides.

The selector is re-resolved every look: a reconnecting sender is followed (`moved_from` lists the old
inputs; new inputs get their own baseline and hold), and one not yet connected is waited for. For
`silence`/`change`, a sender that leaves (aplay ends) is still watched on its last inputs unless
another sender took them, so "until the DUT stops" is met: `"sender_left": true`, `resolved: null`,
those inputs in `channels`.

Reply: `after_ms` — wall time from request to answer, **including `hold_ms`** and, except for
`live`, **`capture_delay_ms`**, plus up to 100 ms (tone 200 ms) of refresh. When it actually started
(null unless `met`): `met_sample`/`met_unix_ms` — the reading that began the hold (with `all=1`, the
last input's; a reading ends its 100 ms window, so the change was up to 100 ms earlier); `onset_ms` —
`met_unix_ms − capture_delay_ms −` request time (for `live`, the look's time), i.e. "how long after I
pressed play", ≤ 0 if it already held (≈ −`capture_delay_ms` for `sound` on a playing input).
`sample`/`unix_ms` — the last reading; per input its levels, `holds` (held on the last look) and, on
network inputs, `net` with `frames_received`, `frames_per_s`, `last_audio_ms_ago`.

Timeout = 200, `met: false`, `reason`: `no connected sender matches stream 'main'` (+ `last_seen`);
`sender not streaming: NET 1 (heston120 connected, last audio 4004 ms ago); NET 3 (no sender)` — for
`sound`/`tone`/`live` waits about senders only (`device`/`stream`, or only network inputs); else
`timed out after 20 s: sound did not hold for 300 ms on any selected input`. **400** missing/unknown
`until`, bad number or selector; **429** 8 waits pending; **503** `the daemon is shutting down`.
Hanging up frees the slot within 100 ms. Use `curl -m` ≥ `timeout_s` + 5.
```sh
curl -s -m 30 "$B/api/genie/wait?device=heston&until=sound&timeout_s=20" | jq '{met, after_ms, reason}'
curl -s -m 40 "$B/api/genie/wait?kind=local&until=change&timeout_s=30" |
  jq '.channels[] | select(.changed) | {label, changed}'
```

### `GET /api/genie/watch[?<selector>&seconds=&from=&block_ms=&silence_db=&click_k=&expect_hz=&resid_dbc=&slip_deg=&level_step_db=]`
Dropouts, clicks, level steps and (on a test tone) phase slips and distortion over a span, each at
its sample, plus level statistics.
```json
{"from": "now", "seconds": 10.0, "start_sample": 7384064, "len": 960000, "analysed": 960000,
 "rate": 96000.0, "block_ms": 10.0, "unix_ms_start": 1763560000000, "t_ago_s_start": 10.04,
 "capture_delay_ms": 1000.0, "generation": 1, "generation_changed": false,
 "incomplete": false, "reason": null, "elapsed_ms": 10051, "analysis_ms": 46,
 "params": {"silence_db": -60.0, "click_k": 10.0, "expect_hz": "auto", "resid_dbc": -50.0,
            "slip_deg": 20.0, "level_step_db": 6.0},
 "resolved": {…},
 "channels": [{"ch": 6, "label": "NET 1", "name": "main · heston120 (1/2)", "kind": "net",
               "net": {…}, "sound_pct": 82.9, "silent_s": 1.71,
               "rms_db": {"min": -120.0, "p10": -120.0, "median": -13.47, "max": -13.4},
               "peak_max_db": -10.46, "clipped_samples": 0,
               "counts": {"gap": 1, "click": 0, …},
               "tone": {"expect_hz": 998.145, "source": "analysis", "blocks": 829, "bad_blocks": 2,
                        "freq_hz_median": 1000.0, "freq_hz_min": 998.004, "freq_hz_max": 1000.0,
                        "thd_n_pct_median": 0.0666, "thd_n_pct_max": 42.1,
                        "resid_dbc_median": -63.5, "resid_dbc_max": -7.5},
               "tone_note": null,
               "link": {"underrun_events": 2, "late_drops": 7, …}}],
 "events": [{"kind": "gap", "ch": [6, 7], "labels": ["NET 1", "NET 2"], "sample": 7811998,
             "t_s": 4.4576, "t_ago_s": 5.58, "len_ms": 1720.792, "zeros": true, "click": true}],
 "events_truncated": false}
```
Span: `seconds` (10) of each selected input (≤ 16; none = all). `from=past` (default) — the last
`seconds`, at once; ≤ ring − 3 s (~84 s with 6 network inputs at 96 kHz, ~40 s with 16).
`from=now` — the next `seconds` (≤ 60), then analysed: start it, then act; `curl -m` `seconds` + 10;
hanging up ends it.
The audio was heard `capture_delay_ms` earlier. Events carry `sample`, `t_s` (from span start),
`t_ago_s` (before the reply). If the writer overtakes the read (very long span, busy Pi):
`incomplete: true`, `reason`, `analysed` < `len`.

Per channel, over `block_ms` blocks (10; 1–100):
- `sound_pct` — share in blocks with RMS ≥ `silence_db` (−60); `silent_s` the rest.
- `rms_db` — block RMS `min`/`p10`/`median`/`max`; `peak_max_db`; `clipped_samples` (|x| ≥ 0.999).
- `counts` — events per kind (every kind below; complete even past the event cap).
- `tone` — tone mode (null when off): each block least-squares fitted to a sine at `expect_hz` (+DC,
  linear amplitude/phase drift). `freq_hz_*` from phase advance between clean blocks (finer than FFT
  bins: shows a sender's ASRC trim, e.g. 998.0 Hz at its 0.2 % limit); `thd_n_pct_*` (%) and
  `resid_dbc_*` — per-block residual; `blocks` fitted (non-quiet), `bad_blocks` above `resid_dbc`;
  `source` of `expect_hz`. `tone_note` — why tone mode is off under `auto`.
- `link` — `from=now`, network inputs only: change in the sender's `GET /api/net` counters
  (`underruns`, `underrun_events`, `late_drops`, `range_drops`, `resyncs`, `reconnects`),
  `device_changed` (another sender at the end; false if the channel ended empty — a leaver shows
  `state: disconnected`), end `state` and `frames_per_s`. Counters lead the ring by
  `capture_delay_ms`: a hole in the span's last second shows here before it shows as an event.

Events (all channels, by sample, ≤ 200, then `events_truncated`): `kind`, `ch`, `labels` (one event
for all members of a sender's run within 2 ms), `sample`, `t_s`, `t_ago_s`, `len_ms`.
- `gap` — quiet blocks (< `silence_db`) with level (≥ `silence_db` + 20) within 30 ms on **both**
  sides, sustained (0.5 s median each side): a dropout. Pauses of intermittent signals (ping, click
  track) are not gaps, nor their bursts onsets. `zeros: true` = one run of exact 0.0 (network hole, muted input): sample-exact,
  else block-exact. Analog paths are never exactly zero (muted output → `zeros: false`).
- `onset` / `offset` — that abrupt edge on one side only (`onset`: `sample` its first block, `len_ms`
  0; `offset`: `len_ms` = quiet time, to the span's end or a soft return). Fades are neither.
- `zero_run` — ≥ 1 ms of exact 0.0 inside audio (level within 50 ms both sides), shorter than a quiet
  block; sample-exact. Holes inside a `gap` belong to it.
- `click` — |x[n−1] − 2x[n] + x[n+1]| > `click_k` (10) × the median of its and the neighbouring 10 ms
  blocks, and ≥ `silence_db`; `sample` = worst of a 2 ms cluster, `len_ms` the cluster, `level_db`.
  Edges of gaps, holes, slips and steps are flagged `click: true` on that event instead. 40 s of noise
  or music reads no clicks at 10; go down to ~8 for smaller ones.
- `level_step` — RMS of two **steady** 100 ms stretches (all blocks but one within 1.5 dB, quiet ones
  left out) differs by
  > `level_step_db` (6): `from_db`, `to_db`; block-exact. Music shows its own dynamics: use a test
  signal.
- `phase_slip` — tone mode: phase jump > `slip_deg` (20°) against the steady drift (lost or repeated
  samples). `slip_samples` (> 0 ahead/lost, < 0 behind/repeated, modulo one period: ±48 at 1 kHz /
  96 kHz), `slip_deg`; sample-exact (block-exact if the span left the ring before the second look).
  A slip across a `gap`/`zero_run` (≤ 1 s) is reported on that event (absent: the hole kept its place,
  as network holes do). It must persist (clean blocks before and after each agree with their
  neighbour), so a one-block disturbance is none, nor is anything in the first or last block or two.
  A trailing partial block counts for levels and clicks, for `resid` only if long enough, never for
  phase.
- `resid` — tone mode: consecutive blocks with residual > `resid_dbc` (−50) not explained by another
  event (distortion, interferer, wrong `expect_hz`); `resid_dbc` = worst.

`expect_hz`: `auto` (default; per input the tone detector's `freq_hz` if valid at the span's end,
else off), `none`, or `1000` / `1000,1500` (all or one per input; needs a whole cycle per block, ≥ 100
Hz at 10 ms, and below Nyquist). Ranges: `resid_dbc` −200…0, `slip_deg` 0…180, `level_step_db`
0…100.

`generation_changed` — the span holds a seam (xrun, network delay change, device input bound), which
can read as a gap or slip (`from=now`: counter moved during the span; `from=past`: the latest move is
inside it).

**400** bad selector or param, `seconds` over the limit or not yet in the ring, `expect_hz` count
mismatch, > 16 inputs; **404** sender not connected; **429** 2 watches running; **503** `the daemon
is shutting down` or `the engine is not advancing: …` (span not captured 5 s late; see
`engine.last_error`). Analysis ~1000× real time per input on a desktop (`analysis_ms`), 10–20× slower
on a Pi 3 (whole ring of all inputs ~10 s): `curl -m 30` for `from=past`.
```sh
curl -s -m 30 "$B/api/genie/watch?device=heston&seconds=30" | jq '[.events[] | {kind, labels, t_ago_s, len_ms}]'
curl -s -m 75 "$B/api/genie/watch?stream=main&from=now&seconds=60&expect_hz=1000" | jq '.channels[] | {label, counts, link}'
```

### `GET /api/genie/sync[?<pair>&mode=&since=&win=&step=&min_confidence=&cur_x=&cur_y=]`
Delay between two inputs (positive = later on `ch_b`). Pair, one of:
- `device=`/`stream=` alone — **left vs right** of that sender (members 1 and 2 of the first matching
  run; a mono sender is a 400).
- `a=`, `b=` — input number or sender `<device>[/<stream>]` (`heston`, `heston/main`, `/notif`) = its
  run's first channel.
- `ch_a=`, `ch_b=` — input numbers (with `a`/`b`: 400).
- none or one side missing — the first `playing` inputs, then those with held peak > −60 dB.

`ch`, `stream_index`, `kind` are 400s; a disconnected sender is a 404 with `last_seen` (`a: `/`b: `
prefix). The reply echoes `ch_a`, `ch_b`, `a`/`b` = `{ch, label, name, kind, device, stream,
stream_index}` (last three null on analog inputs) and `resolved`.

`mode` (`auto`):
- `pings` — one window per ping marker, from its emission to just before the next (one arrival each).
  Needs the ping routed into the path.
- `sliding` — anything both inputs carry: `win`-sample windows (256…524288; default the power of two
  ≥ 0.17 s: 16384 at 96 kHz, 8192 at 48 kHz) every `step` (= `win`), ≤ **200** (evenly spaced beyond). Windows with
  either RMS < −60 dBFS are skipped (`n_quiet`); `kept` needs `confidence ≥ min_confidence` (1.5) and
  `peak ≥ 0.05`. Lags beyond ~half a window are missed: raise `win` (`131072` for a Bluetooth
  speaker).
- `auto` — `pings` if ≥ 3 ping readings reach `min_confidence` (markers are logged even when the ping
  is not routed; music in a ping window reads ~1.1), else `sliding`; if that finds nothing, 1–2
  confident pings are the answer. `mode_used` says which.

`since=<sample>` / `since_ms_ago=<ms>` — only markers/windows from then on (echoed as `since`).
Snapshot: uses the current freeze if frozen (left frozen), else freezes, measures, resumes
(`frozen`). It is the Analyze buffer (`capture/config`, 10 s) ending now, `delay_ms` behind a
sender's wire.
```json
{"ok": true, "reason": null, "mode": "auto", "mode_used": "sliding", "trusted": true,
 "ch_a": 6, "ch_b": 7, "a": {…}, "b": {…}, "frozen": false,
 "n_markers": 5, "n_skipped": 0, "n_no_arrival": 0, "n_windows": 58, "n_quiet": 0,
 "win": 16384, "step": 16384, "min_confidence": 1.5,
 "snapshot": {"freeze_sample": 3018112, "valid_start": 2058112, "valid_len": 960000, "generation": 0},
 "measurements": [{"start": 2058112, "len": 16384, "lag_samples": 137, "lag_ms": 1.4271,
                   "confidence": 4.74, "peak": 0.99, "kept": true}],
 "summary": {"n": 58, "lag_samples_median": 137.0, "lag_ms_median": 1.4271, "lag_m_median": 0.4895,
             "lag_samples_min": 137, "lag_samples_max": 137, "lag_samples_spread": 0,
             "lag_samples_mad": 0.0, "lag_ms_mad": 0.0, "lag_ms_stdev": 0.0,
             "lag_ms_p10": 1.4271, "lag_ms_p90": 1.4271, "confidence_median": 4.74,
             "bimodal": false}}
```
- `ok` false (still 200; `summary` = `{"n": 0}`): `reason` is one of `no ping markers in the snapshot
  (ping generator off?)`, `all N pings no_arrival on ch A/B: check routing` (`: the ping is routed to
  no output or sink` when nothing plays it), `both channels silent (RMS below -60 dB)`, `ch_a (IN 5)
  silent …`, `the two channels are never loud in the same window`, `snapshot shorter than one window
  (…)`, `no window correlated with confidence >= 1.5 (unrelated signals, or a steady tone)`. In
  `auto` the sliding reason is followed by `; pings: …` only if the ping is routed to an output or an
  enabled sink's channel.
- `trusted` — pings: `confidence_median > 3`, `lag_samples_mad ≤ 2`; sliding: ≥ 10 kept, `lag_ms_stdev
  < 0.5` and < `win`/64, not `bimodal`, `win` ≥ ¼ default, 4096 at 96 kHz (small windows agree by construction, and
  on a wrong lag near 0 on tonal music). False when `!ok`.
- `n_markers` (after `since`), `n_no_arrival`, `n_windows`, `n_quiet`, `n_skipped` (entries with
  `skipped`); 0 for a mode that did not run.
- `measurements` — ping: `{center, variant, start, len, lag_samples, lag_ms, lag_m, confidence,
  peak}`, `"no_arrival": true` if `peak` < 0.05, or `{center, variant, skipped}` (`outside buffer`,
  `before since`, `too near buffer end`, an xcorr error); sliding: `{start, len, lag_samples, lag_ms,
  confidence, peak, kept}`.
- `summary` — over arrivals or kept windows: `n`, medians, min/max, `lag_samples_spread` (max − min),
  `lag_samples_mad`/`lag_ms_mad` (jitter without outliers), `lag_ms_stdev`, `lag_ms_p10`/`p90`,
  `confidence_median`, `bimodal` (> 20 % of lags beyond 5 MAD, or two ≥ 20 % clusters more than 5 MAD
  apart).

`cur_x`/`cur_y` (bracketing **one** window) override `mode` and answer one reading: `{ok, reason,
ch_a, ch_b, a, b, frozen, start, len, lag_samples, lag_ms, lag_m, confidence, peak}`; a silent input
is `ok: false`, a window outside the snapshot a 400.

**503** not enough audio to freeze; **400** no pair given and no two inputs with sound; **429** a
second call waited > 10 s (one at a time); **410** snapshot resumed/replaced mid-read — retry. Takes
~0.5–3 s on a Pi 3 (≤ 200 xcorrs): `curl -m 15`.
```sh
curl -s -m 15 "$B/api/genie/sync?device=heston&mode=sliding" | jq 'if .ok then .summary.lag_ms_median else .reason end'
curl -s -m 15 "$B/api/genie/sync?a=atron&b=heston&since_ms_ago=5000" | jq '{ok, reason, trusted, summary}'
```

## Network inputs

Any Linux machine sends audio with the `soundtester` ALSA plugin (`alsa-plugin/`); it arrives as an
ordinary input. Playback into the tester only. Bluetooth input lands here too (`transport:
"bluetooth"`, address as `peer`).

Each packet carries its offset in the sender's stream. The tester anchors a stream at its first
packet plus the **alignment delay** (`delay_ms`, default 1000) and holds local capture back by the
same delay, so a ring index is one instant on every channel and network-vs-analog delays are true
path delays. The delay is active while network or Bluetooth input is enabled, else 0. Each stream
passes an ASRC whose ratio is trimmed to hold it `delay_ms` ahead of playout (clock drift and sender
rate, no step corrections, no work on the sender). `lead_frames` (averaged over a couple of seconds,
against an interpolated playout position) should sit within a fraction of a ms of
`target_lead_frames`. A stream more than 250 ms off is re-anchored (`resyncs`); a climbing count means
the link cannot hold `delay_ms`.

Ring slots are permanent; `active` turns true on first use and stays until `DELETE
/api/net/channels/{n}`.

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
`?connected=1|0` filters (other values: 400). `unix_ms` pairs with `n_now`. `inputs` — running count,
`inputs_configured` — next start's. Per channel:
- `input` — the index for routing/measuring. `name` — sender's hostname; `host` — its address resolved to a name (when possible);
  `device` — `name`, else `host`, else address; `stream` — PCM `name` (empty without); `last_device`,
  `last_stream` survive disconnects. `mac` — sender interface MAC, kept (empty from an older plugin
  or a phone). `transport` — `net` | `bluetooth`.
- `state` — `streaming` (audio in the last second), `connected` (link open, no audio),
  `disconnected`, empty if never used. `last_audio_ms_ago` (null without sender or before audio);
  `frames_per_s` — growth of `frames_received` over 1–1.25 s in engine-rate frames, ≈ `rate` ±10 %
  while streaming, 0 when stalled, null for the first second. `connected` with 0 `frames_per_s` = the
  link/sender, not the DUT. `connects` counts claims (a change = reconnect); `connected_since_ms`.
- `sender_rate`, `format` (`S16_LE`, `S24_3LE`, `S32_LE`, `FLOAT_LE`), `encoding` (`pcm`, `vorbis`) —
  as declared; 0/empty without a sender.
- `lead_error_ms` — `lead_frames − target_lead_frames` in ms, valid while `lead_valid`.
- `peak_db` — the input's 3 s-hold meter on the ring (trails the sender by `delay_ms`). `peak` — linear
  max |sample| since connect, at arrival after the ASRC: can exceed 1.0, never falls; not a live level.
- Counters (since start or forget, kept across reconnects): `late_drops` — packets past their
  instant, discarded not shifted (steady count: raise `delay_ms`); `range_drops` — aimed outside the
  buffer; `underruns` — engine blocks where playout ran past the sender's audio **while streaming**,
  `underrun_events` — distinct dropouts (watch this). Judged per run, so stereo channels agree. A
  paused or silent-but-open sender counts nothing. With `delay_ms` 1000 a sender-side stall shows as
  `late_drops` + a `resyncs` step, and a stall-then-backlog as one event; with a short delay any gap
  longer than it counts.

### `DELETE /api/net/channels/{n}`
Forgets a free channel: `active` false, counters and `last_device` cleared, no longer reserved for its
last sender. Routing and ring audio stay. 409 while a sender is on it.

### Rates, formats and multi-channel senders
Any usual rate; `S16_LE`, `S24_3LE`, `S32_LE`, `FLOAT_LE`; no `plug:` needed. `ENCODING=vorbis`
(raw Vorbis packets, no Ogg; headers once) cuts 96 kHz S32 stereo from 877 to 65 kB/s but is
**lossy**: monitoring and stimulus only. `QUALITY` −0.1…1.0 (0.4).

`CHANNELS` (1; ≤ 8 and ≤ `inputs`) gets that many **adjacent** inputs, carried in one packet through
one ASRC, so they stay sample-identical. `stream_index`/`stream_count` per channel; names gain
`(1/2)`. The mixer drives the whole run.

### Ports, and which channel a sender lands on
Base port = any channel; **base + 1 + N = NET N+1** (`4013` = NET 3), overriding `CHANNEL`. Otherwise
a returning sender gets **its previous channel**, keyed by MAC (or address, from a plugin without
one) and stream `name` — routing survives reconnects and DHCP changes; memory lasts while the daemon
runs. Streams are distinct by `name` (`main`, `notif`). New senders take never-used or forgotten
channels first, then the longest-released one.

**One output holds one run** (output = MAC + PCM `name`, or the MAC alone; streams pinned to
different channels are different outputs). A HELLO for a held output shuts the old
connection down unasked and hands the **same inputs** to the new one (resume from suspend, reopened
PCM, dropped link; the HELLO's instance id does this for MAC-less plugins). A second live
`snd_pcm_open()` of one output fails with `EBUSY` locally (an abstract-socket lock on stream name, pin
and host, released when the PCM closes or the process dies). A shut-down stream that was alive (the
same machine under two host names, or an older plugin) is refused with `ST_HELLO_TAKEN`: the
application gets `EBUSY` and `snd_pcm_prepare()` fails until the newer open goes. If the old session
does not let go within 1 s, the new one is refused and retries.

**30 minutes without audio** disconnects a stream (its inputs remain its place). The tester says
`BYE`, so the plugin reconnects only when audio resumes (an older plugin on the next PCM call).

### `PUT /api/net`
`{"enabled": true, "delay_ms": 1000, "port": 4010, "inputs": 8}` → `{enabled, listening, port,
delay_ms, delay_frames, inputs, inputs_configured, inputs_max, error}`; a failed bind is still 200:
check `listening` and `error`.
- `port` — 1–65503 (else 400). Rebinds and drops senders; only stored while disabled.
- `delay_ms` — clamped to `limits.net_delay_min_ms`…`max_ms` (0–2000).
- `inputs` — `limits.net_inputs_min`…`max` (else 400). Each pins a ring column (~37 MB at 96 kHz,
  20 MB with the shortened ring), so it is saved immediately to `net.inputs` (alone) and applies after
  `POST /api/system/restart`; a failed save fills `error`. `--net-inputs N` overrides. If RAM is short
  at startup the ring shrinks from 87 s to 43.7 s (see `limits.capture_max_frames`), then fewer inputs
  are taken (≥ 6; `net_inputs_max` = what fits). Gains, names and routing follow their inputs; device
  inputs shift with them; outputs routed to a removed input go silent.

`port` and `delay_ms` are kept by `config/save`.

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
Arguments positional or named. PCM: `HOST`, `PORT`, `CHANNEL`, `NAME` (≤ 32 chars), `CHANNELS`,
`ENCODING`, `QUALITY`, `MIXER`; mixer: the first four. `alsa-plugin/examples/` has an
`/etc/asound.conf` (default output and mixer) and a PipeWire sink.

The mixer is one Master Volume + Switch over the machine's whole run — the inputs' `gain_db`/`mute`,
mirrored with the console. Unpinned, it follows the channel memory (widening from one channel to
NET 1+2 when stereo starts); a pinned `CHANNEL`/port covers that channel's run; a `NAME` targets that
stream (any of the machine's until it has played).

The plugin paces frames on the sender's monotonic clock like a sound card (bursts of whole periods,
≥ 40 ms) and reports position from that clock, so timer-driven clients work (PipeWire `tsched` needs
no `api.alsa.disable-tsched`). It needs no tester to open or play: it connects in the background,
audio starts when the tester answers, a vanished tester leaves a gap; both logged once to stderr.
Built without Vorbis (`make plugin VORBIS=0`, `-DST_VORBIS=OFF`) it has no libvorbis dependency and
rejects `ENCODING=vorbis` with a clear error.

### A PCM the mixer does not touch
`mixer off` (or `MIXER=0`) makes a stream neither alsamixer nor the console can attenuate or mute —
for reference stimuli:
```
pcm.soundtester_ref {
        type soundtester
        host "soundtester"
        mixer off
}
```
While it plays, its inputs report `"bypass": true` and the mixer elements are inactive (`amixer cset`:
`Operation not permitted`).

## Listening

≤ 12 listen streams (WS + WAV + Ogg); more get 503 (WebSocket: close 1011). `{ch}` 0…`inputs_total` −
1.

### `WS /api/listen/{ch}`
Binary frames: LE `uint64` start sample, then audio. `?codec=pcm` (default): 4096 mono **S16_LE**
samples at the native rate. `?codec=opus`: one raw Opus packet (20 ms, 48 kHz), `?bitrate=<kbps>`;
only at 48/96 kHz (`limits.listen_codecs`), else PCM. Unknown channel: close 1008.

### `GET /api/stream.ogg`
Endless Ogg/Opus of every input interleaved, sample-aligned, uncoupled (family 255 — extract, don't
play as surround):
```sh
ffmpeg -i http://soundtester/api/stream.ogg -af 'pan=mono|c0=c0' in0.wav -af 'pan=mono|c0=c6' net1.wav
```
`?bitrate=<kbps>` per channel. Max 2 at once; 501 unless 48/96 kHz.

### `GET /api/inputs/{ch}/stream.wav`
Endless mono WAV; sizes `0xFFFFFFFF`, so size-trusting players stop at 4 GiB (~6.2 h at 96 kHz).

### `POST /api/listen/codec`
`{"codec": "pcm", "bitrate_kbps": 96}` — default codec and Opus bitrate; live, echoed, kept by
`config/save`. The WS default stays PCM (browsers ask for `?codec=opus`).

## Telemetry

### `GET /api/meters`
`{"type": "meters", "sample": 1488896, "rms_db": [-20.1, …], "peak_db": [-18.0, …]}` — per input:
100 ms RMS, 3 s peak hold, post-gain, silence ≈ −120.

### `GET /api/spectrum[?<selector>]`
```json
{"sample": 1488896, "bins_hz": [20.32, …, 39371.6],
 "channels": [{"ch": 0, "bins_db": [240], "tone": {…}}]}
```
240 log bins, 20 Hz → min(Nyquist, 40 kHz), dBFS, each the loudest FFT bin it spans (`bins_hz` =
centers); for a precise frequency use `tone.peak_hz` (interpolated, tone valid or not). `tone` as in `genie/sound`.

### `WS /api/ws` — push only
| rate | message |
|---|---|
| 10 Hz | `meters`, as `GET /api/meters` |
| 5 Hz | `{"type":"spectrum","channels":[{"ch":0,"bins":[240],"tone":{…}}]}` — 0.1 dB steps, skips inputs off in `telemetry/inputs` |
| 10 Hz | binary envelope: LE `u8 type=2`, `u64 first_sample`, `u16 ncols`, `u8 nchan` (`inputs_total`), then `ncols × nchan × {i16 min, i16 max}`; a column = `limits.env_column_frames` (480 at 96 kHz) |
| 1 Hz | `system`: per network channel `net_active`, `net_connected`, `net` (as in `/api/state`); `xruns`, `generation`, `sync_errors`, `listen_streams`, `engine_running`, `cpu_pct`, `cpu_cores`, `temp_c`, `uptime_s`, `mem`, `throttle`; `sinks[]` {`id`, `present`, `enabled`, `playing`, `layout`, `latency_ms`, `trim_ppm`, `xruns`, `resyncs`, `error`}; `sources[]` {`id`, `first`, `channels`, `present`, `capturing`, `error`}; `bt_request` |

A sink or source appearing or vanishing changes those lists: refetch `GET /api/state`.

### `POST /api/telemetry/inputs`
`{"enabled": [true, false, …]}` — exactly `inputs_total` bools (else 400); drops inputs from the WS
spectrum. Global, last writer wins, all on after restart.

## System

### `POST /api/config/save`
Writes `/data/config.json` (remounting `/data` rw briefly): input gains/mutes, routing (sinks by
device id), generators, channel map, names, `loopback_offset_samples`, listen codec/bitrate, network
input (enabled, port, delay), Bluetooth (radio, name, pairable, timeout, input switch, output
device) — not pairings. → `{"ok": true, "path": "/data/config.json"}`; 500 if `/data` is not mounted
(`data_persistent: false`).

### `POST /api/config/reset`
Deletes the saved file (next boot uses image defaults; pairings stay). `{"ok": true}` even if none;
500 if it cannot be removed.

### `POST /api/system/reboot` · `POST /api/system/shutdown` · `POST /api/system/restart`
`{"ok":true}`, then `systemctl reboot` / `poweroff` / `restart soundtesterd` (applies a new network
input count). 403 in a simulated run or on a desktop (board.json `desktop`). Shut down before cutting
power: a cut during a `/data` save can corrupt the card.

### `POST /api/system/inject-kmsg`
Test hook for the I2S-sync-error banner: the body (default `bcm2835-i2s: I2S SYNC error!`) is fed to
the kmsg watcher. →
`{"ok": true, "sync_errors": N}`.
