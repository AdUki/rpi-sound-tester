# CLAUDE.md

User-facing setup, image build, flashing: README.md.

Read-only audio bench tester: Raspberry Pi 3 (only Pi; never Pi 5) + Audio Injector Octo (CS42448,
6 in / 8 out, 96 kHz S32_LE), or Khadas VIM3L (no card). C++17 daemon `soundtesterd` serves a
vanilla-JS console. Pi kernel pinned to **5.15.92** (Octo gives only noise on every 6.x): do not
bump.

## Commands

```sh
make                  # list targets
make build            # cmake Release build into app/build
make test             # ctest --test-dir app/build --output-on-failure
make run              # localhost:8080, simulated card (--sim); BOARD= picks the profile
make run DEVICE=hw:audioinjectoroc,0     # real engine card; SINK=default adds the desktop sound server
make pc [BT=hci1|<addr>|ask|fake]        # real daemon beside PipeWire (tools/pc/run; no Yocto)
make plugin           # alsa-plugin/ for THIS host, not the Pi (VORBIS=0 drops libvorbis)
make image [BOARD=rpi3|vim3l] [DEV=1]    # Yocto (poky/bitbake, scarthgap); needs `make configure`
make bitbake ARGS="soundtesterd" [BOARD=…]   # cross-build the daemon; ARGS="-e <recipe>" inspects variables
make deploy-www / make deploy-daemon [BOARD=…] [TARGET=root@host]   # ssh push to a running board
```

- One test: `ctest --test-dir app/build -R test_capture --output-on-failure` or
  `app/build/tests/test_capture`. Plain executables with `CHECK`/`CHECK_EQ` (`app/tests/check.h`);
  add via `add_st_test(name)` in `app/tests/CMakeLists.txt`.
- Needs submodules (`git submodule update --init`; header-only, `app/third_party/`) and pkg-config
  alsa, opus, ogg, samplerate, vorbis, libsystemd (+ vorbisenc for `test_net_session`).
- Build renders `docs/api.md` → `app/www/api.html` (git-ignored, `tools/md2html`): API changes
  include that doc.
- Simulator: output c → input c delayed `period + c*STAGGER` frames (default 137), so IN1→IN2
  cross-correlation must read exactly 137 samples, IN1→IN3 274.
- `--www` served from disk per request: web edits live on reload.

## Architecture

**One sample clock**: the **engine card** (Octo; capture/playback `snd_pcm_link()`ed, no
resampling) or a `CLOCK_MONOTONIC` timer. Generators run off the absolute sample counter `n` that
indexes the capture ring: a sample index is one instant on every channel. Preserve this. Other ALSA
devices follow via ASRC.

Threads (wired in `app/src/main.cpp`, connected *before* the audio thread starts):
- **AudioEngine** (`audio_engine.cpp`, SCHED_FIFO 80, mlocked) drives `AudioBackend`
  (`AlsaLinkedBackend` = Octo, `TimerBackend` = no card / `--sim`) → **EngineCore**
  (`engine_core.cpp`): read 8 TDM slots → remap to 6 ADCs → `RingBuffer` (float32, ~87 s) → render
  outputs and sink channels via `route_output()` (`output_route.h`, the only routing function) →
  write.
- **Devices** (`devices.cpp`, `alsa_devices.cpp`): scans ALSA every 2 s (`aplay -l`/`arecord -l`),
  binds non-engine devices by id `<card id>,<dev>`: playback → one of 8 sink slots, capture →
  device-input ring columns. Unplugged USB keeps slot, columns, routing.
  - **DeviceInput** (`device_input.cpp`, per capture device): each frame goes to the ring index it
    was captured at (engine position minus driver-held frames) in per-channel `NetTimeline`s,
    ASRC-trimmed; audio thread reads a capture delay behind, ≥ 150 ms while any is bound.
  - **SinkOutput** (`sink_out.cpp`, per bound slot): reads a handoff ring the engine writes at the
    block's `n`, on the device clock with ASRC trim; never on the audio thread's path. HDMI: CEA-861
    layouts (`sink_layout.h`); others: stereo and their own width.
