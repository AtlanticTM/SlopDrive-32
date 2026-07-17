/**
 * Travel rail + stroke-window band — the spine control surface.
 *
 * Canvas phosphor trail (blue actual marker + purple commanded caret) with a
 * purple translucent stroke-window band (drag/resize/tap). Replaces the old
 * vertical track designer and Position slider as the primary control surface.
 *
 * The rAF/canvas loop is allocation-free: no object/array literals, no closures
 * created in the loop, canvas state set once, rect cached on resize.
 */
import { $, clamp, pad, setVV, setVVState } from '../core/ui.js';
import { TRAVEL, winMin, winMax, setWinMin, setWinMax, renderWindow, pushWindow } from '../core/range.js';

// ===================== Single reused state object (no per-frame allocation) =====================
var S = {
  // Canvas
  cv: null, ctx: null, dpr: 1,
  rect: { w: 0, h: 0, x: 0, y: 0 },
  // Orientation: 'horizontal' (desktop) or 'vertical' (portrait)
  orient: 'horizontal',
  // Host elements
  host: null, railSvg: null, bandEl: null, bandFill: null, bandHandleLo: null, bandHandleHi: null, bandLabel: null,
  // Hero numerals
  heroActual: null, heroCmd: null, heroLag: null,
  // Telemetry feed (time-indexed, ring buffer)
  queue: [], qHead: 0, qTail: 0, qLen: 0, QCAP: 256,
  lastScheduledTs: 0, defaultDtMs: 10, lastSeenSeq: -1,
  // Interpolated display values
  posDisplay: 0, tgtDisplay: 0, posTarget: 0, tgtTarget: 0,
  // Drag state
  drag: null, dragStartMm: 0, dragStartMin: 0, dragStartMax: 0,
  // Flags
  reducedMotion: false, animRunning: false, moving: false, lastMoveTs: 0,
  // Callbacks (set by initRail)
  onTap: null, onPatternStop: null,
  // Cached canvas style strings (set once)
  fadeAlpha: 'rgba(0,0,0,0.10)',
  realityColor: '#4DA6FF',
  intentColor: '#A78BFA',
};

// Pre-allocate the queue as a typed array ring (no per-frame push/shift)
// Each sample: {ts, pos, tgt} — stored in 3 parallel Float64Arrays
var qTs = new Float64Array(S.QCAP);
var qPos = new Float64Array(S.QCAP);
var qTgt = new Float64Array(S.QCAP);

// Pre-allocate dash arrays for setLineDash (avoids per-frame array literal allocation)
var DASH_CMD = [4, 3];
var DASH_NONE = [];

// Fade timing (frame-rate-independent destination-out)
var lastFrameTs = 0;

// Interpolation smoothing timing (separate clock from the fade)
var lastInterpTs = 0;

// ===================== Init =====================

export function initRail(opts) {
  S.onTap = opts.onTap || function(){};
  S.onPatternStop = opts.onPatternStop || function(){};
  S.reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  S.host = $('#spineRailHost');
  if (!S.host) return;

  // Detect orientation from viewport
  updateOrientation();

  // Build static DOM layer (ruler, endcaps, triangles, trk mark)
  buildStaticLayer();

  // Build canvas overlay
  S.cv = document.createElement('canvas');
  S.cv.className = 'rail-canvas';
  // pointer-events:none — the canvas must NEVER intercept pointer events.
  // Root cause of the dead tap handler (task spec §B diagnosis): with the
  // canvas painted on top of the rail at full inset and no pointer-events
  // override, clicks on empty rail (not covered by the DOM band) hit the
  // canvas element. Because the host's delegated listener still receives
  // the bubbled event today, this alone wasn't fully fatal — but leaving
  // the canvas hit-testable is fragile (any future z-index/composite change
  // silently breaks tap-to-command again) and violates the explicit spec
  // requirement. Locking it down here.
  S.cv.style.cssText = 'position:absolute;inset:0;width:100%;height:100%;touch-action:none;pointer-events:none;';
  S.host.appendChild(S.cv);
  S.ctx = S.cv.getContext('2d');

  // Build band (DOM, not canvas — pointer events are easier)
  buildBand();

  // Build hero numerals in #spineHeroes (hero strip, Row 2)
  buildHeroes();

  // Size canvas + cache rect
  sizeCanvas();
  window.addEventListener('resize', function() { updateOrientation(); sizeCanvas(); positionBand(); drawStaticLayer(); });

  // ResizeObserver for the host
  if (typeof ResizeObserver !== 'undefined') {
    var ro = new ResizeObserver(function() { sizeCanvas(); positionBand(); });
    ro.observe(S.host);
  }

  // Pointer events for band + tap
  wirePointerEvents();

  // Start rAF loop
  startAnimLoop();
}

