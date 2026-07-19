/**
 * link.js — WebSocket connection state machine.
 *
 * Manages the binary WS connection to ws://<host>:<port>/ws/ui,
 * HELLO handshake validation, reconnect with backoff, clock sync,
 * and automatic fallback to HTTP polling.
 *
 * Fallback rule: WS fails twice within 10s → HTTP polling active,
 * body.degraded class set, event emitted. WS retried in background
 * every 5s; on success, restore.
 */

import {
  FRAME_HELLO, FRAME_TELEMETRY, FRAME_STATUS, FRAME_CLOCK, FRAME_INTERP, FRAME_ANOMALY, FRAME_ECHO,
  parseHello, parseTelemetry, parseStatus, parseClockReply, parseInterp, parseAnomaly,
  buildClock, clockCalc, frameType
} from './wire.js';
import { setClockState, feedWireSamples } from './telebuf.js';
import { setSendBinary, setFallback, processEcho, noteCfgGen } from './cmd.js';

// ---- Configuration ---------------------------------------------------------
var WS_PORT = 81;
var WS_PATH = '/ws/ui';
var BACKOFF_INITIAL_MS = 500;
var BACKOFF_MAX_MS = 5000;
var WS_RETRY_INTERVAL_MS = 5000;
var CLOCK_SYNC_INTERVAL_MS = 2000;
var FALLBACK_FAIL_WINDOW_MS = 10000;
var HELLO_TIMEOUT_MS = 3000;
var CLOCK_EWMA_ALPHA = 0.1;
var RTT_MEDIAN_WINDOW = 15;
var MAX_JITTER_SAMPLES = 60;

// ---- State -----------------------------------------------------------------
var _ws = null;
var _host = null;            // set in initLink
var _reconnectScheduled = false;  // single-flight guard (F-006/A-005)

// Backoff / reconnect
var _backoffMs = BACKOFF_INITIAL_MS;
var _reconnectTimer = null;
var _helloTimeout = null;
var _receivedHello = false;

// Fallback
var _fallbackActive = false;
var _failCount = 0;
var _failWindowStart = 0;

// Clock sync
var _clockSyncTimer = null;
var _clockOffsetUs = 0;
var _rttSamples = [];
var _jitterSamples = [];
var _lastRtt = 0;
var _p95JitterUs = 500;
var _synced = false;

// Frame dispatch callbacks
var _onTelemetryCb = null;
var _onStatusCb = null;
var _onInterpCb = null;
var _onAnomalyCb = null;
var _onDegradedCb = null;
var _onRestoredCb = null;
var _onDisconnectedCb = null;

// Stats exposure (read by Health tab + connection-health dot / diag hazard shading)
var _stats = {
  wsRttUs: 0,
  clockOffsetUs: 0,
  p95JitterUs: 0,
  txDrops: 0,
  reconnectCount: 0,
  connected: false,
  fallback: false,
  droppedFrames: 0,   // cumulative telemetry frames lost (0x01 seq discontinuities)
  lastTeleMs: 0,      // performance.now() of the most recent 0x01 telemetry frame
  lastGapMs: 0,       // performance.now() of the most recent detected drop
  lastStatusMs: 0     // performance.now() of the most recent STATUS heartbeat
};

// Last 0x01 telemetry sequence number seen — for dropped-frame detection. -1 =
// no baseline yet (fresh connect / after a reset), so the first frame just seeds
// it without counting a phantom drop. :3
var _lastSeq = -1;

// ============================================================================
// Public API
// ============================================================================

export function getStats() { return _stats; }
export function isConnected() { return _stats.connected && !_fallbackActive; }
export function isFallback() { return _fallbackActive; }

export function onTelemetry(cb)  { _onTelemetryCb = cb; }
export function onStatus(cb)     { _onStatusCb = cb; }
export function onInterp(cb)     { _onInterpCb = cb; }
export function onAnomaly(cb)    { _onAnomalyCb = cb; }
export function onDegraded(cb)   { _onDegradedCb = cb; }
export function onRestored(cb)   { _onRestoredCb = cb; }
export function onDisconnected(cb){ _onDisconnectedCb = cb; }

