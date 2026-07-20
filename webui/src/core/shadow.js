/**
 * shadow.js — per-control desired/reported reconciliation.
 *
 * Every mutable control has a shadow record: {reported, desired, state, sentAt}.
 * Controls ALWAYS render from `reported` — the ground truth.
 * While `desired ≠ reported`, an intent overlay shows `desired` (`.pending` class).
 *
 * States: confirmed → pending → overdue1 (500ms) → overdue2 (2s) → fault (retries exhausted)
 * On ECHO: reported = echoed value. If echoed ≠ desired (clamp), `.settle` (300ms) then converge.
 * On get_cfg resync: overwrite ALL reported values, drop stale desires.
 *
 * Staleness machine (consumes telebuf/link events):
 *   >150ms no 0x01 → body.stale (hollow numerals, desaturation, glows off)
 *   >1s         → body.suspended (controls disabled, cmd.js blocked, banner)
 *   HTTP fallback: thresholds ×5 (750ms / 5s)
 *
 * Decision of record: never render unconfirmed values in reality styling.
 * Purple = intent/desired, blue = reality/reported — never mix.
 */

import { setVV, setVVState, toast } from './ui.js';
import * as cmd from './cmd.js';
import { OP_SET_WINDOW, OP_SET_SPEED, OP_SET_ACCEL, OP_GEN_CFG,
         OP_GEN_RUN, OP_MODE, OP_BLEND, OP_PAUSE, OP_HALT, OP_ESTOP,
         OP_HOME, OP_OVERRIDE, OP_BYPASS, OP_CLEAR_FAULT, OP_MOVE,
         OP_GET_CFG, OP_STREAM_MODE, OP_OVERSHOOT } from './wire.js';
import { isFallback } from './link.js';
import { settingsAuthoritative } from '../core/range.js';

// ============================================================================
// Shadow records — one per mutable control key
// ============================================================================

/**
 * Shadow state for a single control.
 * @typedef {{
 *   reported: *,        // the value the device confirms
 *   desired: *,         // the value the client sent (pending change)
 *   state: string,      // 'confirmed' | 'pending' | 'overdue1' | 'overdue2' | 'fault' | 'settle'
 *   sentAt: number,     // performance.now() when the last cmd was sent
 *   settleAt: number,   // time when settle animation should end
 *   id: number,         // most recent cmd id for this key
 *   elIds: string[],    // DOM element ids to apply class hooks to
 *   renderFn: function, // called to apply reported/desired to DOM
 * }}
 */
var _shadows = {};

// Per-key config: how to render reported vs desired, and which DOM ids get class hooks
var _keyConfig = {
  'window': {
    elIds: ['trackHost', 'spineRailHost'],
    render: function(sh) {
      // sh.reported = {min, max}, sh.desired = {min, max}
      // The band in rail.js reads winMin/winMax directly from range.js.
      // When shadow is pending, we set data attributes on the rail host
      // so rail.js can read intended values for the band display.
      var host = document.getElementById('spineRailHost');
      if (!host) return;
      if (sh.desired) {
        host.setAttribute('data-d-min', sh.desired.min != null ? sh.desired.min : '');
        host.setAttribute('data-d-max', sh.desired.max != null ? sh.desired.max : '');
      } else {
        host.removeAttribute('data-d-min');
        host.removeAttribute('data-d-max');
      }
      _applyElState(sh, sh.elIds);
    }
  },
  'speed': {
    elIds: ['maxSpeed', 'speedVal'],
    render: function(sh) {
      var el = document.getElementById('maxSpeed');
      if (!el) return;
      if (sh.desired && sh.desired.mm_s != null && sh.desired.mm_s !== sh.reported.mm_s) {
        // Show desired as purple intent overlay on the slider readout
        setVV('speedVal', sh.desired.mm_s, 4, 0);
        var readout = document.getElementById('speedVal');
        if (readout) readout.classList.add('intent');
      } else {
        setVV('speedVal', sh.reported.mm_s || 0, 4, 0);
        var readout = document.getElementById('speedVal');
        if (readout) readout.classList.remove('intent');
      }
      _applyElState(sh, sh.elIds);
    }
  },
  'accel': {
    elIds: ['accel', 'accelVal'],
    render: function(sh) {
      var el = document.getElementById('accel');
      if (!el) return;
      if (sh.desired && sh.desired.mm_s2 != null && sh.desired.mm_s2 !== sh.reported.mm_s2) {
        setVV('accelVal', sh.desired.mm_s2, 5, 0);
        var readout = document.getElementById('accelVal');
        if (readout) readout.classList.add('intent');
      } else {
        setVV('accelVal', sh.reported.mm_s2 || 0, 5, 0);
        var readout = document.getElementById('accelVal');
        if (readout) readout.classList.remove('intent');
      }
      _applyElState(sh, sh.elIds);
    }
  },
  'blend': {
    elIds: ['blendModeSeg'],
    render: function(sh) {
      _applyElState(sh, sh.elIds);
    }
  },
  // Stream speed-feed + overshoot clamp segmented controls (settings.js
  // registers external renderers that reflect the confirmed value). :3
  'stream_mode': {
    elIds: ['streamModeSeg'],
    render: function(sh) { _applyElState(sh, sh.elIds); }
  },
  'overshoot': {
    elIds: ['overshootSeg'],
    render: function(sh) { _applyElState(sh, sh.elIds); }
  },
};