// ===================== Orientation =====================

function updateOrientation() {
  // The rail panel is a full-width horizontal instrument at every breakpoint.
  S.orient = 'horizontal';
  if (S.host) {
    S.host.classList.remove('vertical');
    S.host.classList.add('horizontal');
  }
}

// ===================== Static layer (ruler, endcaps, triangles) =====================

function buildStaticLayer() {
  // Clear host (keep canvas + band which are added after)
  var existing = S.host.querySelector('.rail-static');
  if (existing) existing.remove();

  var layer = document.createElement('div');
  layer.className = 'rail-static';
  layer.style.cssText = 'position:absolute;inset:0;pointer-events:none;';

  // Endcap labels
  var endLo = document.createElement('div');
  endLo.className = 'rail-endcap lo';
  endLo.textContent = '0';
  var endHi = document.createElement('div');
  endHi.className = 'rail-endcap hi';
  endHi.textContent = '--';

  // Inward triangle glyphs
  var triLo = document.createElement('div');
  triLo.className = 'rail-tri lo';
  var triHi = document.createElement('div');
  triHi.className = 'rail-tri hi';

  // Ghost mid-scale numeral
  var ghost = document.createElement('div');
  ghost.className = 'rail-ghost';
  ghost.textContent = '--';

  // Ruler ticks (SVG)
  var svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('class', 'rail-ruler-svg');
  svg.setAttribute('preserveAspectRatio', 'none');

  layer.appendChild(endLo);
  layer.appendChild(endHi);
  layer.appendChild(triLo);
  layer.appendChild(triHi);
  layer.appendChild(ghost);
  layer.appendChild(svg);

  S.host.insertBefore(layer, S.host.firstChild);
  S.railSvg = svg;
  S.endHi = endHi;
  S.ghost = ghost;

  drawStaticLayer();
}

function drawStaticLayer() {
  if (!S.railSvg) return;
  var travel = Math.round(TRAVEL);
  // Update endcap
  if (S.endHi) S.endHi.textContent = String(travel);
  // Update ghost mid-scale
  if (S.ghost) S.ghost.textContent = String(Math.round(travel / 2));

  // Build ticks: 26 ticks, majors every 5
  var svg = S.railSvg;
  var w = S.rect.w || S.host.clientWidth;
  var h = S.rect.h || S.host.clientHeight;
  svg.setAttribute('viewBox', '0 0 ' + w + ' ' + h);
  svg.setAttribute('width', w);
  svg.setAttribute('height', h);

  // Clear existing ticks
  while (svg.firstChild) svg.removeChild(svg.firstChild);

  // Spec §A.1 — tick ruler geometry, expressed as fractions of the 72px BASE
  // rail height (scaled automatically since h already carries --s): ticks
  // anchored at top:26px/72px, majors 14px/72px long, minors 7px/72px long,
  // baseline hairline at top:33px/72px full width. All scale with h.
  var BASE_H = 72;
  var tickTop = (26 / BASE_H) * h;
  var majorLen = (14 / BASE_H) * h;
  var minorLen = (7 / BASE_H) * h;
  var baselineY = (33 / BASE_H) * h;

  var n = 26;
  for (var i = 0; i <= n; i++) {
    var isMajor = (i % 5 === 0);
    var frac = i / n;
    var line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
    var x = frac * w;
    var len = isMajor ? majorLen : minorLen;
    line.setAttribute('x1', x);
    line.setAttribute('y1', tickTop);
    line.setAttribute('x2', x);
    line.setAttribute('y2', tickTop + len);
    line.setAttribute('stroke', isMajor ? 'var(--line-3)' : 'var(--line-1)');
    line.setAttribute('stroke-width', '1');
    svg.appendChild(line);
  }

  // Baseline hairline — full width, 1px, at top:33px/72px
  var baseline = document.createElementNS('http://www.w3.org/2000/svg', 'line');
  baseline.setAttribute('x1', 0);
  baseline.setAttribute('y1', baselineY);
  baseline.setAttribute('x2', w);
  baseline.setAttribute('y2', baselineY);
  baseline.setAttribute('stroke', 'var(--line-1)');
  baseline.setAttribute('stroke-width', '1');
  svg.appendChild(baseline);
}

