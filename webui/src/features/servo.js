/**
 * servo.js — AIM servo drive over RS485 Modbus: telemetry card + Configure pane.
 *
 * Two surfaces, one poll:
 *   1. #rs485Card (Health host, Drive view) — live drive-reported telemetry
 *      meters (speed/current/voltage/temp/PWM) + enable/output/alarm chips.
 *   2. #pv-configure (side pane, pill next to Settings/Log) — the programmer:
 *      live tuning, guarded structural programming, OSSM defaults, raw writes.
 *
 * Ground Truth Doctrine: every field ADOPTS the drive's register mirror (the
 * firmware's async scan readback), never its own request. An edited field goes
 * intent-purple (dirty); after Apply/Program the rescan either confirms it
 * (field settles to adopted) or the drive's value wins and the field flags
 * red (rejected). The UI never shows a value the drive didn't echo back.
 *
 * Steps/rev (reg 0x0B) is the loaded gun: the firmware recalculates steps/mm
 * LIVE when it's programmed and unhomes the machine, so the confirm modal for
 * it demands the operator literally type the new number. Let them ball — but
 * make them mean it. :3
 */

import { $, toast } from '../core/ui.js';
import { Meter } from '../core/meter.js';
import { get, post } from '../core/api.js';

// ---- Register metadata ------------------------------------------------------

const REG_NAMES = [
  'Modbus enable',        // 0x00
  'Output enable',        // 0x01
  'Target speed',         // 0x02
  'Acceleration',         // 0x03
  'Field-weaken angle',   // 0x04
  'Speed loop KP',        // 0x05
  'Speed loop Ti',        // 0x06
  'Position loop KP',     // 0x07
  'Speed feed-forward',   // 0x08
  'DIR polarity',         // 0x09
  'E-gear numerator',     // 0x0A
  'Steps/rev (e-gear den)', // 0x0B
  'Target pos LO',        // 0x0C
  'Target pos HI',        // 0x0D
  'Alarm code',           // 0x0E
  'System current',       // 0x0F
  'Motor speed',          // 0x10
  'System voltage',       // 0x11
  'Temperature',          // 0x12
  'Output PWM',           // 0x13
  'Param save flag',      // 0x14
  'Device address',       // 0x15
  'Abs position LO',      // 0x16
  'Abs position HI',      // 0x17
  'Max output',           // 0x18
  'Special function'      // 0x19
];

// OSSM gold-motor standard values (KinkyMakers gold-motor-control reference).
// reg (decimal) -> value. 0x00/0x01/0x14 are managed by the program sequence.
const OSSM_DEFAULTS = {
  2: 1500,    // target speed (r/min)
  3: 50000,   // acceleration
  4: 495,     // field-weakening angle
  5: 3000,    // speed loop KP
  6: 10,      // speed loop integration time (ms)
  7: 3000,    // position loop KP
  8: 3900,    // speed feed-forward
  9: 1,       // DIR polarity
  10: 32768,  // e-gear numerator
  11: 800,    // steps/rev  (e-gear denominator) — THE dangerous one
  24: 600     // max output (0x18)
};

const LIVE_REGS = [2, 3, 5, 6, 7, 24];          // safe while running
const STRUCT_REGS = [4, 8, 9, 10, 11, 21];      // program sequence only
const STEPS_REG = 11;
const DIR_REG = 9;

function alarmText(code) {
  if (!code) return 'no alarm';
  if (code === 0x10) return 'battery loss';
  if (code === 0x12) return 'OVERCURRENT';
  if (code === 0x14) return 'STALL';
  if (code === 0x15) return 'OVERVOLTAGE';
  return 'alarm 0x' + code.toString(16).toUpperCase();
}

// ---- Module state -----------------------------------------------------------

let _inited = false;
let _cardBuilt = false;
let _meters = null;
let _pollTimer = null;
let _fastUntil = 0;        // fast-poll deadline (ms, performance.now clock)
let _lastStamp = -1;       // cfg stamp of the mirror we last adopted
let _lastKnown = 0;
let _mirror = null;        // last adopted regs array
let _await = {};           // reg -> value we asked for (cleared on verify)
let _dirSel = null;        // DIR seg selection (null = adopt from mirror)
let _rehome = false;       // structural change happened this session
let _overlay = null;
let _autoScanned = false;  // one-shot register pull on first contact
let _geom = null;          // last device geometry (steps/rev, steps/mm)

