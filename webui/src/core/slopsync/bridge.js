/**
 * bridge.js — dual-plane SlopSync ⇆ legacy WebUI integration (Phase A + B).
 *
 * ADDITIVE by design (roadmap §5.3): this owns a SlopSync session to
 * ws://<host>:82 and, while that session is live-and-granted, drives the SAME
 * existing renderers the legacy :81 UiSocket plane feeds (telebuf, shadow,
 * main.js flag/stats appliers). It deletes nothing — when the session is down,
 * `isSlopSyncLive()` is false and the legacy plane resumes ownership untouched.
 *
 * PRECEDENCE (single source of truth per surface, no double-feed flicker):
 *   - motion telemetry → telebuf via feedExternalSample(); legacy feedWireSamples
 *     is suppressed by telebuf's motion-authority gate while we hold it.
 *   - motion flags (homed/paused/estop/override/gen/stream) → the passed
 *     applyFlags() (main.js `_applyWsFlags`), rebuilt into the legacy flag byte.
 *   - machine-config (window/speeds + measured rail) → the passed applyConfig()
 *     (shadow `processConfig`, whose settingsAuthoritative + pending guards we
 *     reuse), plus setTravel() for the measured stroke.
 *   - odometer → the passed renderSession() (main.js `renderSessionCard`).
 *   main.js guards its legacy application sites with `!isSlopSyncLive()`, so when
 *   we are live those legacy paths no-op and only the bridge writes the surface.
 *
 * GROUND TRUTH (CLAUDE.md §3): STATE carries the device's reported values; ECHO
 * carries post-clamp APPLIED values. Page load ADOPTS device state from the
 * retained-on-grant STATE — it never pushes defaults. Write-plane controls show
 * pending until their ECHO confirms, then render the DEVICE's clamped value; a
 * NACK/timeout reverts to the last reported truth. Nothing here optimistically
 * assumes a command took.
 */

import { createSession, CH } from './index.js';
import { PRIORITY, ACCESS, SAFETY_OP, SAFETY_CAUSE_NAME } from './frames.js';
import { getInstanceId } from './identity.js';
import { acquireToken } from './credentials.js';
import { setMotionAuthority, feedExternalSample } from '../telebuf.js';
import { noteSent, processEcho, getReported, getState } from '../shadow.js';
import {
  setWinMin, setWinMax, renderWindow, setSuppressPush, setTravel,
  settingsAuthoritative,
} from '../range.js';
import { OP_SET_WINDOW,
  FLAG_HOMED, FLAG_GEN_RUNNING, FLAG_ESTOP, FLAG_PAUSED,
  FLAG_OVERRIDE, FLAG_INTIFACE_ACTIVE } from '../wire.js';
import { toast } from '../ui.js';

// ---- module state ----------------------------------------------------------
let _session = null;
let _welcomed = false;
let _connected = false;
let _readonly = false;         // true when WELCOME roles lack controller
let _motionAuthority = false;  // true once the MOTION channel is granted
let _hooks = {};               // {applyFlags, applyConfig, renderSession}
let _win = null;               // last device-reported window {min,max} (revert target)
let _synthId = 0x70000000;     // synthetic cmd ids for shadow (disjoint from cmd.js)
let _homed = false;            // last homed flag from the motion channel
let _safety = null;            // last 0x0003 snapshot (incl. RFC-025c modes byte)
let _modes  = null;            // last 0x008A machine-modes snapshot (M5b)

/**
 * Is a granted SlopSync session driving the shared surfaces right now?
 *
 * v1.0: this now means the session reached LIVE — catalog adopted, readiness
 * declared, data plane open. Before that the hub NACKs every INTENT NOT_READY
 * (RFC-015), so claiming the path while SYNCING would swallow a control press
 * that the legacy plane could still have delivered. The e-stop is the one
 * deliberate exception (see sendEstop): it escalates to the raw 0xE5 plane,
 * which no gate applies to.
 */
export function isSlopSyncLive() {
  return _connected && _welcomed && !!(_session && _session.isLive);
}

/** Does this session hold the controller role (write plane enabled)? */
export function isController() { return !_readonly; }

// ---- STATE handlers (read plane) -------------------------------------------

