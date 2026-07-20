/**
 * applyCapabilities(caps) — central travel/ceiling/feature-flag binder. :3
 *
 * Fetched once from /api/capabilities at boot, this function patches every
 * DOM element whose max/label/hint is tied to machine geometry or speed/accel
 * ceilings in a single pass. It also shows/hides feature-flagged UI cards
 * (BLE button, dongle button, current/voltage chips, RS485 card, blend card).
 *
 * The stored ceilings object feeds applyExpertCaps() so it re-derives slider
 * max attrs from the API instead of hardcoding 10000/5000 literals.
 *
 * A dev assertion logs any element that still carries a stale literal after
 * the patch, so regressions are caught immediately — no more "only half the
 * sliders updated" bugs. :3
 *
 * Per plan.md §5.13 / §5.10.1.
 */
import { $, setRead, clamp } from './ui.js';
import { TRAVEL, setTravel } from '../core/range.js';
import { Meter } from './meter.js';
import { ac } from './theme.js';

/** Ceilings and feature flags cached after the API responds. */
export let capsCache = null;
export let capsReady = false;

/** Whether we're in expert mode — kept here so applyExpertCeilings() can
 *  re-derive slider max attrs from capsCache without importing settings.js. */
export let expertMode = false;
export function setExpertMode(v) { expertMode = v; }

/**
 * Fetch /api/capabilities and apply everything in one shot.
 * Returns the caps object (or null on failure).
 */
export async function fetchAndApplyCapabilities() {
  try {
    const r = await fetch('/api/capabilities');
    const caps = await r.json();
    if (!caps || !caps.max_travel_mm) return null;
    applyCapabilities(caps);
    return caps;
  } catch (e) {
    return null;
  }
}

/**
 * Patch every DOM element tied to travel, ceilings, or feature flags.
 * Called once at boot after the API responds; can be re-called if the
 * firmware advertises a different measured_stroke after homing.
 */
export function applyCapabilities(caps) {
  capsCache = caps;
  capsReady = true;

  // ---- 1. Travel (machine geometry) ----------------------------------------
  // measured_stroke > 0 means sensorless homing has felt out the real rail.
  // Otherwise fall back to the geometry ceiling (max_travel_mm).
  const travel = (caps.measured_stroke_mm && caps.measured_stroke_mm > 0)
    ? caps.measured_stroke_mm
    : caps.max_travel_mm;
  if (travel) setTravel(travel);

  // ---- 2. Speed / accel ceilings (normal vs expert) ------------------------
  // Store for applyExpertCeilings() to re-derive from.
  // speed_ceiling_mm_s / accel_ceiling_mm_s2 each have .normal and .expert.

  // ---- 3. Feature flags — show/hide transport buttons + UI cards -----------
  const feat = caps.features || {};

  // BLE button in transport mode selector
  const btBtn = $('#btModeBtn');
  if (btBtn) btBtn.style.display = feat.has_ble ? '' : 'none';

  // Dongle button is always visible on current boards, but respect the flag
  // if the firmware advertises otherwise.

  // Blend card — only show if the firmware supports continuous blending
  const blendCard = $('#blendCard');
  if (blendCard) blendCard.style.display = feat.blend_mode ? '' : 'none';

  // Advanced pattern mode seg (fray-d port) — the toggle only exists when
  // the firmware carries the engine. Panel visibility itself follows the
  // device's reported ap_mode via pattern.js adoptApState().
  const patSeg = $('#patModeSeg');
  if (patSeg && feat.advanced_pattern) patSeg.hidden = false;

  // Current / voltage chips are already patched by pollStatus() each cycle
  // based on has_current_sensor / has_power_monitor — no static patch needed.

    // Health tab cards — build them dynamically
    buildHealthCards(caps, feat);

    // RS485 Modbus card (§7) — shown when the firmware advertises has_rs485
    if (feat.has_rs485) {
      buildRs485Card(caps);
    }

  // ---- Footer "fw" chip — the OTA verification surface (§1.6h). Always the
  // device-reported version, never a baked-in string. ----------------------
  if (caps.fw_version) setRead('fwChip', caps.fw_version);

  // ---- 4. Dev assertion — log any element still carrying a stale literal ---
  if (typeof window.__CAPS_DEBUG === 'function') {
    window.__CAPS_DEBUG(caps);
  }
}

