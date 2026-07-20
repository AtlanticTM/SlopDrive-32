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
import { initApEditor, setApEditorState, getApEditorBaseFields, getApSnapshot } from "./apEditor.js";

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

// ===================== Advanced mode (fray-d port) =====================
// Reported device truth: apMode is null until the device says otherwise (old
// firmware never says — the seg + panel simply never appear). The visual
// curve editor (apEditor.js) owns the base-parameter and modifier surfaces;
// this module owns the transport: live drags stream over the same gen_cfg
// path the sliders used, and every drag release reconciles the editor from
// GET /api/pattern so it converges to the device's post-clamp truth.
var apMode = null;

function apBasePayload() {
    var out = getApEditorBaseFields();
    var s = $('apSpeed');
    if (s && !s.disabled) out.ap_speed = parseInt(s.value);
    return out;
}

var apPP = null;
function pushApParams() {
    clearTimeout(apPP);
    apPP = setTimeout(function () {
        cmd.send(OP_GEN_CFG, apBasePayload());
    }, 60);
}

// Editor live-drag sends — either the full base set or one {ap_mod} block.
// Same 60ms debounce cadence as the sliders had; latest payload wins.
var apLiveT = null, apLivePending = null;
function pushApLive(fields) {
    apLivePending = fields;
    clearTimeout(apLiveT);
    apLiveT = setTimeout(function () {
        if (apLivePending) cmd.send(OP_GEN_CFG, apLivePending);
        apLivePending = null;
    }, 60);
    // Live-edit → re-evaluate whether the current shape still matches the
    // loaded preset; toggles the SAVE button and the " *" dirty marker.
    recomputeApDirty();
}

// Drag released → adopt the device's post-clamp truth back into the editor.
// Delayed past the last in-flight cmd's apply/echo so the GET reads the
// settled state, not a race with our own final send.
var apRecT = null;
function apReconcile() {
    clearTimeout(apRecT);
    apRecT = setTimeout(async function () {
        try {
            const d = await get('/api/pattern');
            adoptApState(d);
        } catch (e) {}
    }, 250);
}

// ---- fray-d factory presets --------------------------------------------
// Decoded from OSSM-Lite advanced_penetration_strings.h factoryReset().
// Every preset is a DELTA on the reset baseline (in/out speed 100, accels
// 40, all modifiers off) — the firmware applies {ap_reset:true, …deltas}
// atomically. Depths and master speed are never part of a preset (fray-d
// semantics: presets change stroke character, not your safety window).
// mods keys are advpat ctrl ids: 0=max depth, 1=min depth, 2=in speed,
// 3=out speed, 4=in accel, 5=out accel.
var AP_PRESETS = [
    { name: 'Simple' },
    { name: 'Teasing',    base: { ap_in_speed: 50 } },
    { name: 'Pounding',   base: { ap_out_speed: 50 } },
    { name: 'Robo',       base: { ap_in_accel: 100, ap_out_accel: 100 } },
    { name: "Half'n'half", mods: { 0: { amplitude: 50 } } },
    { name: 'Deeper',     mods: { 0: { amplitude: 15, out_step: 10 } } },
    { name: 'Insist',     mods: { 1: { amplitude: 15, in_step: 10 } } },
    { name: 'Jackhammer', base: { ap_out_accel: 0 },
      mods: { 0: { amplitude: 15, out_step: 9 },
              1: { amplitude: 15, in_step: 9 },
              3: { amplitude: 50, in_wait: 8 } } },
    { name: 'Progressive',
      mods: { 0: { amplitude: 15, in_step: 10, in_wait: 1, out_step: 10 },
              1: { amplitude: 15, in_step: 10, in_wait: 1, out_step: 10, offset: 11 } } },
    { name: 'Mid',
      mods: { 0: { amplitude: 15, in_step: 10, in_wait: 1, out_step: 10 },
              1: { amplitude: 15, in_step: 10, in_wait: 1, out_step: 10 } } },
    { name: 'Knot (75%)',
      mods: { 0: { amplitude: 75 }, 1: { amplitude: 25 },
              2: { amplitude: 25, offset: 1 }, 3: { amplitude: 2 } } },
    { name: 'Knot (50%)',
      mods: { 0: { amplitude: 50 }, 1: { amplitude: 50 },
              2: { amplitude: 25, offset: 1 }, 3: { amplitude: 2 } } }
];

var AP_MOD_DEFAULTS = { amplitude: 100, in_step: 1, in_wait: 0, out_step: 1, out_wait: 0, offset: 0 };

