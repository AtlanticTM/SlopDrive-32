/**
 * SlopDrive-32 WebUI — main entry point.
 * CSS must be imported here so Vite/vite-plugin-singlefile inlines it.
 *
 * Elastic-rack layout (>=761px): Drive (Pattern + Power cluster) is always
 * visible; Settings/Log open as a sticky side pane beside it (pane-controller
 * lives in ui.js's initTabs()). Below 761px it's classic three-tab with a
 * fixed bottom bar. See SD32-PAGE-SYSTEM-SONNET-PROMPT.md §1.6b.
 *
 * Task 7: binary WS transport via link.js, clock-synced telebuf.js, control
 * plane via cmd.js. HTTP polling is the fallback when SlopSync is not LIVE.
 * Header chips + Health stats are fed from 0x02 STATUS frames when WS is live.
 */
console.log('[SD32] booting — imports loaded');
import './style.css';
import { injectIcons, initTabs, initCollapsibleCards, initTooltips, initHoverTips, initHivis, applyHivis, hivisInitialState, wireActions, toast, $, setRead, clamp, onLiveSlider, paintSlider, pad, setVV, setVVState, measureMartianMonoCh, renumberPanels } from './core/ui.js';
import { post, get, getText } from './core/api.js';
import { TRAVEL, renderWindow, setPosTarget, syncManualWindow, nudgeWindow, trim, setBound, useCurrentAsDefault, initWindowInputs, winMin, winMax, setWinMin, setWinMax, setWindowReady } from './core/range.js';
import { initPattern, startPattern, stopPattern, pat, refreshPatternState, setPatternButton, rebuildPatternGrid } from './features/pattern.js';
import { currentMode, reflectMode, initSettings, saveSettings, restoreDefaults } from './features/settings.js';
import { initRail, railUpdatePosition, setRailManualMode } from './features/rail.js';
import { initPlanStrip } from './features/planstrip.js';
import { initDiag } from './features/diag.js';
import { initConn } from './features/conn.js';
import { applyTheme, currentThemeId, initThemeUI, ACCENT } from './core/theme.js';
import { fetchAndApplyCapabilities, refreshHealthCards, setLinkBssid } from './core/capabilities.js';
import { initServo } from './features/servo.js';

// Apply the stored theme IMMEDIATELY at module evaluation — before init()
// and the first canvas frame — so there's no flash of default accents.
applyTheme(currentThemeId());
// Same for high-legibility (L2d) — set the html.hivis class before first paint
// so brightened text/tokens don't flash in from the default ramp. initHivis()
// re-asserts and wires the controls once the DOM is ready.
applyHivis(hivisInitialState());

// ---- Task 7: new core modules ---------------------------------------------
import { initTeleBuf, feedHttpSamples, sampleAt, stableRenderTime,
         updateRenderDelay, onStale, onStaleSuspended, onFresh, isSuspended,
         setTeleFallback, noteLinkAlive, getBufferStats } from './core/telebuf.js';
import * as cmd from './core/cmd.js';
import {
  OP_SET_WINDOW, OP_SET_SPEED, OP_SET_ACCEL, OP_GEN_CFG, OP_GEN_RUN,
  OP_MODE, OP_BLEND, OP_PAUSE, OP_HALT, OP_ESTOP, OP_HOME,
  OP_OVERRIDE, OP_BYPASS, OP_CLEAR_FAULT, OP_SAVE, OP_MOVE,
  OP_STREAM_MODE, OP_OVERSHOOT, OP_GET_CFG, OP_HOME_OVERRIDE,
  FLAG_HOMED, FLAG_GEN_RUNNING, FLAG_ESTOP, FLAG_PAUSED,
  FLAG_OVERRIDE, FLAG_INTIFACE_ACTIVE,
  SPEED_CEILING_PEGGED, SPEED_VELOCITY_MATCHED
} from './core/wire.js';
import { initShadow, processEcho, processConfig, tick as shadowTick,
         wireStaleness, notifyRecovery } from './core/shadow.js';
import { initSlopSyncBridge, isSlopSyncLive } from './core/slopsync/bridge.js';
import { SAFETY_OP } from './core/slopsync/frames.js';

// Expose action handlers on window so [data-action] buttons wired by
// wireActions() can find them. These functions live in ES module scope,
// not global scope — we have to explicitly put them there.
window.nudgeWindow = nudgeWindow;
window.trim = trim;
window.setBound = setBound;
window.useCurrentAsDefault = useCurrentAsDefault;
window.saveSettings = saveSettings;
window.restoreDefaults = restoreDefaults;
window.__sendMove = sendMove;

export const state = { paused: false, override: false, ifActive: false, position: 0, homed: false, estopped: false };

// ---- Latest 0x04 INTERP snapshot (interpolator debug) ----------------------
// Updated at ~45Hz from the WS interp frame. Exposed on window for the debug
// overlay / rail renderer to read without a per-frame callback chain.
export const interpState = {
  active: false, liveMode: false, gradMode: false, style: 0, styleName: '-',
  startPos: 0.5, endPos: 0.5, curPos: 0.5, curVel: 0, durationUs: 0, elapsedUs: 0,
  lastRxMs: 0
};
window.__INTERP = interpState;

// ---- 0x05 ANOMALY event log (interpolator path anomalies) ------------------
// Event-driven, not periodic. The firmware fires one frame per anomaly the
// interpolator invents (Overshoot / PointDropped / DecelOverrun / DurFallback)
// — exactly the events behind the v3/v4 stutter + slow-speed dropout. We keep a
// bounded ring here for the Log/Health surface. Newest at the FRONT (unshift).
export const ANOMALY_LOG_CAP = 100;
export const anomalyLog = [];
// Rolling per-kind counters so the operator sees frequency, not just the tail.
export const anomalyCounts = { Overshoot: 0, PointDropped: 0, DecelOverrun: 0, DurFallback: 0 };
window.__ANOMALY_LOG = anomalyLog;
window.__ANOMALY_COUNTS = anomalyCounts;

// Send the stream speed-feed A/B mode (0=ceiling-pegged, 1=velocity-matched).
function sendStreamMode(mode) {
  cmd.send(OP_STREAM_MODE, { mode: mode });
}
window.__sendStreamMode = sendStreamMode;

// Send the v4 overshoot-clamp toggle (monotone Fritsch–Carlson tangent limiter).
// on=true → the interpolator can never bulge the cubic past [start,end]; kills
// the invented overshoot-then-return micromotion. Firmware echoes overshoot_clamp.
function sendOvershoot(on) {
  cmd.send(OP_OVERSHOOT, { on: !!on });
}
window.__sendOvershoot = sendOvershoot;

// ===================== Header activity heatmap ==============================
// 14x3 netdata-style canvas in the header (spec §1.6d). Rows: |velocity|
// (from real position samples in the rail telemetry loop), bus current (the
// same live value the header current chip shows), link activity (WS frame
// arrival rate). Real telemetry only — no synthetic fill, no decoration.
// ~220ms bucket cadence via setInterval, not per rAF frame.
var AG_COLS = 14, AG_ROWS = 3, AG_CELL = 4, AG_GAP = 1;
var agData = [];
for (var _agi = 0; _agi < AG_COLS; _agi++) agData.push([0, 0, 0]);
var agCanvas = null, agCtx = null;
var _agLastPos = null, _agVelEma = 0;
var _agLastAmps = 0;
var _agLinkMsgs = 0;

