/**
 * Plan strip — canvas diagnostic lane under the rail showing the in-flight
 * streamed segment (DELTA D8). Replaces the old green/amber line the v0.4
 * interpolator overlay used to draw directly on the rail canvas.
 *
 * Geometry: the lane's x-axis is the SAME mm->px mapping as the rail
 * (mm / TRAVEL * width) so segment endpoints land pixel-aligned under the
 * rail markers above them.
 *
 * The sweep head is NOT linear elapsed-time — it's the interpolated
 * commanded ("told") position from telebuf's render clock, clamped into the
 * current segment, so the head's motion visibly carries the plan's real
 * velocity profile (ease-in/cruise/ease-out).
 *
 * Segment endpoints come from window.__INTERP (the 0x04 INTERP frame main.js
 * already parses at ~45Hz) — read via the global the same way rail.js used
 * to, not an ES import, since this module and main.js would otherwise cycle.
 * Displayed endpoints are eased 30%/frame toward the latest segment so a
 * retarget GLIDES instead of snapping.
 */
import { $, clamp, pad } from '../core/ui.js';
import { TRAVEL, winMin, winMax } from '../core/range.js';
import { sampleAt, stableRenderTime } from '../core/telebuf.js';
import { ACCENT, ac } from '../core/theme.js';

var HIDE_DELAY_MS = 1000; // grace period after streaming stops before fading out

var S = {
  wrap: null, cv: null, ctx: null,
  vEl: null, aEl: null, dlEl: null, segEl: null,
  segFromMm: 0, segToMm: 0,   // raw (unsmoothed) current-segment endpoints, mm
  dispFrom: 0, dispTo: 0,     // eased display endpoints, mm
  lastSegKey: '',
  visible: false,
  hideTimer: null,
  lastDurFallbackCount: 0,
  lateLatched: false,
  cssW: 0, cssH: 0,
};

// Segment-history ghosts — the last few completed segments, drawn as dim
// fading bars behind the live one so the stroke RHYTHM is legible at a
// glance (where the stream has been commanding, not just where it is now).
// Fixed ring, no per-frame allocation.
var GHOST_N = 6;
var GHOST_FADE_MS = 1400;
var gFrom = new Float64Array(GHOST_N);
var gTo = new Float64Array(GHOST_N);
var gBorn = new Float64Array(GHOST_N);
var gHead = 0, gLen = 0;

function pushGhost(fromMm, toMm, nowMs) {
  gHead = (gHead + 1) % GHOST_N;
  gFrom[gHead] = fromMm;
  gTo[gHead] = toMm;
  gBorn[gHead] = nowMs;
  if (gLen < GHOST_N) gLen++;
}

export function initPlanStrip() {
  S.wrap = $('#planStrip');
  S.cv = $('#planCv');
  if (!S.wrap || !S.cv) return;
  S.ctx = S.cv.getContext('2d');
  S.vEl = $('#planV');
  S.aEl = $('#planA');
  S.dlEl = $('#planDl');
  S.segEl = $('#planSeg');
  requestAnimationFrame(frame);
}

function sizeCanvasIfNeeded() {
  var w = S.cv.clientWidth, h = S.cv.clientHeight;
  if (w === 0 || h === 0) return false;
  if (w === S.cssW && h === S.cssH) return true;
  var dpr = window.devicePixelRatio || 1;
  S.cv.width = w * dpr;
  S.cv.height = h * dpr;
  S.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  S.cssW = w; S.cssH = h;
  return true;
}

