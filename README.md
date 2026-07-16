# RPi Sound Tester

A read-only Raspberry Pi appliance for testing audio gear on the bench, built around the
**Audio Injector Octo** (Cirrus CS42448, 6 in / 8 out, 96 kHz / S32_LE).

Plug a device into the sound card, open `http://soundtester.local`, and you get:

- **Inputs (6):** listen to any channel in the browser — send each one to the left ear, the
  right, both or neither, and channels sharing an ear are mixed — per-channel level/peak meters,
  spectrum, THD+N, a 6-lane scope you can freeze, and up to +40 dB of digital make-up gain for
  a device too quiet to read.
- **Outputs (8):** route any input to any output; sine, white/pink noise, and tick/bing/bong
  pings.
- **Multiroom sync measurement:** freeze the capture, bracket a ping, and get the delay
  between two inputs **to the sample** — with a confidence number that tells you when not to
  trust it.

Everything hangs off one clock: capture and playback are `snd_pcm_link()`ed on one card, the
card's FPGA is the master clock for both the codec and the Pi, and every generator is driven
from the same absolute sample counter that indexes the capture ring. A sample index means the
same instant on every channel.

> ### Read this before buying/wiring anything
> The Octo produces **only distorted noise on every 6.x kernel**. This image pins **5.15.92**,
> which is the last version known to work. **The Pi 5 cannot work at all** — supported boards
> are the Pi 2 / 3 / 4. The full evidence is in
> [docs/octo-known-issues.md](docs/octo-known-issues.md), and milestone 0 is to confirm it on
> your own card.

## Try it without hardware

The simulator loops every output back into the matching input with a delay you choose, so the
entire chain — generators, routing, ring buffer, scope, cross-correlation, listening — works
on a laptop.

```sh
sudo apt install libopus-dev libogg-dev   # the daemon links these (plus libasound2-dev)
git clone --recurse-submodules <url>   # the header-only libraries are submodules; --init works after the fact
make            # list every target
make test       # generators, ring buffer, xcorr, wav, config, opus, dsp
make run        # http://localhost:8080, simulated card
```

Then: route the ping generator to OUT 1/2/3, go to **Scope & sync**, press **Analyze**,
bracket a ping with the cursors and press **Measure** — IN 1→IN 2 reads exactly 137 samples,
IN 1→IN 3 exactly 274.

## Build the image

Everything you would normally want to change lives in one file:
**`yocto/meta-soundtester/conf/soundtester-device.conf`** — hostname, root password, SSH,
Wi-Fi SSID/PSK, HTTP port, sample rate and period. The rootfs is read-only, so these are
baked in at build time.

That file is not in the repo — it carries a root password and a Wi-Fi PSK in the clear, and git
history keeps whatever it is given. `make configure` creates it from the tracked `.sample` next
to it and asks for the values; `make image` refuses to run until it exists.

```sh
make configure        # hostname, root password, Wi-Fi, cache dirs
make host-deps        # the Yocto host packages (Debian/Ubuntu), once
make image            # clones the layers, then builds (hours, the first time)
make image DEV=1      # writable image with alsa-utils + ssh, for bring-up
make flash            # lists the disks it could write to
make flash DISK=/dev/mmcblk0   # shows what it will erase, then asks before writing
```

`flash` refuses the disk this system is running from and anything carrying a mounted system
directory; it warns if the target is not flagged removable (normal for a card in a built-in
reader, but also what an internal drive looks like). Add `DEV=1` to flash the dev image.

Plain poky and bitbake — no kas, no pip. The first `make image` clones the three layers and
generates the two conf files (from `yocto/conf/*.sample`) on its own; `make bitbake` with no
ARGS drops you into the usual bitbake environment if you want to poke at it by hand.

Two images:

| | rootfs | ssh | tools |
|---|---|---|---|
| `soundtester-image` | **read-only** | yes (password from the conf file) | none |
| `soundtester-image-dev` | writable | yes + package management | `alsa-utils`, `i2c-tools`, `strace`, `htop` |

Use the **dev** image when you need `alsa-utils` and friends on the box to poke at the card by
hand (`aplay -l`, `speaker-test`, `arecord`). The production image is what ships; both have ssh,
so `journalctl -u soundtesterd -f` on the device is the usual way to watch the daemon.

Settings changed in the web UI live in RAM: the device always boots into a known state.
**Configuration → Save as boot defaults** writes them to a small ext4 partition (briefly
remounted read-write), which is also where the SSH host keys live so they survive a reboot.

## How it fits together

One C++17 daemon, `soundtesterd`. No scripting runtime, no GStreamer, no audio framework. The
linked libraries are alsa-lib plus libopus + libogg (only the encoded "listen" stream uses those
two); everything else is header-only, pinned as a git submodule under `app/third_party/`, and
nothing but the include path points at it.

```
Octo 6-in ──ALSA──▶ AUDIO THREAD (SCHED_FIFO 80, mlocked ring)
                    read 8ch → remap → ring buffer (float32, ~87 s) + sample counter n
                    generators driven by n, routed per output
Octo 8-out ◀─ALSA── remap → write 8ch      (streams snd_pcm_link'ed: one clock, one start)
                    ├── ANALYSIS (10 Hz): meters, 8192-pt FFT, THD+N, scope columns
                    ├── CAPTURE: freeze snapshot, window, zero-padded FFT cross-correlation
                    └── WEB (cpp-httplib): REST + WebSocket push + live audio streams
```

| what | choice |
|---|---|
| HTTP + WebSocket | [cpp-httplib](https://github.com/yhirose/cpp-httplib) — small, and its handlers may *block*, which is what an hours-long audio stream needs |
| FFT | [pocketfft](https://github.com/mreineck/pocketfft) — the transform inside NumPy/SciPy |
| audio | raw **alsa-lib**. Every abstraction (miniaudio, RtAudio, PortAudio) either converts formats behind your back or cannot express a linked duplex start |
| JSON / logging / CLI | nlohmann-json, spdlog, CLI11 |

The concurrency rules are documented in the source: a single-writer ring with
**seqlock-style post-validation** on every read (a pre-check alone would let a stalled reader
hand back silently stitched-together audio), and compound control values packed into one
atomic (a torn `{type, index}` would index out of bounds in the audio thread).

## Layout

```
app/            C++17 daemon + vanilla-JS web console (no build step)
  src/          engine, generators, analysis, capture, web server
  tests/        ctest: ping spacing, ring seqlock under a concurrent writer,
                xcorr known-lag recovery, WAV header, config round-trip,
                Opus cross-channel alignment, dsp conversions
  third_party/  submodules: cpp-httplib, pocketfft, nlohmann/json, spdlog, CLI11
                (header-only, pinned at a tag — pocketfft at a commit, it has no tags)
yocto/
  meta-soundtester/   layer: images, app recipe, pinned kernel, wic layout
    conf/soundtester-device.conf   <- hostname, password, ssh, Wi-Fi
  conf/               local.conf / bblayers.conf templates
  layers/             poky + meta-openembedded + meta-raspberrypi (cloned, gitignored)
docs/           api.md · calibration.md · octo-known-issues.md · bench-tests.md
```

## Documentation

- **[docs/calibration.md](docs/calibration.md)** — how to get a delay number you can trust,
  and why `tick` is the only variant worth measuring with.
- **[docs/octo-known-issues.md](docs/octo-known-issues.md)** — the 6.x kernel breakage, the
  TDM slot-rotation bug, and what the software does about them.
- **[docs/api.md](docs/api.md)** — the HTTP/WebSocket API.
- **[docs/bench-tests.md](docs/bench-tests.md)** — the acceptance checklist for a finished
  device.