function onMotion(d) {
  // pos_10um / tgt_10um are already scaled to mm by decodePacked; feed the
  // clock-synced display buffer so the existing rail/hero renderers interpolate
  // it exactly as they do legacy 0x01 — 20 Hz in, smoothed out (no stutter).
  if (typeof d.pos_10um === 'number') {
    feedExternalSample(d.pos_10um, typeof d.tgt_10um === 'number' ? d.tgt_10um : d.pos_10um);
  }
  // Rebuild the legacy flag byte from the decoded bitfield — the slopsync motion
  // flag layout (homed,homing,gen,paused,override,estop,stream) does NOT share
  // bit positions with the legacy byte, so map by name, never by raw value.
  const b = d.flags_bits || {};
  _homed = !!b.homed;
  let fb = 0;
  if (b.homed)       fb |= FLAG_HOMED;
  if (b.gen_running) fb |= FLAG_GEN_RUNNING;
  if (b.estop)       fb |= FLAG_ESTOP;
  if (b.paused)      fb |= FLAG_PAUSED;
  if (b.override)    fb |= FLAG_OVERRIDE;
  if (b.stream)      fb |= FLAG_INTIFACE_ACTIVE;
  if (_hooks.applyFlags) _hooks.applyFlags(fb, true /* fromBridge */);
}

function onMachineConfig(d) {
  // Adopt the device's authoritative window + limit set. Reuse shadow's
  // processConfig so the settingsAuthoritative + pending-drag guards apply — but
  // pass through the CURRENT reported values for the fields this channel does
  // NOT carry, so a config adoption never clobbers a blend/mode selection with
  // undefined.
  //
  // WINDOW CAVEAT (live-verified fw 2.1.45): 0x0081 publishes the EFFECTIVE
  // motion window, which while UNHOMED is clamped to the full rail
  // [0, max_rail] regardless of the stored config. Adopting that as the
  // "window setting" stomps the operator's stored window with [0,500] after
  // every drag — so while unhomed we adopt window values only from ECHO
  // (stored config, the write path's ground truth) and skip them here.
  // Homed: effective == stored and the STATE adoption is correct.
  if (_homed) _win = { min: d.window_min, max: d.window_max };
  if (typeof d.max_rail === 'number' && d.max_rail > 0) {
    // Measured stroke — keep TRAVEL fresh (e.g. after a re-home) without a
    // capabilities refetch. Guard the push so setTravel→renderWindow can't
    // re-emit a window intent back at the device.
    setSuppressPush(true);
    try { setTravel(d.max_rail); } finally { setSuppressPush(false); }
  }
  if (_hooks.applyConfig) {
    const blend = getReported('blend'), mode = getReported('mode');
    const ov = getReported('override'), bp = getReported('bypass');
    const sm = getReported('stream_mode'), os = getReported('overshoot');
    _hooks.applyConfig({
      range_min: _homed ? d.window_min : undefined,
      range_max: _homed ? d.window_max : undefined,
      max_speed: d.user_speed,
      accel: d.user_accel,
      // pass-through (undefined-safe) so non-owned controls aren't reset:
      blend_mode: blend ? blend.bm : undefined,
      mode: mode ? mode.transport : undefined,
      manual_override: ov ? ov.on : undefined,
      bypass_limits: bp ? bp.on : undefined,
      stream_speed_mode: sm ? sm.mode : undefined,
      overshoot_clamp: os ? os.on : undefined,
    });
  }
}

/**
 * 0x008A machine-modes (M5b) — blend / transport / stream-speed / overshoot.
 *
 * These four were readable ONLY over HTTP (`/api/settings`) until now, which is
 * why the legacy plane could not be cut: a SlopSync-only client could see the
 * whole machine except the four switches that decide what it does with a
 * command. This closes that hole, so they render from DEVICE truth like
 * everything else instead of from whatever the page last pushed.
 *
 * `enabled_mask` is honoured, not ignored: the device drops the `transport` bit
 * while it is being driven, and a control that is greyed for a real reason is
 * the difference between "refused" and "mysteriously did nothing".
 */