/**
 * Re-derive EVERY speed/accel slider `max` attr from the stored capability
 * ceilings when expert mode flips. Called from settings.js when #expertMode
 * changes. No more hardcoded 10000/5000 — the API is the sole source of truth. :3
 */
export function applyExpertCeilings() {
  if (!capsCache) return;
  const speed = capsCache.speed_ceiling_mm_s || {};
  const accel = capsCache.accel_ceiling_mm_s2 || {};

  // GOLDEN RULE (Thing 5): the safe ceilings live on the MACHINE, the UI just
  // asks. Normal mode is 1000 mm/s / 20000 mm/s², expert goes higher — but the
  // UI never bakes those numbers in. We read them straight from the advertised
  // /api/capabilities payload. If a tier is genuinely absent (old firmware),
  // we leave that slider's max untouched rather than inventing a ceiling the
  // firmware never authorized. :3
  const spdMax = expertMode ? speed.expert : speed.normal;
  const accMax = expertMode ? accel.expert : accel.normal;

  // Patch every speed slider — only when the machine advertised a real ceiling.
  if (typeof spdMax === 'number' && spdMax > 0) {
    ['maxSpeed', 'defMaxSpeed', 'userMaxSpeed', 'inputMaxSpeed'].forEach(function(id) {
      const s = $(id); if (!s) return;
      s.max = spdMax;
      if (parseFloat(s.value) > parseFloat(s.max)) s.value = s.max;
      s.dispatchEvent(new Event('input'));
    });
  }

  // Patch every accel slider — only when the machine advertised a real ceiling.
  if (typeof accMax === 'number' && accMax > 0) {
    ['accel', 'defAccel', 'userAccel', 'inputAccel'].forEach(function(id) {
      const s = $(id); if (!s) return;
      s.max = accMax;
      if (parseFloat(s.value) > parseFloat(s.max)) s.value = s.max;
      s.dispatchEvent(new Event('input'));
    });
  }

  // Other ceilings (genRate, modRate, modAmp) keep their fixed ranges —
  // they're not machine-specific.

  // Reflect expert-mode banner + checkbox
  var eb = $('#expBanner'); if (eb) eb.classList.toggle('show', expertMode);
  var em = $('#expertMode'); if (em) em.checked = expertMode;
}

/**
 * Build Load card (INA228 power/current/voltage/temp/peak) and Link card
 * (WiFi RSSI/channel/BSSID/reconnects) for the Health tab.
 * Replaces the static "not available" placeholders in index.html. :3
 */
