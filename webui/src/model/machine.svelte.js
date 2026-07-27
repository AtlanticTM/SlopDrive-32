/**
 * machine.svelte.js — the single reactive spine.
 *
 * Owns exactly one SlopSync session and projects it into Svelte 5 reactive
 * state. Every component reads from here; nothing else opens a socket, and
 * nothing anywhere fabricates a value the machine did not send.
 *
 * ── What "ground truth" means mechanically ─────────────────────────────────
 *
 * `samples[channelId]` is whatever arrived in that channel's last STATE push,
 * decoded by the catalog's own layout. There is no merge with local intent, no
 * optimistic pre-application, no defaulting. A control that wants to know what
 * the machine is doing reads this; a control that wants to know whether its own
 * write landed asks shadow.svelte.js. Keeping those two questions in separate
 * files is what stops the second one from quietly answering the first.
 *
 * ── Subscribing at the rate we draw (SPEC 10.2) ────────────────────────────
 *
 * We ask for min(catalog max, DRAW_HZ) rather than the channel's ceiling.
 * Asking 333 Hz to render 60 wastes the machine's airtime and its heap, and the
 * hub would shed us first under congestion. On-change channels are wished at 0,
 * which is the protocol's "push when it changes" and is strictly better than
 * any polling rate we could pick.
 */

import {
  createSession, CHANNEL_CLASS, PRIORITY, acquireToken, getInstanceId, toHex,
} from '../core/slopsync/index.js';
import { buildSettingsModel } from './settings.js';

/** Highest rate we can actually paint. Everything is capped to this. */
const DRAW_HZ = 30;

/** Bounded rings — an EVENT channel is a firehose and memory is not free. */
const LOG_MAX = 400;
const ANOM_MAX = 200;
const EVT_MAX = 120;
const NACK_MAX = 60;

/**
 * THE reactive machine state. One object, deeply proxied by Svelte.
 */
export const machine = $state({
  /** Connection lifecycle as the UI understands it. */
  link: {
    phase: 'idle',          // idle | connecting | handshaking | live | retrying | failed
    since: 0,
    sessionId: null,
    roles: 0,               // access tier granted to THIS session
    error: null,
    willReconnect: false,
    closeReason: '',
    deadmanMs: 0,
    cfgGen: 0,
    hubIdentity: null,      // RFC-016 in-band identity, when the hub sends it
  },

  /** Catalog + everything derived from it. Replaced wholesale on adoption. */
  catalog: {
    ready: false,
    entries: [],
    etag: '',               // hex, ready to display
    verified: false,        // etag matched what the hub declared (SPEC 8.3)
    bytes: 0,
    cached: false,          // served from the local cache, not re-fetched
    model: null,            // buildSettingsModel() output
  },

  /** channelId -> last decoded STATE sample. The ONLY source of device values. */
  samples: {},
  /** channelId -> ms timestamp of that sample, for staleness display. */
  sampleTs: {},
  /** channelId -> granted {rate, priority}, so the UI can show what it really gets. */
  grants: {},

  /** Bounded event rings, newest last. */
  events: {
    log: [],
    anomaly: [],
    session: [],
    nacks: [],
  },

  /** Link quality counters for the SlopSync pane. */
  stats: {
    framesIn: 0,
    framesOut: 0,
    bytesIn: 0,
    statePushes: 0,
    lastRxMs: 0,
    clockOffsetUs: null,
    clockRttUs: null,
    reconnects: 0,
  },
});

let session = null;
let _host = '';

/** The live session handle, for the write plane. Null until connect(). */
export function getSession() {
  return session;
}

/** Is the hub plane usable for writes right now? */
export function isLive() {
  return !!session && session.isLive;
}

// ---------------------------------------------------------------------------
// Subscription policy
// ---------------------------------------------------------------------------

/**
 * Build SUBSCRIBE wishes from the catalog itself — every hub-to-client channel
 * it advertises, at a rate we can paint.
 *
 * Generic on purpose: a machine with channels we have never heard of gets
 * subscribed to anyway, and its data shows up in the diagnostics surface even
 * though no bespoke widget knows what it means. That is the difference between
 * a client and OUR client.
 */
function subscriptionWishes(entries) {
  const wishes = [];
  for (const e of entries) {
    if (e.dir !== 0) continue;                       // h2c only; we do not publish
    if (e.cls !== CHANNEL_CLASS.STATE && e.cls !== CHANNEL_CLASS.EVENT) continue;
    // EVENTs are edge-driven; a rate on them is meaningless. On-change STATE
    // channels advertise 0 and mean it.
    const rate = (e.cls === CHANNEL_CLASS.EVENT || !e.maxRateHz)
      ? 0
      : Math.min(e.maxRateHz, DRAW_HZ);
    wishes.push([e.id, rate, e.priority != null ? e.priority : PRIORITY.background]);
  }
  return wishes;
}

// ---------------------------------------------------------------------------
// Ring helpers
// ---------------------------------------------------------------------------

function push(ring, item, max) {
  ring.push(item);
  if (ring.length > max) ring.splice(0, ring.length - max);
}

// ---------------------------------------------------------------------------
// Connect
// ---------------------------------------------------------------------------

/**
 * Open the one session.
 *
 * The token is passed as a PROVIDER, not a value: credentials are re-resolved
 * on every reconnect, so a token that expired while we were away is replaced
 * instead of being retried forever. This is also the entire Tauri seam — a
 * desktop shell overrides host + setHttpGet() and changes nothing else.
 */
