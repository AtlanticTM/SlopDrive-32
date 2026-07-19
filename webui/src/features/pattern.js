/**
 * Pattern Engine panel — Generator 01.
 *
 * Pattern select as segmented control, speed/depth/stroke/sensation sliders
 * with .vv readouts, start/stop through cmd.js (gen_cfg/gen_run ops), and
 * a recessed .screen wave scope canvas previewing the generator waveform.
 *
 * The wave scope traces the pattern waveform: --line-3 at standby,
 * --reality + bloom when running, with a leading dot at the write head.
 * The rAF/canvas loop is allocation-free (Task 3R discipline applies).
 */
import { $, setRead, icon, toast, pad, setVV } from "../core/ui.js";
import { post, get } from "../core/api.js";
import * as cmd from "../core/cmd.js";
import { OP_GEN_CFG, OP_GEN_RUN } from "../core/wire.js";
import { TRAVEL, winMin, winMax } from "../core/range.js";

export let pat = { running: false };

// Pattern names for the segmented control
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

// ===================== Wave scope =====================
// Recessed .screen canvas previewing the generator waveform.
// Allocation-free rAF loop: no per-frame object/array literals, canvas state
// set once, rect cached on resize.

var _scope = {
    cv: null, ctx: null, dpr: 1,
    rect: { w: 0, h: 0 },
    animRunning: false,
    reducedMotion: false,
    // Waveform phase (advances when running)
    phase: 0,
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

// Compute a waveform sample at phase t (0..1) for the given pattern index.
// Returns 0..1 (normalized position within the stroke window).
function waveformSample(t, patternIndex, sensation) {
    var s = sensation / 100; // -1..1

    switch (patternIndex) {
        case 0: // Simple Stroke — trapezoidal, 1/3 accel, 1/3 coast, 1/3 decel
            return trapezoid(t, 1/3, 1/3);
        case 1: // TeasingPounding — asymmetric in/out speeds
            var ratio = s > 0 ? 1 / (1 + s * 4) : (1 + (-s) * 4) / (1 + (-s) * 4 + 1);
            return trapezoid(t, ratio * 0.5, ratio * 0.5);
        case 2: // RoboStroke — variable accel/decel ratio
            var x = s >= 0 ? (1/3 + s * (0.5 - 1/3)) : (1/3 + (-s) * (0.05 - 1/3));
            return trapezoid(t, x, 1 - x);
        case 3: // Half'n'Half — every second stroke is half depth
            var half = Math.floor(t * 2) % 2 === 0;
            var localT = (t * 2) % 1;
            return half ? trapezoid(localT, 1/3, 1/3) * 0.5 : trapezoid(localT, 1/3, 1/3);
        case 4: // Deeper — ramping amplitude
            var cycle = Math.floor(t * 4) % 4;
            var amp = (cycle + 1) / 4;
            var localT = (t * 4) % 1;
            return trapezoid(localT, 1/3, 1/3) * amp;
        case 5: // Stop'n'Go — pauses between stroke series
            var inPause = (t * 3) % 1 > 0.7;
            if (inPause) return 0;
            return trapezoid((t * 3) % 1 / 0.7, 1/3, 1/3);
        case 6: // Insist — fractional stroke, vibrational
            var frac = (100 - Math.abs(sensation)) / 100;
            return trapezoid(t, 0.15, 0.15) * frac + (1 - frac) * 0.5;
        default:
            return trapezoid(t, 1/3, 1/3);
    }
}

// Trapezoidal waveform: accel for `accel` fraction, coast, decel for `decel` fraction.
// Returns 0..1 (0 = out, 1 = in).
function trapezoid(t, accel, decel) {
    var half = t % 1;
    var goingIn = half < 0.5;
    var localT = goingIn ? half * 2 : (half - 0.5) * 2; // 0..1 for each half
    var pos;
    if (localT < accel) {
        pos = 0.5 * (localT / accel);
    } else if (localT < 1 - decel) {
        pos = 0.5;
    } else {
        pos = 0.5 + 0.5 * ((localT - (1 - decel)) / (decel || 0.001));
    }
    return goingIn ? pos : 1 - pos;
}

var _lastScopeFrameTs = 0;

function startScopeLoop() {
    if (_scope.animRunning) return;
    _scope.animRunning = true;
    requestAnimationFrame(scopeFrame);
}

function scopeFrame(nowMs) {
    _scope.animRunning = true;
    if (!_scope.ctx || _scope.rect.w === 0) {
        requestAnimationFrame(scopeFrame);
        return;
    }

    var dtMs = nowMs - _lastScopeFrameTs;
    if (dtMs <= 0 || dtMs > 500) dtMs = 16.667;
    _lastScopeFrameTs = nowMs;

    // Advance phase when running
    if (pat.running) {
        var speed = parseInt($('#patSpeed') ? $('#patSpeed').value : 0);
        // Speed slider 0-100 maps to stroke period ~0.3s..5s. At speed 0 the
        // generator idles at a standstill (period → very long) and ramps up
        // from nothing — no forced 50% floor. :3
        var periodMs = 5000 - speed * 45; // 5000ms at 0, 500ms at 100
        _scope.phase += dtMs / periodMs;
        if (_scope.phase > 1) _scope.phase -= 1;
    }

    drawScope(nowMs);
    requestAnimationFrame(scopeFrame);
}

// Pre-allocated canvas style constants
var SCOPE_LINE_DIM = '#33373E';   // --line-3
var SCOPE_LINE_LIVE = '#4DA6FF';  // --reality
var SCOPE_FILL = '#000';

function drawScope(nowMs) {
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

    // Draw waveform
    var patternIndex = parseInt($('#patSelect') ? $('#patSelect').value : 0);
    var sensation = parseInt($('#patSensation') ? $('#patSensation').value : 50);
    var isRunning = pat.running;
    var lineColor = isRunning ? SCOPE_LINE_LIVE : SCOPE_LINE_DIM;

    ctx.strokeStyle = lineColor;
    ctx.lineWidth = 1.5;
    ctx.lineJoin = 'round';

    if (isRunning) {
        ctx.shadowColor = SCOPE_LINE_LIVE;
        ctx.shadowBlur = 8;
    } else {
        ctx.shadowBlur = 0;
    }

    // Draw 2 cycles of the waveform
    var N = Math.max(w, 80);
    ctx.beginPath();
    for (var i = 0; i <= N; i++) {
        var x = (i / N) * w;
        // Map x to phase: show 2 cycles
        var t = (i / N) * 2 + _scope.phase;
        var sample = waveformSample(t, patternIndex, sensation);
        var py = h - sample * h;
        if (i === 0) ctx.moveTo(x, py);
        else ctx.lineTo(x, py);
    }
    ctx.stroke();

    // Leading dot at write head (right edge) when running
    if (isRunning) {
        ctx.shadowBlur = 12;
        ctx.fillStyle = SCOPE_LINE_LIVE;
        var headT = 1 + _scope.phase;
        var headSample = waveformSample(headT, patternIndex, sensation);
        var headY = h - headSample * h;
        ctx.beginPath();
        ctx.arc(w - 4, headY, 3, 0, Math.PI * 2);
        ctx.fill();
    }

    ctx.shadowBlur = 0;
}

// ===================== Pattern select segmented control =====================

function buildPatternSeg() {
    var host = $('#patSelectSeg');
    if (!host) return;
    host.innerHTML = '';
    for (var i = 0; i < PATTERN_NAMES.length; i++) {
        var btn = document.createElement('button');
        btn.dataset.pat = i;
        btn.textContent = PATTERN_NAMES[i];
        if (i === 0) btn.classList.add('active');
        host.appendChild(btn);
    }
    host.addEventListener('click', function(e) {
        var b = e.target.closest('button');
        if (!b) return;
        host.querySelectorAll('button').forEach(function(x) { x.classList.remove('active'); });
        b.classList.add('active');
        // Update hidden select for patPayload compat
        var sel = $('#patSelect');
        if (sel) sel.value = b.dataset.pat;
        pushPatParams();
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
    var r = $('#' + readId); if (r) r.textContent = Math.round(val);
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
        // Reflect the selected pattern in the hidden select + segmented control.
        if (typeof d.pattern === 'number') {
            var sel = $('#patSelect'); if (sel) sel.value = d.pattern;
            var host = $('#patSelectSeg');
            if (host) host.querySelectorAll('button').forEach(function(b) {
                b.classList.toggle('active', parseInt(b.dataset.pat) === d.pattern);
            });
        }
    } catch (e) {}
}

export function initPattern() {
    // Build segmented control
    buildPatternSeg();

    // Seed the generator sliders from device truth (un-gates them).
    loadPatternParams();

    // Slider inputs — push params on drag
    ["patSpeed", "patDepth", "patStroke", "patSensation"].forEach(function (id) {
        var s = $(id);
        if (!s) return;
        s.addEventListener("input", function () {
            setRead(id + "Val", s.value);
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
    const note = $("patNote");
    if (note) note.textContent = "Pattern running on the device — keeps going even if you close this tab.";
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
    const note = $("patNote");
    if (note) note.textContent = "Patterns run on-device — they keep going even if you close this tab. Stop before switching patterns.";
}

function setPatternButton() {
    const btn = $("patStartBtn");
    if (!btn) return;
    btn.innerHTML = pat.running ? icon("i-stop") + " Stop Pattern" : icon("i-play") + " Start Pattern";
    btn.classList.toggle("primary", !pat.running);
    btn.classList.toggle("danger", pat.running);
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
      const btn = $('#patStartBtn');
      if (btn) {
        btn.innerHTML = pat.running ? icon('i-stop') + ' Stop Pattern' : icon('i-play') + ' Start Pattern';
        btn.classList.toggle('primary', !pat.running);
        btn.classList.toggle('danger', pat.running);
      }
    }
  } catch (e) {}
}
