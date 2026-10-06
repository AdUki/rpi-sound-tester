# RPi Sound Tester

A read-only appliance for testing audio gear on the bench. Two boards are supported:

- **Raspberry Pi 3** with the **Audio Injector Octo** (Cirrus CS42448, 6 in / 8 out, 96 kHz /
  S32_LE), whose clock everything follows;
- **Khadas VIM3L** with no card of its own: HDMI out, the HDMI loopback and USB audio, at 48 kHz
  on a timer clock.

Plug a device in, open `http://soundtester.local`, and you get:

- **Inputs:** the Octo's 6 ADCs, 6 **network inputs** that any Linux machine on the LAN plays
  into through an ALSA plugin, and the capture side of USB interfaces (and the VIM3L's HDMI
  loopback), found when they are plugged in. Listen to any channel in the browser — send each one
  to the left ear, the right, both or neither, and channels sharing an ear are mixed — with
  per-channel level/peak meters, spectrum, THD+N, a scope you can freeze, and up to +40 dB of
  digital make-up gain for a device too quiet to read.
- **Outputs:** the Octo's 8 DACs, and every other playback device as a **sink**, found at runtime:
  HDMI (mono up to 7.1), the Pi's 3.5 mm jack, a USB interface, a Bluetooth speaker. Route any
  input to any output; sine, white/pink noise, tick/bing/bong pings, and a short looping melody.
  Every sink plays on the same sample axis, so a TV or AV receiver's latency can be measured like
  a DAC's.
- **Bluetooth (A2DP, both ways, Pi 3):** scan, pair and connect from the browser, with nothing to
  confirm. A paired speaker is one more output (its buttons pick the test signal), and a phone
  playing to the tester lands on a pair of inputs on the same sample axis. Pairings survive reboots.
- **Multiroom sync measurement:** freeze the capture, bracket a ping, and get the delay
  between two inputs **to the sample** — with a confidence number that tells you when not to
  trust it.

Everything hangs off one clock. On the Pi it is the Octo's: capture and playback are
`snd_pcm_link()`ed, and the card's FPGA is the master clock for both the codec and the Pi. On the
VIM3L it is a timer. Every generator is driven from the same absolute sample counter that indexes
the capture ring, and every other device — a sink, a USB input, a network sender, a phone —
follows that clock through a sample-rate converter. A sample index means the same instant on
every channel.

> ### Read this before buying/wiring anything
> The Octo produces **only distorted noise on every 6.x kernel**. This image pins **5.15.92**,
> which is the last version known to work. **The Pi 5 cannot work at all**; the image is built
> and tested for the Pi 3 only. The evidence was collected in `docs/octo-known-issues.md`, since
> dropped from the tree (`git show 13ea712^:docs/octo-known-issues.md`); milestone 0 is to
> confirm it on your own card.

## Try it without hardware

The simulator loops every output back into the matching input with a delay you choose, so the
entire chain — generators, routing, ring buffer, scope, cross-correlation, listening — works
on a laptop.

```sh
sudo apt install libasound2-dev libopus-dev libogg-dev libsamplerate0-dev libvorbis-dev libsystemd-dev
git clone --recurse-submodules <url>   # the header-only libraries are submodules; --init works after the fact
make            # list every target
make test       # the unit tests in app/tests
make run        # http://localhost:8080, simulated card
make run BT=fake          # ...with a scripted fake BlueZ, to try pairing without a radio
make run SINK=default     # ...and the desktop's speakers as one more output
make run BOARD=vim3l      # ...with the VIM3L's profile (48 kHz; still a simulated Octo)
```

Then: route the ping generator to OUT 1/2/3, go to **Scope & sync**, press **Analyze**,
bracket a ping with the cursors and press **Measure** — IN 1→IN 2 reads exactly 137 samples,
IN 1→IN 3 exactly 274.

To feed it from another machine, build the ALSA plugin there (`make plugin plugin-install`) and
`aplay -D soundtester:<host> file.wav`; it lands on a network input. The plugin is documented in
[docs/api.md](docs/api.md), *Network inputs*.

## Run it on a PC

