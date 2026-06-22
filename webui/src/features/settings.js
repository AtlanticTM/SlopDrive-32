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
import { TRAVEL, setTravel, winMin, winMax, setWinMin, setWinMax, setWindowReady, renderWindow, useCurrentAsDefault } from '../core/range.js';

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

var expertMode = false;

function applyExpertCaps() {
  var ex = expertMode;
  // Normal mode: 5000 mm/s speed, 50000 mm/s² accel.
  // Expert mode: 10000 mm/s speed, 100000 mm/s² accel.
  // Expert mode is for people who know what they're doing and want to
  // absolutely fist the machine until the carriage bulges. You asked for it. yippie! :3
  // The 57AIM servo drive at 800 steps/rev × 10 steps/mm is a closed-loop
  // servo — it won't skip steps, it'll just go. Strap in. owo
  var EM = { maxSpeed: 10000, accel: 100000, lookahead: 80, runCurrent: 3000, genRate: 50, modRate: 5, modAmp: 10 };
  var SM = { maxSpeed: 5000,  accel: 50000,  lookahead: 50, runCurrent: 2500, genRate: 5,  modRate: 1, modAmp: 2  };

  var groups = {
    maxSpeed: ['maxSpeed', 'defMaxSpeed'], accel: ['accel', 'defAccel'],
    lookahead: ['lookahead', 'defLookahead'], runCurrent: ['runCurrent'],
    genRate: ['genRate'], modRate: ['modRate'], modAmp: ['modAmp']
  };
  for (var key in groups) {
    groups[key].forEach(function(id) {
      var s = $(id); if (!s) return;
      s.max = ex ? EM[key] : SM[key];
      if (parseFloat(s.value) > parseFloat(s.max)) s.value = s.max;
      s.dispatchEvent(new Event('input'));
    });
  }
  var eb = $('#expBanner'); if (eb) eb.classList.toggle('show', ex);
  var em = $('#expertMode'); if (em) em.checked = ex;
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

var overrideTimer = null;
function pushOverride() {
  clearTimeout(overrideTimer);
  // Live-override POST: only send the fields the firmware actually uses now.
  // lookahead/overshoot are gone — the firmware ignores unknown keys anyway,
  // but there's no point whispering dead commands into the void. :3
  overrideTimer = setTimeout(function() {
    post('/api/settings', {
      max_speed: parseInt($('maxSpeed').value), accel: parseInt($('accel').value),
      no_persist: true
    });
  }, 120);
}

export function restoreDefaults() {
  // Restore speed + accel from the saved-defaults sliders. lookahead/overshoot
  // sliders are legacy UI that §4 will clean up — skip them here so we don't
  // try to read elements that may already be gone. :3
  ['maxSpeed', 'accel'].forEach(function(id) {
    var cap = id.charAt(0).toUpperCase() + id.slice(1);
    var s = $(id), d = $('def' + cap);
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
    // blend_mode is included so the firmware persists the operator's chosen
    // reversal policy alongside speed/accel. Additive key — old firmware that
    // doesn't know about it just ignores it. :3
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
    var d = await get('/api/settings');
    if (!d || d.range_min === undefined) return;
    // Set TRAVEL FIRST so renderWindow() clamps against the real rail length,
    // not the hardcoded 240 default. If we set winMax=260 then renderWindow()
    // before setTravel(260), the clamp fires at 240 and silently truncates it. :3
    if (d.max_travel) setTravel(d.max_travel);
    setWinMin(d.range_min);
    setWinMax(d.range_max);
    setWindowReady(true);
    renderWindow();
    var dn = $('#defMinNum'); if (dn) dn.value = d.default_range_min || 0;
    var dx = $('#defMaxNum'); if (dx) dx.value = d.default_range_max || TRAVEL;
    expertMode = d.expert_mode || false;
    // Reflect the Intiface compat toggle — default OFF (MFP spec decode) when
    // the firmware doesn't advertise it. Note: explicit !! so an absent field
    // reads as unchecked rather than undefined. :3
    var ic = $('#intifaceCompat'); if (ic) ic.checked = !!d.intiface_compat;
    var spd = d.max_speed || 550, acc = d.accel || 1500;
    var look = d.lookahead || 20, over = d.overshoot || 8;
    ['defMaxSpeed', 'maxSpeed'].forEach(function(id) { var e = $(id); if (e) e.value = spd; });
    ['defAccel', 'accel'].forEach(function(id) { var e = $(id); if (e) e.value = acc; });
    ['defLookahead', 'lookahead'].forEach(function(id) { var e = $(id); if (e) e.value = look; });
    ['defOvershoot', 'overshoot'].forEach(function(id) { var e = $(id); if (e) e.value = over; });
    // Reflect the firmware's current blend mode — default to 1 (let-it-land)
    // if the field is absent (old firmware). The card is always shown; the
    // firmware just keeps pounding with whatever mode it was last told. :3
    reflectBlendMode(d.blend_mode || 1);
    applyExpertCaps();
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
    expertMode = eb.checked; applyExpertCaps();
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
  // Live Overrides sliders — update readout + gradient + push to firmware.
  ['maxSpeed', 'accel', 'lookahead', 'overshoot'].forEach(function(id) {
    var s = $(id); if (!s) return;
    s.addEventListener('input', function() {
      onLiveSlider(id === 'maxSpeed' ? 'speedVal' : id + 'Val', s, id);
      pushOverride();
    });
  });
  // Default Motion sliders — update readout + gradient only (no live push;
  // these are saved on "Save All Settings"). The readout IDs follow the
  // def<Cap>Val pattern (defSpeedVal, defAccelVal, etc.). :3
  [['defMaxSpeed','defSpeedVal'], ['defAccel','defAccelVal'],
   ['defLookahead','defLookaheadVal'], ['defOvershoot','defOvershootVal']].forEach(function(pair) {
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
    post('/api/settings', { blend_mode: blendMode, no_persist: true });
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