// External renderers — feature modules (settings.js) register a callback per
// key so device-confirmed values drive their own reflect functions (the fix
// for controls whose visible selection was only ever set locally on click). :3
var _externalRenderers = {};

export function registerRenderer(key, fn) {
  _externalRenderers[key] = fn;
  var sh = _shadows[key];
  if (sh) _render(sh);   // reflect current truth immediately on registration
}

// Map op codes → shadow key (for cmd.onEcho dispatch)
var _opToKey = {};
_opToKey[OP_SET_WINDOW] = 'window';
_opToKey[OP_SET_SPEED]  = 'speed';
_opToKey[OP_SET_ACCEL]  = 'accel';
_opToKey[OP_BLEND]      = 'blend';
_opToKey[OP_GEN_CFG]    = 'gen_cfg';
_opToKey[OP_GEN_RUN]    = 'gen_run';
_opToKey[OP_MODE]       = 'mode';
_opToKey[OP_PAUSE]      = 'pause';
_opToKey[OP_OVERRIDE]   = 'override';
_opToKey[OP_BYPASS]     = 'bypass';
_opToKey[OP_MOVE]       = 'move';
_opToKey[OP_STREAM_MODE] = 'stream_mode';
_opToKey[OP_OVERSHOOT]   = 'overshoot';

// Timing thresholds
var OVERDUE1_MS = 500;
var OVERDUE2_MS = 2000;
var SETTLE_MS   = 300;

// ---- Public API ------------------------------------------------------------

/**
 * Register a shadow record for a control key.
 * @param {string} key — 'window' | 'speed' | 'accel' | 'blend' | ...
 * @param {*} initialReported — the ground-truth value from the device
 */
export function register(key, initialReported) {
  // Use keyConfig if available, else generic
  var cfg = _keyConfig[key] || { elIds: [], render: null };
  _shadows[key] = {
    key: key,
    reported: initialReported,
    desired: null,
    state: 'confirmed',
    sentAt: 0,
    settleAt: 0,
    id: 0,
    elIds: cfg.elIds || [],
    renderFn: cfg.render || null,
  };
}

/**
 * Notify shadow that a command was sent for a key.
 * Called by cmd.send() wrappers or directly after cmd.send().
 * @param {string} key
 * @param {*} desired — the value that was sent
 * @param {number} cmdid — cmd id for matching echo
 */
