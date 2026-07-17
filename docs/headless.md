# Headless quickref

Everything the web console does is HTTP, so a script can drive the device with nothing but
`curl`. This is the cheat-sheet for the four things people actually want from a shell — *which
channel has sound, what does it look like, what's its spectrum, how far apart are two inputs* —
with the one-liners that answer them. The full contract is in [api.md](api.md); this page is the
short path.

Base URL is `http://soundtester.local` on the device (port 80), or `http://localhost:8080` under
`make run`. Examples assume that `$DEV` holds it and that `jq` is around:

```sh
DEV=http://soundtester.local
```

Two facts that make the numbers mean something, both a consequence of the [one-clock
design](calibration.md):

- **Every reading is post input-gain.** `PUT /api/inputs/{ch} {"gain_db":…}` sits *before* the
  capture ring, so meters, spectrum, wave and delay all see the amplified signal. Gain is part of
  the measurement, not a display knob.
- **One sample index means the same instant on every channel.** `start`/`len` everywhere below are
  absolute indices on that shared counter, which is why two inputs can be compared to the sample.

## Which channel has sound

`GET /api/meters` is the 10 Hz console meter as a plain GET — no WebSocket needed. `rms_db` is a
100 ms window; `peak_db` carries a 3 s hold. Silence sits near −120 dB (`kMinDb`).

```sh
curl -s $DEV/api/meters | jq
# {"type":"meters","sample":1488896,"rms_db":[-9.0,-95.3,…],"peak_db":[-6.0,-90.5,…]}

# just the inputs currently above −60 dBFS RMS:
curl -s $DEV/api/meters | jq -r '.rms_db | to_entries[] | select(.value > -60) | "IN\(.key+1): \(.value|floor) dBFS"'
```

`GET /api/state` carries the same `rms_db`/`peak_db` per input plus each input's `tone`
(`freq_hz`, `thd_n_pct`) and everything else — routing, generators, engine, host health. Reach for
`/api/meters` when you only want levels and want them cheap; `/api/state` when you want the whole
picture in one shot.

## Get spectrum data

`GET /api/spectrum` is the 5 Hz console analyser as a GET. 240 log-spaced bins per input from
20 Hz to min(Nyquist, 40 kHz), in dBFS, at full float precision (the WebSocket feed quantises to
0.1 dB to save bandwidth — this one-shot does not). `bins_hz` gives the center frequency of each
bin so you never have to reconstruct the log axis, and `tone` is the dominant-partial detector
(`freq_hz` sub-bin interpolated, `thd_n_pct` in-band, `valid:false` below −40 dBFS).

```sh
curl -s "$DEV/api/spectrum?ch=0" | jq '.channels[0].tone'
# {"valid":true,"freq_hz":996.09,"thd_n_pct":0.0032}

# peak bin -> its frequency, for input 0:
curl -s "$DEV/api/spectrum?ch=0" | jq -r '
  .bins_hz as $hz | .channels[0].bins_db
  | (to_entries | max_by(.value)) as $p
  | "peak \($hz[$p.key]|floor) Hz @ \($p.value|floor) dBFS"'
```

Omit `?ch=` for all six inputs (`bins_hz` is shared; `channels` is an array). `?ch=0..5` returns
just that one.

## Get wave data (the scope)

`GET /api/capture/window?ch=&start=&len=&cols=` returns the envelope of a stretch of one input.
It reduces `len` samples into `cols` columns of `{min,max}` pairs — or, when `len ≤ 2×cols`, the
raw samples themselves (`"raw":true`, one value per point). It serves the frozen snapshot if you
have frozen, otherwise best-effort off the live ring.

```sh
# last 1 s of IN 1 as 500 min/max columns, straight off the live ring
now=$(curl -s $DEV/api/capture/status | jq .live_now)
curl -s "$DEV/api/capture/window?ch=0&start=$((now-96000))&len=96000&cols=500" | jq '{start,len,raw,n:(.min|length)}'

# raw samples around one instant (len small enough to be un-reduced)
curl -s "$DEV/api/capture/window?ch=0&start=$((now-256))&len=256&cols=256" | jq '.samples | length'
```

For a measurement you can trust, freeze first (next section) so the window can't shift under you.

## Measure delay between two inputs

The device's whole reason to exist. Route a **`tick`** ping (broadband → sharp correlation peak),
feed it into two inputs, freeze, and cross-correlate a window bracketing **one** ping. Full
background in [calibration.md](calibration.md); the scripted version:

```sh
# 1. play a tick out of the outputs under test
curl -s -X PUT $DEV/api/outputs/0 -d '{"source":{"type":"gen","index":"ping"}}'
curl -s -X PUT $DEV/api/outputs/1 -d '{"source":{"type":"gen","index":"ping"}}'
curl -s -X PUT $DEV/api/generators/ping -d '{"variant":"tick","interval_s":2.0,"level_db":-6}'

# 2. freeze the ring so measurements can't move
fr=$(curl -s -X POST $DEV/api/capture/freeze)   # -> {"valid_start":…,"valid_len":…}

# 3. find one ping's emission sample inside the frozen range, bracket ±8192 around it
start=$(curl -s $DEV/api/pings/recent | jq --argjson f "$fr" '
  [ .[].sample | select(. > ($f.valid_start+8192) and . < ($f.valid_start+$f.valid_len-8192)) ]
  | last - 4096')

# 4. cross-correlate the two inputs over that window
curl -s -X POST $DEV/api/capture/xcorr \
  -d "{\"ch_a\":0,\"ch_b\":1,\"start\":$start,\"len\":16384}"
# -> {"lag_samples":137,"lag_ms":1.427,"lag_m":0.489,"confidence":42.0,"peak":0.99}

curl -s -X POST $DEV/api/capture/resume   # back to live
```

Two things to get right, or the answer is a plausible lie:

- **Bracket exactly one ping.** More than one in the window and the delay is only known *modulo the
  ping interval* — the correlation finds a rival peak one interval away. This is what `pings/recent`
  is for: it lists the absolute emission sample of the last 64 pings, so you window around one.
- **Read `confidence`, not just `lag_samples`.** It's the winning peak over the tallest genuinely
  separate rival. `> 3` → trust it; `< 2` → ambiguous (too many pings in the window, or you used a
  continuous tone — the sine's delay is only known modulo its carrier period; use a ping).

Sign: **a positive `lag_samples` means the signal arrives *later* on `ch_b`.** `lag_m` is the
acoustic distance (`lag_ms × 0.343`), meaningful only for an air path, not a cable. Comparing two
*inputs* cancels the constant playback→ADC loopback offset; you only subtract
`loopback_offset_samples` when comparing an output event against an input arrival.

`xcorr` **requires a freeze** (a live read of up to half a million frames would race the writer)
and `len ≤ 2^19`.

## Grab the raw audio

When you want the samples themselves rather than a measurement:

```sh
# endless mono WAV of one input (ffmpeg/VLC/curl)
ffplay $DEV/api/inputs/0/stream.wav

# all six inputs, sample-aligned, in one Ogg/Opus container — split them out:
ffmpeg -i $DEV/api/stream.ogg -filter_complex "channelsplit=channel_layout=6.0" \
  -map '[FL]' in0.wav -map '[FR]' in1.wav …
```

Both keep every channel on the shared clock. `stream.ogg` needs an Opus-capable engine rate
(48/96 kHz); see [api.md](api.md#listening) for the WebSocket listen path and codec options.

## Play a stimulus without the UI

Routing is `PUT /api/outputs/{0-7} {"source":{"type":…}}` where `type` is `silence` | `input` |
`gen` (`index` = input `0-5`, or generator `sine` | `noise` | `ping`). The generators are
`PUT /api/generators/{sine,noise,ping}`.

```sh
# 996.09 Hz (bin-centred for the analysis FFT — no leakage in THD+N) out of OUT 1
curl -s -X PUT $DEV/api/generators/sine -d '{"freq_hz":996.09375,"level_db":-20}'
curl -s -X PUT $DEV/api/outputs/0 -d '{"source":{"type":"gen","index":"sine"},"gain_db":0}'

# which physical socket is OUT 3? three beeps on it only, then it reverts:
curl -s -X POST $DEV/api/outputs/2/identify
```

Changes live in RAM. `POST /api/config/save` writes routing, generators and channel map to
`/data` so they survive a reboot; `POST /api/config/reset` drops back to the image defaults.

## The endpoints on one screen

| want | call |
|---|---|
| levels per input | `GET /api/meters` |
| full state (levels + tone + routing + engine + host) | `GET /api/state` |
| spectrum + bin frequencies + tone | `GET /api/spectrum[?ch=0..5]` |
| scope envelope / raw samples | `GET /api/capture/window?ch=&start=&len=&cols=` |
| freeze / resume / status | `POST /api/capture/{freeze,resume}` · `GET /api/capture/status` |
| ping emission samples | `GET /api/pings/recent` |
| delay between two inputs | `POST /api/capture/xcorr` (freeze first) |
| route a source to an output | `PUT /api/outputs/{0-7}` |
| set a generator | `PUT /api/generators/{sine,noise,ping}` |
| raw audio | `GET /api/inputs/{0-5}/stream.wav` · `GET /api/stream.ogg` |
| persist / reset | `POST /api/config/{save,reset}` |