function onMachineModes(d) {
  const en = d.enabled_mask_bits || {};
  _modes = {
    blend: d.blend_mode,
    transport: d.transport,
    streamSpeedMode: d.stream_speed_mode,
    overshoot: d.overshoot_clamp,
    enabled: {
      blend: en.blend_mode !== false,
      transport: en.transport !== false,
      streamSpeedMode: en.stream_speed_mode !== false,
      overshoot: en.overshoot_clamp !== false,
    },
  };
  if (_hooks.applyModes) _hooks.applyModes(_modes);
}

/** Last device-reported mode snapshot (null until the retained push lands). */
export function getModes() { return _modes; }

/**
 * Mode write → 0x0104. `fields` uses the catalog's own keys, so a caller says
 * sendModes({ 1: blendMode }) and never has to know a WS op code.
 * @returns {boolean} true if slopsync took the press (false → caller falls back)
 */
export function sendModes(fields) {
  if (!isSlopSyncLive() || !_session) return false;
  if (_readonly) { toast('Viewer session — controls read-only', 'warn', 'i-alert'); return true; }
  _session.sendModesSet(fields).then((res) => {
    // Ground truth: adopt the APPLIED echo, which for transport/blend may
    // differ from what was asked. Re-rendering from the echo is what stops a
    // refused change from leaving a dropdown lying about the machine.
    if (_hooks.applyModesEcho) _hooks.applyModesEcho(res.applied);
  }).catch((e) => {
    if (window.__DEBUG_ECHO) console.warn('[slopsync] modes-set rejected', e && e.message);
    toast('Mode change not confirmed by the machine', 'bad', 'i-alert', 3000);
  });
  return true;
}

/**
 * Resolve a select field's option LABEL from the catalog this hub actually
 * served, by field name, for either a layout (STATE) or schema (INTENT/EVENT
 * body) field.
 *
 * NEVER a table in this file. The firmware grew a fourth plan kind (`cubic`)
 * after this UI was written, and a hardcoded client table would have shown it
 * as "unknown" — or, in the bug that actually happened device-side, aliased it
 * to index 0 and reported "no active plan" mid-stroke. The catalog is the only
 * thing that cannot drift from the machine.
 *
 * Falls back to the raw number: an unlabelled value is still true.
 */
function optionLabel(channelId, fieldName, value) {
  if (typeof value !== 'number') return value == null ? null : String(value);
  const map = _session && _session.channelMap;
  const entry = map && map.get(channelId);
  if (entry) {
    const pools = [entry.layout, entry.schema];
    for (const pool of pools) {
      if (!Array.isArray(pool)) continue;
      for (const f of pool) {
        if (f && f.name === fieldName && Array.isArray(f.options) && f.options[value] != null) {
          return f.options[value];
        }
      }
    }
  }
  return String(value);
}

/**
 * 0x0086 plan-strip → the legacy `interpState` shape (M5c).
 *
 * This is one of the two jobs `:81` still did on its own. The units line up
 * exactly — the legacy 0x04 decoder read `getUint16()/10000` and this channel
 * declares scale 10000 on the same three positions — so nothing is rescaled
 * here; a mismatch would be silent and would misdraw the planned path.
 *
 * `styleName` comes from the CATALOG's own option labels rather than a table
 * copied into the client, which is the whole point of RFC-009 select fields:
 * when the firmware gained `cubic` as a fourth plan kind, a hardcoded client
 * table would have rendered it as "unknown" or, worse, aliased it to index 0.
 */
function onPlanStrip(d) {
  const f = d.flags_bits || {};
  _hooks.applyInterp && _hooks.applyInterp({
    active:     !!f.active,
    liveMode:   !!f.live,
    gradMode:   !!f.grad,
    style:      d.style,
    styleName:  optionLabel(CH.PLAN_STRIP, 'style', d.style),
    startPos:   d.start_norm,
    endPos:     d.end_norm,
    curPos:     d.cur_norm,
    curVel:     d.cur_vel,
    durationUs: d.duration_us,
    elapsedUs:  d.elapsed_us,
  });
}