export function noteSent(key, desired, cmdid) {
  var sh = _shadows[key];
  if (!sh) {
    // Auto-register with null reported (keyConfig render/elIds if defined)
    var kc = _keyConfig[key] || { elIds: [], render: null };
    sh = { key: key, reported: null, desired: null, state: 'confirmed', sentAt: 0, settleAt: 0, id: 0, elIds: kc.elIds || [], renderFn: kc.render || null };
    _shadows[key] = sh;
  }

  sh.desired = _cloneDeep(desired);
  sh.sentAt = performance.now();
  sh.id = cmdid;
  _transition(sh, 'pending');
}

/**
 * Process an echo result from cmd.onEcho.
 * @param {{id:number, ok:number, op:number, reported:object, cfg_gen:number}} ev
 */
export function processEcho(ev) {
  const key = _opToKey[ev.op];
  if (!key) return;

  let sh = _shadows[key];
  if (!sh) return;

  // Update reported to the device's post-clamp ground truth
  sh.reported = ev.ok ? _cloneDeep(ev.reported) : sh.reported;

  // Normalize window echo: firmware echoes full settings object with range_min/max,
  // but the window shadow + render expect {min, max}. Extract just the bounds.
  if (key === 'window' && ev.ok && ev.reported && typeof ev.reported.range_min === 'number') {
    sh.reported = { min: ev.reported.range_min, max: ev.reported.range_max };
    // Normalize desired too so the comparison matches
    if (sh.desired && typeof sh.desired.range_min === 'number') {
      sh.desired = { min: sh.desired.range_min, max: sh.desired.range_max };
    }
  }

  // If the device clamped our desired value, flag .settle
  if (_isDifferent(sh.desired, sh.reported)) {
    sh.settleAt = performance.now() + SETTLE_MS;
    _transition(sh, 'settle');
  } else {
    sh.desired = null;
    _transition(sh, 'confirmed');
  }

  _render(sh);
}

export function processConfig(cfg) {
  if (!cfg) return;

  // GOLDEN RULE: only apply device-authored window bounds when they are REAL.
  // A cfg snapshot that arrives before the machine has homed/populated its
  // window can carry 0/0 or garbage; blindly stamping that over the correct
  // HTTP-loaded window is exactly the "wrong window on boot" defect. We reject
  // non-finite or degenerate (min>=max) bounds, and we never clobber a window
  // the operator is actively dragging (pending shadow). :3
  var _wMin = cfg.range_min, _wMax = cfg.range_max;
  var _wValid = (typeof _wMin === 'number' && typeof _wMax === 'number' &&
                 isFinite(_wMin) && isFinite(_wMax) && _wMax > _wMin);
  var _wState = getState('window');
  var _wPending = (_wState === 'pending' || _wState === 'overdue1' || _wState === 'overdue2');
  if (_wValid && !_wPending) {
    // Thing 3 guard: NEVER apply a WS config-push window before the
    // authoritative HTTP /api/settings pull completes. The WS cmd path
    // (WS_OP_GET_CFG echo or config push) can fire before loadSettings()
    // returns and carry stale/wrong bounds from before the machine was homed.
    // Once settingsAuthoritative is true we know loadSettings() has seeded
    // winMin/winMax correctly and we can safely accept config pushes. :3
    if (settingsAuthoritative) {
      _applyConfigField('window', { min: _wMin, max: _wMax });
      // Push device-authored window values into the DOM band (P5 cold path), but
      // only once range.js actually knows the measured travel — renderWindow()
      // bails on TRAVEL<=0 and would flip windowReady back off otherwise. :3
      (async () => {
        const rangeModule = await import('./range.js');
        if (rangeModule.TRAVEL > 0 && rangeModule.setWinMin && rangeModule.setWinMax && rangeModule.renderWindow) {
          rangeModule.setWinMin(_wMin);
          rangeModule.setWinMax(_wMax);
          rangeModule.renderWindow();
        }
      })();
    }
  }
  _applyConfigField('speed',    { mm_s: cfg.max_speed });
  _applyConfigField('accel',    { mm_s2: cfg.accel });
  _applyConfigField('blend',    { bm: cfg.blend_mode });
  _applyConfigField('mode',     { transport: cfg.mode });
  _applyConfigField('override', { on: cfg.manual_override || false });
  _applyConfigField('bypass',   { on: cfg.bypass_limits || false });
  // Stream speed-feed + overshoot clamp — device-authored pushes drive these
  // controls too (previously they only ever reflected local click state). :3
  if (typeof cfg.stream_speed_mode === 'number')
    _applyConfigField('stream_mode', { mode: cfg.stream_speed_mode });
  if (typeof cfg.overshoot_clamp !== 'undefined')
    _applyConfigField('overshoot', { on: !!cfg.overshoot_clamp });
}

