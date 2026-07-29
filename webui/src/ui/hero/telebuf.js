/**
 * telebuf.js — a tiny ring-buffer interpolator for smoothing ONE live scalar.
 *
 * This is a deliberately shrunk port of the pre-refactor `core/telebuf.js`.
 * The original also did WS-frame parsing and dual-clock (device/client) sync
 * because it fed off a raw binary telemetry plane; that plane is gone. Here,
 * `machine.svelte.js` already timestamps every decoded STATE sample
 * (`machine.sampleTs[channelId]`) with `Date.now()` — the epoch-ms domain set
 * at `session.js`'s `emit('state', ..., Date.now())` — so there is nothing
 * left to synchronize — a caller just pushes {value, tsMs} pairs as they
 * arrive off `machine.samples`/`machine.sampleTs`.
 *
 * CLOCK-DOMAIN TRAP (bit us once, guard stays): `sampleAt(tMs)` must be
 * called with a timestamp in that SAME Date.now() epoch-ms domain. A
 * `requestAnimationFrame` callback's own argument is a DOMHighResTimeStamp —
 * ms since `performance.timeOrigin`, a much smaller number — and passing it
 * straight through makes `tMs` permanently "older" than every real sample in
 * the ring, so `sampleAt()` always takes the `tMs <= bufT[head]` branch and
 * returns the OLDEST entry still buffered: with the default 256-deep ring at
 * ~30 Hz that reads as the display lagging live position by ~8-9 SECONDS,
 * constantly, which is exactly the "incredibly laggy" field report this
 * comment exists to prevent a repeat of. Callers driven by rAF must convert
 * with `performance.timeOrigin + rafTimestamp` before calling `sampleAt()` —
 * see RailWidget.svelte's `draw()`.
 *
 * What is kept, because it is still true at ~20-30 Hz STATE cadence rendered
 * at 60 fps rAF: samples arrive slower than frames are drawn, so a caller
 * wanting a smooth line needs to interpolate BETWEEN real samples and briefly
 * extrapolate PAST the newest one along its last velocity rather than
 * snapping/holding — the same policy the original used for the rail's comet
 * and hero numerals. `sampleAt()` never fabricates a value out of nothing: with
 * zero samples it reports `fresh:false` and a null value, which callers must
 * treat as "withhold", not "draw at zero" (Ground Truth Doctrine).
 *
 * Deliberately NOT a Svelte store / `.svelte.js` — this is called from inside
 * a rAF loop up to 60 times a second, and routing that through reactive state
 * would fire the whole reactivity graph for no reason. Callers push the
 * handful of *derived* display numbers they actually render into `$state`.
 *
 * ── The render-delay bug (jitter regression #3) ─────────────────────────────
 *
 * The pre-refactor `core/telebuf.js` never sampled at raw "now". It sampled at
 * `nowSynced - renderDelay`, where renderDelay was an adaptively-slewed buffer
 * (20-120ms, `p95_jitter*1.5 + frame_dt`) whose whole job was to keep the
 * render instant reliably BEHIND the newest real sample, so `sampleAt()`
 * almost always finds two real bracketing samples to interpolate between.
 * That delay was dropped in the port (its header now says so explicitly) —
 * the reasoning given was "there is nothing left to synchronize" (true: the
 * old delay's OTHER job, device/client clock-offset sync, really is gone now
 * that machine.svelte.js stamps every sample in one epoch-ms domain). But the
 * delay had a second, independent job the port's reasoning missed: it was
 * also a JITTER BUFFER against irregular sample arrival, which is still very
 * much a live problem — SlopSync STATE pushes arrive at ~20-30 Hz with real
 * gaps (measured on-device: mean ~20-45ms, p95 45-55ms, occasional gaps past
 * 100ms). Sampling at raw "now" with no buffer means any push arriving a
 * little late leaves "now" sitting PAST the newest sample, past the
 * extrapolate window, so the display FREEZES (the `pastMs > EXTRAPOLATE_MS`
 * hold branch) until the next push arrives and it SNAPS forward to catch up —
 * a visible stutter, worse the larger the gap. `createRenderClock` below is a
 * single-clock re-derivation of the old technique: no device/client offset
 * (nothing to sync), but the same "buffer by a bit more than the typical gap
 * between samples" idea, driven by the MEASURED gap between real pushes
 * instead of a removed network-clock jitter estimate.
 *
 * ── jitter regression #4: 50/2 outrun by hub-pacing bursts ──────────────────
 *
 * EXTRAPOLATE_MS=50 / SLEW_MS_PER_FRAME=2 (the values regression #3 above
 * landed with) held for steady ~20-45ms gaps but were sized to the p95, not
 * the tail: the hub's 5ms tick pacing occasionally strings together several
 * >100ms real gaps in a row (a burst, not one straggler). A single >100ms gap
 * already outruns a 50ms extrapolate window (hold-then-snap for that frame);
 * a BURST of them also outruns delayMs's ability to slew up fast enough — at
 * 2ms/frame, closing even a 60ms shortfall takes 30 frames (~0.5s at 60fps),
 * so the display sits in hold/snap for the whole burst instead of buffering
 * through it. EXTRAPOLATE_MS=80 covers a single straggler outright;
 * SLEW_MS_PER_FRAME=4 acquires a safe delay within ~1s of a burst starting
 * instead of chasing it for the burst's whole duration. Do not lower either
 * without re-measuring the on-device gap distribution — these are sized to
 * the worst observed tail, not the mean.
 */