/** Translate the Max speed field's rpm into carriage mm/s at the machine's
 *  CURRENT gearing: mm per motor rev = steps/rev ÷ steps/mm. */
function updateSpeedHint() {
  const out = $('#svR02Mms');
  const inp = $('#svR02');
  if (!out || !inp) return;
  const rpm = parseFloat(inp.value);
  if (!_geom || !_geom.steps_per_mm || isNaN(rpm)) { out.textContent = ''; return; }
  const mmPerMotorRev = _geom.motor_steps_per_rev / _geom.steps_per_mm;
  const mms = (rpm / 60) * mmPerMotorRev;
  out.textContent = '≈ ' + Math.round(mms) + ' mm/s machine max';
}

export function initServo() {
  if (_inited) return;
  _inited = true;
  wireConfigurePane();
  schedulePoll(800);
}

// ---- Polling ----------------------------------------------------------------

function schedulePoll(ms) {
  clearTimeout(_pollTimer);
  _pollTimer = setTimeout(pollServo, ms);
}

async function pollServo() {
  if (document.hidden) { schedulePoll(1500); return; }
  const d = await get('/api/servo');
  if (d) render(d);
  // 500ms base: the firmware's Modbus cycle refreshes every ~0.3s now, so a
  // 1s card poll would throw away half the samples it fought for.
  const fast = d && (d.queue > 0 || (d.cfg && d.cfg.scanning) || performance.now() < _fastUntil);
  schedulePoll(fast ? 300 : 500);
}

// ---- Render (both surfaces) -------------------------------------------------

function render(d) {
  const ready = !!d.ready;

  // Reveal the Configure pills once the drive has ever answered.
  if (ready) {
    const dt = $('#cfgTabDesk'), mt = $('#cfgTabMob');
    if (dt) dt.style.display = '';
    if (mt) mt.style.display = '';
  }

  renderTeleCard(d);
  renderConfigure(d);
}

// ---- Telemetry card (#rs485Card in the Health host) -------------------------

function buildTeleCard() {
  const body = $('#rs485Body');
  if (!body) return false;
  body.style.cssText = '';
  body.innerHTML =
    '<div class="sv-telechips">' +
      '<span class="chip" data-tip="Drive alarm register 0x0E — OVERCURRENT / STALL / OVERVOLTAGE stop the motor"><span class="dot" id="rsAlarmDot"></span><span id="rsAlarmTxt">no alarm</span></span>' +
      '<span class="chip" data-tip="Register 0x00 — drive accepts Modbus writes"><span class="dot" id="rsEnDot"></span><span>modbus</span></span>' +
      '<span class="chip" data-tip="Register 0x01 — motor power stage enabled"><span class="dot" id="rsOutDot"></span><span>output</span></span>' +
    '</div>' +
    '<div id="rs485Meters"></div>' +
    '<div id="rsEncRow" style="display:none;margin-top:8px;align-items:center;gap:8px;flex-wrap:wrap">' +
      '<span class="chip" data-tip="Drive encoder (regs 0x16/0x17) vs FastAccelStepper commanded position. Verdicts are scored at standstill only — the live delta is timing-noisy while moving."><span class="dot" id="rsEncDot"></span><span id="rsEncTxt">encoder</span></span>' +
      '<span class="mn" id="rsEncDetail" style="font-size:11px;opacity:.75"></span>' +
    '</div>' +
    '<button class="btn ghost sm" id="rsClearPeakBtn" style="margin-top:10px;width:auto"><span data-ico="i-reset"></span> Clear peak</button>';
  const host = $('#rs485Meters');
  _meters = {
    rpm:  new Meter(host, { label: 'SPEED', min: 0, max: 3000, decimals: 0, hazards: [[0.83, 0.17]], tip: 'Drive-reported motor speed (reg 0x10) · |r/min|' }),
    amp:  new Meter(host, { label: 'DRV A', min: 0, max: 10, decimals: 2, hazards: [[0.6, 0.4]], peakHold: 'ratchet', tip: 'Drive-reported motor current (reg 0x0F) · cross-checks the INA228 bus reading' }),
    volt: new Meter(host, { label: 'DRV V', min: 20, max: 40, decimals: 1, hazards: [[0, 0.2]], tip: 'Drive-reported supply voltage (reg 0x11) · hazard at the LOW end' }),
    temp: new Meter(host, { label: 'DRV °C', min: 0, max: 100, decimals: 0, hazards: [[0.7, 0.3]], tip: 'Drive internal temperature (reg 0x12)' }),
    pwm:  new Meter(host, { label: 'PWM %', min: 0, max: 100, decimals: 0, hazards: [[0.85, 0.15]], tip: 'Drive output PWM duty (reg 0x13) · |%| — sustained 100% means the loop is saturated' })
  };
  const cp = $('#rsClearPeakBtn');
  if (cp) cp.addEventListener('click', function () {
    if (_meters) _meters.amp.resetPeak();
  });
  if (typeof window.injectIcons === 'function') window.injectIcons();
  return true;
}