// A canonical preset "def" = the full stroke-shape snapshot:
//   { in_speed, out_speed, in_accel, out_accel, mods:[6 full blocks] }
// Factory presets are authored sparse (base deltas + mods); factoryDef()
// expands one into a canonical def so factory + user presets load, compare
// (dirty), and save through ONE code path.
function factoryDef(p) {
    var b = p.base || {};
    var def = {
        in_speed:  typeof b.ap_in_speed  === 'number' ? b.ap_in_speed  : 100,
        out_speed: typeof b.ap_out_speed === 'number' ? b.ap_out_speed : 100,
        in_accel:  typeof b.ap_in_accel  === 'number' ? b.ap_in_accel  : 40,
        out_accel: typeof b.ap_out_accel === 'number' ? b.ap_out_accel : 40,
        mods: []
    };
    for (var id = 0; id < 6; id++) {
        var m = Object.assign({ ctrl: id }, AP_MOD_DEFAULTS);
        if (p.mods && p.mods[id]) Object.assign(m, p.mods[id]);
        def.mods.push(m);
    }
    return def;
}
var RESET_DEF = factoryDef({});   // the fray-d baseline

// Order-stable string key of a def — the basis for dirty-detection.
function defKey(def) {
    if (!def) return '';
    var parts = [def.in_speed, def.out_speed, def.in_accel, def.out_accel];
    (def.mods || []).slice().sort(function (a, b) { return a.ctrl - b.ctrl; })
        .forEach(function (m) {
            parts.push(m.ctrl, m.amplitude, m.in_step, m.in_wait, m.out_step, m.out_wait, m.offset);
        });
    return parts.join(',');
}

// def → /api/pattern apply body (reset baseline + the def's values).
function defToApplyBody(def) {
    return {
        ap_reset: true,
        ap_in_speed: def.in_speed, ap_out_speed: def.out_speed,
        ap_in_accel: def.in_accel, ap_out_accel: def.out_accel,
        ap_mods: (def.mods || []).map(function (m) {
            return { ctrl: m.ctrl, amplitude: m.amplitude, in_step: m.in_step, in_wait: m.in_wait,
                     out_step: m.out_step, out_wait: m.out_wait, offset: m.offset };
        })
    };
}

function escHtml(s) {
    return String(s).replace(/[&<>"']/g, function (c) {
        return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
    });
}

// ---- Preset state ---------------------------------------------------------
var LS_KEY = 'sd32_ap_presets';
var apUserPresets = [];       // [{name, def}] mirrored from device NVS
var apLoadedName  = null;     // currently loaded preset name (factory OR user)
var apLoadedIsUser = false;
var apLoadedKey   = null;     // defKey of the loaded preset, for dirty compare
var apDirtyState  = false;
var apMenuOpen    = false;
var apDeleteArm   = null;     // user-preset name armed for delete
var apDeleteTimer = null;

function findUserPreset(name) {
    for (var i = 0; i < apUserPresets.length; i++)
        if (apUserPresets[i].name === name) return apUserPresets[i];
    return null;
}
function normalizePresets(list) {
    return (Array.isArray(list) ? list : [])
        .filter(function (p) { return p && p.name && p.def; })
        .map(function (p) { return { name: String(p.name), def: p.def }; });
}
function mirrorLS() { try { localStorage.setItem(LS_KEY, JSON.stringify(apUserPresets)); } catch (e) {} }
function readLS()   { try { var s = localStorage.getItem(LS_KEY); return s ? normalizePresets(JSON.parse(s)) : null; } catch (e) { return null; } }

// ---- Dirty / label --------------------------------------------------------
function setApLoadedLabel() {
    var el = $('apPresetLabel');
    if (!el) return;
    el.textContent = apLoadedName ? (apLoadedName + (apDirtyState ? ' *' : '')) : '— apply a preset —';
}
function setApDirty(on) {
    apDirtyState = !!on;
    var sb = $('apSaveBtn'); if (sb) sb.hidden = !apDirtyState;
    setApLoadedLabel();
}
// Recomputed on every editor live-edit: dirty = state differs from the loaded
// preset (or, with nothing loaded, from the reset baseline → offers a Save).
function recomputeApDirty() {
    var snap = getApSnapshot();
    if (!snap) { setApDirty(false); return; }
    setApDirty(defKey(snap) !== (apLoadedKey || defKey(RESET_DEF)));
}

