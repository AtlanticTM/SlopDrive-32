/**
 * Pattern Engine panel — Pattern (01).
 *
 * Registry-driven glyph tile grid (§2.1a/b), speed/depth/stroke/sensation
 * sliders with .vv readouts, start/stop through cmd.js (gen_cfg/gen_run
 * ops), and a recessed .screen wave scope canvas tracing the REAL commanded
 * position off telebuf (not a synthetic waveform — see the wave-scope
 * section below for the R2 finding/fix).
 *
 * The wave scope: --line-3 at standby (flat — no data flowing), --reality +
 * bloom when running, with a leading dot at the write head. The rAF/canvas
 * loop is allocation-free (Task 3R discipline applies).
 */
import { $, setRead, icon, toast, pad, setVV } from "../core/ui.js";
import { post, get } from "../core/api.js";
import * as cmd from "../core/cmd.js";
import { OP_GEN_CFG, OP_GEN_RUN } from "../core/wire.js";
import { TRAVEL, winMin, winMax } from "../core/range.js";
import { capsCache } from "../core/capabilities.js";
import { sampleAt, stableRenderTime } from "../core/telebuf.js";
import { ACCENT } from "../core/theme.js";

export let pat = { running: false };

// ===================== Pattern registry (§2.1b) =====================
// Runtime list, never a hardcoded DOM — the grid renders from whichever
// source is present. capabilities.js's cached caps object may eventually
// advertise a `patterns` array ({id, name, glyph?}); when absent (true of
// the current firmware — this IS the live V1 test per the spec) we fall
// back to the built-in seven with the approved-mock glyph set. An entry
// with no glyph renders the dashed ghost tile. This decouples the future
// generator-library swap from all UI work — the grid just handles any count.
var FALLBACK_PATTERN_REGISTRY = [
    { id: 0, name: 'Simple', tip: 'Smooth sine strokes',                glyph: 'M2,11 C8,2 14,2 20,11 S32,20 38,11 S50,2 56,11' },
    { id: 1, name: 'Tease',  tip: 'Long dwell, sharp stroke',           glyph: 'M2,16 L14,16 L20,4 L26,16 L38,16 L44,4 L50,16 L56,16' },
    { id: 2, name: 'Robo',   tip: 'Square in-out holds',                glyph: 'M2,16 L10,16 L10,4 L22,4 L22,16 L34,16 L34,4 L46,4 L46,16 L56,16' },
    { id: 3, name: 'Half',   tip: 'Alternating half and full strokes',  glyph: 'M2,10 C6,6 10,6 14,10 S22,14 26,10 C32,2 40,2 46,10 S54,16 56,12' },
    { id: 4, name: 'Deeper', tip: 'Each stroke reaches further',        glyph: 'M2,12 C6,9 10,9 14,12 C19,7 25,7 30,12 C36,4 44,4 50,12 L56,12' },
    { id: 5, name: 'StopGo', tip: 'Bursts with pauses',                 glyph: 'M2,11 C6,4 10,4 14,11 L26,11 C30,4 34,4 38,11 L50,11 C53,7 55,7 56,9' },
    { id: 6, name: 'Insist', tip: 'Dense relentless rhythm',            glyph: 'M2,11 C5,3 8,3 11,11 S17,19 20,11 S26,3 29,11 S35,19 38,11 S44,3 47,11 S53,19 56,11' }
];
var GHOST_GLYPH = 'M2,11 L8,11 M14,11 L20,11 M26,11 L32,11 M38,11 L44,11 M50,11 L56,11';

function patternRegistry() {
    if (capsCache && Array.isArray(capsCache.patterns) && capsCache.patterns.length) return capsCache.patterns;
    return FALLBACK_PATTERN_REGISTRY;
}

// Pattern names — kept for the hidden #patSelect <option> labels only.
var PATTERN_NAMES = [
    'Simple', 'Tease', 'Robo', 'Half\'n\'Half', 'Deeper', 'Stop\'n\'Go', 'Insist'
];

