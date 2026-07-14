# Measuring delay

**Capture and playback run from one hardware clock**, so a sample index means the same instant
on every input. Two inputs can therefore be compared with sample accuracy — at 96 kHz, one
sample is 10.4 µs, or 3.6 mm of air.

## The measurement

1. **Generators → Ping**, variant **tick**. Only `tick` is a measurement stimulus (see below).
2. Route the ping to the outputs under test (Dashboard → OUT *n* → Source: `Gen: ping`).
3. Feed the signal back into two inputs — by cable, or through a speaker and a microphone.
4. **Scope → Freeze.** Everything after this point is computed on the frozen snapshot, so the
   numbers cannot shift under you while you work.
5. Left-click to place cursor **A**, right-click for **B**, bracketing **one** ping.
6. **Measure delay A→B**, pick the two input channels.

You get `lag_samples`, `lag_ms` and `lag_m`. Sign convention: **a positive lag means the
signal arrives *later* on the second channel.**

The distance is `lag_ms × 0.343` m (343 m/s). It is only meaningful for an acoustic path —
for a cable, the lag is the device's delay, not a distance.

## Read the confidence number

`confidence` is the winning correlation peak divided by the best rival peak more than 1 ms
away:

| confidence | meaning |
|---|---|
| > 3 | one clear peak. Trust the lag. |
| < 2 | **ambiguous.** Something in the window correlates with itself. |

The usual cause of a low number is that your window contains **more than one ping**. A
repeating stimulus correlates with itself one interval away, so the true delay is only known
*modulo the ping interval* — the classic cycle-slip trap. Fix it by narrowing the cursors to
a single ping, or by raising the ping interval.

## Why `tick` and not `bing`/`bong`

A tone burst that rings for many cycles correlates almost as well one carrier period away as
it does at the true lag. Measured on the loopback:

| stimulus | shape | peak-to-rival ratio |
|---|---|---|
| **tick** | 3 kHz, 5 ms, τ = 0.4 ms | **4.2** |
| tick, if it decayed slowly (τ = 2.5 ms) | | 1.3 |
| bing | 1 kHz, 60 ms, τ = 20 ms | 1.0 |
| bong | 440 Hz, 250 ms, τ = 80 ms | 1.0 |

`tick` decays in well under one ring-down, which makes it broadband, which makes the
correlation peak sharp. `bing` and `bong` exist to be *heard* — for finding which speaker is
which — not to be measured. The measurement still lands on the right sample with them, but
the confidence will be near 1 and you will not be able to tell a good result from a bad one.

## Loopback offset (the constant you may want to subtract)

Output and input share a clock, but a sample handed to the DAC still has to travel through
the playback buffer, the codec, and the ADC before it comes back. That is a **constant**
offset, not a drift — the same on every channel, every run, until you change the period size.

Measure it once:

1. Wire **OUT 1 straight into IN 1** with a short cable.
2. Route the ping to OUT 1 and freeze. Cross-correlation is no help with only one channel:
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
