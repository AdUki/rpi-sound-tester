# RPi Sound Tester

Read-only appliance for bench-testing audio gear, at `http://soundtester.local`. Boards:

- **Raspberry Pi 3** + **Audio Injector Octo** (Cirrus CS42448, 6 in / 8 out, 96 kHz / S32_LE);
- **Khadas VIM3L**, no card: HDMI out, HDMI loopback, USB audio; 48 kHz on a timer clock.

Features:

- **Inputs:** 6 Octo ADCs; 6 **network inputs** (any LAN Linux machine, via an ALSA plugin); USB
  capture (and the VIM3L's HDMI loopback) on plug-in. Listen in the browser, each channel to left,
  right, both or neither ear (shared ears mix). Level/peak meters, spectrum, THD+N, freezable
  scope, up to +40 dB make-up gain.
- **Outputs:** 8 Octo DACs, plus every other playback device as a runtime **sink**: HDMI (mono to
  7.1), the Pi's 3.5 mm jack, USB, Bluetooth speaker. Any input to any output; sine, white/pink
  noise, tick/bing/bong pings, a looping melody. Sinks share the sample axis, so TV/AVR latency
  measures like a DAC's.
- **Bluetooth (A2DP both ways, Pi 3):** scan/pair/connect from the browser, nothing to confirm. A
  speaker is an output (its buttons pick the signal); a phone lands on an input pair. Pairings
  survive reboots.
- **Multiroom sync:** freeze, bracket a ping, read the delay between two inputs **to the sample**,
  with a confidence number.

One clock: the Octo's (capture + playback `snd_pcm_link()`ed; its FPGA clocks codec and Pi), or a
timer on the VIM3L. Generators and the capture ring share one sample counter; every other device
follows via a sample-rate converter, so a sample index is the same instant on every channel.

> ### Read this before buying/wiring anything
> The Octo gives **only distorted noise on every 6.x kernel**; the image pins **5.15.92**, the last
> that works. **The Pi 5 cannot work at all**; only the Pi 3 is built and tested. Evidence: `git
> show 13ea712^:docs/octo-known-issues.md`. Milestone 0: confirm it on your card.

## Try it without hardware

The simulator loops each output into the matching input with a set delay; the whole chain runs on
a laptop.

```sh
sudo apt install libasound2-dev libopus-dev libogg-dev libsamplerate0-dev libvorbis-dev libsystemd-dev
git clone --recurse-submodules <url>   # or later: git submodule update --init
make                      # list targets
make test                 # unit tests
make run                  # http://localhost:8080, simulated card
make run BT=fake          # + fake BlueZ (pairing without a radio)
make run SINK=default     # + desktop speakers as an output
make run BOARD=vim3l      # VIM3L profile (48 kHz, simulated Octo)
```

Check: ping generator → OUT 1/2/3, **Scope & sync** → **Analyze**, bracket a ping, **Measure**:
IN 1→IN 2 = 137 samples, IN 1→IN 3 = 274.

Network input from another machine: `make plugin plugin-install` there, then `aplay -D
soundtester:<host> file.wav`. See [docs/api.md](docs/api.md), *Network inputs*.

## Run it on a PC

The real daemon on a Linux desktop: no Yocto, host compiler, timer clock, audio via PipeWire.

```sh
make pc-deps              # -dev packages (Debian/Ubuntu), once
make pc                   # http://localhost:8080
make pc BT=hci1           # Bluetooth on hci1 (or its address)
make pc BT=ask            # ask which; make pc-bt-adapters lists them
make pc BT=fake           # fake BlueZ, two adapters
make pc INPUT=hw:2,0      # also record a card PipeWire doesn't run (SINK= to play to one)
```

- PipeWire config is untouched: the daemon is an ALSA client (`pipewire` PCM), shown as *PipeWire
  ALSA [soundtesterd]* in pavucontrol / `wpctl status` (move its streams there). It never opens a
  card PipeWire runs (`app/config/boards/pc.json`: `"scan": false`).
- Recording the default source holds the mic (`tools/pc/run --no-input` avoids it).
- Settings: `~/.local/state/soundtester`. Reboot/shutdown buttons do nothing.
- **Bluetooth** needs an adapter the desktop doesn't use (e.g. USB dongle). Once: `make
  pc-bt-setup` (bluez-alsa via apt-get download into `~/.local/share/soundtester`, nothing
  installed, no service; with sudo, D-Bus policy `/etc/dbus-1/system.d/soundtester-bluealsa.conf`;
  `make pc-bt-remove` undoes both). `make pc BT=hci1` runs bluez-alsa for hci1 only, alongside the
  daemon. PipeWire registers endpoints on every adapter, so:
  - a device connecting to hci1 may land on PipeWire: disconnect it in desktop settings, reconnect
    from the console;
  - the tester is BlueZ's default agent while running: it refuses pairings a device starts towards
    the desktop's adapter (pairing from desktop settings still works).

## Build the image

`BOARD=rpi3` (default) or `vim3l` (no Wi-Fi/Bluetooth): `yocto/boards/<board>.mk` +
`yocto/conf/boards/<board>.conf`, built in `yocto/build-<board>/` (shared downloads and sstate).
Plain poky + bitbake, no kas or pip; the first `make image` clones poky, meta-openembedded and the
BSP layer and generates the conf.

Per-bench settings (hostname, root password, SSH, Wi-Fi SSID/PSK, Bluetooth, ports) are baked in
from **`yocto/meta-soundtester/conf/soundtester-device.conf`**; rate/period are in the board conf.
It is **untracked** (cleartext password and PSK; git history keeps everything): `make configure`
creates it from the `.sample`; `make image` refuses without it.

```sh
make configure                 # hostname, root password, Wi-Fi, cache dirs
make host-deps                 # Yocto host packages (Debian/Ubuntu), once
make image                     # hours the first time
make image DEV=1               # writable, alsa-utils + ssh, for bring-up
make image BOARD=vim3l
make flash                     # list candidate disks
make flash DISK=/dev/mmcblk0   # shows what it erases, asks; DEV=1 for the dev image
make bitbake                   # bitbake shell; ARGS="soundtesterd" cross-builds the daemon
```

`flash` refuses the running system's disk and any disk with a mounted system directory; warns if
not flagged removable (normal for built-in readers, but also what an internal drive looks like).

Update a running board over ssh (both take `BOARD=`, `TARGET=root@host`):

```sh
make deploy-www                                           # app/www; live on next page load
make bitbake ARGS="soundtesterd" && make deploy-daemon    # daemon + /etc files; restarts it
```

| | rootfs | ssh | tools |
|---|---|---|---|
| `soundtester-image` | **read-only** | yes, unless `SOUNDTESTER_ENABLE_SSH = "0"` (password from the conf file) | none |
| `soundtester-image-dev` | writable | yes + package management | `alsa-utils`, `i2c-tools`, `strace`, `htop` |

Dev is for poking the card (`aplay -l`, `speaker-test`, `arecord`); production ships. Logs:
`journalctl -u soundtesterd -f`. UI settings live in RAM; **Configuration → Save as boot defaults**
writes a small ext4 partition (briefly rw), which also keeps the SSH host keys.

## How it fits together

One C++17 daemon, `soundtesterd`; no scripting runtime, GStreamer or audio framework. Links
alsa-lib, libopus + libogg (listen streams), libvorbis (compressing sender), libsamplerate (ASRC),
libsystemd (sd-bus for BlueZ); the rest is header-only submodules in `app/third_party/`.

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

- [cpp-httplib](https://github.com/yhirose/cpp-httplib): HTTP + WebSocket; handlers may *block*,
  as hours-long audio streams need.
- [pocketfft](https://github.com/mreineck/pocketfft): NumPy/SciPy's FFT.
- Raw **alsa-lib**: miniaudio/RtAudio/PortAudio convert formats behind your back or can't do a
  linked duplex start.
- nlohmann-json, spdlog, CLI11.

Threads and concurrency rules: [CLAUDE.md](CLAUDE.md).

## Layout

```
app/            C++17 daemon + vanilla-JS console (no build step)
  src/          engine, generators, analysis, capture, devices/sinks, net input, Bluetooth, web
  config/       factory defaults; boards/<profile>.json (per-board data)
  tests/        ctest, one plain executable per area
  third_party/  submodules, header-only, pinned at tags (pocketfft at a commit: no tags)
alsa-plugin/    sender ALSA PCM + mixer plugin
tools/          md2html (docs/api.md → GET /api), fake-bluez, pc (make pc helpers)
yocto/
  meta-soundtester/   images, app recipe, wic; BSP parts (pinned Pi kernel) in dynamic-layers/<bsp>/
    conf/soundtester-device.conf   <- hostname, password, ssh, Wi-Fi
  boards/             <board>.mk: MACHINE, BSP layer, daemon profile
  conf/               local.conf / bblayers.conf templates; boards/<board>.conf
  layers/             poky, meta-openembedded, BSP (cloned, gitignored)
docs/           api.md
```

## Documentation

[docs/api.md](docs/api.md): HTTP/WebSocket API and ALSA plugin, served rendered at `/api`; the
only reference since 13ea712. Older docs (calibration, Octo known issues, bench checklist): `git
show 13ea712^:docs/<name>.md`.
