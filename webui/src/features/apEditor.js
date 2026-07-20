/**
 * apEditor — visual curve editor for the Advanced pattern mode (fray-d port).
 *
 * Replaces the advanced slider farm with two direct-manipulation surfaces:
 *
 *  1. STROKE SHAPE (top SVG): one in+out stroke cycle drawn as position-vs-
 *     time. Drag the peak/valley dots vertically for max/min depth, drag the
 *     edge-midpoint dots horizontally for in/out speed (slope IS speed), and
 *     drag the corner diamonds radially for in/out accel — pull toward the
 *     corner for a hard snap, away for a smooth triangle. The rendered corner
 *     rounding is derived from the same accel→profile relation the firmware
 *     uses (accel fraction f = 1/(r+1), r = 1..10× minimum accel), so the
 *     curve you see is the profile family the machine actually runs.
 *
*  2. MODIFIER CYCLE (bottom SVG): the selected control's per-stroke
 *     modulation as a STAIRCASE — one bar per stroke (the view users found
 *     clearest). fray-d field names: drag the corner handles to stretch the
 *     "to min" / "at min" / "to max" / "at max" phases (each = one stroke
 *     count), the left-gutter fader for Amplitude, and the marker on the lower
 *     track for Offset. A chip row selects which control the cycle modulates.
 *     (Corner handles replaced the thin, funky segment dividers — grabbable
 *     dots track the pointer where the dividers did not.)
 *
 * Ground Truth Doctrine: the editor renders CONFIRMED device state (blue,
 * --reality). During a drag the active geometry follows the pointer and the
 * touched handle renders purple (--intent = desired/unconfirmed) while
 * debounced sends stream over the same gen_cfg path the sliders used; on
 * release the owner reconciles from GET /api/pattern and re-seeds the editor
 * with the device's post-clamp truth (onCommit callback). Until first seeded
 * from the device the surfaces are gated — dim guides, no handles, no sends.
 *
 * Rendering: full SVG rebuild per frame, throttled through rAF. Drags are
 * transient interactions, not a telemetry loop — the allocation-free rule for
 * rAF loops (Task 3R) targets always-on loops like the wave scope, and this
 * editor renders only on state change or drag, idling completely otherwise.
 * Hit testing is pure math against handle positions (never DOM events on
 * rebuilt nodes), with pointer capture on the SVG root.
 */

import { $ } from "../core/ui.js";

// ---- Wire ids (advpat::BaseId order — must match firmware) -----------------
var CTRL_NAMES = ['MAX DEPTH', 'MIN DEPTH', 'IN SPEED', 'OUT SPEED', 'IN ACCEL', 'OUT ACCEL'];

// ---- Editor state -----------------------------------------------------------
// base: confirmed-or-dragging values; mods: six reported modifier blocks.
var S = {
    seeded: false,
    base: { min_depth: 0, max_depth: 10, in_speed: 100, out_speed: 100, in_accel: 40, out_accel: 40 },
    mods: null,          // [{ctrl, amplitude, in_step, in_wait, out_step, out_wait, offset} x6]
    ctrl: 0,             // selected modifier target
    drag: null,          // {surface:'wave'|'env', handle, ...} while dragging
    cb: { onLive: null, onCommit: null },
    wave: { svg: null, w: 0, h: 0 },
    env:  { svg: null, w: 0, h: 0 },
    raf: 0
};

var PAD_X = 34, PAD_TOP = 18, PAD_BOT = 20;
var HIT_R = 28;          // pointer hit radius (px) around each handle — thumb-sized
var HANDLE_R = 8;        // visual circle radius
var DIAMOND = 13;        // visual diamond side

// ============================================================================
// Public API
// ============================================================================

