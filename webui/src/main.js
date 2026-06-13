/**
 * SlopDrive-32 WebUI — main entry point. :3
 * CSS must be imported here so Vite/vite-plugin-singlefile inlines it. :3
 *
 * Desktop layout: on ≥1024px screens the Stroke Window + Position cards get
 * cloned into a permanent left sidebar. The right panel hosts the tab system
 * (Drive / Health / Settings / Log). On mobile (<1024px) everything stays in
 * the single-column scroll — the sidebar is hidden and the bottom tab bar runs
 * the show.
 *
 * Focus Mode: strips secondary chrome out of the UI so the top can focus on
 * the pounding. Toggled from the header button; persisted to localStorage.
 *
 * Resizable sidebar: a drag handle between sidebar and main panel lets the
 * operator decide how wide they want their stroke window to be. Width is
 * persisted to localStorage so it survives reloads. :3
 */
import './style.css';
import { injectIcons, initTabs, initCollapsibleCards, initTooltips, wireActions, toast, icon, $, setRead, clamp, onLiveSlider, paintSlider } from './core/ui.js';
import { post, get, getText } from './core/api.js';
import { TRAVEL, initRangeDesigner, renderWindow, setPosTarget, startPosAnim, syncManualWindow, nudgeWindow, trim, setBound, useCurrentAsDefault } from './core/range.js';
import { initGenerator, startGenerator, stopGenerator, toggleWaveViz, gen } from './features/generator.js';
import { currentMode, reflectMode, initSettings, saveSettings, restoreDefaults } from './features/settings.js';

// Expose action handlers on window so [data-action] buttons wired by
// wireActions() can find them. These functions live in ES module scope,
// not global scope — we have to explicitly put them there. :3
window.nudgeWindow = nudgeWindow;
window.trim = trim;
window.setBound = setBound;
window.useCurrentAsDefault = useCurrentAsDefault;
window.saveSettings = saveSettings;
window.restoreDefaults = restoreDefaults;

export const state = { paused: false, override: false, ifActive: false, position: 0, homed: false };
let lastMoveSent = 0, posDragging = false;

function reflectGating() {
  var bp = $('#btnPause'); if (bp) bp.classList.toggle('on', state.paused);
  var ps = $('#pauseSub'); if (ps) ps.textContent = state.paused ? 'Intiface paused' : 'ignore Intiface';
  var ob = $('#ovBanner'); if (ob) ob.classList.toggle('show', state.override);
  var pb = $('#pauseBanner'); if (pb) pb.classList.toggle('show', state.paused);
  var ot = $('#overrideTog'); if (ot) ot.checked = state.override;
}

async function togglePause() {
  var next = !state.paused; await post('/api/pause', { paused: next });
  state.paused = next; reflectGating(); if (next) stopGenerator();
}
async function halt() { await post('/api/halt', {}); stopGenerator(); toast('Motion halted', 'warn', 'i-stop'); }
async function estop() {
  await post('/api/stop', {}); stopGenerator();
  state.paused = false; state.override = false; reflectGating();
  toast('E-STOP — power cut, re-home required', 'bad', 'i-alert', 6000);
}
async function toggleOverride() {
  var ov = $('#overrideTog'); var on = ov ? ov.checked : false;
  await post('/api/override', { override: on }); state.override = on; reflectGating();
}
// Home button flavor: the machine is finding its happy place. Bottoming out,
// getting into position — this is the submissive ritual before the thrusting
// begins. :3
async function moveToHome() { await post('/api/home', {}); toast('Homing…', 'info', 'i-home'); }
async function clearFault() {
  try { await post('/api/clearfault', {}); var fb = $('#faultBanner'); if (fb) fb.classList.remove('show'); toast('Fault cleared', 'info', 'i-check'); } catch (e) {}
}
function sendMove(pos, force) {
  var now = performance.now();
  if (!force && now - lastMoveSent < 50) return; lastMoveSent = now;
  var bp = $('#bypassLimits');
  post('/api/move', { position: Math.round(pos * 10) / 10, stream: !force, bypass_limits: bp ? bp.checked : false });
}

