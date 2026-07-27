/**
 * position-jitter-probe.mjs — measure REAL on-wire cadence + implied
 * acceleration of the position/target telemetry while the carriage is
 * actually moving, at a chosen subscribe rate for that channel.
 *
 * Scratch tool for the rail-jitter investigation (operator report: "the live
 * position telemetry is still pretty bad"). This is the wire-level half of
 * the measurement — no browser, no rendering, just: what does the hub
 * actually deliver, and does the RAW reported position sequence itself imply
 * stutter (via the non-uniform second difference / implied acceleration) at
 * a given subscription rate.
 *
 * Mirrors machine.svelte.js's subscription policy (min(catalog maxRateHz,
 * MAX_SUBSCRIBE_HZ) for every h2c STATE/EVENT channel — the rate the DEVICE
 * paces STATE at, unrelated to how fast the browser draws) for realism, but
 * lets the caller OVERRIDE the rate wished for the
 * telemetry.position/telemetry.target channel specifically (both roles live
 * on the same catalog channel on this device, but this does not assume that)
 * — that override is the whole point: comparing 30 Hz (the original client
 * policy) against rates whose period is an exact multiple of the hub's 5 ms
 * tick (40 Hz=25ms=5 ticks, 50 Hz=20ms=4 ticks, 25 Hz=40ms=8 ticks) against
 * 60 Hz (the catalog's advertised ceiling) tells us whether H1 (pacing alias
 * against the hub tick) is real and whether H2 (we are under-subscribed at
 * 30) helps or hurts.
 *
 * NOTE — this only measures the SUBSCRIBE-rate half of the picture. DRAW rate
 * (how often the browser repaints) is free and uncapped (rAF), a completely
 * separate axis; see render-vs-samplerate-probe.mjs for the harness that
 * proves draw rate does NOT fix jitter on its own.
 *
 * Bound by ROLE (roles.js), never a hardcoded channel id, per CLAUDE.md's
 * SlopSync layering rule and this repo's existing probe convention
 * (telemetry-latency-probe.mjs).
 *
 * Commands a real sweep (session.sendMove) across a caller-given set of
 * waypoints inside the stroke window, so the machine is actually glide-moving
 * for the whole measurement window — this is NOT a synthetic test.
 *
 * SAFETY: caller is responsible for passing waypoints inside the window.
 * Leaves no motion queued when it exits (last waypoint's move settles before
 * the script closes).
 *
 * Run: node webui/test/position-jitter-probe.mjs [host] [port] [posHz] [mode] [legs] [legWaitMs]
 *   posHz     subscribe rate wished for the position/target channel (default: catalog max, capped by policy below)
 *   mode      'full' (mirrors real page's whole subscription set) or 'solo' (position/target channel only) — default 'full'
 *   legs      comma-separated waypoints in mm, e.g. "40,170,60,150,45" — default "40,170,60,150,45"
 *   legWaitMs time to let each leg glide before commanding the next — default 3500
 *   dumpPath  optional: write the raw {t,v} position trace as JSON here, for
 *             feeding into render-vs-samplerate-probe.mjs (real captured
 *             arrival timing, not synthetic)
 */

import { createSession, CHANNEL_CLASS, PRIORITY } from '../src/core/slopsync/index.js';
import { acquireToken } from '../src/core/slopsync/credentials.js';
import { buildSettingsModel } from '../src/model/settings.js';
import { claimRoles, ROLE } from '../src/model/roles.js';
import { writeFileSync } from 'node:fs';

const HOST = process.argv[2] || '192.168.1.229';
const PORT = parseInt(process.argv[3] || '82', 10);
const POS_HZ_ARG = process.argv[4] != null && process.argv[4] !== '' ? parseFloat(process.argv[4]) : null;
const MODE = process.argv[5] || 'full';
const LEGS = (process.argv[6] || '40,170,60,150,45').split(',').map(Number);
const LEG_WAIT_MS = parseInt(process.argv[7] || '3500', 10);
const DUMP_PATH = process.argv[8] || null;

