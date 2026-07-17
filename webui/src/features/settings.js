/**
 * Settings tab — transport mode, defaults, timing, generation rate,
 * and the continuous-blend reversal policy. :3
 *
 * blend_mode is loaded from /api/settings (GET) and saved back on
 * "Save All Settings". The segmented control reflects the firmware's
 * current mode and lets the operator pick 1/2/3 without a page reload.
 * Old firmware that doesn't advertise blend_mode just gets the default
 * (1 = let-it-land) — backward-compatible, no drama. :3
 */
import { $, setRead, onLiveSlider, clamp, icon, toast } from '../core/ui.js';
import { post, get } from '../core/api.js';
import * as cmd from '../core/cmd.js';
import { OP_SET_SPEED, OP_SET_ACCEL, OP_BLEND, OP_MODE, OP_SAVE, OP_STREAM_MODE, OP_OVERSHOOT } from '../core/wire.js';
import { TRAVEL, setTravel, winMin, winMax, setWinMin, setWinMax, setWindowReady, renderWindow, useCurrentAsDefault, suppressPush, setSuppressPush } from '../core/range.js';
import { applyExpertCeilings, expertMode, setExpertMode } from '../core/capabilities.js';

export var currentMode = 'WS';

var MODE_HINTS = {
  WS:     'WebSocket — Intiface over WiFi',
  SER:    'Serial — Intiface over USB',
  BT:     'Bluetooth — BLE serial',
  DONGLE: 'Dongle — T-Dongle C5 relay via UART (pins 8/9). MFP connects to the dongle USB port; WiFi stays up for the web UI. :3'
};

export function reflectMode(mode) {
  currentMode = mode || 'WS';
  document.querySelectorAll('#modeSeg button').forEach(function(b) {
    b.classList.toggle('active', b.dataset.mode === currentMode);
  });
  var btBtn = $('#btModeBtn');
  if (btBtn) btBtn.classList.toggle('active', currentMode === 'BT');
  var hint = $('#modeHint');
  if (hint) hint.textContent = MODE_HINTS[currentMode] || '';
  var bs = $('#bleStatus');
  if (bs) bs.style.display = (currentMode === 'BT') ? 'inline-flex' : 'none';
}

async function setMode(mode) {
  if (mode === currentMode) return;
  reflectMode(mode);
  try {
    var r = await post('/api/mode', { mode: mode });
    if (!r) return;
    var d = await r.json();
    reflectMode(d.mode || mode);
    toast('Transport: ' + (d.mode || mode), 'good', 'i-link');
  } catch (e) {
    toast('Failed to switch transport', 'bad', 'i-alert');
  }
}

async function loadMode() {
  try { var d = await get('/api/mode'); if (d) reflectMode(d.mode); } catch (e) {}
}

// expertMode state is now owned by capabilities.js (plan.md §5.10.1 / §5.13).
// The old hardcoded 10000/5000 ceilings in applyExpertCaps() are replaced by
// applyExpertCeilings() which re-derives from the /api/capabilities payload
// — no more baked-in literals, the API is the sole source of truth. :3
function applyExpertCaps() {
  applyExpertCeilings();               // re-derive ALL slider max attrs from capsCache
}

// ---- Continuous-blend mode state ----------------------------------------
// Tracks which reversal policy the firmware is currently running.
// 1 = let-it-land (default), 2 = allow-reversal, 3 = hybrid.
// Loaded from /api/settings on init; saved back with "Save All Settings".
// The segmented control in #blendModeSeg reflects this live. :3
var blendMode = 1;

var BLEND_HINTS = {
  1: 'Finish the current stroke before reversing — smoothest ride, may drop a waypoint at extreme Hz. :3',
  2: 'Retarget immediately on reversal — tighter tracking, one clean decel per true turnaround.',
  3: 'Hybrid: let-it-land while the remaining distance is large, allow reversal when close to the end.'
};

