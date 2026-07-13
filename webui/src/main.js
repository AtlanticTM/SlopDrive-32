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
import { initGenerator, startGenerator, stopGenerator, gen } from './features/generator.js';
import { currentMode, reflectMode, initSettings, saveSettings, restoreDefaults } from './features/settings.js';
import { pushTelemetryBatch, initMotionGraph } from './features/motiongraph.js';
import { fetchAndApplyCapabilities, refreshHealthCards } from './core/capabilities.js';

// Expose action handlers on window so [data-action] buttons wired by
// wireActions() can find them. These functions live in ES module scope,
// not global scope — we have to explicitly put them there. :3
window.nudgeWindow = nudgeWindow;
window.trim = trim;
window.setBound = setBound;
window.useCurrentAsDefault = useCurrentAsDefault;
window.saveSettings = saveSettings;
window.restoreDefaults = restoreDefaults;
// Expose the telemetry feed for console injection — lets us pump fake samples
// into the graph for screenshots when the hardware isn't around. :3
window.pushTelemetryBatch = pushTelemetryBatch;

export const state = { paused: false, override: false, ifActive: false, position: 0, homed: false };
let lastMoveSent = 0, posDragging = false;
// Last measured_stroke_mm seen in the status poll — used to detect when a
// homing cycle has produced a fresh rail measurement so we can resync. :3
let lastMeasuredStroke = -1;

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
// Monotonic telemetry cursor — the seq of the last sample we've already
// pulled. We hand it back to the firmware as ?since= so it only ships the
// NEW samples since our last poll. Nothing resent, nothing dropped. :3
var teleSince = 0;
// Poll mutex — the one rule that kills duplicate traces. pollStatus() is async
// and was fired on a flat setInterval, so if the ESP32 was busy (fat page
// transfer, BLE storm) a poll could still be in-flight when the next tick
// fired. Both requests then carried the SAME stale teleSince, the firmware
// re-shipped the SAME sample window, and motiongraph drew it twice — two lines
// gaping over each other. We refuse to start a second poll until the first
// finishes its business and pulls out. One hose at a time. :3
var pollInFlight = false;

// ---- Dropped-packet hardening (plan.md §6) ---------------------------------
// Exponential backoff on poll failure + stale-data indicator after N missed
// polls. When the firmware goes quiet (WiFi flap, reboot, out of range) we
// slow the polling cadence so we're not hammering an absent server, and show
// a "Stale data" banner the instant the data ages past the stale threshold.
// On the first successful poll after a failure streak, we snap back to the
// fast interval and clear the stale flag. :3
var pollFailures  = 0;          // consecutive failed polls
var pollBackoffMs = 100;        // current poll cadence (ms) — starts at 100ms
var POLL_NOMINAL  = 100;        // normal poll interval
var POLL_MAX      = 2000;       // backoff ceiling
var POLL_STALE_N  = 8;          // show stale indicator after this many misses
var pollLastSuccessMs = 0;      // performance.now() of last successful poll
var staleIndicatorShown = false;

function showStaleIndicator(on) {
  staleIndicatorShown = on;
  var sc = $('#staleChip');
  if (on) {
    if (!sc) {
      sc = document.createElement('span');
      sc.id = 'staleChip';
      sc.className = 'chip';
      sc.innerHTML = '<span class="dot bad"></span><span>Stale data</span>';
      var chips = document.querySelector('.chips');
      if (chips) chips.appendChild(sc);
    }
    sc.style.display = '';
  } else if (sc) {
    sc.style.display = 'none';
  }
}

