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
 * ── Two rates that must never be conflated ─────────────────────────────────
 *
 * DRAW rate — how often a widget repaints. Free (it's a couple of floats and
 * a canvas stroke), so it runs at whatever `requestAnimationFrame` gives it —
 * 60 Hz, 120 Hz, 240 Hz, whatever the display refreshes at. Nothing in the
 * render path (RailWidget.svelte, PlanStrip.svelte) may add a fixed-interval
 * timer or a self-imposed fps cap; `prefers-reduced-motion` is the one
 * sanctioned exception (PlanStrip's reduced path drops to a 1 Hz `setInterval`
 * on purpose — that is accessibility, not a performance cap).
 *
 * SUBSCRIBE rate — how often the DEVICE sends a STATE push for a channel.
 * This one is emphatically not free: ESP32 airtime, heap, WS frame count, and
 * the SlopSyncHubService task's own 5 ms tick budget, shared across every
 * channel ~30 of them subscribed at once. This is the rate `MAX_SUBSCRIBE_HZ`
 * and `TELEMETRY_HZ` below actually govern. A higher DRAW rate cannot make a
 * ragged SUBSCRIBE rate look better — more frames just render the same uneven
 * samples more finely (see telebuf.js's interpolation, which is where
 * smoothness is actually won). Raising subscribe rate is a mistake in the
 * OTHER direction too — see TELEMETRY_HZ below: the fastest available rate
 * (the catalog's 60 Hz ceiling) measured WORSE than a slower one, because the
 * hub can't pace it evenly.
 *
 * The old name here was `DRAW_HZ`, reasoning "subscribe at the rate we draw"
 * (SPEC 10.2's intent, but SPEC 10.2 is about not over-asking a channel's
 * ceiling for no reason — it never said the two rates should be numerically
 * equal). That name is what led this policy to cap subscriptions at 30
 * because the page draws at ~60: a readout drawn at 60+ fps does not need
 * 60+ Hz of NEW data to look smooth, it needs EVENLY spaced data it can
 * interpolate between. Renamed to say what it actually bounds.
 */

import {
  createSession, CHANNEL_CLASS, PRIORITY, NACK, acquireToken, getInstanceId, toHex,
} from '../core/slopsync/index.js';
import { buildSettingsModel } from './settings.js';
import { ROLE } from './roles.js';

/**
 * Default ceiling for ordinary channels (settings, diagnostics, tuning) —
 * nobody's eye tracks these in real time frame-to-frame, so a modest,
 * unmeasured-but-safe rate is fine. NOT a draw rate; see this file's header.
 * TELEMETRY_HZ below overrides this for the three roles that actually need
 * to be paced well. This is a two-tier policy (telemetry vs everything else),
 * not full per-priority stratification — nothing measured here showed
 * background/diagnostic channels need their own tier, and the override
 * mechanism (telemetryChannelIds()) generalizes to adding one if that changes.
 */
const MAX_SUBSCRIBE_HZ = 30;

/**
 * Live kinematic telemetry (the rail comet: position/target/velocity) gets
 * its OWN subscribe rate instead of MAX_SUBSCRIBE_HZ — measured, not guessed,
 * against the real device with the carriage moving
 * (webui/test/position-jitter-probe.mjs).
 *
 * The hub paces each STATE subscription on its own SlopSyncHubService task
 * tick (firmware: 5 ms — SPEC's pacer truncates `periodMs = 1000/rate_hz` to
 * whole ms and only checks it at tick boundaries). The closer a channel's
 * wished period sits to that 5 ms grain, the more the delivery shows
 * duplicate-timestamp/burst artifacts: measured on-device, 60 Hz (16.7 ms
 * period, the catalog's own advertised ceiling — NOT reachable at any higher
 * rate regardless of draw rate) produced duplicate-timestamped pushes on ~9%
 * of samples and a visibly heavier tail (p95/max inter-arrival,
 * implied-acceleration p95) than 25 Hz (40 ms = exactly 8 hub ticks), which
 * measured ZERO duplicates and the tightest p95/max-vs-median ratio of every
 * rate from 20-60 Hz tried under a full realistic subscription load (all
 * ~30 channels live, not isolated). An aligned 40/50 Hz sounded like it should
 * win on paper (also exact tick multiples) but measured WORSE than 25 Hz in
 * practice — the tick's fraction of the period (5ms/25ms=20% at 40Hz vs
 * 5ms/40ms=12.5% at 25Hz) tracked the real jitter better than alignment alone.
 *
 * Only the three ROLES that feed the rail's live comet/numerals get this
 * treatment; every other channel (settings, diagnostics, tuning) keeps the
 * MAX_SUBSCRIBE_HZ policy — this is not "subscribe to everything faster", it
 * is "the one signal a human's eye tracks in real time gets a SUBSCRIBE rate
 * chosen for even pacing, independent of and much lower than the DRAW rate
 * that renders it".
 */
const TELEMETRY_HZ = 25;
const TELEMETRY_ROLES = new Set([ROLE.telemetryPosition, ROLE.telemetryTarget, ROLE.telemetryVelocity]);

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
    limits: {},             // the hub's declared ceilings, from WELCOME
    subsDropped: 0,         // channels we had to shed to fit max_subscriptions
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
 * it advertises, at a rate the DEVICE can pace, not a rate the browser draws.
 *
 * Generic on purpose: a machine with channels we have never heard of gets
 * subscribed to anyway, and its data shows up in the diagnostics surface even
 * though no bespoke widget knows what it means. That is the difference between
 * a client and OUR client.
 *
 * ONE combined wish list — STATE and EVENT wishes mixed in the same frame.
 * RFC-033 settled this: mixing classes in one SUBSCRIBE is, and always was,
 * legal. See subscribeInBatches() for the field bug this used to be blamed on.
 *
 * `telemetryChanIds` (from the telemetryChannelIds() helper below) gets
 * TELEMETRY_HZ instead of MAX_SUBSCRIBE_HZ — see that constant's header for why.
 */
function subscriptionWishes(entries, maxSubs, telemetryChanIds) {
  const wishes = [];
  for (const e of entries) {
    if (e.dir !== 0) continue;                       // h2c only; we do not publish
    if (e.cls !== CHANNEL_CLASS.STATE && e.cls !== CHANNEL_CLASS.EVENT) continue;
    // EVENTs are edge-driven; a rate on them is meaningless. On-change STATE
    // channels advertise 0 and mean it.
    let rate = (e.cls === CHANNEL_CLASS.EVENT || !e.maxRateHz)
      ? 0
      : Math.min(e.maxRateHz, MAX_SUBSCRIBE_HZ);
    if (telemetryChanIds && telemetryChanIds.has(e.id) && e.maxRateHz) {
      rate = Math.min(e.maxRateHz, TELEMETRY_HZ);
    }
    wishes.push([e.id, rate, e.priority != null ? e.priority : PRIORITY.background]);
  }

  // ---- RESPECT THE HUB'S SUBSCRIPTION CAP --------------------------------
  //
  // FIELD BUG, found by pointing this client at the real machine: it advertises
  // 33 channels, this wanted 21 of them, and the hub's per-session cap is
  // smaller than that. Over-subscribing did NOT earn a NACK — the SUBSCRIBE was
  // dropped WHOLESALE, so the session went LIVE with zero grants and zero STATE,
  // and every readout on every tab rendered `--`. It looked like a rendering
  // bug and was a protocol-etiquette bug. The simulator hid it by advertising
  // fewer channels.
  //
  // The cap is whatever the hub declared in WELCOME, so this adapts to any
  // machine rather than hardcoding a number. When we have to drop some, drop
  // the LEAST important: sorting by priority descending keeps safety and motion
  // and sheds background diagnostics, which is the same ordering the hub itself
  // uses when it sheds under congestion (SPEC 10.4).
  const cap = (typeof maxSubs === 'number' && maxSubs > 0) ? maxSubs : wishes.length;
  if (wishes.length <= cap) return wishes;
  const ranked = wishes.slice().sort((a, b) => b[2] - a[2]);
  const kept = ranked.slice(0, cap);
  machine.link.subsDropped = wishes.length - cap;
  return kept;
}


/**
 * Send SUBSCRIBE in batches that fit the hub's declared per-frame wish cap.
 *
 * ── Two field bugs, and the corrected diagnosis (RFC-033) ──────────────────
 *
 * FIELD BUG #1: the real machine advertises `max_frame: 512` bytes. This
 * client wanted 21 channels in one SUBSCRIBE; the hub dropped it WHOLESALE —
 * no NACK, no grants, no STATE. The session sat happily LIVE while every
 * readout on every tab rendered `--`, which reads exactly like a rendering
 * bug and is not one.
 *
 * FIELD BUG #2 (the one that actually mattered): the byte-size math above was
 * the wrong model entirely. A conservative fixed batch of 8 was shipped as
 * the fix, tuned down from an estimate that regressed the moment the catalog
 * grew from 33 to 44 entries. Both "mixed STATE+EVENT frame" drops that were
 * blamed on class-mixing at the time were actually this: the hub was silently
 * dropping any SUBSCRIBE over its UNDECLARED wish-count cap (16, on this
 * hub), which nobody could see without binary-searching a live machine.
 *
 * THE FIX, now that the cap is advertised (WELCOME `limits` key 4,
 * `max_subscriptions_per_frame`): batch by COUNT, sized from that value
 * directly. No byte estimate, no class split — mixing STATE and EVENT wishes
 * in one frame is, and always was, legal (RFC-033's ruling). Fall back to 8
 * only when the hub is old enough not to advertise key 4 at all; a hub that
 * DOES advertise it is trusted completely, because overflow now answers
 * `SUBSCRIBE_REJECTED` (0x0204) instead of silence — see the `nack` handler
 * below, which treats that code as a loud client-bug error rather than
 * routine congestion shedding.
 */
function subscribeInBatches(wishes) {
  const perFrame = machine.link.limits.max_subscriptions_per_frame;
  const budget = (typeof perFrame === 'number' && perFrame > 0) ? perFrame : 8;
  for (let i = 0; i < wishes.length; i += budget) {
    session.subscribe(wishes.slice(i, i + budget));
  }
}

/** Channel ids carrying any of TELEMETRY_ROLES on this machine, by role — never a hardcoded id. */
function telemetryChannelIds(model) {
  const ids = new Set();
  if (!model || !model.byRole) return ids;
  for (const role of TELEMETRY_ROLES) {
    const list = model.byRole.get(role);
    if (list) for (const f of list) ids.add(f.channelId);
  }
  return ids;
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
    // The hub's own declared ceilings. max_subscriptions is the one that bites:
    // exceeding it drops the whole SUBSCRIBE silently. See subscriptionWishes().
    machine.link.limits = w.limits || {};
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
    // ONE combined wish list, STATE and EVENT together — RFC-033 settled that
    // mixing classes in a SUBSCRIBE is legal; the only real constraint is the
    // per-frame wish count, which subscribeInBatches() sizes from the hub's
    // own advertised cap. See that function's header for the corrected story.
    const lim = machine.link.limits.max_subscriptions;
    subscribeInBatches(subscriptionWishes(entries, lim, telemetryChannelIds(machine.catalog.model)));
  });

  session.on('grant', (grants) => {
    for (const g of grants || []) {
      machine.grants[g.channel] = { rate: g.rate, priority: g.priority };
    }
  });

  // A PAIR_GRANT upgrades this session's tier IN PLACE — the hub does not
  // require a reconnect, so neither may we. Without mirroring it here, every
  // widget outside the pairing pane keeps rendering the OLD tier and greys
  // configure-only controls that the machine would now accept: the UI would be
  // lying about what this session can do, in the direction that hides working
  // controls. PairingPane tracks session.state.roles directly and so was
  // already correct; this makes the rest of the page agree with it.
  session.on('pairGrant', (g) => {
    if (g && g.role != null) machine.link.roles = g.role | 0;
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
    // SUBSCRIBE_REJECTED (0x0204) means a client bug — this client sent a
    // SUBSCRIBE the hub could not process (RFC-033: usually more wishes than
    // max_subscriptions_per_frame). Every other NACK the link surfaces via
    // the events ring alone; this one is loud enough to earn a standing
    // link-level error, because it means the batching logic above regressed,
    // not that the operator did anything wrong.
    if (n.code === NACK.SUBSCRIBE_REJECTED) {
      machine.link.error = 'SUBSCRIBE_REJECTED' + (n.detail ? ': ' + n.detail : '') + ' — client bug, see machine.svelte.js';
    }
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