function renderTeleCard(d) {
  const state = $('#rs485State');
  if (!d.ready) {
    if (state) state.textContent = 'no answer · reprobing';
    return;
  }
  if (!_cardBuilt) _cardBuilt = buildTeleCard();
  if (!_cardBuilt) return;

  const t = d.tele || {};
  if (state) state.textContent = t.valid ? ('addr ' + d.addr + ' · 19200 8N1') : 'linked · first poll…';
  if (!t.valid) return;

  _meters.rpm.set(Math.abs(t.speed_rpm || 0));
  _meters.amp.set(Math.abs(t.current_a || 0));
  _meters.volt.set(t.voltage_v || 0);
  _meters.temp.set(t.temp_c || 0);
  _meters.pwm.set(Math.abs(t.pwm_pct || 0));

  const aDot = $('#rsAlarmDot'), aTxt = $('#rsAlarmTxt');
  const bad = t.alarm && t.alarm !== 0x10;
  if (aDot) aDot.className = 'dot ' + (t.alarm ? (bad ? 'bad' : 'warn') : 'ok');
  if (aTxt) aTxt.textContent = alarmText(t.alarm);
  const enDot = $('#rsEnDot'), outDot = $('#rsOutDot');
  if (enDot) enDot.className = 'dot ' + (t.enabled ? 'ok' : '');
  if (outDot) outDot.className = 'dot ' + (t.output_on ? 'ok' : 'warn');

  renderEncoder(d.enc);
}

/** FAS-vs-encoder cross-check row. Ground Truth: everything shown here is the
 *  firmware's own verdict — the UI never computes deviation itself. */
function renderEncoder(e) {
  const row = $('#rsEncRow');
  if (!row) return;
  if (!e || !e.valid) { row.style.display = 'none'; return; }
  row.style.display = 'flex';

  const dot = $('#rsEncDot'), txt = $('#rsEncTxt'), det = $('#rsEncDetail');
  const fmt = function (v) { return (v >= 0 ? '+' : '') + v.toFixed(2); };

  if (e.warn) {
    if (dot) dot.className = 'dot bad';
    if (txt) txt.textContent = 'LOST STEPS? Δ ' + fmt(e.dev_steady_mm) + ' mm — re-home';
  } else if (e.state === 2) {
    if (dot) dot.className = 'dot ok';
    if (txt) txt.textContent = 'encoder Δ ' + fmt(e.dev_mm) + ' mm';
  } else if (e.state === 1) {
    if (dot) dot.className = 'dot warn';
    if (txt) txt.textContent = 'encoder · measuring direction…';
  } else {
    if (dot) dot.className = 'dot';
    if (txt) txt.textContent = 'encoder · awaiting home';
  }

  if (det) {
    let s = e.counts + ' cnt';
    if (e.state === 2) {
      s += ' · steady ' + fmt(e.dev_steady_mm) + ' · max ' + e.max_steady_mm.toFixed(2) + ' mm';
      // Measured vs theoretical counts/mm — flags a wrong geometry model.
      if (e.cpmm_meas > 0 && e.cpmm_theory > 0) {
        const pct = (e.cpmm_meas / e.cpmm_theory) * 100;
        if (pct < 90 || pct > 110) s += ' · scale ' + pct.toFixed(0) + '%!';
      }
    }
    det.textContent = s;
  }
}

