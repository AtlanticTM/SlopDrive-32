/**
 * Diagnostics graph — full-width strip chart replacing the .main panel area
 * while toggled on (DIAG pill in the desktop tab bar).
 *
 * Traces, all on the same scrolling 10s time axis:
 *   L1 (position lane): ACTUAL (reality) + COMMANDED (intent) + the
 *      INTERPOLATOR's own reported position (bright intent, dashed) — the
 *      three should nest; daylight between them IS the diagnosis. The
 *      actual↔commanded gap is additionally shaded amber so tracking lag is
 *      visible as area, not just as two lines being apart.
 *   L2: LAG (|actual − commanded|, mm) — auto-scaled, amber.
 *   L3: BUS — volts (fixed 0–42V scale, gray) + watts (auto-scaled, bright).
 *
 * Sampling: one push per rAF frame from the SAME interpolated render clock
 * the rail uses (telebuf sampleAt(stableRenderTime)) — so this graph is
 * frame-accurate to what the rail shows, not a decimated side channel. Bus
 * V/A come from a supplier callback registered by main.js (0x01 telemetry
 * mA at ~45Hz — genuinely live at the INA's 40Hz cache rate as of fw 2.1.4 —
 * plus 0x02 bus_mV). Collection runs ALWAYS (cheap: one sample/frame into
 * fixed Float64Array rings) so toggling the panel open shows the last 10s
 * immediately instead of starting cold.
 *
 * Allocation discipline: fixed rings, no per-frame objects; paths are
 * rebuilt per frame (canvas requires it) but from ring reads only.
 */
import { $, clamp, pad } from '../core/ui.js';
import { TRAVEL, winMin, winMax } from '../core/range.js';
import { sampleAt, stableRenderTime } from '../core/telebuf.js';
import { ACCENT, ac } from '../core/theme.js';
import { getStats as linkStats } from '../core/link.js';

var N = 2048;               // ring capacity (~34s at 60fps — > the 10s window)
var WINDOW_MS = 10000;      // visible time span
var GAP_MS = 250;           // break the trace across sampling gaps

var tR = new Float64Array(N);
var posR = new Float64Array(N);
var tgtR = new Float64Array(N);
var itpR = new Float64Array(N);   // interpolator-reported pos (NaN when idle)
var vR = new Float64Array(N);
var wR = new Float64Array(N);
var gapR = new Uint8Array(N);     // 1 = telemetry gap / dropped-frame span at this sample
var head = -1, len = 0;

var S = {
  host: null, cv: null, ctx: null, btn: null,
  on: false,
  cssW: 0, cssH: 0,
  supplier: null,           // () -> {v: volts, a: amps} from main.js
  lastLegendMs: 0,
};

function push(t, pos, tgt, itp, v, w, gap) {
  head = (head + 1) % N;
  tR[head] = t; posR[head] = pos; tgtR[head] = tgt; itpR[head] = itp;
  vR[head] = v; wR[head] = w; gapR[head] = gap ? 1 : 0;
  if (len < N) len++;
}

export function initDiag(supplier) {
  S.supplier = supplier || null;
  S.host = $('#diagHost');
  S.cv = $('#diagCv');
  S.btn = $('#diagToggle');
  if (S.cv) S.ctx = S.cv.getContext('2d');

  if (S.btn) S.btn.addEventListener('click', function () { setDiagOn(!S.on); });

  // Opening Settings/Log while the graph is up would toggle a pane under a
  // hidden .main — close the graph first so the pane action is visible.
  document.querySelectorAll('.tabs-desktop .tab[data-tab]').forEach(function (t) {
    t.addEventListener('click', function () { if (S.on) setDiagOn(false); });
  });

  requestAnimationFrame(frame);
}

function setDiagOn(on) {
  S.on = !!on;
  var mp = document.querySelector('.main-panel');
  if (mp) mp.classList.toggle('diag-on', S.on);
  if (S.btn) S.btn.classList.toggle('active', S.on);
  if (S.on) window.dispatchEvent(new Event('resize'));
}

