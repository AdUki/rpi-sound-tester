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

// Monitor volume. This is the browser's playback level, not the device's: it hangs off the far
// end of the graph, downstream of the merger, so it lifts every channel and both ears together
// and leaves the samples the daemon measures untouched. The per-input Gain slider is the other
// thing entirely — that one is make-up gain applied on the device, and it moves the meters.
const MON_MIN_DB = -20, MON_MAX_DB = 40;
const dbToLin = db => Math.pow(10, db / 20);
let monitorDb = 0;

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

  // Ramped for the same reason the taps are: a gain stepped under a running stream clicks.
  setVolume(db) {
    if (!this.master) return;   // nothing is playing yet; ensure() will pick monitorDb up
    const g = this.master.gain, t = this.ctx.currentTime;
    g.cancelScheduledValues(t);
    g.setValueAtTime(g.value, t);
    g.linearRampToValueAtTime(dbToLin(db), t + RAMP_S);
  },

  // Absolute capture sample -> context play time. When the mapped time falls outside its
  // window — a late chunk, a ring lap, or capture/DAC clock drift finally accumulating — the
  // anchor shifts for EVERY stream at once: one brief glitch on all channels, and they come
  // out the other side still aligned with each other. A per-stream recovery would not.
  timeFor(sample) {
    const now = this.ctx.currentTime;
    if (this.anchorN === null) {
      this.anchorN = sample;
      this.anchorT = now + 0.15;
    }
    let t = this.anchorT + (sample - this.anchorN) / rate;
    if (t < now + 0.02 || t > now + 0.6) {
      this.anchorT += (now + 0.15) - t;
      t = now + 0.15;
    }
    return t;
  },

  idle() {
    if (listeners.size === 0) this.anchorN = null;  // next session anchors fresh
  }
};

class Listener {
  constructor(ch) {
    this.ch = ch;
    this.sides = {l: false, r: false};
    const ctx = monitor.ensure();

    // One socket per channel however many ears it feeds. Opening a second stream for the
    // second ear would carry identical audio and burn another of the device's twelve slots.
    this.taps = {l: ctx.createGain(), r: ctx.createGain()};
    this.taps.l.gain.value = 0;
    this.taps.r.gain.value = 0;
    this.taps.l.connect(monitor.merger, 0, 0);
    this.taps.r.connect(monitor.merger, 0, 1);

    this.ws = new WebSocket(`ws://${location.host}/api/listen/${ch}`);
    this.ws.binaryType = 'arraybuffer';
    this.ws.onmessage = e => this.onchunk(e.data);
    this.ws.onclose = () => this.stop();
  }

  get live() { return this.sides.l || this.sides.r; }

  // Only the tap gains move. The stream, its scheduling and the shared anchor are untouched,
  // so routing a channel to the other ear — or dropping it and bringing it back — never
  // re-times anything: whatever is still playing stays sample-aligned with it.
  setSide(side, on) {
    this.sides[side] = on;
    const g = this.taps[side].gain, t = monitor.ctx.currentTime;
    g.cancelScheduledValues(t);
    g.setValueAtTime(g.value, t);
    g.linearRampToValueAtTime(on ? 1 : 0, t + RAMP_S);
  }

  onchunk(buf) {
    const ctx = monitor.ctx;
    const view = new DataView(buf);
    const start = Number(view.getBigUint64(0, true));
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

    // Fed to both taps unconditionally; which ears actually hear it is the taps' gains.
    const src = ctx.createBufferSource();
    src.buffer = ab;
    src.connect(this.taps.l);
    src.connect(this.taps.r);
    src.start(monitor.timeFor(start));
  }