export function initApEditor(callbacks) {
    S.cb.onLive   = callbacks.onLive   || function () {};
    S.cb.onCommit = callbacks.onCommit || function () {};

    S.wave.svg = $('apWaveSvg');
    S.env.svg  = $('apEnvSvg');
    if (!S.wave.svg || !S.env.svg) return;

    [['wave', S.wave], ['env', S.env]].forEach(function (pair) {
        var surface = pair[0], st = pair[1];
        var svg = st.svg;
        svg.addEventListener('pointerdown', function (e) { onDown(surface, st, e); });
        svg.addEventListener('pointermove', function (e) { onMove(surface, st, e); });
        svg.addEventListener('pointerup',   function (e) { onUp(e); });
        svg.addEventListener('pointercancel', function (e) { onUp(e); });
        if (typeof ResizeObserver !== 'undefined') {
            new ResizeObserver(function () { sizeSurface(st); scheduleRender(); }).observe(svg);
        }
        sizeSurface(st);
    });

    // Modifier target chips
    var chips = $('apModChips');
    if (chips) {
        chips.innerHTML = '';
        CTRL_NAMES.forEach(function (name, i) {
            var b = document.createElement('button');
            b.type = 'button';
            b.className = 'ap-chip';
            b.dataset.ctrl = i;
            b.innerHTML = '<span class="ap-chip-dot"></span>' + name;
            b.addEventListener('click', function () {
                S.ctrl = i;
                renderChips();
                scheduleRender();
            });
            chips.appendChild(b);
        });
        renderChips();
    }

    scheduleRender();
}

/** Adopt a full /api/pattern readback (device truth) — seeds/reconciles. */
export function setApEditorState(d) {
    if (!d) return;
    // Never yank geometry out from under an active drag — the release that
    // ends it triggers its own reconcile.
    if (S.drag) return;
    var b = S.base, any = false;
    ['min_depth', 'max_depth', 'in_speed', 'out_speed', 'in_accel', 'out_accel'].forEach(function (k) {
        var v = d['ap_' + k];
        if (typeof v === 'number') { b[k] = v; any = true; }
    });
    if (Array.isArray(d.ap_mods)) { S.mods = d.ap_mods; any = true; }
    if (any) S.seeded = true;
    renderChips();
    scheduleRender();
}

/** Current base fields for the shared gen_cfg payload. */
export function getApEditorBaseFields() {
    if (!S.seeded) return {};
    return {
        ap_min_depth: S.base.min_depth,  ap_max_depth: S.base.max_depth,
        ap_in_speed:  S.base.in_speed,   ap_out_speed: S.base.out_speed,
        ap_in_accel:  S.base.in_accel,   ap_out_accel: S.base.out_accel
    };
}

/** Full stroke-shape snapshot for preset save / dirty-detection. Captures the
 *  4 per-direction speed/accel controls + the 6 modifier blocks — NEVER depths
 *  or master speed (a preset is stroke character, matching the factory set). */
export function getApSnapshot() {
    if (!S.seeded || !S.mods) return null;
    var mods = [];
    for (var id = 0; id < 6; id++) {
        var src = null;
        S.mods.forEach(function (m) { if (m && m.ctrl === id) src = m; });
        if (!src) src = { ctrl: id, amplitude: 100, in_step: 1, in_wait: 0, out_step: 1, out_wait: 0, offset: 0 };
        mods.push({ ctrl: id, amplitude: src.amplitude, in_step: src.in_step, in_wait: src.in_wait,
                    out_step: src.out_step, out_wait: src.out_wait, offset: src.offset });
    }
    return {
        in_speed: S.base.in_speed, out_speed: S.base.out_speed,
        in_accel: S.base.in_accel, out_accel: S.base.out_accel,
        mods: mods
    };
}

// ============================================================================
// Shared helpers
// ============================================================================

function sizeSurface(st) {
    st.w = st.svg.clientWidth;
    st.h = st.svg.clientHeight;
    st.svg.setAttribute('viewBox', '0 0 ' + st.w + ' ' + st.h);
}

function scheduleRender() {
    if (S.raf) return;
    S.raf = requestAnimationFrame(function () {
        S.raf = 0;
        renderWave();
        renderEnv();
    });
}

function clamp(v, a, b) { return v < a ? a : (v > b ? b : v); }

function selMod() {
    if (!S.mods) return null;
    for (var i = 0; i < S.mods.length; i++) if (S.mods[i] && S.mods[i].ctrl === S.ctrl) return S.mods[i];
    return null;
}

function svgPoint(st, e) {
    var r = st.svg.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
}

// accel value (0..100) → accel-phase fraction of the edge (0.5 = pure
// triangle, →0 = hard snap). Mirrors firmware: r = 1+9k, f = 1/(r+1).
function accelFrac(val) {
    var r = 1 + 9 * (clamp(val, 0, 100) / 100);
    return 1 / (r + 1);
}