- **NetAudioServer** (`net_audio.cpp`): ALSA-plugin inputs. Packets carry their absolute play
  index; `NetTimeline` places them (it *is* the jitter buffer). Ring slots
  `[channels().net_base(), device_base())`, ordinary inputs elsewhere. `net_proto.h` is plain C
  shared verbatim with `alsa-plugin/`: keep it C-only; bump `ST_NET_PROTO_VERSION` on wire changes.
- **Analysis** (10 Hz meters, FFT, THD+N, scope envelope); **CaptureStore** (freeze, windowed
  reads, FFT cross-correlation; `genie.h` aggregates per-ping delays); **KmsgWatch** (TDM
  slot-rotation "I2S SYNC error"s in /dev/kmsg).
- **WebServer** (`webserver.cpp`, cpp-httplib) + **WsHub**: REST, push-only `/api/ws`, Opus/Ogg
  listen streams per channel (`listen_encoder`/`listen_stream`). API: `docs/api.md`.

Rules:
- Ring is single-writer; every read is **seqlock-style post-validated** (a pre-check alone lets a
  stalled reader return stitched audio).
- Controls: atomics in `control.h`; compound values (e.g. output source `{type, index}`) **packed
  into one atomic** (no torn pair). Read atomics once per block, not per sample (breaks
  vectorization).
- Release: `-O3 -ftree-vectorize -fno-math-errno`, + `-funsafe-math-optimizations` on 32-bit ARM
  (NEON). Never `-ffast-math`: analysis relies on NaN/Inf guards.
- Unopenable card/device is never fatal: retry, report in `/api/state`.
- Channel counts are runtime (`channel_layout.h`, `channels()`, set in main before threads):
  `[engine inputs (6|0) | NET n | device inputs (board.json)]`, n = `net.inputs` (config.json,
  default 6, changes only across restart; `Config::remap_columns` moves saved per-column
  settings). Arrays sized `kMaxInputs` (32); loops/strides use `channels().total()`.

Web UI `app/www/` (`app.js`, `index.html`, `style.css`), no build step; feature-detects via
`limits.*` in `/api/state`, one section per `sinks` entry. Ship console and daemon API renames
together (static files are `Cache-Control: no-cache`).

## Bluetooth

BlueZ = radio, bluez-alsa (meta-multimedia, 4.0.0) = audio; daemon uses sd-bus (`bluetooth.cpp`,
`BtManager`: one thread owns the bus; also the pairing agent, says yes to everything by request:
`bt_agent_policy`). On only with board.json `"bluetooth": true` (recipe: `SOUNDTESTER_BLUETOOTH`)
or `--bluetooth`. Desktop: `make run BT=fake` (tools/fake-bluez, private bus). **Never
`--bluetooth` on the laptop's real bus** (takes its adapter and default agent). `make run DEVICE=…`
loads a profile with a radio, so passes `--no-bluetooth`; `--sim` already has it off.
- **Output**: sink `bluetooth` (`kBtSinkId`) via `Devices::add_fixed_sink` (no scan finds
  bluez-alsa PCMs); `PUT /api/bluetooth/output` retargets. Sets `SinkDevice::local_queue`:
  bluez-alsa's `snd_pcm_delay` includes the speaker's varying codec/radio delay (150-900 ms); the
  servo must not hold it.
- **Input** (`bt_input.cpp`): in-process *network sender* (`claim_channels("bt:"+addr)`, shared
  `NetAudioServer::Feed`), so needs `net_delay_frames()` (on with `net.enabled || net.bt_input`).
  Re-anchors on `Feed::behind()` (paused phone).
- **AVRCP**: phone's `MediaPlayer1` carried forward (Position only arrives on change). Tester is a
  target too (`bt_player.cpp`, MPRIS via `Media1.RegisterPlayer`): title = BT sink's signal, artist
  "Sound Tester"; play/pause = unmute/mute, stop = sink off, next/prev cycle
  Silence/Sine/Noise/Ping/Music. Speakers subscribe on *connect*: after daemon restart, silent until
  reconnect. Verify with `btmon` on the board.
