/**
 * telebuf.js — time-indexed telemetry buffer.
 *
 * Preallocated ring of ~1.5s of samples. No per-frame allocation.
 * Fed by WS 0x01 frames (primary) or HTTP batch polls (fallback).
 *
 * On 0x01 arrival, 10um samples are expanded to client-time via the current
 * clock offset. API: sampleAt(t) linear-interpolates between bracketing
 * samples; extrapolation uses the last segment's velocity.
 *
 * Render clock: t_render = nowSynced - delay, where
 *   delay = clamp(p95_jitter * 1.5 + frame_dt, 20ms, 120ms),
 * adapted slowly (slew <= 2ms per frame — no visible warp).
 *
 * Gap policy: past newest sample — extrapolate along last velocity for up to
 * 50ms, then hold and fire stale escalation events.
 */

import { setVV, setVVState } from './ui.js';

// ---- Configuration ---------------------------------------------------------
var BUFFER_DURATION_MS = 1500;   // ring capacity in client-time
var DT_US = 4200;                // default 0x01 sample spacing in µs (240Hz → 4.2ms dt_100us=42)
var MIN_RENDER_DELAY_MS = 20;
var MAX_RENDER_DELAY_MS = 120;
var EXTRAPOLATE_HOLD_MS = 50;    // how long we extrapolate past the newest sample
var DELAY_SLEW_LIMIT_MS = 2;     // max change per frame to render delay
var MAX_FRAME_GAP_MS = 500;      // gap larger than this triggers a fresh-snap

// ---- Preallocated ring buffers (Float64Array for time + 3 channels) ---------
// Cap computed on first init() based on BUFFER_DURATION_MS / sample spacing.
// Default: 1500ms at 4.2ms = ~357 slots → allocate 512 for safety.
var CAP = 512;
var bufT = new Float64Array(CAP);    // client-time timestamps (ms, performance.now)
var bufPos = new Float64Array(CAP);  // actual position (mm)
var bufTgt = new Float64Array(CAP);  // commanded target (mm)
var bufRaw = new Float64Array(CAP);  // raw demand (mm)
var bufHead = 0;   // write index (oldest valid sample)
var bufLen = 0;    // number of valid samples currently in buffer
var bufSeq = 0;    // rotating internal insert counter

// ---- Clock sync state (owned by link.js, read here) ------------------------
var _clockOffsetUs = 0;     // µs: device_time − client_time (from EWMA)
var _p95JitterUs = 500;     // µs: p95 clock jitter
var _synced = false;        // true once we have at least one valid sync sample

// ---- Render clock state ----------------------------------------------------
var _renderDelayMs = 30;    // current delay target (smoothly adapting)
var _frameDtMs = 16.667;    // smoothed frame delta

// ---- Staleness -------------------------------------------------------------
var _lastSampleTs = 0;      // client-time (ms) of the newest sample inserted
var _lastLinkTs = 0;        // client-time (ms) of last proof the WS link is alive
var _staleFired = false;
var _staleSuspended = false;
var _teleFallback = false;  // HTTP fallback mode: staleness thresholds ×5
var STALE_MS = 150;
var SUSPEND_MS = 1000;
var STALE_FB_MS = 750;
var SUSPEND_FB_MS = 5000;

// ---- Velocity of last segment (for extrapolation) ---------------------------
var _lastVel = 0;           // mm/ms (position velocity)
var _lastTgtVel = 0;        // mm/ms (target velocity)

// ---- Event emitters ---------------------------------------------------------
var _onStale = null;        // called when data goes stale (>150ms)
var _onStaleSuspended = null; // called when suspended (>1s)
var _onFresh = null;        // called when fresh data resumes after stale/suspended

// ---- Callbacks for hero numerals (set by main.js) ---------------------------
var _heroActualCb = null;
var _heroCmdCb = null;
var _heroLagCb = null;

// ---- Cached result object for sampleAt (reused, no per-frame alloc) ---------
var _sampleAtResult = { pos: 0, tgt: 0, raw: 0, lag: 0, extrapolating: false, stale: false };