// ============================================================================
// Stroke-shape surface geometry
// ============================================================================

// Each edge has a raw "weight" ∝ its stroke time (fast = small, slow = large).
// The two weights are scaled so the curve always FILLS the panel width, while
// the PEAK position encodes the in/out balance (per-direction speed is a
// multiplier on master speed, so the balance is what the shape means; the
// absolute values live in the labels). Dragging a handle sets the split so the
// grabbed handle tracks the pointer, deriving that direction's speed while the
// other stays fixed — decoupled in value, no runaway.
var EDGE_MIN_W = 0.14, EDGE_MAX_W = 0.44;

function edgeWeight(speed) {
    var s = clamp(speed, 1, 100);
    return EDGE_MIN_W + (1 - (s - 1) / 99) * (EDGE_MAX_W - EDGE_MIN_W);  // fast→min, slow→max
}
function speedFromWeight(wght) {
    var frac = (clamp(wght, EDGE_MIN_W, EDGE_MAX_W) - EDGE_MIN_W) / (EDGE_MAX_W - EDGE_MIN_W); // 0 fast..1 slow
    return clamp(Math.round(100 - frac * 99), 1, 100);
}

function waveGeom() {
    var w = S.wave.w, h = S.wave.h;
    var W = w - 2 * PAD_X, H = h - PAD_TOP - PAD_BOT;
    var b = S.base;
    var yOf = function (depth) { return PAD_TOP + (1 - depth / 100) * H; };
    var rIn = edgeWeight(b.in_speed), rOut = edgeWeight(b.out_speed);
    var scale = W / (rIn + rOut);            // fill the width; keep the ratio
    var wIn = rIn * scale, wOut = W - wIn;
    var x0 = PAD_X, x1 = x0 + wIn, x2 = x0 + W;
    var yMin = yOf(b.min_depth), yMax = yOf(b.max_depth);
    var fIn = accelFrac(b.in_accel), fOut = accelFrac(b.out_accel);
    return {
        W: W, H: H, x0: x0, x1: x1, x2: x2, yMin: yMin, yMax: yMax,
        fIn: fIn, fOut: fOut, wIn: wIn, wOut: wOut, yOf: yOf
    };
}

// Cubic bezier point (horizontal tangents at both ends = zero-velocity
// endpoints, control distance = accel fraction × edge width).
function edgePoint(xa, ya, xb, yb, f, w, t) {
    var c1x = xa + f * w, c1y = ya, c2x = xb - f * w, c2y = yb;
    var mt = 1 - t;
    return {
        x: mt*mt*mt*xa + 3*mt*mt*t*c1x + 3*mt*t*t*c2x + t*t*t*xb,
        y: mt*mt*mt*ya + 3*mt*mt*t*c1y + 3*mt*t*t*c2y + t*t*t*yb
    };
}

function waveHandles(g) {
    return {
        max:  { x: g.x1, y: g.yMax },
        min:  { x: g.x2, y: g.yMin },
        vin:  edgePoint(g.x0, g.yMin, g.x1, g.yMax, g.fIn, g.wIn, 0.5),
        vout: edgePoint(g.x1, g.yMax, g.x2, g.yMin, g.fOut, g.wOut, 0.5),
        ain:  edgePoint(g.x0, g.yMin, g.x1, g.yMax, g.fIn, g.wIn, 0.22),
        aout: edgePoint(g.x1, g.yMax, g.x2, g.yMin, g.fOut, g.wOut, 0.78)
    };
}

