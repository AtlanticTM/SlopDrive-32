/**
 * render-vs-samplerate-probe.mjs — separates the two rates the operator
 * flagged as conflated: DRAW rate (how often the browser repaints, free,
 * uncapped rAF) vs SUBSCRIBE/SAMPLE rate (how often the DEVICE sends a new
 * STATE push, bounded by hub-tick pacing quality).
 *
 * Feeds REAL captured device traces (webui/test/evidence/trace-30hz.json —
 * the ORIGINAL subscribe policy — and trace-25hz.json — the new
 * MAX_SUBSCRIBE_HZ-independent TELEMETRY_HZ policy, both from
 * position-jitter-probe.mjs commanding a real sweep on the live device)
 * through the ACTUAL production interpolator (src/ui/hero/telebuf.js's
 * createTelebuf + createRenderClock — not a reimplementation), replayed at
 * simulated draw rates of 60/120/240 Hz.
 *
 * The point this proves or disproves: does raising the DRAW rate reduce the
 * rendered position's implied stutter (2nd difference)? If draw rate matters
 * more than sample rate, the SAME trace should get smoother as draw rate
 * rises. If sample rate is what matters, draw rate should barely move the
 * number for a given trace, while the 25Hz trace should measurably beat the
 * 30Hz trace at every draw rate tested.
 *
 * Run: node webui/test/render-vs-samplerate-probe.mjs
 * (expects webui/test/evidence/trace-30hz.json and trace-25hz.json, produced by:
 *   node webui/test/position-jitter-probe.mjs <host> 82 30 full "<legs>" <ms> test/evidence/trace-30hz.json
 *   node webui/test/position-jitter-probe.mjs <host> 82 25 full "<legs>" <ms> test/evidence/trace-25hz.json )
 */
import { readFileSync } from 'node:fs';
import { createTelebuf, createRenderClock } from '../src/ui/hero/telebuf.js';

const TRACE_DIR = new URL('./evidence/', import.meta.url);
const TRACES = [
  { label: '30Hz (original MAX_SUBSCRIBE_HZ policy)', file: 'trace-30hz.json' },
  { label: '25Hz (new TELEMETRY_HZ policy)', file: 'trace-25hz.json' },
];
const DRAW_RATES_HZ = [60, 120, 240];

function loadTrace(file) {
  const raw = readFileSync(new URL(file, TRACE_DIR), 'utf8');
  const d = JSON.parse(raw);
  return d.posSamples.filter((s) => typeof s.v === 'number' && isFinite(s.v));
}

/** Replay real (t,v) samples into the PRODUCTION telebuf+renderClock, sampled at a fixed simulated draw rate. */
function renderAtDrawRate(samples, drawHz) {
  const dtMs = 1000 / drawHz;
  const tele = createTelebuf();
  const clock = createRenderClock();
  const t0 = samples[0].t;
  const tEnd = samples[samples.length - 1].t;
  let simT = t0;
  let idx = 0;
  const frames = [];
  while (simT <= tEnd) {
    while (idx < samples.length && samples[idx].t <= simT) {
      tele.push(samples[idx].v, samples[idx].t);
      clock.noteArrival(samples[idx].t);
      idx++;
    }
    clock.update(dtMs);
    const rt = clock.stableRenderTime(simT);
    const r = tele.sampleAt(rt);
    frames.push({ t: simT, v: r.value, fresh: r.fresh });
    simT += dtMs;
  }
  return frames;
}

/** Uniform-grid 2nd difference (frames are evenly spaced by construction) — implied acceleration, unit/s^2. */
function impliedAccelUniform(frames, dtMs) {
  const dt = dtMs / 1000;
  const out = [];
  for (let i = 1; i < frames.length - 1; i++) {
    const v0 = frames[i - 1].v, v1 = frames[i].v, v2 = frames[i + 1].v;
    if (v0 == null || v1 == null || v2 == null) continue;
    if (!frames[i - 1].fresh || !frames[i].fresh || !frames[i + 1].fresh) continue;
    out.push((v2 - 2 * v1 + v0) / (dt * dt));
  }
  return out;
}

function stats(a) {
  if (!a.length) return null;
  const abs = a.map(Math.abs).sort((x, y) => x - y);
  const rms = Math.sqrt(a.reduce((s, x) => s + x * x, 0) / a.length);
  const p95 = abs[Math.min(abs.length - 1, Math.floor(abs.length * 0.95))];
  return { n: a.length, rms, p95, max: abs[abs.length - 1] };
}

console.log('Real captured device traces replayed through the PRODUCTION interpolator (telebuf.js), at simulated draw rates.');
console.log('Metric: implied acceleration (2nd difference) of the RENDERED position — what visually reads as stutter.\n');

const results = [];
for (const tr of TRACES) {
  const samples = loadTrace(tr.file);
  console.log('=== ' + tr.label + '  (n=' + samples.length + ' real samples) ===');
  for (const hz of DRAW_RATES_HZ) {
    const frames = renderAtDrawRate(samples, hz);
    const st = stats(impliedAccelUniform(frames, 1000 / hz));
    results.push({ trace: tr.label, hz, st });
    console.log('  draw ' + String(hz).padStart(3) + 'Hz -> rendered n=' + frames.length +
      '  implied-accel rms=' + (st ? st.rms.toFixed(2) : 'n/a') +
      '  p95=' + (st ? st.p95.toFixed(2) : 'n/a') +
      '  max=' + (st ? st.max.toFixed(2) : 'n/a'));
  }
  console.log('');
}

console.log('=== READING THE RESULT ===');
console.log('Within one trace, p95 across 60/120/240Hz draw should be roughly FLAT (draw rate is not the lever).');
console.log('Across traces at the SAME draw rate, 25Hz-sample should show a materially lower p95 than 30Hz-sample (sample evenness is the lever).\n');

const byHz = new Map();
for (const r of results) {
  if (!byHz.has(r.hz)) byHz.set(r.hz, []);
  byHz.get(r.hz).push(r);
}
for (const [hz, rows] of byHz) {
  const line = rows.map((r) => r.trace.split(' ')[0] + '=' + (r.st ? r.st.p95.toFixed(1) : 'n/a')).join('   ');
  console.log('  @' + hz + 'Hz draw:  ' + line);
}