/**
 * 0x0089 motion-anomaly EVENT → the anomaly log/renderer (M5c).
 *
 * The other job `:81` did alone. Note this is an EVENT, not a STATE: one frame
 * per occurrence, which is exactly right for "the planner had to compromise"
 * and is why it must never be polled or conflated.
 *
 * The device's kind names are the SlopMotion `AnomalyType` names verbatim, so
 * they line up with the legacy `anomalyCounts` keys without translation. A kind
 * this client has never heard of is still logged rather than dropped — §4.3
 * tolerance, and the whole reason the anomaly panel is worth having.
 */
function onAnomalyEvent(evt) {
  if (!_hooks.applyAnomaly) return;
  const b = evt.body || {};
  _hooks.applyAnomaly({
    kind:      b.kind,
    kindName:  optionLabel(CH.MOTION_ANOMALY, 'kind', b.kind),
    seq:       b.seq,
    targetPos: b.target,
    detail:    b.detail,
    tDevUs:    b.t_us,
  });
}

function onOdometer(d) {
  if (!_hooks.renderSession) return;
  _hooks.renderSession({
    distance_mm: typeof d.distance_m === 'number' ? d.distance_m * 1000 : undefined,
    max_speed_mm_s: d.peak_mm_s,
    strokes: d.strokes,
  });
}

/**
 * 0x0003 safety — the retained, critical-priority latch snapshot. RFC-025c
 * APPENDED a 9th byte, `modes` (manual_override + bypass_limits), because
 * override/bypass are SAFETY-domain state by operator ruling: they render near
 * the rail but they change what the machine does with a motion command, so
 * every surface needs them. They used to be readable only from a legacy HTTP
 * endpoint.
 */
function onSafety(d) {
  const w = d.word_bits || {};
  const m = d.modes_bits || {};
  _safety = {
    estop: !!w.estop, stop: !!w.stop, hold: !!w.hold, pause: !!w.pause,
    override: !!m.override, bypass: !!m.bypass,
    cause: d.cause, causeName: SAFETY_CAUSE_NAME[d.cause] || String(d.cause),
    ownerSession: d.owner_session, estopSeq: d.estop_seq,
  };
  // Ground truth: the checkbox/banner render from the DEVICE's latch, never
  // from a local optimistic toggle.
  if (typeof document !== 'undefined') {
    const ot = document.getElementById('overrideTog');
    if (ot && ot.checked !== _safety.override) ot.checked = _safety.override;
    const bp = document.getElementById('bypassLimits');
    if (bp && bp.checked !== _safety.bypass) bp.checked = _safety.bypass;
  }
}

function onState(channelId, sample) {
  switch (channelId) {
    case CH.MOTION:         onMotion(sample); break;
    case CH.MACHINE_CONFIG: onMachineConfig(sample); break;
    case CH.MACHINE_MODES:  onMachineModes(sample); break;
    case CH.PLAN_STRIP:     onPlanStrip(sample); break;
    case CH.ODOMETER:       onOdometer(sample); break;
    case CH.SAFETY:         onSafety(sample); break;
    // plan-strip / power / slopmotion-diag decode from the catalog and are
    // published on the 'state' event for whatever card consumes them; the
    // gauge wiring is the RFC-009 renderer's job, not this bridge's.
    default: break;
  }
}

/** Last device-reported safety snapshot (null until the retained push lands). */
export function getSafety() { return _safety; }

// ---- Link stats (M5c: the shape link.js used to publish) -------------------
// conn.js's header dot and diag.js's gap shading both read this. Keeping the
// SHAPE identical is what let the :81 plane be deleted without touching either
// renderer — the fields mean the same things, they are just sourced from the
// SlopSync session now.
//
// `fallback` is the honest inversion: it means "the UI is NOT being fed by the
// hub plane", which is exactly when HTTP polling is driving the page.
let _lastStateMs = 0;
let _reconnects = 0;

/** @returns {Object} link stats in the legacy shape (see conn.js / diag.js). */
export function getLinkStats() {
  const live = isSlopSyncLive();
  return {
    wsRttUs: 0,                       // not measured on this plane (CLOCK gives offset, not RTT)
    clockOffsetUs: _session && _session.state ? (_session.state.clockOffsetUs || 0) : 0,
    p95JitterUs: 0,
    txDrops: 0,
    reconnectCount: _reconnects,
    connected: live,
    fallback: !live,
    droppedFrames: 0,                 // the hub conflates rather than drops; §10.4
    lastTeleMs: _lastStateMs,
    lastGapMs: 0,
    // Any granted STATE is this plane's heartbeat — see the noteAlive hook.
    lastStatusMs: _lastStateMs,
    unknownFrames: 0,
  };
}