// ---- Apply / load ---------------------------------------------------------
async function applyApDef(def, name, isUser) {
    try {
        const r = await post('/api/pattern', defToApplyBody(def));
        if (!r) { toast('Preset apply failed — no response', 'bad', 'i-alert'); return; }
        const d = await r.json();
        if (!d || !d.ok) { toast('Preset rejected by device', 'bad', 'i-alert'); return; }
        apLoadedName = name; apLoadedIsUser = !!isUser; apLoadedKey = defKey(def);
        setApDirty(false);
        apReconcile();     // editor → confirmed device truth
        toast('Preset loaded: ' + name, 'good', 'i-wave');
    } catch (e) {}
}
function loadFactoryPreset(idx) { var p = AP_PRESETS[idx]; if (p) applyApDef(factoryDef(p), p.name, false); }
function loadUserPreset(name)   { var u = findUserPreset(name); if (u) applyApDef(u.def, name, true); }

async function apResetCmd() {
    try {
        const r = await post('/api/pattern', { ap_reset: true });
        if (!r) { toast('Reset failed — no response', 'bad', 'i-alert'); return; }
        const d = await r.json();
        if (d && d.ok) {
            apLoadedName = null; apLoadedIsUser = false; apLoadedKey = null;
            setApDirty(false);
            apReconcile();
            toast('Advanced controls reset to defaults', 'info', 'i-wave');
        }
    } catch (e) {}
}

// ---- Save / delete / import / export --------------------------------------
async function saveApPreset() {
    var snap = getApSnapshot();
    if (!snap) return;
    var name;
    if (apLoadedName && apLoadedIsUser) {
        name = apLoadedName;                 // overwrite the loaded user preset
    } else {
        name = window.prompt('Save preset as:', apLoadedName ? (apLoadedName + ' copy') : 'My Pattern');
        if (name === null) return;
        name = name.trim();
        if (!name) return;
    }
    try {
        const r = await post('/api/pattern/presets', { name: name, def: snap });
        if (!r) { toast('Save failed — no response', 'bad', 'i-alert'); return; }
        const d = await r.json();
        if (!d || !d.ok) { toast('Save failed: ' + ((d && d.error) || 'device error'), 'bad', 'i-alert'); return; }
        apUserPresets = normalizePresets(d.presets); mirrorLS();
        apLoadedName = name; apLoadedIsUser = true; apLoadedKey = defKey(snap);
        setApDirty(false); buildPresetMenu();
        toast('Preset saved: ' + name, 'good', 'i-check');
    } catch (e) { toast('Save failed', 'bad', 'i-alert'); }
}

function armDelete(name) {
    if (apDeleteArm === name) {               // second click → delete
        clearTimeout(apDeleteTimer); apDeleteArm = null;
        deleteUserPreset(name);
        return;
    }
    apDeleteArm = name;                       // first click → arm (red ?)
    buildPresetMenu();
    clearTimeout(apDeleteTimer);
    apDeleteTimer = setTimeout(function () { apDeleteArm = null; buildPresetMenu(); }, 3000);
}
async function deleteUserPreset(name) {
    try {
        const r = await post('/api/pattern/presets', { name: name, delete: true });
        if (!r) { toast('Delete failed — no response', 'bad', 'i-alert'); return; }
        const d = await r.json();
        if (!d || !d.ok) { toast('Delete failed', 'bad', 'i-alert'); return; }
        apUserPresets = normalizePresets(d.presets); mirrorLS();
        if (apLoadedName === name && apLoadedIsUser) { apLoadedIsUser = false; }  // now unsaved
        buildPresetMenu();
        toast('Preset deleted: ' + name, 'info', 'i-trash');
    } catch (e) {}
}

function exportApPresets() {
    var data = JSON.stringify({ type: 'sd32-advanced-presets', v: 1, presets: apUserPresets }, null, 2);
    var url = URL.createObjectURL(new Blob([data], { type: 'application/json' }));
    var a = document.createElement('a');
    a.href = url; a.download = 'sd32-presets.json';
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(function () { URL.revokeObjectURL(url); }, 1000);
    closePresetMenu();
    toast('Exported ' + apUserPresets.length + ' preset' + (apUserPresets.length === 1 ? '' : 's'), 'good', 'i-down');
}
async function importApPresets(file) {
    try {
        var obj = JSON.parse(await file.text());
        var list = normalizePresets(Array.isArray(obj) ? obj : (obj && obj.presets));
        if (!list.length) { toast('No presets in file', 'warn', 'i-alert'); return; }
        var ok = 0;
        for (var i = 0; i < list.length; i++) {
            const r = await post('/api/pattern/presets', { name: list[i].name, def: list[i].def });
            if (r) { const d = await r.json(); if (d && d.ok) { ok++; apUserPresets = normalizePresets(d.presets); } }
        }
        mirrorLS(); buildPresetMenu();
        toast('Imported ' + ok + ' preset' + (ok === 1 ? '' : 's'), 'good', 'i-check');
    } catch (e) { toast('Import failed — bad file', 'bad', 'i-alert'); }
}