// ============================================================================
// Initialization
// ============================================================================

export function initTeleBuf(opts) {
  if (opts && typeof opts.bufferDurationMs === 'number') BUFFER_DURATION_MS = opts.bufferDurationMs;
  if (opts && typeof opts.sampleDtUs === 'number') DT_US = opts.sampleDtUs;

  // Compute capacity: buffer duration / sample spacing, round up + 20% margin
  var expected = Math.ceil(BUFFER_DURATION_MS / (DT_US / 1000)) * 1.2;
  CAP = Math.max(512, Math.ceil(expected));

  bufT = new Float64Array(CAP);
  bufPos = new Float64Array(CAP);
  bufTgt = new Float64Array(CAP);
  bufRaw = new Float64Array(CAP);
  bufHead = 0;
  bufLen = 0;
  bufSeq = 0;
  _lastSampleTs = 0;
  _lastLinkTs = 0;
  _lastVel = 0;
  _lastTgtVel = 0;
  _staleFired = false;
  _staleSuspended = false;
}

// ============================================================================
// Clock sync injection (called from link.js)
// ============================================================================

export function setClockState(offsetUs, p95JitterUs, synced) {
  _clockOffsetUs = offsetUs;
  _p95JitterUs = p95JitterUs;
  _synced = synced;
}

export function getClockState() {
  return { offsetUs: _clockOffsetUs, p95JitterUs: _p95JitterUs, synced: _synced };
}

// ---- Stale event callbacks --------------------------------------------------
export function onStale(cb) { _onStale = cb; }
export function onStaleSuspended(cb) { _onStaleSuspended = cb; }
export function onFresh(cb) { _onFresh = cb; }

/**
 * Proof the WS link is alive, independent of motion telemetry. The firmware
 * only emits 0x01 TELEMETRY frames while the ring is advancing (i.e. while the
 * machine is actually moving); an idle-but-connected rig produces none. STATUS
 * (0x02) heartbeats, however, arrive ~every 500ms regardless. Feeding those
 * here keeps the >1s control-suspension gate from tripping on a healthy link.
 * @param {number} [nowMs] performance.now() at receive (defaults to now)
 */
export function noteLinkAlive(nowMs) {
  _lastLinkTs = (typeof nowMs === 'number')
    ? nowMs
    : (typeof performance !== 'undefined' ? performance.now() : Date.now());
  // A live-link proof must release a suspension raised purely because motion
  // telemetry went idle. (Visual 'stale' stays owned by motion-sample age.)
  if (_staleSuspended) {
    _staleSuspended = false;
    if (_onFresh) _onFresh();
  }
}

/**
 * Set fallback mode: staleness thresholds are ×5 in HTTP polling mode
 * (samples arrive per-poll, not per-frame). Called by main.js on
 * degraded/restored transitions.
 */
export function setTeleFallback(on) {
  var wasFallback = _teleFallback;
  _teleFallback = !!on;
  // When switching from fallback → live WS, reset the staleness clock so
  // WS has a fresh grace period to deliver its first 0x01 frame before
  // stale (150ms) and suspended (1000ms) fire. Without this reset, the
  // last HTTP poll sample's age races against the tightened thresholds.
  if (wasFallback && !_teleFallback) {
    _lastSampleTs = typeof performance !== 'undefined' ? performance.now() : Date.now();
    _lastLinkTs = _lastSampleTs;
    if (_staleFired || _staleSuspended) {
      _staleFired = false;
      _staleSuspended = false;
      if (_onFresh) _onFresh();
    }
  }
}

// ---- Hero numeral callbacks -------------------------------------------------
export function setHeroCallbacks(actualCb, cmdCb, lagCb) {
  _heroActualCb = actualCb;
  _heroCmdCb = cmdCb;
  _heroLagCb = lagCb;
}

// ============================================================================
// Feed: insert samples from 0x01 frame OR HTTP batch
// ============================================================================

