# Bench acceptance checklist

Work down this list on the real device. Anything that fails here is a real defect — every one
of these was verified in simulation first, so a failure means the hardware, the wiring or the
kernel, not the DSP.

## 0. The card works on this kernel at all

See [octo-known-issues.md](octo-known-issues.md) §"Milestone 0". Nothing below is meaningful
until `speaker-test` produces a clean tone rather than pulsing noise.

```sh
# The driver stack must be loaded first. If these are missing, no wiring will make the card
# appear — the image once built perfectly with an empty /lib/modules.
lsmod | grep -E "audioinjector|cs42xx8|bcm2835_i2s"

aplay -l                                   # card "audioinjectoroc" is listed
speaker-test -D hw:audioinjectoroc,0 -c 8 -r 96000 -F S32_LE -t sine -f 440
arecord -D hw:audioinjectoroc,0 -c 6 -r 96000 -f S32_LE -d 10 /tmp/t.wav
dmesg | grep -i "i2s\|audioinjector"       # no "I2S SYNC error"
```

## 1. Reachability

- [ ] `http://soundtester.local` loads over Ethernet DHCP (and Wi-Fi, if configured).
- [ ] `ssh root@soundtester.local` accepts the password from `soundtester-device.conf`.
- [ ] The host key does **not** change after a reboot (it lives on `/data`).
- [ ] UI works in Chrome and Firefox.

## 2. The stream really is running at full rate

```sh
cat /proc/asound/card*/pcm0p/sub0/hw_params    # playback
cat /proc/asound/card*/pcm0c/sub0/hw_params    # capture
```
- [ ] Both show `format: S32_LE`, `rate: 96000`, `channels: 8` (capture may show 8; only 6
      carry ADC data).
- [ ] **System** tab shows 0 xruns after 10 minutes.

## 3. Channels are where you think they are

- [ ] Every one of the 6 inputs moves its meter when you feed it a signal.
- [ ] **Identify** on OUT *n* beeps out of physical socket *n*. If not, fix it in
      **Channel map** — do not rewire. (This is the TDM rotation bug, not your wiring.)
- [ ] Route IN 1 → OUT 1 and confirm the signal comes back out.

## 4. The measurement

Wire **OUT 1 into IN 1 and IN 2** with identical cables. Ping = `tick`, interval 2 s.

- [ ] Analyze, bracket one ping, Measure IN 1 → IN 2.
- [ ] **Lag = 0 ± 1 sample.** Confidence > 3.
- [ ] `GET /api/pings/recent`: successive samples differ by exactly `2 × 96000 = 192000`.

Now make it lie on purpose: put a long cable, or a speaker + microphone, in one path.

- [ ] The lag goes positive and the distance is plausible (343 m/s — a metre is ~2.9 ms).
- [ ] Widen the cursors to cover several pings: the confidence **drops below 2**. It is
      supposed to — a repeating stimulus is ambiguous. If it stays high, the confidence
      metric is broken.

## 5. Analysis

- [ ] Route the sine generator (**996.09375 Hz** — bin-centred) to an output, loop it back.
- [ ] THD+N reads **< 0.5 %** (the codec's own floor; the simulator's clean loopback reads
      0.006 %, so anything much above 0.5 % is analogue, not software).
- [ ] The spectrum shows one peak at the right frequency.

## 6. Listening

- [ ] **Listen** on an input plays in Chrome and in Firefox, with under a second of latency.
- [ ] `vlc http://soundtester.local/api/inputs/0/stream.wav` plays.
- [ ] Opening listeners on all 6 inputs at once works; a 13th stream is refused with 503.

## 7. Read-only and power-loss

- [ ] `findmnt /` shows `ro`.
- [ ] Change routing → **Save as boot defaults** → reboot → the settings came back.
- [ ] **Factory reset** → reboot → back to image defaults.
- [ ] Change routing *without* saving → reboot → the change is gone (the device always boots
      into a known state).
- [ ] **Pull the power 10 times** during operation. It boots cleanly every time and the saved
      config is never corrupted.

## 8. Soak

- [ ] One hour at 96 kHz with all 6 inputs metering and a listener attached:
      `xruns == 0`, `sync_errors == 0`, CPU < 60 %.
- [ ] The I2S-sync banner works at all — prove the alarm is wired up:
      ```sh
      curl -X POST http://soundtester.local/api/system/inject-kmsg \
           -d 'bcm2835-i2s: I2S SYNC error!'
      ```
      The banner must appear. A monitor that never fires is indistinguishable from a healthy
      system.