// ===================== Canvas sizing (resize only, rect cached) =====================

function sizeCanvas() {
  if (!S.cv || !S.host) return;
  var w = S.host.clientWidth;
  var h = S.host.clientHeight;
  S.dpr = window.devicePixelRatio || 1;
  var cw = Math.round(w * S.dpr);
  var ch = Math.round(h * S.dpr);
  if (S.cv.width !== cw || S.cv.height !== ch) {
    S.cv.width = cw;
    S.cv.height = ch;
  }
  S.rect.w = w;
  S.rect.h = h;
  S.rect.x = 0;
  S.rect.y = 0;
}

// ===================== Band (DOM, pointer events) =====================

function buildBand() {
  var existing = S.host.querySelector('.rail-band');
  if (existing) existing.remove();

  var band = document.createElement('div');
  band.className = 'rail-band';

  var fill = document.createElement('div');
  fill.className = 'rail-band-fill';

  var label = document.createElement('div');
  label.className = 'rail-band-label';

  var handleLo = document.createElement('div');
  handleLo.className = 'rail-band-handle lo';

  var handleHi = document.createElement('div');
  handleHi.className = 'rail-band-handle hi';

  band.appendChild(fill);
  band.appendChild(label);
  band.appendChild(handleLo);
  band.appendChild(handleHi);
  S.host.appendChild(band);

  S.bandEl = band;
  S.bandFill = fill;
  S.bandLabel = label;
  S.bandHandleLo = handleLo;
  S.bandHandleHi = handleHi;

  positionBand();
}

function positionBand() {
  if (!S.bandEl || TRAVEL <= 0) return;
  var lo = clamp(winMin, 0, TRAVEL);
  var hi = clamp(winMax, 0, TRAVEL);
  var fracLo = lo / TRAVEL;
  var fracHi = hi / TRAVEL;

  var left = fracLo * S.rect.w;
  var width = (fracHi - fracLo) * S.rect.w;
  // Only position along the rail (left/width) here. top/height/background/
  // border/box-shadow are owned by the .rail-band CSS rule (spec §A.3:
  // top:18px height:30px BASE, scaled by --s) — setting them inline here
  // previously stomped the CSS with top:0/height:100%, turning the faint
  // edge-lit band into a full-height filled box. Never override those two
  // properties from JS again.
  S.bandEl.style.left = left + 'px';
  S.bandEl.style.width = Math.max(width, 8) + 'px';

  // Label: min–max · lengthmm — Spec §A.3 exact format `010–110 · 100mm`
  if (S.bandLabel) {
    S.bandLabel.textContent = pad(Math.round(lo), 3, 0) + '\u2013' + pad(Math.round(hi), 3, 0) + ' \u00b7 ' + pad(Math.round(hi - lo), 3, 0) + 'mm';
  }
}