function _agNoteVel(pos) {
  if (_agLastPos !== null) {
    var dv = Math.abs(pos - _agLastPos);
    _agVelEma = _agVelEma * 0.7 + dv * 0.3;
  }
  _agLastPos = pos;
}
function _agNoteLinkMsg() { _agLinkMsgs++; }

function initActivityGrid() {
  agCanvas = $('#actGrid');
  if (!agCanvas) return;
  agCtx = agCanvas.getContext('2d');
  setInterval(function () {
    if (!agCtx) return;
    // Heuristic ceilings so a fully-lit cell means "clearly moving/loaded",
    // not "hit some invented max" — velEma is mm of travel per ~16ms tick,
    // ampFrac reuses the same 12A header-chip badge scale, linkFrac assumes
    // a healthy multi-frame-per-bucket telemetry rate.
    var velFrac = Math.min(1, _agVelEma / 3);
    var ampFrac = Math.min(1, Math.abs(_agLastAmps) / 12);
    var linkFrac = Math.min(1, _agLinkMsgs / 4);
    _agLinkMsgs = 0;
    agData.shift();
    agData.push([velFrac, ampFrac, linkFrac]);
    agCtx.clearRect(0, 0, agCanvas.width, agCanvas.height);
    for (var c = 0; c < AG_COLS; c++) {
      for (var r = 0; r < AG_ROWS; r++) {
        var v = agData[c][r];
        var a = 0.06 + v * 0.85;
        agCtx.fillStyle = 'rgba(' + ACCENT.realityRgb + ',' + a.toFixed(2) + ')';
        agCtx.fillRect(c * (AG_CELL + AG_GAP), r * (AG_CELL + AG_GAP + 1), AG_CELL, AG_CELL);
      }
    }
  }, 220);
}

// ---- HTTP fallback poll control -------------------------------------------
var _fallbackPollInterval = null;
var _wsLive = false;             // true when WS is connected and not in fallback
var teleSince = 0;
var pollInFlight = false;

// ---- 0x02 status cache (updated by WS status frames) ----------------------
var _wsStatusCache = null;

// ---- Live bus current from 0x01 telemetry (mA). The 0x02 peak_mA field is a
// peak-hold value that spikes once a second — never show it as "draw".
var _liveBusmA = 0;
// Latest bus voltage (V) — from 0x02 STATUS in WS mode, /api/status in HTTP
// fallback. Feeds the diag graph's power supplier alongside _liveBusmA.
var _lastBusV = 0;

// ---- Staleness flag exposed for cmd.js suspension -------------------------
window.__CMD_SUSPENDED = false;

// ---- Polling backoff (used in fallback mode) ------------------------------
let pollFailures  = 0;
let pollBackoffMs = 100;
const POLL_NOMINAL  = 100;
const POLL_MAX      = 500;   // capped at 500ms for HTTP fallback (A-008)
const POLL_STALE_N  = 8;
let pollSuccessStreak = 0;   // consecutive successes for hysteresis reset (A-008)
let pollAttemptCount  = 0;   // total attempts for fast-probe every 5th (A-008)
let pollLastSuccessMs = 0;
let staleIndicatorShown = false;

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

// ===================== State reflection =====================================

function reflectGating() {
  var bp = $('#btnPause'); if (bp) bp.classList.toggle('on', state.paused);
  var ps = $('#pauseSub'); if (ps) ps.textContent = state.paused ? 'Intiface paused' : 'ignore Intiface';
  var ob = $('#ovBanner'); if (ob) ob.classList.toggle('show', state.override);
  var pb = $('#pauseBanner'); if (pb) pb.classList.toggle('show', state.paused);
  var ot = $('#overrideTog'); if (ot) ot.checked = state.override;
}

// ===================== Actions (routed through cmd.js) ======================

// Ground Truth Doctrine: none of these mutate state.* locally. The pause pill /
// override checkbox / banners render only from CONFIRMED device state — the
// next 0x01 flags frame (~45Hz) or /api/status poll updates them, so a
// rejected command can never leave the UI lying about machine state. :3
function togglePause() {
  var next = !state.paused;
  // SlopSync safety-intents 0x0005 when live (pause/resume), legacy op fallback.
  var ss = window.__slopsync;
  if (!(ss && ss.isLive() && ss.sendSafety(next ? SAFETY_OP.pause : SAFETY_OP.resume)))
    cmd.send(OP_PAUSE, { paused: next });
  if (next) stopPattern();
}
function halt() {
  var ss = window.__slopsync;
  if (!(ss && ss.isLive() && ss.sendSafety(SAFETY_OP.stop))) cmd.send(OP_HALT, {});
  stopPattern();
  toast('Halt sent', 'warn', 'i-stop');
}
function estop() {
  // SlopSync safety_ops::estop (6) on 0x0005 — the hub treats it exactly as a
  // valid 0xE5 frame (latch, cause=user, publish 0x0003). The session escalates
  // to the raw 0xE5 plane by itself if it is connected but not yet LIVE, so a
  // press is never silently swallowed. Legacy op only when there is no session.
  var ss = window.__slopsync;
  if (!(ss && ss.sendEstop && ss.sendEstop())) cmd.send(OP_ESTOP, {});
  stopPattern();
  toast('E-STOP sent — waiting for device confirmation', 'bad', 'i-alert', 6000);
}
function toggleOverride() {
  var ov = $('#overrideTog'); var on = ov ? ov.checked : false;
  // SlopSync 0x0005 ops override_on/off (RFC-025c: override is SAFETY-domain
  // state, carried on the 0x0003 snapshot's appended `modes` byte).
  var ss = window.__slopsync;
  if (!(ss && ss.isLive() && ss.sendOverride && ss.sendOverride(on))) cmd.send(OP_OVERRIDE, { on: on });
}
function moveToHome() {
  // SlopSync home 0x0103 {1:op=1} when live, legacy op fallback.
  var ss = window.__slopsync;
  if (!(ss && ss.isLive() && ss.sendHome(1))) cmd.send(OP_HOME, {});
  toast('Homing…', 'info', 'i-home');
}
function clearFault() {
  // The banner is driven by the device's latched e-stop flag (FLAG_ESTOP) —
  // never hidden locally. Exiting the e-stopped state requires a re-home, so
  // that's what the banner button requests; the banner drops when the device
  // confirms via the flags frame. :3
  cmd.send(OP_CLEAR_FAULT, {});
  moveToHome();
}

var lastMoveSent = 0;
function sendMove(pos, force) {
  var now = performance.now();
  if (!force && now - lastMoveSent < 50) return; lastMoveSent = now;
  var bp = $('#bypassLimits');
  var bypass = bp ? bp.checked : false;
  // SlopSync move 0x0100 {1:posMm,2:bypass} when live, legacy op fallback.
  var ss = window.__slopsync;
  if (ss && ss.isLive() && ss.sendMove(Math.round(pos * 10) / 10, bypass)) return;
  cmd.send(OP_MOVE, {
    position: Math.round(pos * 10) / 10,
    stream: !force,
    bypass_limits: bypass
  });
}

// ===================== HTTP poll (fallback mode only) ========================

var prevStatus = { buttplug: false, serial: false, ready: false };
var lastMeasuredStroke = -1;