/**
 * Feed samples from a parsed 0x01 TELEMETRY frame.
 * Each sample: {pos_10um, tgt_10um, raw_10um}
 * The frame carries t_dev_us (device clock of LAST sample) and dt_100us.
 * Samples are ordered oldest→newest in the frame.
 *
 * @param {object} parsed — result from wire.parseTelemetry()
 * @param {number} nowMs — performance.now() at receive time
 */
export function feedWireSamples(parsed, nowMs) {
  if (!parsed || !parsed.samples || parsed.samples.length === 0) return;
  if (typeof parsed.dt_100us === 'number' && parsed.dt_100us > 0) DT_US = parsed.dt_100us * 100;

  var n = parsed.samples.length;
  // t_dev_us is the device clock of the LAST sample. Walk backward to get each
  // sample's device time, then convert to client time via offset.
  var lastDevUs = parsed.t_dev_us;
  var offsetUs = _clockOffsetUs;
  var spacingUs = DT_US;

  // The first sample in the frame is (n-1)*spacing before the last
  for (var i = 0; i < n; i++) {
    var idx = (n - 1) - i; // process from oldest to newest
    var devUs = (lastDevUs - idx * spacingUs) >>> 0; // u32 wrap-safe subtraction
    var clientUs = (devUs - offsetUs) >>> 0;         // device→client conversion
    var tsMs = clientUs / 1000;                       // µs → ms
    // If we have a sync reference, anchor: the newest sample should be near nowMs.
    // Without sync, use nowMs as anchor for the newest sample.
    if (i === 0) {
      // Newest sample: shift so it lands at nowMs - render delay
      var anchorShift = (nowMs - _renderDelayMs) - tsMs;
      if (_synced) {
        // With sync, limit anchor shift to avoid large jumps
        if (Math.abs(anchorShift) > 500) anchorShift = Math.sign(anchorShift) * 500;
      }
    }
    if (i === 0 && !_synced) {
      // No sync yet: place newest sample at nowMs
      tsMs = nowMs;
    }

    var s = parsed.samples[i];
    var posMm = s.pos_10um / 100.0;
    var tgtMm = s.tgt_10um / 100.0;
    var rawMm = s.raw_10um / 100.0;

    pushSample(tsMs, posMm, tgtMm, rawMm);
  }

  _lastSampleTs = nowMs;
  _lastLinkTs = nowMs;

  // Reset staleness on fresh data
  if (_staleFired || _staleSuspended) {
    _staleFired = false;
    _staleSuspended = false;
    if (_onFresh) _onFresh();
  }
}

/**
 * Feed samples from HTTP poll batch (fallback mode).
 * Samples are [[pos_mm, tgt_mm], ...] already in mm, with a fixed dtMs spacing.
 *
 * @param {Array<Array<number>>} rawSamples
 * @param {number} dtMs — spacing between samples
 * @param {number} nowMs — performance.now() at receive
 */
export function feedHttpSamples(rawSamples, dtMs, nowMs) {
  if (!rawSamples || rawSamples.length === 0) return;
  var n = rawSamples.length;
  var spacingMs = dtMs || 10;

  // Anchor: newest sample at nowMs, older ones backward
  var newestTs = nowMs;
  for (var i = n - 1; i >= 0; i--) {
    var tsMs = newestTs - (n - 1 - i) * spacingMs;
    var s = rawSamples[i];
    var posMm = s[0] || 0;
    var tgtMm = s.length > 1 ? s[1] : posMm;
    var rawMm = s.length > 2 ? s[2] : tgtMm;
    pushSample(tsMs, posMm, tgtMm, rawMm);
  }

  _lastSampleTs = nowMs;
  _lastLinkTs = nowMs;

  if (_staleFired || _staleSuspended) {
    _staleFired = false;
    _staleSuspended = false;
    if (_onFresh) _onFresh();
  }
}

