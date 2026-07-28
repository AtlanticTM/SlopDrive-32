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