function frame(nowMs) {
  // ---- Collect (always) ---------------------------------------------------
  var s = sampleAt(stableRenderTime(nowMs));
  var itp = NaN;
  var it = window.__INTERP;
  if (it && it.active && (performance.now() - it.lastRxMs) < 250) {
    var span = winMax - winMin;
    if (span > 0) itp = winMin + clamp(it.curPos, 0, 1) * span;
  }
  var v = 0, a = 0;
  if (S.supplier) { var p = S.supplier(); v = p.v || 0; a = p.a || 0; }

  // Telemetry-gap flag for this sample — SAME connection-trouble signal the
  // header dot uses (link.js seq-gap + heartbeat), so the shading agrees with
  // the dot. NOT plain motion staleness: an idle-but-connected rig emits no
  // 0x01 frames yet must not read as a gap. We shade when the link is actually
  // down, OR the buffer is stale AND the link reported real trouble. :3
  var ls = linkStats();
  var pnow = performance.now();
  var linkDown  = !ls.connected || ls.fallback;
  var recentDrop = ls.lastGapMs > 0 && (pnow - ls.lastGapMs) < 1500;
  var hbStale    = ls.lastStatusMs > 0 && (pnow - ls.lastStatusMs) > 1500;
  var gap = linkDown || (s.stale && (recentDrop || hbStale)) ? 1 : 0;
  push(performance.now(), s.pos, s.tgt, itp, v, Math.abs(v * a), gap);

  // ---- Draw (only while open) --------------------------------------------
  if (S.on) draw(nowMs);
  requestAnimationFrame(frame);
}

function sizeIfNeeded() {
  var w = S.cv.clientWidth, h = S.cv.clientHeight;
  if (w === 0 || h === 0) return false;
  if (w !== S.cssW || h !== S.cssH) {
    var dpr = window.devicePixelRatio || 1;
    S.cv.width = w * dpr;
    S.cv.height = h * dpr;
    S.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    S.cssW = w; S.cssH = h;
  }
  return true;
}

// Stroke one ring series into the current path. yFn maps value -> y.
// Returns the newest value (for the legend). Breaks the line across gaps.
function tracePath(ctx, arr, x0, x1, tNow, yFn) {
  ctx.beginPath();
  var started = false, prevT = 0;
  for (var i = len - 1; i >= 0; i--) {
    var idx = (head - i + N) % N;
    var t = tR[idx];
    var age = tNow - t;
    if (age > WINDOW_MS) continue;
    var val = arr[idx];
    if (isNaN(val)) { started = false; continue; }
    var x = x1 - (age / WINDOW_MS) * (x1 - x0);
    var y = yFn(val);
    if (!started || (t - prevT) > GAP_MS) { ctx.moveTo(x, y); started = true; }
    else ctx.lineTo(x, y);
    prevT = t;
  }
  ctx.stroke();
}

var LANE_GAP = 14;

