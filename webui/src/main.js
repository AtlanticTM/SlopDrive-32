/**
 * SlopDrive-32 WebUI — main entry point.
 * CSS must be imported here so Vite/vite-plugin-singlefile inlines it.
 *
 * Desktop layout: on >=1024px screens the Stroke Window + Position cards get
 * cloned into a permanent left sidebar. The right panel hosts the tab system
 * (Drive / Health / Settings / Log). On mobile (<1024px) everything stays in
 * the single-column scroll — the sidebar is hidden and the bottom tab bar runs
 * the show.
 *
 * Resizable sidebar: a drag handle between sidebar and main panel lets the
 * operator decide how wide they want their stroke window to be. Width is
 * persisted to localStorage so it survives reloads.
 *
 * Task 7: binary WS transport via link.js, clock-synced telebuf.js, control
 * plane via cmd.js. HTTP polling is the fallback (gated behind isFallback()).
 * Header chips + Health stats are fed from 0x02 STATUS frames when WS is live.
 */
console.log('[SD32] booting — imports loaded');
import './style.css';
import { injectIcons, initTabs, initCollapsibleCards, initTooltips, wireActions, toast, icon, $, setRead, clamp, onLiveSlider, paintSlider, pad, setVV, setVVState, measureMartianMonoCh } from './core/ui.js';
import { post, get, getText } from './core/api.js';
import { TRAVEL, renderWindow, setPosTarget, syncManualWindow, nudgeWindow, trim, setBound, useCurrentAsDefault, initWindowInputs, winMin, winMax, setWinMin, setWinMax, setWindowReady } from './core/range.js';
import { initPattern, startPattern, stopPattern, pat, refreshPatternState } from './features/pattern.js';
import { currentMode, reflectMode, initSettings, saveSettings, restoreDefaults } from './features/settings.js';
import { initRail, railUpdatePosition, setRailManualMode } from './features/rail.js';
import { fetchAndApplyCapabilities, refreshHealthCards } from './core/capabilities.js';

// ---- Task 7: new core modules ---------------------------------------------
import { initLink, onTelemetry as _onWsTelemetry, onStatus as _onWsStatus,
         onInterp as _onWsInterp, onAnomaly as _onWsAnomaly,
         onDegraded, onRestored, isFallback, getStats as getLinkStats,
         isConnected as _wsConnected } from './core/link.js';
import { initTeleBuf, feedHttpSamples, sampleAt, stableRenderTime,
         updateRenderDelay, onStale, onStaleSuspended, onFresh, isSuspended,
         setTeleFallback, noteLinkAlive, getBufferStats } from './core/telebuf.js';
import * as cmd from './core/cmd.js';
import {
  OP_SET_WINDOW, OP_SET_SPEED, OP_SET_ACCEL, OP_GEN_CFG, OP_GEN_RUN,
  OP_MODE, OP_BLEND, OP_PAUSE, OP_HALT, OP_ESTOP, OP_HOME,
  OP_OVERRIDE, OP_BYPASS, OP_CLEAR_FAULT, OP_SAVE, OP_MOVE,
  OP_STREAM_MODE, OP_OVERSHOOT, OP_GET_CFG, OP_HOME_OVERRIDE,
  SPEED_CEILING_PEGGED, SPEED_VELOCITY_MATCHED
} from './core/wire.js';
import { initShadow, processEcho, processConfig, tick as shadowTick,
         wireStaleness, notifyRecovery } from './core/shadow.js';

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

export const state = { paused: false, override: false, ifActive: false, position: 0, homed: false };

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

function togglePause() {
  var next = !state.paused;
  cmd.send(OP_PAUSE, { paused: next });
  state.paused = next; reflectGating(); if (next) stopPattern();
}
function halt() {
  cmd.send(OP_HALT, {}); stopPattern();
  toast('Motion halted', 'warn', 'i-stop');
}
function estop() {
  cmd.send(OP_ESTOP, {});
  stopPattern();
  state.paused = false; state.override = false; reflectGating();
  toast('E-STOP — power cut, re-home required', 'bad', 'i-alert', 6000);
}
function toggleOverride() {
  var ov = $('#overrideTog'); var on = ov ? ov.checked : false;
  cmd.send(OP_OVERRIDE, { on: on });
  state.override = on; reflectGating();
}
function moveToHome() {
  cmd.send(OP_HOME, {});
  toast('Homing…', 'info', 'i-home');
}
function clearFault() {
  cmd.send(OP_CLEAR_FAULT, {});
  var fb = $('#faultBanner'); if (fb) fb.classList.remove('show');
  toast('Fault cleared', 'info', 'i-check');
}

var lastMoveSent = 0;
function sendMove(pos, force) {
  var now = performance.now();
  if (!force && now - lastMoveSent < 50) return; lastMoveSent = now;
  var bp = $('#bypassLimits');
  cmd.send(OP_MOVE, {
    position: Math.round(pos * 10) / 10,
    stream: !force,
    bypass_limits: bp ? bp.checked : false
  });
}