// ---- write plane -----------------------------------------------------------

/**
 * Adopt applied window bounds into the rail band as DEVICE truth, without
 * re-triggering pushWindow (which would echo the value back at the device).
 */
function adoptWindow(minMm, maxMm) {
  setSuppressPush(true);
  try {
    setWinMin(minMm);
    setWinMax(maxMm);
    renderWindow();
  } finally {
    setSuppressPush(false);
  }
}

/**
 * Stroke-window write (the reported-defect path). Routes the drag/nudge/trim to
 * config-set 0x0101 {1:min,2:max}, drives the existing shadow pending lifecycle,
 * and adopts the post-clamp ECHO as ground truth. NACK/timeout reverts + toasts.
 *
 * COALESCED, latest-wins: config-set is granted at ≤10 Hz, but a drag through
 * the 60 ms pushWindow debounce can emit ~16 Hz. Only one intent is ever in
 * flight; newer values queue over older ones (never a backlog), sends are
 * spaced ≥100 ms, and only the FINAL failure of a drag reverts + toasts —
 * mid-drag NACKs with a newer value queued are superseded, not user-visible.
 * @returns {boolean} true if it took the slopsync path (false → caller falls back)
 */
let _winInflight = false;
let _winQueued = null;        // latest {min,max} superseding the in-flight one
let _winLastSendMs = 0;
const WIN_SEND_SPACING_MS = 100;   // config-set catalog rate: 10 Hz

export function sendWindow(minMm, maxMm) {
  if (!isSlopSyncLive() || !_session) return false;
  if (_readonly) { toast('Viewer session — window is read-only', 'warn', 'i-alert'); return true; }
  if (_winInflight) { _winQueued = { min: minMm, max: maxMm }; return true; }
  _winInflight = true;
  const wait = Math.max(0, WIN_SEND_SPACING_MS - (performance.now() - _winLastSendMs));
  setTimeout(() => _dispatchWindow(minMm, maxMm), wait);
  return true;
}

function _windowSettled() {
  _winInflight = false;
  if (_winQueued) {
    const q = _winQueued;
    _winQueued = null;
    sendWindow(q.min, q.max);
  }
}

function _dispatchWindow(minMm, maxMm) {
  // A newer value may have queued while we waited out the spacing — send that.
  if (_winQueued) { const q = _winQueued; _winQueued = null; minMm = q.min; maxMm = q.max; }
  if (!isSlopSyncLive() || !_session) { _winInflight = false; return; }
  _winLastSendMs = performance.now();
  const id = ++_synthId;
  // pending overlay via the existing shadow model (same shape cmd.js sends).
  noteSent('window', { range_min: minMm, range_max: maxMm, no_persist: true }, id);
  _session.sendConfigSet({ 1: minMm, 2: maxMm }).then((res) => {
    const a1 = res.applied[1], a2 = res.applied[2];
    const am = typeof a1 === 'number' ? a1 : minMm;
    const ax = typeof a2 === 'number' ? a2 : maxMm;
    _win = { min: am, max: ax };
    // ECHO = applied (post-clamp) truth → converge shadow + render the band on
    // it — unless a newer value is already queued (this echo is stale; the
    // shadow id check drops it and the successor will converge the band).
    processEcho({ id, ok: 1, op: OP_SET_WINDOW, reported: { range_min: am, range_max: ax }, cfg_gen: res.cfgGen });
    if (!_winQueued) adoptWindow(am, ax);
    _windowSettled();
  }).catch((err) => {
    processEcho({ id, ok: 0, op: OP_SET_WINDOW, reported: null });
    if (!_winQueued) {
      // Terminal rejection (NACK/timeout, nothing newer coming) — never leave
      // the band lying. Revert to the last device-reported truth.
      if (_win && typeof _win.min === 'number') adoptWindow(_win.min, _win.max);
      toast('Window change not confirmed — reverted to device value', 'bad', 'i-alert', 3500);
    }
    if (window.__DEBUG_ECHO) console.warn('[slopsync] window intent rejected', err && err.message);
    _windowSettled();
  });
}

