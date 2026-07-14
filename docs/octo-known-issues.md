# Audio Injector Octo — known issues

Verified against the kernel sources and the vendor's issue tracker. Read this before you
spend a day debugging your wiring.

## 1. The card is broken on every 6.x kernel

**Symptom:** the card probes, `aplay -l` lists it, and then playback is a distorted
"rhythmic pulsing noise" and capture is noise.

| Kernel | Result |
|---|---|
| 5.15.92 | works (last known-good) |
| 6.1, 6.6, 6.12 | probes, but audio is garbage |

Trail of evidence:

- [Audio-Injector/Octo#64](https://github.com/Audio-Injector/Octo/issues/64) — pulsing noise on 6.x
- [raspberrypi/firmware#1884](https://github.com/raspberrypi/firmware/issues/1884) — still open
- [raspberrypi/linux#6909](https://github.com/raspberrypi/linux/issues/6909) — closed **"not planned"**, June 2025. A Raspberry Pi engineer noted the manufacturer no longer actively supports the card.
- A probe regression was fixed by Phil Elwell in commit `fe027c6` (April 2024), which restored *probing* but not clean audio.
- flatmax (the vendor) suspected the `bcm2835-i2s` driver's move to TDM handling. He has been inactive since.

**What this project does about it:** pins kernel **5.15.92** (`rpi-5.15.y` at
`14b35093ca68bf2c81bbc90aace5007142b40b40`) in
`yocto/meta-soundtester/recipes-kernel/linux/linux-raspberrypi_5.15.bb`.

**Consequences you must accept:**

- The image runs an end-of-life kernel. Keep the device on a trusted lab network.
- **Raspberry Pi 5 / CM5 cannot work at all.** 5.15 predates Pi 5, and the overlay targets
  `i2s_clk_producer` while the card is the clock master — on the RP1 the producer and
  consumer are separate blocks. There is no working configuration reported anywhere, and
  the old-kernel workaround cannot apply. Supported: **Pi 2, 3, 4**.

### Milestone 0: prove this on your own hardware first

Nothing else in the build is worth doing until you know which kernel your card works on.

```sh
# A. Current Raspberry Pi OS (kernel 6.x) — expected to FAIL with pulsing noise
#    Add to /boot/firmware/config.txt:  dtoverlay=audioinjector-addons,non-stop-clocks
#    and make sure dtparam=audio=on is commented out. Reboot, then:
aplay -l                                   # expect: card "audioinjectoroc"
speaker-test -D hw:audioinjectoroc,0 -c 8 -r 96000 -F S32_LE -t sine -f 440
arecord -D hw:audioinjectoroc,0 -c 6 -r 96000 -f S32_LE -d 10 /tmp/t.wav
dmesg | grep -i "i2s\|audioinjector"       # expect no "I2S SYNC error" spam

# B. This project's dev image (kernel 5.15) — expected to WORK
make image DEV=1 && make flash DEV=1 DISK=<your card>
# boot it, ssh in, and run exactly the same four commands
```

Record what you saw in this file. If 6.x turns out to have been fixed, drop the pin — the
whole `linux-raspberrypi_5.15.bb` recipe exists only because of this bug.

### Building a 5.15 kernel on scarthgap: what it took

Pinning an old kernel inside a current Yocto release is not free. Two things had to be
fixed; both are already in the tree.

**1. GCC plugins (`recipes-kernel/linux/files/gcc13-compat.cfg`).** The kernel builds its
GCC plugins against the *host* compiler's plugin headers. 5.15's plugin sources predate
GCC 13, so `scripts/gcc-plugins/gcc-common.h` explodes with

```
error: use of enum 'gsi_iterator_update' without previous declaration
make[2]: *** [scripts/gcc-plugins/arm_ssp_per_task_plugin.so] Error 1
```

The plugins are optional hardening, so they are turned off. ARM's
`STACKPROTECTOR_PER_TASK` *selects* the ARM SSP plugin, so that has to go too — the stack
protector itself stays on, just with a global canary instead of a per-task one.

**2. Device-tree paths (`RPI_KERNEL_DEVICETREE` in `yocto/conf/local.conf.sample`).**
meta-raspberrypi/scarthgap targets kernel 6.x, where ARM device trees moved into vendor
subdirectories, so its list says `broadcom/bcm2710-rpi-3-b.dtb`. In 5.15 they are still
flat in `arch/arm/boot/dts/`, and the build dies with

```
make[1]: *** No rule to make target 'arch/arm/boot/dts/broadcom/bcm2708-rpi-zero.dtb'
```

Its list also names boards that do not exist in 5.15 at all (Pi 5, CM5). Both lists are
therefore pinned to what this kernel actually ships.

**If it ever fights the toolchain harder than this:** move the whole layer stack to the
`kirkstone` branches, where the 5.15 recipe is native. Kirkstone is EOL, which is acceptable
for an offline appliance.

## 2. TDM slot rotation — channel numbers are not stable

The card's slot alignment can shift after a `bcm2835-i2s: I2S SYNC error!`. Audio sent to
channels 1–2 can come out of 7–8, and the *input* mapping can change across reboots or
stream restarts (Octo issues [#1](https://github.com/Audio-Injector/Octo/issues/1),
[#8](https://github.com/Audio-Injector/Octo/issues/8), #36, #54 — never fixed).

It silently relabels your channels.

Mitigations built into this design:

1. **`non-stop-clocks`** on the overlay (the documented workaround from issue #8) keeps BCLK
   running between streams.
2. **The PCMs are opened once at boot and never closed.** Xrun recovery keeps the handles
   open, so there is no stream restart to trigger a rotation.
3. **Software remap tables** (`input_map`, `output_map`) in the config and the *Channel map*
   tab, so a rotation can be corrected in the UI instead of by rewiring.
4. **Identify** buttons play three beeps on a single output, so you can find out where a
   channel actually comes out in about a minute.
5. A **kmsg watcher** counts `I2S SYNC error` lines and raises a banner in the UI:
   *"verify your channel mapping"*.

## 3. Facts worth knowing

- **96 kHz is the ceiling.** The machine driver constrains the rate to a fixed list
  (96000, 88200, 48000, 44100, 32000, 24000, 22050, 16000, 14700, 8000). 192 kHz is not
  reachable, even though the bare CS42448 codec supports it.
- **Formats:** S16_LE, S24_LE, S32_LE. This project uses **S32_LE** (24 valid bits).
- **Capture may open with 8 channels.** The machine driver raises the codec's capture
  `channels_max` to 8 while a stream runs, because the TDM frame has 8 slots — but only 6
  carry real ADC data. The daemon opens 8 and falls back to 6.
- **Capture and playback genuinely share one clock.** An on-board FPGA is the BCLK/LRCLK
  master for *both* the codec and the Pi's I2S (both are clock slaves), and `bcm2835-i2s`
  sets `symmetric_rate` / `symmetric_sample_bits`. Sample-accurate measurement depends on
  this; `snd_pcm_link()` is what makes the two streams *start* together.
- The vendor's site (audioinjector.net) has an expired TLS certificate and the forum is
  intermittently down. GitHub is the only reliable source.