async function pollStatus() {
  var nowMs = performance.now();
  // Backoff guard: skip polls when we're in a failure backoff window
  var backoffElapsed = nowMs - (pollLastSuccessMs || nowMs);
  if (pollFailures > 0 && backoffElapsed < pollBackoffMs) return;
  if (pollInFlight) return;
  pollInFlight = true;
  try {
    var d = await get('/api/status?since=' + teleSince); if (!d) {
      // fetch returned null — network error or firmware not responding
      pollFailures++;
      pollBackoffMs = Math.min(pollBackoffMs * 2, POLL_MAX);
      if (pollFailures >= POLL_STALE_N) showStaleIndicator(true);
      return;
    }
    // Successful poll — reset backoff, clear stale indicator
    pollFailures = 0;
    pollBackoffMs = POLL_NOMINAL;
    pollLastSuccessMs = performance.now();
    if (staleIndicatorShown) showStaleIndicator(false);

    state.position = d.position || 0; state.homed = !!d.homed;
    // ---- Live measured-stroke resync -------------------------------------
    // The firmware mirrors measured_stroke_mm into /api/status so we can
    // notice the moment homing produces a NEW measurement — even if we missed
    // the not-homed→homed edge (page already open, poll backoff, re-home).
    // Whenever the value changes we re-fetch capabilities, which rescales the
    // travel (mm) everywhere: endcap label, sliders, range designer, graph. :3
    if (typeof d.measured_stroke_mm === 'number' && d.measured_stroke_mm !== lastMeasuredStroke) {
      lastMeasuredStroke = d.measured_stroke_mm;
      fetchAndApplyCapabilities();
    }
    state.ifActive = !!d.buttplug_connected && d.measured_hz > 0;
    state.paused = !!d.paused; state.override = !!d.manual_override; reflectGating();
    var wd = $('#wifiDot'); if (wd) wd.className = 'dot ' + (d.wifi_connected ? 'good' : 'bad');
    setRead('wifiText', d.wifi_connected ? (d.ip || 'WiFi') : 'Off');
    var hd = $('#homeDot');
    if (d.homed) { if (hd) hd.className = 'dot good'; setRead('homeText', 'Homed'); }
    else { if (hd) hd.className = 'dot warn'; setRead('homeText', d.homing ? 'Homing' : 'Home'); }
    var tport = d.transport || 'WS';
    // DONGLE: linked when the UART has seen a TCode frame in the last 2s.
    // WS: linked when buttplug (MFP) is connected.
    // SER: linked when the serial handshake is complete.
    // BT: linked when a BLE client is connected. :3
    var linked = tport === 'SER'    ? !!d.serial_linked
               : tport === 'BT'     ? !!d.ble_connected
               : tport === 'DONGLE' ? !!d.dongle_active
               :                      !!d.buttplug_connected;
    var ifd = $('#ifDot'); if (ifd) ifd.className = 'dot ' + (linked ? 'good' : 'bad');
    setRead('ifText', linked ? (d.measured_hz > 0 ? d.measured_hz + 'Hz' : 'idle') : tport);

    // ---- Live motor-bus current chip (INA228 on the 57AIM board) ------------
    // Only show it when the firmware advertises a real sensor. The number is
    // the live gulp off the 5mΩ shunt — near-0A gliding free, climbing hard
    // when the carriage stuffs itself against a wall. Colour the dot amber past
    // ~8A and red past ~15A so a stall or bind is obvious at a glance. :3
    var curChip = $('#currentChip');
    if (curChip) {
        if (d.has_current_sensor) {
            curChip.style.display = '';
            var amps = (typeof d.bus_current_a === 'number') ? d.bus_current_a : 0;
            setRead('currentText', amps.toFixed(2) + ' A');
            var cd = $('#currentDot');
            if (cd) {
                var mag = Math.abs(amps);
                cd.className = 'dot ' + (mag >= 15 ? 'bad' : mag >= 8 ? 'warn' : 'good');
            }
        } else {
            curChip.style.display = 'none';
        }
    }

    // ---- Bus voltage chip (INA228, sibling to the current chip) -------------
    // Only shown when the firmware advertises a power monitor. Threshold
    // coloring per plan.md §4: green nominal, amber sag (<22V), red (<20V
    // under load) — a sagging 36V-nominal bus is an early warning the supply
    // or wiring can't keep up with the draw. :3
    var voltChip = $('#voltageChip');
    if (voltChip) {
        if (d.has_power_monitor) {
            voltChip.style.display = '';
            var volts = (typeof d.bus_voltage_v === 'number') ? d.bus_voltage_v : 0;
            setRead('voltageText', volts.toFixed(1) + ' V');
            var vd = $('#voltageDot');
            if (vd) {
                vd.className = 'dot ' + (volts < 20 ? 'bad' : volts < 22 ? 'warn' : 'good');
            }
        } else {
            voltChip.style.display = 'none';
        }
    }

    // ---- Refresh Health tab cards from the status payload ----------------
    // Same poll data that drives the toolbar chips also feeds the Load card
    // (INA228 power/temp/peak) and Link card (WiFi RSSI/channel/BSSID). :3
    refreshHealthCards(d);

    if (tport !== currentMode) reflectMode(tport);

    // Show Home button on both mobile FAB and desktop header button
    var hf = $('#homeFab'); if (hf) hf.classList.toggle('show', !d.homed);
    var hh = $('#homeBtnHeader'); if (hh) hh.style.display = d.homed ? 'none' : '';
    setRead('currentPos', (d.position || 0).toFixed(1));
    // ---- Feed the telemetry batch into the motion-graph playback engine. ----
    // The firmware ships only the samples NEW since our last poll — each one
    // is [actual_pos_mm, commanded_target_mm], spaced a fixed tele_dt (10ms)
    // apart. We advance our seq cursor (tele_seq) so the next poll asks for
    // exactly what comes after, and hand the batch to motiongraph.js which
    // replays it at that 10ms spacing on its own monotonic clock — no firmware
    // timestamps, so an ESP32 reboot can't desync the playback. When batches
    // flow, motiongraph owns posTarget via requestAnimationFrame; when the
    // machine is idle we fall back to the instant position. :3
    if (typeof d.tele_seq === 'number') teleSince = d.tele_seq;
    if (d.samples && d.samples.length) {
      // Hand the batch over WITH its absolute starting seq (tele_from). The
      // graph computes each sample's true seq as tele_from + index and refuses
      // to plot any it's already swallowed — belt-and-suspenders dedupe so even
      // a stale/overlapping response can never smear a second line on top. :3
      pushTelemetryBatch(d.samples, d.tele_dt || 10, d.tele_from);
    } else {
      setPosTarget(state.position);
    }

    if (!posDragging) { var mp = $('#manualPos'); if (mp) { mp.value = clamp(state.position, parseFloat(mp.min), parseFloat(mp.max)); setRead('manualPosVal', Math.round(state.position)); } }
    var sm = $('#serialModeBar'); var sd = $('#serialDot');
    if (d.serial_mode && sm) { sm.style.display = 'inline-flex'; if (sd) sd.className = 'dot ' + (d.serial_linked ? 'good' : 'warn'); }
    else if (sm) sm.style.display = 'none';
    setRead('measuredHz', d.measured_hz > 0 ? d.measured_hz : '--');
    setRead('measuredMs', d.measured_interval_ms > 0 ? d.measured_interval_ms : '--');
    if (d.buttplug_connected && !prevStatus.buttplug) toast('Intiface connected', 'good', 'i-link');
    if (!d.buttplug_connected && prevStatus.buttplug) toast('Intiface disconnected', 'warn', 'i-link');
    if (d.serial_linked && !prevStatus.serial) toast('Serial handshake', 'good', 'i-link');
    if (d.homed && !prevStatus.ready) {
      toast('Homed & ready', 'good', 'i-check');
      // Homing just completed — re-fetch capabilities so measured_stroke_mm
      // (the REAL usable rail length felt out by sensorless homing) rescales
      // TRAVEL, the range designer, and the motion graph live, without a
      // page reload. :3
      fetchAndApplyCapabilities();
    }
    prevStatus.buttplug = !!d.buttplug_connected; prevStatus.serial = !!d.serial_linked; prevStatus.ready = !!d.homed;
    if (gen.running && state.ifActive && !state.paused && !state.override) { stopGenerator(); var gn = $('#genNote'); if (gn) gn.textContent = 'Auto-paused'; }
  } catch (e) {
    // swallow — a single dropped poll is a non-event; the next one catches up.
  } finally {
    // ALWAYS release the hose, even if a poll bailed early (!d) or threw.
    // Forgetting this would leave pollInFlight stuck true and freeze all
    // future polling — a permanently limp graph. uhoh. :3
    pollInFlight = false;
  }
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
    cloneGraphCard();
  } else if (!e.matches && sidebarCloned) {
    restoreSidebarCards();
    restoreGraphCard();
  }
}