export function connect(opts = {}) {
  if (session) return session;
  _host = opts.host || (typeof location !== 'undefined' ? location.hostname : '');

  machine.link.phase = 'connecting';
  machine.link.since = Date.now();

  session = createSession({
    host: _host,
    port: opts.port || 82,
    clientKind: 'webui',
    clientName: opts.clientName || 'SlopDrive WebUI',
    instanceId: getInstanceId(),
    token: (h) => acquireToken(h),
    autoReconnect: true,
  });

  session.on('open', () => {
    machine.link.phase = 'handshaking';
    machine.link.error = null;
  });

  session.on('welcome', (w) => {
    machine.link.sessionId = w.sessionId;
    machine.link.roles = w.roles || 0;
    machine.link.deadmanMs = w.deadmanMs || 0;
    machine.link.cfgGen = w.cfgGen || 0;
    machine.link.hubIdentity = w.identity || null;
  });

  session.on('catalog', (entries, _map, meta) => {
    // Rebuild the entire renderable model. Anything the machine dropped or
    // added between connections is picked up here with no per-channel code —
    // which is the claim this whole refactor exists to make true.
    // The session emits { cached, verified, etag } and the etag is RAW BYTES —
    // rendering it directly would print a garbled array. Hex it once here so
    // every consumer gets something displayable.
    machine.catalog = {
      ready: true,
      entries,
      etag: (meta && meta.etag) ? toHex(meta.etag) : '',
      verified: !!(meta && meta.verified),
      bytes: session.catalogBytes ? session.catalogBytes.length : 0,
      cached: !!(meta && meta.cached),
      model: buildSettingsModel(entries),
    };
    // Subscribe only once we know what exists. Wishing for channels before the
    // catalog is how a client ends up hardcoding ids.
    session.subscribe(subscriptionWishes(entries));
  });

  session.on('grant', (grants) => {
    for (const g of grants || []) {
      machine.grants[g.channel] = { rate: g.rate, priority: g.priority };
    }
  });

  session.on('live', () => {
    machine.link.phase = 'live';
    machine.link.since = Date.now();
  });

  session.on('state', (channelId, sample, tsMs) => {
    machine.samples[channelId] = sample;
    machine.sampleTs[channelId] = tsMs || Date.now();
    machine.stats.statePushes++;
    machine.stats.lastRxMs = Date.now();
  });

  session.on('event', (evt) => {
    machine.stats.lastRxMs = Date.now();
    // Route by the channel's CLASS and the catalog's own naming rather than by
    // a hardcoded id table: an unknown EVENT channel still lands somewhere
    // visible instead of being silently dropped.
    const entry = machine.catalog.entries.find((e) => e.id === evt.channel);
    const name = entry ? entry.name : ('channel ' + evt.channel);
    const rec = { ...evt, channelName: name, at: Date.now() };
    if (/log/i.test(name)) push(machine.events.log, rec, LOG_MAX);
    else if (/anomaly/i.test(name)) push(machine.events.anomaly, rec, ANOM_MAX);
    else push(machine.events.session, rec, EVT_MAX);
  });

  session.on('sessionEvent', (evt) => {
    push(machine.events.session, { ...evt, at: Date.now() }, EVT_MAX);
  });

  session.on('nack', (n) => {
    push(machine.events.nacks, { ...n, at: Date.now() }, NACK_MAX);
  });

  session.on('clock', (c) => {
    machine.stats.clockOffsetUs = c.offsetUs;
    machine.stats.clockRttUs = c.rttUs;
  });

  session.on('close', (c) => {
    machine.link.phase = c.willReconnect ? 'retrying' : 'failed';
    machine.link.willReconnect = !!c.willReconnect;
    machine.link.closeReason = c.reason || '';
    machine.link.sessionId = null;
    // Roles are a property of the session, not of the machine. Dropping them
    // here is what makes every write control grey the instant the link dies,
    // instead of looking usable until the user tries.
    machine.link.roles = 0;
    if (c.willReconnect) machine.stats.reconnects++;
  });

  session.connect();
  installVisibilityRecovery();
  return session;
}

// ---------------------------------------------------------------------------
// The alt-tab problem
// ---------------------------------------------------------------------------

/**
 * Browsers throttle background tabs to roughly one timer callback per minute.
 * The hub's deadman is 600 ms. So a backgrounded tab stops PINGing, gets torn
 * down as a dead session, and — because the RECONNECT backoff timer is
 * throttled too — does not come back until the tab is focused again. To the
 * operator this reads as "alt-tabbing kills the page", which is exactly what
 * was reported.
 *
 * There is no client-side fix for the throttling itself. What we can do is
 * treat regaining visibility as an explicit signal to re-establish now rather
 * than waiting for a timer that may be minutes away.
 */
let _visibilityInstalled = false;
function installVisibilityRecovery() {
  if (_visibilityInstalled || typeof document === 'undefined') return;
  _visibilityInstalled = true;
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState !== 'visible') return;
    if (!session) return;
    if (!session.isLive) {
      machine.link.phase = 'connecting';
      try { session.connect(); } catch (e) { /* already connecting: harmless */ }
    }
  });
}

/** Tear down (used by tests and by the Tauri shell on host change). */
export function disconnect() {
  if (!session) return;
  try { session.close(); } catch (e) { /* ignore */ }
  session = null;
  machine.link.phase = 'idle';
}