var prevStatus = { buttplug: false, serial: false, ready: false };
async function pollStatus() {
  try {
    var d = await get('/api/status'); if (!d) return;
    state.position = d.position || 0; state.homed = !!d.homed;
    state.ifActive = !!d.buttplug_connected && d.measured_hz > 0;
    state.paused = !!d.paused; state.override = !!d.manual_override; reflectGating();
    var wd = $('#wifiDot'); if (wd) wd.className = 'dot ' + (d.wifi_connected ? 'good' : 'bad');
    setRead('wifiText', d.wifi_connected ? (d.ip || 'WiFi') : 'Off');
    var hd = $('#homeDot');
    if (d.homed) { if (hd) hd.className = 'dot good'; setRead('homeText', 'Homed'); }
    else { if (hd) hd.className = 'dot warn'; setRead('homeText', d.homing ? 'Homing' : 'Home'); }
    var tport = d.transport || 'WS';
    var linked = tport === 'SER' ? !!d.serial_linked : tport === 'BT' ? !!d.ble_connected : !!d.buttplug_connected;
    var ifd = $('#ifDot'); if (ifd) ifd.className = 'dot ' + (linked ? 'good' : 'bad');
    setRead('ifText', linked ? (d.measured_hz > 0 ? d.measured_hz + 'Hz' : 'idle') : tport);
    if (tport !== currentMode) reflectMode(tport);
    // Show Home button on both mobile FAB and desktop header button
    var hf = $('#homeFab'); if (hf) hf.classList.toggle('show', !d.homed);
    var hh = $('#homeBtnHeader'); if (hh) hh.style.display = d.homed ? 'none' : '';
    setRead('currentPos', (d.position || 0).toFixed(1)); setPosTarget(state.position);
    if (!posDragging) { var mp = $('#manualPos'); if (mp) { mp.value = clamp(state.position, parseFloat(mp.min), parseFloat(mp.max)); setRead('manualPosVal', Math.round(state.position)); } }
    var sm = $('#serialModeBar'); var sd = $('#serialDot');
    if (d.serial_mode && sm) { sm.style.display = 'inline-flex'; if (sd) sd.className = 'dot ' + (d.serial_linked ? 'good' : 'warn'); }
    else if (sm) sm.style.display = 'none';
    setRead('measuredHz', d.measured_hz > 0 ? d.measured_hz : '--');
    setRead('measuredMs', d.measured_interval_ms > 0 ? d.measured_interval_ms : '--');
    if (d.buttplug_connected && !prevStatus.buttplug) toast('Intiface connected', 'good', 'i-link');
    if (!d.buttplug_connected && prevStatus.buttplug) toast('Intiface disconnected', 'warn', 'i-link');
    if (d.serial_linked && !prevStatus.serial) toast('Serial handshake', 'good', 'i-link');
    if (d.homed && !prevStatus.ready) toast('Homed & ready', 'good', 'i-check');
    prevStatus.buttplug = !!d.buttplug_connected; prevStatus.serial = !!d.serial_linked; prevStatus.ready = !!d.homed;
    if (gen.running && state.ifActive && !state.paused && !state.override) { stopGenerator(); var gn = $('#genNote'); if (gn) gn.textContent = 'Auto-paused'; }
  } catch (e) {}
}

async function refreshLog() {
  try { var txt = await getText('/api/log'); var box = $('#logBox'); if (!box) return; var atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 30; box.textContent = txt || '(no output)'; if (atBottom) box.scrollTop = box.scrollHeight; } catch (e) {}
}

// ===================== Focus Mode =====================
// Strips the UI down to the essentials — stroke window, position slider,
// generator start/stop, and transport. Everything else gets the leash
// yanked from under it. Persisted to localStorage so it survives reloads
// and the vibe is maintained across sessions. :3

const FOCUS_STORAGE_KEY = 'sd32_focus';

function isFocusOn() {
  return document.body.classList.contains('focus-mode');
}

function applyFocusState(on) {
  document.body.classList.toggle('focus-mode', on);
  var fb = $('#btnFocus');
  if (fb) fb.classList.toggle('on', on);
  try { localStorage.setItem(FOCUS_STORAGE_KEY, on ? '1' : '0'); } catch (e) {}
}

function toggleFocus() {
  applyFocusState(!isFocusOn());
}

function initFocusMode() {
  var saved = '0';
  try { saved = localStorage.getItem(FOCUS_STORAGE_KEY) || '0'; } catch (e) {}
  if (saved === '1') applyFocusState(true);

  var fb = $('#btnFocus');
  if (fb) fb.addEventListener('click', toggleFocus);
}

// ===================== Desktop sidebar card cloning =====================
// On desktop (≥1024px) we move the Stroke Window + Position cards from the
// Drive tab-content into the permanent sidebar. These cards carry
// [data-sidebar-card] attributes. On mobile the cards stay right where they
// are — the sidebar is just hidden via CSS.
//
// Uses matchMedia so the cards only get cloned when we're actually on a
// desktop viewport. The responsive guard listens for resize events so the
// cards move back if the window is resized below the breakpoint. :3
// This runs AFTER all modules have wired their event listeners, so the cloned
// cards keep their baked-in DOM event bindings (e.g. the range designer drag
// events). We use appendChild which MOVES the nodes — no deep-clone needed,
// event listeners stay attached. :3

var SIDEBAR_BP = '(min-width: 1024px)';
var sidebarCloned = false;

function cloneSidebarCards() {
  var sidebarCards = $('#sidebarCards');
  if (!sidebarCards) return;
  var cards = document.querySelectorAll('[data-sidebar-card]');
  cards.forEach(function (card) {
    sidebarCards.appendChild(card);
  });
  sidebarCloned = true;
  initCollapsibleCards();
  // Re-wire action buttons now that cards have moved into the sidebar
  wireActions();
}