// ===================== HTTP poll (fallback mode only) ========================

var prevStatus = { buttplug: false, serial: false, ready: false };
var lastMeasuredStroke = -1;

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
  var amps = _wsLive ? (_liveBusmA / 1000) : (s.peak_mA / 1000);
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
}

// ---- Apply 0x01 flags to UI state -----------------------------------------
function _applyWsFlags(flags) {
  state.homed = !!(flags & 0x01);
  state.paused = !!(flags & 0x10);
  state.override = !!(flags & 0x20);
  state.ifActive = !!(flags & 0x40);
  // Pattern running flag from telemetry (PB-004) — single source of truth
  const patRunning = !!(flags & 0x02);
  if (patRunning !== pat.running) {
    pat.running = patRunning;
    const btn = $('#patStartBtn');
    if (btn) {
      btn.innerHTML = pat.running ? icon('i-stop') + ' Stop Pattern' : icon('i-play') + ' Start Pattern';
      btn.classList.toggle('primary', !pat.running);
      btn.classList.toggle('danger', pat.running);
    }
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
  // WS peer connected? Let telemetry frames own the UI. Check live state
  // (not a callback flag) to avoid the race where HELLO hasn't fired yet.
  if (_wsConnected()) { _wsLive = true; return; }
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
      fetchAndApplyCapabilities();
    }
    state.ifActive = !!d.buttplug_connected && d.measured_hz > 0;
    state.paused = !!d.paused; state.override = !!d.manual_override; reflectGating();

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
      fetchAndApplyCapabilities();
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

async function refreshLog() {
  try { var txt = await getText('/api/log'); var box = $('#logBox'); if (!box) return; var atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 30; box.textContent = txt || '(no output)'; if (atBottom) box.scrollTop = box.scrollHeight; } catch (e) {}
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

async function refreshClients() {
  var body = $('#clientsBody');
  if (!body) return;
  var data = await get('/api/clients');
  if (!data || !Array.isArray(data.clients)) { body.textContent = 'Client list unavailable.'; return; }
  if (data.clients.length === 0) { body.textContent = 'No clients connected.'; return; }

  var rows = data.clients.map(function (c) {
    var badge = c.most_recent
      ? '<span class="chip" style="color:var(--good)">● active (this session)</span>'
      : (c.streaming ? '<span class="chip" style="color:var(--good)">● streaming</span>'
                     : '<span class="chip" style="color:var(--tx-mut)">○ muted (idle)</span>');
    var mine = (c.streaming && c.most_recent) ? ' — likely this tab' : '';
    return '<div class="client-row" style="display:flex;align-items:center;gap:10px;padding:6px 0;border-top:1px solid var(--line)">'
      +   '<code style="flex:0 0 auto">#' + c.num + ' ' + c.ip + '</code>'
      +   '<span style="flex:1 1 auto;font-size:.85em;color:var(--tx-mut)">' + badge + ' · last active ' + _fmtIdle(c.idle_ms) + mine + '</span>'
      +   '<button class="btn ghost sm" data-kick="' + c.num + '"><span data-ico="i-stop"></span> Kick</button>'
      + '</div>';
  }).join('');
  body.innerHTML = '<div style="font-size:.8em;color:var(--tx-mut);margin-bottom:4px">'
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
    }

    // Task 8: shadow overdue escalation tick
    shadowTick(nowMs);
  }
  _railRafId = requestAnimationFrame(_frame);
}

// ===================== Init =================================================