// ---- SESSION card (0x06 STATS frame / /api/status fallback) ----------------
function _fmtDist(mm) {
  if (!(mm > 0)) return '0 mm';
  if (mm < 1000) return Math.round(mm) + ' mm';
  if (mm < 1e6)  return (mm / 1000).toFixed(2) + ' m';
  return (mm / 1e6).toFixed(2) + ' km';
}
function _fmtEnergy(wh) {
  if (!(wh > 0)) return '0 Wh';
  if (wh < 1)    return (wh * 1000).toFixed(0) + ' mWh';
  if (wh < 1000) return wh.toFixed(2) + ' Wh';
  return (wh / 1000).toFixed(2) + ' kWh';
}
function _fmtDur(ms) {
  var s = Math.floor((ms || 0) / 1000);
  var h = Math.floor(s / 3600); s -= h * 3600;
  var m = Math.floor(s / 60);   s -= m * 60;
  if (h > 0) return h + 'h ' + m + 'm';
  if (m > 0) return m + 'm ' + s + 's';
  return s + 's';
}
// ---- L3 — session-time clock ----------------------------------------------
// #sessTime shows the DEVICE session odometer (session_ms since boot / last
// Reset — the semantic this build intends: it survives a WS drop/reconnect and
// zeroes only on the Session Reset button, NOT on a new socket). session_ms
// arrives only on ~2Hz STATS frames — and stops entirely when this tab isn't
// the streamed client — so the readout used to freeze between (or without)
// frames. We anchor the last reported value to a performance.now() timestamp
// and a shared 1Hz interval (startUiClock) extrapolates it forward, re-syncing
// on every STATS frame, so it ticks every second. Format is zero-padded
// hh:mm:ss (fixed-width Martian Mono) so the seconds visibly advance.
var _sessionAnchor = null; // { ms:<session_ms>, at:<performance.now()> }

function _fmtClock(ms) {
  var s = Math.max(0, Math.floor((ms || 0) / 1000));
  var h = Math.floor(s / 3600); s -= h * 3600;
  var m = Math.floor(s / 60);   s -= m * 60;
  var p = function (n) { return (n < 10 ? '0' : '') + n; };
  return p(h) + ':' + p(m) + ':' + p(s);
}

function _tickSessionTime() {
  var t = $('#sessTime');
  if (!t || !_sessionAnchor) return;
  // performance.now() keeps advancing while the tab is hidden, so on return
  // this yields the correct jumped value with no extra bookkeeping.
  t.textContent = _fmtClock(_sessionAnchor.ms + (performance.now() - _sessionAnchor.at));
}

// One shared 1Hz UI clock for every time-derived readout. Pauses while the tab
// is hidden (cheap); resyncs immediately on return.
function startUiClock() {
  if (window.__uiClock) return;
  window.__uiClock = setInterval(function () {
    if (document.hidden) return;
    _tickSessionTime();
    // (future time-derived readouts — device uptime, "last seen / ago" — go here)
  }, 1000);
  document.addEventListener('visibilitychange', function () {
    if (!document.hidden) _tickSessionTime();
  });
}

function renderSessionCard(st) {
  if (!st) return;
  var d = $('#sessDist');    if (d  && typeof st.distance_mm === 'number')    d.textContent  = _fmtDist(st.distance_mm);
  var e = $('#sessEnergy');  if (e  && typeof st.energy_wh === 'number')      e.textContent  = _fmtEnergy(st.energy_wh);
  var x = $('#sessMaxSpd');  if (x  && typeof st.max_speed_mm_s === 'number') x.textContent  = Math.round(st.max_speed_mm_s) + ' mm/s';
  var k = $('#sessStrokes'); if (k  && typeof st.strokes === 'number')        k.textContent  = String(st.strokes);
  // Re-anchor the session clock on each STATS frame; the 1Hz clock ticks it.
  if (typeof st.session_ms === 'number') {
    _sessionAnchor = { ms: st.session_ms, at: performance.now() };
    _tickSessionTime(); // reflect the fresh value now, don't wait up to 1s
  }
}

// ---- Patch the header chips + Health from 0x02 WS status -------------------
function _applyWsStatusToUI(s) {
  if (!s) return;

  // WiFi link-dot — WS connected implies wifi is up
  var wd = $('#wifiDot'); if (wd) wd.className = 'link-dot good';
  if (s.ip && s.ip.length === 4) {
    setRead('wifiText', s.ip[0] + '.' + s.ip[1] + '.' + s.ip[2] + '.' + s.ip[3]);
  } else {
    setRead('wifiText', 'WiFi');
  }

  // Home dot — driven by 0x01 flags, not 0x02. Keep last known state.
  // Transport — from 0x02 transport field
  var tportName = ['WS','SER','BT','DONGLE','OSSM'][s.transport] || 'WS';
  setRead('ifText', tportName);
  var ifd = $('#ifDot'); if (ifd) ifd.className = 'dot good';

  // ---- Bus current chip: live draw from 0x01 telemetry, NOT peak-hold ------
  var busV = s.bus_mV / 1000; // mV -> V
  _lastBusV = busV;
  var amps = _wsLive ? (_liveBusmA / 1000) : (s.peak_mA / 1000);
  _agLastAmps = amps;
  var curChip = $('#currentChip');
  if (curChip) {
    curChip.style.display = '';
    var mag = Math.abs(amps);
    setRead('currentText', pad(mag, 2, 2, 'A'));
    var cd = $('#currentDot');
    if (cd) cd.className = 'dot ' + (mag >= 15 ? 'bad' : mag >= 8 ? 'warn' : 'good');
    var ct = $('#currentText');
    if (ct) { ct.classList.remove('w1','w2'); if (mag >= 15) ct.classList.add('w2'); else if (mag >= 8) ct.classList.add('w1'); }
  }

  // ---- Bus voltage chip ----------------------------------------------------
  var voltChip = $('#voltageChip');
  if (voltChip) {
    voltChip.style.display = '';
    setRead('voltageText', pad(busV, 2, 1, 'V'));
    // 36V bus: bad < 22V, warn < 24V, else nominal quiet gray. :3
    var vd = $('#voltageDot');
    if (vd) vd.className = 'dot ' + (busV < 22 ? 'bad' : busV < 24 ? 'warn' : 'good');
    var vt = $('#voltageText');
    if (vt) { vt.classList.remove('w1','w2'); if (busV < 22) vt.classList.add('w2'); else if (busV < 24) vt.classList.add('w1'); }
  }

  // RSSI chip
  if (typeof s.rssi === 'number') {
    var rssiEl = $('#rssiChip');
    if (!rssiEl) {
      rssiEl = document.createElement('span');
      rssiEl.id = 'rssiChip';
      rssiEl.className = 'chip';
      rssiEl.innerHTML = '<span class="dot good" id="rssiDot"></span><span id="rssiText">--</span>';
      var chips = document.querySelector('.chips');
      if (chips) chips.appendChild(rssiEl);
    }
    var rd = $('#rssiDot');
    if (rd) rd.className = 'dot ' + (s.rssi < -75 ? 'warn' : s.rssi < -85 ? 'bad' : 'good');
    setRead('rssiText', s.rssi + ' dBm');
  }

  // ---- Health tab — feed from 0x02 STATUS ---------------------------------
  var healthPayload = {
    bus_voltage_v: busV,
    bus_current_a: amps,
    bus_power_w: busV * amps,
    die_temp_c: s.die_c10 / 10,
    peak_current_a: s.peak_mA / 1000,
    rssi: s.rssi,
    wifi_channel: s.wifi_ch,
    wifi_reconnects: s.reconnects,
    wifi_connected: true,
    has_power_monitor: true,
    has_current_sensor: true,
    tx_drops: s.tx_drops,
    heap_free: s.heap_free,
    cfg_gen: s.cfg_gen
  };
  refreshHealthCards(healthPayload);

  // Footer diagnostics — surface heap + tx-drops (parsed but previously unused).
  var hc = $('#heapChip');
  if (hc && typeof s.heap_free === 'number')
    hc.textContent = (s.heap_free >= 1024) ? (s.heap_free / 1024).toFixed(0) + 'k' : String(s.heap_free);
  var dc = $('#dropsChip');
  if (dc && typeof s.tx_drops === 'number') dc.textContent = String(s.tx_drops);
}