function patPayload() {
    return {
        speed:     parseInt($("patSpeed").value),
        depth:     parseInt($("patDepth").value),
        stroke:    parseInt($("patStroke").value),
        sensation: parseInt($("patSensation").value),
        pattern:   parseInt($("patSelect").value)
    };
}

let pp = null;
export function pushPatParams() {
    clearTimeout(pp);
    pp = setTimeout(function () {
        cmd.send(OP_GEN_CFG, patPayload());
    }, 60);
}

// ===================== Wave scope — telemetry-fed (§2.1d / R2) =====================
// R2 FINDING: the prior implementation computed a pure synthetic waveform
// from slider values (waveformSample/trapezoid below), never touching
// telemetry — a Ground Truth Doctrine violation. Rewired to draw the REAL
// commanded ("told") position: a trailing-window ring buffer sampled once
// per rAF tick from the same telebuf.sampleAt()/stableRenderTime() exports
// main.js's rail loop already calls (telebuf INTERNALS stay untouched —
// only its public API is used, same as main.js does). Flat centered line
// when nothing is commanding movement (idle naturally holds a flat tgt, no
// special-cased "standby" branch needed); glow only while pat.running.
// Allocation-free: fixed Float64Array ring, no per-frame object/array
// literals, canvas state set once, rect cached on resize.

var SCOPE_RING_N = 300; // ~5s trailing window at 60fps

var _scope = {
    cv: null, ctx: null, dpr: 1,
    rect: { w: 0, h: 0 },
    animRunning: false,
    reducedMotion: false,
    ringX: new Float64Array(SCOPE_RING_N), // normalized -1..1, oldest..newest
    ringHead: 0, ringLen: 0,
    // Pre-allocated dash arrays
    DASH_NONE: [],
};

function initWaveScope() {
    _scope.cv = $('#waveScopeCanvas');
    if (!_scope.cv) return;
    _scope.ctx = _scope.cv.getContext('2d');
    _scope.reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    sizeScopeCanvas();
    window.addEventListener('resize', sizeScopeCanvas);
    if (typeof ResizeObserver !== 'undefined') {
        var ro = new ResizeObserver(function() { sizeScopeCanvas(); });
        ro.observe(_scope.cv);
    }
    startScopeLoop();
}

function sizeScopeCanvas() {
    if (!_scope.cv) return;
    var w = _scope.cv.clientWidth;
    var h = _scope.cv.clientHeight;
    _scope.dpr = window.devicePixelRatio || 1;
    var cw = Math.round(w * _scope.dpr);
    var ch = Math.round(h * _scope.dpr);
    if (_scope.cv.width !== cw || _scope.cv.height !== ch) {
        _scope.cv.width = cw;
        _scope.cv.height = ch;
    }
    _scope.rect.w = w;
    _scope.rect.h = h;
}

// Push the current commanded position (telebuf's "told" channel) into the
// trailing ring, normalized to the stroke window (-1..1, 0 = window center).
function pushScopeSample(tgt) {
    var mid = (winMin + winMax) / 2;
    var half = (winMax - winMin) / 2 || 1;
    var norm = Math.max(-1, Math.min(1, (tgt - mid) / half));
    var idx = (_scope.ringHead + _scope.ringLen) % SCOPE_RING_N;
    if (_scope.ringLen === SCOPE_RING_N) {
        _scope.ringHead = (_scope.ringHead + 1) % SCOPE_RING_N;
        _scope.ringLen--;
        idx = (_scope.ringHead + _scope.ringLen) % SCOPE_RING_N;
    }
    _scope.ringX[idx] = norm;
    _scope.ringLen++;
}

function startScopeLoop() {
    if (_scope.animRunning) return;
    _scope.animRunning = true;
    requestAnimationFrame(scopeFrame);
}