const MAX_SUBSCRIBE_HZ = 30; // machine.svelte.js's current baseline policy, for the OTHER channels in 'full' mode

if (typeof WebSocket === 'undefined') {
  console.error('No global WebSocket (need node >= 22). Aborting.');
  process.exit(1);
}

const s = createSession({
  host: HOST, port: PORT, clientKind: 'webui', clientName: 'position-jitter-probe',
  autoReconnect: false, WebSocketImpl: WebSocket,
  token: (h) => acquireToken(h),
});

const catalogP = new Promise((resolve, reject) => {
  const to = setTimeout(() => reject(new Error('timeout waiting for catalog')), 20000);
  s.on('catalog', (entries) => { clearTimeout(to); resolve(entries); });
  s.on('close', (c) => { clearTimeout(to); reject(new Error('closed before catalog: ' + (c.reason || c.code))); });
});

let welcomeLimits = {};
s.on('welcome', (w) => { welcomeLimits = w.limits || {}; });

const nacks = [];
s.on('nack', (n) => nacks.push(n));

s.connect();
const entries = await catalogP;
const model = buildSettingsModel(entries);

const claim = claimRoles(model.byRole, {
  require: {},
  optional: { pos: ROLE.telemetryPosition, target: ROLE.telemetryTarget, vel: ROLE.telemetryVelocity },
});

if (!claim || !claim.pos) {
  console.error('This catalog has no telemetry.position role — cannot measure. Aborting.');
  s.close();
  process.exit(1);
}
const posField = claim.pos;
const targetField = claim.target;
const posEntry = entries.find((e) => e.id === posField.channelId);
const telemetryChannelIds = new Set([posField.channelId, targetField ? targetField.channelId : null].filter((x) => x != null));

const catalogCeilHz = posEntry.maxRateHz || 0;
const posHz = POS_HZ_ARG != null ? Math.min(POS_HZ_ARG, catalogCeilHz || POS_HZ_ARG) : Math.min(catalogCeilHz, MAX_SUBSCRIBE_HZ);

console.log('host                 : ' + HOST + ':' + PORT);
console.log('mode                 : ' + MODE);
console.log('position channel     : id 0x' + posField.channelId.toString(16).padStart(4, '0')
  + '  field=' + posField.name + '  catalog maxRateHz=' + catalogCeilHz);
console.log('subscribing pos/tgt @ : ' + posHz + ' Hz (period ' + (1000 / posHz).toFixed(2) + ' ms)');
console.log('legs                 : ' + LEGS.join(' -> ') + ' mm, ' + LEG_WAIT_MS + ' ms glide each');

// ---- build wishes -----------------------------------------------------------
function subscriptionWishes() {
  const wishes = [];
  if (MODE === 'solo') {
    for (const chId of telemetryChannelIds) {
      const e = entries.find((x) => x.id === chId);
      wishes.push([chId, Math.min(posHz, e.maxRateHz || posHz), e.priority != null ? e.priority : PRIORITY.elevated]);
    }
    return wishes;
  }
  for (const e of entries) {
    if (e.dir !== 0) continue;
    if (e.cls !== CHANNEL_CLASS.STATE && e.cls !== CHANNEL_CLASS.EVENT) continue;
    let rate = (e.cls === CHANNEL_CLASS.EVENT || !e.maxRateHz) ? 0 : Math.min(e.maxRateHz, MAX_SUBSCRIBE_HZ);
    if (telemetryChannelIds.has(e.id)) rate = Math.min(e.maxRateHz, posHz);
    wishes.push([e.id, rate, e.priority != null ? e.priority : PRIORITY.background]);
  }
  const cap = (typeof welcomeLimits.max_subscriptions === 'number' && welcomeLimits.max_subscriptions > 0)
    ? welcomeLimits.max_subscriptions : wishes.length;
  if (wishes.length <= cap) return wishes;
  const ranked = wishes.slice().sort((a, b) => b[2] - a[2]);
  return ranked.slice(0, cap);
}