// ---- Apply 0x01 flags to UI state -----------------------------------------
function _applyWsFlags(flags) {
  state.homed = !!(flags & FLAG_HOMED);
  state.paused = !!(flags & FLAG_PAUSED);
  state.override = !!(flags & FLAG_OVERRIDE);
  state.ifActive = !!(flags & FLAG_INTIFACE_ACTIVE);
  // Pattern running flag from telemetry (PB-004) — single source of truth
  const patRunning = !!(flags & FLAG_GEN_RUNNING);
  if (patRunning !== pat.running) {
    pat.running = patRunning;
    setPatternButton();
  }

  // ---- E-STOP from confirmed device state (FLAG_ESTOP, latched firmware-side)
  // This is the ONLY thing that raises/drops the fault banner — so an e-stop
  // from a hardware switch, watchdog, another client, or a serial/BT transport
  // shows here too, and the banner can never be dismissed while the device
  // still reports the e-stopped state. :3
  var estopped = !!(flags & FLAG_ESTOP);
  if (estopped !== state.estopped) {
    state.estopped = estopped;
    var fb = $('#faultBanner');
    if (fb) fb.classList.toggle('show', estopped);
    if (estopped) toast('Device E-STOPPED — motion blocked until re-home', 'bad', 'i-alert', 6000);
  }
  reflectGating();

  var hd = $('#homeDot');
  if (state.homed) { if (hd) hd.className = 'dot good'; setRead('homeText', 'Homed'); }
  else { if (hd) hd.className = 'dot warn'; setRead('homeText', 'Home'); }

  // Show/hide Home buttons
  var hf = $('#homeFab'); if (hf) hf.classList.toggle('show', !state.homed);
  var hh = $('#homeBtnHeader'); if (hh) hh.style.display = state.homed ? 'none' : '';
}

// ---- PollStatus (HTTP fallback path — kept intact) -------------------------

async function pollStatus() {
  // A live SlopSync session owns the shared surfaces — never let the HTTP
  // fallback poll fight it for the flags/window/stats. Checked as LIVE state
  // rather than a callback flag, so the WELCOME race cannot open a window where
  // both planes write the same DOM.
  if (isSlopSyncLive()) { _wsLive = true; return; }
  const nowMs = performance.now();
  const backoffElapsed = nowMs - (pollLastSuccessMs || nowMs);
  // Fast-probe: every 5th attempt fires regardless of backoff (A-008)
  const isFastProbe = pollAttemptCount > 0 && pollAttemptCount % 5 === 0;
  if (!isFastProbe && pollFailures > 0 && backoffElapsed < pollBackoffMs) return;
  if (pollInFlight) return;
  pollInFlight = true;
  try {
    const d = await get('/api/status?since=' + teleSince); if (!d) {
      pollFailures++;
      pollAttemptCount++;
      pollSuccessStreak = 0;
      pollBackoffMs = Math.min(pollBackoffMs * 2, POLL_MAX);
      if (pollFailures >= POLL_STALE_N) showStaleIndicator(true);
      return;
    }
    pollSuccessStreak++;
    pollAttemptCount++;
    if (pollSuccessStreak >= 3) {
      pollFailures = 0;
      pollBackoffMs = POLL_NOMINAL;
    } else {
      pollBackoffMs = Math.max(POLL_NOMINAL, Math.floor(pollBackoffMs / 2));
    }
    pollLastSuccessMs = performance.now();
    if (staleIndicatorShown) showStaleIndicator(false);

    state.position = d.position || 0; state.homed = !!d.homed;

    if (typeof d.measured_stroke_mm === 'number' && d.measured_stroke_mm !== lastMeasuredStroke) {
      lastMeasuredStroke = d.measured_stroke_mm;
      fetchAndApplyCapabilities().then(function (caps) { if (caps) { rebuildPatternGrid(); renumberPanels(); } });
    }
    state.ifActive = !!d.buttplug_connected && d.measured_hz > 0;
    state.paused = !!d.paused; state.override = !!d.manual_override; reflectGating();

    // E-stop banner from confirmed device state (fallback-path mirror of the
    // FLAG_ESTOP handling in _applyWsFlags). :3
    if (typeof d.estopped === 'boolean' && d.estopped !== state.estopped) {
      state.estopped = d.estopped;
      var fbn = $('#faultBanner'); if (fbn) fbn.classList.toggle('show', d.estopped);
      if (d.estopped) toast('Device E-STOPPED — motion blocked until re-home', 'bad', 'i-alert', 6000);
    }

    var wd = $('#wifiDot'); if (wd) wd.className = 'link-dot ' + (d.wifi_connected ? 'good' : 'bad');
    setRead('wifiText', d.wifi_connected ? (d.ip || 'WiFi') : 'Off');
    var hd = $('#homeDot');
    if (d.homed) { if (hd) hd.className = 'dot good'; setRead('homeText', 'Homed'); }
    else { if (hd) hd.className = 'dot warn'; setRead('homeText', d.homing ? 'Homing' : 'Home'); }
    var tport = d.transport || 'WS';
    var linked = tport === 'SER'    ? !!d.serial_linked
               : tport === 'BT'     ? !!d.ble_connected
               : tport === 'DONGLE' ? !!d.dongle_active
               :                      !!d.buttplug_connected;
    var ifd = $('#ifDot'); if (ifd) ifd.className = 'dot ' + (linked ? 'good' : 'bad');
    setRead('ifText', linked ? (d.measured_hz > 0 ? d.measured_hz + 'Hz' : 'idle') : tport);

    // Header chips
    var curChip = $('#currentChip');
    if (curChip) {
        if (d.has_current_sensor) {
            curChip.style.display = '';
            var amps = (typeof d.bus_current_a === 'number') ? d.bus_current_a : 0;
            var mag = Math.abs(amps);
            setRead('currentText', pad(mag, 2, 2, 'A'));
            var cd = $('#currentDot');
            if (cd) cd.className = 'dot ' + (mag >= 15 ? 'bad' : mag >= 8 ? 'warn' : 'good');
            var ct = $('#currentText');
            if (ct) { ct.classList.remove('w1','w2'); if (mag >= 15) ct.classList.add('w2'); else if (mag >= 8) ct.classList.add('w1'); }
        } else {
            curChip.style.display = 'none';
        }
    }
    var voltChip = $('#voltageChip');
    if (voltChip) {
        if (d.has_power_monitor) {
            voltChip.style.display = '';
            var volts = (typeof d.bus_voltage_v === 'number') ? d.bus_voltage_v : 0;
            setRead('voltageText', pad(volts, 2, 1, 'V'));
            var vd = $('#voltageDot');
            if (vd) vd.className = 'dot ' + (volts < 22 ? 'bad' : volts < 24 ? 'warn' : 'good');
            var vt = $('#voltageText');
            if (vt) { vt.classList.remove('w1','w2'); if (volts < 22) vt.classList.add('w2'); else if (volts < 24) vt.classList.add('w1'); }
        } else {
            voltChip.style.display = 'none';
        }
    }

    // ---- Motion-mode chip (D4 planner diagnostics) ----------------------------
    // Shows whether the planner is deriving gentle motion (D4 sparse profiling)
    // or running flat-out at the limit set (position-only retarget at ceiling).
    // Format: "mot DV:128 C:128" or "mot PEAK" or "mot LATE".
    var motChip = $('#motChip'); var motDot = $('#motDot'); var motText = $('#motText');
    if (motChip && motText) {
        motChip.style.display = '';
        var dspd = d.plan_derived_spd || 0, cspd = d.plan_clamped_spd || 0;
        var dacc = d.plan_derived_acc || 0, cacc = d.plan_clamped_acc || 0;
        if (dspd > 0 || cspd > 0) {
            if (d.plan_late) {
                motText.textContent = 'LATE';
                if (motDot) motDot.className = 'dot bad';
                motText.className = 'w2';
            } else if (cspd < dspd || cacc < dacc) {
                var ratio = dspd > 0 ? Math.round(cspd / dspd * 100) : 100;
                motText.textContent = 'C:' + ratio + '%';
                if (motDot) motDot.className = 'dot warn';
                motText.className = 'w1';
            } else {
                motText.textContent = 'DV';
                if (motDot) motDot.className = 'dot good';
                motText.className = '';
            }
        } else {
            motText.textContent = 'mot --';
            if (motDot) motDot.className = 'dot';
            motText.className = '';
        }
    }

    refreshHealthCards(d);
    renderSessionCard(d);   // /api/status carries the same session-stat fields

    // Keep the diag-graph power supplier fresh on the HTTP-fallback path too.
    if (typeof d.bus_voltage_v === 'number') _lastBusV = d.bus_voltage_v;
    if (typeof d.bus_current_a === 'number') _liveBusmA = d.bus_current_a * 1000;

    if (tport !== currentMode) reflectMode(tport);

    var hf = $('#homeFab'); if (hf) hf.classList.toggle('show', !d.homed);
    var hh = $('#homeBtnHeader'); if (hh) hh.style.display = d.homed ? 'none' : '';
    setVV('currentPos', d.position || 0, 3, 1);

    if (typeof d.tele_seq === 'number') teleSince = d.tele_seq;
    if (d.samples && d.samples.length) {
      var now = performance.now();
      feedHttpSamples(d.samples, d.tele_dt || 10, now);
    } else {
      setPosTarget(state.position);
    }

    // Transport status bar
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
      fetchAndApplyCapabilities().then(function (caps) { if (caps) { rebuildPatternGrid(); renumberPanels(); } });
    }
    prevStatus.buttplug = !!d.buttplug_connected; prevStatus.serial = !!d.serial_linked; prevStatus.ready = !!d.homed;
    // Firmware yields internally via PatternEngine emit gate — UI trusts it (PB-003/PB-012)
  } catch (e) {
    // swallow
  } finally {
    pollInFlight = false;
  }
}

