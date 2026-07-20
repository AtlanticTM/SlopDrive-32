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
import { registerRenderer } from '../core/shadow.js';
import { OP_SET_SPEED, OP_SET_ACCEL, OP_BLEND, OP_MODE, OP_SAVE, OP_STREAM_MODE, OP_OVERSHOOT } from '../core/wire.js';
import { TRAVEL, setTravel, winMin, winMax, setWinMin, setWinMax, setWindowReady, renderWindow, useCurrentAsDefault, suppressPush, setSuppressPush, setSettingsAuthoritative } from '../core/range.js';
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
// persist=false (default, during drag): apply live only (no_persist) to avoid
// flogging NVS every slider tick. persist=true (on release / restore): commit to
// NVS so the limit survives a reboot WITHOUT hunting for "Save All Settings" —
// the "my speed keeps reverting" fix. :3
function pushOverride(persist) {
  clearTimeout(overrideTimer);
  overrideTimer = setTimeout(function() {
    var us = parseInt($('userMaxSpeed').value);
    var ua = parseInt($('userAccel').value);
    var is = parseInt($('inputMaxSpeed').value);
    var ia = parseInt($('inputAccel').value);
    var body = {
      user_max_speed: us, user_max_accel: ua,
      input_max_speed: is, input_max_accel: ia
    };
    if (!persist) body.no_persist = true;
    post('/api/settings', body).then(async function(r) {
      // Ground Truth Doctrine: read the firmware's POST-CLAMP echo and report
      // THOSE values, never the raw slider numbers we sent. Reconcile any
      // clamped slider back to device truth (the old code discarded the
      // response entirely and toasted the pre-clamp request). :3
      if (!r || !r.ok) {
        var errBody = r ? await r.text() : 'no response';
        toast('Limit apply FAILED: ' + errBody, 'bad', 'i-alert', 4000);
        return;
      }
      var d = await r.json();
      // Reconcile sliders + readouts to the applied (post-clamp) values.
      [['userMaxSpeed', 'userSpeedVal', 'user_max_speed'],
       ['userAccel', 'userAccelVal', 'user_max_accel'],
       ['inputMaxSpeed', 'inputSpeedVal', 'input_max_speed'],
       ['inputAccel', 'inputAccelVal', 'input_max_accel']
      ].forEach(function(t) {
        var el = $(t[0]);
        if (el && typeof d[t[2]] === 'number') {
          el.value = d[t[2]];
          onLiveSlider(t[1], el, t[0], true);   // confirmed device value
        }
      });
      var aus = (typeof d.user_max_speed === 'number') ? d.user_max_speed : us;
      var aua = (typeof d.user_max_accel === 'number') ? d.user_max_accel : ua;
      var ais = (typeof d.input_max_speed === 'number') ? d.input_max_speed : is;
      var aia = (typeof d.input_max_accel === 'number') ? d.input_max_accel : ia;
      var now = Date.now();
      if (now - _lastPushToastMs > 1500) {
        _lastPushToastMs = now;
        toast((persist ? 'Limits saved' : 'Limits applied') +
              ' — User: ' + aus + '/' + aua + ' | Input: ' + ais + '/' + aia,
              persist ? 'good' : 'info', persist ? 'i-check' : 'i-sliders');
      }
    }).catch(function(e) {
      // A swallowed network failure here left the sliders lying about applied
      // limits with zero signal — say it out loud. :3
      console.warn('[settings] pushOverride failed:', e);
      toast('Limit apply FAILED — network error; device limits unchanged', 'bad', 'i-alert', 4000);
    });
  }, persist ? 0 : 120);
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
  pushOverride(true);   // restore should stick across reboot, not just apply live
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
    var body = {
      range_min: Math.round(winMin), range_max: Math.round(winMax),
      max_speed: parseInt($('defMaxSpeed').value), accel: parseInt($('defAccel').value),
      blend_mode: blendMode,
      intiface_compat: !!($('#intifaceCompat') && $('#intifaceCompat').checked),
      default_range_min: clamp(parseInt($('defMinNum').value) || 0, 0, TRAVEL),
      default_range_max: clamp(parseInt($('defMaxNum').value) || TRAVEL, 0, TRAVEL),
      expert_mode: expertMode
    };
    // Dual limit sets — the live-override sliders push with no_persist during
    // drag, so "Save All Settings" is the ONLY place they get baked into NVS.
    // Include them here (from real device-seeded slider values) so User/Input
    // speed+accel actually survive a reboot instead of reverting to defaults.
    // Only send a slider that carries a real value (seeded from the device) —
    // never fabricate one for a gated/blank control (ground-truth doctrine). :3
    [['userMaxSpeed','user_max_speed'], ['userAccel','user_max_accel'],
     ['inputMaxSpeed','input_max_speed'], ['inputAccel','input_max_accel']
    ].forEach(function(pair) {
      var el = $(pair[0]);
      if (el && !el.disabled) {
        var v = parseInt(el.value);
        if (!isNaN(v)) body[pair[1]] = v;
      }
    });
    // Max rail length — only send a REAL value (ground-truth doctrine: never
    // push an invented default onto a live device). Field is seeded from the
    // device in loadSettings; if it's somehow blank we omit it entirely. :3
    var railEl = $('#railMaxNum');
    var railVal = railEl ? parseInt(railEl.value) : NaN;
    if (!isNaN(railVal)) body.max_rail = clamp(railVal, 10, 2000);
    var r = await post('/api/settings', body);
    // Check the HTTP response — a 400 from the firmware (e.g. rmin>=rmax)
    // doesn't throw, so we must check r.ok explicitly. :3
    if (!r || !r.ok) {
      var errBody = r ? await r.text() : 'no response';
      if (!silent) toast('Save failed: ' + errBody, 'bad', 'i-alert');
      return;
    }
    // Reconcile the default-motion sliders to the firmware's POST-CLAMP echo
    // and clear their unconfirmed (.intent) readout styling — the number shown
    // is now what the device actually applied, not the raw drag value. :3
    var saved = await r.json();
    [['defMaxSpeed', 'defSpeedVal', 'max_speed'], ['defAccel', 'defAccelVal', 'accel']]
      .forEach(function(t) {
        var el = $(t[0]);
        if (el && typeof saved[t[2]] === 'number') {
          el.value = saved[t[2]];
          onLiveSlider(t[1], el, t[0], true);
        }
      });
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

    // Rail length is MEASURED at homing and reported rounded to the nearest
    // 1mm — the machine is length-agnostic, 260 is only the geometry ceiling.
    // Measured stroke is the sole source of truth once homed; before that we
    // fall back to the advertised ceiling purely so the rail can draw at all,
    // but the number the operator sees is always the device's, never invented
    // client-side. :3
    const measured = (typeof d.measured_stroke === 'number' && d.measured_stroke > 0)
               ? Math.round(d.measured_stroke)
               : 0;
    const travel = measured || (d.max_travel ? Math.round(d.max_travel) : 0);
    if (travel) setTravel(travel);

    // Max rail length — seed the input from device truth ONLY (never invent a
    // value). This is the configured rail ceiling, distinct from the measured
    // stroke shown by TRAVEL above. :3
    const railEl = $('#railMaxNum');
    if (railEl && typeof d.max_rail === 'number' && d.max_rail > 0) {
      railEl.value = Math.round(d.max_rail);
    }

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
    // GOLDEN RULE: the UI never invents a setting value — it pulls device
    // truth. The firmware always advertises max_speed/accel from _state.config,
    // so these are read straight from the payload. We only skip seeding a slider
    // when its field is genuinely absent (old firmware), rather than fabricating
    // a 550/1500 that would silently misrepresent the machine's real config. :3
    const spd = (typeof d.max_speed === 'number') ? d.max_speed : undefined;
    const acc = (typeof d.accel === 'number') ? d.accel : undefined;
    // seedSlider — set a slider's value from a REAL device number, refresh its
    // readout, and un-gate it. The markup ships every machine-owned slider
    // `disabled` with a `—` readout so NOTHING is ever prepopulated with an
    // invented number on boot; the field only comes alive once the firmware's
    // own value lands here. If the field is genuinely absent (old firmware) the
    // slider stays disabled/blank rather than fabricating a value. :3
    function seedSlider(id, readId, val) {
      var e = $(id); if (!e) return;
      if (typeof val !== 'number') return;   // no device truth → leave gated/blank
      e.value = val;
      e.disabled = false;
      // Refresh the readout via the shared live-slider formatter (falls back to
      // a plain number write if it's unavailable for any reason).
      if (readId) { try { onLiveSlider(readId, e, id, true); } catch (err) { var r = $('#' + readId); if (r) r.textContent = Math.round(val); } }
    }
    // Seed default-motion sliders ONLY from real device values.
    seedSlider('defMaxSpeed', 'defSpeedVal', spd);
    seedSlider('defAccel',    'defAccelVal', acc);
    // Seed live dual-limit sliders from the dual-limit API fields, falling back
    // to the (real) combined values only when the dual-limit keys are absent.
    var us = (typeof d.user_max_speed === 'number') ? d.user_max_speed : spd;
    var ua = (typeof d.user_max_accel === 'number') ? d.user_max_accel : acc;
    var is = (typeof d.input_max_speed === 'number') ? d.input_max_speed : spd;
    var ia = (typeof d.input_max_accel === 'number') ? d.input_max_accel : acc;
    seedSlider('userMaxSpeed',  'userSpeedVal',  us);
    seedSlider('userAccel',     'userAccelVal',  ua);
    seedSlider('inputMaxSpeed', 'inputSpeedVal', is);
    seedSlider('inputAccel',    'inputAccelVal', ia);
    // Seed the old combined sliders for backward compat (they still exist in
    // the Settings tab "Default Motion" card, driving the "Save All" defaults).
    var eSp = $('maxSpeed'); if (eSp) eSp.value = spd;
    var eAc = $('accel'); if (eAc) eAc.value = acc;
    if (typeof d.blend_mode === 'number') reflectBlendMode(d.blend_mode);
    applyExpertCaps();

    // Re-arm push after init so user drags/inputs push normally
    setSuppressPush(false);

    // Thing 3: signal that the authoritative HTTP pull is complete. From this
    // point forward, WS config pushes (processConfig) are allowed to overwrite
    // the window — before this flag, they were blocked to prevent the "wrong
    // window on boot" race where a WS cfg snapshot arrives before the HTTP
    // response and paints stale bounds. :3
    setSettingsAuthoritative(true);
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
  try {
    var d = await get('/api/gen');
    if (!d || typeof d.rate_tick !== 'number') return;   // no device truth → leave un-selected
    var hz = d.rate_tick;
    document.querySelectorAll('#genTickSeg button').forEach(function(b) { b.classList.toggle('active', parseInt(b.dataset.hz) === hz); });
  } catch (e) {}
}