function init() {
  console.log('[SD32] init begin');
  try {
    injectIcons();
    measureMartianMonoCh();
    console.log('[SD32] icons + fonts OK');

    // ---- Task 7: telemetry buffer + WS transport ----------------------------
    initTeleBuf();
    initLink(); // connects ws://<hostname>:81/ws/ui
    console.log('[SD32] telebuf + link OK');

    // ---- Wire WS frame callbacks ---------------------------------------------
    _onWsTelemetry(function(t) {
      _applyWsFlags(t.flags);
      if (typeof t.i_bus_mA === 'number') _liveBusmA = t.i_bus_mA;
      // telebuf is auto-fed from link.js's feedWireSamples
      // Poll disarm is now owned by onRestored only (A-004).
    });

    _onWsStatus(function(s) {
      // 0x02 STATUS is a ~500ms link heartbeat that flows even when the rig is
      // idle and emitting no 0x01 motion telemetry. Feed it to telebuf so the
      // >1s control-suspension gate never trips on a healthy-but-idle link.
      noteLinkAlive();
      _applyWsStatusToUI(s);
    });

    // ---- 0x04 INTERP — interpolator debug snapshot (~45Hz) ------------------
    // Mirror the frame into interpState for the debug overlay / rail planned-
    // path renderer. Pure data capture; no DOM writes here to keep it cheap.
    _onWsInterp(function(it) {
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
    });

    // ---- 0x05 ANOMALY — interpolator path-anomaly events (event-driven) -----
    // One frame per invented micromotion / dropped point / decel overrun /
    // duration fallback. Push into the bounded log + bump the kind counter, then
    // hand to renderAnomaly() for the Log-tab surface. This is the diagnostic
    // that tells us WHICH of the stutter causes actually fired on the wire. :3
    _onWsAnomaly(function(a) {
      if (Object.prototype.hasOwnProperty.call(anomalyCounts, a.kindName))
        anomalyCounts[a.kindName]++;
      a.rxMs = performance.now();
      anomalyLog.unshift(a);
      if (anomalyLog.length > ANOMALY_LOG_CAP) anomalyLog.pop();
      renderAnomaly(a);
    });

    // ---- Fallback / restore -------------------------------------------------
    onDegraded(function() {
      console.warn('[SD32] WS degraded — starting HTTP fallback poll');
      _wsLive = false;
      window.__CMD_SUSPENDED = false;
      setTeleFallback(true); // staleness thresholds ×5 in HTTP polling mode
      // Start HTTP polling if not already running
      if (!_fallbackPollInterval) {
        _fallbackPollInterval = setInterval(pollStatus, 100);
        pollStatus();
      }
    });

    onRestored(function() {
      console.log('[SD32] WS restored — clearing HTTP poll');
      // FIRST: disarm HTTP polling before touching any other state (A-004)
      if (_fallbackPollInterval) {
        clearInterval(_fallbackPollInterval);
        _fallbackPollInterval = null;
      }
      setTeleFallback(false);
      // Stamp link-liveness immediately on HELLO so the suspend gate has a full
      // grace window before the first 0x02 STATUS heartbeat (~500ms out) lands,
      // even when the rig is idle and no 0x01 motion frames will ever arrive.
      noteLinkAlive();
      _wsLive = true;
      window.__CMD_SUSPENDED = false;
      document.body.classList.remove('stale', 'suspended', 'degraded');
      notifyRecovery();
      // Thing #3 — the UI must NEVER assume config on (re)connect. Pull the
      // machine's authoritative cfg snapshot the instant the link is live so
      // window / speeds / accel / mode all populate from device truth instead
      // of whatever stale defaults the DOM booted with. processConfig() (via
      // cmd.onConfig) applies it. Also re-fetch capabilities so the measured
      // rail length + ceilings resync after a re-home during a dropout. :3
      cmd.send(OP_GET_CFG, {});
      fetchAndApplyCapabilities();
    });

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
      processConfig(cfg);
      if (window.__DEBUG_CFG) console.log('[cfg]', cfg);
    });

    wireStaleness();

    // Desktop sidebar clone — guarded by matchMedia so it only fires on
    // desktop viewports. The listener handles resize crossings.
    var mq = window.matchMedia(SIDEBAR_BP);
    handleSidebarBreakpoint(mq);
    mq.addEventListener('change', handleSidebarBreakpoint);

    console.log('[SD32] tabs+collapse+tooltips+wire...');
    initTabs(); initCollapsibleCards(); initTooltips(); wireActions();
    console.log('[SD32] window+pattern+settings...');
    initWindowInputs(); renderWindow(); initPattern(); initSettings();
    console.log('[SD32] rail...');
    initRail({
      onTap: function(mm) { sendMove(mm, true); },
      onPatternStop: function() { if (pat.running) stopPattern(); }
    });
    console.log('[SD32] rail topbar...');
    initRailTopbar();
    console.log('[SD32] sidebar...');
    initResizableSidebar();
    console.log('[SD32] capabilities...');
    fetchAndApplyCapabilities();

    // Start the rail telemetry loop (runs continuously, reads from telebuf)
    _startRailTelemetryLoop();
    console.log('[SD32] rail loop started');

    // Start HTTP polling UNLESS WS is already live. The degraded/restored
    // callbacks handle the start/stop transitions.
    console.log('[SD32] poll gate — wsConnected=', _wsConnected());
    if (!_wsConnected()) {
      console.log('[SD32] starting HTTP poll (WS not connected)');
      setTeleFallback(true); // relaxed staleness thresholds until WS is live
      _fallbackPollInterval = setInterval(pollStatus, 100);
      pollStatus();
    } else {
      console.log('[SD32] WS already live, skipping poll');
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
    setInterval(function () { if (document.hidden) return; var lt = $('#log'), al = $('#autoLog'); if (lt && lt.classList.contains('active') && al && al.checked) refreshLog(); }, 2000);
    refreshLog();

    // Poll the connected-clients panel while it's actually on-screen (offsetParent
    // is null when its tab is hidden) — cheap, and it stops when you tab away.
    setInterval(function () {
      if (document.hidden) return;
      var card = $('#clientsCard');
      if (card && card.offsetParent !== null) refreshClients();
    }, 3000);

    // Expose Health metrics for the Health tab (link stats)
    window.__LINK_STATS = getLinkStats;
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