function _applyConfigField(key, reported) {
  const sh = _shadows[key];
  if (!sh) {
    var kc = _keyConfig[key] || { elIds: [], render: null };
    _shadows[key] = { key: key, reported: _cloneDeep(reported), desired: null, state: 'confirmed', sentAt: 0, settleAt: 0, id: 0, elIds: kc.elIds || [], renderFn: kc.render || null };
    _render(_shadows[key]);
    return;
  }
  // Guard: if this shadow has a pending desired value, don't overwrite it (A-009)
  if (sh.state === 'pending' || sh.state === 'overdue1' || sh.state === 'overdue2') return;
  sh.reported = _cloneDeep(reported);
  sh.desired = null; // cfg_gen resync drops all pending desires
  sh.state = 'confirmed';
  _render(sh);
}

/**
 * Called every frame to update overdue timers and settle transitions.
 */
export function tick(nowMs) {
  for (var key in _shadows) {
    if (!_shadows.hasOwnProperty(key)) continue;
    var sh = _shadows[key];

    // Settle resolution
    if (sh.state === 'settle' && nowMs >= sh.settleAt) {
      // After .settle, converge to reported
      sh.desired = null;
      _transition(sh, 'confirmed');
      _render(sh);
    }

    // Overdue escalation
    if (sh.state === 'pending' && sh.sentAt > 0) {
      var elapsed = nowMs - sh.sentAt;
      if (elapsed >= OVERDUE2_MS && sh.desired != null) {
        _transition(sh, 'overdue2');
        _render(sh);
        _toastOverdue(key, sh.desired);
      } else if (elapsed >= OVERDUE1_MS) {
        _transition(sh, 'overdue1');
        _render(sh);
      }
    }
  }
}

/**
 * Get the current shadow state for a key (for external inspection).
 */
export function getState(key) {
  var sh = _shadows[key];
  return sh ? sh.state : 'confirmed';
}

/**
 * Get the reported value for a key.
 */
export function getReported(key) {
  var sh = _shadows[key];
  return sh ? sh.reported : null;
}

// ---- Internal transitions --------------------------------------------------

function _transition(sh, newState) {
  if (sh.state === 'fault') return; // fault is terminal until next send
  sh.state = newState;
}

function _render(sh) {
  // Apply state classes to DOM elements
  _applyElState(sh, sh.elIds);

  // Call custom render function
  if (sh.renderFn) sh.renderFn(sh);

  // Call any externally-registered renderer (feature-module reflect hooks)
  var ext = sh.key ? _externalRenderers[sh.key] : null;
  if (ext) ext(sh);
}

/**
 * Apply shadow state CSS classes to DOM elements.
 *   pending/overdue1/overdue2 → .pending
 *   overdue1/overdue2 → .w1/.w2 on the pending indicator
 *   settle → .settle
 *   confirmed → remove all shadow classes
 */
function _applyElState(sh, elIds) {
  var cls = sh.state;
  for (var i = 0; i < elIds.length; i++) {
    var el = document.getElementById(elIds[i]);
    if (!el) continue;

    // Remove all shadow state classes first
    el.classList.remove('pending', 'overdue1', 'overdue2', 'settle', 'fault');

    if (cls === 'confirmed') continue;

    // Apply the base pending class for all non-confirmed states
    el.classList.add('pending');

    // Layer on the specific escalation class
    if (cls === 'overdue1') el.classList.add('overdue1');
    else if (cls === 'overdue2') el.classList.add('overdue2');
    else if (cls === 'settle') el.classList.add('settle');
    else if (cls === 'fault') el.classList.add('fault');
  }
}

