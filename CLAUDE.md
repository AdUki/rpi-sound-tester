# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

A read-only appliance for bench-testing audio gear: a Raspberry Pi 3 (the only Pi supported; never a
Pi 5) with an Audio Injector Octo (CS42448, 6 in / 8 out, 96 kHz S32_LE), or a Khadas VIM3L with no
card of its own. One C++17 daemon, `soundtesterd`, serves a vanilla-JS web console. The Pi's kernel
is pinned to **5.15.92** because the Octo produces only noise on every 6.x kernel — do not bump it.

## Commands

```sh
make                  # list targets
make build            # cmake configure (Release) + build into app/build
make test             # ctest --test-dir app/build --output-on-failure
make run              # http://localhost:8080 against the simulated card (--sim); BOARD= picks the profile
make run DEVICE=hw:audioinjectoroc,0     # real engine card; SINK=default adds a desktop's sound server as an output
make plugin           # the ALSA sender plugin in alsa-plugin/ (built for THIS host, not the Pi; VORBIS=0 drops libvorbis)
make image [BOARD=rpi3|vim3l] [DEV=1]    # Yocto (plain poky/bitbake, scarthgap); needs `make configure` first
make bitbake ARGS="soundtesterd" [BOARD=…]   # cross-build just the daemon (ARGS="-e <recipe>" to inspect variables)
make deploy-www / make deploy-daemon [BOARD=…] [TARGET=root@host]   # push to a running board over ssh, no reflash
```

Single test: `ctest --test-dir app/build -R test_capture --output-on-failure`, or run the binary
directly (`app/build/tests/test_capture`). Tests are plain executables using the `CHECK`/`CHECK_EQ`
macros in `app/tests/check.h` (no framework); add one with `add_st_test(name)` in
`app/tests/CMakeLists.txt`.

Build prerequisites: submodules (`git submodule update --init`, all header-only under
`app/third_party/`), and pkg-config packages alsa, opus, ogg, samplerate, vorbis, libsystemd (+
vorbisenc for `test_net_session`). The build also renders `docs/api.md` → `app/www/api.html`
(git-ignored) with `tools/md2html`, so editing the API doc is part of changing the API.

Simulator: output c loops back into input c delayed by `period + c*STAGGER` frames (default 137),
so IN1→IN2 cross-correlation must read exactly 137 samples, IN1→IN3 274. The daemon serves `--www`
from disk per request, so web edits are live on reload without a restart.

## Architecture

Everything hangs off **one sample clock**: the **engine card** (the Octo: capture and playback
`snd_pcm_link()`ed, no resampling) or, on a board without one, a `CLOCK_MONOTONIC` timer. Every
generator is driven from the same absolute sample counter `n` that indexes the capture ring. A
sample index means the same instant on every channel — preserve this in any change. Every other
ALSA device follows that clock through an ASRC.

Threads (wired in `app/src/main.cpp`; objects are connected *before* the audio thread starts):
- **AudioEngine** (`audio_engine.cpp`, SCHED_FIFO 80, mlocked) drives an `AudioBackend`
  (`AlsaLinkedBackend` = the Octo, `TimerBackend` = no card or `--sim`) and hands each block to
  **EngineCore** (`engine_core.cpp`): read 8 TDM slots → remap to 6 ADCs → append to `RingBuffer`
  (float32, ~87 s) → render each output and sink channel via `route_output()` (`output_route.h`,
  the single routing function) → write.
- **Devices** (`devices.cpp`, `alsa_devices.cpp`): scans ALSA every 2 s (what `aplay -l` /
  `arecord -l` list) and binds each device other than the engine card: playback to one of 8 sink
  slots, capture to device-input ring columns, by device id `<card id>,<dev>`; a USB device
  unplugged keeps its slot, columns and routing. **DeviceInput** (`device_input.cpp`, one per
  capture device) places each frame at the ring index it was captured at (engine position minus
  what the driver still holds) into per-channel `NetTimeline`s, ASRC-trimmed; the audio thread reads
  them a capture delay behind, and holds that delay at ≥ 150 ms while any is bound.
  **SinkOutput** (`sink_out.cpp`, one per bound slot) consumes a handoff ring the engine writes at
  the block's `n`, on the device's clock with ASRC trim; never on the audio thread's path. HDMI
  sinks offer CEA-861 speaker layouts (`sink_layout.h`), others stereo and their own width.