// ===================== Log refresh ==========================================

// In-flight guard: the 2 s interval fires regardless of completion, so a slow
// server response would stack requests until the browser's per-host connection
// pool saturates and EVERYTHING (including page loads) queues behind the pile
// (observed live 2026-07-24: waves of 5 s-canceled log/clients/servo fetches).
// One request in flight, ever; a slow tick just skips.
let _logInflight = false;
async function refreshLog() {
  if (_logInflight) return;
  _logInflight = true;
  try {
    var txt = await getText('/api/log');
    var box = $('#logBox');
    if (!box) return;
    var atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 30;
    box.textContent = txt || '(no output)';
    // Terminal blinking cursor (§4.1) — textContent above wipes any prior
    // child, so it's re-appended at the tail on every refresh rather than
    // living as static markup.
    var cur = document.createElement('span');
    cur.className = 'term-cursor';
    cur.id = 'logCursor';
    cur.textContent = '▌';
    box.appendChild(cur);
    if (atBottom) box.scrollTop = box.scrollHeight;
  } catch (e) {} finally { _logInflight = false; }
}

// ===================== Connected-clients panel (Health tab) =================
// Lists WS clients from /api/clients. The device streams only to the
// most-recently-active tab (+ any active within active_window_ms); the rest
// are muted (harmless). "Kick" force-disconnects to reclaim a slot.

function _fmtIdle(ms) {
  if (ms == null) return '—';
  if (ms < 1000) return 'now';
  var s = Math.round(ms / 1000);
  if (s < 60) return s + 's ago';
  var m = Math.floor(s / 60);
  return m + 'm' + (s % 60) + 's ago';
}

let _clientsInflight = false;   // same in-flight guard as refreshLog
async function refreshClients() {
  var body = $('#clientsBody');
  if (!body) return;
  if (_clientsInflight) return;
  _clientsInflight = true;
  var data;
  try { data = await get('/api/clients'); } finally { _clientsInflight = false; }
  if (!data || !Array.isArray(data.clients)) { body.textContent = 'Client list unavailable.'; return; }
  if (data.clients.length === 0) { body.textContent = 'No clients connected.'; return; }

  var rows = data.clients.map(function (c) {
    var badge = c.most_recent
      ? '<span class="chip" style="color:var(--good)">● active (this session)</span>'
      : (c.streaming ? '<span class="chip" style="color:var(--good)">● streaming</span>'
                     : '<span class="chip" style="color:var(--tx-mut)">○ muted (idle)</span>');
    var mine = (c.streaming && c.most_recent) ? ' — likely this tab' : '';
    return '<div class="client-row">'
      +   '<code>#' + c.num + ' ' + c.ip + '</code>'
      +   '<span class="client-row-meta">' + badge + ' · last active ' + _fmtIdle(c.idle_ms) + mine + '</span>'
      +   '<button class="btn ghost sm" data-kick="' + c.num + '"><span data-ico="i-stop"></span> Kick</button>'
      + '</div>';
  }).join('');
  body.innerHTML = '<div class="hint" style="margin-bottom:6px">'
    + data.clients.length + ' / ' + data.max + ' slots · muted after '
    + Math.round(data.active_window_ms / 1000) + 's idle (unless it\'s the last-active tab)</div>' + rows;

  body.querySelectorAll('[data-kick]').forEach(function (btn) {
    btn.addEventListener('click', async function () {
      var num = parseInt(btn.getAttribute('data-kick'), 10);
      btn.disabled = true;
      await post('/api/clients', { kick: num });
      setTimeout(refreshClients, 300);
    });
  });
}

// ===================== Anomaly surface (0x05) ===============================
// Renders one interpolator anomaly event into the Log-tab panel: bumps the
// per-kind count chip (with a severity dot) and prepends a formatted line to
// the anomBox. Pure DOM writes — the ring + counters live in module state so
// the panel can be rebuilt on tab switch without losing history.

