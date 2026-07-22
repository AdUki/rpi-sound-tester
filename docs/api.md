# HTTP API

Base URL: `http://soundtester.local` (port 80 on the device; `--port` elsewhere). All bodies are
JSON except the audio streams. Inputs are 0–5, outputs 0–7 (the UI labels them IN 1–6 / OUT 1–8).

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

### `PUT /api/inputs/{0-5}`
```json
{"gain_db": 12.0}
```
Make-up gain, 0…+40 dB, applied on capture **before the ring buffer** — so every reading (meters,
spectrum, THD+N, scope, xcorr, listen streams) is post-gain. It cannot undo ADC clipping, which
already happened in the codec; that is why there is no attenuation.

## Routing and outputs

### `PUT /api/outputs/{0-7}`
```json
{"source": {"type": "input", "index": 3}, "gain_db": -6.0, "mute": false}
{"source": {"type": "gen", "index": "ping"}}
{"source": {"type": "silence"}}
```
`type` is `silence` | `input` | `gen`. `index` is 0–5 for `input`, or `sine` | `noise` | `ping` for
`gen`. `gain_db` clamps to −60…0.

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