The same daemon also runs for real on a Linux desktop, with no Yocto and no simulated card: built
with the host's compiler, paced by a timer, playing to and recording from the desktop's PipeWire.

```sh
make pc-deps              # the -dev packages (Debian/Ubuntu), once
make pc                   # http://localhost:8080
make pc BT=hci1           # ...with Bluetooth on adapter hci1 (or its address)
make pc BT=ask            # ...asking which; make pc-bt-adapters lists them
make pc BT=fake           # ...with the fake BlueZ (two adapters), no radio needed
make pc INPUT=hw:2,0      # ...also recording from a card PipeWire does not run (SINK= to play to one)
```

PipeWire is not touched. The daemon is one more ALSA client of it (its `pipewire` PCM), listed as
*PipeWire ALSA [soundtesterd]* in pavucontrol or `wpctl status`, where its output and input can be
moved to any device; it never opens a sound card PipeWire runs (`app/config/boards/pc.json` has
`"scan": false`). Recording from PipeWire's default source means the microphone is in use while it
runs (`tools/pc/run --no-input` leaves it alone). Settings save to `~/.local/state/soundtester`;
the console's reboot and shutdown buttons do nothing on a PC.

**Bluetooth** wants an adapter the desktop is not using, e.g. a USB dongle next to the built-in
radio. Set it up once with `make pc-bt-setup`: it unpacks bluez-alsa under
`~/.local/share/soundtester` (apt-get download — nothing installed, no service enabled) and, with
sudo, adds one D-Bus policy, `/etc/dbus-1/system.d/soundtester-bluealsa.conf`, so that you can run
it (`make pc-bt-remove` takes both out). `make pc BT=hci1` then starts bluez-alsa for that adapter
alone and stops it with the daemon; the console's Bluetooth tab pairs and connects on hci1 only.
Two things still clash with PipeWire, which registers its own audio endpoints on every adapter:
a device that connects to hci1 may end up with PipeWire rather than the tester (disconnect it in
the desktop's Bluetooth settings and connect it from the console again), and while the tester runs
it is BlueZ's default pairing agent, so it refuses pairings a device starts towards the desktop's
adapter (pairing from the desktop's own Bluetooth settings still works).

## Build the image

`BOARD=` picks the hardware: `rpi3` (Raspberry Pi 3 with the Octo, the default) or `vim3l` (Khadas
VIM3L, built without Wi-Fi and Bluetooth). What the board means for the build is in
`yocto/boards/<board>.mk` and `yocto/conf/boards/<board>.conf`; each board builds in its own
`yocto/build-<board>/`, sharing downloads and sstate.

Everything you would normally want to change for a bench lives in one file:
**`yocto/meta-soundtester/conf/soundtester-device.conf`** — hostname, root password, SSH, Wi-Fi
SSID/PSK, Bluetooth and ports. The rootfs is read-only, so these are baked in at build time. (The
sample rate and period are the board's, in its board conf.)

That file is not in the repo — it carries a root password and a Wi-Fi PSK in the clear, and git
history keeps whatever it is given. `make configure` creates it from the tracked `.sample` next
to it and asks for the values; `make image` refuses to run until it exists.

```sh
make configure        # hostname, root password, Wi-Fi, cache dirs
make host-deps        # the Yocto host packages (Debian/Ubuntu), once
make image            # clones the layers, then builds (hours, the first time)
make image DEV=1      # writable image with alsa-utils + ssh, for bring-up
make image BOARD=vim3l          # the same for another board
make flash            # lists the disks it could write to
make flash DISK=/dev/mmcblk0   # shows what it will erase, then asks before writing
```

`flash` refuses the disk this system is running from and anything carrying a mounted system
directory; it warns if the target is not flagged removable (normal for a card in a built-in
reader, but also what an internal drive looks like). Add `DEV=1` to flash the dev image.

Plain poky and bitbake — no kas, no pip. The first `make image` clones poky,
meta-openembedded and the board's BSP layer, and generates the build dir's conf files on its
own; `make bitbake` with no ARGS drops you into the usual bitbake environment if you want to poke
at it by hand, and `make bitbake ARGS="soundtesterd"` cross-builds just the daemon.

A board that is already running takes a new console or daemon over ssh, no reflash:

```sh
make deploy-www                    # app/www only; the next page load picks it up
make bitbake ARGS="soundtesterd" && make deploy-daemon   # the daemon and its /etc files; restarts it
```

Both take `BOARD=` and `TARGET=root@host`.

Two images:

| | rootfs | ssh | tools |
|---|---|---|---|
| `soundtester-image` | **read-only** | yes, unless `SOUNDTESTER_ENABLE_SSH = "0"` (password from the conf file) | none |
| `soundtester-image-dev` | writable | yes + package management | `alsa-utils`, `i2c-tools`, `strace`, `htop` |

Use the **dev** image when you need `alsa-utils` and friends on the box to poke at the card by
hand (`aplay -l`, `speaker-test`, `arecord`). The production image is what ships; with ssh on,
`journalctl -u soundtesterd -f` on the device is the usual way to watch the daemon.

Settings changed in the web UI live in RAM: the device always boots into a known state.
**Configuration → Save as boot defaults** writes them to a small ext4 partition (briefly
remounted read-write), which is also where the SSH host keys live so they survive a reboot.

## How it fits together

One C++17 daemon, `soundtesterd`. No scripting runtime, no GStreamer, no audio framework. The
linked libraries are alsa-lib, libopus + libogg (the encoded "listen" streams), libvorbis (a
network sender that compresses), libsamplerate (the converters every other clock goes through) and
libsystemd (sd-bus, for BlueZ); everything else is header-only, pinned as a git submodule under
`app/third_party/`, and nothing but the include path points at it.

```
Octo 6-in ──ALSA──▶ AUDIO THREAD (SCHED_FIFO 80, mlocked ring)    or, with no card, a timer
                    read 8ch → remap → ring buffer (float32, ~87 s) + sample counter n
                    generators driven by n, routed per output and per sink channel
Octo 8-out ◀─ALSA── remap → write 8ch      (streams snd_pcm_link'ed: one clock, one start)
                    │
  network senders ──┼─▶ NET AUDIO: each packet placed at the sample it asks for (ASRC)
  phone (A2DP) ─────┤   BLUETOOTH: BlueZ over sd-bus; a phone is one more sender
  USB / HDMI in ────┼─▶ DEVICES: scanned every 2 s; each capture device placed at the sample
                    │            it was captured at (ASRC)
  HDMI / jack / USB ◀── SINKS: one thread per playback device, latency held constant (ASRC)
  BT speaker ◀──────┘
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
  src/          engine, generators, analysis, capture, devices and sinks, network input,
                Bluetooth, web server
  config/       factory defaults; boards/<profile>.json, the per-board data
  tests/        ctest: plain executables, one per area (make test)
  third_party/  submodules: cpp-httplib, pocketfft, nlohmann/json, spdlog, CLI11
                (header-only, pinned at a tag — pocketfft at a commit, it has no tags)
alsa-plugin/    the sender: an ALSA PCM + mixer plugin for any Linux machine
tools/          md2html (renders docs/api.md for GET /api), fake-bluez (BlueZ for make run BT=fake),
                pc (make pc: run, Bluetooth setup and adapter list on a desktop)
yocto/
  meta-soundtester/   layer: images, app recipe, wic layouts; BSP-specific parts (the pinned
                      Pi kernel) under dynamic-layers/<bsp>/
    conf/soundtester-device.conf   <- hostname, password, ssh, Wi-Fi
  boards/             <board>.mk: MACHINE, BSP layer, daemon profile per BOARD=
  conf/               local.conf / bblayers.conf templates; boards/<board>.conf
  layers/             poky + meta-openembedded + BSP layers (cloned, gitignored)
docs/           api.md
```

## Documentation

- **[docs/api.md](docs/api.md)** — the HTTP/WebSocket API and the ALSA plugin, and the single
  reference since 13ea712. The daemon serves it rendered at `/api`. The companion docs it replaced
  (calibration, the Octo's known issues, the bench checklist) are in git history: `git show
  13ea712^:docs/<name>.md`.
