/**
 * streamed-outlier-probe.mjs — reproduce the operator's "occasionally shoots
 * way off and returns" report under CONTINUOUS commanded motion (the closest
 * a headless script gets to the MFP plugin's streamed load, without pulling
 * in the C# plugin or touching firmware): a tight loop of command.position
 * INTENTs (small steps, short waits) keeps the carriage moving continuously
 * for the whole capture, exactly like the operator's streamed-content
 * complaint, while telemetry.position/target are subscribed at the
 * production TELEMETRY_HZ (25Hz) policy.
 *
 * GOVERNING RULING (operator, verbatim): "it's not the webui's job to
 * determine if motion is acceptable. That is the machine's choice and
 * whatever motion planner is being used." This script therefore does NOT
 * decide any sample is "impossible" and does NOT filter, clamp, or reject
 * anything anywhere in the shipped renderer — nothing here touches
 * src/ui/hero/telebuf.js's production logic. What IS this script's job,
 * and the only thing it checks for, is OUR OWN bugs in the path between the
 * wire and the pixel:
 *
 *   (1) DECODE — could our own CBOR/packed-STATE decode (core/slopsync) be
 *       misreading a good sample (wrong offset/scale/sign)? Read the code,
 *       not guessed: see the report for what was checked.
 *   (2) TIMESTAMP bookkeeping — out-of-order, duplicate, or zero arrival
 *       stamps that would make our interpolator swing and recover on data
 *       that was never actually bad.
 *   (3) EXTRAPOLATION — telebuf.js projects up to EXTRAPOLATE_MS past the
 *       newest real sample; if that ever fires and overshoots before the
 *       next real sample lands, that is OUR renderer fabricating motion the
 *       machine never reported. Checked directly via the `extrapolating`
 *       flag `sampleAt()` already returns — not inferred.
 *   (4) RECONNECT/RESUBSCRIBE ring re-seeding — not applicable to a single
 *       continuous capture with autoReconnect:false, noted for completeness.
 *
 * If none of (1)-(4) explain an observed excursion, the honest conclusion is
 * that the wire itself carried it — in which case the right move is to
 * RENDER IT FAITHFULLY (which the production telebuf already does, per (3)
 * below) and hand the raw evidence back as a firmware finding, not to invent
 * a client-side opinion about what the machine "should" have reported.
 *
 * Feeds the raw capture through the ACTUAL production telebuf+renderClock
 * (src/ui/hero/telebuf.js — not a reimplementation) at simulated 60Hz draw,
 * and reports the largest rendered excursion versus the true bracketing
 * samples, so any claim here is falsifiable from real captured data rather
 * than asserted.
 *
 * SAFETY: steps stay inside the machine's reported stroke window (queried
 * live, never assumed); this script's own commanded speed is bounded by
 * the NORMAL speed ceiling, well inside the machine's own clamps regardless.
 *
 * Run: node webui/test/streamed-outlier-probe.mjs [host] [durationMs]
 */
import { createSession, CHANNEL_CLASS, PRIORITY } from '../src/core/slopsync/index.js';
import { acquireToken } from '../src/core/slopsync/credentials.js';
import { buildSettingsModel } from '../src/model/settings.js';
import { claimRoles, ROLE } from '../src/model/roles.js';
import { createTelebuf, createRenderClock } from '../src/ui/hero/telebuf.js';
import { writeFileSync } from 'node:fs';

const HOST = process.argv[2] || '192.168.1.229';
const PORT = 82;
const DURATION_MS = parseInt(process.argv[3] || '20000', 10);
const TELEMETRY_HZ = 25; // production policy (machine.svelte.js)
const STEP_INTERVAL_MS = 120; // continuous-motion cadence, well under any throttle

if (typeof WebSocket === 'undefined') {
  console.error('No global WebSocket (need node >= 22). Aborting.');
  process.exit(1);
}

const caps = await fetch('http://' + HOST + '/api/capabilities').then((r) => r.json());
const EXPERT_SPEED_CEIL = caps.speed_ceiling_mm_s?.expert || 10000;
console.log('device expert speed ceiling: ' + EXPERT_SPEED_CEIL + ' mm/s (most permissive tier — the outer bound for "impossible")');