function frame(nowMs) {
  var it = window.__INTERP;
  var live = !!(it && it.active && (performance.now() - it.lastRxMs) < 250);

  if (live) {
    if (S.hideTimer) { clearTimeout(S.hideTimer); S.hideTimer = null; }
    if (!S.visible) { S.visible = true; S.wrap.classList.add('on'); }

    var span = winMax - winMin;
    if (span > 0) {
      var segKey = it.startPos.toFixed(4) + ':' + it.endPos.toFixed(4);
      if (segKey !== S.lastSegKey) {
        // A genuinely new segment began — retire the old one into the ghost
        // ring and reset the late-latch (DurFallback anomalies from a PRIOR
        // segment shouldn't tint this one amber).
        if (S.lastSegKey) pushGhost(S.segFromMm, S.segToMm, performance.now());
        S.lastSegKey = segKey;
        S.segFromMm = winMin + clamp(it.startPos, 0, 1) * span;
        S.segToMm = winMin + clamp(it.endPos, 0, 1) * span;
        S.lateLatched = false;
        if (S.segEl) {
          S.segEl.textContent = pad(S.segFromMm, 3, 0) + ' → ' + pad(S.segToMm, 3, 0);
        }
      }
    }

    // LATE proxy (arbiter plan_late isn't wired yet): latch amber if a
    // DurFallback anomaly (firmware invented this segment's duration because
    // MFP sent no usable I<ms>) landed while THIS segment is active. Clears
    // on the next segment. See ANOMALY_KIND_DESC in core/wire.js.
    var counts = window.__ANOMALY_COUNTS;
    var durFallbackCount = counts ? counts.DurFallback : 0;
    if (durFallbackCount > S.lastDurFallbackCount) S.lateLatched = true;
    S.lastDurFallbackCount = durFallbackCount;

    // Readout chips. v = real |curVel| off the 0x04 frame. a is a first-
    // order estimate (v / segment duration) — the firmware doesn't transmit
    // measured acceleration; replace with the arbiter's derived value once
    // it exists. dl = segment duration (the deadline), also real telemetry.
    var vAbs = Math.abs(it.curVel || 0);
    var durMs = it.durationUs / 1000;
    var aEst = durMs > 0 ? vAbs / (durMs / 1000) : 0;
    if (S.vEl) S.vEl.textContent = 'v ' + pad(Math.min(9999, vAbs), 4, 0);
    if (S.aEl) S.aEl.textContent = 'a ' + pad(Math.min(99999, aEst), 5, 0);
    if (S.dlEl) {
      S.dlEl.textContent = 'dl ' + pad(Math.min(999, durMs), 3, 0);
      S.dlEl.classList.toggle('late', S.lateLatched);
    }
  } else if (S.visible && !S.hideTimer) {
    S.hideTimer = setTimeout(function () {
      S.visible = false;
      S.wrap.classList.remove('on');
      S.lastSegKey = '';
      S.lateLatched = false;
      if (S.vEl) S.vEl.textContent = 'v ----';
      if (S.aEl) S.aEl.textContent = 'a -----';
      if (S.dlEl) { S.dlEl.textContent = 'dl ---'; S.dlEl.classList.remove('late'); }
      if (S.segEl) S.segEl.textContent = '--- → ---';
      gLen = 0;
      S.hideTimer = null;
    }, HIDE_DELAY_MS);
  }

  draw(nowMs);
  requestAnimationFrame(frame);
}

