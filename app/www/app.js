'use strict';

const api = (p, opt) => fetch('/api' + p, opt).then(async r => {
  const body = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(body.error || r.statusText);
  return body;
});
const put = (p, obj) => api(p, {
  method: 'PUT', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(obj)
});
const post = (p, obj) => api(p, {
  method: 'POST', headers: {'Content-Type': 'application/json'},
  body: obj === undefined ? undefined : JSON.stringify(obj)
});

const $ = id => document.getElementById(id);
const NIN = 6, NOUT = 8;

const tabActive = name => $(name).classList.contains('active');

// localStorage access throws, not just returns null, when the origin has storage blocked
// (browser "block site data", an enterprise policy on this plain-http LAN address). Some of
// these reads run first in the /state chain, where an unguarded throw would skip building the
// whole UI and get swallowed as a "cannot reach the daemon" error — a healthy device
// misreported as dead. So every access goes through these two.
const lsGet = key => {
  try { return localStorage.getItem(key); } catch (e) { return null; }
};
const lsSet = (key, val) => {
  try { localStorage.setItem(key, val); } catch (e) { /* storage blocked / private mode */ }
};

// Non-modal error strip. alert() steals focus and blocks the page — on a bench instrument
// that interrupts exactly the moment you are watching.
function toast(msg) {
  let t = $('toast');
  if (!t) {
    t = document.createElement('div');
    t.id = 'toast';
    document.body.appendChild(t);
  }
  t.textContent = msg;
  t.classList.add('show');
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => t.classList.remove('show'), 4000);
}

let state = null;
let rate = 96000;
let envColumnFrames = 480;
let gainMinDb = 0, gainMaxDb = 40;

// ---------------------------------------------------------------- tabs

document.querySelectorAll('[data-tab]').forEach(el => {
  el.addEventListener('click', e => {
    e.preventDefault();
    const name = el.dataset.tab;
    document.querySelectorAll('.tab').forEach(t => t.classList.toggle('active', t.dataset.tab === name));
    document.querySelectorAll('.panel').forEach(p => p.classList.toggle('active', p.id === name));
    // A canvas in a display:none panel measures zero, so neither could draw until now.
    if (name === 'scope') drawScope();
    else if (name === 'dash') redrawSpectra();
  });
});

// A resize gives every canvas a new backing store; nothing else repaints them. Debounced,
// because a drag of the window edge fires this continuously.
let resizeTimer = null;
window.addEventListener('resize', () => {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => {
    redrawSpectra();
    if (srvFrozen()) invalidateWindows();   // the window fetch is sized to the canvas
    scopeDirty = true;
  }, 150);
});

// ---------------------------------------------------------------- listen
//
// AudioWorklet requires a secure context, so it does not exist on a plain http:// origin
// like http://soundtester.local. Chaining AudioBufferSourceNodes works everywhere and is
// what this device needs: the toggle click is the user gesture the autoplay policy wants.
//
// Each input can be sent to the left ear, the right ear, both, or neither. A ChannelMerger
// is the mixer: everything connected to its input 0 is summed into the left channel and
// everything on input 1 into the right, so IN 1 -> L and IN 2 -> R reproduces a stereo pair,
// and three inputs on L are heard on top of each other.
//
// All of it shares ONE AudioContext and ONE timeline. Every chunk arrives stamped with its
// absolute capture sample index and is scheduled at anchorT + (sample - anchorN) / rate — so
// two channels carrying the same instant play at the same instant, to the sample. Per-channel
// timelines would sit at a random offset from each other and turn a stereo source into an
// audible echo.

const RAMP_S = 0.02;   // stepping a gain from 0 to 1 under a running stream is an audible click

// Every gain move rides this ramp — the monitor volume and the per-ear taps alike.
function rampGain(gainNode, target) {
  const g = gainNode.gain, t = monitor.ctx.currentTime;
  g.cancelScheduledValues(t);
  g.setValueAtTime(g.value, t);
  g.linearRampToValueAtTime(target, t + RAMP_S);
}

// Monitor volume. This is the browser's playback level, not the device's: it hangs off the far
// end of the graph, downstream of the merger, so it lifts every channel and both ears together
// and leaves the samples the daemon measures untouched. The per-input Gain slider is the other
// thing entirely — that one is make-up gain applied on the device, and it moves the meters.
const MON_MIN_DB = -20, MON_MAX_DB = 40;
const dbToLin = db => Math.pow(10, db / 20);
let monitorDb = 0;

// How far ahead of real time the browser schedules monitor audio: the playback buffer. A bigger
// buffer rides out a jittery link (fewer of the late re-anchors connWatch flags) but adds that
// much delay between the device and what you hear. Configurable from the dashboard.
const MON_LAT_MIN_MS = 50, MON_LAT_MAX_MS = 500;
let monLatency = 0.15;   // seconds

// Listen codec. PCM (raw S16) is the wire default and the always-available fallback; Opus is
// opt-in per connection (?codec=opus) and decoded in the browser by the vendored WASM decoder,
// which keeps channels sample-aligned exactly as PCM does (each chunk still carries its absolute
// capture index). listenCodec is the user's preference; opusAvailable gates it on both the daemon
// (a supported sample rate) and the decoder library having loaded.
let listenCodec = 'pcm';
let listenBitrate = 96;   // kbps, per channel
let opusRate = 48000;     // the rate the daemon encodes Opus at; the decoder is built for it
let opusAvailable = false;
const OpusDecoderClass = (window['opus-decoder'] && window['opus-decoder'].OpusDecoder) || null;

const monitor = {
  ctx: null,
  merger: null,    // input 0 = left ear, input 1 = right; Web Audio sums each input's connections
  master: null,    // monitor volume, applied to the summed mix
  anchorN: null,   // capture sample mapped to anchorT on the context clock
  anchorT: 0,

  ensure() {
    if (!this.ctx) {
      this.ctx = new (window.AudioContext || window.webkitAudioContext)({sampleRate: rate});
      this.merger = this.ctx.createChannelMerger(2);
      this.master = this.ctx.createGain();
      this.master.gain.value = dbToLin(monitorDb);   // the slider may have been moved before this
      this.merger.connect(this.master);
      this.master.connect(this.ctx.destination);
    }
    if (this.ctx.state === 'suspended') this.ctx.resume();
    return this.ctx;
  },

  setVolume(db) {
    if (!this.master) return;   // nothing is playing yet; ensure() will pick monitorDb up
    rampGain(this.master, dbToLin(db));
  },

  // Absolute capture sample -> context play time. Chunks are scheduled monLatency ahead of real
  // time (the playback buffer). When the mapped time falls outside its window — a late chunk, a
  // ring lap, or capture/DAC clock drift finally accumulating — the anchor shifts for EVERY stream
  // at once: one brief glitch on all channels, and they come out the other side still aligned with
  // each other. A per-stream recovery would not. The upper bound tracks the buffer so a larger
  // buffer is not itself mistaken for a discontinuity.
  timeFor(sample) {
    const now = this.ctx.currentTime;
    if (this.anchorN === null) {
      this.anchorN = sample;
      this.anchorT = now + monLatency;
    }
    let t = this.anchorT + (sample - this.anchorN) / rate;
    if (t < now + 0.02 || t > now + monLatency + 0.45) {
      // A chunk mapped into the past (or barely ahead) arrived too late to schedule — that is the
      // link falling behind, the one re-anchor cause we can pin on the connection. A far-future
      // map is a capture discontinuity or a start-up burst, so it re-anchors silently.
      if (t < now + 0.02) connWatch.drop();
      this.anchorT += (now + monLatency) - t;
      t = now + monLatency;
    }
    return t;
  },

  idle() {
    if (listeners.size === 0) this.anchorN = null;  // next session anchors fresh
  }
};

// A dropout you HEAR has two possible causes: the device's own xruns (already in the header) or a
// slow / lossy link between this browser and the Sound Tester. The scheduler re-anchors — one
// glitch across every channel — whenever a monitor chunk reaches us too late to play on time, and
// a live listen socket closing on its own is the same story. Both are the link, not the device, so
// they get their own banner. The count clears once the link has run clean for a few seconds.
const connWatch = {
  count: 0,
  timer: null,
  drop(reason) {
    this.count++;
    const b = $('connbanner');
    if (!b) return;
    $('conntext').textContent =
      `${this.count} dropout${this.count > 1 ? 's' : ''} in the last few seconds${reason ? ' — ' + reason : ''}.`;
    b.classList.remove('hidden');
    clearTimeout(this.timer);
    this.timer = setTimeout(() => { this.count = 0; b.classList.add('hidden'); }, 6000);
  },
};

class Listener {
  constructor(ch) {
    this.ch = ch;
    this.sides = {l: false, r: false};
    this.codec = (listenCodec === 'opus' && opusAvailable) ? 'opus' : 'pcm';
    this.dec = null;
    this.ws = null;
    this.closed = false;
    const ctx = monitor.ensure();

    // One socket per channel however many ears it feeds. Opening a second stream for the
    // second ear would carry identical audio and burn another of the device's twelve slots.
    this.taps = {l: ctx.createGain(), r: ctx.createGain()};
    this.taps.l.gain.value = 0;
    this.taps.r.gain.value = 0;
    this.taps.l.connect(monitor.merger, 0, 0);
    this.taps.r.connect(monitor.merger, 0, 1);

    if (this.codec === 'opus') this.connectOpus();
    else this.connect('pcm');
  }

  connect(codec) {
    if (this.closed) return;   // user stopped while the decoder was still compiling
    this.codec = codec;
    const q = codec === 'opus' ? '?codec=opus' : '';
    this.ws = new WebSocket(`ws://${location.host}/api/listen/${this.ch}${q}`);
    this.ws.binaryType = 'arraybuffer';
    this.ws.onmessage = e => this.onchunk(e.data);
    // A socket still feeding an ear that closes on its own is a dropped connection, not a user
    // stop (which clears the sides first, so this.live is already false by the time it closes).
    this.ws.onclose = () => {
      if (this.live) connWatch.drop('the audio stream dropped');
      this.stop();
    };
  }

  connectOpus() {
    // The decoder compiles its WASM asynchronously; open the socket only once it is ready, and
    // fall back to PCM if it fails to load so listening always works.
    let dec;
    try {
      dec = new OpusDecoderClass({channels: 1, streamCount: 1, coupledStreamCount: 0,
                                  channelMappingTable: [0], preSkip: 0, sampleRate: opusRate});
    } catch (e) { this.connect('pcm'); return; }
    this.dec = dec;
    dec.ready.then(() => this.connect('opus'))
             .catch(() => { this.dec = null; this.connect('pcm'); });
  }

  get live() { return this.sides.l || this.sides.r; }

  // Only the tap gains move. The stream, its scheduling and the shared anchor are untouched,
  // so routing a channel to the other ear — or dropping it and bringing it back — never
  // re-times anything: whatever is still playing stays sample-aligned with it.
  setSide(side, on) {
    this.sides[side] = on;
    rampGain(this.taps[side], on ? 1 : 0);
  }

  // Fed to both taps unconditionally; which ears actually hear it is the taps' gains. Scheduled
  // on the shared timeline by the chunk's absolute capture index.
  schedule(ab, start) {
    const src = monitor.ctx.createBufferSource();
    src.buffer = ab;
    src.connect(this.taps.l);
    src.connect(this.taps.r);
    src.start(monitor.timeFor(start));
  }

  // Opus: [u64 capture index][raw Opus packet]. Decode to Float32 @ opusRate and schedule on the
  // shared timeline by the SAME absolute index the PCM path uses, so channels stay sample-aligned.
  // A 48 kHz buffer in the 96 kHz context is resampled by the source node on playback.
  onopus(buf, start) {
    if (!this.dec) return;
    let out;
    try { out = this.dec.decodeFrame(new Uint8Array(buf, 8)); } catch (e) { return; }
    const n = out.samplesDecoded;
    if (!n) return;   // an encoder warm-up frame with nothing to play yet
    const ab = monitor.ctx.createBuffer(1, n, out.sampleRate);
    ab.getChannelData(0).set(out.channelData[0].subarray(0, n));
    this.schedule(ab, start);
  }

  onchunk(buf) {
    const ctx = monitor.ctx;
    const view = new DataView(buf);
    const start = Number(view.getBigUint64(0, true));
    if (this.codec === 'opus') { this.onopus(buf, start); return; }
    const pcm = new Int16Array(buf, 8);

    // Browsers honor {sampleRate} by resampling internally, so ratio is 1 in practice; the
    // fallback keeps the duration correct if a context ever comes back at another rate.
    const ratio = ctx.sampleRate / rate;
    const n = Math.round(pcm.length * ratio);
    const ab = ctx.createBuffer(1, n, ctx.sampleRate);
    const out = ab.getChannelData(0);
    if (ratio === 1) {
      for (let i = 0; i < n; i++) out[i] = pcm[i] / 32768;
    } else {
      for (let i = 0; i < n; i++) {
        const x = i / ratio, i0 = Math.floor(x), f = x - i0;
        const a = pcm[Math.min(i0, pcm.length - 1)] / 32768;
        const b = pcm[Math.min(i0 + 1, pcm.length - 1)] / 32768;
        out[i] = a + (b - a) * f;
      }
    }

    this.schedule(ab, start);
  }