// ---- Internal ring push ----------------------------------------------------
function pushSample(tsMs, posMm, tgtMm, rawMm) {
  var idx = bufHead + bufLen;
  if (idx >= CAP) idx -= CAP; // wrap
  if (bufLen === CAP) {
    // Ring is full — advance head (drop oldest)
    bufHead = (bufHead + 1) % CAP;
    bufLen--;
  }

  bufT[idx] = tsMs;
  bufPos[idx] = posMm;
  bufTgt[idx] = tgtMm;
  bufRaw[idx] = rawMm;
  bufLen++;

  // Update velocity from last two samples
  if (bufLen >= 2) {
    var prevIdx = (idx - 1 + CAP) % CAP;
    var dtMs = tsMs - bufT[prevIdx];
    if (dtMs > 0) {
      _lastVel = (posMm - bufPos[prevIdx]) / dtMs;
      _lastTgtVel = (tgtMm - bufTgt[prevIdx]) / dtMs;
    }
  }
}

// ============================================================================
// Render clock
// ============================================================================

/**
 * Update render delay from current jitter estimate. Call once per frame.
 * @param {number} frameDtMs — elapsed ms since last frame
 */
export function updateRenderDelay(frameDtMs) {
  if (frameDtMs > 0 && frameDtMs < 200) _frameDtMs = frameDtMs;

  // Target delay: p95 jitter * 1.5 + frame time, clamped
  var jitterMs = _p95JitterUs / 1000;
  var target = jitterMs * 1.5 + _frameDtMs;
  target = Math.max(MIN_RENDER_DELAY_MS, Math.min(MAX_RENDER_DELAY_MS, target));

  // Slew limit
  var delta = target - _renderDelayMs;
  if (delta > DELAY_SLEW_LIMIT_MS) delta = DELAY_SLEW_LIMIT_MS;
  else if (delta < -DELAY_SLEW_LIMIT_MS) delta = -DELAY_SLEW_LIMIT_MS;
  _renderDelayMs += delta;
}

/**
 * Get the render time for the current frame: nowSynced − delay.
 * @param {number} nowSyncedMs — clock-synced "now" (performance.now + offset)
 * @returns {number} t_render in ms (may be in the past)
 */
export function getRenderTime(nowSyncedMs) {
  return nowSyncedMs - _renderDelayMs;
}

// ============================================================================
// Interpolation: sampleAt(t)
// ============================================================================

/**
 * Sample the buffer at a given client-time t (ms). Linear interpolation between
 * bracketing samples. Past the newest sample: extrapolate along last velocity
 * for up to EXTRAPOLATE_HOLD_MS, then hold.
 *
 * @param {number} tMs — client-time to sample at (ms)
 * @returns {{pos:number, tgt:number, raw:number, lag:number, extrapolating:boolean, stale:boolean}}
 */
