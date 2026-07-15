/**
 * cmd.js — control plane.
 *
 * Monotonic id, in-flight map, resend at 300ms up to 3 attempts,
 * coalescing (at most ONE in-flight per key), 0x11 ECHO dispatch,
 * cfg_gen watch, and transparent HTTP fallback mapping.
 *
 * In WS mode, commands go through link.sendBinary() as 0x10 CMD frames.
 * In HTTP fallback mode, cmd.send() maps ops to existing /api/* POSTs.
 */

import {
  FRAME_CMD, FRAME_ECHO,
  OP_SET_WINDOW, OP_SET_SPEED, OP_SET_ACCEL,
  OP_GEN_CFG, OP_GEN_RUN, OP_MODE, OP_BLEND,
  OP_PAUSE, OP_HALT, OP_ESTOP, OP_HOME,
  OP_OVERRIDE, OP_BYPASS, OP_CLEAR_FAULT, OP_SAVE,
  OP_GET_CFG, OP_MOVE,
  buildCmd, parseEcho
} from './wire.js';
import { post } from './api.js';

// ---- State -----------------------------------------------------------------
var _nextId = 1;          // monotonic command id (wraps at 65535)
var _inflight = {};       // {id -> {op, desired, sentAt, attempts, coalesceKey}}
var _sendBinary = null;   // function to send raw Uint8Array (set by link.js)
var _fallback = false;    // true when in HTTP fallback mode

// ---- Echo event dispatch ---------------------------------------------------
var _onEcho = null;       // called with {id, ok, op, reported, cfg_gen}
var _onConfig = null;     // called with full config object from get_cfg echo
var _onFault = null;      // called when a command exhausts retries

// ---- Known cfg_gen (updated by 0x02 and ECHO) ------------------------------
var _cfgGen = 0;

// ---- Coalescing key map ----------------------------------------------------
// Map logical key → op codes that share coalescing
var _coalesceKeys = {
  'window':    [OP_SET_WINDOW],
  'speed':     [OP_SET_SPEED],
  'accel':     [OP_SET_ACCEL],
  'gen_cfg':   [OP_GEN_CFG],
  'gen_run':   [OP_GEN_RUN],
  'mode':      [OP_MODE],
  'blend':     [OP_BLEND],
  'pause':     [OP_PAUSE],
  'halt':      [OP_HALT],
  'estop':     [OP_ESTOP],
  'home':      [OP_HOME],
  'override':  [OP_OVERRIDE],
  'bypass':    [OP_BYPASS],
  'move':      [OP_MOVE],
};

// Build reverse lookup: op → coalesceKey
var _opToKey = {};
for (var k in _coalesceKeys) {
  var ops = _coalesceKeys[k];
  for (var i = 0; i < ops.length; i++) {
    _opToKey[ops[i]] = k;
  }
}

// ---- Retry config ----------------------------------------------------------
var RESEND_MS = 300;
var MAX_ATTEMPTS = 3;

/**
 * Set the binary send function (called by link.js when WS is connected).
 * @param {function(Uint8Array):boolean} fn — returns true if send succeeded
 */
export function setSendBinary(fn) { _sendBinary = fn; }

/**
 * Set fallback mode. When true, cmd.send() transparently maps to HTTP POSTs.
 * @param {boolean} f
 */
export function setFallback(f) { _fallback = f; }

/**
 * Set the echo callback.
 * @param {function({id:number, ok:number, op:number, reported:object, cfg_gen:number})} cb
 */
export function onEcho(cb) { _onEcho = cb; }

/**
 * Set the full-config callback (called on get_cfg response or cfg_gen bump).
 * @param {function(object)} cb
 */
export function onConfig(cb) { _onConfig = cb; }

/**
 * Set the comms-fault callback (called when a command exhausts retries).
 * @param {function({id:number, op:number, desired:object})} cb
 */
export function onFault(cb) { _onFault = cb; }

// ---- cfg_gen watch ---------------------------------------------------------

/**
 * Notify cmd.js of a new cfg_gen seen in 0x02 or ECHO.
 * If newer than our known value, triggers a get_cfg request.
 * @param {number} gen
 * @returns {boolean} true if we sent get_cfg
 */
export function noteCfgGen(gen) {
  if (gen > _cfgGen) {
    _cfgGen = gen;
    _sendGetCfg();
    return true;
  }
  // Update even if equal (from echo with same gen after apply)
  _cfgGen = Math.max(_cfgGen, gen);
  return false;
}