var _scopeWasRunning = false;
function scopeFrame(nowMs) {
    _scope.animRunning = true;
    if (!_scope.ctx || _scope.rect.w === 0) {
        requestAnimationFrame(scopeFrame);
        return;
    }

    // Trace ONLY while the pattern engine is running. The told channel also
    // moves during TCode/MFP streaming, but tracing that here was redundant
    // (the plan strip under the rail is the streaming diagnostics surface)
    // and made "wave · told" read as a second, confusing stream monitor.
    // This scope is the PATTERN's preview — flat in every other state.
    if (pat.running) {
        var s = sampleAt(stableRenderTime(nowMs));
        if (s && typeof s.tgt === 'number') pushScopeSample(s.tgt);
        _scopeWasRunning = true;
    } else if (_scopeWasRunning) {
        // Pattern just stopped — clear the ring once so the trace collapses
        // to the flat standby line instead of freezing mid-waveform.
        _scope.ringHead = 0;
        _scope.ringLen = 0;
        _scopeWasRunning = false;
    }

    drawScope();
    requestAnimationFrame(scopeFrame);
}

// Pre-allocated canvas style constants
var SCOPE_LINE_DIM = '#33373E';   // --line-3 (neutral — identical across themes)
var SCOPE_FILL = '#000';

function drawScope() {
    var ctx = _scope.ctx;
    var w = _scope.rect.w;
    var h = _scope.rect.h;

    ctx.setTransform(_scope.dpr, 0, 0, _scope.dpr, 0, 0);

    // Clear
    ctx.fillStyle = SCOPE_FILL;
    ctx.globalAlpha = 1;
    ctx.globalCompositeOperation = 'source-over';
    ctx.clearRect(0, 0, w, h);

    // Grid: horizontal lines at 25%, 50%, 75%
    ctx.strokeStyle = 'rgba(51,55,62,0.3)';
    ctx.lineWidth = 0.5;
    ctx.setLineDash(_scope.DASH_NONE);
    for (var frac = 0.25; frac < 1; frac += 0.25) {
        var y = h - frac * h;
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(w, y);
        ctx.stroke();
    }

    var isRunning = pat.running;
    var lineColor = isRunning ? ACCENT.reality : SCOPE_LINE_DIM;

    ctx.strokeStyle = lineColor;
    ctx.lineWidth = 1.5;
    ctx.lineJoin = 'round';

    if (isRunning) {
        ctx.shadowColor = ACCENT.reality;
        ctx.shadowBlur = 8;
    } else {
        ctx.shadowBlur = 0;
    }

    var len = _scope.ringLen;
    var lastX = w / 2, lastY = h / 2;
    if (len > 1) {
        var span = len - 1;
        ctx.beginPath();
        for (var i = 0; i < len; i++) {
            var idx = (_scope.ringHead + i) % SCOPE_RING_N;
            var x = (i / span) * w;
            var y = h / 2 - _scope.ringX[idx] * (h / 2 - 4);
            if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
            lastX = x; lastY = y;
        }
        ctx.stroke();
    } else {
        // No samples yet — flat centered line (standby, matches R2: never a
        // synthetic approximation, just the honest "no data" state).
        ctx.beginPath();
        ctx.moveTo(0, h / 2);
        ctx.lineTo(w, h / 2);
        ctx.stroke();
    }

    // Leading dot at the write head (rightmost sample) when running
    if (isRunning && len > 0) {
        ctx.shadowBlur = 12;
        ctx.fillStyle = ACCENT.reality;
        ctx.beginPath();
        ctx.arc(lastX, lastY, 3, 0, Math.PI * 2);
        ctx.fill();
    }

    ctx.shadowBlur = 0;
}

// ===================== Pattern glyph tile grid (§2.1a/b) =====================
// Renders from patternRegistry() — a runtime list, never a hardcoded DOM.
// Handles any count (wraps to new 4-wide rows via CSS grid). Click still
// drives the exact same patPayload()/pushPatParams()/hidden #patSelect sync
// path the old segmented control used — no new endpoint (R4).

function selectedPatternId() {
    var sel = $('#patSelect');
    var v = sel ? parseInt(sel.value) : 0;
    return isNaN(v) ? 0 : v;
}