function renderWave() {
    var svg = S.wave.svg;
    if (!svg || !S.wave.w) return;
    var out = [];
    var w = S.wave.w, h = S.wave.h;

    // Depth grid: 0/25/50/75/100
    for (var d = 0; d <= 100; d += 25) {
        var gy = PAD_TOP + (1 - d / 100) * (h - PAD_TOP - PAD_BOT);
        out.push('<line class="ap-grid" x1="' + PAD_X + '" y1="' + gy + '" x2="' + (w - PAD_X) + '" y2="' + gy + '"/>');
        out.push('<text class="ap-axis" x="' + (PAD_X - 6) + '" y="' + (gy + 3) + '" text-anchor="end">' + d + '</text>');
    }

    if (!S.seeded) {
        out.push('<text class="ap-axis" x="' + (w / 2) + '" y="' + (h / 2) + '" text-anchor="middle">awaiting device state…</text>');
        svg.innerHTML = out.join('');
        return;
    }

    var g = waveGeom();
    var hs = waveHandles(g);
    var b = S.base;

    // Depth guide lines at min/max
    out.push('<line class="ap-guide" x1="' + PAD_X + '" y1="' + g.yMax + '" x2="' + (w - PAD_X) + '" y2="' + g.yMax + '"/>');
    out.push('<line class="ap-guide" x1="' + PAD_X + '" y1="' + g.yMin + '" x2="' + (w - PAD_X) + '" y2="' + g.yMin + '"/>');

    // The stroke curve: rise then fall
    var p = 'M' + g.x0 + ',' + g.yMin +
        ' C' + (g.x0 + g.fIn * g.wIn) + ',' + g.yMin + ' ' + (g.x1 - g.fIn * g.wIn) + ',' + g.yMax + ' ' + g.x1 + ',' + g.yMax +
        ' C' + (g.x1 + g.fOut * g.wOut) + ',' + g.yMax + ' ' + (g.x2 - g.fOut * g.wOut) + ',' + g.yMin + ' ' + g.x2 + ',' + g.yMin;
    out.push('<path class="ap-curve" d="' + p + '"/>');

    // Handles
    var dragH = S.drag && S.drag.surface === 'wave' ? S.drag.handle : null;
    var circ = function (name, pt, label, lx, ly, anchor) {
        var cls = 'ap-handle' + (dragH === name ? ' drag' : '');
        // Faint halo advertises the true grab zone on touch screens
        out.push('<circle class="ap-halo" cx="' + pt.x + '" cy="' + pt.y + '" r="' + (HIT_R - 8) + '"/>');
        out.push('<circle class="' + cls + '" cx="' + pt.x + '" cy="' + pt.y + '" r="' + HANDLE_R + '"/>');
        out.push('<text class="ap-val' + (dragH === name ? ' drag' : '') + '" x="' + lx + '" y="' + ly + '" text-anchor="' + (anchor || 'middle') + '">' + label + '</text>');
    };
    var diam = function (name, pt, label, lx, ly, anchor) {
        var cls = 'ap-handle ap-diamond' + (dragH === name ? ' drag' : '');
        var hd = DIAMOND / 2;
        out.push('<circle class="ap-halo" cx="' + pt.x + '" cy="' + pt.y + '" r="' + (HIT_R - 8) + '"/>');
        out.push('<rect class="' + cls + '" x="' + (pt.x - hd) + '" y="' + (pt.y - hd) + '" width="' + DIAMOND + '" height="' + DIAMOND + '" transform="rotate(45 ' + pt.x + ' ' + pt.y + ')"/>');
        out.push('<text class="ap-val' + (dragH === name ? ' drag' : '') + '" x="' + lx + '" y="' + ly + '" text-anchor="' + (anchor || 'middle') + '">' + label + '</text>');
    };

    // deep: above the peak, flipping below only when the peak is at the top.
    var maxLy = g.yMax > PAD_TOP + 22 ? g.yMax - 14 : g.yMax + 20;
    circ('max',  hs.max,  'deep ' + b.max_depth, hs.max.x, maxLy);
    // shallow: to the RIGHT of the end handle so it never overlaps the
    // out-accel diamond's label (both cluster low when min depth is 0).
    circ('min',  hs.min,  'shallow ' + b.min_depth, hs.min.x + 12, g.yMin + 4, 'start');
    circ('vin',  hs.vin,  'in v' + b.in_speed,  hs.vin.x - 14, hs.vin.y, 'end');
    circ('vout', hs.vout, 'out v' + b.out_speed, hs.vout.x + 14, hs.vout.y, 'start');
    // Accel labels sit ABOVE their diamonds (toward the peak), clear of the
    // depth labels along the bottom.
    diam('ain',  hs.ain,  'a' + b.in_accel,  hs.ain.x - 12, hs.ain.y - 10, 'end');
    diam('aout', hs.aout, 'a' + b.out_accel, hs.aout.x + 12, hs.aout.y - 10, 'start');

    svg.innerHTML = out.join('');
}

// ============================================================================
// Modifier-envelope surface geometry
// ============================================================================