function _toastOverdue(key, desired) {
  // Only toast once per overdue2 transition (state tracking handles dedup)
  var label = key;
  if (key === 'window') label = 'Window';
  else if (key === 'speed') label = 'Speed';
  else if (key === 'accel') label = 'Accel';
  else if (key === 'blend') label = 'Blend';
  toast(label + ' echo overdue — device may be unresponsive', 'warn', 'i-alert', 3000);
}

function _cloneDeep(obj) {
  if (obj === null || typeof obj !== 'object') return obj;
  try { return JSON.parse(JSON.stringify(obj)); } catch(e) { return obj; }
}

function _isDifferent(a, b) {
  if (a === b) return false;
  if (a == null || b == null) return a !== b;
  if (typeof a !== 'object' || typeof b !== 'object') return a !== b;
  // Shallow compare for our use case (flat objects)
  var ka = Object.keys(a), kb = Object.keys(b);
  if (ka.length !== kb.length) return true;
  for (var i = 0; i < ka.length; i++) {
    if (a[ka[i]] !== b[ka[i]]) return true;
  }
  return false;
}

/**
 * Hook cmd.send to auto-track shadow state.
 * Call this ONCE in main.js after shadow module is initialized.
 * Wraps cmd.send so every sent command is auto-registered in shadow.
 */
var _origCmdSend = null;

export function wireCmdShadow() {
  if (_origCmdSend) return; // already wired
  _origCmdSend = cmd.send;

  cmd.send = function(op, payload) {
    // Parse the desired value
    var desired = (typeof payload === 'string')
      ? (function() { try { return JSON.parse(payload); } catch(e) { return null; } })()
      : payload;

    var key = _opToKey[op];

    // Call original send first
    var result = _origCmdSend(op, payload);

    // Track in shadow if we have a key for this op
    if (key && result >= 0) {
      noteSent(key, desired, result);
    }

    return result;
  };
}

// ============================================================================
// Staleness escalation machine
// ============================================================================

// Thresholds (HTTP fallback ×5)
var STALE_THRESHOLD_MS      = 150;
var SUSPENDED_THRESHOLD_MS  = 1000;
var STALE_THRESHOLD_FB_MS   = 750;
var SUSPENDED_THRESHOLD_FB_MS = 5000;

var _staleActive = false;
var _suspendedActive = false;
var _lastRecoveryMs = 0;

/**
 * Wire staleness escalation.
 * Call once from main.js after telebuf/link are initialized.
 * Takes refs to callback setters so we can intercept/filter.
 * @param {object} deps — {onStale, onStaleSuspended} from telebuf
 */
export function wireStaleness(teleBufModule) {
  // teleBufModule is optional — the staleness observer only needs the DOM.
  // Hook the existing stale callbacks (set by main.js) to add our own processing
  // We use the existing body.stale/body.suspended classes set by main.js
  // and add the hollow numeral / desaturation / suspension logic here.

  // Listen for staleness transition via the DOM class observer
  _startStalenessObserver();
}

var _observer = null;

function _startStalenessObserver() {
  if (_observer) return;
  if (typeof MutationObserver === 'undefined') return;

  _observer = new MutationObserver(function(mutations) {
    for (var i = 0; i < mutations.length; i++) {
      var m = mutations[i];
      if (m.type !== 'attributes' || m.attributeName !== 'class') continue;
      var body = document.body;

      var isStale = body.classList.contains('stale');
      var isSuspended = body.classList.contains('suspended');
      var isDegraded = body.classList.contains('degraded');
      var inFallback = isFallback();

      if (isStale && !_staleActive) {
        _staleActive = true;
        _applyStaleVisuals(true, inFallback);
      } else if (!isStale && _staleActive) {
        _staleActive = false;
        _lastRecoveryMs = performance.now();
        _applyStaleVisuals(false, false);
      }

      if (isSuspended && !_suspendedActive) {
        _suspendedActive = true;
        _applySuspendedVisuals(true, inFallback);
      } else if (!isSuspended && _suspendedActive) {
        _suspendedActive = false;
        _applySuspendedVisuals(false, false);
      }
    }
  });

  _observer.observe(document.body, { attributes: true, attributeFilter: ['class'] });
}