function buildPatternGrid() {
    var host = $('#patGrid');
    if (!host) return;
    var registry = patternRegistry();
    var selected = selectedPatternId();
    host.innerHTML = '';
    registry.forEach(function (p) {
        var tile = document.createElement('button');
        tile.type = 'button';
        tile.className = 'pat-tile' + (p.id === selected ? ' on' : '') + (!p.glyph ? ' ghost' : '');
        tile.dataset.pat = p.id;
        tile.setAttribute('data-tip', p.tip || p.name);
        var glyphPath = p.glyph || GHOST_GLYPH;
        tile.innerHTML = '<svg viewBox="0 0 58 20" aria-hidden="true"><path d="' + glyphPath + '"/></svg><span>' + p.name + '</span>';
        host.appendChild(tile);
    });
    // Trailing "from fw" ghost tile while running on the built-in fallback
    // set — advertises that the grid is firmware-extensible (mock r6's .pg
    // .ghost). Non-interactive: a <div> with no data-pat, so the grid click
    // handler's parseInt guard ignores it. Dropped automatically the day the
    // firmware actually advertises its own `patterns` registry.
    if (registry === FALLBACK_PATTERN_REGISTRY) {
        var ghost = document.createElement('div');
        ghost.className = 'pat-tile ghost';
        ghost.setAttribute('data-tip', 'Patterns the firmware advertises appear here automatically — the grid grows with the generator library');
        ghost.innerHTML = '<svg viewBox="0 0 58 20" aria-hidden="true"><path d="' + GHOST_GLYPH + '"/></svg><span>from fw</span>';
        host.appendChild(ghost);
    }
}

/** Re-render the grid — called once at init and again once capabilities.js
 *  resolves /api/capabilities, in case the firmware advertises `patterns`. */
export function rebuildPatternGrid() {
    buildPatternGrid();
}

function initPatternGrid() {
    buildPatternGrid();
    var host = $('#patGrid');
    if (!host) return;
    host.addEventListener('click', function (e) {
        var tile = e.target.closest('.pat-tile');
        if (!tile) return;
        var id = parseInt(tile.dataset.pat);
        if (isNaN(id)) return; // "from fw" ghost tile — display-only
        host.querySelectorAll('.pat-tile').forEach(function (t) { t.classList.toggle('on', t === tile); });
        var sel = $('#patSelect');
        if (sel) sel.value = id;
        pushPatParams();
        updatePatState();
    });
}

// seedPatSlider — set a generator slider's value from a REAL device number,
// refresh its readout, and un-gate it. The markup ships every generator slider
// `disabled` with a `—` readout so NOTHING is prepopulated with an invented
// number on boot; the field only comes alive once the firmware's own value
// lands here. If the field is genuinely absent the slider stays gated/blank. :3
function seedPatSlider(id, readId, val) {
    var e = $(id); if (!e) return;
    if (typeof val !== 'number') return;   // no device truth → leave gated/blank
    e.value = val;
    e.disabled = false;
    // Zero-padded 3-digit mono chip ("050"), matching the mock's .fv format.
    var r = $('#' + readId); if (r) r.textContent = pad(Math.round(val), 3, 0);
}

// Pull the generator's live params from the device and seed the sliders. Runs
// once on init so the Generator card reflects machine truth instead of the
// retired hardcoded 0/100/100/50 markup values. :3
export async function loadPatternParams() {
    try {
        const d = await get('/api/pattern');
        if (!d) return;
        seedPatSlider('patSpeed',     'patSpeedVal', typeof d.speed === 'number' ? d.speed : undefined);
        seedPatSlider('patDepth',     'patDepthVal', typeof d.depth === 'number' ? d.depth : undefined);
        seedPatSlider('patStroke',    'patStrokeVal', typeof d.stroke === 'number' ? d.stroke : undefined);
        seedPatSlider('patSensation', 'patSensVal', typeof d.sensation === 'number' ? d.sensation : undefined);
        // Reflect the selected pattern in the hidden select + glyph grid.
        if (typeof d.pattern === 'number') {
            var sel = $('#patSelect'); if (sel) sel.value = d.pattern;
            var host = $('#patGrid');
            if (host) host.querySelectorAll('.pat-tile').forEach(function(t) {
                t.classList.toggle('on', parseInt(t.dataset.pat) === d.pattern);
            });
        }
    } catch (e) {}
}