function restoreSidebarCards() {
  var driveTab = $('#drive');
  if (!driveTab) return;
  var sidebarCards = $('#sidebarCards');
  if (!sidebarCards) return;
  // Move cards back from sidebar to their original spots inside the drive tab
  var cards = sidebarCards.querySelectorAll('[data-sidebar-card]');
  // Insert them at the top of the drive tab (before the Motion Generator card)
  cards.forEach(function (card) {
    driveTab.insertBefore(card, driveTab.firstChild);
  });
  sidebarCloned = false;
  initCollapsibleCards();
  wireActions();
}

function handleSidebarBreakpoint(e) {
  if (e.matches && !sidebarCloned) {
    cloneSidebarCards();
  } else if (!e.matches && sidebarCloned) {
    restoreSidebarCards();
  }
}

// ===================== Resizable sidebar =====================
// The grab handle between the sidebar and main panel lets the operator
// drag to resize. Width is clamped between 260px and 600px, and persisted
// to localStorage so your preferred stroke-window thickness survives a
// reload. :3

var SIDEBAR_WIDTH_KEY = 'sd32_sidebar_w';

function initResizableSidebar() {
  var sidebar = $('#sidebarHost');
  var handle = $('#resizeHandle');
  if (!sidebar || !handle) return;

  // Load saved width
  var savedW = 340;
  try { savedW = parseInt(localStorage.getItem(SIDEBAR_WIDTH_KEY)) || 340; } catch (e) {}
  savedW = clamp(savedW, 260, 600);
  sidebar.style.flexBasis = savedW + 'px';

  var dragging = false, startX = 0, startW = 0;

  function onDown(e) {
    dragging = true;
    startX = e.clientX;
    startW = sidebar.getBoundingClientRect().width;
    document.body.style.cursor = 'col-resize';
    handle.classList.add('active');
    e.preventDefault();
  }

  function onMove(e) {
    if (!dragging) return;
    var delta = e.clientX - startX;
    var newW = clamp(startW + delta, 260, 600);
    sidebar.style.flexBasis = newW + 'px';
  }

  function onUp() {
    if (!dragging) return;
    dragging = false;
    document.body.style.cursor = '';
    handle.classList.remove('active');
    var w = sidebar.getBoundingClientRect().width;
    try { localStorage.setItem(SIDEBAR_WIDTH_KEY, Math.round(w)); } catch (e) {}
    // Trigger window resize so canvas/range designer re-measures
    window.dispatchEvent(new Event('resize'));
  }

  handle.addEventListener('pointerdown', onDown);
  window.addEventListener('pointermove', onMove);
  window.addEventListener('pointerup', onUp);
  window.addEventListener('pointercancel', onUp);
}

function init() {
  injectIcons();

  // Desktop sidebar clone — guarded by matchMedia so it only fires on
  // desktop viewports. The listener handles resize crossings. :3
  var mq = window.matchMedia(SIDEBAR_BP);
  handleSidebarBreakpoint(mq);
  mq.addEventListener('change', handleSidebarBreakpoint);

  initTabs(); initCollapsibleCards(); initTooltips(); wireActions();
  initRangeDesigner(); renderWindow(); initGenerator(); initSettings();
  initFocusMode();
  initResizableSidebar();

  if (import.meta.env.VITE_BLE_ENABLED === 'true') { var bt = $('#btModeBtn'); if (bt) bt.style.display = ''; }
  var bP = $('#btnPause'); if (bP) bP.addEventListener('click', togglePause);
  var hb = document.querySelector('.tbtn.halt'); if (hb) hb.addEventListener('click', halt);
  var eb = document.querySelector('.tbtn.estop'); if (eb) eb.addEventListener('click', estop);
  var ot = $('#overrideTog'); if (ot) ot.addEventListener('change', toggleOverride);
  var bl = $('#bypassLimits'); if (bl) bl.addEventListener('change', function () { syncManualWindow(); });
  var mp = $('#manualPos');
  if (mp) { mp.addEventListener('pointerdown', function () { posDragging = true; }); mp.addEventListener('pointerup', function () { posDragging = false; sendMove(parseFloat(mp.value), true); }); mp.addEventListener('input', function () { setRead('manualPosVal', Math.round(this.value)); sendMove(parseFloat(this.value)); }); }
  // Wire both Home buttons — the mobile FAB and the desktop header button
  var hf = $('#homeFab'); if (hf) hf.addEventListener('click', moveToHome);
  var hh = $('#homeBtnHeader'); if (hh) hh.addEventListener('click', moveToHome);
  var fb = document.querySelector('#faultBanner button'); if (fb) fb.addEventListener('click', clearFault);
  var rl = $('#refreshLogBtn'); if (rl) rl.addEventListener('click', refreshLog);
  toggleWaveViz(); startPosAnim();
  setInterval(function () { var lt = $('#log'), al = $('#autoLog'); if (lt && lt.classList.contains('active') && al && al.checked) refreshLog(); }, 2000);
  setInterval(pollStatus, 300); pollStatus(); refreshLog();
}
init();