async function loadApUserPresets() {
    try {
        const d = await get('/api/pattern/presets');
        if (d && Array.isArray(d.presets)) { apUserPresets = normalizePresets(d.presets); mirrorLS(); }
    } catch (e) {
        var ls = readLS(); if (ls) apUserPresets = ls;   // device down → LS backup
    }
    buildPresetMenu();
}

// ---- Custom dropdown ------------------------------------------------------
function buildPresetMenu() {
    var menu = $('apPresetMenu'); if (!menu) return;
    var h = '<div class="ap-dd-sec">FACTORY</div>';
    AP_PRESETS.forEach(function (p, i) {
        h += '<button type="button" class="ap-dd-fac" data-idx="' + i + '">' + escHtml(p.name) + '</button>';
    });
    if (apUserPresets.length) {
        h += '<div class="ap-dd-div"></div><div class="ap-dd-sec">MY PRESETS</div>';
        apUserPresets.forEach(function (u) {
            var armed = apDeleteArm === u.name;
            h += '<div class="ap-dd-userrow">'
               +   '<button type="button" class="ap-dd-load" data-name="' + escHtml(u.name) + '">' + escHtml(u.name) + '</button>'
               +   '<button type="button" class="ap-dd-del' + (armed ? ' arm' : '') + '" data-name="' + escHtml(u.name) + '" '
               +     'title="' + (armed ? 'Click again to delete' : 'Delete') + '">' + (armed ? '?' : icon('i-trash')) + '</button>'
               + '</div>';
        });
    }
    h += '<div class="ap-dd-div"></div><div class="ap-dd-foot">'
       +   '<button type="button" class="ap-dd-mini" id="apExportBtn">' + icon('i-down') + ' Export</button>'
       +   '<button type="button" class="ap-dd-mini" id="apImportBtn">' + icon('i-up') + ' Import</button>'
       + '</div>';
    menu.innerHTML = h;
}
function openPresetMenu() {
    apMenuOpen = true; buildPresetMenu();
    var m = $('apPresetMenu'); if (m) m.hidden = false;
    var dd = $('apPresetDD'); if (dd) dd.classList.add('open');
}
function closePresetMenu() {
    apMenuOpen = false;
    var m = $('apPresetMenu'); if (m) m.hidden = true;
    var dd = $('apPresetDD'); if (dd) dd.classList.remove('open');
    if (apDeleteArm) { apDeleteArm = null; clearTimeout(apDeleteTimer); }
}
function onPresetMenuClick(e) {
    var del = e.target.closest('.ap-dd-del');
    if (del) { e.stopPropagation(); armDelete(del.dataset.name); return; }
    var load = e.target.closest('.ap-dd-load');
    if (load) { loadUserPreset(load.dataset.name); closePresetMenu(); return; }
    var fac = e.target.closest('.ap-dd-fac');
    if (fac) { loadFactoryPreset(parseInt(fac.dataset.idx)); closePresetMenu(); return; }
    if (e.target.closest('#apExportBtn')) { exportApPresets(); return; }
    if (e.target.closest('#apImportBtn')) { var f = $('apImportFile'); if (f) f.click(); closePresetMenu(); return; }
}

// Panel + seg reflection from the device's CONFIRMED mode. Never called with
// an assumed value — only from GET /api/pattern or a POST response.
function applyApMode(mode) {
    apMode = !!mode;
    var seg = $('#patModeSeg');
    if (seg) {
        seg.hidden = false;
        seg.querySelectorAll('button').forEach(function (b) {
            b.classList.toggle('active', (b.dataset.apmode === '1') === apMode);
        });
    }
    var ap = $('#apPanel'); if (ap) ap.hidden = !apMode;
    var lg = $('#legacyPanel'); if (lg) lg.hidden = apMode;
    updatePatState();
}

// Adopt advanced-mode state from a full /api/pattern readback (page load /
// reconnect / mode-toggle response / drag-release reconcile).
function adoptApState(d) {
    if (!d || typeof d.ap_mode !== 'boolean') return;   // old firmware: no advanced UI
    seedPatSlider('apSpeed', 'apSpeedVal', typeof d.ap_speed === 'number' ? d.ap_speed : undefined);
    setApEditorState(d);
    applyApMode(d.ap_mode);
}