- **NetAudioServer** (`net_audio.cpp`): network inputs from the ALSA plugin. Each packet carries the
  absolute sample index it should play at; `NetTimeline` places it there (the timeline *is* the
  jitter buffer). Network channels occupy ring slots `[channels().net_base(), device_base())` and
  are ordinary inputs everywhere else. `net_proto.h` is plain C shared verbatim by the daemon and
  `alsa-plugin/` — keep it C-only and bump `ST_NET_PROTO_VERSION` on wire changes.
- **Analysis** (10 Hz meters, FFT spectrum, THD+N, scope envelope), **CaptureStore** (freeze snapshot,
  windowed reads, FFT cross-correlation; `genie.h` aggregates per-ping delay readings),
  **KmsgWatch** (flags TDM slot-rotation "I2S SYNC error"s from /dev/kmsg).
- **WebServer** (`webserver.cpp`, cpp-httplib) + **WsHub**: REST, push-only `/api/ws`, per-channel
  listen streams (Opus/Ogg via `listen_encoder`/`listen_stream`). API documented in `docs/api.md`.

Concurrency rules that matter when editing:
- The ring is single-writer; every read is **seqlock-style post-validated** (a pre-check alone lets a
  stalled reader return stitched audio).
- Control values live in `control.h` as atomics; compound values (e.g. an output's `{type, index}`
  source) are **packed into one atomic** so the audio thread can't see a torn pair. Read atomics once
  per block, never per sample (it defeats vectorization).
- Release flags are `-O3 -ftree-vectorize -fno-math-errno`, plus `-funsafe-math-optimizations` on
  32-bit ARM for NEON. Never `-ffast-math`: the analysis code relies on NaN/Inf guards.
- An unopenable card/device is never fatal: threads retry and report the error in `/api/state`.
- Channel counts are runtime (`channel_layout.h`, `channels()`, set once in main before any
  thread): `[engine card's inputs (6 or 0) | NET 6 | device inputs (board.json)]`. Arrays are
  sized by `kMaxInputs`; loops and strides use `channels().total()`.

Web UI: `app/www/` (`app.js`, `index.html`, `style.css`), no build step. The console feature-detects
daemon capabilities via `limits.*` in `/api/state` and builds one section per entry of `sinks`;
console and daemon API renames must ship together (static files go out with `Cache-Control:
no-cache`, so a browser picks up a deploy on the next load).

## Bluetooth

BlueZ runs the radio, bluez-alsa (meta-multimedia, 4.0.0) carries the audio, and the daemon does the
rest over sd-bus (`bluetooth.cpp`, `BtManager`: one thread owns the bus; it is also the pairing agent,
and says yes to everything by request — `bt_agent_policy`). Only where board.json has
`"bluetooth": true` (the recipe sets it from `SOUNDTESTER_BLUETOOTH`), or with `--bluetooth`; on a
desktop use `make run BT=fake` (tools/fake-bluez, a private bus) — **never `--bluetooth` on the
laptop's real bus**, it takes over its adapter and default agent. `make run DEVICE=…` loads a
board profile that has a radio, so it passes `--no-bluetooth` (`--sim` already leaves it off).
- **Output** = the sink `bluetooth` (`kBtSinkId`), added with `Devices::add_fixed_sink` since no
  scan finds a bluez-alsa PCM; `PUT /api/bluetooth/output` retargets it to a speaker. It sets
  `SinkDevice::local_queue`: bluez-alsa's `snd_pcm_delay` includes the speaker-reported codec/radio
  delay (150-900 ms, and it changes), which the servo must not hold.