function buildHealthCards(caps, feat) {
  const healthTab = $('#health');
  if (!healthTab) return;

  // --- Power card (02) — 5 Meter instances + 60s sparkline (§2.2) ---------
  // The old standalone driver-health placeholder card is folded into this
  // card's .info tooltip (§2.2d) instead of a separate sentence-in-a-box.
  const loadCard = $('#loadCard');
  if (loadCard && feat.has_power_monitor) {
    var driverMsg = 'Live INA228 readings off the 5mΩ shunt on the 36V motor bus. Bus current is the total draw — motor + logic. Die temp is the INA228’s internal temperature sensor, same die as the shunt amp. Peak current is the highest |A| seen since boot. Servo drive active — step/dir closed-loop via FastAccelStepper' +
      (feat.has_rs485 ? ', cross-checked over RS485 Modbus.' : '.');
    loadCard.innerHTML =
      '<div class="card-head">' +
        '<span data-ico="i-zap-off"></span><h2>Power</h2>' +
        '<span class="card-state">36V bus · INA228</span>' +
        '<button class="info" data-tip="' + driverMsg + '"><span data-ico="i-info"></span></button>' +
      '</div>' +
      '<div class="card-body">' +
        '<div id="meters"></div>' +
        '<div id="sparkWrap" class="wave-scope screen" data-tip="Bus power over the last 60 seconds"><canvas id="sparkCanvas"></canvas><span class="mn scr-tag">load · 60s</span></div>' +
        '<button class="btn ghost sm" id="resetPeaksBtn" style="margin-top:10px"><span data-ico="i-reset"></span> Reset peaks</button>' +
      '</div>';
    if (typeof window.injectIcons === 'function') window.injectIcons();

    // Hazard zone numbers — reused from the app's OWN existing thresholds,
    // not invented (§2.2a "derive... log the numbers used"):
    //  - BUS V: LOW-end hazard, warn <24V (matches main.js's header bus-
    //    voltage dot logic: `busV < 24 ? 'warn'` / `< 22 ? 'bad'`) — the
    //    prior .stat implementation could only flag "too high" and set
    //    thresholds impossibly above 36V nominal to suppress false positives,
    //    so undervoltage was NEVER actually highlighted. This Meter hazard
    //    zone is a real fix, not just a reskin.
    //  - BUS A / PEAK A: warn >=8A (prior setStat w1, 0-20A range).
    //  - DIE °C: warn >=70°C (prior setStat w1, 0-100°C range).
    //  - BUS W: no prior threshold existed; derived as
    //    currentHazardA(8) * nominalBusV(36) = 288W within a 0-800W range.
    var mHost = $('#meters');
    if (mHost) {
      _powerMeters = {
        busV: new Meter(mHost, { label: 'BUS V', min: 20, max: 40, decimals: 1, hazards: [[0, 0.2]], tip: 'Bus voltage · hazard at the LOW end — undervoltage is the fault (warn <24V)' }),
        busA: new Meter(mHost, { label: 'BUS A', min: 0, max: 20, decimals: 2, hazards: [[0.4, 0.6]], peakHold: 'ratchet', tip: 'Bus current · total draw, motor + logic · amber caret holds the peak since boot' }),
        busW: new Meter(mHost, { label: 'BUS W', min: 0, max: 800, decimals: 1, hazards: [[0.36, 0.64]], peakHold: 'ratchet', tip: 'Bus power · voltage × current · amber caret holds the peak since boot' }),
        dieC: new Meter(mHost, { label: 'DIE °C', min: 0, max: 100, decimals: 1, hazards: [[0.7, 0.3]], tip: 'INA228 die temperature · same die as the shunt amplifier' }),
        peakA: new Meter(mHost, { label: 'PEAK A', min: 0, max: 20, decimals: 2, hazards: [[0.4, 0.6]], tip: 'Highest current seen since boot (device-reported) · Reset peaks clears it' })
      };
    }
    initPowerSparkline();

    var rp = $('#resetPeaksBtn');
    if (rp) rp.addEventListener('click', async function() {
      try { await fetch('/api/settings', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ reset_peaks: true }) }); } catch(e) {}
      if (_powerMeters) { _powerMeters.busA.resetPeak(); _powerMeters.busW.resetPeak(); }
    });
  } else if (loadCard && !feat.has_power_monitor) {
    loadCard.innerHTML = '<div class="card-body" style="text-align:center;padding:20px;color:var(--tx-mut)">Load monitoring not available — no INA228 power monitor on this board.</div>';
  }

  // Link telemetry (RSSI/ch/bssid/disc) now lives in the page footer (§1.6h/
  // §2.3) — static markup in index.html, written by refreshHealthCards()
  // below. No dynamic #linkCard is built here anymore.
}

// ===================== Power sparkline (§2.2c) =====================
// 60s bus-power history, recessed .screen canvas. Ring buffer of
// (timestamp, watts) pairs pruned by AGE at draw time (not sample count) so
// the 60s window holds regardless of push cadence, which varies between WS
// mode (~500ms, 0x02 STATUS) and HTTP fallback (~100ms poll) — mirrors the
// rail comet-trail's own age-based ring idiom without touching rail.js.
// Allocation-free: fixed typed arrays, drawn at a throttled ~3Hz gate (not
// per push) — "2-4Hz is plenty, it's history, not the rail" per spec.
var SPARK_CAP = 400;
var _sparkT = new Float64Array(SPARK_CAP);
var _sparkW = new Float64Array(SPARK_CAP);
var _sparkHead = 0, _sparkLen = 0;
var SPARK_WINDOW_MS = 60000;
var SPARK_DRAW_INTERVAL_MS = 300;
var _sparkCv = null, _sparkCtx = null, _sparkDpr = 1, _sparkRectW = 0, _sparkRectH = 0;
var _sparkLastDrawMs = 0;
var _powerMeters = null;