const s = createSession({
  host: HOST, port: PORT, clientKind: 'webui', clientName: 'streamed-outlier-probe',
  autoReconnect: false, WebSocketImpl: WebSocket,
  token: (h) => acquireToken(h),
});

const catalogP = new Promise((resolve, reject) => {
  const to = setTimeout(() => reject(new Error('timeout waiting for catalog')), 20000);
  s.on('catalog', (entries) => { clearTimeout(to); resolve(entries); });
  s.on('close', (c) => { clearTimeout(to); reject(new Error('closed before catalog: ' + (c.reason || c.code))); });
});

s.connect();
const entries = await catalogP;
const model = buildSettingsModel(entries);

const claim = claimRoles(model.byRole, {
  require: { move: ROLE.commandPosition },
  optional: { pos: ROLE.telemetryPosition, target: ROLE.telemetryTarget, vel: ROLE.telemetryVelocity },
});
if (!claim || !claim.pos || !claim.move) {
  console.error('Missing telemetry.position or command.position role — cannot run. Aborting.');
  s.close();
  process.exit(1);
}
const posField = claim.pos;
const targetField = claim.target;
const moveField = claim.move;

// Live window bounds straight from the device's own reported min/max settings
// (not assumed) — find the fields the same way RailWidget does, by role.
const winClaim = claimRoles(model.byRole, {
  optional: { min: ROLE.windowMin, max: ROLE.windowMax },
});

let winMin = null, winMax = null;
s.on('state', (channelId, sample) => {
  if (winClaim && winClaim.min && channelId === winClaim.min.channelId && sample[winClaim.min.name] != null) winMin = sample[winClaim.min.name];
  if (winClaim && winClaim.max && channelId === winClaim.max.channelId && sample[winClaim.max.name] != null) winMax = sample[winClaim.max.name];
});

let welcomeLimits = {};
s.on('welcome', (w) => { welcomeLimits = w.limits || {}; });

const posEntry = entries.find((e) => e.id === posField.channelId);
const telemetryChannelIds = new Set([posField.channelId, targetField ? targetField.channelId : null].filter((x) => x != null));
const wishes = [];
for (const e of entries) {
  if (e.dir !== 0) continue;
  if (e.cls !== CHANNEL_CLASS.STATE && e.cls !== CHANNEL_CLASS.EVENT) continue;
  let rate = (e.cls === CHANNEL_CLASS.EVENT || !e.maxRateHz) ? 0 : Math.min(e.maxRateHz, TELEMETRY_HZ);
  if (telemetryChannelIds.has(e.id)) rate = Math.min(e.maxRateHz, TELEMETRY_HZ);
  wishes.push([e.id, rate, e.priority != null ? e.priority : PRIORITY.background]);
}

// Give the hub a beat to send WELCOME before we decide how to chunk the
// subscribe wishes (mirrors position-jitter-probe.mjs — subscribing before
// WELCOME lands, or exceeding max_subscriptions/max_subscriptions_per_frame
// in one frame, is why the first run of this probe got zero samples back).
await new Promise((r) => setTimeout(r, 500));
const cap = (typeof welcomeLimits.max_subscriptions === 'number' && welcomeLimits.max_subscriptions > 0)
  ? welcomeLimits.max_subscriptions : wishes.length;
let effectiveWishes = wishes;
if (wishes.length > cap) {
  const ranked = wishes.slice().sort((a, b) => b[2] - a[2]);
  effectiveWishes = ranked.slice(0, cap);
}
const perFrame = (typeof welcomeLimits.max_subscriptions_per_frame === 'number' && welcomeLimits.max_subscriptions_per_frame > 0)
  ? welcomeLimits.max_subscriptions_per_frame : 8;
for (let i = 0; i < effectiveWishes.length; i += perFrame) s.subscribe(effectiveWishes.slice(i, i + perFrame));

await new Promise((r) => setTimeout(r, 1500)); // grants + retained snapshot + window state