// ---- Configure pane ---------------------------------------------------------

function regInputs() {
  return Array.prototype.slice.call(
    document.querySelectorAll('#pv-configure input.num[data-reg]'));
}

function markDirty(inp) {
  const adopted = inp.dataset.adopted;
  const dirty = adopted !== undefined && String(inp.value) !== String(adopted);
  inp.classList.toggle('sv-dirty', dirty);
  if (dirty) inp.classList.remove('sv-reject');
}

function renderConfigure(d) {
  const link = $('#svLinkState'), dot = $('#svLinkDot'), txt = $('#svLinkTxt');
  if (link) link.textContent = d.ready ? 'linked' : 'no answer';
  if (dot) dot.className = 'dot ' + (d.ready ? 'ok' : 'bad');
  if (txt) txt.textContent = d.ready ? 'RS485 up' : 'RS485 down';
  const q = $('#svQueue'); if (q) q.textContent = d.queue || 0;
  const addr = $('#svAddr'); if (addr) addr.textContent = d.addr || '–';

  const t = d.tele || {};
  const aDot = $('#svAlarmDot'), aTxt = $('#svAlarmTxt');
  const bad = t.alarm && t.alarm !== 0x10;
  if (aDot) aDot.className = 'dot ' + (t.alarm ? (bad ? 'bad' : 'warn') : 'ok');
  if (aTxt) aTxt.textContent = alarmText(t.alarm || 0);

  const g = d.geom || {};
  const spm = $('#svStepsMm');
  if (spm && g.steps_per_mm) {
    spm.textContent = g.steps_per_mm.toFixed(3) + ' (spr ' + g.motor_steps_per_rev + ')';
  }
  if (g.steps_per_mm) { _geom = g; updateSpeedHint(); }

  // Re-home banner lifecycle: raised by a structural program, lowered the
  // moment the machine reports homed again.
  const rb = $('#svRehomeBanner');
  if (rb) {
    if (_rehome && g.homed) { _rehome = false; toast('Re-homed — geometry change complete :3'); }
    rb.classList.toggle('show', _rehome && !g.homed);
  }

  const scanHint = $('#svScanHint');
  const cfg = d.cfg || {};
  if (scanHint) {
    scanHint.textContent = d.queue > 0 ? ('Writing — ' + d.queue + ' frame(s) queued…')
      : cfg.scanning ? 'Scanning registers…'
      : cfg.valid ? ('Registers read ' + Math.round((cfg.age_ms || 0) / 1000) + 's ago.')
      : 'No register scan yet — hit Read registers.';
  }

  // First contact: pull the register mirror once without being asked.
  if (d.ready && !cfg.valid && !cfg.scanning && !_autoScanned) {
    _autoScanned = true;
    doScan();
  }

  // Adopt only a SETTLED mirror — never while writes are queued or a rescan is
  // in flight, or pending-write verification would run against stale values.
  if (cfg.valid && !cfg.scanning && !(d.queue > 0)) adoptMirror(cfg);
}