function sizeSparkCanvas() {
  if (!_sparkCv) return;
  var w = _sparkCv.clientWidth, h = _sparkCv.clientHeight;
  _sparkDpr = window.devicePixelRatio || 1;
  var cw = Math.round(w * _sparkDpr), ch = Math.round(h * _sparkDpr);
  if (_sparkCv.width !== cw || _sparkCv.height !== ch) { _sparkCv.width = cw; _sparkCv.height = ch; }
  _sparkRectW = w; _sparkRectH = h;
}

function initPowerSparkline() {
  _sparkCv = $('#sparkCanvas');
  if (!_sparkCv) return;
  _sparkCtx = _sparkCv.getContext('2d');
  sizeSparkCanvas();
  window.addEventListener('resize', sizeSparkCanvas);
  if (typeof ResizeObserver !== 'undefined') {
    var ro = new ResizeObserver(function () { sizeSparkCanvas(); });
    ro.observe(_sparkCv);
  }
}

function pushSparkSample(nowMs, watts) {
  var idx = (_sparkHead + _sparkLen) % SPARK_CAP;
  if (_sparkLen === SPARK_CAP) {
    _sparkHead = (_sparkHead + 1) % SPARK_CAP;
    _sparkLen--;
    idx = (_sparkHead + _sparkLen) % SPARK_CAP;
  }
  _sparkT[idx] = nowMs;
  _sparkW[idx] = watts;
  _sparkLen++;
}