// Per-kind dot severity: overshoot/decel invent MOTION (bad — you feel these);
// dropped/fallback are TIMING guesses (warn — usually benign but diagnostic).
var ANOM_SEV = {
  Overshoot: 'bad', DecelOverrun: 'bad', PointDropped: 'warn', DurFallback: 'warn'
};

function _fmtAnomLine(a) {
  // us→ms for humans; positions as 0..1 with 3 decimals. `extra` is kind-
  // specific — label it so the meaning is legible on the wire, not just a number.
  var t = (a.rxMs != null ? a.rxMs : performance.now());
  var hhmmss = new Date().toLocaleTimeString('en-GB'); // 24h, stable width
  var extraLabel = a.kindName === 'Overshoot'    ? 'peak+' + (a.extra * 100).toFixed(1) + '%'
                 : a.kindName === 'PointDropped' ? 'gap ' + (a.extra / 1000).toFixed(1) + 'ms'
                 : a.kindName === 'DecelOverrun' ? 'vEnd ' + a.extra.toFixed(3)
                 : a.kindName === 'DurFallback'  ? 'dur ' + (a.durationUs / 1000).toFixed(1) + 'ms'
                 : String(a.extra);
  return hhmmss + '  ' + a.kindName.padEnd(13) +
         ' s=' + a.startPos.toFixed(3) + ' →t=' + a.targetPos.toFixed(3) +
         ' v0=' + a.startVel.toFixed(3) + ' G=' + a.endSlope.toFixed(3) +
         ' [' + extraLabel + ']';
}

function renderAnomaly(a) {
  // Count chip + severity dot
  var cnt = anomalyCounts[a.kindName];
  if (cnt != null) {
    var cntEl = $('#anomCnt' + a.kindName);
    if (cntEl) cntEl.textContent = a.kindName + ' ' + cnt;
    var dotEl = $('#anomDot' + a.kindName);
    if (dotEl) dotEl.className = 'dot ' + (ANOM_SEV[a.kindName] || 'warn');
  }
  // Prepend the line; keep the box bounded to the ring capacity so the DOM
  // never grows unbounded on a long pounding session.
  var box = $('#anomBox');
  if (box) {
    var lines = anomalyLog.map(_fmtAnomLine);
    box.textContent = lines.join('\n');
    box.scrollTop = 0; // newest is at the top
  }
}

// ===================== Desktop sidebar card cloning =========================
// On desktop (>=1024px) we move the Stroke Window + Position cards from the
// Drive tab-content into the permanent sidebar. These cards carry
// [data-sidebar-card] attributes. On mobile the cards stay right where they
// are — the sidebar is just hidden via CSS.

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
  wireActions();
}

function restoreSidebarCards() {
  var driveTab = $('#drive');
  if (!driveTab) return;
  var sidebarCards = $('#sidebarCards');
  if (!sidebarCards) return;
  var cards = sidebarCards.querySelectorAll('[data-sidebar-card]');
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

// ===================== Resizable sidebar ====================================

var SIDEBAR_WIDTH_KEY = 'sd32_sidebar_w';

function initResizableSidebar() {
  var sidebar = $('#sidebarHost');
  var handle = $('#resizeHandle');
  if (!sidebar || !handle) return;

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
    window.dispatchEvent(new Event('resize'));
  }

  handle.addEventListener('pointerdown', onDown);
  window.addEventListener('pointermove', onMove);
  window.addEventListener('pointerup', onUp);
  window.addEventListener('pointercancel', onUp);
}

// ===================== Rail topbar (Thing 6: manual mode + quick-move) ======
// Two controls live above the rail:
//  • #railQuickMove — a fat, finger-friendly tap strip for DEFAULT mode.
//    Tapping (or dragging) maps the x-position across the strip to 0..TRAVEL
//    and fires a quick move, so you can dab a position without hunting the thin
//    rail band. Mirrors the rail's tap-to-command with a bigger target.
//  • #manualModeToggle — flips the rail into manual scrub mode (rail.js owns
//    the actual behavior); here we just reflect aria-pressed + the .on state
//    and dim the now-redundant quick strip.
function initRailTopbar() {
  var strip = $('#railQuickMove');
  if (strip) {
    // Ghost pip that rides under the finger (CSS: .rail-quickmove-pip, shown
    // via the .armed class). Build it once and reuse — no per-event alloc.
    var pip = strip.querySelector('.rail-quickmove-pip');
    if (!pip) {
      pip = document.createElement('div');
      pip.className = 'rail-quickmove-pip';
      strip.appendChild(pip);
    }
    var stripDrag = false;
    var placePip = function(frac) { pip.style.left = (frac * 100) + '%'; };
    var fireFromEvent = function(e) {
      var r = strip.getBoundingClientRect();
      if (r.width <= 0 || TRAVEL <= 0) return;
      var frac = clamp((e.clientX - r.left) / r.width, 0, 1);
      placePip(frac);
      sendMove(frac * TRAVEL, true);
      if (pat.running) stopPattern();
    };
    var disarm = function() {
      stripDrag = false;
      strip.classList.remove('armed');
    };
    strip.addEventListener('pointerdown', function(e) {
      if (strip.classList.contains('disabled')) return;
      stripDrag = true;
      strip.classList.add('armed');
      if (strip.setPointerCapture) { try { strip.setPointerCapture(e.pointerId); } catch (err) {} }
      fireFromEvent(e);
      e.preventDefault();
    });
    strip.addEventListener('pointermove', function(e) {
      if (!stripDrag) return;
      fireFromEvent(e);
      e.preventDefault();
    });
    strip.addEventListener('pointerup', function(e) { disarm(); e.preventDefault(); });
    strip.addEventListener('pointercancel', disarm);
  }

  var toggle = $('#manualModeToggle');
  if (toggle) {
    // Manual mode now OWNS bypass-limits: engaging Manual checks the hidden
    // #bypassLimits input (so scrubs + sendMove() span the full measured
    // travel), and exiting restores whatever state it had before. The
    // checkbox's existing 'change' listeners (range.js syncManualWindow +
    // the wiring below) do the actual work — we just drive the input.
    var _bypassBeforeManual = false;
    toggle.addEventListener('click', function() {
      var on = setRailManualMode(toggle.getAttribute('aria-pressed') !== 'true');
      toggle.setAttribute('aria-pressed', on ? 'true' : 'false');
      toggle.classList.toggle('on', on);
      if (strip) strip.classList.toggle('disabled', on);
      var bp = $('#bypassLimits');
      if (bp) {
        if (on) {
          _bypassBeforeManual = bp.checked;
          if (!bp.checked) {
            bp.checked = true;
            bp.dispatchEvent(new Event('change', { bubbles: true }));
          }
        } else if (bp.checked !== _bypassBeforeManual) {
          bp.checked = _bypassBeforeManual;
          bp.dispatchEvent(new Event('change', { bubbles: true }));
        }
      }
      // Update the hint text to reflect the active mode so the operator
      // always knows what the rail will do (Thing 5). :3
      var hint = $('#railHintText');
      if (hint) hint.textContent = on
        ? 'drag anywhere to scrub \u00b7 full travel \u00b7 set min/max by feel'
        : 'drag band \u00b7 drag edges \u00b7 tap to command';
    });
  }
}

// ===================== WS telemetry → rail bridge ===========================
// When WS is live, the rail reads from telebuf.sampleAt(). The rail's rAF
// loop uses stableRenderTime() to get the clock-synced render time and
// sampleAt() to get the interpolated position. We also update render delay.
// In fallback mode, telebuf is fed from HTTP polls via feedHttpSamples().