/**
 * Initialize the link. Call once at boot from main.js.
 * @param {object} [opts]
 */
export function initLink(opts) {
  if (opts) {
    if (opts.host) _host = opts.host;
    if (opts.port) WS_PORT = opts.port;
    if (opts.path) WS_PATH = opts.path;
  }
  if (!_host) _host = (typeof location !== 'undefined') ? location.hostname : '192.168.4.1';

  _stats.reconnectCount = 0;
  _connect();
  _startClockSync();
}

/**
 * Send raw binary data over the WebSocket.
 * @param {Uint8Array} data
 * @returns {boolean}
 */
export function sendBinary(data) {
  if (_ws && _ws.readyState === WebSocket.OPEN) {
    try { _ws.send(data); return true; } catch(e) { return false; }
  }
  return false;
}

// ============================================================================
// Connect / reconnect
// ============================================================================

function _connect() {
  if (_ws) { try { _ws.close(); } catch(e) {} _ws = null; }
  _clearTimers();
  _receivedHello = false;
  _reconnectScheduled = false;    // reset on entry — _handleFail re-arms it

  var url = 'ws://' + _host + ':' + WS_PORT + WS_PATH;
  if (window.__DEBUG_LINK) console.log('[link] _connect →', url);
  try {
    _ws = new WebSocket(url);
    _ws.binaryType = 'arraybuffer';
    _ws.onopen    = _onOpen;
    _ws.onmessage = _onMessage;
    _ws.onclose   = _onClose;
    _ws.onerror   = _onError;
  } catch(e) {
    _handleFail();
  }
}

function _onOpen() {
  if (window.__DEBUG_LINK) console.log('[link] _onOpen — waiting for HELLO');
  _lastSeq = -1;                  // fresh socket → reseed seq baseline, don't count a phantom drop
  _reconnectScheduled = false;    // connection confirmed, reset guard (F-006)
  if (_helloTimeout) clearTimeout(_helloTimeout);
  _helloTimeout = setTimeout(function() {
    _reconnectScheduled = false;  // unblock retries on timeout (F-006)
    if (!_receivedHello) _handleFail(); // no HELLO in time
  }, HELLO_TIMEOUT_MS);
}

function _onMessage(event) {
  var data = event.data;
  if (!(data instanceof ArrayBuffer)) return;

  var dv = new DataView(data);
  var type = frameType(dv);

  switch (type) {
    case FRAME_HELLO:     _handleHello(dv); break;
    case FRAME_TELEMETRY: _handleTelemetry(dv, data.byteLength); break;
    case FRAME_STATUS:    _handleStatus(dv); break;
    case FRAME_CLOCK:     _handleClockReply(dv); break;
    case FRAME_INTERP:    _handleInterp(dv, data.byteLength); break;
    case FRAME_ANOMALY:   _handleAnomaly(dv, data.byteLength); break;
    case FRAME_ECHO:      processEcho(dv, data.byteLength); break;
  }
}

// ---- HELLO ----------------------------------------------------------------

function _handleHello(dv) {
  if (_receivedHello) return;
  _receivedHello = true;
  if (_helloTimeout) { clearTimeout(_helloTimeout); _helloTimeout = null; }

  var hello = parseHello(dv);
  if (!hello || hello.proto_ver !== 1) { _handleFail(); return; }

  // Connection confirmed
  _stats.connected = true;
  _stats.reconnectCount++;
  _failCount = 0;
  _failWindowStart = 0;
  _backoffMs = BACKOFF_INITIAL_MS;

  setSendBinary(function(data) { return sendBinary(data); });

  // Fire restored callback on EVERY successful connection, not just
  // fallback recovery. Without this, _wsLive stays false on clean boots,
  // pollStatus() keeps writing DOM, and header chips flicker at 0.5-2Hz.
  if (_onRestoredCb) _onRestoredCb();

  // If recovering from fallback, also clear the degraded state
  if (_fallbackActive) {
    _fallbackActive = false;
    setFallback(false);
    _stats.fallback = false;
    if (typeof document !== 'undefined') document.body.classList.remove('degraded');
  }

  noteCfgGen(hello.cfg_gen);
}