const wishes = subscriptionWishes();
const perFrame = (typeof welcomeLimits.max_subscriptions_per_frame === 'number' && welcomeLimits.max_subscriptions_per_frame > 0)
  ? welcomeLimits.max_subscriptions_per_frame : 8;
for (let i = 0; i < wishes.length; i += perFrame) s.subscribe(wishes.slice(i, i + perFrame));

let grantedRate = null;
s.on('grant', (grants) => {
  for (const g of grants || []) if (g.channel === posField.channelId) grantedRate = g.rate;
});

// ---- capture ------------------------------------------------------------
const posSamples = []; // {t, v}
const targetSamples = [];
s.on('state', (channelId, sample, tsMs) => {
  if (channelId === posField.channelId) {
    const v = sample[posField.name];
    if (typeof v === 'number' && isFinite(v)) posSamples.push({ t: tsMs, v });
  }
  if (targetField && channelId === targetField.channelId) {
    const v = sample[targetField.name];
    if (typeof v === 'number' && isFinite(v)) targetSamples.push({ t: tsMs, v });
  }
});

await new Promise((r) => setTimeout(r, 1200)); // let grants land + retained snapshot arrive
console.log('granted rate         : ' + (grantedRate == null ? 'NO GRANT SEEN' : grantedRate + ' Hz'));

// ---- command the sweep ----------------------------------------------------
for (const leg of LEGS) {
  try {
    const echo = await s.sendMove(leg);
    const applied = (echo && echo.applied && echo.applied[1] != null) ? echo.applied[1] : leg;
    console.log('  leg -> ' + leg + ' mm  (applied ' + applied + ')');
  } catch (err) {
    console.log('  leg -> ' + leg + ' mm  REFUSED: ' + ((err && (err.name || err.message)) || err));
  }
  await new Promise((r) => setTimeout(r, LEG_WAIT_MS));
}

s.close();
await new Promise((r) => setTimeout(r, 300));

// ---- analysis ---------------------------------------------------------------
// Date.now() has 1ms resolution; a burst of >=2 STATE pushes for the SAME
// channel dispatched inside one JS macrotask (WS message coalescing) can
// legitimately stamp two DIFFERENT pushes with the same tsMs, or 1ms apart.
// telebuf.js's push() already drops a non-newer-tsMs sample outright (dt<=0
// is silently discarded, so it never reaches the renderer) — but a naive 2nd
// finite-difference blows up on a tiny positive dt (h0/h1 near zero dividing
// the formula), which is a measurement artifact, not real machine jerk: the
// firmware's own accel ceiling (20000 mm/s^2 normal tier, per
// /api/capabilities) bounds what real motion can produce. DUPE_FLOOR_MS
// excludes any second-difference whose EITHER half-interval is too small to
// trust — same floor telebuf effectively applies by dropping dt<=0 outright.
const DUPE_FLOOR_MS = 3;

function interArrivalStats(samples) {
  if (samples.length < 3) return null;
  const deltas = [];
  for (let i = 1; i < samples.length; i++) deltas.push(samples[i].t - samples[i - 1].t);
  const sorted = [...deltas].sort((a, b) => a - b);
  const sum = deltas.reduce((a, b) => a + b, 0);
  const mean = sum / deltas.length;
  const p50 = sorted[Math.floor(sorted.length * 0.5)];
  const p95 = sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * 0.95))];
  const dupes = deltas.filter((d) => d < DUPE_FLOOR_MS).length;
  return { n: samples.length, mean, p50, p95, max: sorted[sorted.length - 1], min: sorted[0], dupes };
}