function reflectBlendMode(mode) {
  blendMode = (mode >= 1 && mode <= 3) ? mode : 1;
  document.querySelectorAll('#blendModeSeg button').forEach(function(b) {
    b.classList.toggle('active', parseInt(b.dataset.bm) === blendMode);
  });
  var hint = $('#blendModeHint');
  if (hint) hint.textContent = BLEND_HINTS[blendMode] || '';
}

// ---- Stream speed-feed A/B state ----------------------------------------
// 0 = ceiling-pegged (hold input speed limit as cruise feed),
// 1 = velocity-matched (derive feed from distance + command cadence).
// The lever most likely to fix the v3-fails-at-slow-speeds stall. Pushed live
// via the WS control plane (OP_STREAM_MODE) so it can be A/B'd against a
// running stream; the firmware echoes it back in the cfg snapshot. :3
var streamMode = 0;

var STREAM_HINTS = {
  0: 'Hold the input speed ceiling as the cruise feed — flat-out tracking, may stall very slow moves.',
  1: 'Derive the feed from stroke distance and command cadence — gentle inputs get gentle-but-sufficient speed.'
};

function reflectStreamMode(mode) {
  streamMode = (mode === 1) ? 1 : 0;
  document.querySelectorAll('#streamModeSeg button').forEach(function(b) {
    b.classList.toggle('active', parseInt(b.dataset.sm) === streamMode);
  });
  var hint = $('#streamModeHint');
  if (hint) hint.textContent = STREAM_HINTS[streamMode] || '';
}

// ---- v4 overshoot-clamp A/B state ---------------------------------------
// 0 = off (raw MFP end-slope shaping — the cubic may bulge past the endpoint),
// 1 = on  (monotone Fritsch–Carlson tangent limit — the cubic can never leave
//          [start,end], killing the invented overshoot-then-return micromotion).
// Pushed live via OP_OVERSHOOT so it can be A/B'd against a running v4 stream
// while watching the rail interp overlay + the 0x05 Overshoot anomaly counter;
// the firmware echoes overshoot_clamp back in the cfg snapshot. :3
var overshootClamp = 0;

var OVERSHOOT_HINTS = {
  0: 'Raw MFP slope shaping — steep G tangents can bulge the cubic past the target and back (invented micromotion you can feel).',
  1: 'Monotone tangent clamp — the interpolated path can never overshoot the commanded endpoint. Watch the Overshoot anomaly count drop to zero.'
};

function reflectOvershoot(on) {
  overshootClamp = on ? 1 : 0;
  document.querySelectorAll('#overshootSeg button').forEach(function(b) {
    b.classList.toggle('active', parseInt(b.dataset.oc) === overshootClamp);
  });
  var hint = $('#overshootHint');
  if (hint) hint.textContent = OVERSHOOT_HINTS[overshootClamp] || '';
}

var _lastPushToastMs = 0;
function pushOverride() {
  clearTimeout(overrideTimer);
  overrideTimer = setTimeout(function() {
    var us = parseInt($('userMaxSpeed').value);
    var ua = parseInt($('userAccel').value);
    var is = parseInt($('inputMaxSpeed').value);
    var ia = parseInt($('inputAccel').value);
    post('/api/settings', {
      user_max_speed: us, user_max_accel: ua,
      input_max_speed: is, input_max_accel: ia,
      no_persist: true
    }).then(function() {
      // Realtime feedback — brief toast on limit commit
      var now = Date.now();
      if (now - _lastPushToastMs > 1500) {
        _lastPushToastMs = now;
        toast('Limits applied — User: ' + us + '/' + ua + ' | Input: ' + is + '/' + ia, 'info', 'i-sliders');
      }
    }).catch(function(){});
  }, 120);
}