- **Input** (`bt_input.cpp`) is an in-process *network sender*: `claim_channels("bt:"+addr)`, the
  shared `NetAudioServer::Feed`, so it needs the net alignment delay (`net_delay_frames()`, on with
  `net.enabled || net.bt_input`). Re-anchors on `Feed::behind()` (a paused phone).
- **AVRCP**: a phone's `MediaPlayer1` is carried forward (Position only arrives on change). The
  tester is a target too (`bt_player.cpp`): an MPRIS player `Media1.RegisterPlayer`ed, title = the
  BT sink's signal, artist "Sound Tester"; play/pause = unmute/mute, stop = sink off, next/prev
  cycle Silence/Sine/Noise/Ping/Music. A speaker subscribes when it *connects*, so after a daemon
  restart it hears nothing until it reconnects. Verify over the air with `btmon` on the board.
- **Pi 3 B**: needs `dtparam=krnbt=on` (5.15 DT ships the node disabled), and
  `krnbt_baudrate=921600` under `[board-type=0x8]` (no RTS/CTS: 3 Mbaud overruns, `0x0c14 tx
  timeout`) — both in `yocto/conf/boards/rpi3.conf`. Every 3 B boots as 43:43:A1:12:1F:AC;
  `soundtester-bluetooth` sets `B8:27:EB:`+serial^0xAA with `btmgmt public-addr` before bluetoothd
  (only a cold boot exercises it; `btmgmt info` never exits without a tty). Pairings live on a
  tmpfs and are mirrored to `/data/bluetooth` (`ConfigStore::save_dir`).
- Loading the BT modules made the Octo lose a latent probe race on every boot (the machine driver
  pulses the codec reset for 1.5 s; a cs42xx8 probe inside it got -121 for good): kernel patch 0003
  defers instead. A manual sysfs `bind` does not exercise the deferred list — test via a module load.
- Device names are attacker-controlled text from anyone in radio range: `esc()` them in app.js.

## Boards (Yocto)

`BOARD=` (default `rpi3`) selects `yocto/boards/<board>.mk` (MACHINE, BSP layer, daemon profile
id) and `yocto/conf/boards/<board>.conf` (the board's bitbake settings). The profile id picks
`app/config/boards/<profile>.json`, installed as `/etc/soundtester/board.json`: the engine card
(none on the VIM3L) and its `capture_channels`, rate, period and periods (patched from
`SOUNDTESTER_RATE/PERIOD`), `device_inputs`, `bluetooth`, and optional labels, `hdmi` flags and
`hidden` flags for device ids — the only per-board data the daemon has. `make run` loads the same
file with `--board`. Each board builds in `yocto/build-<board>/`; the Makefile writes its
`bblayers.conf` and an `auto.conf` that sets MACHINE and `require`s the board conf by absolute path. `meta-soundtester` depends only on core;
BSP-specific recipes live under `dynamic-layers/<collection>/` (the pinned 5.15 Pi kernel under
`raspberrypi`, VIM3L bits under `meson`).

Bitbake parse order bites here: `soundtester-device.conf` (required from `layer.conf`) is read
first, then `auto.conf` → board conf → `local.conf`, and the BSP's machine conf last. So board
values are hard `=`, per-bench overrides go in `local.conf`, and anything a machine conf hard-sets
(meta-meson sets `WKS_FILE`, `IMAGE_BOOT_FILES`, `IMAGE_NAME_SUFFIX`) must be set at recipe scope.
Check with `make bitbake BOARD=… ARGS="-e soundtester-image"`.

## Config and deployment

- `yocto/meta-soundtester/conf/soundtester-device.conf` (hostname, root password, Wi-Fi) is
  **untracked** and holds secrets; created by `make configure` from the `.sample`. Never commit it,
  nor anything under `yocto/build-*/` (rendered Wi-Fi config, `/etc/shadow`).
- Runtime settings live in RAM; "Save as boot defaults" writes them to a small ext4 partition.
  Factory defaults: `app/config/default-config.json`.
- The rootfs is mounted read-only; deploy targets remount rw, copy, sync, remount ro. `deploy-www`
  needs no restart; `deploy-daemon` needs the bitbake cross-build and restarts the service.