// ===================== Hero numerals =====================

function buildHeroes() {
  var host = $('#spineHeroes');
  if (!host) return;
  // Clear existing
  host.innerHTML = '';

  // actual — flagship numeral. Label sits ABOVE, with the + crosshair glyph.
  var actWrap = document.createElement('div');
  actWrap.className = 'hero-item hero-primary';
  actWrap.innerHTML =
    '<span class="hero-label"><svg viewBox="0 0 10 10" aria-hidden="true"><path d="M5 0v10M0 5h10" stroke="currentColor" stroke-width="1" fill="none"/></svg>actual \u00b7 mm</span>' +
    '<span class="vv hero-val" id="heroActual">000.0</span>';
  host.appendChild(actWrap);

  // commanded (purple .vv.intent, baseline-aligned beside actual)
  var cmdWrap = document.createElement('div');
  cmdWrap.className = 'hero-item hero-secondary';
  cmdWrap.innerHTML = '<span class="hero-label">commanded</span><span class="vv hero-val intent" id="heroCmd">000.0</span>';
  host.appendChild(cmdWrap);

  // lag (gray .vv, .w1 >5mm, .w2 >15mm)
  var lagWrap = document.createElement('div');
  lagWrap.className = 'hero-item hero-secondary';
  lagWrap.innerHTML = '<span class="hero-label">lag</span><span class="vv hero-val" id="heroLag">00.0</span>';
  host.appendChild(lagWrap);

  S.heroActual = $('#heroActual');
  S.heroCmd = $('#heroCmd');
  S.heroLag = $('#heroLag');
}

// ===================== Pointer events (band drag/resize + tap) =====================

function wirePointerEvents() {
  if (!S.host) return;

  S.host.addEventListener('pointerdown', function(e) {
    if (TRAVEL <= 0) return;
    var mm = pxToMm(e);
    // Check if on a handle
    if (e.target === S.bandHandleLo) { startDrag('lo', e, mm); return; }
    if (e.target === S.bandHandleHi) { startDrag('hi', e, mm); return; }
    // Check if on band body
    if (e.target === S.bandEl || e.target === S.bandFill || e.target === S.bandLabel) { startDrag('move', e, mm); return; }
    // Otherwise: tap-to-command (handled on pointerup). Capture on the HOST,
    // not e.target — capturing on the canvas caused the pointerup to be
    // retargeted/lost in some browsers so the tap never fired. :3
    S.drag = 'tap';
    S.dragStartMm = mm;
    if (S.host.setPointerCapture) {
      try { S.host.setPointerCapture(e.pointerId); } catch (err) {}
    }
    e.preventDefault();
  });

  S.host.addEventListener('pointermove', function(e) {
    if (!S.drag) return;
    var mm = pxToMm(e);
    if (S.drag === 'move') {
      var span = S.dragStartMax - S.dragStartMin;
      var delta = mm - S.dragStartMm;
      var nMin = clamp(S.dragStartMin + delta, 0, TRAVEL - span);
      setWinMin(nMin); setWinMax(nMin + span);
      positionBand();
      S.bandEl.classList.add('pending');
      pushWindow();
    } else if (S.drag === 'lo') {
      var nLo = clamp(mm, 0, winMax - 5);
      setWinMin(nLo);
      positionBand();
      S.bandEl.classList.add('pending');
      pushWindow();
    } else if (S.drag === 'hi') {
      var nHi = clamp(mm, winMin + 5, TRAVEL);
      setWinMax(nHi);
      positionBand();
      S.bandEl.classList.add('pending');
      pushWindow();
    }
    e.preventDefault();
  });

  S.host.addEventListener('pointerup', function(e) {
    if (!S.drag) return;
    if (S.drag === 'tap') {
      // Tap-to-command: sendMove + stop pattern
      var mm = pxToMm(e);
      S.onTap(mm);
      S.onPatternStop();
    } else {
      // End drag: persist window
      S.bandEl.classList.remove('pending');
      renderWindow(); // persist via range.js
    }
    S.drag = null;
    e.preventDefault();
  });

  S.host.addEventListener('pointercancel', function() {
    if (S.drag && S.drag !== 'tap' && S.bandEl) S.bandEl.classList.remove('pending');
    S.drag = null;
  });
}