export function restoreDefaults() {
  // Restore dual limit sets from the default-settings sliders. Format:
  // live slider id → 'def' + corresponding default slider id, e.g.:
  // userMaxSpeed → defMaxSpeed, inputMaxSpeed → defMaxSpeed (both seed from legacy).
  var map = {
    userMaxSpeed: 'defMaxSpeed', userAccel: 'defAccel',
    inputMaxSpeed: 'defMaxSpeed', inputAccel: 'defAccel'
  };
  Object.keys(map).forEach(function(liveId) {
    var s = $(liveId), d = $(map[liveId]);
    if (s && d) s.value = d.value;
    if (s) s.dispatchEvent(new Event('input'));
  });
  pushOverride();
  toast('Restored saved defaults', 'info', 'i-reset');
}

export async function saveSettings(silent) {
  // Guard: winMin/winMax must be valid before we try to save. If they're
  // still at the module default (10/110) because loadSettings() hasn't
  // returned yet, we just wait — don't block the user with an alert. :3
  if (winMin >= winMax) { if (!silent) toast('Range invalid — min must be less than max', 'bad', 'i-alert'); return; }
  try {
    // Fire save via WS control plane AND HTTP for response validation
    cmd.send(OP_SAVE, {});
    var r = await post('/api/settings', {
      range_min: Math.round(winMin), range_max: Math.round(winMax),
      max_speed: parseInt($('defMaxSpeed').value), accel: parseInt($('defAccel').value),
      blend_mode: blendMode,
      intiface_compat: !!($('#intifaceCompat') && $('#intifaceCompat').checked),
      default_range_min: clamp(parseInt($('defMinNum').value) || 0, 0, TRAVEL),
      default_range_max: clamp(parseInt($('defMaxNum').value) || TRAVEL, 0, TRAVEL),
      expert_mode: expertMode
    });
    // Check the HTTP response — a 400 from the firmware (e.g. rmin>=rmax)
    // doesn't throw, so we must check r.ok explicitly. :3
    if (!r || !r.ok) {
      var errBody = r ? await r.text() : 'no response';
      if (!silent) toast('Save failed: ' + errBody, 'bad', 'i-alert');
      return;
    }
    ['saveInd', 'saveIndRange'].forEach(function(id) {
      var i = $(id);
      if (i) { i.classList.add('show'); setTimeout(function() { i.classList.remove('show'); }, 1800); }
    });
    if (!silent) toast('Settings saved', 'good', 'i-check');
  } catch (e) { if (!silent) toast('Failed to save: ' + e, 'bad', 'i-alert'); }
}

async function loadSettings() {
  try {
    const d = await get('/api/settings');
    if (!d || d.range_min === undefined) return;
    // Prevent pushing defaults DOWN to a live device on page refresh (P3-002/C4.2).
    // Load is READ-ONLY: populate DOM from device truth, never dispatch settings back.
    setSuppressPush(true);

    const travel = (d.measured_stroke && d.measured_stroke > 0)
               ? d.measured_stroke
               : d.max_travel;
    if (travel) setTravel(travel);

    setWinMin(d.range_min);
    setWinMax(d.range_max);
    setWindowReady(true);
    renderWindow();
    const dn = $('#defMinNum'); if (dn) dn.value = d.default_range_min || 0;
    const dx = $('#defMaxNum'); if (dx) dx.value = d.default_range_max || TRAVEL;
    setExpertMode(d.expert_mode || false);
    if (typeof d.stream_speed_mode === 'number') reflectStreamMode(d.stream_speed_mode);
    if (typeof d.overshoot_clamp !== 'undefined') reflectOvershoot(!!d.overshoot_clamp);
    const ic = $('#intifaceCompat'); if (ic) ic.checked = !!d.intiface_compat;
    const spd = d.max_speed || 550, acc = d.accel || 1500;
    const look = d.lookahead || 20, over = d.overshoot || 8;
    // Seed default-motion sliders from the legacy max_speed/accel fields
    ['defMaxSpeed', 'defAccel'].forEach(function(pair) {
      var id = pair; var val = (pair === 'defMaxSpeed') ? spd : acc;
      var e = $(id); if (e) e.value = val;
    });
    // Seed live dual-limit sliders from the dual-limit API fields
    var us = d.user_max_speed || spd, ua = d.user_max_accel || acc;
    var is = d.input_max_speed || spd, ia = d.input_max_accel || acc;
    var els = { userMaxSpeed: us, userAccel: ua, inputMaxSpeed: is, inputAccel: ia };
    Object.keys(els).forEach(function(id) {
      var e = $(id); if (e) e.value = els[id];
    });
    // Seed the old combined sliders for backward compat (they still exist in
    // the Settings tab "Default Motion" card, driving the "Save All" defaults).
    var eSp = $('maxSpeed'); if (eSp) eSp.value = spd;
    var eAc = $('accel'); if (eAc) eAc.value = acc;
    reflectBlendMode(d.blend_mode || 1);
    applyExpertCaps();

    // Re-arm push after init so user drags/inputs push normally
    setSuppressPush(false);
  } catch (e) {}
}