function _sendGetCfg() {
  if (_fallback) {
    // HTTP fallback: GET /api/settings triggers config load
    (async function() {
      try {
        var r = await fetch('/api/settings');
        if (!r) return;
        var d = await r.json();
        if (d && _onConfig) _onConfig(d);
      } catch(e) {}
    })();
  } else if (_sendBinary) {
    var id = _nextId++;
    var json = '{}';
    var frame = buildCmd(id, OP_GET_CFG, json);
    _inflight[id] = { op: OP_GET_CFG, desired: null, sentAt: performance.now(), attempts: 1, coalesceKey: null };
    _sendBinary(frame);
    _scheduleRetry(id);
  }
}

// ---- In-flight management --------------------------------------------------

/**
 * Coalesce: cancel any in-flight commands for the same key.
 * Only the NEWEST desired value for each key is kept.
 * @param {number} op
 */
function _coalesce(op) {
  var key = _opToKey[op];
  if (!key) return; // no coalescing for this op
  for (var id in _inflight) {
    if (!_inflight.hasOwnProperty(id)) continue;
    var entry = _inflight[id];
    if (entry.coalesceKey === key) {
      // Cancel this older in-flight entry
      if (entry._timer) clearTimeout(entry._timer);
      delete _inflight[id];
    }
  }
}

/**
 * Send a command.
 * @param {number} op — op code (OP_SET_WINDOW, etc.)
 * @param {string|object} payload — if string, used as raw JSON; if object, serialized
 * @returns {number} the command id (0 if queued, negative if rejected)
 */
export function send(op, payload) {
  // Check suspension
  if (typeof window !== 'undefined' && window.__CMD_SUSPENDED) return -1;

  var jsonStr = (typeof payload === 'string') ? payload : JSON.stringify(payload || {});
  var desired = (typeof payload === 'string') ? (function() { try { return JSON.parse(jsonStr); } catch(e) { return null; } })() : payload;

  // Coalesce before sending
  _coalesce(op);

  var id = _nextId++;
  var key = _opToKey[op] || null;

  if (_fallback) {
    // HTTP fallback: map op to POST endpoint
    _fallbackSend(op, desired, id);
    return id;
  }

  if (!_sendBinary) {
    // No WS transport available — queue for retry via HTTP fallback
    _fallbackSend(op, desired, id);
    return id;
  }

  var frame = buildCmd(id, op, jsonStr);
  var sentAt = performance.now();
  _inflight[id] = {
    op: op,
    desired: desired,
    sentAt: sentAt,
    attempts: 1,
    coalesceKey: key
  };

  var ok = _sendBinary(frame);
  if (ok) {
    _scheduleRetry(id);
  } else {
    delete _inflight[id];
    if (_onFault) _onFault({ id: id, op: op, desired: desired });
  }

  return id;
}

function _scheduleRetry(id) {
  var entry = _inflight[id];
  if (!entry) return;
  if (entry._timer) clearTimeout(entry._timer);
  entry._timer = setTimeout(function() {
    _retry(id);
  }, RESEND_MS);
}

function _retry(id) {
  var entry = _inflight[id];
  if (!entry) return;

  if (entry.attempts >= MAX_ATTEMPTS) {
    // Exhausted — fire fault event
    if (_onFault) _onFault({ id: id, op: entry.op, desired: entry.desired });
    delete _inflight[id];
    return;
  }

  // Try HTTP fallback on retry exhaustion or if WS is gone
  if (_fallback || !_sendBinary) {
    _fallbackSend(entry.op, entry.desired, id);
    delete _inflight[id];
    return;
  }

  var jsonStr = JSON.stringify(entry.desired || {});
  var frame = buildCmd(id, entry.op, jsonStr);
  entry.attempts++;
  entry.sentAt = performance.now();
  var ok = _sendBinary(frame);
  if (ok) {
    _scheduleRetry(id);
  } else {
    // Send failed, but retry cycle continues
    if (entry.attempts < MAX_ATTEMPTS) {
      _scheduleRetry(id);
    } else {
      if (_onFault) _onFault({ id: id, op: entry.op, desired: entry.desired });
      delete _inflight[id];
    }
  }
}

// ---- HTTP fallback mapping -------------------------------------------------