function pxToMm(e) {
  if (!S.host) return 0;
  var r = S.host.getBoundingClientRect();
  var frac = clamp((e.clientX - r.left) / r.width, 0, 1);
  return frac * TRAVEL;
}

function startDrag(mode, e, mm) {
  S.drag = mode;
  S.dragStartMm = mm;
  S.dragStartMin = winMin;
  S.dragStartMax = winMax;
  // Capture on the host so pointermove/up keep flowing to our listeners
  // even when the pointer leaves the element that was pressed.
  if (S.host && S.host.setPointerCapture) {
    try { S.host.setPointerCapture(e.pointerId); } catch (err) {}
  }
  e.preventDefault();
}

// ===================== Telemetry feed =====================

export function railFeed(samples, dtMs, fromSeq) {
  if (!samples || samples.length === 0) return;
  if (typeof dtMs === 'number' && dtMs > 0) S.defaultDtMs = dtMs;

  // Seq dedupe
  var skip = 0;
  if (typeof fromSeq === 'number') {
    if (fromSeq + samples.length - 1 <= S.lastSeenSeq) return;
    if (fromSeq <= S.lastSeenSeq) skip = S.lastSeenSeq - fromSeq + 1;
    S.lastSeenSeq = fromSeq + samples.length - 1;
  }
  if (skip > 0) samples = samples.slice(skip);
  if (samples.length === 0) return;

  var now = performance.now();
  var n = samples.length;
  var anchorEnd = now + 30;
  if (anchorEnd < S.lastScheduledTs + S.defaultDtMs) anchorEnd = S.lastScheduledTs + S.defaultDtMs;
  if (anchorEnd > now + 150) anchorEnd = now + 150;
  var startTs = anchorEnd - (n - 1) * S.defaultDtMs;

  for (var i = 0; i < n; i++) {
    var ts = startTs + i * S.defaultDtMs;
    var pos = clamp(samples[i][0], 0, TRAVEL);
    var tgt = clamp(samples[i].length > 1 ? samples[i][1] : samples[i][0], 0, TRAVEL);
    var idx = S.qTail % S.QCAP;
    qTs[idx] = ts;
    qPos[idx] = pos;
    qTgt[idx] = tgt;
    S.qTail++;
    if (S.qLen < S.QCAP) S.qLen++;
    else S.qHead = (S.qHead + 1) % S.QCAP;
    S.lastScheduledTs = ts;
  }

  if (!S.animRunning) startAnimLoop();
}

export function railSetPos(v) {
  S.posTarget = clamp(v, 0, TRAVEL);
}

export function railSetTarget(v) {
  S.tgtTarget = clamp(v, 0, TRAVEL);
}

// ===================== Interpolation (no allocation) =====================

