/**
 * telebuf-sim.mjs — synthetic replay proving the render-delay fix against
 * REALISTIC arrival jitter (measured on-device via jitter-measure.mjs: mean
 * ~20-45ms, p95 45-55ms, occasional gaps to 120ms). Not part of the app.
 *
 * Simulates constant-velocity motion (10 mm/s) pushed into a telebuf at
 * irregular intervals drawn from the measured distribution, then compares
 * rAF-rendered output (a) sampled at raw "now" (the current/buggy behavior)
 * vs (b) sampled at renderClock.stableRenderTime(now) (the fix). Metric:
 * per-frame velocity implied by consecutive rendered positions — smooth
 * motion should show a tight, low-variance distribution around 10mm/s;
 * snap-and-hold shows alternating ~0 and large spikes (the "jitter").
 *
 * Also carries deterministic pass/fail assertions (below the simulation) for
 * sampleAt()'s cubic-Hermite interpolation: velocity continuity across a
 * shared sample boundary, the linear fallback for a tangent-less first span,
 * and the overshoot clamp on a bad velocity estimate. Exits nonzero on any
 * assertion failure.
 *
 * Run: node webui/test/telebuf-sim.mjs
 */
import { createTelebuf, createRenderClock } from '../src/ui/hero/telebuf.js';

const TRUE_VEL_MM_S = 10;
const SIM_MS = 8000;
const RAF_DT = 16.667;

// Jittered arrival intervals: mostly ~20ms, occasional gap to ~120ms,
// matching the on-device WS measurement (mean 19.5ms, p95 55.5ms, max 120ms).
function nextGapMs(rng) {
  const r = rng();
  if (r < 0.85) return 15 + rng() * 15;      // 15-30ms, the common case
  if (r < 0.97) return 30 + rng() * 30;      // 30-60ms, a slower beat
  return 60 + rng() * 60;                     // 60-120ms, a real stall
}

