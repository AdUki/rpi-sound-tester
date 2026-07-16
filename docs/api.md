# HTTP API

Base URL: `http://soundtester.local` (port 80 on the device, `--port` elsewhere).
All request and response bodies are JSON, except the audio streams.

Inputs are numbered 0–5, outputs 0–7 (the UI shows them as IN 1–6 / OUT 1–8).

## State

### `GET /api/state`
Everything the UI needs to render itself: inputs (gain, meters and tone metrics), outputs
(routing, gain, mute), generator settings, channel map, capture status (including
`analyze_frames`, the configured freeze length), engine stats (rate, format, period, xruns,
generation), system info (CPU, temperature, IP, sync errors) and the `limits` object the
console uses for feature detection and slider ranges.

Each input and output carries a `name`, shown next to its number in the console. Names are
set only by editing `config.json` on the data partition (or the image default) — there is no
API to change them.

## Inputs

### `PUT /api/inputs/{0-5}`
```json
{"gain_db": 12.0}
```
Digital make-up gain for a device whose output is too quiet to read, clamped to **0…+40 dB**
(`limits.input_gain_min_db` / `input_gain_max_db` in `/api/state`).

It is applied on the capture path *before the ring buffer*, so every consumer sees the same
amplified signal: meters, spectrum, THD+N, envelope columns, `/api/capture/window`,
`/api/capture/xcorr`, the listen streams, and any output routed from that input. The gained
sample is clamped to full scale, because the ring's readers all assume |x| ≤ 1 — overdrive
therefore flat-tops the scope and pins the peak meter at 0.0 dBFS instead of wrapping.

Two consequences:

- **Levels are reported post-gain.** With +20 dB on IN 1, a −40 dBFS source meters at −20 dBFS.
  Gain is part of the measurement chain, not a display setting.
- **It cannot undo ADC clipping** — that happened in the codec, upstream of anything software can
  see. This is why attenuation is not offered: it would only hide a clipped signal from the meters.

## Routing and outputs

### `PUT /api/outputs/{0-7}`
```json
{"source": {"type": "input", "index": 3}, "gain_db": -6.0, "mute": false}
{"source": {"type": "gen", "index": "ping"}}
{"source": {"type": "silence"}}
```
`type` is `silence` | `input` | `gen`. For `input`, `index` is 0–5; for `gen` it is
`sine` | `noise` | `ping`. Every field is optional — send only what you want to change.
`gain_db` is clamped to −60…0.

### `POST /api/outputs/{0-7}/identify`
Plays three 100 ms beeps on that output *only*, overriding whatever is routed there, then
reverts by itself. Use it to find out which physical socket a channel really comes out of.

### `PUT /api/channel-map`
```json
{"input_map": [0,1,2,3,4,5], "output_map": [0,1,2,3,4,5,6,7]}
```
`input_map[logical] = physical TDM slot to capture from`, `output_map[logical] = slot to play
into` — the remedy for the Octo's slot rotation (see
[octo-known-issues.md](octo-known-issues.md)). Each map must be a permutation: slots in range
and no duplicates, or the whole request is rejected.

## Generators

All generator timing is derived from the same absolute sample counter that indexes the
capture ring, which is what makes generated audio and captured audio directly comparable.

### `PUT /api/generators/sine`
```json
{"freq_hz": 996.09375, "level_db": -20.0}
```
996.09375 Hz = 85 × 96000 / 8192 is bin-centred for the 8192-point analysis FFT, so it
measures THD+N without windowing leakage inflating the result.

### `PUT /api/generators/noise`
```json
{"mode": "white", "level_db": -20.0}
```
`mode` is `white` | `pink`. The pink filter's coefficients are tuned for 44.1 kHz; at 96 kHz
the slope tilts by a couple of dB across the band.

### `PUT /api/generators/ping`
```json
{"variant": "tick", "interval_s": 2.0, "level_db": -20.0}
```
`variant` is `tick` | `bing` | `bong`. **Use `tick` to measure** — see
[calibration.md](calibration.md). Changing any field reschedules the next emission from the
current sample.

### `GET /api/pings/recent`
```json
[{"sample": 1466240, "variant": "tick"}]
```
The absolute sample index at which each of the last 64 pings was emitted. Successive entries
differ by exactly `round(interval_s × rate)`.

## Capture, scope and delay measurement