// ---- Shadow value extractors ---------------------------------------------
// While a command is in flight show the DESIRED value (shadow adds .pending
// styling to the seg), otherwise the device-REPORTED ground truth. Each value
// may arrive under its command-payload name or its cfg/echo name. :3
function _shadowSrc(sh) {
  var pending = sh.state === 'pending' || sh.state === 'overdue1' || sh.state === 'overdue2';
  return (pending && sh.desired) ? sh.desired : sh.reported;
}
function _pickNum(sh, keys) {
  var o = _shadowSrc(sh); if (!o) return null;
  for (var i = 0; i < keys.length; i++) if (typeof o[keys[i]] === 'number') return o[keys[i]];
  return null;
}
function _pickBool(sh, keys) {
  var o = _shadowSrc(sh); if (!o) return null;
  for (var i = 0; i < keys.length; i++) if (typeof o[keys[i]] !== 'undefined') return !!o[keys[i]];
  return null;
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
      pushOverride();          // live-apply during drag (no persist)
    });
    s.addEventListener('change', function() {
      pushOverride(true);      // commit to NVS on release — no "Save All" needed
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
    // Send only — the shadow renderer registered below reflects the desired
    // value (with .pending styling) and converges to the device's echo. :3
    cmd.send(OP_BLEND, { bm: parseInt(b.dataset.bm) });
  });

  // ---- Stream speed-feed A/B segmented control ----------------------------
  // Ground Truth Doctrine: the click only SENDS the command — the visible
  // selection flips when the device's echo (or a cfg push) confirms, via the
  // shadow renderer registered below. The seg shows .pending styling while the
  // command is in flight. (Previously the UI flipped before sending, and under
  // HTTP fallback flipped while sending NOTHING.) :3
  var sms = $('#streamModeSeg');
  if (sms) sms.addEventListener('click', function(e) {
    var b = e.target.closest('button'); if (!b || b.dataset.sm === undefined) return;
    cmd.send(OP_STREAM_MODE, { mode: parseInt(b.dataset.sm) });
  });

  // ---- v4 overshoot-clamp A/B segmented control ---------------------------
  // Same echo-confirmed lifecycle as the stream-mode control above. :3
  var ocs = $('#overshootSeg');
  if (ocs) ocs.addEventListener('click', function(e) {
    var b = e.target.closest('button'); if (!b || b.dataset.oc === undefined) return;
    cmd.send(OP_OVERSHOOT, { on: !!parseInt(b.dataset.oc) });
  });

  // ---- Shadow renderers: device-confirmed values drive the reflect fns -----
  // These fire on every ECHO and every device-authored cfg push (cfg_gen
  // bump / reconnect resync), so another client changing blend / stream-mode /
  // overshoot updates THIS browser's controls too — the fix for the segmented
  // controls that only ever tracked local click state. :3
  registerRenderer('blend', function(sh) {
    var v = _pickNum(sh, ['bm', 'blend_mode']);
    if (v != null) reflectBlendMode(v);
  });
  registerRenderer('stream_mode', function(sh) {
    var v = _pickNum(sh, ['mode', 'stream_speed_mode']);
    if (v != null) reflectStreamMode(v);
  });
  registerRenderer('overshoot', function(sh) {
    var v = _pickBool(sh, ['on', 'overshoot_clamp']);
    if (v != null) reflectOvershoot(v ? 1 : 0);
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