  stop() {
    try { this.ws.close(); } catch (e) { /* already gone */ }
    listeners.delete(this.ch);
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
  const saved = parseFloat(localStorage.getItem('monitor_db'));
  monitorDb = Number.isFinite(saved) ? Math.min(MON_MAX_DB, Math.max(MON_MIN_DB, saved)) : 0;

  const sl = $('monvol');
  sl.value = monitorDb;
  setMonitorLabel(monitorDb);
  sl.oninput = e => {
    monitorDb = parseFloat(e.target.value);
    setMonitorLabel(monitorDb);
    monitor.setVolume(monitorDb);
    try { localStorage.setItem('monitor_db', String(monitorDb)); } catch (err) { /* private mode */ }
  };
}

function setMonitorLabel(db) {
  const el = $('monvolv');
  el.textContent = db === 0 ? 'unity' : (db > 0 ? '+' : '') + db.toFixed(1);
  el.classList.toggle('active', db > 0);
}

// ---------------------------------------------------------------- cards

const dbToPct = db => Math.max(0, Math.min(100, (db + 60) / 60 * 100));

// Padded to a fixed width, because the card's layout must not depend on how many digits the
// reading happens to have. .readout is white-space: pre so the padding survives.
const pad = (s, n) => String(s).padStart(n);
const fmtDb = db => pad(db > -119 ? db.toFixed(1) : '-inf', 6);
// Three decimals on a 38 % reading is noise, and it is four characters wider than the slot.
const fmtThd = p => pad(p >= 10 ? p.toFixed(1) : p >= 1 ? p.toFixed(2) : p.toFixed(3), 6);

function buildInputs() {
  // A daemon built before input gain existed sends no gain_db; render no slider rather than
  // a control wired to a 404.
  const hasGain = state.inputs.length && state.inputs[0].gain_db !== undefined;
  $('gainnote').classList.toggle('hidden', !hasGain);

  $('inputs').innerHTML = state.inputs.map(i => `
    <div class="card">
      <div class="chan-head">
        <span class="chan-name">IN ${i.channel + 1}${i.name ? ' — ' + i.name : ''}</span>
        <span class="listen-grp">
          <button id="listenL${i.channel}" class="lbtn">Listen L</button>
          <button id="listenR${i.channel}" class="lbtn">Listen R</button>
        </span>
      </div>
      <div class="meter"><div class="rms" id="rms${i.channel}"></div><div class="peak" id="pk${i.channel}"></div></div>
      <div class="readout">
        <span id="rmsv${i.channel}" class="mono">     — dBFS</span>
        <span id="pkv${i.channel}" class="mono rt">pk      —</span>
        <span id="tone${i.channel}" class="mono">      — Hz</span>
        <span id="thd${i.channel}" class="mono rt">THD+N      —%</span>
      </div>
      ${hasGain ? `<label>Gain <input type="range" id="igain${i.channel}"
             min="${gainMinDb}" max="${gainMaxDb}" step="0.5">
        <span id="igainv${i.channel}" class="mono val"></span> dB</label>` : ''}
      <canvas class="mini" id="spec${i.channel}"></canvas>
    </div>`).join('');

  state.inputs.forEach(i => {
    const c = i.channel;
    $('listenL' + c).onclick = () => toggleListen(c, 'l');
    $('listenR' + c).onclick = () => toggleListen(c, 'r');
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

function sourceValue(src) {
  if (!src || src.type === 'silence') return 'silence';
  if (src.type === 'input') return 'in' + src.index;
  return 'gen' + src.index;
}

function buildOutputs() {
  const opts = ['<option value="silence">Silence</option>']
    .concat([...Array(NIN).keys()].map(i => `<option value="in${i}">IN ${i + 1}</option>`))
    .concat(['sine', 'noise', 'ping'].map(g => `<option value="gen${g}">Gen: ${g}</option>`))
    .join('');

  $('outputs').innerHTML = state.outputs.map(o => `
    <div class="card">
      <div class="chan-head">
        <span class="chan-name">OUT ${o.channel + 1}${o.name ? ' — ' + o.name : ''}</span>
        <button id="id${o.channel}">Identify</button>
      </div>
      <label>Source <select id="src${o.channel}">${opts}</select></label>
      <label>Gain <input type="range" id="gain${o.channel}" min="-60" max="0" step="0.5">
        <span id="gainv${o.channel}" class="mono val"></span> dB</label>
      <label><input type="checkbox" id="mute${o.channel}"> Mute</label>
    </div>`).join('');

  state.outputs.forEach(o => {
    const c = o.channel;
    $('src' + c).value = sourceValue(o.source);
    $('gain' + c).value = o.gain_db;
    $('gainv' + c).textContent = o.gain_db.toFixed(1);
    $('mute' + c).checked = o.mute;

    $('src' + c).onchange = e => {
      const v = e.target.value;
      let source;
      if (v === 'silence') source = {type: 'silence'};
      else if (v.startsWith('in')) source = {type: 'input', index: parseInt(v.slice(2), 10)};
      else source = {type: 'gen', index: v.slice(3)};
      put(`/outputs/${c}`, {source}).catch(err => toast(err.message));
    };
    $('gain' + c).oninput = e => {
      $('gainv' + c).textContent = parseFloat(e.target.value).toFixed(1);
      put(`/outputs/${c}`, {gain_db: parseFloat(e.target.value)});
    };
    $('mute' + c).onchange = e => put(`/outputs/${c}`, {mute: e.target.checked});
    $('id' + c).onclick = () => post(`/outputs/${c}/identify`);
  });
}

// Kept per channel so the traces can be repainted without waiting for the next message — after a
// tab switch or a resize, which is when the canvas has just been given a new backing store.
const specBins = {};

function drawSpectrum(ch, bins) {
  if (bins) specBins[ch] = bins;
  const b = specBins[ch];
  const cv = $('spec' + ch);
  if (!cv || !b || b.length < 2) return;   // one bin would divide by zero below
  const w = cv.clientWidth, h = cv.clientHeight;
  // Zero while the Dashboard is hidden. Sizing the canvas to it would throw the trace away and
  // nothing would repaint it, because the next message would find the width unchanged.
  if (!w || !h) return;
  if (cv.width !== w) cv.width = w;
  if (cv.height !== h) cv.height = h;
  const g = cv.getContext('2d');
  g.clearRect(0, 0, w, h);
  g.strokeStyle = '#4da3ff';
  g.beginPath();
  for (let i = 0; i < b.length; i++) {
    const x = i / (b.length - 1) * w;
    const y = h - Math.max(0, Math.min(1, (b[i] + 100) / 100)) * h;
    i ? g.lineTo(x, y) : g.moveTo(x, y);
  }
  g.stroke();
}

function redrawSpectra() {
  for (const ch of Object.keys(specBins)) drawSpectrum(Number(ch), null);
}

// ---------------------------------------------------------------- generators

function bindGenerators() {
  const g = state.generators;
  $('sineFreq').value = g.sine.freq_hz;
  $('sineLevel').value = g.sine.level_db;
  $('sineLevelV').textContent = g.sine.level_db.toFixed(1);
  $('noiseMode').value = g.noise.mode;
  $('noiseLevel').value = g.noise.level_db;
  $('noiseLevelV').textContent = g.noise.level_db.toFixed(1);
  $('pingVariant').value = g.ping.variant;
  $('pingInterval').value = g.ping.interval_s;
  $('pingLevel').value = g.ping.level_db;
  $('pingLevelV').textContent = g.ping.level_db.toFixed(1);

  $('sineFreq').onchange = e => put('/generators/sine', {freq_hz: parseFloat(e.target.value)});
  $('sineLevel').oninput = e => {
    $('sineLevelV').textContent = parseFloat(e.target.value).toFixed(1);
    put('/generators/sine', {level_db: parseFloat(e.target.value)});
  };
  $('noiseMode').onchange = e => put('/generators/noise', {mode: e.target.value});
  $('noiseLevel').oninput = e => {
    $('noiseLevelV').textContent = parseFloat(e.target.value).toFixed(1);
    put('/generators/noise', {level_db: parseFloat(e.target.value)});
  };
  $('pingVariant').onchange = e => put('/generators/ping', {variant: e.target.value});
  $('pingInterval').onchange = e => put('/generators/ping', {interval_s: parseFloat(e.target.value)});
  $('pingLevel').oninput = e => {
    $('pingLevelV').textContent = parseFloat(e.target.value).toFixed(1);
    put('/generators/ping', {level_db: parseFloat(e.target.value)});
  };

  setInterval(refreshPings, 1000);
}

let pings = [];
function refreshPings() {
  if (!document.getElementById('gen').classList.contains('active') && !document.getElementById('scope').classList.contains('active')) return;
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

const lanes = [true, true, true, true, true, true];
let view = {start: 0, len: 96000 * 2};   // absolute samples
let cursorA = null, cursorB = null;
let paused = false;        // user is inspecting the live view (zoom/pan): stop auto-scroll
let envCols = [];          // {sample, min[6], max[6]}
const ENV_KEEP = 12000;    // 60 s at 200 col/s — the server's own envelope depth
let scopeDirty = false;

const srvFrozen = () => !!(state && state.capture && state.capture.frozen);

// Whether the scope follows is decided by what the view shows: if it contains the live edge, it
// follows; pan or zoom into history and it holds still, and it picks up again the moment a pan
// or zoom brings the live edge back in. Do not go back to "any zoom or pan pauses until Fit is
// pressed": zooming out of a running wave then reads as a hang, since the view stops following
// and the live edge runs off-screen. The slack covers the scroll easing, which trails the live
// edge by up to a quarter of the per-frame advance.
function updateFollow() {
  if (srvFrozen()) return;
  if (envCols.length) {
    const last = envCols[envCols.length - 1].sample;
    paused = view.start + view.len < last - 20 * envColumnFrames;
  } else {
    paused = false;
  }
  setCapState(paused ? 'paused' : 'live', paused ? 'press Follow to catch up' : '');
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
const XCORR_IDLE = 'Freeze, bracket one ping with cursor A and cursor B, then press Measure.';

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

function invalidateWindows() {
  if (!srvFrozen()) return;
  clearTimeout(winTimer);
  winTimer = setTimeout(fetchWindows, 120);
}

function fetchWindows() {
  if (!srvFrozen()) return;
  const cap = state.capture;
  const lo = Math.max(view.start, cap.valid_start);
  const hi = Math.min(view.start + view.len, cap.valid_start + cap.valid_len);
  if (hi <= lo) { winCache = {}; scopeDirty = true; return; }

  const len = Math.floor(hi - lo);
  // clientWidth, not width: the backing store is still the canvas's 300 px default until the
  // Scope tab has been drawn once, and a freeze from another browser can land before that.
  const cols = Math.min(2048, Math.max(64, $('scopecanvas').clientWidth || 1024));
  const seq = ++winSeq;
  const shown = lanes.map((on, i) => on ? i : -1).filter(i => i >= 0);

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

function buildLanes() {
  $('lanes').innerHTML = lanes.map((on, i) =>
    `<label><input type="checkbox" id="lane${i}" ${on ? 'checked' : ''}> IN ${i + 1}</label>`).join('');
  lanes.forEach((_, i) => {
    $('lane' + i).onchange = e => {
      lanes[i] = e.target.checked;
      scopeDirty = true;
      invalidateWindows();
    };
  });
}

function onWave(buf) {
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
  if (document.getElementById('scope').classList.contains('active')) {
    if (!srvFrozen() && !paused && envCols.length) {
      const target = envCols[envCols.length - 1].sample - view.len;
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

function drawScope() {
  const cv = $('scopecanvas');
  const w = cv.clientWidth;
  if (!w) return;
  if (cv.width !== w) cv.width = w;
  const h = cv.height;
  const g = cv.getContext('2d');

  g.fillStyle = '#0e1014';
  g.fillRect(0, 0, w, h);

  const shown = lanes.map((on, i) => on ? i : -1).filter(i => i >= 0);
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

  shown.forEach((ch, row) => {
    const top = row * lh, mid = top + lh / 2;
    g.strokeStyle = '#2c313b';
    g.beginPath(); g.moveTo(0, mid); g.lineTo(w, mid); g.stroke();
    g.strokeStyle = '#2c313b';
    g.beginPath(); g.moveTo(0, top); g.lineTo(w, top); g.stroke();

    g.fillStyle = '#8b93a3';
    g.fillText('IN ' + (ch + 1), 6, top + 14);

    g.strokeStyle = '#3ecf8e';
    let drew = false;
    const half = lh / 2 - 6;
    const yOf = s => mid - Math.max(-1, Math.min(1, s)) * half;

    if (srvFrozen()) {
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
          g.fillStyle = '#3ecf8e';
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
        g.strokeStyle = '#2b6e52';
        drew = drawEnvLane(ch, yOf, cap.valid_start, cap.valid_start + cap.valid_len);
        g.strokeStyle = '#3ecf8e';
        provisional = provisional || drew;
      }
      if (!drew) {
        g.fillStyle = '#8b93a3';
        g.fillText(has ? 'outside the frozen range' : 'loading…', w / 2 - 40, mid);
      }
    } else {
      drew = drawEnvLane(ch, yOf, -Infinity, Infinity);
      if (!drew) {
        g.fillStyle = '#8b93a3';
        g.fillText('no data in view', w / 2 - 40, mid);
      }
    }
  });

  if (provisional) {
    g.fillStyle = '#8b93a3';
    g.fillText('5 ms preview — loading samples…', 6, h - 6);
  }

  // Ping emission markers, opt-in: with a short interval they stripe the whole view and bury
  // the waveform, and when you are not measuring delay they mean nothing.
  if ($('showpings').checked) {
    g.strokeStyle = 'rgba(245,166,35,0.7)';
    for (const p of pings) {
      if (p.sample < x0 || p.sample > x1) continue;
      const x = toX(p.sample);
      g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke();
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

function initScope() {
  const cv = $('scopecanvas');
  const sampleAt = e => {
    const r = cv.getBoundingClientRect();
    return Math.round(view.start + (e.clientX - r.left) / r.width * view.len);
  };

  cv.addEventListener('click', e => { cursorA = sampleAt(e); scopeDirty = true; });
  cv.addEventListener('contextmenu', e => { e.preventDefault(); cursorB = sampleAt(e); scopeDirty = true; });

  cv.addEventListener('wheel', e => {
    e.preventDefault();
    const at = sampleAt(e);
    const f = e.deltaY > 0 ? 1.25 : 0.8;
    const len = Math.max(32, Math.min(8 * rate, Math.round(view.len * f)));
    // keep the sample under the pointer pinned
    view.start = Math.round(at - (at - view.start) * (len / view.len));
    view.len = len;
    if (!srvFrozen()) {
      updateFollow();
      if (len < envColumnFrames * 8) {
        scopeMsg('The live view is 5 ms min/max columns — freeze to zoom down to samples.');
      }
    }
    invalidateWindows();
    scopeDirty = true;
  }, {passive: false});

  let drag = null;
  cv.addEventListener('mousedown', e => { drag = {x: e.clientX, start: view.start}; });
  window.addEventListener('mouseup', () => { drag = null; });
  window.addEventListener('mousemove', e => {
    if (!drag) return;
    const r = cv.getBoundingClientRect();
    const dx = (e.clientX - drag.x) / r.width * view.len;
    view.start = Math.round(drag.start - dx);
    updateFollow();
    invalidateWindows();
    scopeDirty = true;
  });

  $('freeze').onclick = () => {
    if (srvFrozen()) {
      post('/capture/resume').then(() => {
        state.capture.frozen = false;
        paused = false;
        winCache = {};
        $('freeze').textContent = 'Freeze';
        setCapState('live', '');
        clearResult();
        scopeMsg('');
        if (envCols.length) view.start = envCols[envCols.length - 1].sample - view.len;
        scopeDirty = true;
      }).catch(e => scopeMsg(e.message));
    } else {
      post('/capture/freeze').then(cs => {
        state.capture = cs;
        $('freeze').textContent = 'Resume';
        setCapState('frozen', frozenDetail(cs));
        scopeMsg('');
        fetchWindows();
        scopeDirty = true;
      }).catch(e => scopeMsg(e.message));
    }
  };

  $('clear').onclick = () => {
    const done = () => {
      envCols = [];
      cursorA = cursorB = null;
      curKey = null;
      winCache = {};
      paused = false;
      view.len = 2 * rate;
      clearResult();
      $('freeze').textContent = 'Freeze';
      setCapState('live', '');
      scopeMsg('');
      scopeDirty = true;
    };
    if (srvFrozen()) {
      post('/capture/resume').then(() => { state.capture.frozen = false; done(); })
        .catch(e => scopeMsg(e.message));
    } else {
      done();
    }
  };

  $('zoomout').onclick = () => {
    view.len = Math.min(8 * rate, view.len * 2);
    updateFollow();
    invalidateWindows();
    scopeDirty = true;
  };
  $('zoomfit').onclick = () => {
    view.len = 2 * rate;
    if (srvFrozen()) {
      view.start = state.capture.freeze_sample - view.len;
      invalidateWindows();
    } else if (envCols.length) {
      view.start = envCols[envCols.length - 1].sample - view.len;
    }
    updateFollow();
    scopeDirty = true;
  };

  // Fit without the zoom reset: jump to the end at the current zoom level. Live, that is the
  // live edge (which also resumes following); frozen, the end of the snapshot.
  $('follow').onclick = () => {
    if (srvFrozen()) {
      view.start = state.capture.freeze_sample - view.len;
      invalidateWindows();
    } else if (envCols.length) {
      view.start = envCols[envCols.length - 1].sample - view.len;
    }
    updateFollow();
    scopeDirty = true;
  };

  $('showpings').onchange = () => { scopeDirty = true; };

  $('measure').onclick = () => {
    if (!srvFrozen()) {
      scopeMsg('Freeze first — the delay is measured on the frozen snapshot.');
      return;
    }
    if (cursorA === null || cursorB === null) {
      scopeMsg('Set cursor A (left-click) and cursor B (right-click) to bracket one ping.');
      return;
    }
    const lo = Math.min(cursorA, cursorB), hi = Math.max(cursorA, cursorB);
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
         <span class="corelabel">cpu${i}</span>
         <div class="corebar"><div class="corefill" id="corefill${i}"></div></div>
         <span class="coreval" id="coreval${i}"></span>
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
         <span class="corelabel">ram</span>
         <div class="corebar"><div class="corefill${pct >= 90 ? ' hot' : ''}"
              style="width:${pct.toFixed(1)}%"></div></div>
         <span class="coreval">${fmtMB(m.used_kb)} / ${fmtMB(m.total_kb)}</span>
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
    .then(r => { $('sysmsg').textContent = 'saved to ' + r.path; })
    .catch(e => { $('sysmsg').textContent = e.message; });
  $('reset').onclick = () => {
    if (!confirm('Delete the saved settings? The next boot will use the image defaults.')) return;
    post('/config/reset')
      .then(() => { $('sysmsg').textContent = 'saved settings removed'; })
      .catch(e => { $('sysmsg').textContent = e.message; });
  };
  $('reboot').onclick = () => {
    if (!confirm('Reboot the device?')) return;
    post('/system/reboot').then(() => { $('sysmsg').textContent = 'rebooting…'; });
  };
  $('shutdown').onclick = () => {
    if (!confirm('Shut the device down? It will have to be powered off and on again by hand.')) return;
    post('/system/shutdown').then(() => {
      $('sysmsg').textContent = 'shutting down — wait for the green LED to stop blinking before pulling power';
    });
  };
}

// ---------------------------------------------------------------- telemetry

function onMeters(m) {
  for (let c = 0; c < NIN; c++) {
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
    const t = $('tone' + c.ch);
    if (!t) continue;
    // An invalid tone renders a placeholder of the same width, never an empty string: a signal
    // sitting on the detector's threshold flips valid/invalid every frame, and an empty slot
    // would flicker with it.
    t.textContent = c.tone.valid ? `${pad(c.tone.freq_hz.toFixed(1), 7)} Hz` : `${pad('—', 7)} Hz`;
    $('thd' + c.ch).textContent =
      c.tone.valid ? `THD+N ${fmtThd(c.tone.thd_n_pct)}%` : `THD+N ${pad('—', 6)}%`;
  }
  for (const c of msg.channels) drawSpectrum(c.ch, c.bins);
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

function connect() {
  const ws = new WebSocket(`ws://${location.host}/api/ws`);
  ws.binaryType = 'arraybuffer';

  ws.onopen = () => {
    $('conn').textContent = 'connected';
    $('conn').className = 'pill good';
  };
  ws.onclose = () => {
    $('conn').textContent = 'reconnecting';
    $('conn').className = 'pill bad';
    setTimeout(connect, 1000);
  };
  ws.onmessage = e => {
    if (e.data instanceof ArrayBuffer) { onWave(e.data); return; }
    const msg = JSON.parse(e.data);
    if (msg.type === 'meters') onMeters(msg);
    else if (msg.type === 'spectrum') onSpectrum(msg);
    else if (msg.type === 'system') onSystem(msg);
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
  view.len = 2 * rate;

  initMonitorVolume();
  buildInputs();
  buildOutputs();
  bindGenerators();
  buildMap();
  buildLanes();
  buildSystem();
  initScope();
  clearResult();
  connect();
  refreshPings();
  requestAnimationFrame(scopeTick);

  setInterval(() => api('/state').then(s2 => {
    const wasFrozen = srvFrozen();
    state.capture = s2.capture;
    // Another browser (or curl) may have frozen or resumed the shared capture.
    if (srvFrozen() !== wasFrozen) {
      $('freeze').textContent = srvFrozen() ? 'Resume' : 'Freeze';
      if (srvFrozen()) setCapState('frozen', frozenDetail(state.capture));
      else setCapState('live', '');
      if (srvFrozen()) fetchWindows(); else { winCache = {}; paused = false; }
      scopeDirty = true;
    }
    buildSystem();
  }).catch(() => {}), 5000);
}).catch(e => {
  document.body.insertAdjacentHTML('afterbegin',
    `<div class="banner">Cannot reach the daemon: ${e.message}</div>`);
});