// ---- Visual application for staleness ---------------------------------------

function _applyStaleVisuals(on, fallback) {
  // The CSS rules in style.css handle most of this via body.stale selectors.
  // We add operational behaviors here that CSS alone can't express.

  if (on) {
    // Stop the rail's phosphor trail — the rail reads telebuf which auto-holds
    // when stale. No additional action needed here.

    // Show stale chip in header
    _ensureStaleChip(true, fallback ? 'w2' : 'w1');
  } else {
    // Remove stale chip
    _ensureStaleChip(false, '');
  }
}

function _applySuspendedVisuals(on, fallback) {
  if (on) {
    // Show suspended banner in banner host
    _ensureBanner('suspendedBanner', 'Connection lost — controls suspended',
                  'warn', 'i-alert');

    // Update stale chip to show severity
    _ensureStaleChip(true, fallback ? 'bad' : 'w2');

    // All controls get disabled-look via body.suspended CSS
    // cmd.js already blocks sends via __CMD_SUSPENDED

    // Toast
    toast('Device unreachable — controls suspended', 'bad', 'i-alert', 5000);
  } else {
    _ensureBanner('suspendedBanner', '', '', '');
    // Re-enable is handled by main.js clearing __CMD_SUSPENDED
    if (_staleActive) {
      _ensureStaleChip(true, isFallback() ? 'w2' : 'w1');
    } else {
      _ensureStaleChip(false, '');
    }
  }
}

// ---- Helper chip/banner management -----------------------------------------

function _ensureStaleChip(show, level) {
  var sc = document.getElementById('staleChip');
  if (show && !sc) {
    sc = document.createElement('span');
    sc.id = 'staleChip';
    sc.className = 'chip';
    sc.innerHTML = '<span class="dot"></span><span id="staleChipText">Stale data</span>';
    var chips = document.querySelector('.chips');
    if (chips) chips.appendChild(sc);
  }
  if (!sc) return;

  if (show) {
    sc.style.display = '';
    if (level) {
      sc.className = 'chip ' + level;
      var dot = sc.querySelector('.dot');
      if (dot) dot.className = 'dot ' + (level === 'bad' ? 'bad' : 'warn');
    }
  } else {
    sc.style.display = 'none';
  }
}

function _ensureBanner(id, text, level, iconName) {
  var host = document.getElementById('bannerHost');
  if (!host) return;

  var banner = document.getElementById(id);
  if (!text && banner) {
    banner.remove();
    return;
  }
  if (!text) return;

  if (!banner) {
    banner = document.createElement('div');
    banner.id = id;
    banner.className = 'banner ' + (level || 'warn');
    host.appendChild(banner);
  }

  banner.className = 'banner ' + (level || 'warn');
  banner.textContent = text;
}

// ============================================================================
// Reconnect recovery animation
// ============================================================================

/**
 * Called when WS reconnects and normal data flow resumes.
 * Animate back from stale → nomal within 300ms.
 */
export function notifyRecovery() {
  if (_staleActive || _suspendedActive) {
    // body.stale/body.suspended are removed by main.js callbacks.
    // We add a recovery class for a 300ms transition.
    document.body.classList.add('recovering');
    setTimeout(function() {
      document.body.classList.remove('recovering');
    }, 350);
  }
}

// ============================================================================
// Initialize shadow records from config + wire to cmd
// ============================================================================

/**
 * Initialize shadow module. Call once from main.js after link/cmd/telebuf are up.
 * @param {object} initialConfig — settings from /api/settings (GET)
 */
export function initShadow(initialConfig) {
  // Register all shadow keys with initial reported values
  if (initialConfig) processConfig(initialConfig);

  // Wire cmd.send tracking
  wireCmdShadow();

  // The staleness observer auto-starts on first body class change
  // which happens from main.js's onStale/onStaleSuspended callbacks.
}