// Trapezoid-envelope geometry. The modifier cycle is one trapezoid: ramp-in
// (baseline → plateau over in_step strokes), hold (in_wait), ramp-out (back to
// baseline over out_step), rest (out_wait at baseline). Plateau DEPTH is the
// modulation amount (ratio). A fixed slot width keeps the shape from
// rubber-banding while dragging for any cycle up to 24 strokes (essentially
// all real use); only very long cycles shrink to fit.
var AMP_X = 16;   // left-gutter vertical amplitude fader

// Canonical per-slot modulation amount (0 at baseline/max, `ratio` at the
// dip/min), ignoring offset. Mirrors fray-d's getModification() staircase:
// ramp down over in_step, hold in_wait, ramp up over out_step, rest at max.
function modAmount(m, i) {
    var ratio = (100 - m.amplitude) / 100;
    if (m.in_step > 0 && i < m.in_step) return ratio * (i + 1) / m.in_step;
    i -= m.in_step;
    if (i < m.in_wait) return ratio;
    i -= m.in_wait;
    if (m.out_step > 0 && i < m.out_step) return ratio * (1 - (i + 1) / m.out_step);
    return 0;
}

function envGeom(m, fixedSw) {
    var w = S.env.w, h = S.env.h;
    var W = w - 2 * PAD_X;
    var H = h - PAD_TOP - PAD_BOT - 16;          // reserve room for offset track
    var total = m.in_step + m.in_wait + m.out_step + m.out_wait;
    if (total < 1) total = 1;
    // Fill the panel width (one stroke per bar). Frozen at the grab width during
    // a drag so the staircase doesn't rubber-band as the count changes.
    var sw = fixedSw || (W / total);
    var ratio = (100 - m.amplitude) / 100;       // 0 = no modulation, 1 = full
    var x0 = PAD_X;
    var xIn   = x0   + m.in_step  * sw;
    var xHold = xIn  + m.in_wait  * sw;
    var xOut  = xHold+ m.out_step * sw;
    var xEnd  = xOut + m.out_wait * sw;
    var yTop  = PAD_TOP;                          // baseline (mod 0)
    var yBot  = PAD_TOP + ratio * H;              // plateau (mod = ratio)
    var trackY = PAD_TOP + H + 14;               // offset track
    return { W:W, H:H, sw:sw, total:total, ratio:ratio, x0:x0,
             xIn:xIn, xHold:xHold, xOut:xOut, xEnd:xEnd,
             yTop:yTop, yBot:yBot, trackY:trackY };
}