  stop() {
    this.closed = true;
    try { if (this.ws) this.ws.close(); } catch (e) { /* already gone */ }
    if (this.dec) {
      // Free after the WASM is ready — free() throws if it never finished compiling.
      const d = this.dec;
      this.dec = null;
      Promise.resolve(d.ready).then(() => d.free()).catch(() => {});
    }
    // Only clear the registry slot if it is still ours. A codec switch replaces the entry with a
    // fresh Listener before this (old) socket's onclose fires a second stop(); without this guard
    // that late stop would evict the replacement.
    if (listeners.get(this.ch) === this) listeners.delete(this.ch);
    // Up to ~150 ms of this channel is already scheduled. Fade it, then tear the nodes down:
    // disconnecting immediately truncates that tail mid-waveform, which is a click.
    this.setSide('l', false);
    this.setSide('r', false);
    setTimeout(() => {
      try { this.taps.l.disconnect(); } catch (e) { /* already gone */ }
      try { this.taps.r.disconnect(); } catch (e) { /* already gone */ }
    }, RAMP_S * 1000 + 50);
    monitor.idle();
    syncListenButtons(this.ch);
  }
}

const listeners = new Map();

function syncListenButtons(ch) {
  const l = listeners.get(ch);
  for (const side of ['l', 'r']) {
    const btn = $('listen' + side.toUpperCase() + ch);
    if (btn) btn.classList.toggle('on', !!(l && l.sides[side]));
  }
}

function toggleListen(ch, side) {
  let l = listeners.get(ch);
  if (!l) {
    l = new Listener(ch);
    listeners.set(ch, l);
  }
  l.setSide(side, !l.sides[side]);
  if (l.live) syncListenButtons(ch); else l.stop();
}

// Survives a reload: on a bench you set the monitor level once for the device under test and
// then stop thinking about it.
function initMonitorVolume() {
  const saved = parseFloat(lsGet('monitor_db'));
  monitorDb = Number.isFinite(saved) ? Math.min(MON_MAX_DB, Math.max(MON_MIN_DB, saved)) : 0;

  const sl = $('monvol');
  sl.min = MON_MIN_DB;
  sl.max = MON_MAX_DB;
  sl.value = monitorDb;
  setMonitorLabel(monitorDb);
  sl.oninput = e => {
    monitorDb = parseFloat(e.target.value);
    setMonitorLabel(monitorDb);
    monitor.setVolume(monitorDb);
    lsSet('monitor_db', String(monitorDb));
  };
}

function setMonitorLabel(db) {
  const el = $('monvolv');
  el.textContent = db === 0 ? 'unity' : (db > 0 ? '+' : '') + db.toFixed(1);
  el.classList.toggle('active', db > 0);
}

// Also survives a reload, for the same bench reason. Stored in ms because that is what the slider
// and the readout speak; monLatency stays in seconds because that is what the scheduler speaks.
function initMonitorLatency() {
  let ms = parseFloat(lsGet('monitor_latency_ms'));
  ms = Number.isFinite(ms) ? Math.min(MON_LAT_MAX_MS, Math.max(MON_LAT_MIN_MS, ms)) : 150;
  monLatency = ms / 1000;

  const sl = $('monlat');
  sl.min = MON_LAT_MIN_MS;
  sl.max = MON_LAT_MAX_MS;
  sl.value = ms;
  setLatencyLabel(ms);
  sl.oninput = e => {
    const v = parseFloat(e.target.value);
    monLatency = v / 1000;
    setLatencyLabel(v);
    lsSet('monitor_latency_ms', String(v));
  };
  // Apply the new buffer once, on release: dropping the anchor re-establishes the lead at the new
  // depth on the next chunk. Doing it per input event would re-anchor once per pixel of the drag.
  sl.onchange = () => { monitor.anchorN = null; };
}

function setLatencyLabel(ms) {
  $('monlatv').textContent = `${Math.round(ms)} ms`;
}

// Listen codec + bitrate. PCM is always offered; Opus only when the daemon supports it at the
// current sample rate and the decoder library loaded. The choice is the console's preference
// (persisted, sent explicitly as ?codec=opus); the daemon's wire default stays PCM.
function initCodec() {
  const sel = $('listencodec');
  const opusOpt = sel.querySelector('option[value="opus"]');
  opusOpt.disabled = !opusAvailable;
  opusOpt.textContent = opusAvailable ? 'Opus' : 'Opus (unavailable)';
  if (!opusAvailable && listenCodec === 'opus') listenCodec = 'pcm';
  sel.value = listenCodec;
  sel.onchange = () => {
    listenCodec = (sel.value === 'opus' && opusAvailable) ? 'opus' : 'pcm';
    sel.value = listenCodec;
    lsSet('listen_codec', listenCodec);
    post('/listen/codec', {codec: listenCodec}).catch(e => toast(e.message));
    restartListeners();   // move any live listeners onto the new codec
    updateBitrateVisibility();
  };

  const sl = $('listenbitrate');
  sl.min = state.limits.listen_bitrate_min_kbps || 16;
  sl.max = state.limits.listen_bitrate_max_kbps || 256;
  sl.value = listenBitrate;
  setBitrateLabel(listenBitrate);
  sl.oninput = () => setBitrateLabel(parseInt(sl.value, 10));
  sl.onchange = () => {
    post('/listen/codec', {bitrate_kbps: parseInt(sl.value, 10)})
      .then(r => { listenBitrate = r.bitrate_kbps; sl.value = listenBitrate; setBitrateLabel(listenBitrate); })
      .catch(e => toast(e.message));
  };
  updateBitrateVisibility();
}

function setBitrateLabel(kbps) { $('listenbitratev').textContent = `${kbps} kbps`; }

// The bitrate only bites for Opus; dim it under PCM rather than hide it, so the layout stays put.
function updateBitrateVisibility() {
  $('bitratewrap').style.opacity = listenCodec === 'opus' ? '1' : '0.4';
}

// Switching codec reconnects any live listeners so they pick it up, preserving which ears each was
// feeding. A brief re-anchor glitch on this deliberate action is acceptable.
function restartListeners() {
  for (const [ch, li] of Array.from(listeners)) {
    const sides = {l: li.sides.l, r: li.sides.r};
    if (!(sides.l || sides.r)) continue;
    li.stop();
    const nl = new Listener(ch);
    listeners.set(ch, nl);
    if (sides.l) nl.setSide('l', true);
    if (sides.r) nl.setSide('r', true);
    syncListenButtons(ch);
  }
}

// ---------------------------------------------------------------- cards

const dbToPct = db => Math.max(0, Math.min(100, (db + 60) / 60 * 100));

// Padded to a fixed width, because the card's layout must not depend on how many digits the
// reading happens to have. .readout is white-space: pre so the padding survives.
const pad = (s, n) => String(s).padStart(n);
const fmtDb = db => pad(db > -119 ? db.toFixed(1) : '-inf', 6);
// Three decimals on a 38 % reading is noise, and it is four characters wider than the slot.
const fmtThd = p => pad(p >= 10 ? p.toFixed(1) : p >= 1 ? p.toFixed(2) : p.toFixed(3), 6);

// Per-input on/off, kept on the client and surviving a reload. Disabling an input stops the page
// rendering it (meters, spectrum, scope lane), closes any listen stream on it, and — via
// sendTelemetryMask() — tells the daemon to drop it from the spectrum message, the widest frame
// on the shared push feed.
const inputEnabled = Array(NIN).fill(true);

function loadInputEnabled() {
  let saved = null;
  try { saved = JSON.parse(lsGet('input_enabled')); } catch (e) { /* not JSON */ }
  for (let c = 0; c < NIN; c++) inputEnabled[c] = !Array.isArray(saved) || saved[c] !== false;
}

function saveInputEnabled() {
  lsSet('input_enabled', JSON.stringify(inputEnabled));
}

// The list of enabled input channels, in order — the lanes the scope shows and the windows the
// frozen view fetches.
const shownInputs = () => inputEnabled.flatMap((on, i) => (on ? [i] : []));

// The Delay pickers (from-IN / to-IN) only offer enabled inputs: a disabled lane carries no
// captured audio to correlate against. Rebuild #xa/#xb from the enabled set, keeping each current
// pick where it still exists and otherwise defaulting a→first enabled, b→a different enabled input.
// If disabling an input would collapse the pair onto one channel, nudge b to a distinct input (a
// deliberately equal pair is left alone). Disable Measure when nothing is enabled.
function syncDelaySelects() {
  const xa = $('xa'), xb = $('xb');
  if (!xa || !xb) return;
  const shown = shownInputs().map(i => i + 1);   // 1-based, matching the option values
  const prevA = parseInt(xa.value, 10), prevB = parseInt(xb.value, 10);
  const opts = shown.length
    ? shown.map(v => `<option value="${v}">IN ${v}</option>`).join('')
    : '<option value="">—</option>';
  xa.innerHTML = opts;
  xb.innerHTML = opts;
  if (shown.length) {
    const a = shown.includes(prevA) ? prevA : shown[0];
    let b = shown.includes(prevB) ? prevB : (shown.find(v => v !== a) || shown[0]);
    if (a === b && prevA !== prevB && shown.length > 1) b = shown.find(v => v !== a);
    xa.value = String(a);
    xb.value = String(b);
  }
  updateMeasureEnabled();
}

// Measure only works on a frozen snapshot with at least one input in view, so the button is disabled
// otherwise. The wrapper span carries the "why" as a title, because a disabled button swallows its
// own tooltip — pointer-events:none on the button lets the hover fall through to the wrapper.
function updateMeasureEnabled() {
  const btn = $('measure'), wrap = $('measurewrap');
  if (!btn) return;
  const frozen = srvFrozen();
  const haveInputs = shownInputs().length > 0;
  btn.disabled = !(frozen && haveInputs);
  const why = !frozen ? 'Press Analyze first — the delay is measured on the captured snapshot.'
            : !haveInputs ? 'Enable at least one input to measure a delay.'
            : '';
  if (wrap) wrap.title = why;
  btn.title = why ? '' : 'Cross-correlate the two selected inputs over the bracketed span (or the whole view when no cursors are set).';
}

function clearSpectrumCanvas(ch) {
  const cv = $('spec' + ch);
  if (!cv || !cv.width) return;
  cv.getContext('2d').clearRect(0, 0, cv.width, cv.height);
}

// Reflect one input's enabled state across the card: dim the body, freeze the readouts at a muted
// placeholder, blank the spectrum, disable the Listen buttons and drop any live listen stream.
function applyInputEnabled(ch) {
  const on = inputEnabled[ch];
  const card = document.querySelector(`#inputs .card:nth-child(${ch + 1})`);
  if (card) card.classList.toggle('disabled', !on);
  const cb = $('en' + ch);
  if (cb) cb.checked = on;
  const lane = $('lane' + ch);   // the twin checkbox on the Scope tab, kept in step
  if (lane) lane.checked = on;
  for (const s of ['L', 'R']) { const b = $('listen' + s + ch); if (b) b.disabled = !on; }
  if (!on) {
    const li = listeners.get(ch);
    if (li) li.stop();
    const set = (id, v) => { const el = $(id + ch); if (el) el.textContent = v; };
    set('rmsv', `${pad('off', 6)} dBFS`);
    set('pkv', `pk ${pad('—', 6)}`);
    set('tone', `${pad('—', 7)} Hz`);
    set('thd', `THD+N ${pad('—', 6)}%`);
    const rms = $('rms' + ch), pk = $('pk' + ch);
    if (rms) rms.style.width = '0%';
    if (pk) pk.style.left = '0%';
    delete barState[ch];
    clearSpectrumCanvas(ch);
  }
  scopeDirty = true;
}

// One entry point for both checkbox sets (dashboard #en, scope #lane): flip the state, persist it,
// reflect it everywhere (applyInputEnabled ticks both twins), refetch the frozen scope without the
// dropped lane, and tell the daemon.
function setInputEnabled(ch, on) {
  inputEnabled[ch] = on;
  saveInputEnabled();
  applyInputEnabled(ch);
  syncDelaySelects();
  invalidateWindows();
  sendTelemetryMask();
}

// Tell the daemon which inputs to keep in the spectrum message so the shared feed actually
// shrinks. Gated on the limits flag: a deployed daemon from before the endpoint existed omits
// it, and this page then degrades to a client-side-only disable rather than POSTing into a 404.
function sendTelemetryMask() {
  if (!(state && state.limits && state.limits.telemetry_mask)) return;
  post('/telemetry/inputs', {enabled: inputEnabled.slice()}).catch(() => { /* best effort */ });
}

