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
  const EXTRAPOLATE_MS = opts.extrapolateMs != null ? opts.extrapolateMs : 50;

  const bufT = new Float64Array(CAP);
  const bufV = new Float64Array(CAP);
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
      if (dt > 0) lastVelPerMs = (value - bufV[newestIdx]) / dt;
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
    const value = bufV[lo] + (bufV[hi] - bufV[lo]) * frac;
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