function renderEnv() {
    var svg = S.env.svg;
    if (!svg || !S.env.w) return;
    var out = [];
    var w = S.env.w, h = S.env.h;
    var m = selMod();

    if (!S.seeded || !m) {
        out.push('<text class="ap-axis" x="' + (w / 2) + '" y="' + (h / 2) + '" text-anchor="middle">awaiting device state…</text>');
        svg.innerHTML = out.join('');
        return;
    }

    var g = envGeom(m);
    var dragH = S.drag && S.drag.surface === 'env' ? S.drag.handle : null;
    var off = m.amplitude >= 100;
    var ctrlName = CTRL_NAMES[S.ctrl] ? CTRL_NAMES[S.ctrl].toLowerCase() : '';

    // OFF state (amplitude 100) — no modulation. Show a flat "off" line + the
    // amplitude fader so it's obvious how to enable, instead of a confusing
    // pile of corner handles collapsed onto the baseline.
    if (off) {
        out.push('<line class="ap-guide" x1="' + g.x0 + '" y1="' + g.yTop + '" x2="' + (w - PAD_X) + '" y2="' + g.yTop + '"/>');
        out.push('<line class="ap-amp-track" x1="' + AMP_X + '" y1="' + g.yTop + '" x2="' + AMP_X + '" y2="' + (g.yTop + g.H) + '"/>');
        out.push('<circle class="ap-halo" cx="' + AMP_X + '" cy="' + g.yTop + '" r="' + (HIT_R - 10) + '"/>');
        out.push('<circle class="ap-handle' + (dragH === 'amp' ? ' drag' : '') + '" cx="' + AMP_X + '" cy="' + g.yTop + '" r="' + HANDLE_R + '"/>');
        out.push('<text class="ap-env-hint" x="' + (w / 2) + '" y="' + (g.yTop + g.H / 2) + '" text-anchor="middle">' +
                 ctrlName + ' holds steady — drag the amp dot ↓ to make it drift across strokes</text>');
        svg.innerHTML = out.join('');
        return;
    }

    // Segment region tints + centered counts
    var seg = function (xa, xb, label, cls) {
        if (xb - xa < 0.5) return;
        out.push('<rect class="ap-region ' + cls + '" x="' + xa + '" y="' + g.yTop + '" width="' + (xb - xa) + '" height="' + g.H + '"/>');
        out.push('<text class="ap-seg-lbl" x="' + ((xa + xb) / 2) + '" y="' + (g.yTop + g.H - 5) + '" text-anchor="middle">' + label + '</text>');
    };
    // fray-d field names: m1 "Steps to Min", m2 "Steps at Min", m3 "Steps to
    // Max", m4 "Steps at Max" — the control drifts DOWN to a minimum, holds,
    // drifts back UP to max, then rests at max.
    seg(g.x0,   g.xIn,   'to min ' + m.in_step,  'ramp');
    seg(g.xIn,  g.xHold, 'at min ' + m.in_wait,  'hold');
    seg(g.xHold,g.xOut,  'to max ' + m.out_step, 'ramp');
    seg(g.xOut, g.xEnd,  'at max ' + m.out_wait, 'rest');

    // Baseline
    out.push('<line class="ap-guide" x1="' + g.x0 + '" y1="' + g.yTop + '" x2="' + (w - PAD_X) + '" y2="' + g.yTop + '"/>');

    // STEPPED envelope — one tread per stroke slot (canonical, offset shown
    // separately by the marker). This is the "staircase" view: it reads as
    // "stroke 1 at this level, stroke 2 at this level …", which is clearer than
    // a smooth trapezoid. amt = 0 at baseline (max), ratio at the dip (min).
    var slotPts = [];
    for (var i = 0; i < g.total; i++) {
        var amt = modAmount(m, i);
        var y = g.yTop + amt * g.H;
        slotPts.push([g.x0 + i * g.sw, y], [g.x0 + (i + 1) * g.sw, y]);
    }
    if (slotPts.length) {
        var line = 'M' + slotPts[0][0] + ',' + slotPts[0][1];
        for (var k = 1; k < slotPts.length; k++) line += ' L' + slotPts[k][0] + ',' + slotPts[k][1];
        // Fill: staircase down to the baseline and back
        var last = slotPts[slotPts.length - 1];
        var fill = line + ' L' + last[0] + ',' + g.yTop + ' L' + slotPts[0][0] + ',' + g.yTop + ' Z';
        out.push('<path class="ap-env-fill' + (off ? ' off' : '') + '" d="' + fill + '"/>');
        out.push('<path class="ap-curve env' + (off ? ' off' : '') + '" d="' + line + '"/>');
    }

    // Amplitude fader — left gutter, vertical. Grab and move up (off) / down
    // (deeper). Kept off the shape so it never collides with the corners.
    out.push('<line class="ap-amp-track" x1="' + AMP_X + '" y1="' + g.yTop + '" x2="' + AMP_X + '" y2="' + (g.yTop + g.H) + '"/>');
    out.push('<circle class="ap-halo" cx="' + AMP_X + '" cy="' + g.yBot + '" r="' + (HIT_R - 10) + '"/>');
    out.push('<circle class="ap-handle' + (dragH === 'amp' ? ' drag' : '') + '" cx="' + AMP_X + '" cy="' + g.yBot + '" r="' + HANDLE_R + '"/>');
    out.push('<text class="ap-val' + (dragH === 'amp' ? ' drag' : '') + '" x="' + (g.x0 + 2) + '" y="' + (PAD_TOP - 6) + '" text-anchor="start">amp ' + m.amplitude + (off ? ' · off' : '') + '</text>');

    // Corner handles: each stretches one segment (drag horizontally).
    var corner = function (name, x, y) {
        out.push('<circle class="ap-halo" cx="' + x + '" cy="' + y + '" r="' + (HIT_R - 8) + '"/>');
        out.push('<circle class="ap-handle' + (dragH === name ? ' drag' : '') + '" cx="' + x + '" cy="' + y + '" r="' + HANDLE_R + '"/>');
    };
    corner('in',   g.xIn,   g.yBot);
    corner('hold', g.xHold, g.yBot);
    corner('out',  g.xOut,  g.yTop);
    corner('rest', g.xEnd,  g.yTop);

    // Offset track + draggable marker (phase-shifts which stroke the cycle
    // starts on) — a dedicated control, no more "drag the background".
    out.push('<line class="ap-track" x1="' + g.x0 + '" y1="' + g.trackY + '" x2="' + (w - PAD_X) + '" y2="' + g.trackY + '"/>');
    var offX = g.x0 + (g.total > 0 ? (m.offset % g.total) / g.total : 0) * g.W;
    out.push('<circle class="ap-halo" cx="' + offX + '" cy="' + g.trackY + '" r="' + (HIT_R - 10) + '"/>');
    out.push('<path class="ap-off-marker' + (dragH === 'offset' ? ' drag' : '') + '" d="M' + offX + ',' + (g.trackY - 6) + ' l5,9 l-10,0 Z"/>');
    out.push('<text class="ap-off-lbl" x="' + g.x0 + '" y="' + (g.trackY - 10) + '" text-anchor="start">offset ' + m.offset + ' — cycle start</text>');

    svg.innerHTML = out.join('');
}