function interpolate(nowMs) {
  var dtMs = nowMs - lastInterpTs;
  if (dtMs <= 0 || dtMs > 500) dtMs = 16.667;
  lastInterpTs = nowMs;

  if (S.qLen === 0) {
    // No sample queue (JSON/1Hz fallback or between bursts): exponentially
    // approach the target instead of snapping — frame-rate independent
    // (~85ms time constant), so sparse updates still read as smooth motion.
    var a = 1 - Math.pow(0.82, dtMs / 16.667);
    S.posDisplay += (S.posTarget - S.posDisplay) * a;
    S.tgtDisplay += (S.tgtTarget - S.tgtDisplay) * a;
    // Snap when within a hair to avoid asymptotic shimmer
    if (Math.abs(S.posTarget - S.posDisplay) < 0.05) S.posDisplay = S.posTarget;
    if (Math.abs(S.tgtTarget - S.tgtDisplay) < 0.05) S.tgtDisplay = S.tgtTarget;
    return;
  }

  // Advance head past consumed samples
  while (S.qLen > 1) {
    var nextIdx = (S.qHead + 1) % S.QCAP;
    if (qTs[nextIdx] <= nowMs) {
      S.qHead = nextIdx;
      S.qLen--;
    } else break;
  }

  var curIdx = S.qHead;
  if (S.qLen === 1 || nowMs <= qTs[curIdx]) {
    S.posDisplay = qPos[curIdx];
    S.tgtDisplay = qTgt[curIdx];
    S.posTarget = S.posDisplay;
    S.tgtTarget = S.tgtDisplay;
    return;
  }

  var nextIdx = (S.qHead + 1) % S.QCAP;
  if (nowMs >= qTs[nextIdx]) {
    // Past the last scheduled sample — ease onto it rather than snapping,
    // which hides the seam between telemetry bursts.
    var a2 = 1 - Math.pow(0.82, dtMs / 16.667);
    S.posDisplay += (qPos[nextIdx] - S.posDisplay) * a2;
    S.tgtDisplay += (qTgt[nextIdx] - S.tgtDisplay) * a2;
    if (Math.abs(qPos[nextIdx] - S.posDisplay) < 0.05) S.posDisplay = qPos[nextIdx];
    if (Math.abs(qTgt[nextIdx] - S.tgtDisplay) < 0.05) S.tgtDisplay = qTgt[nextIdx];
    S.posTarget = qPos[nextIdx];
    S.tgtTarget = qTgt[nextIdx];
    return;
  }

  var span = qTs[nextIdx] - qTs[curIdx];
  var frac = span > 0 ? clamp((nowMs - qTs[curIdx]) / span, 0, 1) : 1;
  S.posDisplay = qPos[curIdx] + (qPos[nextIdx] - qPos[curIdx]) * frac;
  S.tgtDisplay = qTgt[curIdx] + (qTgt[nextIdx] - qTgt[curIdx]) * frac;
  S.posTarget = S.posDisplay;
  S.tgtTarget = S.tgtDisplay;
}

// ===================== rAF loop (allocation-free) =====================

function startAnimLoop() {
  if (S.animRunning) return;
  S.animRunning = true;
  requestAnimationFrame(animFrame);
}

function animFrame(nowMs) {
  S.animRunning = true;

  interpolate(nowMs);

  // Detect motion for hero glow
  var lag = Math.abs(S.posDisplay - S.tgtDisplay);
  var isMoving = lag > 0.5;
  if (isMoving) S.lastMoveTs = nowMs;
  var glowActive = (nowMs - S.lastMoveTs) < 300;

  // Draw canvas
  drawCanvas(nowMs, glowActive);

  // Update hero numerals
  if (S.heroActual) {
    setVV('heroActual', S.posDisplay, 3, 1);
    if (glowActive) S.heroActual.classList.add('glow');
    else S.heroActual.classList.remove('glow');
  }
  if (S.heroCmd) {
    setVV('heroCmd', S.tgtDisplay, 3, 1);
  }
  if (S.heroLag) {
    setVV('heroLag', lag, 3, 1);
    S.heroLag.classList.remove('w1', 'w2');
    if (lag > 15) S.heroLag.classList.add('w2');
    else if (lag > 5) S.heroLag.classList.add('w1');
  }

  requestAnimationFrame(animFrame);
}

// Smooth velocity for speed-aware fade (EMA a=0.2, clamped)
var _velSmooth = 0;

// Previous frame's marker positions for bridge segments
var _prevPx = 0, _prevTx = 0;
var _prevDrawn = false;

// ===================== Canvas drawing (state set once, no per-frame alloc) =====================