function buildInputs() {
  loadInputEnabled();
  // A daemon built before input gain existed sends no gain_db; render no slider rather than
  // a control wired to a 404.
  const hasGain = state.inputs.length && state.inputs[0].gain_db !== undefined;
  $('gainnote').classList.toggle('hidden', !hasGain);

  $('inputs').innerHTML = state.inputs.map(i => `
    <div class="card">
      <div class="chan-head">
        <label class="en" title="Enable / disable this input"><input type="checkbox"
          id="en${i.ch}" ${inputEnabled[i.ch] ? 'checked' : ''}></label>
        <span class="chan-name">IN ${i.ch + 1}${i.name ? ' — ' + i.name : ''}</span>
        <span class="listen-grp">
          <button id="listenL${i.ch}" class="lbtn">Listen L</button>
          <button id="listenR${i.ch}" class="lbtn">Listen R</button>
        </span>
      </div>
      <div class="meter"><div class="rms" id="rms${i.ch}"></div><div class="peak" id="pk${i.ch}"></div></div>
      <div class="readout">
        <span id="rmsv${i.ch}" class="mono">     — dBFS</span>
        <span id="pkv${i.ch}" class="mono rt">pk      —</span>
        <span id="tone${i.ch}" class="mono">      — Hz</span>
        <span id="thd${i.ch}" class="mono rt">THD+N      —%</span>
      </div>
      ${hasGain ? `<label>Gain <input type="range" id="igain${i.ch}"
             min="${gainMinDb}" max="${gainMaxDb}" step="0.5">
        <span id="igainv${i.ch}" class="mono val"></span> dB</label>` : ''}
      <canvas class="mini" id="spec${i.ch}"></canvas>
    </div>`).join('');

  state.inputs.forEach(i => {
    const c = i.ch;
    $('listenL' + c).onclick = () => toggleListen(c, 'l');
    $('listenR' + c).onclick = () => toggleListen(c, 'r');
    $('en' + c).onchange = e => setInputEnabled(c, e.target.checked);
    applyInputEnabled(c);   // reflect the persisted state on this fresh card
    const nm = document.querySelector(`#inputs .card:nth-child(${c + 1}) .chan-name`);
    if (nm) nm.title = nm.textContent;   // the name now ellipsises, so keep it readable on hover
    if (!hasGain) return;
    setInputGainLabel(c, i.gain_db);
    $('igain' + c).value = i.gain_db;
    $('igain' + c).oninput = e => {
      const db = parseFloat(e.target.value);
      setInputGainLabel(c, db);
      put(`/inputs/${c}`, {gain_db: db}).catch(err => toast(err.message));
    };
  });
}

// +0.0 dB is the untouched signal; say "unity" so zero is not misread as muted.
function setInputGainLabel(ch, db) {
  const el = $('igainv' + ch);
  if (!el) return;
  el.textContent = db > 0 ? '+' + db.toFixed(1) : 'unity';
  el.classList.toggle('active', db > 0);
}

// The ping generator has one global variant (tick/bing/bong) shared by every output routed to it.
// We surface those variants directly in each output's Source list — picking one both routes the
// output to the ping generator and sets that shared variant — so there is no separate control to
// hunt for. Value form: "ping:<variant>".
function pingVariant() {
  return (state.generators && state.generators.ping && state.generators.ping.variant) || 'tick';
}

function sourceValue(src) {
  if (!src || src.type === 'silence') return 'silence';
  if (src.type === 'input') return 'in' + src.index;
  if (src.index === 'ping') return 'ping:' + pingVariant();
  return 'gen' + src.index;
}

// Changing the shared variant re-labels every OTHER output already on a ping, so no dropdown lies
// about which sound it now emits. DOM-driven so it works even for outputs just routed this session.
function setPingVariant(variant) {
  if (state.generators && state.generators.ping) state.generators.ping.variant = variant;
  const val = 'ping:' + variant;
  state.outputs.forEach(o => {
    const el = $('src' + o.ch);
    if (el && el.value.startsWith('ping:')) el.value = val;
  });
  return put('/generators/ping', {variant});
}

function buildOutputs() {
  const opts = ['<option value="silence">Silence</option>']
    .concat([...Array(NIN).keys()].map(i => `<option value="in${i}">IN ${i + 1}</option>`))
    .concat([
      '<option value="gensine">Sine</option>',
      '<option value="gennoise">Noise</option>',
      '<option value="ping:tick">Test: tick</option>',
      '<option value="ping:bing">Test: bing</option>',
      '<option value="ping:bong">Test: bong</option>',
    ])
    .join('');

  $('outputs').innerHTML = state.outputs.map(o => `
    <div class="card">
      <div class="chan-head">
        <span class="chan-name">OUT ${o.ch + 1}${o.name ? ' — ' + o.name : ''}</span>
        <button id="id${o.ch}">Identify</button>
      </div>
      <label>Source <select id="src${o.ch}">${opts}</select></label>
      <label>Gain <input type="range" id="gain${o.ch}" min="-60" max="0" step="0.5">
        <span id="gainv${o.ch}" class="mono val"></span> dB</label>
      <label><input type="checkbox" id="mute${o.ch}"> Mute</label>
    </div>`).join('');

  state.outputs.forEach(o => {
    const c = o.ch;
    $('src' + c).value = sourceValue(o.source);
    $('gain' + c).value = o.gain_db;
    $('gainv' + c).textContent = o.gain_db.toFixed(1);
    $('mute' + c).checked = o.mute;

    $('src' + c).onchange = e => {
      const v = e.target.value;
      if (v.startsWith('ping:')) {
        // A ping sound: set the shared variant and route this output to the ping generator.
        setPingVariant(v.slice(5)).catch(err => toast(err.message));
        put(`/outputs/${c}`, {source: {type: 'gen', index: 'ping'}}).catch(err => toast(err.message));
        return;
      }
      let source;
      if (v === 'silence') source = {type: 'silence'};
      else if (v.startsWith('in')) source = {type: 'input', index: parseInt(v.slice(2), 10)};
      else source = {type: 'gen', index: v.slice(3)};
      put(`/outputs/${c}`, {source}).catch(err => toast(err.message));
    };
    $('gain' + c).oninput = e => {
      $('gainv' + c).textContent = parseFloat(e.target.value).toFixed(1);
      put(`/outputs/${c}`, {gain_db: parseFloat(e.target.value)}).catch(err => toast(err.message));
    };
    $('mute' + c).onchange = e =>
      put(`/outputs/${c}`, {mute: e.target.checked}).catch(err => toast(err.message));
    $('id' + c).onclick = () => post(`/outputs/${c}/identify`).catch(err => toast(err.message));
  });
}

// A live bar spectrum-analyser per input. The daemon pushes 240 log-spaced dB bins at 5 Hz; we fold
// them into SPEC_BARS bars and animate at 60 fps (spectraTick) so the display glides instead of
// stepping five times a second. State is kept per channel so a tab switch or a resize can repaint
// from the current bar heights — that is when the canvas has just been handed a new backing store —
// without waiting for the next message.
const barState = {};        // ch -> {targ, cur, peak: Float32Array(SPEC_BARS)}
const SPEC_BARS = 40;       // 240 bins / 40 = 6 bins per bar (divides evenly)
const BAR_RISE = 0.35;      // fast attack when the level climbs
const BAR_FALL = 0.12;      // gentler release when it drops
const PEAK_FALL = 0.006;    // the peak-hold cap sinks this fraction of full scale per frame

// Each whole bar is one flat colour chosen by its height, sweeping smoothly green -> yellow ->
// orange -> red as it climbs — a continuous level cue with no visible steps. v is normalised height
// (0..1 over -100..0 dBFS); we interpolate the HSL hue linearly from 150 deg (green) down to 0 deg
// (red), so the colour tracks height gradiently.
const barColor = v => `hsl(${(1 - Math.max(0, Math.min(1, v))) * 150}, 65%, 52%)`;

// Fold one 240-bin spectrum message into SPEC_BARS bar targets. Max within each group, so a lone
// tone still lights its whole bar; same dB->height mapping the old trace used (-100..0 dBFS -> 0..1).
function ingestSpectrum(ch, bins) {
  if (!bins || bins.length < 2) return;
  let st = barState[ch];
  if (!st) st = barState[ch] = {
    targ: new Float32Array(SPEC_BARS), cur: new Float32Array(SPEC_BARS),
    peak: new Float32Array(SPEC_BARS),
  };
  const per = bins.length / SPEC_BARS;
  for (let i = 0; i < SPEC_BARS; i++) {
    const k0 = Math.floor(i * per), k1 = Math.min(bins.length, Math.floor((i + 1) * per));
    let m = -Infinity;
    for (let k = k0; k < k1; k++) if (bins[k] > m) m = bins[k];
    st.targ[i] = Math.max(0, Math.min(1, (m + 100) / 100));
  }
}

// One frame of easing: heights chase their targets (fast up, slow down), peak caps settle downward.
function advanceSpectrumBars(st) {
  for (let i = 0; i < SPEC_BARS; i++) {
    const t = st.targ[i], c = st.cur[i];
    st.cur[i] = c + (t - c) * (t > c ? BAR_RISE : BAR_FALL);
    st.peak[i] = st.cur[i] > st.peak[i] ? st.cur[i] : Math.max(st.cur[i], st.peak[i] - PEAK_FALL);
  }
}

function drawSpectrumBars(ch) {
  const st = barState[ch];
  const cv = $('spec' + ch);
  if (!cv || !st) return;
  const w = cv.clientWidth, h = cv.clientHeight;
  // Zero while the Dashboard is hidden. Sizing the canvas to it would blank the display and nothing
  // would repaint it, because the next message would find the width unchanged.
  if (!w || !h) return;
  if (cv.width !== w) cv.width = w;
  if (cv.height !== h) cv.height = h;
  const g = cv.getContext('2d');
  g.clearRect(0, 0, w, h);

  const bw = w / SPEC_BARS, bwFill = Math.max(1, bw - 1);
  for (let i = 0; i < SPEC_BARS; i++) {
    const v = st.cur[i], bh = v * h;
    if (bh > 0.5) { g.fillStyle = barColor(v); g.fillRect(i * bw + 0.5, h - bh, bwFill, bh); }
  }
  // Peak-hold caps: a thin bright tick riding above each bar.
  g.fillStyle = 'rgba(230,233,239,0.75)';
  for (let i = 0; i < SPEC_BARS; i++) {
    if (st.peak[i] > 0.01) g.fillRect(i * bw + 0.5, h - st.peak[i] * h - 1, bwFill, 2);
  }
}

// Force an immediate repaint of every live bar display from its current heights — used after a tab
// switch or resize, so the view is not blank for the frame before spectraTick next runs.
function redrawSpectra() {
  for (const key of Object.keys(barState)) {
    const ch = Number(key);
    if (inputEnabled[ch]) drawSpectrumBars(ch);
  }
}

// Dashboard render loop, twin of scopeTick: while the Dashboard is showing, ease every input's bars
// toward the latest spectrum targets and repaint. Gated on the tab so it idles elsewhere; targets
// keep updating in the background via ingestSpectrum, so a return to the tab catches up smoothly.
function spectraTick() {
  if (tabActive('dash')) {
    for (const key of Object.keys(barState)) {
      const ch = Number(key);
      if (!inputEnabled[ch]) continue;
      advanceSpectrumBars(barState[ch]);
      drawSpectrumBars(ch);
    }
  }
  requestAnimationFrame(spectraTick);
}

// ---------------------------------------------------------------- generators

function bindGenerators() {
  const g = state.generators;
  const report = e => toast(e.message);
  $('sinefreq').value = g.sine.freq_hz;
  $('sinelevel').value = g.sine.level_db;
  $('sinelevelv').textContent = g.sine.level_db.toFixed(1);
  $('noisemode').value = g.noise.mode;
  $('noiselevel').value = g.noise.level_db;
  $('noiselevelv').textContent = g.noise.level_db.toFixed(1);
  $('pinginterval').value = g.ping.interval_s;
  $('pinglevel').value = g.ping.level_db;
  $('pinglevelv').textContent = g.ping.level_db.toFixed(1);

  $('sinefreq').onchange = e => put('/generators/sine', {freq_hz: parseFloat(e.target.value)}).catch(report);
  $('sinelevel').oninput = e => {
    $('sinelevelv').textContent = parseFloat(e.target.value).toFixed(1);
    put('/generators/sine', {level_db: parseFloat(e.target.value)}).catch(report);
  };
  $('noisemode').onchange = e => put('/generators/noise', {mode: e.target.value}).catch(report);
  $('noiselevel').oninput = e => {
    $('noiselevelv').textContent = parseFloat(e.target.value).toFixed(1);
    put('/generators/noise', {level_db: parseFloat(e.target.value)}).catch(report);
  };
  $('pinginterval').onchange = e => put('/generators/ping', {interval_s: parseFloat(e.target.value)}).catch(report);
  $('pinglevel').oninput = e => {
    $('pinglevelv').textContent = parseFloat(e.target.value).toFixed(1);
    put('/generators/ping', {level_db: parseFloat(e.target.value)}).catch(report);
  };
  // The 1 s ping poll is started (and stopped) with the live feed, not here.
}

let pings = [];
function refreshPings() {
  if (!tabActive('config') && !tabActive('scope')) return;
  api('/pings/recent').then(list => {
    pings = list;
    const el = $('pinglog');
    if (!el) return;
    el.innerHTML = list.slice(-8).reverse()
      .map(p => `${p.variant} @ ${p.sample} (${(p.sample / rate).toFixed(3)} s)`).join('<br>')
      || '<span class="muted">none yet</span>';
  }).catch(() => {});
}

// ---------------------------------------------------------------- channel map