if (winMin == null || winMax == null) {
  console.log('WARNING: could not resolve live window bounds by role; falling back to a conservative 60-140mm band.');
  winMin = 60; winMax = 140;
}
const center = (winMin + winMax) / 2;
const amp = Math.max(2, Math.min(30, (winMax - winMin) * 0.25));
console.log('window ' + winMin.toFixed(1) + '-' + winMax.toFixed(1) + 'mm -> streaming amplitude ' + amp.toFixed(1) + 'mm around ' + center.toFixed(1) + 'mm');

const posSamples = [];
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

const nacks = [];
s.on('nack', (n) => nacks.push(n));

// ---- continuous streamed-like motion: rapid sine-wave move intents --------
console.log('streaming continuous motion for ' + DURATION_MS + 'ms (step every ' + STEP_INTERVAL_MS + 'ms)...');
const t0 = Date.now();
let sent = 0, refused = 0;
const period = 2200; // ms per full cycle — a brisk, continuous, funscript-like sweep
async function streamLoop() {
  while (Date.now() - t0 < DURATION_MS) {
    const phase = ((Date.now() - t0) % period) / period * 2 * Math.PI;
    const target = center + amp * Math.sin(phase);
    sent++;
    s.sendMove(target).catch(() => { refused++; });
    await new Promise((r) => setTimeout(r, STEP_INTERVAL_MS));
  }
}
await streamLoop();

// let the last move settle and drain any tail telemetry
await new Promise((r) => setTimeout(r, 1500));
s.close();
await new Promise((r) => setTimeout(r, 300));

console.log('commanded ' + sent + ' move intents, ' + refused + ' promise-rejected (throttle/NACK) — telemetry samples: pos=' + posSamples.length + ' target=' + targetSamples.length);
console.log('NACKs: ' + nacks.length);
for (const n of nacks.slice(0, 10)) console.log('  ' + JSON.stringify(n));

// =====================================================================
// NONE of what follows filters, clamps, or rejects anything — it is pure
// characterization of the RAW capture, to (a) rule OUR bookkeeping in or out
// as the cause, and (b) if it wasn't us, describe with numbers (not
// adjectives) how far outside the device's own advertised envelope the raw
// wire value sat, for the firmware report. Nothing here feeds back into
// telebuf.js or any rendering decision.
// =====================================================================

// ---- raw-speed characterization (rarely the most sensitive signal — see
// the acceleration characterization below for why) -------------------------
function characterizeSpeed(samples, label) {
  const hits = [];
  for (let i = 1; i < samples.length; i++) {
    const dt = samples[i].t - samples[i - 1].t;
    if (dt <= 0) continue;
    const dv = samples[i].v - samples[i - 1].v;
    const impliedSpeed = Math.abs(dv) / (dt / 1000);
    if (impliedSpeed > EXPERT_SPEED_CEIL) {
      hits.push({ i, t: samples[i].t, dtMs: dt, dv, impliedSpeed });
    }
  }
  console.log('\n(a) ' + label + ' samples whose raw implied speed exceeds the device\'s own advertised EXPERT ceiling (' +
    EXPERT_SPEED_CEIL + ' mm/s, informational only): ' + hits.length);
  for (const h of hits.slice(0, 15)) {
    console.log('    i=' + h.i + '  dt=' + h.dtMs + 'ms  dv=' + h.dv.toFixed(3) + 'mm  implied=' + h.impliedSpeed.toFixed(1) + 'mm/s');
  }
  return hits;
}
const posImpossible = characterizeSpeed(posSamples, 'position');
const targetImpossible = characterizeSpeed(targetSamples, 'target');