/** Adopt a fresh register mirror into the pane (Ground Truth adoption). */
function adoptMirror(cfg) {
  // The firmware sends age_ms, not an absolute stamp — synthesize a stamp and
  // only re-adopt when a NEW scan committed (age reset below our poll period).
  const stamp = Math.round(performance.now() - (cfg.age_ms || 0));
  if (_lastStamp >= 0 && Math.abs(stamp - _lastStamp) < 1500) return;
  _lastStamp = stamp;
  _lastKnown = cfg.known || 0;
  _mirror = cfg.regs;

  regInputs().forEach(function (inp) {
    const reg = parseInt(inp.dataset.reg, 10);
    const known = (_lastKnown >>> reg) & 1;
    const val = cfg.regs[reg];

    // Verify a pending write: drive echoed our value -> confirmed; drive kept
    // its own -> rejected (red). Either way the mirror wins the display.
    if (_await[reg] !== undefined && known) {
      if (Number(_await[reg]) === val) {
        delete _await[reg];
      } else {
        inp.classList.add('sv-reject');
        toast('Drive kept reg 0x' + reg.toString(16).toUpperCase() + ' = ' + val + ' (asked ' + _await[reg] + ')');
        delete _await[reg];
      }
    }

    inp.dataset.adopted = known ? String(val) : '';
    if (document.activeElement === inp) return;           // never clobber typing
    if (inp.classList.contains('sv-dirty') &&
        String(inp.value) !== String(val)) return;        // preserve unapplied edits
    if (known) { inp.value = val; inp.classList.remove('sv-dirty'); }
  });

  // DIR polarity seg
  const seg = $('#svDirSeg');
  if (seg && ((_lastKnown >>> DIR_REG) & 1)) {
    const devDir = cfg.regs[DIR_REG] ? 1 : 0;
    if (_await[DIR_REG] !== undefined) delete _await[DIR_REG];
    if (_dirSel === devDir) _dirSel = null;               // selection confirmed
    const shown = (_dirSel === null) ? devDir : _dirSel;
    seg.querySelectorAll('button').forEach(function (b) {
      b.classList.toggle('active', parseInt(b.dataset.dir, 10) === shown);
      b.classList.toggle('sv-pending', _dirSel !== null && parseInt(b.dataset.dir, 10) === shown);
    });
  }

  renderRegTable(cfg);
  updateSpeedHint();   // svR02 may have just adopted a new value
}

function renderRegTable(cfg) {
  const pre = $('#svRegTable');
  if (!pre) return;
  let out = 'reg   value   hex     name\n';
  for (let r = 0; r < REG_NAMES.length; r++) {
    const known = (cfg.known >>> r) & 1;
    const v = cfg.regs[r];
    out += '0x' + r.toString(16).toUpperCase().padStart(2, '0') + '  ' +
      (known ? String(v).padStart(6) : '    --') + '  ' +
      (known ? ('0x' + v.toString(16).toUpperCase().padStart(4, '0')) : '    --') + '  ' +
      REG_NAMES[r] + '\n';
  }
  pre.textContent = out;
}

// ---- Actions ----------------------------------------------------------------

function collectDirty(regSet) {
  const out = {};
  regInputs().forEach(function (inp) {
    const reg = parseInt(inp.dataset.reg, 10);
    if (regSet.indexOf(reg) === -1) return;
    if (!inp.classList.contains('sv-dirty')) return;
    const v = parseInt(inp.value, 10);
    if (!isNaN(v)) out[reg] = v;
  });
  return out;
}

function fastPoll() {
  _fastUntil = performance.now() + 6000;
  _lastStamp = -1;          // force re-adoption of the next committed scan
  schedulePoll(250);
}

async function doScan() {
  await post('/api/servo', { scan: true });
  _lastStamp = -1;
  fastPoll();
}

async function applyLive() {
  const changes = collectDirty(LIVE_REGS);
  if (!Object.keys(changes).length) { toast('Nothing to apply — no live fields changed'); return; }
  Object.keys(changes).forEach(function (r) { _await[r] = changes[r]; });
  const rsp = await post('/api/servo', { live: changes });
  const j = rsp ? await rsp.json().catch(function () { return null; }) : null;
  if (!j || j.ok === false) { toast('Live tuning write failed' + (j && j.error ? ' — ' + j.error : '')); return; }
  toast('Live tuning sent — verifying by readback…');
  fastPoll();
}

function structuralChanges() {
  const changes = collectDirty(STRUCT_REGS.concat(LIVE_REGS));
  if (_dirSel !== null) changes[DIR_REG] = _dirSel;
  return changes;
}