function drawCanvas(nowMs, glowActive) {
  if (!S.ctx || !S.cv || S.rect.w === 0) return;

  S.ctx.setTransform(S.dpr, 0, 0, S.dpr, 0, 0);

  var posFrac = clamp(S.posDisplay, 0, TRAVEL) / (TRAVEL || 1);
  var tgtFrac = clamp(S.tgtDisplay, 0, TRAVEL) / (TRAVEL || 1);
  var px = posFrac * S.rect.w;
  var tx = tgtFrac * S.rect.w;

  // (a) Speed-aware phosphor fade (F3)
  if (!S.reducedMotion) {
    var dtMs = nowMs - lastFrameTs;
    if (dtMs <= 0 || dtMs > 500) dtMs = 16.667;
    lastFrameTs = nowMs;
    // Compute instantaneous velocity in px/ms, EMA-smooth
    var instVel = _prevDrawn ? Math.abs(px - _prevPx) / Math.max(dtMs, 1) : 0;
    _velSmooth += 0.2 * (instVel - _velSmooth);
    // Speed-aware fade: ~0.06 at rest → ~0.16 at max speed (~200px/16ms ≈ 12.5 px/ms)
    var fadeAlpha = clamp(0.06 + _velSmooth * 0.008, 0.06, 0.16);
    S.ctx.globalCompositeOperation = 'destination-out';
    S.ctx.globalAlpha = fadeAlpha;
    S.ctx.fillStyle = '#000';
    S.ctx.fillRect(0, 0, S.rect.w, S.rect.h);
    S.ctx.globalAlpha = 1;
  } else {
    S.ctx.clearRect(0, 0, S.rect.w, S.rect.h);
  }

  // Precompute y-coordinates (fractional of 72px BASE)
  var BASE_H = 72;
  var h = S.rect.h;
  var caretY0 = (22 / BASE_H) * h;
  var caretY1 = (46 / BASE_H) * h;
  var markY0 = (16 / BASE_H) * h;
  var markY1 = (52 / BASE_H) * h;
  var dotY = (34 / BASE_H) * h;

  // (a2) v0.4 interpolator planned-segment overlay (amber/green). Reads the
  // latest 0x04 INTERP snapshot from window.__INTERP (fed by main.js at ~45Hz).
  // Drawn beneath the markers so the live took/told lines stay on top.
  drawInterpOverlay(h);

  // (b) Commanded caret — purple, crisp point (no trail, F4)
  S.ctx.globalCompositeOperation = 'source-over';
  S.ctx.strokeStyle = S.intentColor;
  S.ctx.lineWidth = 1;
  S.ctx.shadowColor = 'rgba(167,139,250,.7)';
  S.ctx.shadowBlur = 6;
  S.ctx.setLineDash(DASH_NONE);
  S.ctx.beginPath();
  S.ctx.moveTo(tx, caretY0);
  S.ctx.lineTo(tx, caretY1);
  S.ctx.stroke();

  // (c) Actual marker — bridge segment from prev position (F2)
  S.ctx.strokeStyle = S.realityColor;
  S.ctx.lineWidth = 2;
  S.ctx.shadowColor = 'rgba(77,166,255,.9)';
  S.ctx.shadowBlur = glowActive ? 14 : 10;
  S.ctx.setLineDash(DASH_NONE);
  if (_prevDrawn) {
    // Draw continuous segment spanning from last frame's position
    S.ctx.beginPath();
    S.ctx.moveTo(_prevPx, markY0);
    S.ctx.lineTo(px, markY0);
    S.ctx.lineTo(px, markY1);
    S.ctx.lineTo(_prevPx, markY1);
    S.ctx.closePath();
    S.ctx.fillStyle = 'rgba(77,166,255,0.15)';
    S.ctx.fill();
    // Main stroke line along the center of the trail
    S.ctx.shadowBlur = 8;
    S.ctx.beginPath();
    S.ctx.moveTo(_prevPx, (markY0 + markY1) / 2);
    S.ctx.lineTo(px, (markY0 + markY1) / 2);
    S.ctx.stroke();
    S.ctx.fillStyle = '#000'; // reset fillStyle
  }
  // Always draw the current vertical marker line
  S.ctx.shadowBlur = glowActive ? 14 : 10;
  S.ctx.beginPath();
  S.ctx.moveTo(px, markY0);
  S.ctx.lineTo(px, markY1);
  S.ctx.stroke();

  // (d) Bright core dot at current position
  S.ctx.shadowBlur = glowActive ? 12 : 8;
  S.ctx.fillStyle = '#DCEEFF';
  S.ctx.beginPath();
  S.ctx.arc(px, dotY, 2, 0, Math.PI * 2);
  S.ctx.fill();

  S.ctx.shadowBlur = 0;

  _prevPx = px;
  _prevTx = tx;
  _prevDrawn = true;
}