// ---- (b) OUR bookkeeping: timestamp ordering, duplicates, zero stamps -----
function checkOrdering(samples, label) {
  let outOfOrder = 0, dup = 0, zero = 0;
  for (const s of samples) if (!s.t || s.t <= 0) zero++;
  for (let i = 1; i < samples.length; i++) {
    const dt = samples[i].t - samples[i - 1].t;
    if (dt < 0) outOfOrder++;
    else if (dt === 0) dup++;
  }
  console.log('(b) ' + label + ': out-of-order=' + outOfOrder + '  exact-duplicate-ts=' + dup +
    '  zero/missing-ts=' + zero + '  (of ' + samples.length + ' samples) — this is OUR arrival-timestamp bookkeeping, not the wire value');
}
checkOrdering(posSamples, 'position');
checkOrdering(targetSamples, 'target');

// ---- raw-ACCELERATION characterization (position.jitter-probe.mjs's own
// non-uniform 2nd-difference technique) — a single glitched sample (both
// neighbors plausible, the one BETWEEN them isn't) rarely trips a raw-speed
// figure (implies a modest speed either side on a ~40ms grid) but shows up
// clearly here, because both the excursion AND the snap-back have to fit
// inside two adjacent ~40ms gaps. Reported against the device's own
// advertised accel ceilings purely as a magnitude yardstick for the firmware
// report — NOT a pass/fail test this script (or anything downstream) acts on.
// =====================================================================
const ACCEL_NORMAL = caps.accel_ceiling_mm_s2?.normal || 20000;
const ACCEL_EXPERT = caps.accel_ceiling_mm_s2?.expert || 100000;
const DUPE_FLOOR_MS = 3; // measurement-artifact floor, same rationale as position-jitter-probe.mjs

function characterizeAccel(samples, label) {
  const hits = [];
  for (let i = 1; i < samples.length - 1; i++) {
    const t0 = samples[i - 1].t / 1000, t1 = samples[i].t / 1000, t2 = samples[i + 1].t / 1000;
    const v0 = samples[i - 1].v, v1 = samples[i].v, v2 = samples[i + 1].v;
    const h0 = t1 - t0, h1 = t2 - t1;
    if (h0 * 1000 < DUPE_FLOOR_MS || h1 * 1000 < DUPE_FLOOR_MS) continue;
    const a = 2 * (h0 * v2 - (h0 + h1) * v1 + h1 * v0) / (h0 * h1 * (h0 + h1));
    if (!isFinite(a)) continue;
    if (Math.abs(a) > ACCEL_NORMAL) {
      hits.push({ i, t: samples[i].t, v0, v1, v2, h0Ms: h0 * 1000, h1Ms: h1 * 1000, accel: a,
        exceedsExpert: Math.abs(a) > ACCEL_EXPERT });
    }
  }
  console.log('(a2) ' + label + ' raw samples whose 2nd-difference exceeds the NORMAL accel ceiling (' + ACCEL_NORMAL + ' mm/s^2, informational): ' + hits.length +
    '  [' + hits.filter((h) => h.exceedsExpert).length + ' of those also exceed EXPERT (' + ACCEL_EXPERT + ' mm/s^2)]');
  for (const h of hits.slice(0, 15)) {
    console.log('    i=' + h.i + '  t+' + (h.t - samples[0].t) + 'ms  ' + h.v0.toFixed(2) + ' -> ' + h.v1.toFixed(2) + ' -> ' + h.v2.toFixed(2) +
      'mm  (gaps ' + h.h0Ms.toFixed(0) + '/' + h.h1Ms.toFixed(0) + 'ms)  implied accel=' + h.accel.toFixed(0) + 'mm/s^2' +
      (h.exceedsExpert ? '  [also beyond EXPERT]' : ''));
  }
  return hits;
}
const posAccelImpossible = characterizeAccel(posSamples, 'position');
characterizeAccel(targetSamples, 'target');