var inputMode = 'extrapolate';
function reflectInputMode(mode) {
  inputMode = (mode === 'buffered') ? 'buffered' : 'extrapolate';
  document.querySelectorAll('#inputModeSeg button').forEach(function(b) {
    b.classList.toggle('active', b.dataset.im === inputMode);
  });
  var ep = $('#extrapPanel'), bp = $('#bufPanel');
  if (ep) ep.style.display = (inputMode === 'extrapolate') ? 'block' : 'none';
  if (bp) bp.style.display = (inputMode === 'buffered') ? 'block' : 'none';
}
function pushInterp(save) {
  // Read the active interpolation rate button instead of assuming 50 Hz.
  // Default to 100 when nothing is selected — we're faster by default now. :3
  var a = document.querySelector('#bufTickSeg button.active');
  var tick = a ? parseInt(a.dataset.hz) : 100;
  post('/api/interp', {
    mode: inputMode, depth: parseInt($('bufDepth').value),
    tick: tick, easing: parseInt($('bufEasing').value), save: !!save
  });
}
async function loadInterp() {
  try { var d = await get('/api/interp'); if (d) reflectInputMode(d.mode); } catch (e) {}
}

function genTickValue() {
  var a = document.querySelector('#genTickSeg button.active');
  return a ? parseInt(a.dataset.hz) : 100;
}
async function loadGenTick() {
  try { var d = await get('/api/gen'); if (!d) return; var hz = d.rate_tick || 100; document.querySelectorAll('#genTickSeg button').forEach(function(b) { b.classList.toggle('active', parseInt(b.dataset.hz) === hz); }); } catch (e) {}
}