- **Pi 3 B** (`yocto/conf/boards/rpi3.conf`): `dtparam=krnbt=on` (5.15 DT disables the node);
  `krnbt_baudrate=921600` under `[board-type=0x8]` (no RTS/CTS; 3 Mbaud overruns: `0x0c14 tx
  timeout`). All 3 B boot as 43:43:A1:12:1F:AC; `soundtester-bluetooth` sets
  `B8:27:EB:`+serial^0xAA via `btmgmt public-addr` before bluetoothd (only cold boot tests it;
  `btmgmt info` hangs without a tty). Pairings on tmpfs, mirrored to `/data/bluetooth`
  (`ConfigStore::save_dir`).
- BT modules made the Octo lose a probe race every boot (codec reset pulsed 1.5 s; a cs42xx8 probe
  inside it got -121 for good); kernel patch 0003 defers. Sysfs `bind` skips the deferred list:
  test via module load.
- Device names are attacker-controlled (radio range): `esc()` them in app.js.

## PC (no Yocto)

`make pc` (`tools/pc/run`), host compiler, `app/config/boards/pc.json`: timer, `"desktop": true`
(no reboot/shutdown; pairings not mirrored to data dir), `"scan": false` (never opens a PipeWire
card; only `--sink`/`--input`, default the `pipewire` ALSA PCM both ways). **Do not touch
PipeWire's config.**
- BT on one adapter (`--bt-adapter hciN|<addr>`, `bt_pick_adapter`); others' devices unlisted,
  their agent requests rejected.
- `make pc-bt-setup`: bluez-alsa in `~/.local/share/soundtester/bluez-alsa` (no apt install, no
  service: the packaged one takes every adapter from PipeWire) + a D-Bus policy letting the user
  own `org.bluealsa`. `run` starts `bluealsa -i <addr>` as the user; `ALSA_CONFIG_PATH` points at
  the plugin for the daemon only.
- `make pc BT=fake`: fake-bluez, `FAKE_BLUEZ_ADAPTERS=2` (world on hci1).

## Boards (Yocto)

`BOARD=` (default `rpi3`) → `yocto/boards/<board>.mk` (MACHINE, BSP layer, profile id) +
`yocto/conf/boards/<board>.conf` (bitbake settings). Profile → `app/config/boards/<profile>.json`
→ `/etc/soundtester/board.json`, the daemon's only per-board data: engine card (none on VIM3L),
`capture_channels`, rate/period/periods (patched from `SOUNDTESTER_RATE/PERIOD`), `device_inputs`,
`bluetooth`, optional labels, `hdmi`/`hidden` flags per device id. `make run` loads it (`--board`).
- Builds in `yocto/build-<board>/`; Makefile writes `bblayers.conf` and `auto.conf` (MACHINE,
  `require` board conf by absolute path).
- `meta-soundtester` depends only on core; BSP recipes in `dynamic-layers/<collection>/` (pinned
  5.15 Pi kernel: `raspberrypi`; VIM3L: `meson`).
- Parse order: `soundtester-device.conf` (from `layer.conf`) → `auto.conf` → board conf →
  `local.conf` → BSP machine conf. Hence board values hard `=`, per-bench overrides in
  `local.conf`, and what a machine conf hard-sets (meta-meson: `WKS_FILE`, `IMAGE_BOOT_FILES`,
  `IMAGE_NAME_SUFFIX`) set at recipe scope. Check: `make bitbake BOARD=… ARGS="-e
  soundtester-image"`.

## Config and deployment

- `yocto/meta-soundtester/conf/soundtester-device.conf` (hostname, root password, Wi-Fi):
  untracked secrets, from `make configure` + `.sample`. Never commit it or `yocto/build-*/`
  (rendered Wi-Fi config, `/etc/shadow`).
- Runtime settings in RAM; "Save as boot defaults" → small ext4 partition. Factory defaults:
  `app/config/default-config.json`.
- Read-only rootfs: deploys remount rw, copy, sync, remount ro. `deploy-www`: no restart;
  `deploy-daemon`: needs the bitbake cross-build, restarts the service.