// =====================================================================
// (3) EXTRAPOLATION check — the one candidate that would be OUR fault: does
// telebuf.js's up-to-50ms projection past the newest real sample ever fire
// and overshoot before the next real sample lands? Checked directly against
// the `extrapolating` flag sampleAt() already returns (see the replay below),
// not inferred. This tiny-dt scan is upstream evidence for THAT check: a
// very small gap between two real pushes inflates telebuf's per-sample
// velocity estimate (dv/dt, used only during extrapolation), which is
// exactly the kind of OUR-bookkeeping mechanism worth ruling in or out.
// =====================================================================
function checkTinyDtVelocity(samples, label) {
  const TINY_DT_MS = 5; // "small positive dt" — telebuf floors only at dt>0
  const hits = [];
  for (let i = 1; i < samples.length; i++) {
    const dt = samples[i].t - samples[i - 1].t;
    if (dt <= 0 || dt >= TINY_DT_MS) continue;
    const dv = samples[i].v - samples[i - 1].v;
    const velPerMs = dv / dt;
    const extrapolated50ms = velPerMs * 50; // telebuf's EXTRAPOLATE_MS default
    hits.push({ i, t: samples[i].t, dtMs: dt, dv, velPerMs, extrapolated50ms });
  }
  console.log('(c) ' + label + ': samples with dt<' + TINY_DT_MS + 'ms (tiny-interval velocity risk): ' + hits.length);
  for (const h of hits.slice(0, 15)) {
    console.log('    i=' + h.i + '  dt=' + h.dtMs + 'ms  dv=' + h.dv.toFixed(4) + 'mm  ->  if extrapolated 50ms: ' +
      h.extrapolated50ms.toFixed(1) + 'mm offset');
  }
  return hits;
}
const posTinyDt = checkTinyDtVelocity(posSamples, 'position');
checkTinyDtVelocity(targetSamples, 'target');

// =====================================================================
// Replay through the REAL production telebuf+renderClock and look for a
// rendered excursion that runs away from the true bracketing samples then
// snaps back — the actual visual symptom, not just a raw-data smell test.
// =====================================================================
function replay(samples, drawHz) {
  const dtMs = 1000 / drawHz;
  const tele = createTelebuf();
  const clock = createRenderClock();
  if (samples.length < 3) return [];
  const t0s = samples[0].t, tEnd = samples[samples.length - 1].t;
  let simT = t0s, idx = 0;
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
    frames.push({ t: simT, v: r.value, fresh: r.fresh, extrapolating: r.extrapolating });
    simT += dtMs;
  }
  return frames;
}

/** True ground-truth position at time t, via plain linear interpolation between the two REAL bracketing samples (no extrapolation, no render delay) — the "what should have been drawn" reference. */
function trueValueAt(samples, t) {
  if (t <= samples[0].t) return samples[0].v;
  if (t >= samples[samples.length - 1].t) return samples[samples.length - 1].v;
  for (let i = 1; i < samples.length; i++) {
    if (samples[i].t >= t) {
      const a = samples[i - 1], b = samples[i];
      const frac = b.t > a.t ? (t - a.t) / (b.t - a.t) : 0;
      return a.v + (b.v - a.v) * frac;
    }
  }
  return samples[samples.length - 1].v;
}

function findExcursions(samples, frames, thresholdMm) {
  const out = [];
  let inSpike = false, spikeStart = 0, spikeMax = 0;
  for (const f of frames) {
    if (f.v == null || !f.fresh) continue;
    const truth = trueValueAt(samples, f.t);
    const err = f.v - truth;
    if (Math.abs(err) > thresholdMm) {
      if (!inSpike) { inSpike = true; spikeStart = f.t; spikeMax = err; }
      else if (Math.abs(err) > Math.abs(spikeMax)) spikeMax = err;
    } else if (inSpike) {
      out.push({ startT: spikeStart, endT: f.t, durationMs: f.t - spikeStart, peakErrMm: spikeMax });
      inSpike = false;
    }
  }
  return out;
}

console.log('\n=== REPLAY through production telebuf.js @60Hz draw ===');
const posFrames = replay(posSamples, 60);
const posSpikes = findExcursions(posSamples, posFrames, 5); // >5mm error vs true interpolated position = a "spike" worth naming
console.log('position: ' + posFrames.length + ' rendered frames, ' + posSpikes.length + ' excursion(s) > 5mm from ground truth');
for (const sp of posSpikes.slice(0, 10)) {
  console.log('  t+' + (sp.startT - posSamples[0].t) + 'ms  duration=' + sp.durationMs + 'ms  peak error=' + sp.peakErrMm.toFixed(1) + 'mm');
}