export function initPattern() {
    // Build the pattern glyph tile grid
    initPatternGrid();

    // Seed the generator sliders from device truth (un-gates them).
    loadPatternParams();

    // Slider inputs — push params on drag. Explicit slider→chip id map: the
    // old `id + "Val"` convention silently broke for patSensation (chip id
    // is patSensVal, not patSensationVal), so the Sensation readout never
    // updated during a drag — only on the next full param reload.
    [["patSpeed", "patSpeedVal"], ["patDepth", "patDepthVal"],
     ["patStroke", "patStrokeVal"], ["patSensation", "patSensVal"]].forEach(function (pair) {
        var s = $(pair[0]);
        if (!s) return;
        s.addEventListener("input", function () {
            setRead(pair[1], pad(parseInt(s.value) || 0, 3, 0));
            pushPatParams();
        });
    });

    // Hidden select (kept for patPayload compat, hidden by CSS)
    var sel = $("patSelect");
    if (sel) {
        sel.addEventListener("change", function () {
            pushPatParams();
        });
    }

    // Start/stop button
    var btn = $("patStartBtn");
    if (btn) btn.addEventListener("click", togglePattern);

    // Init wave scope
    initWaveScope();
}

export async function startPattern() {
    if (pat.running) return;
    cmd.send(OP_GEN_RUN, { run: true });
    const r = await post("/api/pattern", Object.assign(patPayload(), { running: true }));
    if (!r) { toast("Pattern start failed — no response", "bad", "i-alert"); return; }
    const d = await r.json();
    if (d && d.ok && d.running) {
        pat.running = true;
    } else {
        pat.running = false;
        toast(d && d.error ? "Pattern start rejected: " + d.error : "Pattern start failed", "bad", "i-alert");
        return;
    }
    setPatternButton();
}

export async function stopPattern() {
    if (!pat.running) return;
    cmd.send(OP_GEN_RUN, { run: false });
    const r = await post("/api/pattern", { running: false });
    if (!r) { toast("Pattern stop failed — no response", "warn", "i-alert"); return; }
    const d = await r.json();
    if (d && d.ok && !d.running) {
        pat.running = false;
    } else {
        // still set false optimistically — stop is safer than stale-running
        pat.running = false;
    }
    setPatternButton();
}

// Card-head status sub-label (§2.1f) — replaces the old #patNote hint block.
// "standby" / "running · <name>", sourced from confirmed pat.running + the
// selected registry entry — existing state, just relocated.
function updatePatState() {
    var el = $('#patState');
    if (!el) return;
    if (pat.running) {
        var id = selectedPatternId();
        var entry = patternRegistry().filter(function (p) { return p.id === id; })[0];
        el.textContent = 'running · ' + (entry ? entry.name.toLowerCase() : id);
    } else {
        el.textContent = 'standby';
    }
}

// Idle: neutral border "▶ START PATTERN". Running: amber border/glow
// "■ STOP PATTERN" (.running class — amber = attention; NEVER .danger's
// red, which is reserved for hazard/e-stop contexts per R1). Exported so
// main.js's 0x01-telemetry-flag handler (the actual confirmed-state path,
// V2) can call the same reflection instead of keeping its own duplicate.
export function setPatternButton() {
    const btn = $("patStartBtn");
    if (btn) {
        btn.innerHTML = pat.running ? icon("i-stop") + " STOP PATTERN" : icon("i-play") + " START PATTERN";
        btn.classList.toggle("primary", !pat.running);
        btn.classList.toggle("running", pat.running);
    }
    updatePatState();
}

export function togglePattern() {
  pat.running ? stopPattern() : startPattern();
}

/** Adopt device pattern-running state on init/reconnect without dispatching commands. */
export async function refreshPatternState() {
  try {
    const d = await get('/api/pattern');
    if (d && typeof d.running === 'boolean' && d.running !== pat.running) {
      pat.running = d.running;
      setPatternButton();
    }
  } catch (e) {}
}