function drawSparkline(nowMs) {
  if (!_sparkCtx || _sparkRectW === 0) return;
  if (nowMs - _sparkLastDrawMs < SPARK_DRAW_INTERVAL_MS) return;
  _sparkLastDrawMs = nowMs;
  var ctx = _sparkCtx, w = _sparkRectW, h = _sparkRectH;
  ctx.setTransform(_sparkDpr, 0, 0, _sparkDpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  if (_sparkLen < 2) return;
  var maxW = 50; // floor so a quiet bus doesn't zoom the trace into noise
  var i, idx, age;
  for (i = 0; i < _sparkLen; i++) {
    idx = (_sparkHead + i) % SPARK_CAP;
    if (_sparkW[idx] > maxW) maxW = _sparkW[idx];
  }
  ctx.strokeStyle = ac('r', 0.7);
  ctx.lineWidth = 1;
  ctx.shadowColor = ac('r', 0.35);
  ctx.shadowBlur = 4;
  ctx.beginPath();
  var started = false;
  for (i = 0; i < _sparkLen; i++) {
    idx = (_sparkHead + i) % SPARK_CAP;
    age = nowMs - _sparkT[idx];
    if (age > SPARK_WINDOW_MS) continue;
    var x = w - (age / SPARK_WINDOW_MS) * w;
    var y = h - 4 - (_sparkW[idx] / maxW) * (h - 8);
    if (!started) { ctx.moveTo(x, y); started = true; } else { ctx.lineTo(x, y); }
  }
  if (started) ctx.stroke();
  ctx.shadowBlur = 0;
}

// Zero-pad a small integer to 3 digits (footer "ch"/"disc" chips, matching
// the mock's `06`/`002` look) — '--' when the value is genuinely absent.
function pad3(v) {
  var n = typeof v === 'number' ? v : parseInt(v, 10);
  if (isNaN(n)) return '--';
  n = Math.max(0, Math.min(999, n));
  return (n < 10 ? '00' : n < 100 ? '0' : '') + n;
}
// First 3 octets only (short form, §2.3) — enough to identify the AP without
// the full MAC's width.
function shortBssid(b) {
  if (!b || typeof b !== 'string') return '--';
  var parts = b.split(':');
  return parts.length >= 3 ? parts.slice(0, 3).join(':') : b;
}

/**
 * Update the Power card + footer LINK strip from a status poll payload.
 * Called from pollStatus() each cycle alongside the toolbar chips. :3
 */
export function refreshHealthCards(d) {
  // ---- Power card — 5 Meter instances + sparkline ----
  if (d.has_power_monitor && _powerMeters) {
    var volts = d.bus_voltage_v || 0;
    var amps = Math.abs(d.bus_current_a || 0);
    var power = d.bus_power_w || 0;
    var dieC = d.die_temp_c || 0;
    var peakA = Math.abs(d.peak_current_a || 0);
    _powerMeters.busV.set(volts);
    _powerMeters.busA.set(amps);
    _powerMeters.busW.set(power);
    _powerMeters.dieC.set(dieC);
    _powerMeters.peakA.set(peakA);
    var nowMs = performance.now();
    pushSparkSample(nowMs, power);
    drawSparkline(nowMs);
  }

  // ---- Footer LINK strip (§1.6h/§2.3) ----
  if (d.wifi_connected) {
    const rssi = d.rssi || 0;
    // EMA-smooth the displayed dBm — raw RSSI jitters a few dB between
    // polls, which made the footer number (and bar) wiggle constantly.
    // Seed on first sample; ~0.2 alpha settles a step change in ~2s at the
    // status cadence while flattening the sample-to-sample flutter.
    if (_rssiEma === null) _rssiEma = rssi;
    _rssiEma += 0.2 * (rssi - _rssiEma);
    var shown = Math.round(_rssiEma);
    setRead('linkRssi', shown + ' dBm');
    // RSSI bar: map -90..-30 → 0..100% (from the smoothed value, so the bar
    // glides instead of twitching; the CSS width transition does the rest)
    var rssiPct = clamp((shown + 90) / 60 * 100, 0, 100);
    var rf = $('#rssiFill');
    if (rf) rf.style.width = rssiPct + '%';
    setRead('linkCh', pad3(d.wifi_channel));
    // bssid is a STRING the binary 0x02 status frame cannot carry — in WS
    // mode it arrives only via the slow HTTP link-meta poll (main.js).
    // Sticky: only overwrite when a real value is present, so a WS-status
    // refresh between HTTP polls can't blank an already-known AP.
    if (typeof d.wifi_bssid === 'string' && d.wifi_bssid) {
      setRead('linkBssid', shortBssid(d.wifi_bssid));
    }
    // "disc" = disconnect/reconnect count since boot (mock's own tooltip
    // wording) — the separate last-disconnect-reason display from the old
    // Health-tab link card was retired; the footer's chip set is exactly
    // RSSI + ch/bssid/disc + fw/ui, matching the approved mock (R0).
    setRead('linkDisc', pad3(d.wifi_reconnects));
  }
}
var _rssiEma = null;

/** Direct footer-bssid write for the slow HTTP link-meta poll (main.js) —
 *  the binary WS status path has no bssid field to feed refreshHealthCards. */
export function setLinkBssid(b) {
  if (typeof b === 'string' && b) setRead('linkBssid', shortBssid(b));
}

/**
 * Build a placeholder RS485 Modbus card in the Health tab.
 * Shown only when has_rs485 is true (plan.md §7). :3
 */
function buildRs485Card(caps) {
  const healthTab = $('#health');
  if (!healthTab) return;
  if ($('#rs485Card')) return; // already built

  const card = document.createElement('div');
  card.className = 'card';
  card.id = 'rs485Card';
  card.innerHTML = `
    <div class="card-head">
      <span data-ico="i-cable"></span><h2>Servo (RS485)</h2>
      <button class="info" data-tip="Modbus RTU telemetry from the AIM servo drive over RS485. Live drive-reported voltage, current, temperature, and alarm flags — cross-checks the INA228 on the motor bus. Config writes (enable, speed/accel, gains, DIR polarity, save-to-flash) available from this card."><span data-ico="i-info"></span></button>
    </div>
    <div class="card-body" style="text-align:center;padding:20px;color:var(--muted)">
      RS485 Modbus telemetry will be displayed here once the module is wired.
    </div>`;
  healthTab.appendChild(card);
  if (typeof window.injectIcons === 'function') window.injectIcons();
}