async function programDrive(changes) {
  Object.keys(changes).forEach(function (r) { _await[r] = changes[r]; });
  const rsp = await post('/api/servo', { program: changes, save: true });
  const j = rsp ? await rsp.json().catch(function () { return null; }) : null;
  if (!j || j.ok === false) {
    const why = j && j.error === 'machine_busy'
      ? 'machine is moving — stop the pattern first'
      : (j && j.error) || 'no response';
    toast('Programming REFUSED — ' + why);
    return;
  }
  if (j.rehome_required) {
    _rehome = true;
    const det = $('#svRehomeDetail');
    if (det && j.steps_per_mm) det.textContent = 'New steps/mm: ' + j.steps_per_mm.toFixed(3) + '.';
    const rb = $('#svRehomeBanner'); if (rb) rb.classList.add('show');
  }
  toast('Programming sequence queued (' + j.queued + ' frames) — verifying…');
  fastPoll();
}

// ---- Confirm overlay --------------------------------------------------------

function buildOverlay() {
  if (_overlay) return _overlay;
  const ov = document.createElement('div');
  ov.className = 'sv-overlay';
  ov.innerHTML = '<div class="sv-modal" role="dialog" aria-modal="true"></div>';
  document.body.appendChild(ov);
  ov.addEventListener('click', function (e) { if (e.target === ov) hideOverlay(); });
  _overlay = ov;
  return ov;
}
function hideOverlay() { if (_overlay) _overlay.classList.remove('show'); }

/**
 * Confirm modal. If stepsNew !== null the operator must TYPE the new steps/rev
 * value to unlock the confirm button — a click-through can't do it.
 */
function confirmProgram(changes, stepsNew, onConfirm) {
  const ov = buildOverlay();
  const modal = ov.querySelector('.sv-modal');
  const danger = stepsNew !== null;

  let rows = '';
  Object.keys(changes).sort(function (a, b) { return a - b; }).forEach(function (r) {
    const reg = parseInt(r, 10);
    const old = (_mirror && ((_lastKnown >>> reg) & 1)) ? _mirror[reg] : '--';
    rows += '<div><span>' + (REG_NAMES[reg] || 'reg') +
      ' <span class="mn">0x' + reg.toString(16).toUpperCase().padStart(2, '0') + '</span></span>' +
      '<span class="mn">' + old + ' &rarr; <b>' + changes[r] + '</b></span></div>';
  });

  modal.innerHTML =
    '<h3 class="' + (danger ? 'danger' : '') + '">' + (danger ? '&#9888; STEPS/REV CHANGE' : 'Program drive') + '</h3>' +
    (danger
      ? '<div class="sv-danger-text" style="margin-bottom:6px"><b>You are rewriting the machine\'s sense of distance.</b> ' +
        'The drive will be reprogrammed to <b>' + stepsNew + ' steps/rev</b>, firmware steps/mm is recalculated ' +
        'immediately, and the machine is <b>unhomed — all motion stays blocked until you re-home</b>. ' +
        'A wrong value here turns a 10&nbsp;mm stroke into a full-force ram.</div>'
      : '<div class="hint" style="margin-bottom:6px">Motor output is briefly disabled while the sequence writes (each register &times;3), then settings are saved to the drive\'s EEPROM and verified by readback.</div>') +
    '<div class="sv-changes">' + rows + '</div>' +
    (danger
      ? '<div class="sv-typegate"><div class="mini-label">Type <b>' + stepsNew + '</b> to arm the confirm button</div>' +
        '<input class="num" id="svGateInput" type="text" inputmode="numeric" autocomplete="off" placeholder="' + stepsNew + '"></div>'
      : '') +
    '<div class="btn-row" style="margin-top:12px">' +
      '<button class="btn ghost" id="svCancelBtn" style="width:auto">Cancel</button>' +
      '<button class="btn primary" id="svConfirmBtn" style="width:auto"' + (danger ? ' disabled' : '') + '>' +
      (danger ? 'Program &amp; unhome' : 'Program') + '</button>' +
    '</div>';

  modal.querySelector('#svCancelBtn').addEventListener('click', hideOverlay);
  const okBtn = modal.querySelector('#svConfirmBtn');
  okBtn.addEventListener('click', function () { hideOverlay(); onConfirm(); });
  if (danger) {
    const gate = modal.querySelector('#svGateInput');
    gate.addEventListener('input', function () {
      okBtn.disabled = gate.value.trim() !== String(stepsNew);
    });
    setTimeout(function () { gate.focus(); }, 50);
  }
  ov.classList.add('show');
}