// ---- TELEMETRY ------------------------------------------------------------

function _handleTelemetry(dv, byteLength) {
  var t = parseTelemetry(dv, byteLength);
  if (!t) return;
  var nowMs = performance.now();

  // Dropped-frame detection via the 0x01 sequence counter. A jump of >1 means
  // one or more frames never arrived (poor connection). Guard the delta to a
  // sane window so a legitimate u16 wrap or a post-reconnect seq reset doesn't
  // register as thousands of phantom drops. :3
  _stats.lastTeleMs = nowMs;
  if (typeof t.seq === 'number') {
    if (_lastSeq >= 0) {
      var d = (t.seq - _lastSeq) & 0xFFFF;
      if (d > 1 && d < 1000) {
        _stats.droppedFrames += (d - 1);
        _stats.lastGapMs = nowMs;
      }
    }
    _lastSeq = t.seq;
  }

  feedWireSamples(t, nowMs);
  if (_onTelemetryCb) _onTelemetryCb(t);
}

// ---- STATUS ---------------------------------------------------------------

function _handleStatus(dv) {
  var s = parseStatus(dv);
  if (!s) return;
  // STATUS is the ~2Hz heartbeat that proves the link is alive even when the
  // machine is idle and emitting no 0x01 motion frames. The health dot uses its
  // age to tell "idle but connected" from "connection stalled". :3
  _stats.lastStatusMs = performance.now();
  noteCfgGen(s.cfg_gen);
  _stats.txDrops = s.tx_drops;
  _stats.clockOffsetUs = _clockOffsetUs;
  _stats.p95JitterUs = _p95JitterUs;
  _stats.wsRttUs = _lastRtt;
  if (_onStatusCb) _onStatusCb(s);
}

// ---- INTERP ---------------------------------------------------------------

function _handleInterp(dv, byteLength) {
  var it = parseInterp(dv, byteLength);
  if (!it) return;
  if (_onInterpCb) _onInterpCb(it);
}

// ---- ANOMALY --------------------------------------------------------------

function _handleAnomaly(dv, byteLength) {
  var a = parseAnomaly(dv, byteLength);
  if (!a) return;
  if (_onAnomalyCb) _onAnomalyCb(a);
}

// ---- CLOCK reply ----------------------------------------------------------

function _handleClockReply(dv) {
  var cr = parseClockReply(dv);
  if (!cr) return;
  var t3_us = Math.round(performance.now() * 1000) >>> 0;

  var calc = clockCalc(cr.t0_echo, cr.t1_dev_us, cr.t2_dev_us, t3_us);
  var offset_us = calc.offset_us;
  var rtt_us = calc.rtt_us;

  // Median filter for outlier rejection
  _rttSamples.push(rtt_us);
  if (_rttSamples.length > RTT_MEDIAN_WINDOW) _rttSamples.shift();

  var sorted = _rttSamples.slice().sort(function(a, b) { return a - b; });
  var medianRtt = sorted[Math.floor(sorted.length / 2)];

  if (rtt_us > 3 * medianRtt) return; // outlier discard

  // Jitter
  if (_lastRtt > 0) {
    var j = Math.abs(rtt_us - _lastRtt);
    _jitterSamples.push(j);
    if (_jitterSamples.length > MAX_JITTER_SAMPLES) _jitterSamples.shift();
  }
  _lastRtt = rtt_us;

  // EWMA offset
  if (!_synced) { _clockOffsetUs = offset_us; _synced = true; }
  else { _clockOffsetUs += CLOCK_EWMA_ALPHA * (offset_us - _clockOffsetUs); }

  // p95 jitter
  var js = _jitterSamples.slice().sort(function(a, b) { return a - b; });
  _p95JitterUs = js.length > 0 ? js[Math.min(Math.floor(js.length * 0.95), js.length - 1)] : 500;

  setClockState(_clockOffsetUs, _p95JitterUs, _synced);
}