function draw(nowMs) {
  if (!S.ctx || !sizeIfNeeded()) return;
  var ctx = S.ctx;
  var W = S.cssW, H = S.cssH;
  var tNow = performance.now();
  var padL = 38, padR = 10, padT = 8, padB = 16;
  var x0 = padL, x1 = W - padR;
  var plotH = H - padT - padB - LANE_GAP * 2;
  var l1H = plotH * 0.52, l2H = plotH * 0.18, l3H = plotH * 0.30;
  var l1Y = padT, l2Y = l1Y + l1H + LANE_GAP, l3Y = l2Y + l2H + LANE_GAP;

  ctx.clearRect(0, 0, W, H);

  // ---- Scales -------------------------------------------------------------
  var lagMax = 5, wMax = 50;
  for (var i = len - 1; i >= 0; i--) {
    var idx = (head - i + N) % N;
    if (tNow - tR[idx] > WINDOW_MS) continue;
    var lg = Math.abs(posR[idx] - tgtR[idx]);
    if (lg > lagMax) lagMax = lg;
    if (wR[idx] > wMax) wMax = wR[idx];
  }
  lagMax *= 1.15; wMax *= 1.15;

  var y1 = function (mm) { return l1Y + (1 - clamp(mm, 0, TRAVEL) / (TRAVEL || 1)) * l1H; };
  var yLag = function (mm) { return l2Y + (1 - clamp(mm, 0, lagMax) / lagMax) * l2H; };
  var yV = function (v) { return l3Y + (1 - clamp(v, 0, 42) / 42) * l3H; };
  var yW = function (w) { return l3Y + (1 - clamp(w, 0, wMax) / wMax) * l3H; };

  // ---- Grid ---------------------------------------------------------------
  ctx.lineWidth = 1;
  ctx.strokeStyle = 'rgba(29,32,38,0.9)';
  ctx.font = '9px "Martian Mono", monospace';
  ctx.fillStyle = '#4D525C';
  // vertical time ticks every 1s
  for (var sBack = 0; sBack <= 10; sBack++) {
    var x = x1 - (sBack * 1000 / WINDOW_MS) * (x1 - x0);
    ctx.beginPath(); ctx.moveTo(x, padT); ctx.lineTo(x, H - padB); ctx.stroke();
    if (sBack % 2 === 0) ctx.fillText('-' + sBack + 's', x - 8, H - 5);
  }
  // lane frames + y labels
  [[l1Y, l1H], [l2Y, l2H], [l3Y, l3H]].forEach(function (ln) {
    ctx.strokeStyle = 'rgba(39,43,49,0.9)';
    ctx.strokeRect(x0, ln[0], x1 - x0, ln[1]);
  });
  ctx.fillStyle = '#4D525C';
  ctx.fillText(pad(TRAVEL, 3, 0), 8, l1Y + 8);
  ctx.fillText('000', 8, l1Y + l1H);
  ctx.fillText(pad(lagMax, 2, 0) + 'mm', 4, l2Y + 8);
  ctx.fillText('42V', 8, l3Y + 8);
  ctx.fillText(pad(wMax, 3, 0) + 'W', 4, l3Y + 8 + 10);
  // window bounds on the position lane (context: where the band is)
  ctx.strokeStyle = 'rgba(51,55,62,0.8)';
  ctx.setLineDash([2, 3]);
  [winMin, winMax].forEach(function (b) {
    var y = y1(b);
    ctx.beginPath(); ctx.moveTo(x0, y); ctx.lineTo(x1, y); ctx.stroke();
  });
  ctx.setLineDash([]);

  // ---- Hazard shading: telemetry-gap / dropped-frame spans ----------------
  // Full-height yellow-hazard bands over contiguous spans where the connection
  // gapped (same signal as the header telemetry dot). Drawn UNDER the traces so
  // the held/extrapolated line stays readable on top. Iterates oldest→newest so
  // bandStartX is the left (older) edge and bandEndX the right (newer) edge. :3
  ctx.fillStyle = 'rgba(245,185,77,0.16)';
  ctx.strokeStyle = 'rgba(245,185,77,0.55)';
  ctx.lineWidth = 1;
  var bandY = padT, bandH = H - padT - padB;
  var bandStartX = null, bandEndX = 0;
  var flushBand = function () {
    if (bandStartX === null) return;
    var w = Math.max(bandEndX - bandStartX, 2);
    ctx.fillRect(bandStartX, bandY, w, bandH);
    ctx.beginPath(); ctx.moveTo(bandStartX, bandY); ctx.lineTo(bandStartX + w, bandY); ctx.stroke();
    bandStartX = null;
  };
  for (var gi = len - 1; gi >= 0; gi--) {
    var gidx = (head - gi + N) % N;
    var ageG = tNow - tR[gidx];
    if (ageG > WINDOW_MS) continue;
    var xG = x1 - (ageG / WINDOW_MS) * (x1 - x0);
    if (gapR[gidx]) {
      if (bandStartX === null) bandStartX = xG;
      bandEndX = xG;
    } else {
      flushBand();
    }
  }
  flushBand();

  // ---- L1: lag shading between actual & commanded -------------------------
  // Forward pass along commanded, backward along actual, filled amber — the
  // tracking error as AREA. Broken across gaps.
  ctx.fillStyle = 'rgba(245,185,77,0.10)';
  ctx.beginPath();
  var seg = [];
  var flushFill = function () {
    if (seg.length < 2) { seg.length = 0; return; }
    ctx.moveTo(seg[0][0], seg[0][1]);
    for (var k = 1; k < seg.length; k++) ctx.lineTo(seg[k][0], seg[k][1]);
    for (var k2 = seg.length - 1; k2 >= 0; k2--) ctx.lineTo(seg[k2][0], seg[k2][2]);
    ctx.closePath();
    seg.length = 0;
  };
  var prevT2 = 0;
  for (var j = len - 1; j >= 0; j--) {
    var jdx = (head - j + N) % N;
    var age2 = tNow - tR[jdx];
    if (age2 > WINDOW_MS) continue;
    if (prevT2 && (tR[jdx] - prevT2) > GAP_MS) flushFill();
    var x2 = x1 - (age2 / WINDOW_MS) * (x1 - x0);
    seg.push([x2, y1(tgtR[jdx]), y1(posR[jdx])]);
    prevT2 = tR[jdx];
  }
  flushFill();
  ctx.fill();

  // ---- L1 traces ----------------------------------------------------------
  ctx.lineWidth = 1;
  ctx.strokeStyle = ACCENT.intent;
  tracePath(ctx, tgtR, x0, x1, tNow, y1);
  ctx.setLineDash([3, 3]);
  ctx.strokeStyle = ACCENT.intentBright;
  tracePath(ctx, itpR, x0, x1, tNow, y1);
  ctx.setLineDash([]);
  ctx.lineWidth = 1.5;
  ctx.strokeStyle = ACCENT.reality;
  ctx.shadowColor = ac('r', 0.4);
  ctx.shadowBlur = 4;
  tracePath(ctx, posR, x0, x1, tNow, y1);
  ctx.shadowBlur = 0;

  // ---- L2: lag ------------------------------------------------------------
  ctx.lineWidth = 1;
  ctx.strokeStyle = '#F5B94D';
  ctx.beginPath();
  var started3 = false, prevT3 = 0;
  for (var m = len - 1; m >= 0; m--) {
    var mdx = (head - m + N) % N;
    var age3 = tNow - tR[mdx];
    if (age3 > WINDOW_MS) continue;
    var x3 = x1 - (age3 / WINDOW_MS) * (x1 - x0);
    var y3 = yLag(Math.abs(posR[mdx] - tgtR[mdx]));
    if (!started3 || (tR[mdx] - prevT3) > GAP_MS) { ctx.moveTo(x3, y3); started3 = true; }
    else ctx.lineTo(x3, y3);
    prevT3 = tR[mdx];
  }
  ctx.stroke();

  // ---- L3: bus volts + watts ---------------------------------------------
  ctx.strokeStyle = '#8A93A6';
  tracePath(ctx, vR, x0, x1, tNow, yV);
  ctx.strokeStyle = '#ECEFF4';
  tracePath(ctx, wR, x0, x1, tNow, yW);

  // ---- Legend values (~7Hz, not per frame) --------------------------------
  if (nowMs - S.lastLegendMs > 140 && head >= 0) {
    S.lastLegendMs = nowMs;
    var h2 = head;
    var set = function (id, txt) { var e = $(id); if (e) e.textContent = txt; };
    set('#dgActual', pad(posR[h2], 3, 1));
    set('#dgCmd', pad(tgtR[h2], 3, 1));
    set('#dgItp', isNaN(itpR[h2]) ? '---.-' : pad(itpR[h2], 3, 1));
    set('#dgLag', pad(Math.abs(posR[h2] - tgtR[h2]), 2, 1));
    set('#dgV', pad(vR[h2], 2, 1));
    set('#dgW', pad(wR[h2], 3, 1));
  }
}
