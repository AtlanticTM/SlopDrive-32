/**
 * shadow.svelte.js — the Ground Truth Doctrine, implemented once.
 *
 * CLAUDE.md 3: "Optimistic UI state is prohibited: controls reflect confirmed
 * device state... a UI that lies about machine state is a safety defect on this
 * product." This file is the only place a write is allowed to be in flight, and
 * the only place that decides what number a control shows while it is.
 *
 * ── The lifecycle ──────────────────────────────────────────────────────────
 *
 *   confirmed ──write──> pending ──ECHO──> confirmed (value = APPLIED)
 *                          │  │
 *                          │  └─500ms──> overdue  (same value, louder styling)
 *                          │                 └─2s──> fault
 *                          └─NACK────────────────────> fault (value snaps back)
 *
 * While pending we display the REQUESTED value, because a slider that springs
 * back under the user's thumb is unusable. But it is styled unconfirmed the
 * whole time, and the moment the ECHO lands we display the APPLIED value — so
 * if the device clamped 950 to 800, the number visibly changes in front of the
 * operator. That visible snap is the feature, not a glitch: it is the machine
 * correcting the client in public.
 *
 * On NACK or timeout the requested value is DISCARDED and the control returns
 * to the device's reported truth. We never keep a value the machine refused.
 *
 * ── Writing at the rate the machine accepts ────────────────────────────────
 *
 * Dragging a slider can generate 60 changes a second; the catalog says what the
 * INTENT channel will actually take (0x0101 is 10 Hz, 0x0105 is 5 Hz). So each
 * write channel gets a coalescing queue clocked at its OWN advertised rate,
 * read from the catalog. Exceeding it would earn RATE_LIMITED NACKs and make
 * every control look broken under normal use. This is the write-side twin of
 * "subscribe at the rate you draw".
 */

import { machine, getSession } from './machine.svelte.js';
import { reportedValue } from './settings.js';

const OVERDUE_MS = 500;
const FAULT_MS = 2000;
const SETTLE_MS = 900;      // how long a confirm flash lingers

/** key -> shadow record. key is `${writeChannel}:${settingKey}`. */
export const shadows = $state({});

export const STATUS = {
  confirmed: 'confirmed',
  pending: 'pending',
  overdue: 'overdue',
  fault: 'fault',
};

function keyOf(writeChannel, settingKey) {
  return writeChannel + ':' + settingKey;
}

// ---------------------------------------------------------------------------
// Per-channel coalescing queues
// ---------------------------------------------------------------------------

/** writeChannel -> { pending: Map<settingKey, value>, timer, lastSentAt } */
const queues = new Map();

/** The channel's own advertised intent rate, straight from the catalog. */
function intervalFor(channelId) {
  const e = machine.catalog.entries.find((x) => x.id === channelId);
  const hz = e && e.maxRateHz ? e.maxRateHz : 5;
  return Math.max(1000 / hz, 20);
}

function queueFor(channelId) {
  if (!queues.has(channelId)) {
    queues.set(channelId, { pending: new Map(), timer: null, lastSentAt: 0 });
  }
  return queues.get(channelId);
}

/**
 * Flush one channel's coalesced fields as a single INTENT.
 *
 * Coalescing is not just an optimization: sending min and max as two intents
 * lets the hub see a transiently inverted window and clamp against a bound the
 * user was in the middle of moving. One intent carrying both is atomic from
 * the machine's point of view.
 */
async function flush(channelId) {
  const q = queueFor(channelId);
  q.timer = null;
  if (!q.pending.size) return;

  const session = getSession();
  const fields = {};
  for (const [k, v] of q.pending) fields[k] = v;
  const keys = [...q.pending.keys()];
  q.pending.clear();
  q.lastSentAt = Date.now();

  if (!session || !session.isLive) {
    for (const k of keys) fail(channelId, k, 'no link');
    return;
  }

  try {
    // sendIntent encodes each value using the CBOR type the catalog publishes
    // for that key — no local type table, so a machine whose field is a float
    // where ours is an int is encoded correctly without any client change.
    const echo = await session.sendIntent(channelId, fields);
    const applied = (echo && echo.applied) || {};
    for (const k of keys) {
      const sh = shadows[keyOf(channelId, k)];
      if (!sh) continue;
      // The ECHO is the post-clamp APPLIED value. If the key is missing from
      // the echo the machine did not tell us what it did, and we must not
      // pretend it agreed — fall back to reported truth.
      if (Object.prototype.hasOwnProperty.call(applied, k)) {
        sh.applied = applied[k];
        settle(sh);
      } else {
        fail(channelId, k, 'no applied value in echo');
      }
    }
    if (echo && echo.cfgGen != null) machine.link.cfgGen = echo.cfgGen;
  } catch (err) {
    const msg = (err && (err.name || err.message)) || 'rejected';
    for (const k of keys) fail(channelId, k, msg);
  }
}