function draw(nowMs) {
  if (!sizeCanvasIfNeeded()) { requestAnimationFrame(frame); return; }
  var w = S.cssW, h = S.cssH;

  // Glide toward the latest segment endpoints (a≈0.3/frame) so retargets at
  // MFP retarget rates read as continuous motion, never a snap.
  S.dispFrom += (S.segFromMm - S.dispFrom) * 0.3;
  S.dispTo += (S.segToMm - S.dispTo) * 0.3;

  S.ctx.clearRect(0, 0, w, h);
  if (!S.visible) return;

  var nowP = performance.now();

  // ---- Spatial context (readability upgrade) ------------------------------
  // The lane shares the rail's x-axis; give the eye landmarks to USE that:
  // faint mm ticks every 50mm (mirroring the rail ruler's majors), keep-out
  // tint outside the live window, and hairline window-bound markers. Now a
  // segment's position on the strip reads spatially, not just relatively.
  S.ctx.fillStyle = 'rgba(51,55,62,0.5)';
  for (var mm = 50; mm < TRAVEL; mm += 50) {
    S.ctx.fillRect(mm / (TRAVEL || 1) * w, 0, 1, h);
  }
  var wLoX = clamp(winMin, 0, TRAVEL) / (TRAVEL || 1) * w;
  var wHiX = clamp(winMax, 0, TRAVEL) / (TRAVEL || 1) * w;
  S.ctx.fillStyle = 'rgba(255,71,87,0.05)';
  if (wLoX > 0) S.ctx.fillRect(0, 0, wLoX, h);
  if (wHiX < w) S.ctx.fillRect(wHiX, 0, w - wHiX, h);
  S.ctx.fillStyle = 'rgba(51,55,62,0.9)';
  S.ctx.fillRect(wLoX, 0, 1, h);
  S.ctx.fillRect(wHiX - 1, 0, 1, h);

  // ---- Segment-history ghosts --------------------------------------------
  // The last few completed segments as thin fading center-bars — the stroke
  // rhythm (reach, direction changes, cadence) stays visible ~1.4s behind
  // the live segment.
  for (var g = 0; g < gLen; g++) {
    var gi = (gHead - g + GHOST_N) % GHOST_N;
    var age = nowP - gBorn[gi];
    if (age > GHOST_FADE_MS) continue;
    var f = 1 - age / GHOST_FADE_MS;
    var ga = clamp(gFrom[gi], 0, TRAVEL) / (TRAVEL || 1) * w;
    var gb = clamp(gTo[gi], 0, TRAVEL) / (TRAVEL || 1) * w;
    var gl = Math.min(ga, gb), gh = Math.max(ga, gb);
    S.ctx.fillStyle = 'rgba(' + ACCENT.intentRgb + ',' + (0.16 * f * f).toFixed(3) + ')';
    S.ctx.fillRect(gl, h * 0.5 - 1.5, Math.max(1, gh - gl), 3);
  }

  var x0 = clamp(S.dispFrom, 0, TRAVEL) / (TRAVEL || 1) * w;
  var x1 = clamp(S.dispTo, 0, TRAVEL) / (TRAVEL || 1) * w;
  var lo = Math.min(x0, x1), hi = Math.max(x0, x1);
  if (hi - lo < 2) { lo -= 1; hi += 1; }

  var late = S.lateLatched;
  var rgb = late ? '245,185,77' : ACCENT.intentRgb;

  var grad = S.ctx.createLinearGradient(x0, 0, x1, 0);
  grad.addColorStop(0, 'rgba(' + rgb + ',0.10)');
  grad.addColorStop(1, 'rgba(' + rgb + ',0.55)');
  S.ctx.fillStyle = grad;
  S.ctx.fillRect(lo, 1, hi - lo, h - 2);

  // Sweep head — the interpolated commanded ("told") position at the render
  // clock, clamped into the segment. THIS is the interpolation made legible:
  // it moves with the plan's real velocity profile, not linear elapsed time.
  var s = sampleAt(stableRenderTime(nowMs));
  var segLo = Math.min(S.segFromMm, S.segToMm), segHi = Math.max(S.segFromMm, S.segToMm);
  var headMm = clamp(s.tgt, segLo, segHi);
  var hx = clamp(headMm, 0, TRAVEL) / (TRAVEL || 1) * w;

  // Swept region + head are PURPLE (bright lavender, clearly separable from
  // the segment's dimmer gradient) — the mock's near-white head vanished
  // against the pale segment fill on the real display, per user feedback.
  var sLo = Math.min(x0, hx), sHi = Math.max(x0, hx);
  S.ctx.fillStyle = ac('i', 0.22);
  S.ctx.fillRect(sLo, 1, Math.max(1, sHi - sLo), h - 2);

  S.ctx.save();
  S.ctx.shadowColor = ACCENT.intentBright;
  S.ctx.shadowBlur = 7;
  S.ctx.fillStyle = ACCENT.intentBright;
  S.ctx.fillRect(hx - 0.75, 0, 1.5, h);
  S.ctx.restore();

  // Glowing caret at TO — gradient follows direction, amber when late —
  // plus an arrowhead pointing the direction of travel, so which way the
  // stroke is going reads instantly even on a short segment.
  S.ctx.save();
  S.ctx.shadowColor = 'rgba(' + rgb + ',0.85)';
  S.ctx.shadowBlur = 7;
  S.ctx.fillStyle = late ? '#F5B94D' : ACCENT.intent;
  S.ctx.fillRect(x1 - 1, -1, 2, h + 2);
  var dir = x1 >= x0 ? 1 : -1;
  var ax = x1 + dir * 2.5;
  S.ctx.beginPath();
  S.ctx.moveTo(ax, h * 0.5 - 4);
  S.ctx.lineTo(ax + dir * 4.5, h * 0.5);
  S.ctx.lineTo(ax, h * 0.5 + 4);
  S.ctx.closePath();
  S.ctx.fill();
  S.ctx.restore();
}