var _FALLBACK_ROUTES = {};
_FALLBACK_ROUTES[OP_SET_WINDOW]  = { url: '/api/settings', body: function(d) { return { range_min: d.min != null ? d.min : d.range_min, range_max: d.max != null ? d.max : d.range_max, no_persist: d.no_persist != null ? d.no_persist : false }; } };
_FALLBACK_ROUTES[OP_SET_SPEED]   = { url: '/api/settings', body: function(d) { return { max_speed: d.mm_s != null ? d.mm_s : d.max_speed, no_persist: true }; } };
_FALLBACK_ROUTES[OP_SET_ACCEL]   = { url: '/api/settings', body: function(d) { return { accel: d.mm_s2 != null ? d.mm_s2 : d.accel, no_persist: true }; } };
_FALLBACK_ROUTES[OP_GEN_CFG]     = { url: '/api/pattern', body: function(d) { return d; } };
_FALLBACK_ROUTES[OP_GEN_RUN]     = { url: '/api/pattern', body: function(d) { return { running: d.run || false }; } };
_FALLBACK_ROUTES[OP_MODE]        = { url: '/api/mode', body: function(d) { return { mode: d.transport || d.mode }; } };
_FALLBACK_ROUTES[OP_BLEND]       = { url: '/api/settings', body: function(d) { return { blend_mode: d.bm != null ? d.bm : d.blend_mode, no_persist: true }; } };
_FALLBACK_ROUTES[OP_PAUSE]       = { url: '/api/pause', body: function(d) { return { paused: d.paused != null ? d.paused : true }; } };
_FALLBACK_ROUTES[OP_HALT]        = { url: '/api/halt', body: function() { return {}; } };
_FALLBACK_ROUTES[OP_ESTOP]       = { url: '/api/stop', body: function() { return {}; } };
_FALLBACK_ROUTES[OP_HOME]        = { url: '/api/home', body: function() { return {}; } };
_FALLBACK_ROUTES[OP_OVERRIDE]    = { url: '/api/override', body: function(d) { return { override: d.on != null ? d.on : true }; } };
_FALLBACK_ROUTES[OP_BYPASS]      = { url: '/api/override', body: function(d) { return { bypass_limits: d.on != null ? d.on : true }; } };
_FALLBACK_ROUTES[OP_CLEAR_FAULT] = { url: '/api/clearfault', body: function() { return {}; } };
_FALLBACK_ROUTES[OP_SAVE]        = { url: '/api/settings', body: function() { return { no_persist: false }; } };
_FALLBACK_ROUTES[OP_GET_CFG]     = { url: '/api/settings', body: function() { return {}; }, isGet: true };
_FALLBACK_ROUTES[OP_MOVE]        = { url: '/api/move', body: function(d) { return { position: d.position, stream: d.stream != null ? d.stream : false, bypass_limits: d.bypass_limits || false }; } };

function _fallbackSend(op, desired, id) {
  var route = _FALLBACK_ROUTES[op];
  if (!route) return;

  var body = route.body(desired);
  if (route.isGet) {
    // GET request
    (async function() {
      try {
        var r = await fetch(route.url);
        if (r) {
          var json = await r.json();
          if (_onEcho) _onEcho({ id: id, ok: 1, op: op, reported: json, cfg_gen: _cfgGen });
          if (op === OP_GET_CFG && _onConfig) _onConfig(json);
        }
      } catch(e) {}
    })();
  } else {
    post(route.url, body);
  }
}

// ---- ECHO processing -------------------------------------------------------

/**
 * Process an incoming 0x11 ECHO frame.
 * @param {DataView} dv
 * @param {number} byteLength
 */
export function processEcho(dv, byteLength) {
  var echo = parseEcho(dv, byteLength);
  if (!echo) return;

  // Update cfg_gen if newer
  if (echo.cfg_gen > _cfgGen) _cfgGen = echo.cfg_gen;

  // If this is a get_cfg response, fire full config event
  var entry = _inflight[echo.id];
  if (entry && entry.op === OP_GET_CFG && echo.payload) {
    if (_onConfig) _onConfig(echo.payload);
  }

  // Fire echo event
  if (_onEcho) {
    _onEcho({
      id: echo.id,
      ok: echo.ok,
      op: entry ? entry.op : 0,
      reported: echo.payload,
      cfg_gen: echo.cfg_gen
    });
  }

  // Clear the in-flight entry
  if (entry) {
    if (entry._timer) clearTimeout(entry._timer);
    delete _inflight[echo.id];
  }
}

// ---- Diagnostic ------------------------------------------------------------

export function getInflightCount() {
  var count = 0;
  for (var k in _inflight) { if (_inflight.hasOwnProperty(k)) count++; }
  return count;
}

export function getCfgGen() { return _cfgGen; }

// ---- Retry timer processor (called from main loop) -------------------------
var _lastRetryCheck = 0;

/**
 * Check for in-flight commands that need retrying. Call from main loop.
 * Each entry handles its own timer via _scheduleRetry, so this is a
 * no-op in steady state — the scheduled timers fire independently.
 */
export function tick() {
  // Individual retries are scheduled via setTimeout(_retry, RESEND_MS) per entry.
  // This function exists as a hook for future batch processing if needed.
}