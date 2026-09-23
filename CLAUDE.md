# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

A read-only Raspberry Pi (2/3/4, **not** Pi 5) appliance for bench-testing audio gear on an Audio
Injector Octo (CS42448, 6 in / 8 out, 96 kHz S32_LE). One C++17 daemon, `soundtesterd`, serves a
vanilla-JS web console. The kernel is pinned to **5.15.92** because the Octo produces only noise on
every 6.x kernel — do not bump it.

## Commands

```sh
make                  # list targets
make build            # cmake configure (Release) + build into app/build
make test             # ctest --test-dir app/build --output-on-failure
make run              # http://localhost:8080 against the simulated card (--sim)
make run DEVICE=hw:audioinjectoroc,0     # real card; HDMI=<alsa dev> / LINEOUT=<alsa dev> add those outputs
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
`app/third_party/`), and pkg-config packages alsa, opus, ogg, samplerate, vorbis (+ vorbisenc for
`test_net_session`). The build also renders `docs/api.md` → `app/www/api.html` (git-ignored) with
`tools/md2html`, so editing the API doc is part of changing the API.

Simulator: output c loops back into input c delayed by `period + c*STAGGER` frames (default 137),
so IN1→IN2 cross-correlation must read exactly 137 samples, IN1→IN3 274. The daemon serves `--www`
from disk per request, so web edits are live on reload without a restart.

## Architecture

Everything hangs off **one sample clock**: capture and playback are `snd_pcm_link()`ed on one card
and every generator is driven from the same absolute sample counter `n` that indexes the capture
ring. A sample index means the same instant on every channel — preserve this in any change.

Threads (wired in `app/src/main.cpp`; objects are connected *before* the audio thread starts):
- **AudioEngine** (`audio_engine.cpp`, SCHED_FIFO 80, mlocked): read 8 TDM slots → remap to 6
  ADCs → append to `RingBuffer` (float32, ~87 s) → render each output via `route_output()`
  (`output_route.h`, the single routing function shared by the Octo DACs, HDMI and line out) → write.
- **SocOutput** (`soc_out.cpp`, one instance per sink: HDMI, 3.5 mm line out): consumes a handoff
  ring the engine writes, on its own clock with ASRC trim; never on the audio thread's path.
  HDMI slot order is CEA-861, not ALSA's (`hdmi_layout.h`).
- **NetAudioServer** (`net_audio.cpp`): network inputs from the ALSA plugin. Each packet carries the
  absolute sample index it should play at; `NetTimeline` places it there (the timeline *is* the
  jitter buffer). Network channels occupy ring slots `[kInputs, kTotalInputs)` and are ordinary inputs
  everywhere else. `net_proto.h` is plain C shared verbatim by the daemon and `alsa-plugin/` — keep it
  C-only and bump `ST_NET_PROTO_VERSION` on wire changes.
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

Web UI: `app/www/` (`app.js`, `index.html`, `style.css`), no build step. The console feature-detects
daemon capabilities via `limits.*` in `/api/state`; console and daemon API renames must ship together.

## Boards (Yocto)

`BOARD=` (default `rpi3`) selects `yocto/boards/<board>.mk` (MACHINE, BSP layer, daemon profile
id) and `yocto/conf/boards/<board>.conf` (the board's bitbake settings). Each board builds in
`yocto/build-<board>/`; the Makefile writes its `bblayers.conf` and an `auto.conf` that sets
MACHINE and `require`s the board conf by absolute path. `meta-soundtester` depends only on core;
BSP-specific recipes live under `dynamic-layers/<collection>/` (the pinned 5.15 Pi kernel under
`raspberrypi`, VIM3L bits under `meson`). The multi-board roadmap (M1–M8) is in
`~/.claude/plans/create-grand-plan-change-nested-waffle.md`.

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