function renderChips() {
    var chips = $('apModChips');
    if (!chips) return;
    chips.querySelectorAll('.ap-chip').forEach(function (b) {
        var i = parseInt(b.dataset.ctrl);
        b.classList.toggle('active', i === S.ctrl);
        var m = null;
        if (S.mods) S.mods.forEach(function (x) { if (x && x.ctrl === i) m = x; });
        b.classList.toggle('modded', !!(m && m.amplitude < 100));
    });
}

// ============================================================================
// Pointer interaction
// ============================================================================

function nearest(pt, candidates) {
    var best = null, bestD = HIT_R;
    candidates.forEach(function (c) {
        var d = Math.hypot(pt.x - c.pt.x, pt.y - c.pt.y);
        if (d < bestD) { bestD = d; best = c.name; }
    });
    return best;
}

function onDown(surface, st, e) {
    if (!S.seeded) return;
    var pt = svgPoint(st, e);

    if (surface === 'wave') {
        var g = waveGeom();
        var hs = waveHandles(g);
        var hit = nearest(pt, [
            { name: 'max', pt: hs.max }, { name: 'min', pt: hs.min },
            { name: 'vin', pt: hs.vin }, { name: 'vout', pt: hs.vout },
            { name: 'ain', pt: hs.ain }, { name: 'aout', pt: hs.aout }
        ]);
        if (!hit) return;
        S.drag = { surface: 'wave', handle: hit, x0: pt.x, y0: pt.y };
        // Delta-mapped handles capture their start value on grab so the value
        // never jumps to the pointer — you nudge from where it already is.
        if (hit === 'ain') {
            S.drag.d0 = Math.hypot(pt.x - g.x0, pt.y - g.yMin);
            S.drag.v0 = S.base.in_accel;
        } else if (hit === 'aout') {
            S.drag.d0 = Math.hypot(pt.x - g.x2, pt.y - g.yMin);
            S.drag.v0 = S.base.out_accel;
        } else if (hit === 'vin') {
            S.drag.v0 = S.base.in_speed;
        } else if (hit === 'vout') {
            S.drag.v0 = S.base.out_speed;
        }
    } else {
        var m = selMod();
        if (!m) return;
        var g2 = envGeom(m);
        var hit2 = nearest(pt, [
            { name: 'amp',  pt: { x: AMP_X,    y: g2.yBot } },
            { name: 'in',   pt: { x: g2.xIn,   y: g2.yBot } },
            { name: 'hold', pt: { x: g2.xHold, y: g2.yBot } },
            { name: 'out',  pt: { x: g2.xOut,  y: g2.yTop } },
            { name: 'rest', pt: { x: g2.xEnd,  y: g2.yTop } }
        ]);
        // Offset marker on the lower track
        if (!hit2) {
            var offX = g2.x0 + (g2.total > 0 ? (m.offset % g2.total) / g2.total : 0) * g2.W;
            if (Math.abs(pt.x - offX) < HIT_R && Math.abs(pt.y - g2.trackY) < HIT_R) hit2 = 'offset';
        }
        if (!hit2) return;   // empty space no longer starts an offset drag
        // Freeze the slot width for the whole drag so the staircase doesn't
        // rescale under the pointer as the count changes.
        S.drag = { surface: 'env', handle: hit2, x0: pt.x, v0: m.offset, sw: g2.sw };
    }

    st.svg.setPointerCapture(e.pointerId);
    e.preventDefault();
    scheduleRender();
}