/** Manual point move → 0x0100 move {1:posMm,2:bypass}. @returns {boolean} took path */
export function sendMove(posMm, bypass) {
  if (!isSlopSyncLive() || !_session) return false;
  if (_readonly) { toast('Viewer session — controls read-only', 'warn', 'i-alert'); return true; }
  _session.sendMove(posMm, !!bypass).catch((e) => {
    if (window.__DEBUG_ECHO) console.warn('[slopsync] move rejected', e && e.message);
  });
  return true;
}

/** Home op → 0x0103 home {1:op}. @returns {boolean} took path */
export function sendHome(op) {
  if (!isSlopSyncLive() || !_session) return false;
  if (_readonly) { toast('Viewer session — controls read-only', 'warn', 'i-alert'); return true; }
  _session.sendHome(op == null ? 1 : op).catch((e) => {
    if (window.__DEBUG_ECHO) console.warn('[slopsync] home rejected', e && e.message);
  });
  return true;
}

/**
 * Safety intent → 0x0005 {1:op}. op from SAFETY_OP.
 *
 * NO LOCAL ROLE CHECK, and that is deliberate: 0x0005's access floor is `watch`
 * and its `option_access` vector (catalog key 17) marks `stop` and `estop`
 * role-EXEMPT — anyone connected may stop this machine (RFC-025b; §11.2's
 * "safety outranks authorization" generalized). The hub gates the rest from
 * that same catalog data, so a client that greys from `session.canUse()` and
 * the hub agree by construction. The failure mode of getting this backwards is
 * "the person standing in the room cannot stop the machine".
 * @returns {boolean} took path
 */
export function sendSafety(op) {
  if (!isSlopSyncLive() || !_session) return false;
  _session.sendSafetyIntent(op).catch((e) => {
    if (window.__DEBUG_ECHO) console.warn('[slopsync] safety rejected', e && e.message);
  });
  return true;
}

/**
 * ASSERT E-STOP — the red button (RFC-010). `safety_ops::estop` (6) did not
 * exist when this bridge was written, so main.js routed the hard e-stop to the
 * LEGACY :81 op and it silently degraded to a decel-stop over SlopSync. It
 * exists now, and the hub treats it exactly as a valid 0xE5 frame: latch,
 * cause=user, publish 0x0003, EVENT twin.
 *
 * Returns true whenever SlopSync took responsibility for the press. The
 * session escalates to the RAW 0xE5 frame by itself when it is not yet LIVE
 * (the readiness gate holds the INTENT plane shut, the raw plane is matched
 * before any gate) — so "connected but still syncing" is still a working
 * e-stop, not a silent no-op.
 * @returns {boolean} took path
 */
export function sendEstop() {
  if (!_session || !_connected) return false;
  _session.assertEstop().catch((e) => {
    if (window.__DEBUG_ECHO) console.warn('[slopsync] estop op rejected (raw frame sent)', e && e.message);
  });
  return true;
}

/** Manual override on/off → 0x0005 ops 7/8 (RFC-025c). @returns {boolean} took path */
export function sendOverride(on) {
  return sendSafety(on ? SAFETY_OP.override_on : SAFETY_OP.override_off);
}

/** Limit bypass on/off → 0x0005 ops 9/10 (RFC-025c). @returns {boolean} took path */
export function sendBypass(on) {
  return sendSafety(on ? SAFETY_OP.bypass_on : SAFETY_OP.bypass_off);
}

/** Raw config-set → 0x0101 (keys per catalog). @returns {boolean} took path */
export function sendConfigSet(fields) {
  if (!isSlopSyncLive() || !_session) return false;
  if (_readonly) { toast('Viewer session — controls read-only', 'warn', 'i-alert'); return true; }
  _session.sendConfigSet(fields).catch((e) => {
    if (window.__DEBUG_ECHO) console.warn('[slopsync] config-set rejected', e && e.message);
  });
  return true;
}