async function setApModeOnDevice(mode) {
    try {
        const r = await post('/api/pattern', { ap_mode: mode });
        if (!r) { toast('Mode switch failed — no response', 'bad', 'i-alert'); return; }
        const d = await r.json();
        if (d && typeof d.ap_mode === 'boolean') {
            applyApMode(d.ap_mode);   // device's confirmed answer, not our request
        }
    } catch (e) {}
}

function initAdvancedPanel() {
    // Visual curve editor — owns depth/speed/accel handles + modifier cycle.
    initApEditor({ onLive: pushApLive, onCommit: apReconcile });

    // Master speed stays a slider: it's the throttle, not a shape.
    var s = $('apSpeed');
    if (s) s.addEventListener('input', function () {
        setRead('apSpeedVal', pad(parseInt(s.value) || 0, 3, 0));
        pushApParams();
    });

    // Preset picker — custom dropdown (factory + user sections, delete buttons,
    // import/export). Loading a preset is a COMMAND; the editor re-renders from
    // the device's confirmed post-apply readback.
    var pbtn = $('apPresetBtn');
    if (pbtn) pbtn.addEventListener('click', function (e) {
        e.stopPropagation();
        apMenuOpen ? closePresetMenu() : openPresetMenu();
    });
    var pmenu = $('apPresetMenu');
    if (pmenu) pmenu.addEventListener('click', onPresetMenuClick);
    document.addEventListener('click', function (e) {
        if (!apMenuOpen) return;
        var dd = $('apPresetDD');
        if (dd && !dd.contains(e.target)) closePresetMenu();
    });
    var sb = $('apSaveBtn');
    if (sb) sb.addEventListener('click', saveApPreset);
    var imp = $('apImportFile');
    if (imp) imp.addEventListener('change', function () {
        if (imp.files && imp.files[0]) importApPresets(imp.files[0]);
        imp.value = '';
    });
    loadApUserPresets();

    var rb = $('apResetBtn');
    if (rb) rb.addEventListener('click', apResetCmd);

    // Rhythm-modifier collapsible — advanced, off by default.
    var mt = $('apModToggle');
    if (mt) mt.addEventListener('click', function () {
        var body = $('apModBody');
        if (!body) return;
        var opening = body.hidden;
        body.hidden = !opening;
        mt.setAttribute('aria-expanded', opening ? 'true' : 'false');
        mt.classList.toggle('open', opening);
    });

    // Mode seg — sends the request, renders only the device's answer.
    var seg = $('#patModeSeg');
    if (seg) seg.addEventListener('click', function (e) {
        var b = e.target.closest('button');
        if (!b || !('apmode' in b.dataset)) return;
        setApModeOnDevice(b.dataset.apmode === '1');
    });
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
        // Advanced mode: seed sliders + modifier cache, adopt the device's
        // reported mode (reveals the seg; old firmware → no-op).
        adoptApState(d);
    } catch (e) {}
}

export function initPattern() {
    // Build the pattern glyph tile grid
    initPatternGrid();

    // Advanced panel wiring (sliders/modifier editor/mode seg)
    initAdvancedPanel();

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
    if (!r) {
        // No response — we do NOT know the machine stopped. Keep showing
        // running: falsely showing standby while the pattern still moves
        // suppresses the operator's cue to hit the physical E-stop. :3
        toast("Pattern stop UNCONFIRMED — no response; device may still be running!", "bad", "i-alert", 6000);
        return;
    }
    const d = await r.json();
    // Ground truth: render the device's ACTUAL reported running flag, never an
    // assumed standby. (The old code set pat.running=false in both branches,
    // discarding the device's answer entirely.) :3
    pat.running = d ? !!d.running : pat.running;
    if (pat.running) {
        toast("Pattern stop FAILED — device still reports RUNNING!", "bad", "i-alert", 6000);
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
        if (apMode) {
            el.textContent = 'running · advanced';
        } else {
            var id = selectedPatternId();
            var entry = patternRegistry().filter(function (p) { return p.id === id; })[0];
            el.textContent = 'running · ' + (entry ? entry.name.toLowerCase() : id);
        }
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
    // Re-adopt advanced state too — a reconnect may bridge a mode/param
    // change made from another client or a firmware update.
    adoptApState(d);
  } catch (e) {}
}
