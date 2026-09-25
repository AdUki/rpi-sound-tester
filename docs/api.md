# HTTP API

Base URL: `http://soundtester.local` (port 80 on the device; `--port` elsewhere). All bodies are
JSON except the audio streams.

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
life of the daemon, and the arrays in the meters, spectrum and envelope messages always carry every
channel — so a headless client can ignore `active` entirely.

Every other playback device is a **sink** (HDMI, a Pi's 3.5 mm jack, a USB interface), found at
runtime: see Sinks below.

Every PUT field is optional — send only what changes. Out-of-range numbers clamp to their limits;
bad enums and non-permutation maps are rejected.

### `GET /api`
This document, rendered to HTML (built from `api.md`). Also served as the static `/api.html`.

## State

### `GET /api/state`
The whole device state in one object: `inputs`, `outputs`, `sinks`, `sources`, `devices`, `generators`,
`channel_map`, `capture`, `engine`, `system`, and `limits` (slider ranges and feature flags the
console reads). `engine.backend` is `card` (the engine card), `timer` (a board without one) or
`simulator`. `limits.channel_map` and `limits.sync_watch` are `false` without an engine card: the
TDM slot map and the I2S sync watch are the Octo's.
Each input and output has a `name`, set only in `config.json` — there is no API to change it.

## Inputs

### `PUT /api/inputs/{ch}`
```json
{"gain_db": 12.0, "mute": false}
```
Gain and mute, applied **before the ring buffer** — so every reading (meters, spectrum, THD+N,
scope, xcorr, listen streams) is post-gain and a muted channel reads as silent everywhere.

An ADC channel takes 0…+40 dB: it cannot undo clipping that already happened in the codec, so
attenuating would only hide the damage from the meters. A **network** channel has no ADC and takes
−60…+40 dB, which is what gives a mixer on the sending machine a playback volume worth the name.
Each input reports its own floor as `gain_min_db` in `GET /api/state`.

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
like any other — or `sine` | `noise` | `ping` | `music` for `gen`. `gain_db` clamps to −60…0.

### `POST /api/outputs/{0-7}/identify`
Three 100 ms beeps on that output only, then it reverts. Tells you which physical socket it is.

### `PUT /api/channel-map`
```json
{"input_map": [0,1,2,3,4,5], "output_map": [0,1,2,3,4,5,6,7]}
```
`input_map[logical]` = the TDM slot to capture from; `output_map[logical]` = the slot to play into.
This corrects the Octo's slot rotation. Each map must be a permutation (in range, no duplicates) or
the request is rejected. 404 on a board without an engine card.

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

### `GET /api/sinks` · `GET /api/sinks/{id}`
```json
{"id": "b1,0", "label": "HDMI", "hdmi": true, "usb": false, "present": true,
 "layouts": ["mono", "stereo", "5.1", "7.1"], "rates": [32000, 44100, 48000, 96000],
 "enabled": true, "open": true, "playing": true, "device": "hw:CARD=b1,DEV=0",
 "sample_rate": 48000, "device_rate": 48000, "layout": "stereo", "speakers": 2,
 "device_channels": 2, "format": "S16_LE", "latency_ms": 112.0, "target_ms": 112.0,
 "ring_ms": 27.4, "alsa_ms": 84.6, "trim_ppm": -1.2,
 "xruns": 0, "underruns": 0, "overruns": 0, "resyncs": 0, "error": "",
 "outputs": [{"ch": 0, "position": "L", "slot": 0, "source": {"type": "gen", "index": "music"},
              "gain_db": 0, "mute": false}]}
```
`latency_ms` is engine to driver, averaged, and it is held at `target_ms`. It leaves out the driver's
own pipeline and the TV, receiver or interface. Both are constant, so measure them once: route a
`tick` to the sink, bring it back into an input (an HDMI audio extractor, a loopback cable), and run
`genie/sync` against a reference. Keep the ping interval at 1 s or more, so each window holds
exactly one arrival. `resyncs` counts re-anchors; each is a latency step. `error` says why a device
will not open. `outputs` lists the layout's channels, each with the PCM `slot` it is sent in.
`format` is what the PCM opened in: the first of S16_LE, S32_LE, S24_3LE and S24_LE it takes.

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
Any other sink offers `stereo` and its own channel count (`6ch`, `8ch`), in the device's own order.
`sample_rate` is one of the sink's `rates`: those of 32000, 44100, 48000, 88200, 96000, 176400 and
192000 the device accepts. Use 48000 unless you know the sink takes more: an HDMI driver accepts any
rate whether or not the TV can play it. A new rate or layout restarts that sink only; the engine is
never touched. Sinks are off until switched on here, and a save keeps them on.

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
`sink` is the sink slot its playback is on and `input` the first input its capture is on, -1 for
none; `note` says why one it could be is not (`no free sink slot`, `no free input columns`).

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
Ask only for samples in `[valid_start, valid_start + valid_len)`. `generation` bumps on every xrun —
if it changed, the timeline has a gap. `status` also returns `live_now` (write head) and
`live_oldest` (oldest readable live sample).

### `POST /api/capture/config`
```json
{"seconds": 20.0}
```
How much the next freeze copies. `{"seconds": N}` or `{"frames": N}`, clamped to
[4096, `limits.capture_max_frames`]. The reply echoes what took effect. Resets to 20 s on restart.

### `GET /api/capture/window?ch=&start=&len=&cols=`
The scope. Returns `cols` min/max pairs over the range, or raw samples when `len ≤ 2×cols`. Serves
the frozen snapshot if frozen, else the live ring.

### `POST /api/capture/xcorr`
```json
{"ch_a": 0, "ch_b": 1, "start": 386560, "len": 16384}
```
→
```json
{"lag_samples": 137, "lag_ms": 1.4271, "lag_m": 0.4895, "confidence": 4.2, "peak": 0.99}
```
Cross-correlates two inputs over a window. **Freeze first** (`len` ≤ 2^19). A **positive lag means
the signal arrives later on `ch_b`**. `lag_m` is the acoustic distance — meaningful for an air path,
not a cable.

Check `confidence` before trusting `lag_samples`: it is the winning peak over the tallest separate
rival. Above 3, trust it. Below 2 it is ambiguous — either more than one ping is in the window
(bracket a single ping) or you used a continuous tone (its delay is only known modulo the carrier —
use a ping).

## Genie helpers

Shortcuts that turn the primitives above into a single answer.

### `GET /api/genie/sound[?ch=&threshold_db=]`
Is there sound on an input?
```json
{"sample": 1488896, "threshold_db": -60.0,
 "channels": [{"ch": 0, "sound": true, "rms_db": -20.1, "peak_db": -18.0,
               "tone": {"valid": true, "freq_hz": 996.09, "thd_n_pct": 0.0032}}]}
```
`sound` is `peak_db > threshold_db`. Peak is a 3 s hold, so a tick or ping counts as sound, not just
a steady tone. `threshold_db` defaults to −60. `?ch=0..5` for one input; omit for all six.

### `GET /api/genie/sync[?ch_a=&ch_b=&cur_x=&cur_y=]`
Delay between two inputs. A GET, so you can run it from a browser. Every param is optional:
- `ch_a` / `ch_b` — the pair. Default: the first two inputs that currently have sound. Positive lag
  = later on `ch_b`.
- `cur_x` / `cur_y` — two sample indices bracketing **one** window to measure. Omit both to measure
  **every ping marker in the buffer** instead.

Freeze: if the capture is already frozen (`POST /api/capture/freeze`) it measures on that snapshot
and leaves it frozen; otherwise it freezes, measures and unfreezes. `frozen` in the reply says which.

With `cur_x`/`cur_y` — one window:
```json
{"ch_a": 0, "ch_b": 1, "frozen": true, "start": 1166016, "len": 16384,
 "lag_samples": 137, "lag_ms": 1.4271, "lag_m": 0.4895, "confidence": 42.0, "peak": 0.99}
```
Without them — every ping marker, plus a summary:
```json
{"ch_a": 0, "ch_b": 1, "frozen": false,
 "snapshot": {"freeze_sample": 3018112, "valid_start": 1097728, "valid_len": 1920384, "generation": 0},
 "measurements": [
   {"center": 1170112, "variant": "tick", "start": 1166016,
    "lag_samples": 137, "lag_ms": 1.4271, "lag_m": 0.4895, "confidence": 999.0, "peak": 0.99},
   {"center": 954112, "variant": "tick", "skipped": "outside buffer"}],
 "summary": {"n": 27, "lag_samples_median": 137.0, "lag_ms_median": 1.4271, "lag_m_median": 0.4895,
             "lag_samples_min": 137, "lag_samples_max": 137, "lag_samples_spread": 0,
             "confidence_median": 999.0}}
```
Each ping is bracketed from its emission up to just before the next, so the window holds exactly
one arrival wherever the loopback delay puts it — the same method the console's Scope uses. A ping
too near the buffer end is `skipped`; raise the *Analyze buffer* (`POST /api/capture/config`) to
reach further back. A reading whose `peak` is below 0.05 is flagged `"no_arrival": true` — the pair
carries no captured arrival for that ping (e.g. it was emitted before routing) — and is left out of
the summary. `lag_samples_spread` (max−min) is the marker-to-marker jitter. Read each `confidence`:
below ~2 the lag is ambiguous (a repeating stimulus or a continuous tone). Answers 503 when there is
not enough captured audio to freeze, 400 when no input has sound and no channels were given.

## Network inputs

Any Linux machine on the network can feed audio in through the `soundtester` ALSA plugin
(`alsa-plugin/` in the source tree), and it arrives as an ordinary input channel — metered, in the
scope, routable to an output, and a valid operand for `xcorr` against a real input.

A sender does not know the card's clock and never has to. It says only how far into its own stream
each packet starts; the **device** anchors the stream when its first packet arrives, adds the
**alignment delay** (`delay_ms`, 1 s by default), and places everything after it contiguously — the
way an RTP receiver decides playout. Local capture is held back by the same delay, so ring index
`n` means one instant on every channel. That delay does not appear in measurements: a delay
measured between a network channel and a real input is the true path delay. It is zero when network
input is disabled.

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
true for the rest of the session.

Only playback into the device is supported; there is no capture direction yet.

### `GET /api/net`
```json
{"enabled": true, "listening": true, "port": 4010, "delay_ms": 1000,
 "delay_frames": 96000, "rate": 96000, "n_now": 1466240, "lead_seconds": 1.0, "error": "",
 "channels": [{"channel": 0, "input": 6, "connected": true, "peer": "192.168.1.7:41154",
               "name": "alsa-plugin", "frames_received": 874496, "late_drops": 0,
               "range_drops": 0, "underruns": 0, "last_target": 1562240,
               "write_end": 1562496, "peak": 0.79}]}
```
`input` is the index to use everywhere else — route it with
`{"source": {"type": "input", "index": 6}}`, or measure it with `?ch_a=0&ch_b=6`.

`late_drops` counts packets that arrived after the instant they asked for. They are discarded, not
slid forward: playing them late would put the audio at the wrong place on the axis, which is the
one thing this device must not do. A steady count means the network cannot keep up with `delay_ms`
— raise it. `range_drops` means the sender aimed outside the buffer entirely, and `underruns` means
the sender stopped supplying audio before its slot came round.

### Rates, formats and multi-channel senders
A sender asks for `channels` (default 1) and gets that many **adjacent** inputs — a stereo source
becomes NET 1 + NET 2, not NET 1 and NET 5 — so a pair reads as one source. All of its channels
travel in the same packet and through one converter, which keeps them sample-identical even while
the stream is being resampled: they cannot come apart because they are never handled apart.

`GET /api/net` reports `stream_index` and `stream_count` per channel, and a channel's name gains
`(1/2)`, `(2/2)` and so on so two cards from one machine are told apart. A mixer drives the whole
run at once, the way a card's Master does.

### Ports, and which channel a sender lands on
The device listens on the base port for *any* channel, and on **base + 1 + N for NET N+1** — so
`4010` takes whatever is free and `4013` is NET 3. Choosing a port is as deliberate as passing a
channel, so it wins over the `channel` argument in the sender's config.

With neither, a returning sender is given **the channel it used last time**, matched on its
address. Routing set up against NET 3 therefore keeps meaning that machine across a reconnect, and
a channel goes on saying whose it was while the machine is switched off. Two streams from one
machine cannot be told apart by address — pin those with a port or a channel.

### `PUT /api/net`
```json
{"enabled": true, "delay_ms": 1000, "port": 4010}
```
`port` is 1–65529, so the six per-channel ports above it exist too. Changing it rebinds the
listener and drops any connected sender; with network input off it is only remembered, for the
next enable. Either way `config/save` keeps it. A bind that fails still answers 200 — check
`listening` and `error`.

### On the sending machine
```sh
make plugin plugin-install                       # builds and installs both plugin halves
aplay -D soundtester:192.168.1.42 tone.wav
aplay -D 'plug:"soundtester:192.168.1.42"' any.wav
aplay -D soundtester:HOST=192.168.1.42,PORT=4013 tone.wav   # pin this machine to NET 3
aplay -D soundtester:HOST=192.168.1.42,CHANNELS=2 st.wav    # stereo -> two adjacent inputs
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
stays where it was put, still covering the whole run that channel belongs to. Arguments are
`HOST`, `PORT`, `CHANNEL` and `CHANNELS`, positional or named:
`soundtester:HOST=bench.local,CHANNELS=2`.

`alsa-plugin/examples/` has a working `/etc/asound.conf` that makes the tester a machine's default
output and mixer, and a PipeWire sink.

### A PCM the mixer does not touch
`mixer off` sends a stream the device will not level: neither alsamixer nor the web console can
attenuate or mute it. For a reference stimulus, which must not be turned down by a slider left at
40% in either of the two interfaces that share the value. Define one device each way:

```
pcm.soundtester_ref {
        type soundtester
        host "soundtester.local"
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
flushed — so the sending application sees its buffer drain as it would from hardware, and the
network sees an even packet flow. The position follows that clock rather than the plugin's poll
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

At most 12 listen streams (WS + WAV + Ogg) at once; more get 503.

### `WS /api/listen/{0-5}`
Binary frames: a little-endian `uint64` start sample, then the audio. The index lets a client spot a
gap and keeps channels aligned. Codec per connection:
- **`?codec=pcm`** (default): 4096 mono **S16_LE** samples at the native rate.
- **`?codec=opus`**: one raw **Opus** packet — a 20 ms frame decimated to 48 kHz. `?bitrate=<kbps>`
  overrides. Offered only at 48/96 kHz (`limits.listen_codecs`).

### `GET /api/stream.ogg`
One endless Ogg/Opus stream with all six inputs interleaved, from a single ring cursor so they stay
sample-aligned. Channels are uncoupled (Opus family 255) — extract them, don't play them as surround:
```sh
ffmpeg -i http://soundtester.local/api/stream.ogg -filter_complex \
  "channelsplit=channel_layout=6.0" -map '[FL]' in0.wav -map '[FR]' in1.wav …
```
`?bitrate=<kbps>` per channel. Max 2 concurrent. Needs a 48/96 kHz rate.

### `GET /api/inputs/{0-5}/stream.wav`
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
{"type": "meters", "sample": 1488896, "rms_db": [6], "peak_db": [6]}
```
`rms_db` is a 100 ms window; `peak_db` a 3 s hold. Both post input-gain. Silence sits near −120 dB.

### `GET /api/spectrum?ch=`
```json
{"sample": 1488896, "bins_hz": [20.32, …, 39371.6],
 "channels": [{"ch": 0, "bins_db": [240], "tone": {"valid": true, "freq_hz": 996.09, "thd_n_pct": 0.0032}}]}
```
240 log-spaced bins, 20 Hz → min(Nyquist, 40 kHz), in dBFS. `bins_hz` gives each bin's center so you
can threshold by frequency directly. `?ch=0..5` for one input; omit for all six.

### `WS /api/ws` — push only
| rate | message |
|---|---|
| 10 Hz | `{"type":"meters","sample":…,"rms_db":[6],"peak_db":[6]}` |
| 5 Hz | `{"type":"spectrum","channels":[{"ch":0,"bins":[240],"tone":{…}}]}` |
| 10 Hz | binary envelope frame (below) |
| 1 Hz | `{"type":"system","xruns":…,"generation":…,"sync_errors":…,"cpu_pct":…,"temp_c":…,"sinks":[{"id":…,"present":…,"enabled":…,"playing":…,"layout":…,"latency_ms":…,…}],"sources":[{"id":…,"first":…,"channels":…,"present":…,"capturing":…,"error":…}],…}` |

Spectrum bins are quantised to 0.1 dB on the WS to save bandwidth; the GET gives full float precision.

Binary envelope frame: `u8 type=1`, `u64 first_sample`, `u16 ncols`, then
`ncols × 6 × {i16 min, i16 max}`. One column = 480 frames (200 columns/s at 96 kHz).

### `POST /api/telemetry/inputs`
```json
{"enabled": [true, true, false, false, false, false]}
```
Which inputs the console is watching. Disabled ones are dropped from the spectrum message (the widest
frame). Global, last-writer-wins; resets to all-on at restart.

## System

### `POST /api/config/save`
Writes routing (every sink's included, by device id), generators and channel map to `/data/config.json` — the only
state that survives a reboot. `/data` is remounted read-write for the write, then back. If `/data` did not mount the save
is refused (`data_persistent: false` in `/api/state`).

### `POST /api/config/reset`
Deletes the saved file; the next boot uses the image defaults.

### `POST /api/system/reboot` · `POST /api/system/shutdown`
Answers `{"ok":true}`, then runs `systemctl reboot` / `poweroff`. Disabled in a simulated run.
Shutdown exists because a power cut during a `/data` save can corrupt the card.

### `POST /api/system/inject-kmsg`
Test hook: feed a line to the kmsg watcher to exercise the I2S-sync-error banner.
```sh
curl -X POST http://soundtester.local/api/system/inject-kmsg -d 'bcm2835-i2s: I2S SYNC error!'
```