/** machine-admin → 0x0106 {1:op}. 1 clear_fault, 2 save_config, 3 servo_scan. */
export function sendAdmin(op) {
  if (!isSlopSyncLive() || !_session) return false;
  if (_readonly) { toast('Viewer session — controls read-only', 'warn', 'i-alert'); return true; }
  _session.sendIntent(0x0106, { 1: op }).catch((e) => {
    if (window.__DEBUG_ECHO) console.warn('[slopsync] admin op rejected', e && e.message);
  });
  return true;
}

/** Pattern command → 0x0102 pattern-cmd. fields keyed per catalog. */
export function sendPatternCmd(fields) {
  if (!isSlopSyncLive() || !_session) return false;
  if (_readonly) { toast('Viewer session — controls read-only', 'warn', 'i-alert'); return true; }
  _session.sendPatternCmd(fields).catch((e) => {
    if (window.__DEBUG_ECHO) console.warn('[slopsync] pattern rejected', e && e.message);
  });
  return true;
}

// ---- lifecycle -------------------------------------------------------------

/**
 * Bring up the SlopSync bridge. Feeds the passed legacy renderers; installs the
 * write-plane senders on window.__slopsync for the legacy control sites to
 * prefer when live (range.js pushWindow, main.js sendMove/home/safety).
 *
 * @param {Object} hooks
 * @param {(flagsByte:number, fromBridge:boolean)=>void} hooks.applyFlags
 * @param {(cfg:Object)=>void} hooks.applyConfig   shadow.processConfig
 * @param {(stats:Object)=>void} hooks.renderSession
 * @param {string} [hooks.host]  device host (default location.hostname)
 * @param {Function} [hooks.log]
 */