// ---- Wiring -----------------------------------------------------------------

function wireConfigurePane() {
  regInputs().forEach(function (inp) {
    inp.addEventListener('input', function () {
      markDirty(inp);
      if (inp.id === 'svR02') updateSpeedHint();
    });
  });

  const seg = $('#svDirSeg');
  if (seg) seg.querySelectorAll('button').forEach(function (b) {
    b.addEventListener('click', function () {
      const v = parseInt(b.dataset.dir, 10);
      const devDir = (_mirror && ((_lastKnown >>> DIR_REG) & 1)) ? (_mirror[DIR_REG] ? 1 : 0) : null;
      _dirSel = (v === devDir) ? null : v;
      seg.querySelectorAll('button').forEach(function (x) {
        x.classList.toggle('active', parseInt(x.dataset.dir, 10) === v);
      });
    });
  });

  const scanBtn = $('#svScanBtn');
  if (scanBtn) scanBtn.addEventListener('click', doScan);

  const saveBtn = $('#svSaveBtn');
  if (saveBtn) saveBtn.addEventListener('click', async function () {
    await post('/api/servo', { save: true });
    toast('Save-to-EEPROM sent to the drive (reg 0x14)');
    fastPoll();
  });

  const applyBtn = $('#svApplyLiveBtn');
  if (applyBtn) applyBtn.addEventListener('click', applyLive);

  const progBtn = $('#svProgramBtn');
  if (progBtn) progBtn.addEventListener('click', function () {
    const changes = structuralChanges();
    if (!Object.keys(changes).length) { toast('Nothing to program — no fields changed'); return; }
    const stepsNew = changes[STEPS_REG] !== undefined ? changes[STEPS_REG] : null;
    confirmProgram(changes, stepsNew, function () { programDrive(changes); });
  });

  const defBtn = $('#svDefaultsBtn');
  if (defBtn) defBtn.addEventListener('click', function () {
    regInputs().forEach(function (inp) {
      const reg = parseInt(inp.dataset.reg, 10);
      if (OSSM_DEFAULTS[reg] === undefined) return;
      inp.value = OSSM_DEFAULTS[reg];
      markDirty(inp);
    });
    const devDir = (_mirror && ((_lastKnown >>> DIR_REG) & 1)) ? (_mirror[DIR_REG] ? 1 : 0) : null;
    _dirSel = (OSSM_DEFAULTS[DIR_REG] === devDir) ? null : OSSM_DEFAULTS[DIR_REG];
    if (seg) seg.querySelectorAll('button').forEach(function (x) {
      x.classList.toggle('active', parseInt(x.dataset.dir, 10) === OSSM_DEFAULTS[DIR_REG]);
    });
    toast('OSSM standards loaded into the fields — review, then Program drive');
  });

  const rawBtn = $('#svRawBtn');
  if (rawBtn) rawBtn.addEventListener('click', function () {
    const regStr = ($('#svRawReg') || {}).value || '';
    const reg = parseInt(regStr.trim(), regStr.trim().toLowerCase().indexOf('0x') === 0 ? 16 : 10);
    const val = parseInt(($('#svRawVal') || {}).value, 10);
    if (isNaN(reg) || isNaN(val)) { toast('Raw write needs a register and a value'); return; }
    const changes = {}; changes[reg] = val;
    confirmProgram(changes, null, async function () {
      _await[reg] = val;
      const rsp = await post('/api/servo', { raw: { reg: reg, val: val } });
      const j = rsp ? await rsp.json().catch(function () { return null; }) : null;
      if (!j || j.ok === false) toast('Raw write refused' + (j && j.error ? ' — ' + j.error : ''));
      else { toast('Raw write sent — verifying…'); fastPoll(); }
    });
  });
}
