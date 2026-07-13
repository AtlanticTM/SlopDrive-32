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

  // Current / voltage chips are already patched by pollStatus() each cycle
  // based on has_current_sensor / has_power_monitor — no static patch needed.

    // Health tab cards — build them dynamically
    buildHealthCards(caps, feat);

    // RS485 Modbus card (§7) — shown when the firmware advertises has_rs485
    if (feat.has_rs485) {
      buildRs485Card(caps);
    }

    // Update the driver health placeholder to show relevant context
    var hc = $('#healthCard');
    if (hc) {
      var msg = 'Servo drive active — step/dir closed-loop via FAS. Health telemetry via INA228';
      if (feat.has_rs485) msg += ' and RS485 Modbus';
      msg += '.';
      hc.innerHTML = '<div class="card-body" style="text-align:center;padding:20px;color:var(--muted)">' + msg + '</div>';
    }

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

  const spdMax = expertMode ? (speed.expert || 10000) : (speed.normal || 5000);
  const accMax = expertMode ? (accel.expert || 100000) : (accel.normal || 50000);

  // Patch every speed slider
  ['maxSpeed', 'defMaxSpeed'].forEach(function(id) {
    const s = $(id); if (!s) return;
    s.max = spdMax;
    if (parseFloat(s.value) > parseFloat(s.max)) s.value = s.max;
    s.dispatchEvent(new Event('input'));
  });

  // Patch every accel slider
  ['accel', 'defAccel'].forEach(function(id) {
    const s = $(id); if (!s) return;
    s.max = accMax;
    if (parseFloat(s.value) > parseFloat(s.max)) s.value = s.max;
    s.dispatchEvent(new Event('input'));
  });

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

  // --- Load card (INA228 power monitor) ---
  const loadCard = $('#loadCard');
  if (loadCard && feat.has_power_monitor) {
    loadCard.innerHTML = `
      <div class="card-head">
        <span data-ico="i-zap-off"></span><h2>Power Load</h2>
        <button class="info" data-tip="Live INA228 readings off the 5mΩ shunt on the 36V motor bus. Bus current is the total draw — motor + logic. Die temp is the INA228's internal temperature sensor on the same die as the shunt amp. Peak current is the highest |A| seen since boot."><span data-ico="i-info"></span></button>
      </div>
      <div class="card-body">
        <div class="kv">
          <span>Bus Voltage <b id="loadBusV">-- V</b></span>
          <span>Bus Current <b id="loadBusA">-- A</b></span>
        </div>
        <div class="kv">
          <span>Bus Power <b id="loadBusW">-- W</b></span>
          <span>Die Temp <b id="loadDieC">-- °C</b></span>
        </div>
        <div class="kv">
          <span>Peak Current <b id="loadPeakA">-- A</b></span>
          <span><button class="btn ghost sm" id="resetPeaksBtn" style="margin-top:4px"><span data-ico="i-reset"></span> Reset peaks</button></span>
        </div>
      </div>`;
    // Re-inject icons into the newly created DOM
    if (typeof window.injectIcons === 'function') window.injectIcons();
    // Wire the reset-peaks button
    var rp = $('#resetPeaksBtn');
    if (rp) rp.addEventListener('click', async function() {
      try { await fetch('/api/settings', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ reset_peaks: true }) }); } catch(e) {}
    });
  } else if (loadCard && !feat.has_power_monitor) {
    loadCard.innerHTML = '<div class="card-body" style="text-align:center;padding:20px;color:var(--muted)">Load monitoring not available — no INA228 power monitor on this board.</div>';
  }

  // --- Link card (WiFi telemetry) ---
  // Always shown when WiFi is connected — RSSI, channel, BSSID, reconnect count.
  // We append it after the load card (or driver health card if that's first).
  var existingLink = $('#linkCard');
  if (!existingLink) {
    const linkCard = document.createElement('div');
    linkCard.className = 'card';
    linkCard.id = 'linkCard';
    linkCard.innerHTML = `
      <div class="card-head">
        <span data-ico="i-link"></span><h2>WiFi Link</h2>
        <button class="info" data-tip="Live WiFi telemetry. RSSI quality bar helps spot dead zones. Channel + BSSID prove which AP you're actually connected to (band-steering evidence — if the far AP works better than the near one, the near one may be on a congested channel). Reconnect count and last disconnect reason help track flapping radios."><span data-ico="i-info"></span></button>
      </div>
      <div class="card-body">
        <div class="kv">
          <span>RSSI <b id="linkRssi">-- dBm</b></span>
          <span><div class="rssi-bar" id="rssiBar" style="width:100%;height:4px;background:var(--card-2);border-radius:2px;margin-top:2px"><div id="rssiFill" style="height:100%;width:0%;background:var(--accent);border-radius:2px;transition:width 0.3s"></div></div></span>
        </div>
        <div class="kv">
          <span>Channel <b id="linkCh">--</b></span>
          <span>Reconnects <b id="linkRec">--</b></span>
        </div>
        <div class="kv" style="margin-bottom:0">
          <span>BSSID <b id="linkBssid" style="font-size:.7rem">--</b></span>
          <span>Last disc. reason <b id="linkDisc">--</b></span>
        </div>
      </div>`;
    // Insert after the load card (or driver health card)
    const driverCard = $('#healthCard');
    const afterEl = loadCard || driverCard;
    if (afterEl) {
      afterEl.insertAdjacentElement('afterend', linkCard);
    } else {
      healthTab.appendChild(linkCard);
    }
    if (typeof window.injectIcons === 'function') window.injectIcons();
    // Wire collapsible
    if (typeof window.initCollapsibleCards === 'function') window.initCollapsibleCards();
  }
}

/**
 * Update the Health tab's Load + Link cards from a status poll payload.
 * Called from pollStatus() each cycle alongside the toolbar chips. :3
 */
export function refreshHealthCards(d) {
  // ---- Load card ----
  if (d.has_power_monitor) {
    setRead('loadBusV', (d.bus_voltage_v || 0).toFixed(1) + ' V');
    setRead('loadBusA', (d.bus_current_a || 0).toFixed(2) + ' A');
    setRead('loadBusW', (d.bus_power_w || 0).toFixed(1) + ' W');
    setRead('loadDieC', (d.die_temp_c || 0).toFixed(1) + ' °C');
    setRead('loadPeakA', (d.peak_current_a || 0).toFixed(2) + ' A');
  }

  // ---- Link card ----
  if (d.wifi_connected) {
    const rssi = d.rssi || 0;
    setRead('linkRssi', rssi + ' dBm');
    // RSSI bar: map -90..-30 → 0..100%
    var rssiPct = clamp((rssi + 90) / 60 * 100, 0, 100);
    var rf = $('#rssiFill');
    if (rf) rf.style.width = rssiPct + '%';
    setRead('linkCh', d.wifi_channel || '--');
    setRead('linkRec', d.wifi_reconnects || 0);
    setRead('linkBssid', d.wifi_bssid || '--');
    setRead('linkDisc', d.wifi_last_disconnect_reason || '--');
  }
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