/** Non-uniform-grid second derivative (implied acceleration, units/s^2) at every interior sample. */
function impliedAccel(samples) {
  const out = [];
  for (let i = 1; i < samples.length - 1; i++) {
    const t0 = samples[i - 1].t / 1000, t1 = samples[i].t / 1000, t2 = samples[i + 1].t / 1000;
    const v0 = samples[i - 1].v, v1 = samples[i].v, v2 = samples[i + 1].v;
    const h0 = t1 - t0, h1 = t2 - t1;
    if (h0 * 1000 < DUPE_FLOOR_MS || h1 * 1000 < DUPE_FLOOR_MS) continue; // skip ms-resolution dupes
    // standard non-uniform 2nd-derivative finite difference
    const a = 2 * (h0 * v2 - (h0 + h1) * v1 + h1 * v0) / (h0 * h1 * (h0 + h1));
    if (isFinite(a)) out.push(a);
  }
  return out;
}

function accelStats(a) {
  if (!a.length) return null;
  const abs = a.map(Math.abs).sort((x, y) => x - y);
  const sumSq = a.reduce((s2, x) => s2 + x * x, 0);
  const rms = Math.sqrt(sumSq / a.length);
  const p95 = abs[Math.min(abs.length - 1, Math.floor(abs.length * 0.95))];
  const max = abs[abs.length - 1];
  return { n: a.length, rms, p95, max };
}

console.log('\n=== RESULT (mode=' + MODE + ', wished ' + posHz + ' Hz, granted ' + grantedRate + ' Hz) ===');

const ia = interArrivalStats(posSamples);
console.log('\nposition channel inter-arrival (n=' + posSamples.length + '):');
console.log(ia ? '  mean=' + ia.mean.toFixed(1) + 'ms  p50=' + ia.p50.toFixed(1) + 'ms  p95=' + ia.p95.toFixed(1)
  + 'ms  max=' + ia.max.toFixed(1) + 'ms  min=' + ia.min.toFixed(1) + 'ms  dupes(<' + DUPE_FLOOR_MS + 'ms)=' + ia.dupes : '  too few samples');

const acc = accelStats(impliedAccel(posSamples));
console.log('\nposition implied acceleration (2nd difference, unit/s^2, "unit" = ' + (posField.unit || 'device unit') + '):');
console.log(acc ? '  n=' + acc.n + '  rms=' + acc.rms.toFixed(2) + '  p95(|a|)=' + acc.p95.toFixed(2) + '  max(|a|)=' + acc.max.toFixed(2) : '  too few samples');

if (targetField) {
  const iaT = interArrivalStats(targetSamples);
  console.log('\ntarget channel inter-arrival (n=' + targetSamples.length + '):');
  console.log(iaT ? '  mean=' + iaT.mean.toFixed(1) + 'ms  p50=' + iaT.p50.toFixed(1) + 'ms  p95=' + iaT.p95.toFixed(1)
    + 'ms  max=' + iaT.max.toFixed(1) + 'ms  min=' + iaT.min.toFixed(1) + 'ms' : '  too few samples');
}

console.log('\nNACKs                : ' + nacks.length);
for (const n of nacks) console.log('  ' + JSON.stringify(n));

console.log('\nCSV_SUMMARY,' + MODE + ',' + posHz + ',' + grantedRate + ',' +
  (ia ? ia.mean.toFixed(2) : '') + ',' + (ia ? ia.p50.toFixed(2) : '') + ',' + (ia ? ia.p95.toFixed(2) : '') + ',' + (ia ? ia.max.toFixed(2) : '') + ',' +
  (acc ? acc.rms.toFixed(3) : '') + ',' + (acc ? acc.p95.toFixed(3) : '') + ',' + (acc ? acc.max.toFixed(3) : ''));

if (DUMP_PATH) {
  writeFileSync(DUMP_PATH, JSON.stringify({
    host: HOST, mode: MODE, wishedHz: posHz, grantedHz: grantedRate,
    unit: posField.unit || 'mm', posSamples, targetSamples,
  }));
  console.log('\ndumped raw trace -> ' + DUMP_PATH);
}

process.exit(0);