### `POST /api/capture/freeze` · `POST /api/capture/resume` · `GET /api/capture/status`
Freeze copies the ring into a snapshot so measurements cannot shift while you work. (The
console's **Analyze** button is this call.)
```json
{"frozen": true, "freeze_sample": 2897920, "valid_start": 1857536, "valid_len": 1040384,
 "generation": 0}
```
`valid_start`/`valid_len` bound the samples you may ask for. `generation` increments on every
xrun: if it changed, the timeline has a discontinuity in it. The status reply additionally
carries `live_now` (the write head's sample index) and `live_oldest` (the oldest live sample
still guaranteed readable).

### `POST /api/capture/config`
```json
{"seconds": 20.0}
```
How many recent frames the next freeze copies — the console's *Analyze buffer*. Body:
`{"seconds": N}` (preferred) or `{"frames": N}`, clamped to
[4096, `limits.capture_max_frames`]. The reply echoes what took effect:
`{"analyze_frames":…, "analyze_seconds":…, "max_frames":…, "max_seconds":…}`. Not persisted:
a restart returns to the 20 s default. Advertised by `limits.capture_config: true`.

### `GET /api/capture/window?ch=&start=&len=&cols=`
`start` and `len` are **absolute sample indices** on the shared counter axis. Returns
min/max pairs per column, or raw samples when `len ≤ 2 × cols`. Serves the frozen snapshot
when frozen, otherwise the live ring (best effort).

### `POST /api/capture/xcorr`
```json
{"ch_a": 0, "ch_b": 1, "start": 386560, "len": 16384}
```
→
```json
{"lag_samples": 137, "lag_ms": 1.4271, "lag_m": 0.4895, "confidence": 4.2, "peak": 0.99}
```
**Requires a freeze** (a live read of half a million frames would race the writer).
`len` ≤ 2^19. A **positive lag means the signal arrives later on `ch_b`**. Both channels are
zero-padded before the FFT, so the correlation is linear, not circular — without that, every
lag would alias modulo the transform size.

Check `confidence` before you believe `lag_samples`: below 2, the window contains something
that repeats and the delay is only known modulo that repeat.

## Listening

### `WS /api/listen/{0-5}`
Binary frames: a little-endian `uint64` absolute starting sample index, followed by the audio.
The index lets a client detect a gap when it falls behind, and — because every channel stamps
the same instant with the same index — is what keeps channels sample-aligned. This is what the
browser uses.

Codec is chosen per connection with a query parameter; the default is raw PCM, so a client that
asks for nothing keeps the original format byte for byte:

- **`?codec=pcm`** (default): the index is followed by 4096 mono **S16_LE** samples at the native
  rate.
- **`?codec=opus`**: the index is followed by one raw **Opus** packet — a 20 ms frame of the
  channel decimated to 48 kHz. Decode it with a stateful Opus decoder (the browser ships one as
  WASM); frames arrive in order over TCP. `?bitrate=<kbps>` overrides the bitrate for this
  connection, otherwise the daemon default applies. Opus is only offered when the engine runs at
  a rate it supports (48 or 96 kHz); see `limits.listen_codecs` in `GET /api/state`.

### `GET /api/stream.ogg`
An endless chunked **Ogg/Opus** stream carrying **all six inputs interleaved** in one container,
read from a single ring cursor so every channel shares one clock — the only way an external
player gets sample-aligned channels (separate per-channel URLs cannot be locked together). The
channels are independent (Opus mapping family 255, uncoupled), so extract them rather than "play"
the file — a player rendering to stereo would surround-downmix them:
```sh
# split the six inputs into per-channel WAVs, preserving their sample alignment
ffmpeg -i http://soundtester.local/api/stream.ogg -filter_complex \
  "channelsplit=channel_layout=6.0" -map '[FL]' in0.wav -map '[FR]' in1.wav …
```
`?bitrate=<kbps>` overrides the per-channel bitrate. Capped at **2** concurrent Ogg streams (each
runs six encoders on one thread). Requires an Opus-capable engine rate.

### `GET /api/inputs/{0-5}/stream.wav`
An endless chunked WAV, for VLC / curl / ffmpeg:
```sh
vlc http://soundtester.local/api/inputs/0/stream.wav
ffplay http://soundtester.local/api/inputs/0/stream.wav
```
The RIFF and data sizes are `0xFFFFFFFF`. FFmpeg (so Chrome) reads that as "unknown length"
and keeps going; VLC ignores a size larger than the stream. Players that *do* trust the size
stop after 4 GiB — about **6.2 hours** at 96 kHz mono. That is a property of the format, not
a bug.

At most **12** listen streams (WS + WAV + Ogg combined) are served at once; further requests get
503.

### `POST /api/listen/codec`
Body: `{"codec":"pcm"|"opus"}` and/or `{"bitrate_kbps":N}`. Sets the daemon's **default** codec
(the console's preference, advertised as `limits.listen_default_codec`) and the per-channel Opus
bitrate, which is clamped, applied live — active Opus streams follow a change — and also used by
`stream.ogg` when no `?bitrate=` is given (`stream.ogg` itself is always Ogg/Opus regardless of
the codec default). The reply echoes what took effect: `{"codec":…,"bitrate_kbps":…}`. This never
changes the WS wire default: a browser still opts into Opus explicitly with `?codec=opus`. Saved
by `POST /api/config/save`.

`GET /api/state` advertises `limits.listen_codecs` (e.g. `["pcm","opus"]`),
`limits.listen_default_codec`, `limits.listen_bitrate_kbps`, `limits.listen_bitrate_min_kbps` /
`_max_kbps`, and `limits.opus_rate` (48000).

## Telemetry

### `WS /api/ws` — push only, no client messages
| rate | message |
|---|---|
| 10 Hz | `{"type":"meters","sample":…,"rms_db":[6],"peak_db":[6]}` |
| 5 Hz | `{"type":"spectrum","channels":[{"ch":0,"bins":[240],"tone":{…}}]}` |
| 10 Hz | **binary** envelope frame (below) |
| 1 Hz | `{"type":"system","xruns":…,"generation":…,"sync_errors":…,"listen_streams":…,"engine_running":…,"cpu_pct":…,"temp_c":…,"mem":{…},"throttle":{…}}` |

Spectrum bins are quantised to 0.1 dB — finer than any display can resolve, and it keeps one
spectrum message near 10 kB. Raw floats serialise each bin to full precision
(`-88.61194610595703`) and cost 27.9 kB per message.

Binary envelope frame: `u8 type=1`, `u64 first_sample`, `u16 ncols`, then
`ncols × 6 × {i16 min, i16 max}`. One column is 480 frames (200 columns/s at 96 kHz).

The envelope frame rate is 10 Hz because that is the rate at which the analysis thread
*produces* envelope columns (~20 per tick). Frame rate is only a packing choice — the scope's
fidelity comes from the 200 columns/s. Pushing at 15 Hz finds an empty ring on a third of its
ticks and sends nothing.

### `POST /api/telemetry/inputs`
Body: `{"enabled":[bool ×6]}`. Tells the daemon which inputs the console is watching; inputs set
`false` are **dropped from the spectrum message**, the widest frame on the wire, so a console
showing two of six inputs pulls roughly a third of the spectrum bandwidth. The meters message and
the binary envelope frame keep their fixed six-channel shape for compatibility with any console —
the console hides disabled inputs on its own regardless.

The mask is process-global and last-writer-wins: this is a single-operator bench, so two consoles
disagreeing is out of scope. It resets to all-enabled on daemon restart; the console re-posts it on
every WebSocket (re)connect. Support is advertised by `limits.telemetry_mask: true` in
`GET /api/state`; a console that does not see the flag simply skips the POST and disables inputs
client-side only.

## System

### `POST /api/config/save`
Writes the current routing, generators and channel map to `/data/config.json` — the only
thing that survives a reboot. The data partition is remounted read-write for the duration of
the write and back to read-only afterwards.

### `POST /api/config/reset`
Removes the saved file; the next boot uses the image defaults.

### `POST /api/system/reboot`
Answers `{"ok":true}` first, then runs `systemctl reboot` — the browser needs the response
before the socket dies.

### `POST /api/system/shutdown`
Same shape as reboot, running `systemctl poweroff`. The rootfs is read-only, so pulling the
plug is *usually* survivable — but `/data` is briefly writable during a save, and a hard power
cut inside that window can corrupt the SD card, which is why a clean shutdown exists at all.

### `POST /api/system/inject-kmsg`
Test hook: feeds a line to the kmsg watcher, so the I2S-sync-error banner can be exercised
without provoking a real sync error.
```sh
curl -X POST http://soundtester.local/api/system/inject-kmsg -d 'bcm2835-i2s: I2S SYNC error!'
```