export function initSettings() {
  var ms = $('#modeSeg');
  if (ms) ms.addEventListener('click', function(e) {
    var b = e.target.closest('button'); if (!b) return; setMode(b.dataset.mode);
  });
  var eb = $('#expertMode');
  if (eb) eb.addEventListener('change', function() {
    setExpertMode(eb.checked); applyExpertCaps();
  });
  var ims = $('#inputModeSeg');
  if (ims) ims.addEventListener('click', function(e) {
    var b = e.target.closest('button'); if (!b) return; reflectInputMode(b.dataset.im); pushInterp();
  });
  var bts = $('#bufTickSeg');
  if (bts) bts.addEventListener('click', function(e) {
    var b = e.target.closest('button'); if (!b) return;
    bts.querySelectorAll('button').forEach(function(x) { x.classList.remove('active'); });
    b.classList.add('active'); pushInterp();
  });
  var bd = $('#bufDepth');
  if (bd) bd.addEventListener('input', function() { setRead('bufDepthVal', bd.value); pushInterp(); });
  var be = $('#bufEasing');
  if (be) be.addEventListener('change', function() { pushInterp(); });
  var gts = $('#genTickSeg');
  if (gts) gts.addEventListener('click', function(e) {
    var b = e.target.closest('button'); if (!b) return;
    gts.querySelectorAll('button').forEach(function(x) { x.classList.remove('active'); });
    b.classList.add('active'); post('/api/gen', { rate_tick: genTickValue() });
  });
  // Dual-limit live-override sliders — update readout + push to firmware.
  [['userMaxSpeed','userSpeedVal'], ['userAccel','userAccelVal'],
   ['inputMaxSpeed','inputSpeedVal'], ['inputAccel','inputAccelVal']].forEach(function(pair) {
    var s = $(pair[0]); if (!s) return;
    s.addEventListener('input', function() {
      onLiveSlider(pair[1], s, pair[0]);
      pushOverride();
    });
  });
  // Default Motion sliders — update readout only (no live push; saved on "Save All Settings").
  [['defMaxSpeed','defSpeedVal'], ['defAccel','defAccelVal']].forEach(function(pair) {
    var s = $(pair[0]); if (!s) return;
    s.addEventListener('input', function() { onLiveSlider(pair[1], s, pair[0]); });
  });
  var rb = document.querySelector("[data-action='restoreDefaults']");
  if (rb) rb.addEventListener('click', restoreDefaults);
  var uc = document.querySelector("[data-action='useCurrentAsDefault']");
  if (uc) uc.addEventListener('click', useCurrentAsDefault);
  // ---- Continuous-blend segmented control ---------------------------------
  // Clicking a mode button updates the local blendMode state and reflects it
  // immediately in the UI. The new mode is sent to the firmware right away
  // (no_persist) so the operator can feel the difference live — it gets
  // persisted when they hit "Save All Settings". :3
  var bms = $('#blendModeSeg');
  if (bms) bms.addEventListener('click', function(e) {
    var b = e.target.closest('button'); if (!b || !b.dataset.bm) return;
    var m = parseInt(b.dataset.bm);
    reflectBlendMode(m);
    // Push the new mode to the firmware immediately so the pounding changes
    // right now — no save required to feel the difference. :3
    cmd.send(OP_BLEND, { bm: blendMode });
  });

  // ---- Stream speed-feed A/B segmented control ----------------------------
  // Flips the streaming cruise-feed policy live so the operator can A/B it
  // against a running MFP stream while watching the rail interp overlay. The
  // firmware applies it on the next cruise feed (no re-home, no save needed).
  var sms = $('#streamModeSeg');
  if (sms) sms.addEventListener('click', function(e) {
    var b = e.target.closest('button'); if (!b || b.dataset.sm === undefined) return;
    var m = parseInt(b.dataset.sm);
    reflectStreamMode(m);
    cmd.send(OP_STREAM_MODE, { mode: streamMode });
    toast(streamMode === 1 ? 'Speed-feed: velocity-matched' : 'Speed-feed: ceiling-pegged',
          'info', 'i-gauge');
  });

  // ---- v4 overshoot-clamp A/B segmented control ---------------------------
  // Flips the monotone tangent limiter live so the operator can A/B it against
  // a running MFP v4 stream while watching the rail interp overlay and the
  // Overshoot anomaly counter. Applied on the next committed segment — no
  // re-home, no save needed to feel the difference. :3
  var ocs = $('#overshootSeg');
  if (ocs) ocs.addEventListener('click', function(e) {
    var b = e.target.closest('button'); if (!b || b.dataset.oc === undefined) return;
    var on = parseInt(b.dataset.oc);
    reflectOvershoot(on);
    cmd.send(OP_OVERSHOOT, { on: !!overshootClamp });
    toast(overshootClamp ? 'Overshoot clamp ON — monotone path' : 'Overshoot clamp OFF — raw slope',
          'info', 'i-gauge');
  });

  // Intiface compat toggle — apply live (no_persist) the instant it's flipped
  // so the operator can A/B it against a running stream without saving, then
  // bake it in with "Save All Settings". Default state comes from loadSettings.
  var ic = $('#intifaceCompat');
  if (ic) ic.addEventListener('change', function() {
    post('/api/settings', { intiface_compat: ic.checked, no_persist: true });
    toast(ic.checked ? 'Intiface position fix ON' : 'Intiface position fix OFF',
          'info', 'i-gauge');
  });
  loadSettings(); loadMode(); loadGenTick();
}
