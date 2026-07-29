/**
 * rail-probe.mjs — MEASURES rail-marker smoothness against the live device
 * instead of eyeballing it. Run while the machine is MOVING (a pattern, or
 * tape moves) — an idle trace is reported but proves nothing.
 *
 * Two simultaneous recordings, ~12 s:
 *   1. Page-side: window.__railProbe — per-rAF-frame [frameMs, tRender,
 *      posDisplay, tgtDisplay, fresh] from RailWidget's draw loop (the probe
 *      hook is permanent and free when the flag is unset).
 *   2. Wire-side: an independent read-only session recording every motion
 *      STATE arrival (Date.now(), pos, tgt) — the feed the page interpolates.
 *
 * Report separates the stages so the jitter's HOME is identified:
 *   - wire arrival gaps (burstiness of the feed itself)
 *   - frame pacing (is rAF starving?)
 *   - display-position second differences (is the interpolated output rough?)
 *   - fresh ratio + stall/snap counts (is the render clock holding/snapping?)
 *
 * COMMANDS NO MOTION. Run: node webui/test/rail-probe.mjs [host]
 */

import { chromium } from 'playwright';
import { createSession, CH, PRIORITY } from '../../../SlopSync/clients/js/index.js';

const HOST = process.argv[2] || '192.168.1.229';
const RECORD_MS = 12000;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function stats(arr) {
  if (!arr.length) return { n: 0 };
  const s = [...arr].sort((a, b) => a - b);
  const at = (q) => s[Math.min(s.length - 1, Math.floor(q * s.length))];
  return { n: s.length, p50: at(0.5), p95: at(0.95), p99: at(0.99), max: s[s.length - 1] };
}
const fmt = (o) => o.n ? `n=${o.n} p50=${o.p50.toFixed(2)} p95=${o.p95.toFixed(2)} p99=${o.p99.toFixed(2)} max=${o.max.toFixed(2)}` : 'n=0';

// ---- wire watcher -----------------------------------------------------------
const wire = [];
const watcher = createSession({
  host: HOST, port: 82, clientKind: 'webui', clientName: 'rail probe wire watcher',
  autoReconnect: false, subscriptions: [[CH.MOTION, 25.0, PRIORITY.elevated]],
});
watcher.on('state', (ch, sample) => {
  if (ch === CH.MOTION) wire.push({ t: Date.now(), pos: sample.pos_10um, tgt: sample.tgt_10um });
});
watcher.connect();

// ---- page probe -------------------------------------------------------------
const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
await page.goto('http://' + HOST + '/', { waitUntil: 'domcontentloaded', timeout: 20000 });
await page.waitForSelector('.spine-rail-host', { timeout: 25000 });
await sleep(1500);

console.log('recording ' + RECORD_MS + ' ms ... (machine should be MOVING)');
await page.evaluate(() => { window.__railProbe = []; });
const t0 = Date.now();
await sleep(RECORD_MS);
const frames = await page.evaluate(() => { const p = window.__railProbe; window.__railProbe = null; return p; });
const wireDur = Date.now() - t0;
await browser.close();
watcher.close?.();

// ---- analysis ---------------------------------------------------------------
console.log('\n== WIRE (feed the page interpolates) ==');
const gaps = [];
for (let i = 1; i < wire.length; i++) gaps.push(wire[i].t - wire[i - 1].t);
const dupTs = gaps.filter((g) => g === 0).length;
console.log('arrivals:', wire.length, 'in', wireDur, 'ms  (' + (wire.length / (wireDur / 1000)).toFixed(1) + ' Hz)');
console.log('arrival gaps ms:', fmt(stats(gaps)), ' zero-gap(dup):', dupTs, ' gaps>80ms:', gaps.filter((g) => g > 80).length);
const tgtRange = wire.length ? Math.max(...wire.map((w) => w.tgt)) - Math.min(...wire.map((w) => w.tgt)) : 0;
console.log('tgt range over trace:', tgtRange, '(10um units)', tgtRange < 100 ? ' << MACHINE LOOKS IDLE — trace not meaningful' : '');

console.log('\n== FRAMES (rAF pacing) ==');
const fdt = [];
for (let i = 1; i < frames.length; i++) fdt.push(frames[i][0] - frames[i - 1][0]);
console.log('frames:', frames.length, ' dt ms:', fmt(stats(fdt)), ' frames>25ms:', fdt.filter((d) => d > 25).length, ' frames>40ms:', fdt.filter((d) => d > 40).length);

console.log('\n== DISPLAY POSITION (what the eye sees) ==');
const moving = frames.filter((f) => f[2] != null);
const d1 = [], d2 = [];
for (let i = 1; i < moving.length; i++) d1.push(moving[i][2] - moving[i - 1][2]);
for (let i = 1; i < d1.length; i++) d2.push(Math.abs(d1[i] - d1[i - 1]));
console.log('per-frame step:', fmt(stats(d1.map(Math.abs))));
console.log('second difference (roughness):', fmt(stats(d2)));
const stalls = [];
let run = 0;
for (let i = 1; i < moving.length; i++) {
  if (moving[i][2] === moving[i - 1][2]) run++;
  else { if (run >= 3) stalls.push(run); run = 0; }
}
console.log('stall runs (>=3 frames frozen):', stalls.length, stalls.length ? ' lengths: ' + stalls.slice(0, 10).join(',') : '');
const snaps = d1.filter((d, i) => i > 0 && Math.abs(d) > 6 * (Math.abs(d1[i - 1]) + 0.01)).length;
console.log('snap frames (step >6x previous):', snaps);
const freshRatio = frames.length ? frames.filter((f) => f[4]).length / frames.length : 0;
console.log('fresh ratio:', (freshRatio * 100).toFixed(1) + '%  (held/extrapolating the rest)');

console.log('\ntrace sizes: wire=' + wire.length + ' frames=' + frames.length);
process.exit(0);