/**
 * @param {{capacity?: number, holdMs?: number, extrapolateMs?: number}} [opts]
 *   capacity: ring depth. holdMs: how long a value is considered "fresh" once
 *   no newer sample has arrived. extrapolateMs: how far past the newest real
 *   sample to project along its last velocity before holding flat.
 */
export function createTelebuf(opts = {}) {
  const CAP = opts.capacity || 256;
  const HOLD_MS = opts.holdMs != null ? opts.holdMs : 1200;
  // 80ms: covers a single >100ms hub-pacing straggler outright (see this
  // file's header, "jitter regression #4") without falling into the
  // hold-then-snap branch below.
  const EXTRAPOLATE_MS = opts.extrapolateMs != null ? opts.extrapolateMs : 80;

  const bufT = new Float64Array(CAP);
  const bufV = new Float64Array(CAP);
  // Per-sample INCOMING velocity (value units/ms, from the previous real
  // sample to this one), stored at push time so sampleAt() can Hermite-
  // interpolate rather than linearly interpolate. NaN means "no prior
  // sample" (the first push ever, or the first after reset()) — sampleAt()
  // must fall back to linear for any span touching such an entry. Reusing
  // the SAME stored value as both a span's outgoing tangent (v1) and the
  // next span's incoming tangent (v0) is what makes the curve velocity-
  // continuous across sample boundaries: it is one number read twice, not
  // two independent estimates that happen to agree.
  const bufVel = new Float64Array(CAP);
  let head = 0;
  let len = 0;
  let lastVelPerMs = 0;      // value units per ms, from the last two real samples
  let lastPushTs = 0;

  /** Push one ground-truth sample. Out-of-order/duplicate timestamps are ignored. */
  function push(value, tsMs) {
    if (value == null || !isFinite(value) || !isFinite(tsMs)) return;
    const newestIdx = len ? (head + len - 1) % CAP : -1;
    if (newestIdx >= 0 && tsMs <= bufT[newestIdx]) return; // not newer: drop

    const idx = (head + len) % CAP;
    if (len === CAP) { head = (head + 1) % CAP; len--; }
    bufT[idx] = tsMs;
    bufV[idx] = value;
    len++;

    if (newestIdx >= 0) {
      const dt = tsMs - bufT[newestIdx];
      bufVel[idx] = dt > 0 ? (value - bufV[newestIdx]) / dt : NaN;
      if (dt > 0) lastVelPerMs = bufVel[idx];
    } else {
      bufVel[idx] = NaN; // no prior sample to derive a tangent from
    }
    lastPushTs = tsMs;
  }

  /**
   * Interpolate/extrapolate the display value at client time `tMs`.
   * @returns {{value: number|null, velPerMs: number, fresh: boolean, extrapolating: boolean}}
   */
  function sampleAt(tMs) {
    if (len === 0) return { value: null, velPerMs: 0, fresh: false, extrapolating: false };

    const fresh = (tMs - lastPushTs) < HOLD_MS;
    const newestIdx = (head + len - 1) % CAP;
    const newestT = bufT[newestIdx];

    if (len === 1 || tMs <= bufT[head]) {
      return { value: bufV[head], velPerMs: 0, fresh, extrapolating: false };
    }

    if (tMs >= newestT) {
      const pastMs = tMs - newestT;
      if (pastMs <= EXTRAPOLATE_MS) {
        return { value: bufV[newestIdx] + lastVelPerMs * pastMs, velPerMs: lastVelPerMs, fresh, extrapolating: true };
      }
      return { value: bufV[newestIdx], velPerMs: 0, fresh, extrapolating: false };
    }

    // Linear scan for the bracketing pair. The ring is small (a couple hundred
    // entries at most) and this runs once per rendered frame, not per sample.
    let lo = head;
    let hi = (head + 1) % CAP;
    for (let i = 1; i < len; i++) {
      if (bufT[hi] > tMs) break;
      lo = hi;
      hi = (hi + 1) % CAP;
    }
    const span = bufT[hi] - bufT[lo];
    const frac = span > 0 ? Math.min(1, Math.max(0, (tMs - bufT[lo]) / span)) : 1;
    const p0 = bufV[lo];
    const p1 = bufV[hi];
    const v0 = bufVel[lo];
    const v1 = bufVel[hi];

    let value;
    if (span > 0 && isFinite(v0) && isFinite(v1)) {
      // Cubic Hermite, not linear: linear interp puts a slope kink at every
      // ~40ms sample boundary — measured as the rail marker's visible
      // jitter. Tangents are scaled by `span` (m = v*span) so the curve's
      // real-time derivative at each endpoint equals the stored per-ms
      // velocity exactly, independent of how long this particular span is —
      // that scaling is what keeps the derivative continuous across the
      // shared boundary with the NEXT span (see bufVel's comment above).
      const t = frac;
      const t2 = t * t;
      const t3 = t2 * t;
      const h00 = 2 * t3 - 3 * t2 + 1;
      const h10 = t3 - 2 * t2 + t;
      const h01 = -2 * t3 + 3 * t2;
      const h11 = t3 - t2;
      const m0 = v0 * span;
      const m1 = v1 * span;
      const raw = h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
      // Hermite can legitimately overshoot p0/p1 a little on curved motion,
      // but a bad velocity estimate must never fling the marker off-screen:
      // clamp to the span's range plus a fixed fraction of headroom.
      const overshoot = 0.15 * Math.abs(p1 - p0);
      const clampLo = Math.min(p0, p1) - overshoot;
      const clampHi = Math.max(p0, p1) + overshoot;
      value = Math.min(clampHi, Math.max(clampLo, raw));
    } else {
      // No stored tangent on one side (first sample in the buffer's life,
      // or a degenerate zero-length span): fall back to linear.
      value = p0 + (p1 - p0) * frac;
    }
    return { value, velPerMs: lastVelPerMs, fresh, extrapolating: false };
  }

  /** Reset to empty — call when the bound field's channel/uid changes identity. */
  function reset() {
    head = 0; len = 0; lastVelPerMs = 0; lastPushTs = 0;
  }

  return { push, sampleAt, reset, get length() { return len; } };
}