var _railRafId = null;
var _lastRailFrameTs = 0;

function _startRailTelemetryLoop() {
  if (_railRafId) return;
  function _frame(nowMs) {
    _railRafId = requestAnimationFrame(_frame);
    var dtMs = _lastRailFrameTs ? (nowMs - _lastRailFrameTs) : 16.667;
    _lastRailFrameTs = nowMs;

    updateRenderDelay(dtMs);
    // clock-synced "now" = performance.now() + offset (handled inside telebuf)
    var renderT = stableRenderTime(nowMs);
    var s = sampleAt(renderT);

    // Update dom readouts + feed the rail canvas (phosphor markers + heroes)
    if (s) {
      setVV('currentPos', s.pos, 3, 1);
      setPosTarget(s.pos);
      railUpdatePosition(s.pos, s.tgt);
      _agNoteVel(s.pos);
    }

    // Task 8: shadow overdue escalation tick
    shadowTick(nowMs);
  }
  _railRafId = requestAnimationFrame(_frame);
}

// ===================== Init =================================================

// ---- Bridge hooks (M5c: formerly the :81 0x04/0x05 frame callbacks) --------

/** 0x0086 plan-strip -> the debug overlay / rail planned-path renderer. */
function _applyInterp(it) {
  interpState.active     = it.active;
  interpState.liveMode   = it.liveMode;
  interpState.gradMode   = it.gradMode;
  interpState.style      = it.style;
  interpState.styleName  = it.styleName;
  interpState.startPos   = it.startPos;
  interpState.endPos     = it.endPos;
  interpState.curPos     = it.curPos;
  interpState.curVel     = it.curVel;
  interpState.durationUs = it.durationUs;
  interpState.elapsedUs  = it.elapsedUs;
  interpState.lastRxMs   = performance.now();
}

/**
 * 0x0089 motion-anomaly EVENT -> bounded log + kind counter + the Log tab.
 * One frame per invented micromotion / dropped point / decel overrun /
 * waveform fallback: the diagnostic that says WHICH stutter cause actually
 * fired on the wire.
 */
function _applyAnomaly(a) {
  if (Object.prototype.hasOwnProperty.call(anomalyCounts, a.kindName))
    anomalyCounts[a.kindName]++;
  a.rxMs = performance.now();
  anomalyLog.unshift(a);
  if (anomalyLog.length > ANOMALY_LOG_CAP) anomalyLog.pop();
  renderAnomaly(a);
}

/** SlopSync session lost -> fall back to HTTP polling. */
function _onLinkDegraded() {
  console.warn('[SD32] slopsync down - starting HTTP fallback poll');
  _wsLive = false;
  window.__CMD_SUSPENDED = false;
  setTeleFallback(true); // staleness thresholds x5 in HTTP polling mode
  if (!_fallbackPollInterval) {
    _fallbackPollInterval = setInterval(pollStatus, 100);
    pollStatus();
  }
}

/** SlopSync session LIVE -> disarm polling and adopt device truth. */
function _onLinkRestored() {
  console.log('[SD32] slopsync live - clearing HTTP poll');
  // FIRST: disarm HTTP polling before touching any other state (A-004)
  if (_fallbackPollInterval) {
    clearInterval(_fallbackPollInterval);
    _fallbackPollInterval = null;
  }
  setTeleFallback(false);
  // Stamp liveness immediately so the suspend gate has a full grace window
  // before the first periodic STATE lands, even on an idle rig.
  noteLinkAlive();
  _wsLive = true;
  window.__CMD_SUSPENDED = false;
  document.body.classList.remove('stale', 'suspended', 'degraded');
  notifyRecovery();
  // The UI must NEVER assume config on (re)connect. SlopSync's retained-on-grant
  // STATE already delivers window/speeds/accel the moment the channels are
  // granted, so the old OP_GET_CFG pull is redundant here -- but capabilities
  // still needs a refetch so measured rail + ceilings resync after a re-home
  // during a dropout.
  fetchAndApplyCapabilities().then(function (caps) { if (caps) { rebuildPatternGrid(); renumberPanels(); } });
}