export function initSlopSyncBridge(hooks) {
  _hooks = hooks || {};
  const host = _hooks.host ||
    (typeof location !== 'undefined' && location.hostname) || '192.168.1.229';

  const s = createSession({
    host,
    clientKind: 'webui',
    clientName: 'SlopDrive WebUI',
    log: _hooks.log,
    // IDENTITY, not a nonce: the hub's trust ledger is keyed on instance_id, so
    // this has to be the same 8 bytes across reloads or we are a new device on
    // every refresh and can never stay paired. (See identity.js.)
    instanceId: getInstanceId(),
    // A PROVIDER, not bytes — re-asked on every connect AND every reconnect,
    // because a /uitoken mint is single-use. See credentials.js for the ladder
    // and for why this same call works unchanged inside a Tauri shell.
    token: (h) => acquireToken(h, { log: _hooks.log }),
    subscriptions: [
      // channel, rateHz (0 = on-change/full), priority
      [CH.SAFETY,         0,  PRIORITY.critical],   // retained, never shed
      [CH.CONTROL_OWNER,  0,  PRIORITY.elevated],
      [CH.HUB_STATUS,     1,  PRIORITY.background],
      [CH.MOTION,         20, PRIORITY.elevated],   // live carriage feed
      [CH.MACHINE_CONFIG, 0,  PRIORITY.elevated],   // on-change limit set
      [CH.MACHINE_MODES,  0,  PRIORITY.elevated],   // M5b on-change mode set
      [CH.PATTERN_STATE,  0,  PRIORITY.normal],
      [CH.ODOMETER,       1,  PRIORITY.background],
      [CH.SESSION_EVENTS, 0,  PRIORITY.background],
      // v1.0 device channels. These decode straight from the catalog now that
      // there is no hand-copied fallback table — an unsubscribed channel is
      // simply absent, never stale. A hub that does not advertise one (0x0087
      // power on a machine with no current sensor — RFC-016: capability
      // discovery IS catalog introspection) just never grants it.
      [CH.POWER,          1,  PRIORITY.background],
      [CH.MOTION_DIAG,    1,  PRIORITY.background],
      [CH.MOTION_ANOMALY, 0,  PRIORITY.background],  // EVENT: anomaly panel
    ],
  });
  _session = s;

  s.on('open', () => { _connected = true; });

  // LIVE is the honest "the UI may trust this plane" edge: catalog adopted,
  // readiness declared, retained STATE delivered. Anything earlier and the hub
  // still NACKs intents NOT_READY, so a control press would vanish.
  s.on('live', () => { if (_hooks.onLive) _hooks.onLive(); });

  s.on('welcome', (w) => {
    _welcomed = true;
    // RFC-027 renamed the tiers (wire values unchanged): watch / control /
    // configure. "Read-only" means below `control`.
    _readonly = !(typeof w.roles === 'number' && w.roles >= ACCESS.control);
    if (typeof document !== 'undefined') {
      document.body.classList.toggle('slopsync-live', true);
      document.body.classList.toggle('slopsync-readonly', _readonly);
    }
    if (_hooks.log) _hooks.log('info', 'slopsync welcome roles=' + w.roles + ' readonly=' + _readonly);
  });

  s.on('grant', (grants) => {
    // Engage motion authority only once the carriage feed is actually granted,
    // so telebuf keeps taking legacy 0x01 until we can truly replace it.
    if (grants.some((g) => g.channel === CH.MOTION)) {
      _motionAuthority = true;
      setMotionAuthority(true);
    }
  });

  // Liveness: ANY granted STATE arrival is proof the link is alive. The legacy
  // plane needed a dedicated 0x02 STATUS heartbeat because an idle rig emits no
  // motion frames; here 0x0006 hub-status at 1 Hz plus the on-change channels
  // serve the same purpose, so the >1 s control-suspension gate never trips on
  // a healthy-but-idle machine.
  s.on('state', (ch, sample, ts) => {
    _lastStateMs = performance.now();
    if (_hooks.noteAlive) _hooks.noteAlive();
    onState(ch, sample, ts);
  });

  // 0x0089 motion-anomaly is an EVENT, not a STATE — one frame per occurrence.
  s.on('event', (evt) => {
    if (evt && evt.channel === CH.MOTION_ANOMALY) onAnomalyEvent(evt);
  });

  s.on('nack', (n) => {
    if (window.__DEBUG_ECHO) console.warn('[slopsync] NACK', n.name, 'ch=0x' + (n.channel || 0).toString(16));
  });

  s.on('close', (c) => {
    _connected = false;
    _welcomed = false;
    _safety = null;
    _modes = null;
    if (_motionAuthority) { _motionAuthority = false; setMotionAuthority(false); }
    if (typeof document !== 'undefined') document.body.classList.remove('slopsync-live', 'slopsync-readonly');
    if (_hooks.log) _hooks.log('info', 'slopsync closed willReconnect=' + (c && c.willReconnect));
    if (c && c.willReconnect) _reconnects++;
    if (_hooks.onDown) _hooks.onDown();
    // Legacy plane resumes ownership of every shared surface automatically —
    // the main.js guards go transparent the instant isSlopSyncLive() is false.
  });

  // Expose the write-plane senders for the legacy control sites (range.js
  // pushWindow, main.js sendMove/home/safety) to prefer when live. Using a
  // window handle (not an import) keeps range.js free of a bridge import cycle
  // and matches the codebase's existing window.__sendMove pattern.
  if (typeof window !== 'undefined') {
    window.__slopsync = {
      isLive: isSlopSyncLive,
      isController,
      sendWindow,
      sendMove,
      sendHome,
      sendSafety,
      sendEstop,
      sendOverride,
      sendBypass,
      sendPatternCmd,
      sendConfigSet,
      sendAdmin,
      sendModes,
      getSafety,
      getModes,
      session: s,
    };
  }

  // ---- Background tabs (the alt-tab death) --------------------------------
  // Browsers throttle setInterval hard in a hidden tab — down to once a minute.
  // The session keepalive PING rides one of those timers and the hub's deadman
  // is 600 ms, so a few seconds in the background is a guaranteed teardown.
  // That part is unavoidable and FINE: the machine is right to drop a client it
  // cannot hear from.
  //
  // What is NOT fine is failing to come back. Reconnect backoff runs on a
  // throttled timer too, so on returning to the tab the page could sit
  // disconnected for up to a minute and read as "dead until reload". Becoming
  // visible is the one moment we KNOW timers run at full speed — retry then,
  // and let the normal backoff cover everything else.
  if (typeof document !== 'undefined' && document.addEventListener) {
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState !== 'visible') return;
      if (isSlopSyncLive()) return;
      if (_hooks.log) _hooks.log('info', 'tab foregrounded while down — reconnecting now');
      try { s.connect(); } catch (e) { /* already in flight */ }
    });
  }

  s.connect();
  return s;
}