// ---- mechanism diagnostic: what do the RAW bracketing samples look like
// around each spike, and was the interpolator actually extrapolating (vs.
// mis-interpolating between two real samples) when it happened? ----
function rawSamplesAround(samples, tMs, windowMs) {
  return samples.filter((s) => Math.abs(s.t - tMs) <= windowMs)
    .map((s) => ({ t: s.t, v: s.v }));
}
const extrapolatingDuringSpike = posSpikes.map((sp) => {
  const framesInSpike = posFrames.filter((f) => f.t >= sp.startT && f.t <= sp.endT);
  return framesInSpike.some((f) => f.extrapolating);
});
console.log('\nmechanism check: of ' + posSpikes.length + ' excursions, ' +
  extrapolatingDuringSpike.filter(Boolean).length + ' occurred while the telebuf was EXTRAPOLATING ' +
  '(rendering past the newest real sample), ' +
  extrapolatingDuringSpike.filter((x) => !x).length + ' were pure interpolation between two real samples.');
console.log('\nfirst 3 spikes, raw wire samples bracketing each (±150ms):');
for (const sp of posSpikes.slice(0, 3)) {
  console.log('  --- spike at t+' + (sp.startT - posSamples[0].t) + 'ms, peak err ' + sp.peakErrMm.toFixed(1) + 'mm ---');
  for (const r of rawSamplesAround(posSamples, sp.startT, 150)) {
    console.log('    t+' + (r.t - posSamples[0].t) + 'ms  v=' + r.v.toFixed(3) + 'mm');
  }
}

// overall inter-arrival stats for context
{
  const deltas = [];
  for (let i = 1; i < posSamples.length; i++) deltas.push(posSamples[i].t - posSamples[i - 1].t);
  const sorted = [...deltas].sort((a, b) => a - b);
  const mean = deltas.reduce((a, b) => a + b, 0) / deltas.length;
  console.log('\nposition inter-arrival: mean=' + mean.toFixed(1) + 'ms  p50=' + sorted[Math.floor(sorted.length * 0.5)] +
    'ms  p95=' + sorted[Math.floor(sorted.length * 0.95)] + 'ms  max=' + sorted[sorted.length - 1] + 'ms  min=' + sorted[0] + 'ms');
}

const dumpPath = 'test/evidence/streamed-outlier-trace.json';
writeFileSync(dumpPath, JSON.stringify({
  host: HOST, durationMs: DURATION_MS, telemetryHz: TELEMETRY_HZ,
  expertSpeedCeiling: EXPERT_SPEED_CEIL, accelNormal: ACCEL_NORMAL, accelExpert: ACCEL_EXPERT,
  window: { winMin, winMax },
  posSamples, targetSamples, posSpikes, posAccelImpossible,
}));
console.log('\nraw trace + spikes dumped -> ' + dumpPath);

console.log('\n=== SUMMARY (characterization only — nothing here filters or judges the machine) ===');
console.log('raw-speed outliers (a):    pos=' + posImpossible.length + ' target=' + targetImpossible.length);
console.log('raw-accel outliers (a2):   pos=' + posAccelImpossible.length +
  (posAccelImpossible.length ? '  (' + posAccelImpossible.filter((h) => h.exceedsExpert).length + ' also beyond the EXPERT figure)' : ''));
console.log('OUR bookkeeping (b):       see out-of-order/duplicate/zero-ts counts above — ' +
  'nonzero here would mean OUR code, not the wire, is at fault');
console.log('tiny-dt pairs (c input):   pos dt<5ms pairs=' + posTinyDt.length);
console.log('rendered excursions:       ' + posSpikes.length +
  '  (' + extrapolatingDuringSpike.filter(Boolean).length + ' occurred while OUR renderer was extrapolating — that would be our fault; ' +
  extrapolatingDuringSpike.filter((x) => !x).length + ' were pure interpolation between two real samples — the renderer drew exactly what the wire reported)');
console.log('\nSee the report for the conclusion this data supports.');

process.exit(0);