function mulberry32(seed) {
  let a = seed;
  return function () {
    a |= 0; a = (a + 0x6D2B79F5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function simulate(useDelay) {
  const rng = mulberry32(42);
  const tele = createTelebuf();
  const clock = createRenderClock();

  let simT = 0;       // "true" clock, ms
  let nextPushAt = 0;
  let pos = 0;

  const framePositions = [];
  let rafT = 0;
  let lastFrameDt = RAF_DT;

  while (simT < SIM_MS) {
    // Advance real pushes up to current sim time.
    while (nextPushAt <= simT) {
      pos = TRUE_VEL_MM_S * (nextPushAt / 1000);
      tele.push(pos, nextPushAt);
      clock.noteArrival(nextPushAt);
      nextPushAt += nextGapMs(rng);
    }

    clock.update(lastFrameDt);
    const sampleAtT = useDelay ? clock.stableRenderTime(simT) : simT;
    const r = tele.sampleAt(sampleAtT);
    framePositions.push({ t: simT, v: r.value });

    simT += RAF_DT;
    lastFrameDt = RAF_DT;
  }
  return framePositions;
}

function analyze(label, frames) {
  // Skip the first 200ms warm-up (buffer/delay still filling).
  const warm = frames.filter((f) => f.t > 200 && f.v != null);
  const vels = [];
  for (let i = 1; i < warm.length; i++) {
    const dv = warm[i].v - warm[i - 1].v;
    const dt = (warm[i].t - warm[i - 1].t) / 1000;
    vels.push(dv / dt);
  }
  const mean = vels.reduce((a, b) => a + b, 0) / vels.length;
  const variance = vels.reduce((a, b) => a + (b - mean) ** 2, 0) / vels.length;
  const sd = Math.sqrt(variance);
  const holds = vels.filter((v) => Math.abs(v) < 0.01).length;
  const spikes = vels.filter((v) => Math.abs(v) > TRUE_VEL_MM_S * 2).length;
  console.log(`${label}: mean implied vel=${mean.toFixed(2)}mm/s  stdev=${sd.toFixed(2)}mm/s  ` +
    `held-frames=${holds}/${vels.length} (${(100 * holds / vels.length).toFixed(1)}%)  ` +
    `spike-frames(>2x true vel)=${spikes}`);
}

console.log(`True velocity: ${TRUE_VEL_MM_S} mm/s. Arrival jitter drawn from the on-device WS measurement.\n`);
analyze('BEFORE (raw "now", no render delay)', simulate(false));
analyze('AFTER  (adaptive render-delay clock)', simulate(true));

// ---------------------------------------------------------------------------
// Deterministic assertions — Hermite continuity and overshoot clamping.
//
// sampleAt() Hermite-interpolates between bracketing samples using a
// per-sample tangent stored at push time (bufVel). Reusing the SAME stored
// tangent as both a span's outgoing derivative and the next span's incoming
// derivative is supposed to make the curve's velocity match exactly at the
// shared sample boundary, regardless of the two spans having different
// lengths. These checks prove that property numerically rather than trusting
// the algebra, and prove the overshoot clamp actually bites on a bad
// velocity estimate rather than only on paper.
// ---------------------------------------------------------------------------

// The deterministic fixtures below hand-craft TRUSTED timestamps to probe the
// interpolation math itself, so they opt out of the arrival-time rescheduling
// (jitter regression #5) — which has its own burst-replay section further down.
let fails = 0;
const ok = (name, cond, extra) => {
  console.log('  [' + (cond ? 'PASS' : 'FAIL') + '] ' + name + (extra ? '  — ' + extra : ''));
  if (!cond) fails++;
};

console.log('\ntelebuf.js — Hermite continuity and clamp assertions\n');

// ---- claim: velocity is continuous across a shared sample boundary --------
{
  const tele = createTelebuf({ reschedule: false });
  // Three real samples with uneven spans (40ms then 50ms) and different
  // implied velocities either side of B, so a naive per-span tangent would
  // show a visible kink at B if continuity did not hold.
  tele.push(0, 0);     // A
  tele.push(10, 40);   // B: v_B = 10/40 = 0.25 units/ms
  tele.push(15, 90);   // C: v_C = 5/50  = 0.10 units/ms

  const d = 0.5; // ms, small enough to approximate the instantaneous slope
  const leftSlope = (tele.sampleAt(40).value - tele.sampleAt(40 - d).value) / d;
  const rightSlope = (tele.sampleAt(40 + d).value - tele.sampleAt(40).value) / d;
  const slopeDiff = Math.abs(leftSlope - rightSlope);
  ok('Hermite slope matches across the shared A-B/B-C boundary',
     slopeDiff < 0.01,
     `left=${leftSlope.toFixed(4)} right=${rightSlope.toFixed(4)} diff=${slopeDiff.toFixed(5)}`);
}

// ---- claim: linear fallback still applies to the first (tangent-less) span ----
{
  const tele = createTelebuf({ reschedule: false });
  tele.push(0, 0);    // A: no prior sample -> bufVel[A] is NaN
  tele.push(10, 40);  // B
  const mid = tele.sampleAt(20).value; // exact midpoint of a straight 0->10 run
  ok('first span (no stored tangent) falls back to linear', Math.abs(mid - 5) < 1e-9,
     `mid=${mid}`);
}

// ---- claim: a bad velocity estimate cannot fling the marker past the clamp ----
{
  const tele = createTelebuf({ reschedule: false });
  tele.push(0, 0);      // A
  tele.push(1000, 10);  // B: v_B = 100 units/ms — an absurd jump
  tele.push(1001, 20);  // C: v_C = 0.1 units/ms

  const overshoot = 0.15 * Math.abs(1001 - 1000); // = 0.15, per sampleAt()'s own rule
  const lo = Math.min(1000, 1001) - overshoot;
  const hi = Math.max(1000, 1001) + overshoot;
  const mid = tele.sampleAt(15).value; // midpoint of the B-C span, where B's huge tangent dominates
  ok('overshoot from an absurd velocity estimate is clamped',
     mid >= lo - 1e-9 && mid <= hi + 1e-9,
     `value=${mid.toFixed(4)} bound=[${lo.toFixed(4)}, ${hi.toFixed(4)}]`);
  // B's huge incoming tangent pushes the raw (unclamped) curve above `hi`
  // for this fixture — confirm the clamp actually saturated at the ceiling
  // rather than the fixture happening to land inside the bound on its own.
  ok('the clamp actually engaged (value pinned to the ceiling)',
     Math.abs(mid - hi) < 1e-9, `value=${mid.toFixed(4)} hi=${hi.toFixed(4)}`);
}

// ---------------------------------------------------------------------------
// Burst replay — jitter regression #5 (arrival-time stamping).
//
// Reproduces the on-device measurement of 2026-07-28: the hub samples evenly
// (~30ms) but the network delivers CLUMPS — three samples land with one
// identical Date.now() stamp, then nothing for ~90ms. The old push() dropped
// the duplicates (~18% of all motion) and the display snapped/froze (107
// snap frames + 17 freezes in 719 rendered). With rescheduling (the default),
// every sample must survive and the rendered output must be smooth.
// ---------------------------------------------------------------------------
{
  const tele = createTelebuf();
  const clock = createRenderClock();
  const VEL = 10 / 1000;               // 10 mm/s in mm per ms
  const BURST_EVERY = 90, PER_BURST = 3, SIM = 6000;

  let pushed = 0;
  const frames = [];
  let nextBurst = 0;
  for (let now = 0; now < SIM; now += RAF_DT) {
    while (nextBurst <= now) {
      const arrive = nextBurst;        // all three share ONE arrival stamp
      for (let k = PER_BURST - 1; k >= 0; k--) {
        const sampledAt = arrive - k * (BURST_EVERY / PER_BURST); // hub's even sampling instants
        tele.push(VEL * sampledAt, arrive);
        clock.noteArrival(arrive);
        pushed++;
      }
      nextBurst += BURST_EVERY;
    }
    clock.update(RAF_DT);
    const r = tele.sampleAt(clock.stableRenderTime(now));
    frames.push(r.value);
  }

  ok('burst replay keeps every sample (duplicates no longer dropped)',
     tele.length === Math.min(pushed, 256), 'kept=' + tele.length + ' pushed=' + pushed);

  const warm = frames.filter((v, i) => v != null && i > 30);
  const vels = [];
  for (let i = 1; i < warm.length; i++) vels.push((warm[i] - warm[i - 1]) / (RAF_DT / 1000));
  const spikes = vels.filter((v) => Math.abs(v) > 2 * 10).length;
  const holds = vels.filter((v) => Math.abs(v) < 0.01).length;
  const mean = vels.reduce((a, b) => a + b, 0) / vels.length;
  ok('burst replay renders zero snap frames (implied vel never >2x true)',
     spikes === 0, 'spikes=' + spikes + '/' + vels.length);
  ok('burst replay renders (almost) zero held frames', holds <= vels.length * 0.02,
     'held=' + holds + '/' + vels.length);
  ok('burst replay mean implied velocity tracks truth', Math.abs(mean - 10) < 1,
     'mean=' + mean.toFixed(2) + 'mm/s');
}

console.log('\n' + (fails ? 'FAILURES: ' + fails : 'ALL PASS'));
process.exit(fails ? 1 : 0);