/**
 * A fixed-size ring of RENDERED (already-interpolated) points, for painting a
 * fading trail. Distinct from `createTelebuf`: this records what the display
 * actually drew each frame (deduped so idle jitter doesn't spam the ring), not
 * raw ground-truth samples — exactly what the original comet trail did.
 */
export function createTrail(opts = {}) {
  const CAP = opts.capacity || 320;
  const bufX = new Float64Array(CAP);
  const bufT = new Float64Array(CAP);
  let head = 0;
  let len = 0;

  function record(x, tMs, opts2 = {}) {
    const minDx = opts2.minDx != null ? opts2.minDx : 0.35;
    const minDtMs = opts2.minDtMs != null ? opts2.minDtMs : 8;
    if (len === 0) {
      bufX[0] = x; bufT[0] = tMs; head = 0; len = 1;
      return;
    }
    if ((tMs - bufT[head]) >= minDtMs || Math.abs(x - bufX[head]) >= minDx) {
      head = (head + 1) % CAP;
      bufX[head] = x; bufT[head] = tMs;
      if (len < CAP) len++;
    } else {
      bufX[head] = x; bufT[head] = tMs;
    }
  }

  /** Iterate newest-to-oldest points younger than `maxAgeMs`. */
  function forEachRecent(nowMs, maxAgeMs, fn) {
    let idx = head;
    for (let i = 0; i < len; i++) {
      const age = nowMs - bufT[idx];
      if (age > maxAgeMs) break;
      fn(bufX[idx], bufT[idx], age);
      idx = (idx - 1 + CAP) % CAP;
    }
  }

  function reset() { head = 0; len = 0; }

  return { record, forEachRecent, reset, get length() { return len; } };
}