export function sampleAt(tMs) {
  var res = _sampleAtResult;
  res.extrapolating = false;
  res.stale = false;

  if (bufLen === 0) {
    res.pos = 0;
    res.tgt = 0;
    res.raw = 0;
    res.lag = 0;
    res.stale = true;
    return res;
  }

  // Find bracketing samples in the ring
  var newestIdx = (bufHead + bufLen - 1 + CAP) % CAP;
  var newestTs = bufT[newestIdx];

  // Check staleness — time since newest sample > 150ms
  var nowMs = typeof performance !== 'undefined' ? performance.now() : Date.now();
  var ageMs = nowMs - _lastSampleTs;      // motion-telemetry age → visual staleness
  // Link-liveness age → hard control suspension. STATUS heartbeats (~500ms)
  // keep _lastLinkTs fresh even when the machine is idle and emitting no 0x01
  // motion frames, so an idle-but-connected rig never locks its own controls.
  // Until the link has proven alive at least once (_lastLinkTs===0) we fall
  // back to sample age so HTTP-fallback staleness still escalates on boot.
  var linkRef = _lastLinkTs > 0 ? _lastLinkTs : _lastSampleTs;
  var linkAgeMs = nowMs - linkRef;
  var staleThresh = _teleFallback ? STALE_FB_MS : STALE_MS;
  var suspendThresh = _teleFallback ? SUSPEND_FB_MS : SUSPEND_MS;
  if (ageMs > staleThresh && !_staleFired) {
    _staleFired = true;
    if (_onStale) _onStale();
  }
  if (linkAgeMs > suspendThresh && !_staleSuspended) {
    _staleSuspended = true;
    if (_onStaleSuspended) _onStaleSuspended();
  }
  res.stale = _staleFired;

  // Fast path: before first sample — return it directly
  if (bufLen === 1 || tMs <= bufT[bufHead]) {
    res.pos = bufPos[bufHead];
    res.tgt = bufTgt[bufHead];
    res.raw = bufRaw[bufHead];
    res.lag = Math.abs(res.pos - res.tgt);
    return res;
  }

  // Fast path: at or past newest sample — extrapolate or hold
  if (tMs >= newestTs) {
    var pastMs = tMs - newestTs;
    if (pastMs <= EXTRAPOLATE_HOLD_MS) {
      // Extrapolate
      res.pos = bufPos[newestIdx] + _lastVel * pastMs;
      res.tgt = bufTgt[newestIdx] + _lastTgtVel * pastMs;
      res.raw = bufRaw[newestIdx];
      res.extrapolating = true;
      res.lag = Math.abs(res.pos - res.tgt);
      return res;
    } else {
      // Hold at newest
      res.pos = bufPos[newestIdx];
      res.tgt = bufTgt[newestIdx];
      res.raw = bufRaw[newestIdx];
      res.lag = Math.abs(res.pos - res.tgt);
      return res;
    }
  }

  // Find bracket: first sample with ts > tMs
  // Simple linear scan from head — max ~512 entries, fast enough for rAF
  var prevIdx = bufHead;
  var nextIdx = (bufHead + 1 + CAP) % CAP;
  for (var scanned = 1; scanned < bufLen; scanned++) {
    if (bufT[nextIdx] > tMs) break;
    prevIdx = nextIdx;
    nextIdx = (nextIdx + 1 + CAP) % CAP;
  }

  // At this point: bufT[prevIdx] <= tMs < bufT[nextIdx] (or next is newest)
  var span = bufT[nextIdx] - bufT[prevIdx];
  if (span <= 0 || nextIdx === prevIdx) {
    // Degenerate — just use prev
    res.pos = bufPos[prevIdx];
    res.tgt = bufTgt[prevIdx];
    res.raw = bufRaw[prevIdx];
    res.lag = Math.abs(res.pos - res.tgt);
    return res;
  }

  var frac = (tMs - bufT[prevIdx]) / span;
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;

  res.pos = bufPos[prevIdx] + (bufPos[nextIdx] - bufPos[prevIdx]) * frac;
  res.tgt = bufTgt[prevIdx] + (bufTgt[nextIdx] - bufTgt[prevIdx]) * frac;
  res.raw = bufRaw[prevIdx] + (bufRaw[nextIdx] - bufRaw[prevIdx]) * frac;
  res.lag = Math.abs(res.pos - res.tgt);
  return res;
}

// ============================================================================
// Health / diagnostics exports
// ============================================================================

export function getBufferStats() {
  return {
    cap: CAP,
    len: bufLen,
    lastSampleTs: _lastSampleTs,
    renderDelayMs: _renderDelayMs,
    stale: _staleFired,
    suspended: _staleSuspended,
    lastVel: _lastVel,
    lastTgtVel: _lastTgtVel
  };
}

/**
 * Check if the buffer has been suspended (controls disabled).
 */
export function isSuspended() { return _staleSuspended; }

// ============================================================================
// Rewind safety — never rewind more than one frame
// ============================================================================
var _lastRenderT = 0;

/**
 * Get a render-safe time that never rewinds more than one frame.
 * @param {number} nowSyncedMs — clock-synced "now"
 * @returns {number} render time to use
 */
export function stableRenderTime(nowSyncedMs) {
  var t = getRenderTime(nowSyncedMs);
  var maxRewind = _frameDtMs;
  if (_lastRenderT > 0 && t < _lastRenderT - maxRewind) {
    t = _lastRenderT - maxRewind;
  }
  _lastRenderT = t;
  return t;
}