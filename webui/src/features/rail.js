/**
 * Travel rail + stroke-window band — the spine control surface.
 *
 * Command surface = the INPUT TAPE above the rail (normal mode: spans the
 * reported window exactly, so it is geometrically impossible to command outside
 * it; manual mode: spans full travel). The rail itself is display + window
 * editing only (band body drag = move window, handles = resize). Hazard ribbons
 * hatch the keep-out zones outside the window. The phosphor comet is a single
 * tapered gradient ribbon per direction-run (no per-segment banding).
 *
 * Ground-truth doctrine (R1/section 5.1): band, tape (normal extent) and both
 * hazard ribbons all position from the SAME (winMin,winMax) pair via
 * layoutWindow().
 */
import { $, clamp, pad, setVV, setVVState } from '../core/ui.js';
import { TRAVEL, winMin, winMax, setWinMin, setWinMax, renderWindow, pushWindow, setRailSync } from '../core/range.js';
import { ACCENT, ac } from '../core/theme.js';
import { getBufferStats } from '../core/telebuf.js';

// ===================== Single reused state object (no per-frame allocation) =====================
var S = {
  cv: null, ctx: null, dpr: 1,
  rect: { w: 0, h: 0, x: 0, y: 0 },
  orient: 'horizontal',
  host: null, panel: null, railSvg: null, bandEl: null, bandFill: null, bandHandleLo: null, bandHandleHi: null, bandLabel: null,
  heroActual: null, heroCmd: null, heroLag: null, heroSpeed: null, heroSpeedEma: 0,
  // Input tape (command surface above the rail) + hazard ribbons
  tapeAssembly: null, tapeTrack: null, tapeBar: null, tapePip: null,
  tapeMode: null, tapeExtent: null, tapeDrag: false,
  hzLo: null, hzHi: null,
  queue: [], qHead: 0, qTail: 0, qLen: 0, QCAP: 256,
  lastScheduledTs: 0, defaultDtMs: 10, lastSeenSeq: -1,
  posDisplay: 0, tgtDisplay: 0, posTarget: 0, tgtTarget: 0,
  drag: null, dragStartMm: 0, dragStartMin: 0, dragStartMax: 0,
  reducedMotion: false, animRunning: false, moving: false, lastMoveTs: 0,
  onTap: null, onPatternStop: null,
  // Manual mode — when true the rail is display-only and the tape (full-width)
  // is the live scrub instrument.
  manual: false,
  fadeAlpha: 'rgba(0,0,0,0.10)',
  realityColor: '#4DA6FF',
  intentColor: '#A78BFA',
};

var qTs = new Float64Array(S.QCAP);
var qPos = new Float64Array(S.QCAP);
var qTgt = new Float64Array(S.QCAP);

// Shared minimum window length (mm). Unified with the band handle-drag clamps
// and the section-4 set-min/max yielding-bounds rule.
var MINLEN = 5;

var DASH_CMD = [4, 3];
var DASH_NONE = [];

var lastFrameTs = 0;
var lastInterpTs = 0;

// ===================== Init =====================

export function initRail(opts) {
  S.onTap = opts.onTap || function(){};
  S.onPatternStop = opts.onPatternStop || function(){};
  S.reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  S.host = $('#spineRailHost');
  if (!S.host) return;
  S.panel = document.getElementById('railPanel');

  updateOrientation();
  buildStaticLayer();

  S.cv = document.createElement('canvas');
  S.cv.className = 'rail-canvas';
  // pointer-events:none — the canvas must NEVER intercept pointer events.
  S.cv.style.cssText = 'position:absolute;inset:0;width:100%;height:100%;touch-action:none;pointer-events:none;';
  S.host.appendChild(S.cv);
  S.ctx = S.cv.getContext('2d');

  // Hazard ribbons — inserted BEFORE the canvas so they sit BELOW it in
  // z-order (spec section 2), siblings of the band.
  buildHazards();

  buildBand();

  // Cache + wire the input tape (markup lives in index.html).
  cacheTape();

  // Wire the manual-mode Set-min/Set-max buttons (section 4).
  var minBtn = document.getElementById('railSetMinBtn');
  var maxBtn = document.getElementById('railSetMaxBtn');
  if (minBtn) minBtn.addEventListener('click', function() { railSetBoundHere('min'); });
  if (maxBtn) maxBtn.addEventListener('click', function() { railSetBoundHere('max'); });

  buildHeroes();

  sizeCanvas();
  window.addEventListener('resize', function() { updateOrientation(); sizeCanvas(); layoutWindow(); drawStaticLayer(); });

  if (typeof ResizeObserver !== 'undefined') {
    var ro = new ResizeObserver(function() { sizeCanvas(); layoutWindow(); });
    ro.observe(S.host);
  }

  wirePointerEvents();

  // Mirror the window-shadow state (pending/overdue) from the rail host onto the
  // tape assembly so the tape border goes dashed while the window is in-flight
  // (spec section 5.2).
  mirrorShadowToTape();

  // Register the rail-sync hook so range.js repaints band + tape + hazards +
  // ruler whenever the authoritative window/travel changes (boot, echo, config
  // adoption, cold path).
  setRailSync(function() {
    sizeCanvas();
    drawStaticLayer();
    layoutWindow();
  });

  startAnimLoop();
}

