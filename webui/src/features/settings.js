/**
 * Settings tab — transport mode, defaults, timing, generation rate.
 * Uses setWinMin/setWinMax to avoid ESM import-reassignment errors. :3
 */
import { $, setRead, onLiveSlider, clamp, icon, toast } from '../core/ui.js';
import { post, get } from '../core/api.js';
import { TRAVEL, winMin, winMax, setWinMin, setWinMax, setWindowReady, renderWindow, useCurrentAsDefault } from '../core/range.js';

export var currentMode = 'WS';

var MODE_HINTS = {
  WS: 'WebSocket — Intiface over WiFi',
  SER: 'Serial — Intiface over USB',
  BT: 'Bluetooth — BLE serial'
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
  var EM = { maxSpeed: 3000, accel: 8000, lookahead: 80, runCurrent: 3000, genRate: 50, modRate: 5, modAmp: 10 };
  var SM = { maxSpeed: 1500, accel: 4000, lookahead: 50, runCurrent: 2500, genRate: 5, modRate: 1, modAmp: 2 };
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

var overrideTimer = null;
function pushOverride() {
  clearTimeout(overrideTimer);
  overrideTimer = setTimeout(function() {
    post('/api/settings', {
      max_speed: parseInt($('maxSpeed').value), accel: parseInt($('accel').value),
      lookahead: parseInt($('lookahead').value), overshoot: parseInt($('overshoot').value),
      no_persist: true
    });
  }, 120);
}

export function restoreDefaults() {
  ['maxSpeed', 'accel', 'lookahead', 'overshoot'].forEach(function(id) {
    var cap = id.charAt(0).toUpperCase() + id.slice(1);
    var s = $(id), d = $('def' + cap);
    if (s && d) s.value = d.value;
    if (s) s.dispatchEvent(new Event('input'));
  });
  pushOverride();
  toast('Restored saved defaults', 'info', 'i-reset');
}

export async function saveSettings(silent) {
  if (winMin >= winMax) { if (!silent) alert('Min must be less than Max'); return; }
  try {
    await post('/api/settings', {
      range_min: Math.round(winMin), range_max: Math.round(winMax),
      max_speed: parseInt($('defMaxSpeed').value), accel: parseInt($('defAccel').value),
      lookahead: parseInt($('defLookahead').value), overshoot: parseInt($('defOvershoot').value),
      auto_duration: $('#autoDuration').checked,
      default_range_min: clamp(parseInt($('defMinNum').value) || 0, 0, TRAVEL),
      default_range_max: clamp(parseInt($('defMaxNum').value) || TRAVEL, 0, TRAVEL),
      expert_mode: expertMode
    });
    ['saveInd', 'saveIndRange'].forEach(function(id) {
      var i = $(id);
      if (i) { i.classList.add('show'); setTimeout(function() { i.classList.remove('show'); }, 1800); }
    });
    if (!silent) toast('Settings saved', 'good', 'i-check');
  } catch (e) { if (!silent) toast('Failed to save', 'bad', 'i-alert'); }
}

async function loadSettings() {
  try {
    var d = await get('/api/settings');
    if (!d || d.range_min === undefined) return;
    setWinMin(d.range_min);
    setWinMax(d.range_max);
    setWindowReady(true);
    renderWindow();
    var dn = $('#defMinNum'); if (dn) dn.value = d.default_range_min || 0;
    var dx = $('#defMaxNum'); if (dx) dx.value = d.default_range_max || 240;
    expertMode = d.expert_mode || false;
    var ad = $('#autoDuration'); if (ad) ad.checked = d.auto_duration || true;
    var spd = d.max_speed || 550, acc = d.accel || 1500;
    var look = d.lookahead || 20, over = d.overshoot || 8;
    ['defMaxSpeed', 'maxSpeed'].forEach(function(id) { var e = $(id); if (e) e.value = spd; });
    ['defAccel', 'accel'].forEach(function(id) { var e = $(id); if (e) e.value = acc; });
    ['defLookahead', 'lookahead'].forEach(function(id) { var e = $(id); if (e) e.value = look; });
    ['defOvershoot', 'overshoot'].forEach(function(id) { var e = $(id); if (e) e.value = over; });
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
  ['maxSpeed', 'accel', 'lookahead', 'overshoot'].forEach(function(id) {
    var s = $(id); if (!s) return;
    s.addEventListener('input', function() {
      onLiveSlider(id === 'maxSpeed' ? 'speedVal' : id + 'Val', s, id);
      pushOverride();
    });
  });
  var rb = document.querySelector("[data-action='restoreDefaults']");
  if (rb) rb.addEventListener('click', restoreDefaults);
  var uc = document.querySelector("[data-action='useCurrentAsDefault']");
  if (uc) uc.addEventListener('click', useCurrentAsDefault);
  var ad = $('#autoDuration');
  if (ad) ad.addEventListener('change', pushInterp);
  loadSettings(); loadMode(); loadInterp(); loadGenTick();
}