function drawInterpOverlay(h) {
  var it = window.__INTERP;
  if (!it || !it.active) return;
  // Freshness gate — a stale snapshot means the stream stopped; don't leave a
  // frozen ghost segment painted on the rail.
  if (performance.now() - it.lastRxMs > 250) return;

  var span = winMax - winMin;
  if (span <= 0) return;

  // Interp positions are normalized 0..1 within the ACTIVE stroke window, so
  // map through winMin..winMax → mm → px. With this mapping the amber progress
  // dot should ride directly under the purple commanded caret; if it drifts,
  // the interpolator and the mapper disagree — exactly the kind of divergence
  // we're hunting. (If firmware normalizes over full travel instead, drop the
  // winMin offset and use TRAVEL directly.)
  var sMm = winMin + clamp(it.startPos, 0, 1) * span;
  var eMm = winMin + clamp(it.endPos,   0, 1) * span;
  var cMm = winMin + clamp(it.curPos,   0, 1) * span;
  var sx = (clamp(sMm, 0, TRAVEL) / (TRAVEL || 1)) * S.rect.w;
  var ex = (clamp(eMm, 0, TRAVEL) / (TRAVEL || 1)) * S.rect.w;
  var cx = (clamp(cMm, 0, TRAVEL) / (TRAVEL || 1)) * S.rect.w;

  var BASE_H = 72;
  var planY = (60 / BASE_H) * h;   // just below the baseline, clear of the marker band

  S.ctx.globalCompositeOperation = 'source-over';
  S.ctx.setLineDash(DASH_NONE);

  // Planned span (start → end): thin rule with endpoint ticks. Green marks a
  // v4 gradient segment (G-slope), amber a ramped/eased v3-style segment.
  var planColor = it.gradMode ? '#7CD992' : '#F5A623';
  S.ctx.strokeStyle = planColor;
  S.ctx.lineWidth = 1;
  S.ctx.shadowColor = it.gradMode ? 'rgba(124,217,146,.5)' : 'rgba(245,166,35,.5)';
  S.ctx.shadowBlur = 4;
  S.ctx.beginPath();
  S.ctx.moveTo(sx, planY);
  S.ctx.lineTo(ex, planY);
  S.ctx.stroke();
  S.ctx.beginPath();
  S.ctx.moveTo(sx, planY - 3); S.ctx.lineTo(sx, planY + 3);
  S.ctx.moveTo(ex, planY - 3); S.ctx.lineTo(ex, planY + 3);
  S.ctx.stroke();

  // Progress marker at curPos.
  S.ctx.shadowBlur = 6;
  S.ctx.fillStyle = it.gradMode ? '#B6F0C4' : '#FFD08A';
  S.ctx.beginPath();
  S.ctx.arc(cx, planY, 2.5, 0, Math.PI * 2);
  S.ctx.fill();

  S.ctx.shadowBlur = 0;
  S.ctx.fillStyle = '#000';
}

// ===================== Export for external position updates =====================

export function railUpdatePosition(pos, tgt) {
  S.posTarget = clamp(pos, 0, TRAVEL);
  S.tgtTarget = clamp(tgt, 0, TRAVEL);
  if (!S.animRunning) startAnimLoop();
}