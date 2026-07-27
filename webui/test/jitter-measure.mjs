/**
 * jitter-measure.mjs — instruments the live SlopSync WS on the real device to
 * measure inter-arrival timing of binary frames (dominated by the motion
 * telemetry channel), and separately samples the rendered position numeral's
 * DOM text at rAF cadence to characterize on-screen smoothness (snap vs glide).
 *
 * Scratch tool for the rail-widget bug-fix pass — not part of the app.
 * Run: node webui/test/jitter-measure.mjs [host] [durationMs]
 */
import { chromium } from 'playwright';

const HOST = process.argv[2] || '192.168.1.229';
const DURATION_MS = parseInt(process.argv[3] || '8000', 10);
const PAGE_URL = 'http://' + HOST + '/';

const browser = await chromium.launch();
const page = await browser.newPage();

// Patch WebSocket BEFORE any app script runs so every binary frame's arrival
// time (performance.now()) lands in window.__wsArrivals, regardless of what
// the frame decodes to. The SlopSync socket is a single connection on :82;
// traffic is dominated by the highest-rate STATE channel (motion telemetry),
// so overall inter-arrival stats approximate that channel's cadence without
// needing to reach into the app's decoded model.
await page.addInitScript(() => {
  window.__wsArrivals = [];
  const NativeWS = window.WebSocket;
  function PatchedWS(url, protocols) {
    const ws = protocols !== undefined ? new NativeWS(url, protocols) : new NativeWS(url);
    ws.addEventListener('message', (ev) => {
      if (typeof ev.data !== 'string') { // binary frames only (CBOR/packed)
        window.__wsArrivals.push(performance.now());
      }
    });
    return ws;
  }
  PatchedWS.prototype = NativeWS.prototype;
  Object.setPrototypeOf(PatchedWS, NativeWS);
  window.WebSocket = PatchedWS;
});

await page.goto(PAGE_URL, { waitUntil: 'domcontentloaded', timeout: 20000 });
await page.waitForSelector('nav.tabs button', { timeout: 25000 }).catch(() => {});
await page.waitForTimeout(1500); // let the link settle past HELLO/WELCOME/GRANTs

// Sample the rendered position numeral text at rAF cadence for the window.
await page.evaluate(() => {
  window.__hnSamples = [];
  function findValEl() {
    return document.querySelector('.hn-primary .hn-val') || document.querySelector('#heroActual');
  }
  function loop() {
    const el = findValEl();
    window.__hnSamples.push({ t: performance.now(), v: el ? el.textContent : null });
    if (window.__hnKeepGoing) requestAnimationFrame(loop);
  }
  window.__hnKeepGoing = true;
  requestAnimationFrame(loop);
});

await page.waitForTimeout(DURATION_MS);

const wsArrivals = await page.evaluate(() => window.__wsArrivals);
await page.evaluate(() => { window.__hnKeepGoing = false; });
const hnSamples = await page.evaluate(() => window.__hnSamples);

await browser.close();

function stats(deltas) {
  if (!deltas.length) return null;
  const sorted = [...deltas].sort((a, b) => a - b);
  const sum = deltas.reduce((a, b) => a + b, 0);
  const mean = sum / deltas.length;
  const p95 = sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * 0.95))];
  const max = sorted[sorted.length - 1];
  const min = sorted[0];
  return { n: deltas.length, mean, p95, max, min };
}

const wsDeltas = [];
for (let i = 1; i < wsArrivals.length; i++) wsDeltas.push(wsArrivals[i] - wsArrivals[i - 1]);
const wsStats = stats(wsDeltas);
const hz = wsStats ? 1000 / wsStats.mean : 0;

console.log('=== WS binary frame inter-arrival (approximates motion telemetry cadence) ===');
console.log(wsStats
  ? `n=${wsStats.n} mean=${wsStats.mean.toFixed(1)}ms (~${hz.toFixed(1)}Hz) p95=${wsStats.p95.toFixed(1)}ms max=${wsStats.max.toFixed(1)}ms min=${wsStats.min.toFixed(1)}ms`
  : 'no binary frames observed');

// Rendered-value "snap" analysis: count how many consecutive rAF samples show
// the SAME text (a hold/stutter) vs a changed value, and the longest hold run
// in ms — a smooth interpolator changes on nearly every rAF tick while real
// samples are in flight; a naive snap-per-push holds flat for ~1 push
// interval then jumps.
let holdRun = 0, longestHoldMs = 0, holdStartT = null, changes = 0;
for (let i = 1; i < hnSamples.length; i++) {
  const prev = hnSamples[i - 1], cur = hnSamples[i];
  if (cur.v === prev.v) {
    if (holdStartT == null) holdStartT = prev.t;
    holdRun++;
  } else {
    changes++;
    if (holdStartT != null) {
      longestHoldMs = Math.max(longestHoldMs, cur.t - holdStartT);
      holdStartT = null;
    }
  }
}
if (holdStartT != null && hnSamples.length) {
  longestHoldMs = Math.max(longestHoldMs, hnSamples[hnSamples.length - 1].t - holdStartT);
}
console.log('\n=== Rendered position numeral (rAF-sampled) ===');
console.log(`frames=${hnSamples.length} value-changes=${changes} longest-hold=${longestHoldMs.toFixed(1)}ms first="${hnSamples[0] && hnSamples[0].v}" last="${hnSamples[hnSamples.length - 1] && hnSamples[hnSamples.length - 1].v}"`);