// ---- Disconnect -----------------------------------------------------------

function _onClose(event) {
  _stats.connected = false;
  setSendBinary(null);
  if (_onDisconnectedCb) _onDisconnectedCb();
  _handleFail();
}

function _onError(event) {
  if (!_ws) return;  // _onClose already nulled and handled — prevent double-fire (F-006)
  _stats.connected = false;
  setSendBinary(null);
  if (_ws) { try { _ws.close(); } catch(e) {} _ws = null; }
  if (_onDisconnectedCb) _onDisconnectedCb();
  _handleFail();
}

function _handleFail() {
  if (_reconnectTimer) return;  // guard against duplicate scheduling (F-006)
  _failCount++;
  var now = performance.now();
  if (_failWindowStart === 0) _failWindowStart = now;

  if (now - _failWindowStart < FALLBACK_FAIL_WINDOW_MS) {
    if (_failCount >= 2 && !_fallbackActive) _activateFallback();
  } else {
    _failWindowStart = now;
    _failCount = 1;
  }

  _scheduleReconnect();
}

function _scheduleReconnect() {
  if (_reconnectTimer) clearTimeout(_reconnectTimer);
  if (_fallbackActive) {
    _reconnectTimer = setTimeout(_connect, WS_RETRY_INTERVAL_MS);
  } else {
    _reconnectTimer = setTimeout(_connect, _backoffMs);
    _backoffMs = Math.min(_backoffMs * 2, BACKOFF_MAX_MS);
  }
}

function _activateFallback() {
  _fallbackActive = true;
  setFallback(true);
  _stats.fallback = true;
  if (typeof document !== 'undefined') document.body.classList.add('degraded');
  if (_onDegradedCb) _onDegradedCb();
}

// ============================================================================
// Clock sync timer
// ============================================================================

function _startClockSync() {
  if (_clockSyncTimer) clearInterval(_clockSyncTimer);
  _clockSyncTimer = setInterval(_sendClockPing, CLOCK_SYNC_INTERVAL_MS);

  // Visibility fast-path for the device-side activity gate. A clock ping is the
  // inbound signal the device uses to decide which tabs to stream to. When this
  // tab is hidden we stop pinging, so it deterministically falls out of the
  // device's active window and gets muted (unless it's the last-active tab,
  // which the device keeps live regardless). On refocus we ping immediately, so
  // streaming resumes within one tick instead of waiting for the next interval.
  if (typeof document !== 'undefined' && document.addEventListener) {
    document.addEventListener('visibilitychange', function () {
      if (!document.hidden) _sendClockPing();
    });
  }
}

function _sendClockPing() {
  if (!_ws || _ws.readyState !== WebSocket.OPEN) return;
  // Skip while hidden — a backgrounded tab that isn't rendering doesn't need
  // clock sync, and its silence is what lets the device mute it (see above).
  if (typeof document !== 'undefined' && document.hidden) return;
  var t0_us = Math.round(performance.now() * 1000);
  sendBinary(buildClock(t0_us));
}

// ============================================================================
// Cleanup
// ============================================================================

function _clearTimers() {
  if (_reconnectTimer) { clearTimeout(_reconnectTimer); _reconnectTimer = null; }
  if (_helloTimeout) { clearTimeout(_helloTimeout); _helloTimeout = null; }
}

export function emergencyStop() {
  _clearTimers();
  if (_clockSyncTimer) { clearInterval(_clockSyncTimer); _clockSyncTimer = null; }
  if (_ws) { try { _ws.close(); } catch(e) {} _ws = null; }
  _stats.connected = false;
}