// ===================== Orientation =====================

function updateOrientation() {
  S.orient = 'horizontal';
  if (S.host) {
    S.host.classList.remove('vertical');
    S.host.classList.add('horizontal');
  }
}

// ===================== Static layer (ruler, endcaps, triangles) =====================

function buildStaticLayer() {
  var existing = S.host.querySelector('.rail-static');
  if (existing) existing.remove();

  var layer = document.createElement('div');
  layer.className = 'rail-static';
  layer.style.cssText = 'position:absolute;inset:0;pointer-events:none;';

  var endLo = document.createElement('div');
  endLo.className = 'rail-endcap lo';
  endLo.textContent = '0';
  var endHi = document.createElement('div');
  endHi.className = 'rail-endcap hi';
  endHi.textContent = '--';

  var triLo = document.createElement('div');
  triLo.className = 'rail-tri lo';
  var triHi = document.createElement('div');
  triHi.className = 'rail-tri hi';

  var ghost = document.createElement('div');
  ghost.className = 'rail-ghost';
  ghost.textContent = '--';

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
  if (S.endHi) S.endHi.textContent = String(travel);
  if (S.ghost) S.ghost.textContent = String(Math.round(travel / 2));

  var svg = S.railSvg;
  var w = S.rect.w || S.host.clientWidth;
  var h = S.rect.h || S.host.clientHeight;
  svg.setAttribute('viewBox', '0 0 ' + w + ' ' + h);
  svg.setAttribute('width', w);
  svg.setAttribute('height', h);

  while (svg.firstChild) svg.removeChild(svg.firstChild);

  var BASE_H = 72;
  var tickTop = (26 / BASE_H) * h;
  var majorLen = (14 / BASE_H) * h;
  var minorLen = (7 / BASE_H) * h;
  var baselineY = (33 / BASE_H) * h;
  var midLen = (10 / BASE_H) * h;

  var railMm = Math.max(Math.round(travel), 1);
  var pxPerMm = w / railMm;
  var minorStep = 1;
  if (pxPerMm < 2) minorStep = 2;
  if (pxPerMm < 1) minorStep = 5;
  if (railMm / minorStep > 600) minorStep = Math.ceil(railMm / 600);

  for (var mm = 0; mm <= railMm; mm += minorStep) {
    var isMajor = (mm % 10 === 0);
    var isMid = !isMajor && (mm % 5 === 0);
    var frac = mm / railMm;
    var line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
    var x = frac * w;
    var len = isMajor ? majorLen : (isMid ? midLen : minorLen);
    line.setAttribute('x1', x);
    line.setAttribute('y1', tickTop);
    line.setAttribute('x2', x);
    line.setAttribute('y2', tickTop + len);
    line.setAttribute('stroke', isMajor ? 'var(--line-3)' : 'var(--line-1)');
    line.setAttribute('stroke-width', '1');
    line.setAttribute('opacity', isMajor ? '1' : (isMid ? '0.85' : '0.5'));
    svg.appendChild(line);
  }

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

// ===================== Hazard ribbons (keep-out hatching) =====================

function buildHazards() {
  var lo = document.createElement('div');
  lo.className = 'rail-hz lo';
  var hi = document.createElement('div');
  hi.className = 'rail-hz hi';
  // Insert BEFORE the canvas so both ribbons sit below the phosphor in z-order.
  S.host.insertBefore(lo, S.cv);
  S.host.insertBefore(hi, S.cv);
  S.hzLo = lo;
  S.hzHi = hi;
  positionHazards();
}

function positionHazards() {
  if (!S.hzLo || TRAVEL <= 0) return;
  var lo = clamp(winMin, 0, TRAVEL);
  var hi = clamp(winMax, 0, TRAVEL);
  // Both ribbons are FULL-track-width boxes (CSS: left:0; right:0) revealed
  // by clip-path only. The old approach resized the boxes themselves, and
  // since the repeating hatch is anchored to the box origin, dragging the
  // window max made the .hi ribbon's stripes visibly slide along with the
  // edge. With a static box only the clip edge moves — the stripes stay
  // pinned to the track.
  S.hzLo.style.clipPath = 'inset(0 ' + (100 - lo / TRAVEL * 100) + '% 0 0)';
  S.hzHi.style.clipPath = 'inset(0 0 0 ' + (hi / TRAVEL * 100) + '%)';
  // Vertical: thin ribbon centered on the mid-rail baseline (33/72 of height).
  var baselineY = (33 / 72) * S.rect.h;
  var hh = S.hzLo.offsetHeight || 9;
  var top = (baselineY - hh / 2) + 'px';
  S.hzLo.style.top = top;
  S.hzHi.style.top = top;
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
  // Only position along the rail (left/width). top/height/background/border/
  // box-shadow are owned by the .rail-band CSS rule.
  S.bandEl.style.left = left + 'px';
  S.bandEl.style.width = Math.max(width, 8) + 'px';

  if (S.bandLabel) {
    S.bandLabel.textContent = pad(Math.round(lo), 3, 0) + '\u2013' + pad(Math.round(hi), 3, 0) + ' \u00b7 ' + pad(Math.round(hi - lo), 3, 0) + 'mm';
  }
}

// ===================== Input tape (command surface) =====================

function cacheTape() {
  S.tapeAssembly = document.getElementById('railTape');
  S.tapeTrack = document.getElementById('railTapeTrack');
  S.tapeBar = document.getElementById('railTapeBar');
  S.tapePip = document.getElementById('railTapePip');
  S.tapeMode = document.getElementById('railTapeMode');
  S.tapeExtent = document.getElementById('railTapeExtent');
  if (S.tapeBar) wireTapeEvents();
  positionTape();
  updateTapeLabels();
}

function positionTape() {
  if (!S.tapeBar || TRAVEL <= 0) return;
  if (S.manual) {
    // Manual mode — the tape spans the full rail width.
    S.tapeBar.style.left = '0%';
    S.tapeBar.style.width = '100%';
  } else {
    // Normal mode — the tape spans EXACTLY the reported window extent, so
    // mapping local x to [wMin..wMax] can never command outside the window.
    var lo = clamp(winMin, 0, TRAVEL);
    var hi = clamp(winMax, 0, TRAVEL);
    S.tapeBar.style.left = (lo / TRAVEL * 100) + '%';
    S.tapeBar.style.width = ((hi - lo) / TRAVEL * 100) + '%';
  }
}

function updateTapeLabels() {
  if (S.tapeMode) {
    S.tapeMode.textContent = S.manual ? 'input \u00b7 full travel \u00b7 manual' : 'input \u00b7 window';
  }
  if (S.tapeExtent) {
    var lo, hi;
    if (S.manual) { lo = 0; hi = Math.round(TRAVEL); }
    else { lo = Math.round(clamp(winMin, 0, TRAVEL)); hi = Math.round(clamp(winMax, 0, TRAVEL)); }
    S.tapeExtent.textContent = pad(lo, 3, 0) + '\u2013' + pad(hi, 3, 0);
  }
}

// Map the tape's LOCAL x-fraction to a commandable mm. Normal: [wMin..wMax];
// manual: [0..TRAVEL]. Also parks the scrub pip under the pointer.
function tapeMm(e) {
  var r = S.tapeBar.getBoundingClientRect();
  if (r.width <= 0 || TRAVEL <= 0) return 0;
  var frac = clamp((e.clientX - r.left) / r.width, 0, 1);
  if (S.tapePip) S.tapePip.style.left = (frac * 100) + '%';
  if (S.manual) return frac * TRAVEL;
  return winMin + frac * (winMax - winMin);
}

// Command through the EXISTING move sender (window.__sendMove). force=true on
// the initial press (immediate); force=false while dragging so the sender's own
// rate limit applies (do not add another).
function sendTape(mm, force) {
  if (typeof window.__sendMove === 'function') window.__sendMove(mm, force);
}

function wireTapeEvents() {
  var bar = S.tapeBar;
  bar.addEventListener('pointerdown', function(e) {
    if (TRAVEL <= 0) return;
    S.tapeDrag = true;
    if (bar.setPointerCapture) { try { bar.setPointerCapture(e.pointerId); } catch (err) {} }
    // Manual wins: a running pattern is stopped by the first touch.
    S.onPatternStop();
    if (S.tapePip) S.tapePip.classList.add('on');
    var mm = tapeMm(e);
    sendTape(mm, true);
    e.preventDefault();
  });
  bar.addEventListener('pointermove', function(e) {
    if (!S.tapeDrag) return;
    var mm = tapeMm(e);
    sendTape(mm, false);
    e.preventDefault();
  });
  var up = function(e) {
    if (!S.tapeDrag) return;
    S.tapeDrag = false;
    if (S.tapePip) S.tapePip.classList.remove('on');
    if (e && e.preventDefault) e.preventDefault();
  };
  bar.addEventListener('pointerup', up);
  bar.addEventListener('pointercancel', up);
}

// ===================== One layout owner (band + tape + hazards) =====================
// Ground-truth doctrine (R1/section 5.1): every writer of window geometry routes
// through here so band, tape (normal extent) and both hazard ribbons render from
// the SAME (winMin,winMax) pair. Local drag updates winMin/winMax for preview;
// settle/echo/config-adoption update them from reported state and call
// renderWindow() -> setRailSync -> this function.
function layoutWindow() {
  positionBand();
  positionHazards();
  positionTape();
  updateTapeLabels();
}

// ===================== Shadow mirror (pending/overdue -> tape) =====================

function mirrorShadowToTape() {
  if (!S.host || !S.tapeAssembly || typeof MutationObserver === 'undefined') return;
  var apply = function() {
    S.tapeAssembly.classList.toggle('pending', S.host.classList.contains('pending'));
    S.tapeAssembly.classList.toggle('overdue1', S.host.classList.contains('overdue1'));
    S.tapeAssembly.classList.toggle('overdue2', S.host.classList.contains('overdue2'));
  };
  var obs = new MutationObserver(apply);
  obs.observe(S.host, { attributes: true, attributeFilter: ['class'] });
  apply();
}

// ===================== Section 4 — Set min / max from current position =====================
// Acts on the CURRENT ACTUAL displayed position (interpolated), rounded to
// 0.1mm. Yielding-bounds rule: the stated bound lands exactly where the carriage
// stood; the other bound yields to preserve MINLEN and the shove is announced by
// an amber 700ms flash on the yielding handle + band label. Dispatch goes
// through the SAME debounced window-set path the band uses.
function flashWarn(el) {
  if (!el) return;
  el.classList.remove('rail-flash-warn');
  void el.offsetWidth;   // force reflow so the animation restarts
  el.classList.add('rail-flash-warn');
  setTimeout(function() { el.classList.remove('rail-flash-warn'); }, 700);
}

function railSetBoundHere(which) {
  if (TRAVEL <= 0) return;
  var pos = Math.round(clamp(S.posDisplay, 0, TRAVEL) * 10) / 10;
  var nMin = winMin, nMax = winMax;
  var shoved = false, flashHandle = null;

  if (which === 'min') {
    nMin = clamp(pos, 0, TRAVEL - MINLEN);
    if (nMax < nMin + MINLEN) {
      nMax = Math.min(TRAVEL, nMin + MINLEN);
      shoved = true;
      flashHandle = S.bandHandleHi;   // HI handle yields
    }
  } else {
    nMax = clamp(pos, MINLEN, TRAVEL);
    if (nMin > nMax - MINLEN) {
      nMin = Math.max(0, nMax - MINLEN);
      shoved = true;
      flashHandle = S.bandHandleLo;   // LO handle yields
    }
  }

  setWinMin(nMin);
  setWinMax(nMax);
  // Debounced window-set path — shadow pending, echo confirms, overdue states
  // apply unchanged. renderWindow re-clamps + repaints via setRailSync.
  renderWindow();

  if (shoved) {
    flashWarn(flashHandle);
    flashWarn(S.bandLabel);
  }
}

// ===================== Hero numerals =====================

function buildHeroes() {
  var host = $('#spineHeroes');
  if (!host) return;
  host.innerHTML = '';

  var actWrap = document.createElement('div');
  actWrap.className = 'hero-item hero-primary';
  // Drawn crosshair reticle (DELTA D6) \u2014 was a plain \u2295 character styled
  // via ::first-letter; replaced with the mock's inline SVG (circle + four
  // tick marks + center dot) so it reads as an instrument mark, not a glyph.
  actWrap.innerHTML =
    '<span class="hero-label">' +
    '<svg class="hero-reticle" width="12" height="12" viewBox="0 0 12 12" aria-hidden="true">' +
    '<circle cx="6" cy="6" r="4" fill="none" stroke="currentColor" stroke-width="1"/>' +
    '<line x1="6" y1="0" x2="6" y2="2.6" stroke="currentColor" stroke-width="1"/>' +
    '<line x1="6" y1="9.4" x2="6" y2="12" stroke="currentColor" stroke-width="1"/>' +
    '<line x1="0" y1="6" x2="2.6" y2="6" stroke="currentColor" stroke-width="1"/>' +
    '<line x1="9.4" y1="6" x2="12" y2="6" stroke="currentColor" stroke-width="1"/>' +
    '<circle cx="6" cy="6" r="0.9" fill="currentColor"/>' +
    '</svg>actual \u00b7 mm</span>' +
    '<span class="vv hero-val" id="heroActual">000.0</span>';
  host.appendChild(actWrap);

  var cmdWrap = document.createElement('div');
  cmdWrap.className = 'hero-item hero-secondary';
  cmdWrap.innerHTML = '<span class="hero-label">commanded</span><span class="vv hero-val intent" id="heroCmd">000.0</span>';
  host.appendChild(cmdWrap);

  var lagWrap = document.createElement('div');
  lagWrap.className = 'hero-item hero-secondary';
  lagWrap.innerHTML = '<span class="hero-label">lag</span><span class="vv hero-val" id="heroLag">00.0</span>';
  host.appendChild(lagWrap);

  // Live speed — same size/role as lag, sits to its right. Derived from the
  // telemetry stream (telebuf), so it tracks real shaft motion and eases to 0
  // when idle. :3
  var spdWrap = document.createElement('div');
  spdWrap.className = 'hero-item hero-secondary';
  spdWrap.innerHTML = '<span class="hero-label">speed</span><span class="vv hero-val" id="heroSpeed">000</span>';
  host.appendChild(spdWrap);

  S.heroActual = $('#heroActual');
  S.heroCmd = $('#heroCmd');
  S.heroLag = $('#heroLag');
  S.heroSpeed = $('#heroSpeed');
}

// ===================== Pointer events (band drag/resize only) =====================
// The rail LOSES tap-to-command in normal mode (section 1): it is display +
// window editing only. Band body drag = move window; handles = resize. In manual
// mode the rail is display-only (scrubbing happens on the full-width tape).

function wirePointerEvents() {
  if (!S.host) return;

  S.host.addEventListener('pointerdown', function(e) {
    if (TRAVEL <= 0) return;
    // Manual mode: rail is display-only. The tape owns scrubbing now.
    if (S.manual) return;
    var mm = pxToMm(e);
    if (e.target === S.bandHandleLo) { startDrag('lo', e, mm); return; }
    if (e.target === S.bandHandleHi) { startDrag('hi', e, mm); return; }
    if (e.target === S.bandEl || e.target === S.bandFill || e.target === S.bandLabel) { startDrag('move', e, mm); return; }
    // Otherwise: nothing. The rail surface no longer commands moves (section 1).
  });

  S.host.addEventListener('pointermove', function(e) {
    if (!S.drag) return;
    var mm = pxToMm(e);
    if (S.drag === 'move') {
      var span = S.dragStartMax - S.dragStartMin;
      var delta = mm - S.dragStartMm;
      var nMin = clamp(S.dragStartMin + delta, 0, TRAVEL - span);
      setWinMin(nMin); setWinMax(nMin + span);
      layoutWindow();
      S.bandEl.classList.add('pending');
      pushWindow();
    } else if (S.drag === 'lo') {
      var nLo = clamp(mm, 0, winMax - MINLEN);
      setWinMin(nLo);
      layoutWindow();
      S.bandEl.classList.add('pending');
      pushWindow();
    } else if (S.drag === 'hi') {
      var nHi = clamp(mm, winMin + MINLEN, TRAVEL);
      setWinMax(nHi);
      layoutWindow();
      S.bandEl.classList.add('pending');
      pushWindow();
    }
    e.preventDefault();
  });

  S.host.addEventListener('pointerup', function(e) {
    if (!S.drag) return;
    S.bandEl.classList.remove('pending');
    if (S.panel) S.panel.classList.remove('drag-live');
    renderWindow();
    S.drag = null;
    e.preventDefault();
  });

  S.host.addEventListener('pointercancel', function() {
    if (S.drag && S.bandEl) S.bandEl.classList.remove('pending');
    if (S.panel) S.panel.classList.remove('drag-live');
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
  // drag-live kills the tape/hazard slide transitions so preview tracks the
  // finger 1:1 (section 2: width transitions instant during drag).
  if (S.panel) S.panel.classList.add('drag-live');
  if (S.host && S.host.setPointerCapture) {
    try { S.host.setPointerCapture(e.pointerId); } catch (err) {}
  }
  e.preventDefault();
}

// ===================== Telemetry feed =====================

export function railFeed(samples, dtMs, fromSeq) {
  if (!samples || samples.length === 0) return;
  if (typeof dtMs === 'number' && dtMs > 0) S.defaultDtMs = dtMs;

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
    var a = 1 - Math.pow(0.82, dtMs / 16.667);
    S.posDisplay += (S.posTarget - S.posDisplay) * a;
    S.tgtDisplay += (S.tgtTarget - S.tgtDisplay) * a;
    if (Math.abs(S.posTarget - S.posDisplay) < 0.05) S.posDisplay = S.posTarget;
    if (Math.abs(S.tgtTarget - S.tgtDisplay) < 0.05) S.tgtDisplay = S.tgtTarget;
    return;
  }

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

  var nextIdx2 = (S.qHead + 1) % S.QCAP;
  if (nowMs >= qTs[nextIdx2]) {
    var a2 = 1 - Math.pow(0.82, dtMs / 16.667);
    S.posDisplay += (qPos[nextIdx2] - S.posDisplay) * a2;
    S.tgtDisplay += (qTgt[nextIdx2] - S.tgtDisplay) * a2;
    if (Math.abs(qPos[nextIdx2] - S.posDisplay) < 0.05) S.posDisplay = qPos[nextIdx2];
    if (Math.abs(qTgt[nextIdx2] - S.tgtDisplay) < 0.05) S.tgtDisplay = qTgt[nextIdx2];
    S.posTarget = qPos[nextIdx2];
    S.tgtTarget = qTgt[nextIdx2];
    return;
  }

  var span = qTs[nextIdx2] - qTs[curIdx];
  var frac = span > 0 ? clamp((nowMs - qTs[curIdx]) / span, 0, 1) : 1;
  S.posDisplay = qPos[curIdx] + (qPos[nextIdx2] - qPos[curIdx]) * frac;
  S.tgtDisplay = qTgt[curIdx] + (qTgt[nextIdx2] - qTgt[curIdx]) * frac;
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

  var lag = Math.abs(S.posDisplay - S.tgtDisplay);
  var isMoving = lag > 0.5;
  if (isMoving) S.lastMoveTs = nowMs;
  var glowActive = (nowMs - S.lastMoveTs) < 300;

  drawCanvas(nowMs, glowActive);

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
  if (S.heroSpeed) {
    // telebuf lastVel is mm/ms → ×1000 = mm/s; abs (magnitude, not direction).
    // Light EMA so the numeral doesn't jitter frame-to-frame.
    var spd = Math.abs(getBufferStats().lastVel) * 1000;
    S.heroSpeedEma += 0.2 * (spd - S.heroSpeedEma);
    setVV('heroSpeed', S.heroSpeedEma, 3, 0, 'mm/s');
  }

  requestAnimationFrame(animFrame);
}

// ===================== Comet trail (section 3 — smooth speed-scaled ribbon) =====================
var _velSmooth = 0;
var _prevPx = 0, _prevTx = 0;
var _prevDrawn = false;

// Position-history ring of the CENTER DOT's interpolated position (section
// 3.1/3.2). Fixed-size Float64Array ring, retained ~850ms, no per-frame alloc.
var TRAIL_CAP = 320;
var trailX = new Float64Array(TRAIL_CAP);
var trailT = new Float64Array(TRAIL_CAP);
var trailHead = 0, trailLen = 0;
var TRAIL_MS = 850;

// Scratch arrays for building ONE tapered polygon per direction-run (reuse, no
// per-frame allocation). Sized to the ring capacity.
var _polyX = new Float64Array(TRAIL_CAP);
var _polyW = new Float64Array(TRAIL_CAP);

// Reference speed (px/ms) the head half-width normalizes against (section 3.4).
// Derived from the input-set max speed mapped to px/ms at the current rail
// width; falls back to a sane constant when unavailable. Logged once.
var _refLogged = false;
function referenceSpeedPxPerMs() {
  var mmPerS = 0;
  var el = document.getElementById('maxSpeed');
  if (el) { var v = parseFloat(el.value); if (isFinite(v) && v > 0) mmPerS = v; }
  var pxPerMm = (TRAVEL > 0 && S.rect.w > 0) ? (S.rect.w / TRAVEL) : 0;
  var ref;
  if (mmPerS > 0 && pxPerMm > 0) {
    ref = (mmPerS * pxPerMm) / 1000;   // mm/s -> px/ms
    if (!_refLogged) { console.log('[rail] comet reference speed from maxSpeed=' + mmPerS + 'mm/s -> ' + ref.toFixed(4) + ' px/ms'); _refLogged = true; }
  } else {
    ref = 0.9;
    if (!_refLogged) { console.log('[rail] comet reference speed FALLBACK ' + ref + ' px/ms (maxSpeed/travel unavailable)'); _refLogged = true; }
  }
  if (!isFinite(ref) || ref <= 0.05) ref = 0.9;
  return ref;
}

// ===================== Canvas drawing (state set once, no per-frame alloc) =====================

function drawCanvas(nowMs, glowActive) {
  if (!S.ctx || !S.cv || S.rect.w === 0) return;

  // Refresh accents from the live theme each frame (plain assignments —
  // ACCENT is mutated in place on theme switch, ac() memoizes per theme).
  S.realityColor = ACCENT.reality;
  S.intentColor = ACCENT.intent;

  S.ctx.setTransform(S.dpr, 0, 0, S.dpr, 0, 0);

  var posFrac = clamp(S.posDisplay, 0, TRAVEL) / (TRAVEL || 1);
  var tgtFrac = clamp(S.tgtDisplay, 0, TRAVEL) / (TRAVEL || 1);
  var px = posFrac * S.rect.w;
  var tx = tgtFrac * S.rect.w;

  // FULLY clear every frame (section 3.1) — no destination-out persistence. The
  // entire comet is rebuilt from the position history, so there is no
  // accumulated-blit banding and no bright-dot string.
  S.ctx.clearRect(0, 0, S.rect.w, S.rect.h);

  var dtMs = nowMs - lastFrameTs;
  if (dtMs <= 0 || dtMs > 500) dtMs = 16.667;
  lastFrameTs = nowMs;

  // Record this frame's dot position into the history ring. Dedupe tiny idle
  // jitter, but keep the head timestamp fresh so the comet head tracks live.
  if (trailLen === 0) {
    trailX[0] = px; trailT[0] = nowMs; trailHead = 0; trailLen = 1;
  } else if (nowMs - trailT[trailHead] >= 8 || Math.abs(px - trailX[trailHead]) >= 0.35) {
    trailHead = (trailHead + 1) % TRAIL_CAP;
    trailX[trailHead] = px; trailT[trailHead] = nowMs;
    if (trailLen < TRAIL_CAP) trailLen++;
  } else {
    trailX[trailHead] = px; trailT[trailHead] = nowMs;
  }

  // Instantaneous velocity px/ms, EMA-smoothed (section 3.4, alpha ~0.2).
  var instVel = _prevDrawn ? Math.abs(px - _prevPx) / Math.max(dtMs, 1) : 0;
  _velSmooth += 0.2 * (instVel - _velSmooth);

  var BASE_H = 72;
  var h = S.rect.h;
  var caretY0 = (22 / BASE_H) * h;
  var caretY1 = (46 / BASE_H) * h;
  var markY0 = (16 / BASE_H) * h;
  var markY1 = (52 / BASE_H) * h;
  var midY = (markY0 + markY1) / 2;
  var H = markY1 - markY0;   // full marker-line span (section 3.4 thickness ref)

  // The v0.4 interpolator planned-segment overlay used to draw here (amber/
  // green line on the rail canvas itself) — DELTA D8 replaced it with the
  // dedicated plan strip (features/planstrip.js), a separate canvas below
  // the rail with its own render loop. Green is gone from the palette.

  // COMET TRAIL — one filled, tapered gradient ribbon per direction-run.
  if (S.reducedMotion) {
    drawReducedTrail(nowMs, midY);
  } else {
    var ref = referenceSpeedPxPerMs();
    var speedNorm = clamp(_velSmooth / ref, 0, 1);
    var headHalf = 1 + (0.25 * H - 1) * speedNorm;   // lerp(1px, 0.25*H, speedNorm)
    if (headHalf < 1) headHalf = 1;
    drawCometRibbons(nowMs, midY, headHalf, glowActive);
  }

  // Commanded caret — purple, crisp, redrawn every frame, NO trail (3.1).
  S.ctx.strokeStyle = S.intentColor;
  S.ctx.lineWidth = 1;
  S.ctx.shadowColor = ac('i', 0.7);
  S.ctx.shadowBlur = 6;
  S.ctx.globalAlpha = 1;
  S.ctx.beginPath();
  S.ctx.moveTo(tx, caretY0);
  S.ctx.lineTo(tx, caretY1);
  S.ctx.stroke();

  // Full-height marker line — crisp, NO trail (3.1).
  S.ctx.strokeStyle = S.realityColor;
  S.ctx.lineWidth = 1;
  S.ctx.shadowColor = ac('r', 0.55);
  S.ctx.shadowBlur = 4;
  S.ctx.beginPath();
  S.ctx.moveTo(px, markY0);
  S.ctx.lineTo(px, markY1);
  S.ctx.stroke();

  // Bright core dot at the live leading edge.
  S.ctx.shadowColor = ac('r', 0.9);
  S.ctx.shadowBlur = glowActive ? 12 : 8;
  S.ctx.fillStyle = ACCENT.core;
  S.ctx.beginPath();
  S.ctx.arc(px, midY, 2.6, 0, Math.PI * 2);
  S.ctx.fill();

  S.ctx.shadowBlur = 0;
  S.ctx.globalAlpha = 1;
  S.ctx.lineCap = 'butt';
  S.ctx.lineJoin = 'miter';

  _prevPx = px;
  _prevTx = tx;
  _prevDrawn = true;
}

// Section 3.3 — build ONE filled tapered polygon per contiguous same-direction
// run, filled with ONE linear gradient (head -> transparent tail). A single
// gradient fill cannot band. Ribbons split at direction reversals so polygons
// never self-intersect.
function drawCometRibbons(nowMs, midY, headHalf, glowActive) {
  if (trailLen < 2) return;

  S.ctx.globalCompositeOperation = 'source-over';
  S.ctx.setLineDash(DASH_NONE);
  // One soft glow pass over the fills (section 3.3).
  S.ctx.shadowColor = ac('r', 0.6);
  S.ctx.shadowBlur = glowActive ? 8 : 5;

  var idx = trailHead;
  var runStart = 0;     // index into _poly arrays where current run began
  var runDir = 0;       // -1, 0, +1
  var m = 0;            // points buffered so far (newest to oldest)

  for (var k = 0; k < trailLen; k++) {
    var age = nowMs - trailT[idx];
    if (age > TRAIL_MS) break;
    var f = 1 - (age / TRAIL_MS);
    if (f < 0) f = 0;
    var taper = f * f;                       // (1 - age/850)^2 (section 3.3)
    var x = trailX[idx];
    if (m > 0) {
      var dx = x - _polyX[m - 1];            // newer minus this (older)
      var dir = dx > 0.001 ? 1 : (dx < -0.001 ? -1 : runDir);
      if (runDir !== 0 && dir !== 0 && dir !== runDir) {
        // Direction reversed — flush [runStart .. m-1], start a new run that
        // SHARES the boundary point so ribbons visually connect.
        flushRibbon(runStart, m - 1, midY);
        runStart = m - 1;
        runDir = dir;
      } else if (runDir === 0) {
        runDir = dir;
      }
    }
    _polyX[m] = x;
    _polyW[m] = Math.max(headHalf * taper, 0.15);
    m++;
    idx = (idx - 1 + TRAIL_CAP) % TRAIL_CAP;
  }
  // Flush the final (oldest) run.
  if (m - 1 > runStart) flushRibbon(runStart, m - 1, midY);

  S.ctx.shadowBlur = 0;
  S.ctx.globalAlpha = 1;
}

// Points _polyX[startI..endI] / _polyW[..] are ordered newest to oldest.
function flushRibbon(startI, endI, midY) {
  var nPts = endI - startI + 1;
  if (nPts < 2) return;
  var headX = _polyX[startI];
  var tailX = _polyX[endI];
  // ONE gradient along x from head (alpha ~.55) to tail (transparent).
  var g = S.ctx.createLinearGradient(headX, 0, tailX, 0);
  g.addColorStop(0, ac('r', 0.55));
  g.addColorStop(1, ac('r', 0));
  S.ctx.fillStyle = g;
  S.ctx.beginPath();
  // top edge newest to oldest
  S.ctx.moveTo(_polyX[startI], midY - _polyW[startI]);
  for (var i = startI + 1; i <= endI; i++) S.ctx.lineTo(_polyX[i], midY - _polyW[i]);
  // bottom edge oldest to newest
  for (var j = endI; j >= startI; j--) S.ctx.lineTo(_polyX[j], midY + _polyW[j]);
  S.ctx.closePath();
  S.ctx.fill();
}

// prefers-reduced-motion trail: static thin 40%-opacity polyline, no ribbon.
function drawReducedTrail(nowMs, midY) {
  if (trailLen < 2) return;
  S.ctx.globalCompositeOperation = 'source-over';
  S.ctx.setLineDash(DASH_NONE);
  S.ctx.shadowBlur = 0;
  S.ctx.globalAlpha = 0.4;
  S.ctx.strokeStyle = S.realityColor;
  S.ctx.lineWidth = 1;
  S.ctx.beginPath();
  var idx = trailHead;
  var started = false;
  for (var k = 0; k < trailLen; k++) {
    var age = nowMs - trailT[idx];
    if (age > TRAIL_MS) break;
    if (!started) { S.ctx.moveTo(trailX[idx], midY); started = true; }
    else S.ctx.lineTo(trailX[idx], midY);
    idx = (idx - 1 + TRAIL_CAP) % TRAIL_CAP;
  }
  S.ctx.stroke();
  S.ctx.globalAlpha = 1;
}

// ===================== Export for external position updates =====================

export function railUpdatePosition(pos, tgt) {
  S.posTarget = clamp(pos, 0, TRAVEL);
  S.tgtTarget = clamp(tgt, 0, TRAVEL);
  if (!S.animRunning) startAnimLoop();
}

// ===================== Manual mode =====================
// Toggle the rail between DEFAULT mode (window band editing; the tape spans the
// window) and MANUAL mode (the tape spans full travel and is the live scrub
// instrument; the rail is display-only). The band is left in the DOM so it
// re-appears untouched when manual mode is switched off; CSS
// (.rail-panel.manual-mode) hides it and re-skins the host while active.
export function setRailManualMode(on) {
  S.manual = !!on;
  var panel = $('#railPanel');
  if (panel) panel.classList.toggle('manual-mode', S.manual);
  if (!S.manual && S.bandEl) S.bandEl.classList.remove('pending');
  S.drag = null;
  // Re-lay the tape/hazards for the new mode (extent + labels swap).
  layoutWindow();
  return S.manual;
}

export function isRailManualMode() {
  return S.manual;
}