# HTTP API

Base URL: `http://soundtester.local` (port 80 on the device; `--port` elsewhere). All bodies are
JSON except the audio streams. Inputs are 0–11, outputs 0–7 (the UI labels them IN 1–6, NET 1–6
and OUT 1–8). Inputs 0–5 are the card's ADCs; 6–11 are **network inputs** fed over the LAN, and
everything that takes an input index takes those too. `GET /api/state` reports each input's `kind`
as `local` or `net`, and `limits.inputs_local` says where the split is. Each input also carries
`active`: a network channel reads `false` until a sender has used it, which is how the console
keeps unused ones out of sight. The channel exists either way — the index is fixed, and the arrays
in the meters, spectrum and envelope messages always carry every channel — so a headless client
can ignore `active` entirely.

Every PUT field is optional — send only what changes. Out-of-range numbers clamp to their limits;
bad enums and non-permutation maps are rejected.

### `GET /api`
This document, rendered to HTML (built from `api.md`). Also served as the static `/api.html`.

## State

### `GET /api/state`
The whole device state in one object: `inputs`, `outputs`, `generators`, `channel_map`, `capture`,
`engine`, `system`, and `limits` (slider ranges and feature flags the console reads). Each input and
output has a `name`, set only in `config.json` — there is no API to change it.

## Inputs

### `PUT /api/inputs/{0-11}`
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
```json
{"source": {"type": "input", "index": 3}, "gain_db": -6.0, "mute": false}
{"source": {"type": "gen", "index": "ping"}}
{"source": {"type": "silence"}}
```
`type` is `silence` | `input` | `gen`. `index` is 0–11 for `input` — a network channel routes like
any other — or `sine` | `noise` | `ping` for `gen`. `gain_db` clamps to −60…0.

### `POST /api/outputs/{0-7}/identify`
Three 100 ms beeps on that output only, then it reverts. Tells you which physical socket it is.

### `PUT /api/channel-map`
```json
{"input_map": [0,1,2,3,4,5], "output_map": [0,1,2,3,4,5,6,7]}
```
`input_map[logical]` = the TDM slot to capture from; `output_map[logical]` = the slot to play into.
This corrects the Octo's slot rotation. Each map must be a permutation (in range, no duplicates) or
the request is rejected.

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
Changing the port rebinds the listener and drops any connected sender. A bind that fails still
answers 200 — check `listening` and `error`.

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
| 1 Hz | `{"type":"system","xruns":…,"generation":…,"sync_errors":…,"cpu_pct":…,"temp_c":…,…}` |

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
Writes routing, generators and channel map to `/data/config.json` — the only state that survives a
reboot. `/data` is remounted read-write for the write, then back. If `/data` did not mount the save
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