function schedule(channelId) {
  const q = queueFor(channelId);
  if (q.timer) return;
  const interval = intervalFor(channelId);
  const wait = Math.max(0, q.lastSentAt + interval - Date.now());
  q.timer = setTimeout(() => flush(channelId), wait);
}

// ---------------------------------------------------------------------------
// Shadow state transitions
// ---------------------------------------------------------------------------

function clearTimers(sh) {
  if (sh._t1) { clearTimeout(sh._t1); sh._t1 = null; }
  if (sh._t2) { clearTimeout(sh._t2); sh._t2 = null; }
  if (sh._t3) { clearTimeout(sh._t3); sh._t3 = null; }
}

function settle(sh) {
  clearTimers(sh);
  sh.status = STATUS.confirmed;
  sh.settled = true;
  sh.error = null;
  sh.requested = undefined;
  sh._t3 = setTimeout(() => { sh.settled = false; }, SETTLE_MS);
}

function fail(channelId, settingKey, why) {
  const sh = shadows[keyOf(channelId, settingKey)];
  if (!sh) return;
  clearTimers(sh);
  sh.status = STATUS.fault;
  sh.error = why;
  // Discard the request. The control snaps back to what the machine reports,
  // because that is what is true.
  sh.requested = undefined;
  sh._t3 = setTimeout(() => {
    if (sh.status === STATUS.fault) { sh.status = STATUS.confirmed; sh.error = null; }
  }, FAULT_MS * 2);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Write one setting. Returns immediately; the shadow carries the outcome.
 *
 * @param {Object} field a field from buildSettingsModel (must not be readOnly)
 * @param {number|string|boolean} value the value the operator chose
 */
export function writeSetting(field, value) {
  if (!field || field.readOnly || field.writeChannel == null) return;
  const k = keyOf(field.writeChannel, field.settingKey);
  let sh = shadows[k];
  if (!sh) {
    sh = shadows[k] = {
      status: STATUS.confirmed, requested: undefined, applied: undefined,
      error: null, settled: false, sentAt: 0,
    };
  }
  clearTimers(sh);
  sh.status = STATUS.pending;
  sh.requested = value;
  sh.error = null;
  sh.settled = false;
  sh.sentAt = Date.now();
  sh._t1 = setTimeout(() => {
    if (sh.status === STATUS.pending) sh.status = STATUS.overdue;
  }, OVERDUE_MS);
  sh._t2 = setTimeout(() => {
    if (sh.status === STATUS.pending || sh.status === STATUS.overdue) {
      fail(field.writeChannel, field.settingKey, 'no echo');
    }
  }, FAULT_MS);

  queueFor(field.writeChannel).pending.set(field.settingKey, value);
  schedule(field.writeChannel);
}

/**
 * Fire an INTENT verb (RFC-019 `action.*`). Actions are not settings: there is
 * no value to shadow, only a success or a refusal, so they resolve to a result
 * the caller can surface as a toast.
 *
 * @returns {Promise<{ok: boolean, error?: string, applied?: Object}>}
 */
export async function runAction(action, value = 1) {
  const session = getSession();
  if (!session || !session.isLive) return { ok: false, error: 'no link' };
  try {
    const echo = await session.sendIntent(action.channelId, { [action.key]: value });
    return { ok: true, applied: (echo && echo.applied) || {} };
  } catch (err) {
    return { ok: false, error: (err && (err.name || err.message)) || 'rejected' };
  }
}

/** Current shadow record for a field, or null if it has never been written. */
export function shadowOf(field) {
  if (!field || field.writeChannel == null) return null;
  return shadows[keyOf(field.writeChannel, field.settingKey)] || null;
}

/** Status string for styling: confirmed | pending | overdue | fault. */
export function statusOf(field) {
  const sh = shadowOf(field);
  return sh ? sh.status : STATUS.confirmed;
}

/**
 * The value a control must display.
 *
 * In flight -> the requested value (so the control tracks the operator's hand).
 * Otherwise -> the device's reported value, always. Never a remembered request,
 * never a default, never a guess.
 */
export function displayValue(field, sample) {
  const sh = shadowOf(field);
  if (sh && (sh.status === STATUS.pending || sh.status === STATUS.overdue)
      && sh.requested !== undefined) {
    return sh.requested;
  }
  return reportedValue(field, sample);
}

/** True while any write is outstanding — drives the global "unconfirmed" hint. */
export function anyPending() {
  for (const k in shadows) {
    const s = shadows[k].status;
    if (s === STATUS.pending || s === STATUS.overdue) return true;
  }
  return false;
}