function init() {
  console.log('[SD32] init begin');
  try {
    injectIcons();
    measureMartianMonoCh();
    console.log('[SD32] icons + fonts OK');

    // ---- Telemetry buffer + the ONE remaining link --------------------------
    // M5c: initLink() is GONE. The legacy :81 UiSocket plane ran alongside this
    // and is now deleted outright -- it was measurably the cause of the device
    // reboots and heap exhaustion (docs/http-plane-retirement.md 2), it carried
    // no authorization of any kind, and every surface it fed is now fed from
    // SlopSync below.
    initTeleBuf();
    // ---- SlopSync (ws://<hostname>:82) -- the only telemetry plane ----------
    // motion->telebuf, flags->_applyWsFlags, machine-config->processConfig,
    // odometer->renderSessionCard, plan-strip->interpState, anomaly->the log;
    // installs the write-plane senders on window.__slopsync.
    initSlopSyncBridge({
      host: location.hostname,
      applyFlags: _applyWsFlags,
      applyConfig: processConfig,
      renderSession: renderSessionCard,
      applyInterp: _applyInterp,
      applyAnomaly: _applyAnomaly,
      noteAlive: function () { noteLinkAlive(); _agNoteLinkMsg(); },
      onLive: _onLinkRestored,
      onDown: _onLinkDegraded,
      log: function (lvl) { if (window.__DEBUG_SLOPSYNC) console.log.apply(console, ['[slopsync]'].concat([].slice.call(arguments, 1))); },
    });
    console.log('[SD32] telebuf + slopsync OK');

    // ---- Link lifecycle, now driven by the SlopSync session -----------------
    // These were registered against link.js's :81 callbacks. They are the same
    // logic, hoisted to named functions and called from the bridge hooks: the
    // conditions did not change, only which plane reports them.
    //
    // HTTP FALLBACK STAYS. When SlopSync is down the page still polls
    // /api/status and cmd.js still routes ops to their /api/* twins, so a
    // machine whose hub plane is broken remains observable and controllable.
    // Diagnostics that depend on the subsystem under test are not diagnostics
    // (docs/http-plane-retirement.md 1) -- and that argument applies just as
    // well to the last-resort control path.

    // ---- Staleness escalation -----------------------------------------------
    onStale(function() {
      document.body.classList.add('stale');
    });
    onStaleSuspended(function() {
      document.body.classList.add('suspended');
      window.__CMD_SUSPENDED = true;
    });

    // Fresh data resumed after stale/suspended — clear blocking state
    onFresh(function() {
      window.__CMD_SUSPENDED = false;
      document.body.classList.remove('stale', 'suspended');
      notifyRecovery();
    });

    // ---- Task 8: shadow state reconciliation -------------------------------
    cmd.onEcho(function(ev) {
      processEcho(ev);
      if (window.__DEBUG_ECHO) console.log('[echo]', ev);
    });
    cmd.onConfig(function(cfg) {
      // Precedence: slopsync machine-config 0x0081 owns the window/limit adoption
      // when live; the legacy config push (OP_GET_CFG echo) stands down.
      if (!isSlopSyncLive()) processConfig(cfg);
      if (window.__DEBUG_CFG) console.log('[cfg]', cfg);
    });
    // Command exhausted its WS retries — the device never acked. Without this
    // subscription every transport-level command failure was completely silent
    // (no toast, no log) while the control's pending state quietly aged out. :3
    var _lastFaultToastMs = 0;
    cmd.onFault(function(ev) {
      console.warn('[SD32] command FAILED after retries — op=0x' +
        (ev.op || 0).toString(16), ev.desired);
      var now = performance.now();
      if (now - _lastFaultToastMs > 2000) {
        _lastFaultToastMs = now;
        toast('Command not confirmed by device — change may not have applied', 'bad', 'i-alert', 4000);
      }
    });

    wireStaleness();

    // Desktop sidebar clone — guarded by matchMedia so it only fires on
    // desktop viewports. The listener handles resize crossings.
    var mq = window.matchMedia(SIDEBAR_BP);
    handleSidebarBreakpoint(mq);
    mq.addEventListener('change', handleSidebarBreakpoint);

    console.log('[SD32] tabs+collapse+tooltips+wire...');
    initTabs(); initCollapsibleCards(); initTooltips(); wireActions();
    initHoverTips();   // shared #tipbox hover-hint renderer (L4a)
    initHivis();       // high-legibility toggle: header eye + settings row (L2c/d)
    startUiClock();    // 1Hz clock for time-derived readouts, e.g. session time (L3)
    renumberPanels();
    initThemeUI();
    initActivityGrid();
    // Footer "ui" chip — build-time constant (git short hash, vite.config.js
    // __UI_BUILD__ define), never the device's own fw_version.
    setRead('uiChip', typeof __UI_BUILD__ !== 'undefined' ? __UI_BUILD__ : '--');
    console.log('[SD32] window+pattern+settings...');
    initWindowInputs(); renderWindow(); initPattern(); initSettings();
    console.log('[SD32] rail...');
    initRail({
      onTap: function(mm) { sendMove(mm, true); },
      onPatternStop: function() { if (pat.running) stopPattern(); }
    });
    console.log('[SD32] rail topbar...');
    initRailTopbar();
    initPlanStrip();
    // Diagnostics graph — supplier hands it the freshest bus V/A the UI has
    // (0x01 telemetry mA at ~45Hz + 0x02/HTTP voltage), reused object to
    // keep the per-frame path allocation-free.
    var _diagPow = { v: 0, a: 0 };
    initDiag(function () { _diagPow.v = _lastBusV; _diagPow.a = _liveBusmA / 1000; return _diagPow; });
    initConn();   // header telemetry-gap / connection-health dot

    console.log('[SD32] sidebar...');
    initResizableSidebar();
    console.log('[SD32] capabilities...');
    fetchAndApplyCapabilities().then(function (caps) {
      if (caps) { rebuildPatternGrid(); renumberPanels(); }
      // Servo drive (RS485 Modbus): telemetry card + Configure pane. Only
      // starts polling when the firmware actually carries the feature.
      if (caps && caps.features && caps.features.has_rs485) initServo();
    });

    // Start the rail telemetry loop (runs continuously, reads from telebuf)
    _startRailTelemetryLoop();
    console.log('[SD32] rail loop started');

    // Slow link-meta poll — the ONE link fact the binary 0x02 status frame
    // can't carry is the bssid string, so in WS mode the footer's bssid chip
    // had no source at all. One tiny HTTP status fetch at boot + every 30s
    // fills it (and only it — everything else stays on the WS path).
    function pollLinkMeta() {
      get('/api/status').then(function (d) {
        if (d) setLinkBssid(d.wifi_bssid);
      }).catch(function () {});
    }
    pollLinkMeta();
    setInterval(pollLinkMeta, 30000);

    // Start HTTP polling UNLESS WS is already live. The degraded/restored
    // callbacks handle the start/stop transitions.
    // Poll from boot until SlopSync reaches LIVE. It never has at this point
    // in init (the session has not even opened its socket), so this always
    // starts — _onLinkRestored disarms it. That is deliberate: a page that
    // renders nothing until a WebSocket handshake completes looks broken.
    console.log('[SD32] poll gate — slopsyncLive=', isSlopSyncLive());
    if (!isSlopSyncLive()) {
      console.log('[SD32] starting HTTP poll (slopsync not live yet)');
      setTeleFallback(true); // relaxed staleness thresholds until WS is live
      _fallbackPollInterval = setInterval(pollStatus, 100);
      pollStatus();
    } else {
      console.log('[SD32] slopsync already live, skipping poll');
      _wsLive = true;
    }

    if (import.meta.env.VITE_BLE_ENABLED === 'true') { var bt = $('#btModeBtn'); if (bt) bt.style.display = ''; }
    var bP = $('#btnPause'); if (bP) bP.addEventListener('click', togglePause);
    var hb = document.querySelector('.tbtn.halt'); if (hb) hb.addEventListener('click', halt);
    var eb = document.querySelector('.tbtn.estop'); if (eb) eb.addEventListener('click', estop);
    var ot = $('#overrideTog'); if (ot) ot.addEventListener('change', toggleOverride);
    var bl = $('#bypassLimits'); if (bl) bl.addEventListener('change', function () { syncManualWindow(); });
    var hf = $('#homeFab'); if (hf) hf.addEventListener('click', moveToHome);
    var hh = $('#homeBtnHeader'); if (hh) hh.addEventListener('click', moveToHome);
    var fb = document.querySelector('#faultBanner button'); if (fb) fb.addEventListener('click', clearFault);
    var rl = $('#refreshLogBtn'); if (rl) rl.addEventListener('click', refreshLog);
    var srb = $('#sessionResetBtn');
    if (srb) srb.addEventListener('click', async function () {
      try { await post('/api/settings', { reset_stats: true, no_persist: true }); } catch (e) {}
      renderSessionCard({ distance_mm: 0, energy_wh: 0, max_speed_mm_s: 0, strokes: 0, session_ms: 0 });
      toast('Session stats reset', 'good', 'i-reset');
    });
    // #log was the old tab-content wrapper; the Log pane is now #pv-log
    // (.paneView.active on desktop-pane-open, or just visible via the mobile
    // .m-active branch — the pane controller in ui.js keeps #pv-log.active
    // in sync in both cases, so this one check covers both layouts).
    setInterval(function () { if (document.hidden) return; var lt = $('#pv-log'), al = $('#autoLog'); if (lt && lt.classList.contains('active') && al && al.checked) refreshLog(); }, 2000);
    refreshLog();

    // Poll the connected-clients panel while it's actually on-screen (offsetParent
    // is null when its tab is hidden) — cheap, and it stops when you tab away.
    setInterval(function () {
      if (document.hidden) return;
      var card = $('#clientsCard');
      if (card && card.offsetParent !== null) refreshClients();
    }, 3000);

    // Expose Health metrics for the Health tab (link stats)
    window.__LINK_STATS = function () { return (window.__slopsync && window.__slopsync.session) ? window.__slopsync.session.state : null; };
    window.__BUF_STATS = getBufferStats;

    console.log('[SD32] init complete — booted');
  } catch (e) {
    console.error('[SD32] init CRASHED:', e);
    // Emergency: start HTTP polling so UI doesn't grey out
    console.warn('[SD32] emergency fallback poll after crash');
    if (!_fallbackPollInterval) {
      _fallbackPollInterval = setInterval(pollStatus, 100);
      pollStatus();
    }
  }
}
init();