function buildMap() {
  const cm = state.channel_map;
  $('imap').innerHTML = cm.input_map.map((v, i) => `
    <label>IN ${i + 1} &larr; slot
      <select id="im${i}">${[...Array(8).keys()].map(s => `<option value="${s}"${s === v ? ' selected' : ''}>${s}</option>`).join('')}</select>
    </label>`).join('');
  $('omap').innerHTML = cm.output_map.map((v, i) => `
    <label>OUT ${i + 1} &rarr; slot
      <select id="om${i}">${[...Array(8).keys()].map(s => `<option value="${s}"${s === v ? ' selected' : ''}>${s}</option>`).join('')}</select>
    </label>`).join('');

  $('savemap').onclick = () => {
    const input_map = [...Array(NIN).keys()].map(i => parseInt($('im' + i).value, 10));
    const output_map = [...Array(NOUT).keys()].map(i => parseInt($('om' + i).value, 10));
    put('/channel-map', {input_map, output_map})
      .then(() => { $('mapmsg').textContent = 'applied'; })
      .catch(e => { $('mapmsg').textContent = e.message; });
  };
}

// ---------------------------------------------------------------- scope
//
// Two data sources, one canvas. Live: the WS envelope stream (480-frame min/max columns —
// 5 ms buckets, fine for watching levels scroll by). Frozen: /api/capture/window against the
// server's snapshot, which serves min/max at canvas resolution and raw samples once the view
// is narrow enough. The envelope stream alone can never show a single sample — zooming past
// its 5 ms floor needs the snapshot.

// The scope lanes are the same on/off state as the dashboard's per-input enable: disabling an
// input hides its lane and vice versa. There is one source of truth, inputEnabled, and the two
// checkbox sets are kept in step through applyInputEnabled().
// Widest the scope zooms out. Keep this many seconds of columns so that span is backed by data:
// the server backfills only its own 60 s envelope on connect, and the rest fills in from the live
// stream while the page stays open.
const MAX_SPAN_S = 300;
const FIT_SPAN_S = 8;                // span the Fit button resets the view to, and the default view
const ENV_KEEP = MAX_SPAN_S * 200;   // 300 s at 200 col/s (96 kHz / 480-frame columns)

let view = {start: 0, len: 96000 * FIT_SPAN_S};   // absolute samples
let cursorA = null, cursorB = null;
let paused = false;        // not following the live edge (auto: panned into history, or held)
let held = false;          // Pause was pressed: hold the view until Play, whatever the view shows
let envCols = [];          // {sample, min[6], max[6]}
let scopeDirty = false;
// Waveform amplitude magnification (vertical zoom). 1x = a full-scale sample spans the lane;
// magnifying flat-tops anything past the lane edge. Purely a display setting — it never touches
// the captured samples or the spectrogram lanes.
let vzoom = 1;
const VZOOM_MIN = 1, VZOOM_MAX = 64;

const srvFrozen = () => !!(state && state.capture && state.capture.frozen);

// The newest envelope column's sample — where "now" is on the scope's axis. Null before the
// first column arrives.
const liveEdge = () => (envCols.length ? envCols[envCols.length - 1].sample : null);

// Whether the scope follows is decided by what the view shows: if it contains the live edge, it
// follows; pan or zoom into history and it holds still, and it picks up again the moment a pan
// or zoom brings the live edge back in. Do not go back to "any zoom or pan pauses until Fit is
// pressed": zooming out of a running wave then reads as a hang, since the view stops following
// and the live edge runs off-screen. The slack covers the scroll easing, which trails the live
// edge by up to a quarter of the per-frame advance.
// keepPaused: a zoom passes true so that, once you are inspecting history, zooming in and back
// out never yanks the view to the live edge — you resume by panning to the edge or pressing Play.
// A pan passes nothing, so panning to the edge still resumes. A live view (paused === false) is
// unaffected either way, so zooming a running wave keeps following as before.
function updateFollow(keepPaused) {
  if (srvFrozen()) return;
  const last = liveEdge();
  if (held || (keepPaused && paused)) {
    // Latched by Pause, or holding a zoom in history: stay put even if the edge is back in view.
    paused = true;
  } else if (last !== null) {
    paused = view.start + view.len < last - 20 * envColumnFrames;
  } else {
    paused = false;
  }
  setCapState(paused ? 'paused' : 'live', paused ? 'press Play to catch up' : '');
}

// The state is one of three words, and the sentence that explains it lives in a separate slot
// that clips. Both are rewritten on every mousemove of a pan, so skip a write that changes
// nothing: a text node replaced 60 times a second dirties the toolbar's layout 60 times.
function setCapState(kind, detail) {
  const el = $('capstate'), d = $('capdetail');
  if (el.textContent === kind && d.textContent === detail) return;
  el.textContent = kind;
  el.className = kind;
  d.textContent = detail;
  d.title = detail;
  updatePlayPause();
  syncLaneToggles();
}

// The button reads the state rather than owning it: "Pause" while the view is following the live
// edge, "Play" once it is held, and disabled while frozen, where live scroll has no meaning.
function updatePlayPause() {
  const b = $('playpause');
  if (!b) return;
  const frozen = srvFrozen();
  const playing = !frozen && !paused;
  b.disabled = frozen;
  b.textContent = playing ? 'Pause' : 'Play';
  b.classList.toggle('playing', playing);
}

const frozenDetail = cs => `at ${cs.freeze_sample} · ${(cs.valid_len / rate).toFixed(1)} s of history`;

function scopeMsg(text) {
  const el = $('scopemsg');
  if (!el) return;
  el.textContent = text || '';
  el.title = text || '';   // daemon errors have no fixed length and the line clips
}

// The result box keeps its place in the layout whether or not it holds a result. Unhiding it
// used to move the canvas the moment you had finished lining the cursors up on it.
const XCORR_IDLE = 'Press Analyze, then Measure. Bracket one ping with cursor A and cursor B first, ' +
  'or leave the cursors clear to measure across the whole view.';

function clearResult() {
  const el = $('xcorrout');
  el.classList.remove('on');
  el.textContent = XCORR_IDLE;
}

// Frozen-mode cache: one window per visible lane, keyed by channel. Entries carry their own
// absolute start/len, so a pan draws the cached region in its right place (blank elsewhere)
// until the debounced refetch lands.
let winCache = {};
let winTimer = null;
let winSeq = 0;
// The zoom the live view had when Analyze was pressed, so going back to live returns to it
// instead of keeping whatever narrow span you zoomed to while inspecting the snapshot. Null
// when not entered via the Analyze button.
let frozenReturnLen = null;

// How many columns to ask the daemon for: the canvas width, clamped to the daemon's 2048-col
// cap. clientWidth, not width: the backing store is still the canvas's 300 px default until the
// Scope tab has been drawn once, and a freeze from another browser can land before that.
const scopeCols = () => Math.min(2048, Math.max(64, $('scopecanvas').clientWidth || 1024));

// The frozen snapshot is a fixed span — the configured Analyze buffer (default 20 s, up to the
// ~85 s maximum), often less — and its window endpoint 400s outright for any request that
// reaches past [valid_start, valid_end). Keep the frozen view inside that span: never zoom out
// wider than the snapshot, never pan off its ends. Without this, widening the scope to 300 s
// (live-buffer depth) lets a freeze zoom or pan into blank the snapshot can't fill, and the
// straddling fetch fails instead of returning what it has.
function clampFrozenView() {
  if (!srvFrozen()) return;
  const cap = state.capture;
  if (view.len > cap.valid_len) view.len = cap.valid_len;
  const maxStart = cap.valid_start + cap.valid_len - view.len;
  view.start = Math.max(cap.valid_start, Math.min(maxStart, view.start));
}

function invalidateWindows() {
  if (!srvFrozen()) return;
  clampFrozenView();
  clearTimeout(winTimer);
  winTimer = setTimeout(fetchWindows, 120);
  invalidateSpectrograms();
}

// The frozen and live states are entered from four places — the Analyze button, Clear, the 5 s
// poll noticing another browser flipped the shared capture, and the initial load — so the
// transition lives in one pair of functions. enterLive resets both pause latches: a Pause
// latched locally must not survive a resume and silently re-pause the view later.
function enterFrozen(cs) {
  state.capture = cs;
  $('freeze').textContent = 'Live';
  setCapState('frozen', frozenDetail(cs));
  scopeMsg('');
  updateMeasureEnabled();
  fetchWindows();
  invalidateSpectrograms();
  scopeDirty = true;
}

function enterLive() {
  state.capture.frozen = false;
  paused = false;
  held = false;
  winCache = {};
  sgCache = {};
  $('freeze').textContent = 'Analyze';
  setCapState('live', '');
  scopeMsg('');
  updateMeasureEnabled();
  scopeDirty = true;
}

function fetchWindows() {
  if (!srvFrozen()) return;
  clampFrozenView();
  const cap = state.capture;
  const lo = Math.max(view.start, cap.valid_start);
  const hi = Math.min(view.start + view.len, cap.valid_start + cap.valid_len);
  if (hi <= lo) { winCache = {}; scopeDirty = true; return; }

  const len = Math.floor(hi - lo);
  const cols = scopeCols();
  const seq = ++winSeq;
  // A disabled input is not fetched at all: these windows are pulled per channel on demand.
  const shown = shownInputs();

  Promise.all(shown.map(ch =>
    api(`/capture/window?ch=${ch}&start=${Math.floor(lo)}&len=${len}&cols=${cols}`)
      .then(w => [ch, w]).catch(() => null)))
    .then(rs => {
      if (seq !== winSeq) return;   // a newer view superseded this fetch mid-flight
      winCache = {};
      rs.filter(Boolean).forEach(([ch, w]) => { winCache[ch] = w; });
      scopeDirty = true;
    });
}

// ---------------------------------------------------------------- spectrogram
//
// A frozen-only, per-channel option: a scope lane can show its waveform or a spectrogram of the
// SAME zoomed/panned span. The daemon has no FFT for this — we pull the frozen PCM through the
// existing /api/capture/window endpoint (raw is capped at 4096 frames/request) and run the STFT
// here. Cost is bounded to a request budget per lane so it never scales with the ~85 s buffer:
// a contiguous fetch + dense STFT while the span is narrow, one window per column (sparse) once it
// is wide. Time on X (shares the scope's view), log frequency on Y, colour is level in dBFS.

const laneMode = Array(NIN).fill('wave');   // 'wave' | 'spectro' per input, persisted
try {
  const saved = JSON.parse(lsGet('lane_mode') || '[]');
  for (let c = 0; c < NIN; c++) if (saved[c] === 'spectro') laneMode[c] = 'spectro';
} catch (e) { /* not JSON */ }
function saveLaneMode() {
  lsSet('lane_mode', JSON.stringify(laneMode));
}

// Live-adjustable display settings. FFT is also the per-column window and must stay <= RAWCAP so a
// sparse column is one request. Floor is the colour floor (ceiling is 0 dBFS).
let sgFft = parseInt(lsGet('sg_fft'), 10) || 2048;
let sgFloorDb = parseInt(lsGet('sg_floor'), 10) || -100;
let sgMap = lsGet('sg_map') || 'magma';

const RAWCAP = 4096;      // /api/capture/window returns raw only for len <= 2*cols, cols <= 2048
const SG_BUDGET = 96;     // max raw requests per lane per refresh
const SG_NBINS = 256;     // offscreen height; blitted to the actual lane height
const SG_LOW_HZ = 20;     // bottom of the log frequency axis
// Frequency gridlines the ruler draws across a spectrogram lane (those in [SG_LOW_HZ, Nyquist)).
const FREQ_TICKS = [50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 40000];

let sgCache = {};       // ch -> {start,len,ncols,nbins,db:Float32Array,off:canvas} | {narrow}|{empty}
let sgTimer = null;
let sgSeq = 0;