// ===================== Desktop graph-card movement =====================
// On desktop (≥1024px) we pull the motion graph card OUT of the Drive tab
// and into #graphPanelHost above the tab pills, so it's always visible
// regardless of which tab is active. On mobile it stays in the Drive tab.
// Uses same matchMedia pattern as the sidebar — appendChild moves the DOM
// node, event listeners stay attached, no deep-clone. :3

var graphCloned = false;

function cloneGraphCard() {
  var panel = $('#graphPanelHost');
  if (!panel) return;
  var graphCard = document.querySelector('[data-graph-card]');
  if (!graphCard || panel.contains(graphCard)) return;
  panel.appendChild(graphCard);
  graphCloned = true;
  // Desktop: non-collapsible, no caret, always fully open
  graphCard.classList.remove('collapsible');
  graphCard.classList.remove('collapsed');
  var caret = graphCard.querySelector('.caret');
  if (caret) caret.style.display = 'none';
  var body = graphCard.querySelector('.card-body');
  if (body) { body.style.maxHeight = 'none'; body.style.opacity = '1'; body.style.pointerEvents = 'auto'; }
}

function restoreGraphCard() {
  var driveTab = $('#drive');
  if (!driveTab) return;
  var panel = $('#graphPanelHost');
  if (!panel) return;
  var graphCard = panel.querySelector('[data-graph-card]');
  if (!graphCard) return;
  // Insert back into the Drive tab — after sidebar cards, before the generator
  driveTab.insertBefore(graphCard, driveTab.children[2] || null);
  graphCloned = false;
  // Restore collapsibility on mobile
  graphCard.classList.add('collapsible');
  var caret = graphCard.querySelector('.caret');
  if (caret) caret.style.display = '';
  var body = graphCard.querySelector('.card-body');
  if (body) { body.style.maxHeight = ''; body.style.opacity = ''; body.style.pointerEvents = ''; }
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
  initMotionGraph();    // batched-telemetry playback + canvas motion plot :3
  initFocusMode();
  initResizableSidebar();

  // Fetch /api/capabilities once at boot — patches every travel/ceiling/feature
  // DOM element in a single pass and builds Health-tab cards dynamically. Do
  // this BEFORE the first pollStatus() so the health cards exist when data
  // starts flowing in. :3
  fetchAndApplyCapabilities();

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
  startPosAnim();
  setInterval(function () { var lt = $('#log'), al = $('#autoLog'); if (lt && lt.classList.contains('active') && al && al.checked) refreshLog(); }, 2000);
  // Poll at ~100ms — the firmware's 10ms sampler stuffs ~10 fresh samples
  // into the ring between each poll, and we drain exactly those and replay
  // them 10ms apart. 100ms poll + 250ms ring = comfy headroom if a poll runs
  // late, and the motion graph glides instead of stuttering at the wire rate. :3
  setInterval(pollStatus, 100); pollStatus(); refreshLog();

}
init();