function onMove(surface, st, e) {
    if (!S.drag || S.drag.surface !== surface) return;
    var pt = svgPoint(st, e);
    if (surface === 'wave') dragWave(pt);
    else dragEnv(pt);
    scheduleRender();
}

function dragWave(pt) {
    var b = S.base;
    var g = waveGeom();
    var depthOf = function (y) {
        return clamp(Math.round((1 - (y - PAD_TOP) / g.H) * 100), 0, 100);
    };
    switch (S.drag.handle) {
        case 'max': b.max_depth = clamp(depthOf(pt.y), b.min_depth, 100); break;
        case 'min': b.min_depth = clamp(depthOf(pt.y), 0, b.max_depth); break;
        // Speed handles: the grabbed handle's edge midpoint tracks the pointer
        // (fracIn = 2·(pointer − corner)/W), and that direction's speed is
        // derived from the split while the OTHER speed stays fixed — decoupled
        // in value, the handle follows the mouse, and the curve stays full-width.
        case 'vin': {
            var fracIn = clamp((pt.x - g.x0) * 2 / g.W, 0.15, 0.85);
            var rOut = edgeWeight(b.out_speed);
            b.in_speed = speedFromWeight(rOut * fracIn / (1 - fracIn));
            break;
        }
        case 'vout': {
            var fracOut = clamp((g.x2 - pt.x) * 2 / g.W, 0.15, 0.85);
            var rIn = edgeWeight(b.in_speed);
            b.out_speed = speedFromWeight(rIn * fracOut / (1 - fracOut));
            break;
        }
        case 'ain': {
            // Radial delta from grab: toward the corner = sharper (punchier).
            var d1 = Math.hypot(pt.x - g.x0, pt.y - g.yMin);
            b.in_accel = clamp(Math.round(S.drag.v0 + (S.drag.d0 - d1) / 60 * 100), 0, 100);
            break;
        }
        case 'aout': {
            var d2 = Math.hypot(pt.x - g.x2, pt.y - g.yMin);
            b.out_accel = clamp(Math.round(S.drag.v0 + (S.drag.d0 - d2) / 60 * 100), 0, 100);
            break;
        }
    }
    S.cb.onLive(getApEditorBaseFields());
}

function dragEnv(pt) {
    var m = selMod();
    if (!m) return;
    var g = envGeom(m, S.drag.sw);
    // Each corner sets ONE segment's stroke count = distance from the previous
    // corner, in slot widths. Upstream corners don't include the count being
    // edited, so the math converges frame to frame.
    var span = function (xa) { return clamp(Math.round((pt.x - xa) / g.sw), 0, 25); };
    switch (S.drag.handle) {
        case 'in':   m.in_step  = clamp(span(g.x0),   1, 25); break;
        case 'hold': m.in_wait  = span(g.xIn);                break;
        case 'out':  m.out_step = clamp(span(g.xHold),1, 25); break;
        case 'rest': m.out_wait = span(g.xOut);              break;
        case 'amp': {
            // yBot = PAD_TOP + ratio·H ; ratio 0(top)=amp100, 1(bottom)=amp0
            var frac = clamp((pt.y - PAD_TOP) / g.H, 0, 1);
            m.amplitude = clamp(Math.round(100 - frac * 100), 0, 100);
            break;
        }
        case 'offset': {
            var dSlots = Math.round((pt.x - S.drag.x0) / g.sw);
            m.offset = clamp(S.drag.v0 + dSlots, 0, 100);
            break;
        }
    }
    S.cb.onLive({ ap_mod: {
        ctrl: S.ctrl, amplitude: m.amplitude, in_step: m.in_step, in_wait: m.in_wait,
        out_step: m.out_step, out_wait: m.out_wait, offset: m.offset
    }});
    renderChips();
}

function onUp(e) {
    if (!S.drag) return;
    S.drag = null;
    scheduleRender();
    // Reconcile with the device's post-clamp truth (owner GETs /api/pattern
    // and calls setApEditorState) — the purple desired state converges to
    // confirmed blue here, never the other way round.
    S.cb.onCommit();
}