// --- radix-2 FFT, tables cached per size ---
const fftCache = {};
function getFFT(N) {
  if (fftCache[N]) return fftCache[N];
  const rev = new Uint32Array(N);
  for (let i = 0, j = 0; i < N; i++) {
    rev[i] = j;
    let bit = N >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
  }
  const cos = new Float32Array(N / 2), sin = new Float32Array(N / 2);
  for (let i = 0; i < N / 2; i++) { const a = -2 * Math.PI * i / N; cos[i] = Math.cos(a); sin[i] = Math.sin(a); }
  return (fftCache[N] = {N, rev, cos, sin});
}
// In-place complex FFT (decimation-in-time) on parallel re/im arrays of length f.N.
function fft(re, im, f) {
  const N = f.N, rev = f.rev;
  for (let i = 0; i < N; i++) {
    const j = rev[i];
    if (j > i) { let t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (let len = 2; len <= N; len <<= 1) {
    const half = len >> 1, step = N / len;
    for (let i = 0; i < N; i += len) {
      for (let k = 0, idx = 0; k < half; k++, idx += step) {
        const c = f.cos[idx], s = f.sin[idx];
        const ar = re[i + k + half], ai = im[i + k + half];
        const tr = ar * c - ai * s, ti = ar * s + ai * c;
        re[i + k + half] = re[i + k] - tr; im[i + k + half] = im[i + k] - ti;
        re[i + k] += tr; im[i + k] += ti;
      }
    }
  }
}

// Hann window + its sum, cached. Bin amplitude = 2*|X|/sum(window), so a full-scale sine reads 0 dB.
const sgWin = {};
function hann(N) {
  if (sgWin[N]) return sgWin[N];
  const w = new Float32Array(N);
  let sum = 0;
  for (let i = 0; i < N; i++) { w[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / (N - 1)); sum += w[i]; }
  return (sgWin[N] = {w, sum});
}

// Each of SG_NBINS log-spaced display rows (20 Hz..Nyquist) maps to a linear FFT-bin range, reduced
// by peak-hold. Cached per (N, rate).
const sgBinMap = {};
function binMap(N) {
  const key = N + ':' + rate;
  if (sgBinMap[key]) return sgBinMap[key];
  const nyq = rate / 2, lo = SG_LOW_HZ, r = new Uint32Array(SG_NBINS * 2);
  for (let i = 0; i < SG_NBINS; i++) {
    const f0 = lo * Math.pow(nyq / lo, i / SG_NBINS);
    const f1 = lo * Math.pow(nyq / lo, (i + 1) / SG_NBINS);
    let k0 = Math.max(1, Math.floor(f0 * N / rate));
    let k1 = Math.max(k0 + 1, Math.ceil(f1 * N / rate));
    if (k1 > N / 2 + 1) k1 = N / 2 + 1;
    r[i * 2] = k0; r[i * 2 + 1] = k1;
  }
  return (sgBinMap[key] = r);
}

const sgScratch = {re: null, im: null};
// FFT one window of `src` at `off`, reduce to SG_NBINS log dB rows written at out[base..].
function sgColumn(src, off, out, base) {
  const N = sgFft, f = getFFT(N), win = hann(N), map = binMap(N);
  if (!sgScratch.re || sgScratch.re.length !== N) { sgScratch.re = new Float32Array(N); sgScratch.im = new Float32Array(N); }
  const re = sgScratch.re, im = sgScratch.im;
  for (let i = 0; i < N; i++) { re[i] = (src[off + i] || 0) * win.w[i]; im[i] = 0; }
  fft(re, im, f);
  const norm = 2 / win.sum;
  for (let i = 0; i < SG_NBINS; i++) {
    const k0 = map[i * 2], k1 = map[i * 2 + 1];
    let p = 0;
    for (let k = k0; k < k1; k++) { const m = re[k] * re[k] + im[k] * im[k]; if (m > p) p = m; }
    const amp = Math.sqrt(p) * norm;
    out[base + i] = amp > 1e-9 ? 20 * Math.log10(amp) : -120;
  }
}

// A handful of anchor stops per map, interpolated to a 256-entry RGB LUT (no libraries).
const CMAPS = {
  magma: [[0,0,4],[28,16,68],[79,18,123],[129,37,129],[181,54,122],[229,80,100],[251,135,97],[254,194,135],[252,253,191]],
  inferno: [[0,0,4],[31,12,72],[85,15,109],[136,34,106],[186,54,85],[227,89,51],[249,140,10],[249,201,50],[252,255,164]],
  viridis: [[68,1,84],[70,50,127],[54,92,141],[39,127,142],[31,161,135],[74,194,109],[159,218,58],[253,231,37]],
  grey: [[0,0,0],[255,255,255]],
};
function buildColormap(name) {
  const stops = CMAPS[name] || CMAPS.magma, lut = new Uint8Array(256 * 3);
  for (let i = 0; i < 256; i++) {
    const t = i / 255 * (stops.length - 1), a = Math.min(stops.length - 1, Math.floor(t)), b = Math.min(stops.length - 1, a + 1), fr = t - a;
    for (let c = 0; c < 3; c++) lut[i * 3 + c] = Math.round(stops[a][c] + (stops[b][c] - stops[a][c]) * fr);
  }
  return lut;
}
let cmapLut = buildColormap(sgMap);

// Paint sc.db (col-major, row 0 = low freq) into an offscreen canvas ncols x SG_NBINS, top = high.
function paintSpectrogram(sc) {
  const off = sc.off || (sc.off = document.createElement('canvas'));
  off.width = sc.ncols; off.height = SG_NBINS;
  const g = off.getContext('2d'), img = g.createImageData(sc.ncols, SG_NBINS), d = img.data;
  const floor = sgFloorDb, span = -floor;
  for (let x = 0; x < sc.ncols; x++) {
    const cb = x * SG_NBINS;
    for (let y = 0; y < SG_NBINS; y++) {
      const db = sc.db[cb + (SG_NBINS - 1 - y)];
      let t = (db - floor) / span; t = t < 0 ? 0 : t > 1 ? 1 : t;
      const li = (t * 255) | 0, p = (y * sc.ncols + x) * 4;
      d[p] = cmapLut[li * 3]; d[p + 1] = cmapLut[li * 3 + 1]; d[p + 2] = cmapLut[li * 3 + 2]; d[p + 3] = 255;
    }
  }
  g.putImageData(img, 0, 0);
}

function recolorSpectrograms() {
  cmapLut = buildColormap(sgMap);
  for (const k of Object.keys(sgCache)) { const sc = sgCache[k]; if (sc && sc.db) paintSpectrogram(sc); }
  scopeDirty = true;
}

// Recompute every spectro lane for the current frozen view, debounced. Called wherever the frozen
// view or the lane modes change; clears the cache and bails when live.
function invalidateSpectrograms() {
  if (!srvFrozen()) { sgCache = {}; return; }
  clearTimeout(sgTimer);
  sgTimer = setTimeout(fetchSpectrograms, 150);
}

function fetchSpectrograms() {
  if (!srvFrozen()) { sgCache = {}; return; }
  clampFrozenView();
  const cap = state.capture;
  const chans = shownInputs().filter(i => laneMode[i] === 'spectro');
  for (const k of Object.keys(sgCache)) if (!chans.includes(Number(k))) delete sgCache[k];
  updateSgCtl();
  if (!chans.length) { scopeDirty = true; return; }

  const lo = Math.max(view.start, cap.valid_start);
  const hi = Math.min(view.start + view.len, cap.valid_start + cap.valid_len);
  const span = Math.floor(hi - lo), N = sgFft;
  if (span < N) { chans.forEach(ch => sgCache[ch] = {narrow: true}); scopeDirty = true; return; }

  const w = scopeCols();
  const seq = ++sgSeq;
  const contiguous = Math.ceil(span / RAWCAP) <= SG_BUDGET;
  chans.forEach(ch => contiguous ? fetchSgContig(ch, lo, span, N, w, seq)
                                 : fetchSgSparse(ch, lo, span, N, seq));
}

// Narrow span: pull [lo,lo+span) contiguously (<= SG_BUDGET raw requests), slide a dense STFT.
function fetchSgContig(ch, lo, span, N, w, seq) {
  const nchunks = Math.ceil(span / RAWCAP), reqs = [];
  for (let i = 0; i < nchunks; i++) {
    const s = lo + i * RAWCAP, l = Math.min(RAWCAP, lo + span - s);
    reqs.push(api(`/capture/window?ch=${ch}&start=${s}&len=${l}&cols=2048`).then(r => [s, r]).catch(() => null));
  }
  Promise.all(reqs).then(rs => {
    if (seq !== sgSeq) return;
    const buf = new Float32Array(span);
    let ok = false;
    for (const rr of rs) {
      if (!rr) continue;
      const [s, r] = rr;
      if (!r || !r.raw || !r.samples) continue;
      const at = s - lo;
      for (let i = 0; i < r.samples.length && at + i < span; i++) buf[at + i] = r.samples[i];
      ok = true;
    }
    if (!ok) { sgCache[ch] = {empty: true}; scopeDirty = true; return; }
    const ncols = Math.max(1, Math.min(w, span - N + 1));
    const hop = ncols > 1 ? (span - N) / (ncols - 1) : 0;
    const db = new Float32Array(ncols * SG_NBINS);
    for (let c = 0; c < ncols; c++) sgColumn(buf, Math.round(c * hop), db, c * SG_NBINS);
    const sc = {start: lo, len: span, ncols, nbins: SG_NBINS, db};
    paintSpectrogram(sc);
    sgCache[ch] = sc;
    scopeDirty = true;
  });
}

// Wide span: SG_BUDGET columns, one FFT window per column (coarser in time, still correct).
function fetchSgSparse(ch, lo, span, N, seq) {
  const ncols = SG_BUDGET, reqs = [];
  for (let c = 0; c < ncols; c++) {
    const s = lo + Math.round(c * (span - N) / (ncols - 1));
    reqs.push(api(`/capture/window?ch=${ch}&start=${s}&len=${N}&cols=2048`).then(r => [c, r]).catch(() => null));
  }
  Promise.all(reqs).then(rs => {
    if (seq !== sgSeq) return;
    const db = new Float32Array(ncols * SG_NBINS).fill(-120), tmp = new Float32Array(N);
    let ok = false;
    for (const rr of rs) {
      if (!rr) continue;
      const [c, r] = rr;
      if (!r || !r.raw || !r.samples) continue;
      for (let i = 0; i < N; i++) tmp[i] = r.samples[i] || 0;
      sgColumn(tmp, 0, db, c * SG_NBINS);
      ok = true;
    }
    if (!ok) { sgCache[ch] = {empty: true}; scopeDirty = true; return; }
    const sc = {start: lo, len: span, ncols, nbins: SG_NBINS, db};
    paintSpectrogram(sc);
    sgCache[ch] = sc;
    scopeDirty = true;
  });
}

function buildLanes() {
  $('lanes').innerHTML = inputEnabled.map((on, i) =>
    `<span class="lane"><label><input type="checkbox" id="lane${i}" ${on ? 'checked' : ''}> IN ${i + 1}</label>` +
    `<button class="lm" id="lm${i}">wave</button></span>`).join('');
  inputEnabled.forEach((_, i) => {
    $('lane' + i).onchange = e => setInputEnabled(i, e.target.checked);
    $('lm' + i).onclick = () => toggleLaneMode(i);
  });
  syncLaneToggles();
  syncDelaySelects();
}

function toggleLaneMode(i) {
  laneMode[i] = laneMode[i] === 'spectro' ? 'wave' : 'spectro';
  saveLaneMode();
  if (laneMode[i] !== 'spectro') delete sgCache[i];
  syncLaneToggle(i);
  invalidateSpectrograms();
  scopeDirty = true;
}

// The toggle only bites while frozen; live, the lane always shows the envelope, so grey it out.
function syncLaneToggle(i) {
  const b = $('lm' + i);
  if (!b) return;
  b.textContent = laneMode[i] === 'spectro' ? 'spec' : 'wave';
  b.classList.toggle('on', laneMode[i] === 'spectro');
  b.disabled = !srvFrozen();
  b.title = srvFrozen() ? 'Show this lane as waveform or spectrogram'
    : 'Press Analyze first — the spectrogram reads the frozen buffer';
}

function syncLaneToggles() { inputEnabled.forEach((_, i) => syncLaneToggle(i)); updateSgCtl(); }

// The Spectrogram miniframe sits beside Wave and stays visible at all times — FFT / floor / colour
// are settings that take hold the moment a lane is switched to a spectrogram, so there is nothing
// to hide. (It used to appear only while frozen with a spectrogram lane active.)
function updateSgCtl() {}

function onEnvelope(buf) {
  const v = new DataView(buf);
  const first = Number(v.getBigUint64(1, true));
  const n = v.getUint16(9, true);
  for (let c = 0; c < n; c++) {
    const col = {sample: first + c * envColumnFrames, min: [], max: []};
    for (let ch = 0; ch < NIN; ch++) {
      const off = 11 + (c * NIN + ch) * 4;
      col.min.push(v.getInt16(off, true) / 32768);
      col.max.push(v.getInt16(off + 2, true) / 32768);
    }
    envCols.push(col);
  }
  if (envCols.length > ENV_KEEP) envCols.splice(0, envCols.length - ENV_KEEP);
  if (!srvFrozen() && !paused) scopeDirty = true;
}

// The view scrolls itself: an animation-frame loop eases toward the live edge instead of
// jumping whenever a WS batch happens to arrive (10 Hz batches lurch 100 ms each, and
// coalesced batches lurch harder).
function scopeTick() {
  if (tabActive('scope')) {
    if (!srvFrozen() && !paused && liveEdge() !== null) {
      const target = liveEdge() - view.len;
      const d = target - view.start;
      if (d !== 0) {
        view.start = Math.abs(d) < envColumnFrames ? target : view.start + d * 0.25;
        scopeDirty = true;
      }
    }
    if (scopeDirty) { scopeDirty = false; drawScope(); }
  }
  requestAnimationFrame(scopeTick);
}

// The frozen trace is drawn icy blue, not the live green, so a held snapshot never looks like the
// running signal. The dim variant is the provisional 5 ms envelope shown until the sharp window lands.
const WAVE_LIVE = '#3ecf8e';
const WAVE_FROZEN = '#5ac8fa';
const WAVE_FROZEN_DIM = '#2f6d8a';

function drawScope() {
  const cv = $('scopecanvas');
  const w = cv.clientWidth;
  if (!w) return;
  if (cv.width !== w) cv.width = w;
  const h = cv.height;
  const g = cv.getContext('2d');

  g.fillStyle = '#0e1014';
  g.fillRect(0, 0, w, h);

  const shown = shownInputs();
  if (!shown.length) return;
  const lh = h / shown.length;
  const x0 = view.start, x1 = view.start + view.len;
  const toX = s => (s - x0) / view.len * w;

  g.font = '11px ui-monospace, monospace';

  // One lane's envelope columns, clipped to an absolute [lo, hi) range. This is the live view's
  // picture, and also the provisional one while frozen: the snapshot and the columns are the same
  // audio, so freezing can show the columns immediately and sharpen when the windows land.
  const drawEnvLane = (ch, yOf, lo, hi) => {
    let any = false;
    g.beginPath();
    for (const col of envCols) {
      if (col.sample < x0 - envColumnFrames || col.sample > x1) continue;
      if (col.sample < lo || col.sample >= hi) continue;
      const x = toX(col.sample);
      g.moveTo(x, yOf(col.min[ch]));
      g.lineTo(x, yOf(col.max[ch]));
      any = true;
    }
    g.stroke();
    return any;
  };

  // Below a few columns per screen an envelope column is wider than the feature you are looking
  // at, so the preview would be a lie; at that zoom we wait for the real samples instead.
  const envUsable = view.len >= 8 * envColumnFrames;
  let provisional = false;
  const wave = srvFrozen() ? WAVE_FROZEN : WAVE_LIVE;

  shown.forEach((ch, row) => {
    const top = row * lh, mid = top + lh / 2;
    // The zero line stays dim; the separator above each channel is brighter, so the split between
    // channels reads clearly.
    g.strokeStyle = '#2c313b';
    g.beginPath(); g.moveTo(0, mid); g.lineTo(w, mid); g.stroke();
    g.strokeStyle = '#4b5563';
    g.beginPath(); g.moveTo(0, top); g.lineTo(w, top); g.stroke();

    g.fillStyle = '#8b93a3';
    g.fillText('IN ' + (ch + 1), 6, top + 14);

    g.strokeStyle = wave;
    let drew = false;
    const half = lh / 2 - 6;
    const yOf = s => mid - Math.max(-1, Math.min(1, s * vzoom)) * half;

    if (srvFrozen() && laneMode[ch] === 'spectro') {
      const sc = sgCache[ch];
      if (sc && sc.off) {
        g.imageSmoothingEnabled = true;
        const dx = toX(sc.start), dw = toX(sc.start + sc.len) - dx;
        g.drawImage(sc.off, 0, 0, sc.ncols, sc.nbins, dx, top, dw, lh);
        g.fillStyle = '#cdd3de';
        g.fillText('IN ' + (ch + 1), 6, top + 14);
        drew = true;
      } else {
        g.fillStyle = '#8b93a3';
        g.fillText(sc && sc.narrow ? 'zoom out for the spectrogram'
                 : sc && sc.empty ? 'outside the frozen range'
                 : 'computing spectrogram…', w / 2 - 60, mid);
      }
    } else if (srvFrozen()) {
      const cap = state.capture;
      const win = winCache[ch];
      // A cached window is only useful if it still overlaps the view — after a pan it may sit
      // entirely off-screen, and drawing nothing at all would look like a hang.
      const has = win && (win.raw ? win.samples.length : win.min.length);
      const covers = has && win.start < x1 && win.start + win.len > x0;

      if (covers && win.raw) {
        // Raw samples: a line through every sample, plus a dot per sample once they are far
        // enough apart to point a cursor at individually.
        const pxPer = w / view.len;
        g.beginPath();
        for (let i = 0; i < win.samples.length; i++) {
          const x = toX(win.start + i);
          const y = yOf(win.samples[i]);
          i ? g.lineTo(x, y) : g.moveTo(x, y);
        }
        g.stroke();
        if (pxPer >= 5) {
          g.fillStyle = wave;
          for (let i = 0; i < win.samples.length; i++) {
            g.fillRect(toX(win.start + i) - 1.5, yOf(win.samples[i]) - 1.5, 3, 3);
          }
        }
        drew = true;
      } else if (covers) {
        const colLen = win.len / win.min.length;
        g.beginPath();
        for (let i = 0; i < win.min.length; i++) {
          const x = toX(win.start + i * colLen);
          g.moveTo(x, yOf(win.min[i]));
          g.lineTo(x, yOf(win.max[i]));
        }
        g.stroke();
        drew = true;
      } else if (envUsable) {
        // Nothing fetched for this region yet. Draw the envelope columns, dimmed and clipped to
        // what the snapshot actually holds, so the freeze is instant and the trace only sharpens.
        g.strokeStyle = WAVE_FROZEN_DIM;
        drew = drawEnvLane(ch, yOf, cap.valid_start, cap.valid_start + cap.valid_len);
        g.strokeStyle = wave;
        provisional = provisional || drew;
      }
      if (!drew) {
        g.fillStyle = '#8b93a3';
        g.fillText(has ? 'outside the frozen range' : 'loading…', w / 2 - 40, mid);
      }
    } else {
      drawEnvLane(ch, yOf, -Infinity, Infinity);
    }
  });

  if (provisional) {
    g.fillStyle = '#8b93a3';
    g.fillText('5 ms preview — loading samples…', 6, h - 6);
  }

  // Ruler overlay, opt-in (replacing the old ping-emission markers): a time graticule with units
  // across every lane, plus — on a spectrogram lane — a log-frequency graticule with its own units.
  // A gridded scale reads intervals far better than bare markers. Drawn over the lanes (the
  // spectrogram image is opaque) with dim lines and label chips so it stays legible on either.
  if ($('showruler').checked) {
    const chip = (text, tx, ty, right) => {
      const tw = g.measureText(text).width;
      const lx = right ? tx - tw : tx;
      g.fillStyle = 'rgba(14,16,20,0.66)';
      g.fillRect(lx - 2, ty - 10, tw + 4, 13);
      g.fillStyle = '#aeb6c4';
      g.fillText(text, lx, ty);
    };
    g.lineWidth = 1;
    g.strokeStyle = 'rgba(139,147,163,0.22)';

    // Vertical time gridlines, anchored at the left edge (0) so the labels are small offsets.
    const divs = Math.max(4, Math.min(12, Math.round(w / 90)));
    const stepSec = niceStep((view.len / rate) / divs);
    const stepN = stepSec * rate;
    for (let k = 0, s = x0; s <= x1; k++, s = x0 + k * stepN) {
      const x = toX(s);
      g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke();
    }

    // Horizontal frequency gridlines on spectrogram lanes (Y is the same log axis as the hover
    // readout: SG_LOW_HZ at the bottom, Nyquist at the top).
    if (srvFrozen()) {
      const nyq = rate / 2, span = Math.log(nyq / SG_LOW_HZ);
      shown.forEach((ch, row) => {
        if (laneMode[ch] !== 'spectro' || !(sgCache[ch] && sgCache[ch].off)) return;
        const top = row * lh;
        // Log spacing crowds the decade lines near the top; draw every gridline but drop a label
        // that would collide with the last one placed. Walk high freq -> low (top -> bottom, so y
        // grows) and keep a label only once it clears the previous one by ~a line height.
        let lastLabelY = -Infinity;
        for (let i = FREQ_TICKS.length - 1; i >= 0; i--) {
          const f = FREQ_TICKS[i];
          if (f <= SG_LOW_HZ || f >= nyq) continue;
          const y = top + (1 - Math.log(f / SG_LOW_HZ) / span) * lh;
          g.beginPath(); g.moveTo(0, y); g.lineTo(w, y); g.stroke();
          if (y - lastLabelY >= 13) { chip(fmtHz(f), w - 5, y - 2, true); lastLabelY = y; }
        }
      });
    }

    // Time labels last, so every chip sits above the gridlines.
    for (let k = 0, s = x0; s <= x1; k++, s = x0 + k * stepN) {
      chip(fmtTime(k * stepSec), toX(s) + 3, h - 4, false);
    }
  }

  const cursor = (s, color, label) => {
    if (s === null || s < x0 || s > x1) return;
    const x = toX(s);
    g.strokeStyle = color;
    g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke();
    g.fillStyle = color;
    g.fillText(label, x + 3, 12);
  };
  cursor(cursorA, '#4da3ff', 'A');
  cursor(cursorB, '#ef5b5b', 'B');

  updateCursorReadout();
  updateZoomLabel();
  updateBufLabel();
}

// The delta has its own slot and is never concatenated into cursor B's. As part of B's string it
// swung that field from one character to forty, and B's width was what decided where the toolbar
// wrapped. drawScope calls this every animation frame, so unchanged cursors write nothing.
let curKey = null;

function updateCursorReadout() {
  const key = cursorA + '/' + cursorB;
  if (key === curKey) return;
  curKey = key;

  $('curA').textContent = cursorA === null ? '—' : cursorA;
  $('curB').textContent = cursorB === null ? '—' : cursorB;

  const el = $('curD');
  if (cursorA === null || cursorB === null) {
    el.textContent = '—';
    el.title = '';
    return;
  }
  const d = cursorB - cursorA;
  el.textContent = `${d} sa · ${(d / rate * 1000).toFixed(3)} ms`;
  el.title = el.textContent;
}

// How much time the whole canvas width is showing — the one number that says how far in or out
// you are zoomed. Keyed on view.len so drawScope can call it every frame and it writes only on
// an actual zoom, never on a pan.
let zoomKey = null;

function fmtSpan(samples) {
  const sec = samples / rate;
  if (sec >= 1) return sec.toFixed(2) + ' s';
  if (sec >= 0.001) return (sec * 1000).toFixed(sec >= 0.1 ? 1 : 2) + ' ms';
  return Math.round(sec * 1e6) + ' µs';
}

// The smallest "nice" tick step (1/2/5 x 10^n) at or above x — gives the ruler round gridline spacing.
function niceStep(x) {
  const p = Math.pow(10, Math.floor(Math.log10(x)));
  const n = x / p;
  return (n <= 1 ? 1 : n <= 2 ? 2 : n <= 5 ? 5 : 10) * p;
}

// A ruler tick's time offset (seconds), shown in the unit that reads cleanly. Step is always a nice
// number, so the trimmed values stay round (0, 2 ms, 500 µs, 1 s).
function fmtTime(sec) {
  if (!sec) return '0';
  const a = Math.abs(sec);
  if (a >= 1) return +sec.toFixed(3) + ' s';
  if (a >= 1e-3) return +(sec * 1e3).toFixed(3) + ' ms';
  return +(sec * 1e6).toFixed(1) + ' µs';
}

function fmtHz(hz) {
  return hz >= 1000 ? +(hz / 1000).toFixed(hz % 1000 ? 1 : 0) + ' kHz' : Math.round(hz) + ' Hz';
}

function updateZoomLabel() {
  const el = $('zoomlevel');
  if (!el || view.len === zoomKey) return;
  zoomKey = view.len;
  el.textContent = fmtSpan(view.len);
  el.title = `${view.len} samples across the view`;
}

function setVZoom(v) {
  vzoom = Math.max(VZOOM_MIN, Math.min(VZOOM_MAX, v));
  updateVZoomLabel();
  scopeDirty = true;
}
function updateVZoomLabel() {
  const el = $('vzoomlevel');
  if (el) el.textContent = vzoom + '×';
}

// How much live history is held, so zooming out past it is visibly the buffer's edge, not a bug.
// The client backfills the server's 60 s on connect and grows toward MAX_SPAN_S while the page
// stays open. Keyed on the rounded seconds so it writes only when the number moves, not per frame.
let bufKey = null;
function updateBufLabel() {
  const el = $('buflevel');
  if (!el) return;
  const secs = envCols.length >= 2 ? (liveEdge() - envCols[0].sample) / rate : 0;
  const shown = secs >= 1 ? Math.round(secs) + ' s'
    : secs > 0 ? Math.round(secs * 1000) + ' ms' : '—';
  if (shown === bufKey) return;
  bufKey = shown;
  el.textContent = shown;
  el.title = `${envCols.length} live envelope columns held (max ${ENV_KEEP})`;
}

function initScope() {
  const cv = $('scopecanvas');
  const sampleAtX = clientX => {
    const r = cv.getBoundingClientRect();
    return Math.round(view.start + (clientX - r.left) / r.width * view.len);
  };
  const sampleAt = e => sampleAtX(e.clientX);

  // Cursor A is set on release of a click that did not become a pan — see the drag block below.
  // Right-click sets B; the Clear cursors button clears both.
  cv.addEventListener('contextmenu', e => { e.preventDefault(); cursorB = sampleAt(e); scopeDirty = true; });

  // The shared tail of every zoom: clamp the span, place the view, keep-paused follow logic,
  // the min/max-columns hint, and the frozen-window refetch. The wheel pins the sample under
  // the pointer; the buttons pin the live edge or the centre.
  const applyZoom = (len, startOf) => {
    len = Math.max(32, Math.min(MAX_SPAN_S * rate, len));
    view.start = startOf(len);
    view.len = len;
    if (!srvFrozen()) {
      updateFollow(true);
      if (len < envColumnFrames * 8) {
        scopeMsg('The live view is 5 ms min/max columns — press Analyze to zoom down to samples.');
      }
    }
    invalidateWindows();
    scopeDirty = true;
  };

  cv.addEventListener('wheel', e => {
    e.preventDefault();
    const at = sampleAt(e);
    const f = e.deltaY > 0 ? 1.25 : 0.8;
    // keep the sample under the pointer pinned
    applyZoom(Math.round(view.len * f), len => Math.round(at - (at - view.start) * (len / view.len)));
  }, {passive: false});

  // Pointer-driven pan / tap / pinch, unified across mouse, touch and pen. Touch is direction-gated:
  // a horizontal drag pans, a vertical drag is left to the browser to scroll the page (#scopecanvas
  // sets touch-action:pan-y). Without that split, any drag on the tall canvas panned the waveform
  // and the page could never be scrolled from it. Two pointers pinch to zoom, pinning the sample
  // under the gesture midpoint. A press with no travel sets cursor A (decided on release, because a
  // drag also ends in a 'click'). setPointerCapture is taken only once a drag commits to panning, so
  // it never steals a gesture the browser wants for scrolling.
  const pointers = new Map();   // active pointerId -> {x, y}
  let drag = null;              // {x, y, start, moved, axis} — axis: null (deciding) | 'x' | 'scroll'
  let pinch = null;             // {dist} while two pointers zoom
  const distOf = p => Math.hypot(p[0].x - p[1].x, p[0].y - p[1].y);

  cv.addEventListener('pointerdown', e => {
    if (e.pointerType === 'mouse' && e.button !== 0) return;   // right button is cursor B (contextmenu)
    pointers.set(e.pointerId, {x: e.clientX, y: e.clientY});
    if (pointers.size >= 2) {
      drag = null;
      for (const id of pointers.keys()) { try { cv.setPointerCapture(id); } catch (_) { /* not active */ } }
      pinch = {dist: distOf([...pointers.values()])};
    } else {
      // Mouse commits to panning at once; touch stays undecided until it travels far enough to tell
      // a horizontal pan from a vertical page-scroll.
      drag = {x: e.clientX, y: e.clientY, start: view.start, moved: false,
              axis: e.pointerType === 'mouse' ? 'x' : null};
      if (e.pointerType === 'mouse') { try { cv.setPointerCapture(e.pointerId); } catch (_) {} }
    }
  });

  cv.addEventListener('pointermove', e => {
    const p = pointers.get(e.pointerId);
    if (!p) return;
    p.x = e.clientX; p.y = e.clientY;
    if (pinch && pointers.size >= 2) {
      const pts = [...pointers.values()];
      const dist = distOf(pts);
      if (pinch.dist > 0 && dist > 0) {
        const at = sampleAtX((pts[0].x + pts[1].x) / 2);   // pin the sample under the midpoint
        applyZoom(Math.round(view.len * (pinch.dist / dist)),
                  len => Math.round(at - (at - view.start) * (len / view.len)));
      }
      pinch.dist = dist;
      return;
    }
    if (!drag) return;
    const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
    if (drag.axis === null) {                     // touch: decide pan vs page-scroll once it moves
      if (Math.abs(dx) < 8 && Math.abs(dy) < 8) return;
      drag.axis = Math.abs(dx) > Math.abs(dy) ? 'x' : 'scroll';
      if (drag.axis === 'x') { try { cv.setPointerCapture(e.pointerId); } catch (_) {} }
    }
    if (drag.axis !== 'x') return;                // vertical drag: hands off to the browser to scroll
    drag.moved = true;
    const r = cv.getBoundingClientRect();
    view.start = Math.round(drag.start - dx / r.width * view.len);
    updateFollow();
    invalidateWindows();
    scopeDirty = true;
  });

  const endPointer = e => {
    if (!pointers.has(e.pointerId)) return;
    if (drag && !drag.moved && drag.axis !== 'scroll' && pointers.size === 1 &&
        (e.pointerType !== 'mouse' || e.button === 0)) {
      cursorA = sampleAt(e);
      scopeDirty = true;
    }
    pointers.delete(e.pointerId);
    if (pointers.size < 2) pinch = null;
    if (pointers.size === 0) {
      drag = null;
    } else {
      // A finger lifted mid-gesture: resume panning from a remaining pointer, flagged moved so its
      // eventual release can't drop a cursor.
      const [q] = [...pointers.values()];
      drag = {x: q.x, y: q.y, start: view.start, moved: true, axis: 'x'};
    }
  };
  cv.addEventListener('pointerup', endPointer);
  cv.addEventListener('pointercancel', endPointer);

  $('freeze').onclick = () => {
    if (srvFrozen()) {
      post('/capture/resume').then(() => {
        enterLive();
        clearResult();
        if (frozenReturnLen != null) { view.len = frozenReturnLen; frozenReturnLen = null; }
        if (liveEdge() !== null) view.start = liveEdge() - view.len;
      }).catch(e => scopeMsg(e.message));
    } else {
      frozenReturnLen = view.len;
      post('/capture/freeze').then(enterFrozen).catch(e => scopeMsg(e.message));
    }
  };

  $('clear').onclick = () => {
    const done = () => {
      enterLive();
      envCols = [];
      cursorA = cursorB = null;
      curKey = null;
      clearResult();
    };
    if (srvFrozen()) {
      post('/capture/resume').then(done).catch(e => scopeMsg(e.message));
    } else {
      done();
    }
  };

  $('clearcur').onclick = () => {
    cursorA = cursorB = null;
    curKey = null;
    scopeDirty = true;
  };

  // Zoom by a fixed factor. While following the live edge, keep that edge pinned so a zoom never
  // silently drops the view into history; otherwise pin the centre of what is on screen.
  const zoomView = factor => {
    applyZoom(Math.round(view.len * factor), len =>
      (!srvFrozen() && !paused && liveEdge() !== null)
        ? liveEdge() - len
        : Math.round(view.start + view.len / 2 - len / 2));
  };
  $('zoomin').onclick = () => zoomView(0.5);
  $('zoomout').onclick = () => zoomView(2);
  $('vzoomin').onclick = () => setVZoom(vzoom * 2);
  $('vzoomout').onclick = () => setVZoom(vzoom / 2);
  $('vzoomreset').onclick = () => setVZoom(1);
  updateVZoomLabel();
  $('zoomfit').onclick = () => {
    view.len = FIT_SPAN_S * rate;
    if (srvFrozen()) {
      view.start = state.capture.freeze_sample - view.len;
      invalidateWindows();
    } else if (liveEdge() !== null) {
      view.start = liveEdge() - view.len;
    }
    updateFollow();
    scopeDirty = true;
  };

  // Play resumes the live scroll and jumps to the live edge at the current zoom; Pause holds the
  // view where it is and latches it there. Frozen has no live scroll, so the button is disabled.
  $('playpause').onclick = () => {
    if (srvFrozen()) return;
    if (paused) {
      held = false;
      if (liveEdge() !== null) view.start = liveEdge() - view.len;
      updateFollow();
    } else {
      held = true;
      paused = true;
      setCapState('paused', 'press Play to catch up');
    }
    scopeDirty = true;
  };

  $('showruler').onchange = () => { scopeDirty = true; };

  $('measure').onclick = () => {
    if (!srvFrozen()) return;   // the button is disabled off a frozen snapshot; guard defensively
    // Both cursors set: measure the bracketed span. Otherwise fall back to the whole visible
    // window, so a quick Measure works without placing cursors first — clamped to the snapshot
    // bounds so a rounded view edge can never spill past [valid_start, valid_end).
    let lo, hi;
    if (cursorA !== null && cursorB !== null) {
      lo = Math.min(cursorA, cursorB);
      hi = Math.max(cursorA, cursorB);
    } else {
      const cap = state.capture;
      lo = Math.max(cap.valid_start, Math.round(view.start));
      hi = Math.min(cap.valid_start + cap.valid_len, Math.round(view.start + view.len));
    }
    const len = Math.max(1024, Math.min(1 << 19, hi - lo));
    const a = parseInt($('xa').value, 10);
    const b = parseInt($('xb').value, 10);
    scopeMsg('');

    post('/capture/xcorr', {ch_a: a - 1, ch_b: b - 1, start: lo, len})
      .then(r => {
        const el = $('xcorrout');
        el.classList.add('on');
        const warn = r.confidence < 2
          ? ' <span class="muted">— low confidence: the stimulus repeats inside this window, so the delay is only known modulo the ping interval. Narrow the window to one ping, or use the <em>tick</em> variant.</span>'
          : '';
        el.innerHTML =
          `<strong>IN ${a} &rarr; IN ${b}:</strong> ` +
          `<span class="mono">${r.lag_samples} samples = ${r.lag_ms.toFixed(3)} ms</span> ` +
          `<span class="muted">(confidence ${r.confidence.toFixed(1)})</span>${warn}`;
      })
      .catch(e => scopeMsg(e.message));
  };

  // Spectrogram controls (shared by every spectro lane). FFT changes the analysis, so it refetches;
  // floor and colour only recolour the cached dB, no network.
  const fftSel = $('sgfft'), floorInp = $('sgfloor'), floorV = $('sgfloorv'), mapSel = $('sgmap');
  if (fftSel) {
    fftSel.value = String(sgFft);
    fftSel.onchange = e => {
      sgFft = parseInt(e.target.value, 10);
      lsSet('sg_fft', sgFft);
      invalidateSpectrograms();
    };
    floorInp.value = String(sgFloorDb);
    floorV.textContent = sgFloorDb + ' dB';
    floorInp.oninput = e => {
      sgFloorDb = parseInt(e.target.value, 10);
      floorV.textContent = sgFloorDb + ' dB';
      lsSet('sg_floor', sgFloorDb);
      recolorSpectrograms();
    };
    mapSel.value = sgMap;
    mapSel.onchange = e => {
      sgMap = e.target.value;
      lsSet('sg_map', sgMap);
      recolorSpectrograms();
    };
  }

  // Hover a spectro lane → frequency (log Y) and level from the cached dB. The scope canvas backing
  // store is 1:1 with CSS pixels (no devicePixelRatio), so the mouse Y maps straight onto a lane.
  cv.addEventListener('mousemove', e => {
    const el = $('sghover');
    if (!el) return;
    const shown = shownInputs();
    const r = cv.getBoundingClientRect();
    const lh = shown.length ? r.height / shown.length : 0;
    const row = lh ? Math.floor((e.clientY - r.top) / lh) : -1;
    const ch = row >= 0 && row < shown.length ? shown[row] : null;
    const sc = ch != null ? sgCache[ch] : null;
    if (!srvFrozen() || ch == null || laneMode[ch] !== 'spectro' || !sc || !sc.db) { el.textContent = '— Hz · — dBFS'; return; }
    const frac = Math.max(0, Math.min(1, 1 - ((e.clientY - r.top) - row * lh) / lh));
    const freq = SG_LOW_HZ * Math.pow((rate / 2) / SG_LOW_HZ, frac);
    const s = view.start + (e.clientX - r.left) / r.width * view.len;
    const col = Math.max(0, Math.min(sc.ncols - 1, Math.round((s - sc.start) / sc.len * (sc.ncols - 1))));
    const bin = Math.max(0, Math.min(SG_NBINS - 1, Math.round(frac * (SG_NBINS - 1))));
    const db = sc.db[col * SG_NBINS + bin];
    const fs = freq >= 1000 ? (freq / 1000).toFixed(2) + ' kHz' : Math.round(freq) + ' Hz';
    el.textContent = `${fs} · ${db.toFixed(1)} dBFS`;
  });
}

// ---------------------------------------------------------------- system

function fmtMB(kb) { return (kb / 1024).toFixed(0) + ' MB'; }

// Per-core bars + memory. Called on every 1 Hz system message, so it only touches the values,
// never the DOM structure, once the bars exist.
function renderHost(s) {
  const cores = s.cpu_cores || [];
  const host = $('cores');
  if (host.childElementCount !== cores.length) {
    host.innerHTML = cores.map((_, i) =>
      `<div class="corerow">
         <span class="corelabel mono">cpu${i}</span>
         <div class="corebar"><div class="corefill" id="corefill${i}"></div></div>
         <span class="coreval mono" id="coreval${i}"></span>
       </div>`).join('');
  }
  cores.forEach((v, i) => {
    const fill = $('corefill' + i);
    if (!fill) return;
    fill.style.width = Math.max(0, Math.min(100, v)).toFixed(1) + '%';
    fill.classList.toggle('hot', v >= 85);
    $('coreval' + i).textContent = v.toFixed(0) + '%';
  });

  const m = s.mem;
  if (m && m.total_kb) {
    const pct = (100 * m.used_kb / m.total_kb);
    $('mem').innerHTML =
      `<div class="corerow">
         <span class="corelabel mono">ram</span>
         <div class="corebar"><div class="corefill${pct >= 90 ? ' hot' : ''}"
              style="width:${pct.toFixed(1)}%"></div></div>
         <span class="coreval mono">${fmtMB(m.used_kb)} / ${fmtMB(m.total_kb)}</span>
       </div>
       <p class="muted">${fmtMB(m.available_kb)} available</p>`;
  }
}

// The firmware's throttle register is the only place a sagging 5 V rail is visible at all —
// Linux cannot see it. Distinguish "happening now" from "happened earlier", because the second
// is a warning about the supply and the first is a reason to distrust the reading on screen.
function renderPower(t) {
  if (!t || !t.available) return;
  const now = t.under_voltage || t.throttled || t.freq_capped;
  const seen = t.under_voltage_seen || t.throttled_seen;
  const banner = $('powerbanner');
  banner.classList.toggle('hidden', !(now || seen));
  if (!(now || seen)) return;
  $('powertext').textContent = now
    ? 'Happening right now' + (t.throttled || t.freq_capped ? ' — the CPU is being throttled.' : '.')
    : 'It happened earlier in this boot; the supply is marginal.';
}

function buildSystem() {
  const e = state.engine, s = state.system;
  const rows = [
    ['Device', e.device],
    ['Rate / format', `${e.rate} Hz, ${e.format}`],
    ['Period / buffer', `${e.period} frames x ${e.periods}`],
    ['Capture channels', e.capture_channels],
    ['Xruns', e.xruns],
    ['Timeline generation', e.generation],
    ['I2S sync errors', s.sync_errors],
    ['Captured samples', e.samples],
    ['Listen streams', s.listen_streams],
    ['CPU', s.cpu_pct.toFixed(1) + ' %'],
    ['Memory', s.mem && s.mem.total_kb
      ? `${fmtMB(s.mem.used_kb)} / ${fmtMB(s.mem.total_kb)} used`
      : 'n/a'],
    ['Temperature', s.temp_c < 0 ? 'n/a' : s.temp_c.toFixed(1) + ' °C'],
    ['Uptime', (s.uptime_s / 3600).toFixed(2) + ' h'],
    ['Hostname', s.hostname],
    ['Addresses', s.ips.join('<br>')],
    ['Saved config', s.has_saved_config ? 'yes (data partition)' : 'no (image defaults)'],
    ['Loopback offset', s.loopback_offset_samples + ' samples'],
  ];
  if (e.last_error) rows.push(['Last error', e.last_error]);
  $('systable').innerHTML = rows.map(([k, v]) => `<tr><td>${k}</td><td>${v}</td></tr>`).join('');

  renderHost(s);
  renderPower(s.throttle);
  $('databanner').classList.toggle('hidden', s.data_persistent !== false);

  $('save').onclick = () => post('/config/save')
    .then(r => { $('cfgmsg').textContent = 'saved to ' + r.path; })
    .catch(e => { $('cfgmsg').textContent = e.message; });
  $('reset').onclick = () => {
    if (!confirm('Delete the saved settings? The next boot will use the image defaults.')) return;
    post('/config/reset')
      .then(() => { $('sysmsg').textContent = 'saved settings removed'; })
      .catch(e => { $('sysmsg').textContent = e.message; });
  };
  $('reboot').onclick = () => {
    if (!confirm('Reboot the device?')) return;
    post('/system/reboot')
      .then(() => { $('sysmsg').textContent = 'rebooting…'; })
      .catch(e => { $('sysmsg').textContent = e.message; });
  };
  $('shutdown').onclick = () => {
    if (!confirm('Shut the device down? It will have to be powered off and on again by hand.')) return;
    post('/system/shutdown')
      .then(() => {
        $('sysmsg').textContent = 'shutting down — wait for the green LED to stop blinking before pulling power';
      })
      .catch(e => { $('sysmsg').textContent = e.message; });
  };
}

// ---------------------------------------------------------------- telemetry

function onMeters(m) {
  for (let c = 0; c < NIN; c++) {
    if (!inputEnabled[c]) continue;   // disabled inputs keep their "off" placeholder
    const rms = $('rms' + c);
    if (!rms) continue;
    rms.style.width = dbToPct(m.rms_db[c]) + '%';
    $('pk' + c).style.left = dbToPct(m.peak_db[c]) + '%';
    $('rmsv' + c).textContent = `${fmtDb(m.rms_db[c])} dBFS`;
    $('pkv' + c).textContent = `pk ${fmtDb(m.peak_db[c])}`;
  }
}

function onSpectrum(msg) {
  // Text first, canvases second. Interleaving them makes every textContent write a layout the
  // next clientWidth read has to flush — one forced reflow per channel per message.
  for (const c of msg.channels) {
    if (!inputEnabled[c.ch]) continue;
    const t = $('tone' + c.ch);
    if (!t) continue;
    // An invalid tone renders a placeholder of the same width, never an empty string: a signal
    // sitting on the detector's threshold flips valid/invalid every frame, and an empty slot
    // would flicker with it.
    t.textContent = c.tone.valid ? `${pad(c.tone.freq_hz.toFixed(1), 7)} Hz` : `${pad('—', 7)} Hz`;
    $('thd' + c.ch).textContent =
      c.tone.valid ? `THD+N ${fmtThd(c.tone.thd_n_pct)}%` : `THD+N ${pad('—', 6)}%`;
  }
  for (const c of msg.channels) if (inputEnabled[c.ch]) ingestSpectrum(c.ch, c.bins);
}

function onSystem(s) {
  $('xruns').textContent = `xruns ${s.xruns}`;
  $('xruns').className = 'pill ' + (s.xruns ? 'warn' : 'good');
  $('cpu').textContent = `cpu ${s.cpu_pct.toFixed(0)}%` + (s.temp_c > 0 ? ` · ${s.temp_c.toFixed(0)}°C` : '');
  $('engine').textContent = s.engine_running ? 'engine running' : 'engine stopped';
  $('engine').className = 'pill ' + (s.engine_running ? 'good' : 'bad');
  $('syncbanner').classList.toggle('hidden', !s.sync_errors);
  renderHost(s);
  renderPower(s.throttle);
}

// The single push feed carries every live visual: the waveform envelope, the spectrum, the meters
// and the system telemetry. The socket handle is hoisted so the header's Pause-feed toggle can
// close it to save bandwidth on a slow link; feedStopped gates the auto-reconnect so a deliberate
// close stays closed. The two REST polls are stopped alongside it. Listen audio rides its own
// per-channel sockets and is intentionally left running.
let feedWs = null;
let feedStopped = false;
let statePoll = null;   // 5 s /api/state poll handle
let pingPoll = null;    // 1 s /api/pings/recent poll handle

function connect() {
  if (feedStopped) return;   // a queued reconnect must not reopen a feed the user paused
  if (feedWs) return;        // never run two sockets at once (e.g. a rapid re-toggle)
  const ws = new WebSocket(`ws://${location.host}/api/ws`);
  feedWs = ws;
  ws.binaryType = 'arraybuffer';

  ws.onopen = () => {
    $('conn').textContent = 'connected';
    $('conn').className = 'pill good';
    sendTelemetryMask();   // re-apply the input mask after every (re)connect, incl. a daemon restart
  };
  ws.onclose = () => {
    feedWs = null;
    if (feedStopped) {   // deliberately closed by the Pause-feed toggle — stay closed
      $('conn').textContent = 'paused';
      $('conn').className = 'pill warn';
      return;
    }
    $('conn').textContent = 'reconnecting';
    $('conn').className = 'pill bad';
    setTimeout(connect, 1000);
  };
  ws.onmessage = e => {
    if (e.data instanceof ArrayBuffer) { onEnvelope(e.data); return; }
    const msg = JSON.parse(e.data);
    if (msg.type === 'meters') onMeters(msg);
    else if (msg.type === 'spectrum') onSpectrum(msg);
    else if (msg.type === 'system') onSystem(msg);
  };
}

// The 5 s /state poll: a browser or curl elsewhere may have frozen or resumed the shared capture,
// and it keeps the System table current. Extracted into a named function so it can be stopped and
// restarted with the push feed.
function pollState() {
  api('/state').then(s2 => {
    const wasFrozen = srvFrozen();
    state.capture = s2.capture;
    // Another browser (or curl) may have frozen or resumed the shared capture.
    if (srvFrozen() !== wasFrozen) {
      if (srvFrozen()) enterFrozen(s2.capture);
      else enterLive();
    }
    buildSystem();
  }).catch(() => {});
}
function startStatePoll() { if (!statePoll) statePoll = setInterval(pollState, 5000); }
function stopStatePoll() { clearInterval(statePoll); statePoll = null; }
function startPingPoll() { if (!pingPoll) pingPoll = setInterval(refreshPings, 1000); }
function stopPingPoll() { clearInterval(pingPoll); pingPoll = null; }

// The header Pause-feed toggle. Stopping closes the push feed and both polls to save bandwidth;
// Listen audio (its own sockets) keeps playing. The choice persists across reloads.
function setFeedStopped(stop) {
  feedStopped = stop;
  lsSet('feed_stopped', stop ? '1' : '0');
  updateFeedButton();
  if (stop) {
    $('conn').textContent = 'paused';
    $('conn').className = 'pill warn';
    if (feedWs) { try { feedWs.close(); } catch (e) { /* already gone */ } }
    stopStatePoll();
    stopPingPoll();
  } else {
    $('conn').textContent = 'connecting';
    $('conn').className = 'pill';
    connect();          // its onopen flips the pill to connected
    startStatePoll();
    startPingPoll();
  }
}

function updateFeedButton() {
  const b = $('feedtoggle');
  if (!b) return;
  b.textContent = feedStopped ? 'Resume feed' : 'Pause feed';
  b.classList.toggle('paused', feedStopped);
  b.title = feedStopped
    ? 'Live data feed paused to save bandwidth — click to resume. Listen audio is unaffected.'
    : 'Stop the live data feed (waveform, spectrum, meters, telemetry) to save bandwidth. Listen audio keeps playing.';
}

function initFeedToggle() {
  feedStopped = lsGet('feed_stopped') === '1';
  const b = $('feedtoggle');
  if (b) b.onclick = () => setFeedStopped(!feedStopped);
  updateFeedButton();
}

// The analyze/PCM buffer-length control. Shown only when the daemon advertises
// limits.capture_config (older daemons that predate POST /api/capture/config leave it hidden,
// same forward-compatible pattern as the input stream mask). Sets how many seconds of
// full-resolution PCM the device copies on the next Analyze; the reply echoes the clamped value.
function initBufLen() {
  const wrap = $('buflenwrap'), inp = $('buflen'), na = $('buflenna'), maxEl = $('buflenmax');
  if (!wrap || !inp) return;
  const supported = state.limits.capture_config && state.capture.analyze_frames !== undefined;
  wrap.hidden = !supported;
  if (na) na.hidden = supported;
  if (!supported) return;
  const maxS = Math.floor((state.limits.capture_max_frames || 0) / rate);
  inp.max = maxS;
  inp.value = (state.capture.analyze_frames / rate).toFixed(1);
  if (maxEl) maxEl.textContent = `(max ${maxS} s)`;
  inp.onchange = () => {
    const s = parseFloat(inp.value);
    if (!(s > 0)) { inp.value = (state.capture.analyze_frames / rate).toFixed(1); return; }
    post('/capture/config', {seconds: s})
      .then(r => {
        state.capture.analyze_frames = r.analyze_frames;
        inp.value = r.analyze_seconds.toFixed(1);   // reflect the clamp
        if (maxEl) maxEl.textContent = `(max ${Math.floor(r.max_seconds)} s · applies on next Analyze)`;
      })
      .catch(e => { if (maxEl) maxEl.textContent = e.message; });
  };
}

api('/state').then(s => {
  state = s;
  rate = s.engine.rate || 96000;
  envColumnFrames = s.limits.env_column_frames;
  if (s.limits.input_gain_max_db !== undefined) {
    gainMinDb = s.limits.input_gain_min_db;
    gainMaxDb = s.limits.input_gain_max_db;
  }
  opusRate = s.limits.opus_rate || 48000;
  opusAvailable = Array.isArray(s.limits.listen_codecs) &&
                  s.limits.listen_codecs.includes('opus') && !!OpusDecoderClass;
  listenBitrate = s.limits.listen_bitrate_kbps || 96;
  const savedCodec = lsGet('listen_codec');
  listenCodec = (savedCodec === 'opus' || savedCodec === 'pcm')
      ? savedCodec : (s.limits.listen_default_codec || 'pcm');
  if (listenCodec === 'opus' && !opusAvailable) listenCodec = 'pcm';
  view.len = FIT_SPAN_S * rate;

  initMonitorVolume();
  initMonitorLatency();
  initCodec();
  buildInputs();
  buildOutputs();
  bindGenerators();
  buildMap();
  buildLanes();
  buildSystem();
  initScope();
  initBufLen();
  initFeedToggle();
  // The capture is shared and outlives a reload: pressing Analyze then reloading finds it
  // already frozen. Reflect that, AND load the snapshot — position the view over the frozen
  // range and fetch its windows. Without this the lanes sit on "loading…" forever, because on a
  // fresh load nothing else pulls the frozen data (the poll only fetches on a live→frozen
  // transition).
  if (srvFrozen()) {
    view.start = state.capture.freeze_sample - view.len;
    enterFrozen(state.capture);
  } else {
    setCapState('live', '');
    updatePlayPause();
  }
  clearResult();
  // Respect a persisted "feed paused" preference: open no socket and start no polls until resumed.
  if (feedStopped) {
    $('conn').textContent = 'paused';
    $('conn').className = 'pill warn';
  } else {
    connect();          // its onopen re-sends the input mask (no-op unless the daemon supports it)
    startStatePoll();
    startPingPoll();
    refreshPings();
  }
  requestAnimationFrame(scopeTick);
  requestAnimationFrame(spectraTick);
}).catch(e => {
  document.body.insertAdjacentHTML('afterbegin',
    `<div class="banner">Cannot reach the daemon: ${e.message}</div>`);
});
