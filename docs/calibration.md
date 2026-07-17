# Measuring delay

**Capture and playback run from one hardware clock**, so a sample index means the same instant
on every input. Two inputs can therefore be compared with sample accuracy — at 96 kHz, one
sample is 10.4 µs, or 3.6 mm of air.

## The measurement

1. **Generators → Ping**. Any variant measures correctly; `tick` is the most robust (see below).
2. Route the ping to the outputs under test (Dashboard → OUT *n* → Source: `Gen: ping`).
3. Feed the signal back into two inputs — by cable, or through a speaker and a microphone.
4. **Scope & sync → Analyze.** This freezes the capture: everything after this point is
   computed on the frozen snapshot, so the numbers cannot shift under you while you work.
5. Left-click to place cursor **A**, right-click for **B**, bracketing **one** ping.
6. Pick the two input channels in the Delay selector and press **Measure**.

The console shows the lag in samples and ms with its confidence; the API response also
carries `lag_m`. Sign convention: **a positive lag means the signal arrives *later* on the
second channel.**

The distance is `lag_ms × 0.343` m (343 m/s). It is only meaningful for an acoustic path —
for a cable, the lag is the device's delay, not a distance.

## Read the confidence number

`confidence` is the winning correlation peak divided by the tallest **genuinely separate**
rival peak. (Both are read off the *envelope* of the correlation, so a ringing tone's own
carrier oscillation does not count as a rival — a lone bing/bong scores high, as it should.)

| confidence | meaning |
|---|---|
| > 3 | one clear peak. Trust the lag. |
| < 2 | **ambiguous.** A rival peak is nearly as tall. |

Two things drive the number down, and both are real:

- **More than one ping in the window.** A repeating stimulus correlates with itself one
  interval away, so the true delay is only known *modulo the ping interval* — the classic
  cycle-slip trap. Bracket a single ping with the cursors, or raise the ping interval.
- **A continuous tone** (the sine generator). Its delay is only known modulo the carrier
  period, and no windowing fixes that. Use a ping.

## Which variant to measure with

All three ping variants land on the right sample when one ping is bracketed — measured on
the loopback, each recovers the simulator's programmed delay exactly. `tick` is still the
best-practice stimulus: it decays in well under one carrier ring-down, which makes it
broadband, which makes the correlation peak *sharp*. `bing` and `bong` ring for tens of
cycles, so in a noisy or reflective environment their correlation crests one carrier cycle
either side of the truth (≈ 1–2 ms) are only a few percent down — a strong reflection can
tip the result one cycle over. On a clean bench any variant is fine; when the number matters,
use `tick`.

## Loopback offset (the constant you may want to subtract)

Output and input share a clock, but a sample handed to the DAC still has to travel through
the playback buffer, the codec, and the ADC before it comes back. That is a **constant**
offset, not a drift — the same on every channel, every run, until you change the period size.

Measure it once:

1. Wire **OUT 1 straight into IN 1** with a short cable.
2. Route the ping to OUT 1 and press **Analyze**. Cross-correlation is no help with only one channel:
   read the ping's own emission sample from **Generators → Recent emissions**, then find the
   arrival in the scope on IN 1. The difference is the loopback offset.
3. Store it in `loopback_offset_samples` in the config if you want it recorded. The daemon
   treats it as informational: it never silently adjusts your measurements.

**You do not need this for multiroom work.** Comparing two *inputs* cancels it completely —
the offset is identical on both paths. It only matters if you compare an *output* event
against an *input* arrival.

## Sanity check the whole chain

Wire one output into two inputs with identical cables. The measured lag must be **0 ± 1
sample**. If it is not, you have a wiring or mapping problem — not a measurement problem.

To prove the software without touching hardware, the simulator loops each output back into
the matching input with a delay you choose:

```sh
soundtesterd --sim --sim-stagger 137 --port 8080 --www app/www
# route ping to OUT1/OUT2/OUT3, freeze, then cross-correlate:
#   IN1 vs IN2 -> 137 samples
#   IN1 vs IN3 -> 274 samples
```