/**
 * A single render-delay clock, SHARED across every telebuf that must agree
 * about "now" (position/velocity/target — see RailWidget.svelte, which reads
 * all three at the one instant this produces so the phosphor dot, the tape
 * cursor and the hero numerals never disagree). See this file's header for
 * why the delay exists at all.
 *
 * Usage: call `noteArrival(tsMs)` every time ANY real sample lands (the
 * timestamp the sample itself carries, e.g. `machine.sampleTs[channelId]`),
 * call `update(frameDtMs)` once per rendered frame, then read `renderTime(t)`
 * (or `stableRenderTime(t)`, which additionally guarantees the return value
 * never rewinds by more than one frame — a delay that grows because arrivals
 * just got sparser must not make the display visibly jump backward).
 *
 * @param {{minDelayMs?: number, maxDelayMs?: number, slewMsPerFrame?: number, gapCapacity?: number}} [opts]
 */
export function createRenderClock(opts = {}) {
  const MIN_DELAY_MS = opts.minDelayMs != null ? opts.minDelayMs : 20;
  const MAX_DELAY_MS = opts.maxDelayMs != null ? opts.maxDelayMs : 120;
  // 4ms/frame: acquires a safe delay within ~1s of a hub-pacing burst
  // starting (see this file's header, "jitter regression #4") instead of
  // chasing it for the burst's whole duration.
  const SLEW_MS_PER_FRAME = opts.slewMsPerFrame != null ? opts.slewMsPerFrame : 4;
  const GAP_CAP = opts.gapCapacity || 32; // ~1s of history at ~30Hz — plenty for a p95

  const gaps = new Float64Array(GAP_CAP);
  let gapHead = 0, gapLen = 0;
  let lastArrivalTs = 0;

  let delayMs = (MIN_DELAY_MS + MAX_DELAY_MS) / 2; // sane mid-point before any data
  let frameDtMs = 16.667;
  let lastRenderT = 0;

  /** Record one real sample's arrival timestamp (same epoch-ms domain as sampleAt). */
  function noteArrival(tsMs) {
    if (lastArrivalTs > 0) {
      const gap = tsMs - lastArrivalTs;
      if (gap > 0 && gap < 5000) { // ignore resets/backward jumps/huge stalls
        gaps[gapHead] = gap;
        gapHead = (gapHead + 1) % GAP_CAP;
        if (gapLen < GAP_CAP) gapLen++;
      }
    }
    lastArrivalTs = tsMs;
  }

  function p95Gap() {
    if (gapLen === 0) return MIN_DELAY_MS;
    const sorted = Array.prototype.slice.call(gaps, 0, gapLen).sort((a, b) => a - b);
    const idx = Math.min(sorted.length - 1, Math.floor(sorted.length * 0.95));
    return sorted[idx];
  }

  /** Recompute the target delay from the measured arrival cadence and slew toward it. Call once per rendered frame. */
  function update(dtMs) {
    if (dtMs > 0 && dtMs < 200) frameDtMs = dtMs;
    const target = Math.max(MIN_DELAY_MS, Math.min(MAX_DELAY_MS, p95Gap() * 1.5 + frameDtMs));
    const delta = target - delayMs;
    const clamped = Math.max(-SLEW_MS_PER_FRAME, Math.min(SLEW_MS_PER_FRAME, delta));
    delayMs += clamped;
  }

  function getDelayMs() { return delayMs; }

  /** t_render = t - delay. */
  function renderTime(tMs) { return tMs - delayMs; }

  /** Same, but never rewinds more than one frame versus the last call — avoids a visible backward jump when the delay grows. */
  function stableRenderTime(tMs) {
    let t = renderTime(tMs);
    const maxRewind = frameDtMs;
    if (lastRenderT > 0 && t < lastRenderT - maxRewind) t = lastRenderT - maxRewind;
    lastRenderT = t;
    return t;
  }

  function reset() {
    gapHead = 0; gapLen = 0; lastArrivalTs = 0; lastRenderT = 0;
    delayMs = (MIN_DELAY_MS + MAX_DELAY_MS) / 2;
  }

  return { noteArrival, update, getDelayMs, renderTime, stableRenderTime, reset };
}
