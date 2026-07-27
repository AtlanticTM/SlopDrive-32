/**
 * session.js — SlopSync browser session state machine (read + write plane).
 *
 * Owns ONE WebSocket to ws://<host>:82 (subprotocol slopsync.v1, binary), and
 * drives the full v1.0 session choreography that the reference clients
 * (lib/slopsync/.../client/client_impl.hpp, tools/slopsync_probe.py) implement:
 *
 *   connect → HELLO [+ cached catalog etag] → WELCOME (session id, roles,
 *             deadman, grants, hub identity, catalog etag)
 *   → READINESS GATE (§8.4 / RFC-015):
 *       etag MATCHED  ⇒ ready already, ZERO extra frames (the 99% reconnect)
 *       otherwise     ⇒ BLOB_REQ → BLOB_CHUNK reassembly → verify the SHA-256
 *                       LOCALLY → CATALOG_READY carrying the etag we now
 *                       operate against, re-declared on the chunk-repair
 *                       cadence until the first STATE proves the gate opened
 *   → CLOCK 0x05 NTP-style offset sync
 *   → SUBSCRIBE → GRANT (rate = min(wish, catalog rate))
 *   → retained STATE on grant, live STATE pushes (packed-decoded per the
 *     CATALOG'S layout — there is no fallback table, by design)
 *   → PING keepalive on TX silence (< deadman window)
 *   → intents (c2h) encoded from the catalog's own schema, correlated to their
 *     post-clamp ECHO 0x0E and to NACKs by intent_id / intent_seq
 *   → NACK handling, GOODBYE, reconnect-with-backoff.
 *
 * THE GATE IS WHY THERE IS NO FALLBACK LAYOUT TABLE. The hub emits NO
 * data-plane frame and NACKs every INTENT `NOT_READY` until this client has
 * declared which catalog it operates against, so a STATE frame can no longer
 * arrive before its decoder ring. See catalog.js's header.
 *
 * GROUND TRUTH (CLAUDE.md §3): this layer never fabricates device state. STATE
 * events carry the device's reported values; ECHO carries post-clamp APPLIED
 * values. Intents are ONLY sent when the integrator calls a sender — nothing
 * here auto-commands motion.
 *
 * Events (register with .on(name, cb)):
 *   'open'         ()                              — socket connected
 *   'welcome'      (welcomeInfo)                   — handshake complete
 *   'catalog'      (entries, channelMap, meta)     — catalog adopted (cached or fetched)
 *   'ready'        ({etag, cached})                — readiness declared/inherited
 *   'live'         ()                              — SYNCING → LIVE (§2.2)
 *   'grant'        (grants[])                      — SUBSCRIBE grants applied
 *   'state'        (channelId, decodedSample, tsMs)— a STATE push (packed-decoded)
 *   'echo'         (decodedEcho)                    — intent APPLIED echo
 *   'nack'         ({code, name, channel, detail, intentId, intentSeq})
 *   'event'        (evt)                            — an EVENT frame, `body` decoded
 *   'sessionEvent' (evt)                            — join/leave/takeover/goodbye
 *   'clock'        ({offsetUs, rttUs})              — a CLOCK sync result
 *   'close'        ({code, reason, willReconnect})  — socket closed
 */

import {
  cbUint, cbInt, cbBool, cbF32, cbTstr, cbBstr, cbMap, cbArray, cbDecodeFull,
} from './cbor.js';
import {
  FRAME, FRAME_NAME, K, IDENTITY_K, WELCOME_LIMITS_K, PRIORITY, WS_SUBPROTOCOL,
  PROTO_VER, LIMITS, nackName, GOODBYE_CODE,
  CBOR_FIELD, CHANNEL_CLASS, SAFETY_OP, SAFETY_CAUSE,
  CH_SAFETY, CH_SAFETY_INTENTS,
  CH_MOVE, CH_CONFIG_SET, CH_PATTERN_CMD, CH_MODES_SET, CH_HOME,
  encodeFrame, parseFrames, encodeEstopFrame, ESTOP_FRAME_BYTES,
} from './frames.js';
import {
  buildCatalogRequest, buildCatalogRepair, BlobReassembler, parseBlobChunk,
  decodeCatalog, catalogChannelMap, decodePacked, decodeEventBody, schemaByKey,
  optionAccessFor, canUseOption,
} from './catalog.js';
import { catalogEtag, bytesEqual, toHex, fromHex } from './sha256.js';

const DEFAULT_BACKOFF_MS = [500, 1000, 2000, 5000];

/** Session states, mirroring ClientSessionState in client/client.hpp (§2.2). */
export const SESSION_STATE = {
  CLOSED: 'CLOSED',
  HELLO_SENT: 'HELLO_SENT',
  SYNCING: 'SYNCING',
  LIVE: 'LIVE',
};

/** monotonic microsecond clock, truncated to u32 (CLOCK t0/t3 domain, §7.1). */
function clientNowUs() {
  const now = (typeof performance !== 'undefined' ? performance.now() : Date.now()) * 1000;
  return Math.floor(now) >>> 0;
}

/** windowed signed u32 difference a-b (util/serial_arithmetic.hpp timeDelta). */
function wrapDiff(a, b) {
  let d = (a - b) >>> 0;
  if (d >= 0x80000000) d -= 0x100000000;
  return d;
}

/** 8 random bytes for instance_id (§6.1). */
function newInstanceId() {
  const b = new Uint8Array(LIMITS.instance_id_bytes);
  if (typeof crypto !== 'undefined' && crypto.getRandomValues) crypto.getRandomValues(b);
  else for (let i = 0; i < b.length; i++) b[i] = (Math.random() * 256) | 0;
  return b;
}

// ---- catalog cache ---------------------------------------------------------
// The whole point of the etag: a client that already HOLDS the catalog proves
// possession in its HELLO and the hub opens the data plane with zero extra
// frames. That requires caching the BYTES, not just the tag — the tag alone
// would let us claim readiness for a catalog we cannot decode.
//
// Backed by localStorage in a browser; a module-level Map otherwise (node
// tests, workers), which is also what makes the second of two back-to-back
// test sessions exercise the fast path.
const _memCatalogCache = new Map();

function defaultCatalogStore() {
  const ls = (() => {
    try {
      if (typeof localStorage === 'undefined') return null;
      // Touch it: node defines the global but THROWS on access without
      // --localstorage-file, and a browser in private mode can too. A probe
      // read is the only honest availability test.
      localStorage.getItem('slopsync.probe');
      return localStorage;
    } catch (e) { return null; }
  })();
  const keyFor = (host) => 'slopsync.catalog.' + host;
  return {
    load(host) {
      if (ls) {
        try {
          const raw = ls.getItem(keyFor(host));
          if (raw) {
            const o = JSON.parse(raw);
            return { etag: fromHex(o.etag), bytes: fromHex(o.bytes) };
          }
        } catch (e) { /* corrupt entry: treat as absent */ }
        return null;
      }
      return _memCatalogCache.get(host) || null;
    },
    save(host, etag, bytes) {
      if (ls) {
        try {
          ls.setItem(keyFor(host), JSON.stringify({ etag: toHex(etag), bytes: toHex(bytes) }));
        } catch (e) { /* quota/private mode: caching is an optimization, never a requirement */ }
        return;
      }
      _memCatalogCache.set(host, { etag, bytes });
    },
    clear(host) {
      if (ls) { try { ls.removeItem(keyFor(host)); } catch (e) { /* ignore */ } return; }
      _memCatalogCache.delete(host);
    },
  };
}

/**
 * Create a SlopSync session.
 * @param {Object} opts
 * @param {string} opts.host device host/IP (default 192.168.1.229)
 * @param {number} [opts.port] hub WS port (default 82)
 * @param {string} [opts.clientKind] HELLO client_kind (default 'webui')
 * @param {string} [opts.clientName] HELLO client_name
 * @param {Uint8Array} [opts.instanceId] stable 8-byte identity (default random)
 * @param {Uint8Array} [opts.token] 16-byte pairing token (absent = watch)
 * @param {Array<[number, number, number]>} [opts.subscriptions] [ch, rateHz, priority] wishes to
 *        (re)issue automatically after each WELCOME
 * @param {boolean} [opts.autoCatalog] fetch the catalog when the etag misses (default true)
 * @param {boolean} [opts.autoReconnect] reconnect with backoff on drop (default true)
 * @param {Object} [opts.catalogStore] {load(host), save(host, etag, bytes), clear(host)}
 * @param {Function} [opts.WebSocketImpl] WebSocket constructor (default global WebSocket)
 * @param {Function} [opts.log] optional (level, ...args) logger
 * @returns {Object} session handle
 */
export function createSession(opts = {}) {
  const host = opts.host || '192.168.1.229';
  const port = opts.port || 82;
  const clientKind = opts.clientKind || 'webui';
  const clientName = opts.clientName || 'slopsync-js';
  const instanceId = opts.instanceId || newInstanceId();
  // `token` may be BYTES or a PROVIDER. The provider form exists because the
  // device's /uitoken credential is single-use and short-lived: bytes captured
  // once at construction authorise the first connect and silently demote every
  // reconnect to `watch`, which presents as "connected, but every control is
  // dead" after any blip. A provider is re-asked per connect, so a fresh
  // credential is fetched exactly when one is needed. May return a promise.
  const tokenProvider = typeof opts.token === 'function' ? opts.token : null;
  let liveToken = tokenProvider ? null : (opts.token || null);
  const autoCatalog = opts.autoCatalog !== false;
  const autoReconnect = opts.autoReconnect !== false;
  const catalogStore = opts.catalogStore || defaultCatalogStore();
  const WSImpl = opts.WebSocketImpl || (typeof WebSocket !== 'undefined' ? WebSocket : null);
  const log = opts.log || (() => {});
  const subscribeWishes = opts.subscriptions || [];

  // ---- listener registry --------------------------------------------------
  const listeners = new Map();
  function on(name, cb) {
    if (!listeners.has(name)) listeners.set(name, new Set());
    listeners.get(name).add(cb);
    return () => off(name, cb);
  }
  function off(name, cb) {
    const s = listeners.get(name);
    if (s) s.delete(cb);
  }
  function emit(name, ...args) {
    const s = listeners.get(name);
    if (s) for (const cb of s) { try { cb(...args); } catch (e) { log('error', 'listener', name, e); } }
  }

  // ---- session state ------------------------------------------------------
  let ws = null;
  let intentionalClose = false;
  let backoffIdx = 0;
  let reconnectTimer = null;

  const state = {
    phase: SESSION_STATE.CLOSED,
    connected: false,
    welcomed: false,
    sessionId: null,
    bootId: null,
    cfgGen: null,
    catalogEtag: null, // the HUB's etag from WELCOME
    readyEtag: null, // what WE declared we operate against
    ready: false, // RFC-015: is our data plane open?
    identity: null, // {product, fw_version, hub_name, info}
    roles: null,
    deadmanMs: LIMITS.deadman_default_ms,
    deadmanPolicy: null,
    limits: {},
    grants: new Map(), // channelId -> {rate, priority}
    grantedPublishes: new Map(),
    clockOffsetUs: 0,
  };

  let catalogEntries = null;
  let channelMap = null; // Map<id, entry>
  let catalogBytes = null;
  const blob = new BlobReassembler();

  // RFC-015 readiness bookkeeping (mirrors Client::pumpCatalogReady)
  let readyPending = false;
  let readyAttempts = 0;
  let lastReadySendMs = 0;
  const READY_MAX_ATTEMPTS =
    Math.floor(LIMITS.catalog_ready_timeout_ms / LIMITS.catalog_chunk_gap_timeout_ms);

  // §6.7 snapshot adoption: LIVE once the catalog is adopted and the retained
  // pushes WELCOME promised have landed.
  let requiredRetained = 0;
  let adoptedChannels = new Set();

  // pending intents awaiting ECHO/NACK correlation (by intent_id)
  let intentSeq = 0; // session-scoped u16 idempotency id (§9.3), starts at 1
  const pending = new Map(); // intent_id -> {channelId, resolve, reject, timer}

  // §11.2 repeat-until-latch for the RAW estop path
  let estopActive = false;
  let estopSentSeq = 0;
  let estopNextSeq = 1;
  let estopAttempts = 0;
  let lastEstopSendMs = 0;

  // TX-silence PING keepalive (§6.5)
  let lastTxMs = 0;
  let pumpTimer = null;
  let pendingClock = null; // {t0, resolve}

  function pingIntervalMs() {
    // stay well under the deadman window; ~60% of it, clamped to [150, 400]ms
    return Math.max(150, Math.min(400, Math.floor(state.deadmanMs * 0.6)));
  }

  function setPhase(p) {
    if (state.phase === p) return;
    state.phase = p;
    if (p === SESSION_STATE.LIVE) emit('live');
  }

  // ---- low-level send -----------------------------------------------------
  function sendFrame(type, channel, payload, seq = 0, flags = 0) {
    if (!ws || ws.readyState !== 1) return false;
    ws.send(encodeFrame(type, channel, payload || new Uint8Array(0), seq, flags));
    lastTxMs = Date.now();
    return true;
  }

  function sendRaw(bytes) {
    if (!ws || ws.readyState !== 1) return false;
    ws.send(bytes);
    lastTxMs = Date.now();
    return true;
  }

  // ---- HELLO --------------------------------------------------------------
  let cachedCatalog = null; // {etag, bytes} loaded at connect()

  function buildHello() {
    // keys ascending: proto_ver(1) < client_kind(2) < client_name(3) <
    // instance_id(4) < [token(5)] < [catalog_etag(8)]. No publish wishes (11):
    // the browser SUBSCRIBEs h2c channels, it does not publish a stream.
    //
    // catalog_etag is RFC-015's fast path: presenting an etag the hub agrees
    // with IS proof of possession, so the hub marks us ready in WELCOME and
    // pushes retained state immediately — no fetch, no CATALOG_READY, no
    // round trip. This is the 99% reconnect case and it stays zero-latency.
    const pairs = [
      [K.proto_ver, cbUint(PROTO_VER)],
      [K.client_kind, cbTstr(clientKind)],
      [K.client_name, cbTstr(clientName)],
      [K.instance_id, cbBstr(instanceId)],
    ];
    if (liveToken) pairs.push([K.token, cbBstr(liveToken)]);
    if (cachedCatalog) pairs.push([K.catalog_etag, cbBstr(cachedCatalog.etag)]);
    return cbMap(pairs);
  }

  // ---- SUBSCRIBE ----------------------------------------------------------
  /**
   * @param {Array<[number, number, number]>} wishes [channelId, rateHz, priority]
   */
  function subscribe(wishes) {
    // {10: [{12:rate_hz, 13:priority, 15:channel_id}]} — keys ascending 12<13<15.
    const entries = wishes.map(([ch, rate, prio]) =>
      cbMap([
        [K.rate_hz, cbF32(rate)],
        [K.priority, cbUint(prio == null ? PRIORITY.normal : prio)],
        [K.channel_id, cbUint(ch)],
      ]));
    sendFrame(FRAME.SUBSCRIBE, 0, cbMap([[K.subscriptions, cbArray(entries)]]));
  }

  // ---- CATALOG (BLOB namespace 0) -----------------------------------------
  function requestCatalog() {
    blob.reset();
    sendFrame(FRAME.BLOB_REQ, 0, buildCatalogRequest());
  }

  /** Adopt decoded catalog bytes as this session's decoder ring. */
  function adoptCatalog(bytes, { cached, verified, etag }) {
    catalogEntries = decodeCatalog(bytes);
    channelMap = catalogChannelMap(catalogEntries);
    catalogBytes = bytes;
    emit('catalog', catalogEntries, channelMap, { cached, verified, etag });
  }

  /**
   * §8.4/RFC-015: the hash IS the acknowledgement. Declare which catalog we now
   * operate against so the hub opens our data plane. Mirrors
   * Client::sendCatalogReady, including the honest-degraded case: on a transfer
   * that did NOT verify we declare the digest of the bytes we ACTUALLY hold, so
   * the hub can flag the session rather than be misled.
   */
  function sendCatalogReady(etag) {
    state.readyEtag = etag;
    if (sendFrame(FRAME.CATALOG_READY, 0, etag)) {
      readyPending = true;
      lastReadySendMs = Date.now();
      readyAttempts++;
      emit('ready', { etag, cached: false });
    }
  }

  function pumpCatalogReady(nowMs) {
    if (!readyPending) return;
    // Bounded by the hub's own catalog_ready_timeout_ms: past that the hub has
    // already GOODBYE'd us (READY_TIMEOUT), so re-declaring is pure noise.
    if (readyAttempts >= READY_MAX_ATTEMPTS) { readyPending = false; return; }
    if (nowMs - lastReadySendMs < LIMITS.catalog_chunk_gap_timeout_ms) return;
    // Idempotent on the hub side by design — re-declaring is a flag-set, so a
    // lossy binding costs nothing but 16 bytes per repair interval.
    sendFrame(FRAME.CATALOG_READY, 0, state.readyEtag);
    lastReadySendMs = nowMs;
    readyAttempts++;
  }

  function pumpBlobRepair(nowMs) {
    if (!blob.active || blob.complete()) return;
    if (blob.timedOut(nowMs)) { requestCatalog(); return; } // abandon → restart from scratch
    if (!blob.gapElapsed(nowMs)) return;
    const missing = blob.missingIndices();
    if (missing.length) {
      sendFrame(FRAME.BLOB_REQ, 0, buildCatalogRepair(missing.slice(0, 32)));
      blob.noteRepairSent(nowMs); // one repair per gap interval
    }
  }

  function handleBlobChunk(payload) {
    const h = parseBlobChunk(payload);
    if (!h) return;
    // This session transfers exactly one namespace: the catalog. A store chunk
    // belongs to that fetch's own reassembler; dropping it here is what keeps
    // the two from corrupting each other.
    if (h.ns !== 0) return;
    if (h.chunkCount === 0 || h.chunkIndex >= h.chunkCount) return;

    const now = Date.now();
    if (!blob.active || blob.chunkCount !== h.chunkCount || blob.totalBytes !== h.totalBytes) {
      if (!blob.begin(h, now)) return; // declared size refused: RFC-028
    }
    if (!blob.insert(h, now)) return;
    if (!blob.complete()) return;

    const bytes = blob.assembled().slice();
    blob.reset();

    const digest = catalogEtag(bytes, LIMITS.etag_bytes);
    const verified = !!state.catalogEtag && bytesEqual(digest, state.catalogEtag);
    try {
      adoptCatalog(bytes, { cached: false, verified, etag: verified ? state.catalogEtag : digest });
    } catch (e) {
      log('error', 'catalog decode failed', e);
      return; // undecodable: do NOT declare readiness for something we cannot read
    }
    // Only cache what actually verified (a mismatch means we hold something the
    // hub did not send; caching it would poison every later fast path).
    if (verified) catalogStore.save(host, state.catalogEtag, bytes);
    sendCatalogReady(verified ? state.catalogEtag : digest);
    checkLiveTransition();
  }

  // ---- CLOCK sync (§7.1) --------------------------------------------------
  /**
   * Fire one CLOCK exchange. Resolves {offsetUs, rttUs} or null on timeout.
   * @param {number} [timeoutMs]
   * @returns {Promise<{offsetUs:number, rttUs:number}|null>}
   */
  function syncClock(timeoutMs = 2000) {
    return new Promise((resolve) => {
      const t0 = clientNowUs();
      const buf = new Uint8Array(4);
      new DataView(buf.buffer).setUint32(0, t0, true);
      let done = false;
      const timer = setTimeout(() => { if (!done) { done = true; pendingClock = null; resolve(null); } }, timeoutMs);
      pendingClock = {
        t0,
        resolve: (reply) => {
          if (done) return;
          done = true;
          clearTimeout(timer);
          pendingClock = null;
          const t3 = clientNowUs();
          const offset = Math.floor((wrapDiff(reply.t1, reply.t0e) + wrapDiff(reply.t2, t3)) / 2);
          const rtt = wrapDiff(t3, reply.t0e) - wrapDiff(reply.t2, reply.t1);
          state.clockOffsetUs = offset;
          emit('clock', { offsetUs: offset, rttUs: rtt });
          resolve({ offsetUs: offset, rttUs: rtt });
        },
      };
      sendFrame(FRAME.CLOCK, 0, buf);
    });
  }

  /** current hub-time estimate in µs (u32). */
  function hubNowUs() {
    return (clientNowUs() + state.clockOffsetUs) >>> 0;
  }

  // ---- INTENT senders (c2h) — ONLY sent when the integrator calls them ----

  /**
   * Encode one intent field FROM THE CATALOG's declared CBOR type. There is no
   * hand-copied schema table: the device publishes a per-key type on every
   * INTENT channel and this reads it. (The table that used to live here had
   * drifted — it encoded a float as an integer for months.)
   */
  function encodeIntentValue(field, v, channelId, key) {
    if (!field) {
      throw new Error('sendIntent: channel 0x' + channelId.toString(16) +
        ' declares no schema field for key ' + key + ' (catalog is authoritative)');
    }
    switch (field.type) {
      case CBOR_FIELD.uint_t: return cbUint(v);
      case CBOR_FIELD.int_t: return cbInt(v);
      case CBOR_FIELD.f32_t: return cbF32(v);
      case CBOR_FIELD.bool_t: return cbBool(!!v);
      case CBOR_FIELD.tstr_t: return cbTstr(String(v));
      case CBOR_FIELD.bstr_t: return cbBstr(v);
      default:
        throw new Error('sendIntent: unknown CBOR field type ' + field.type + ' for key ' + key);
    }
  }

  /**
   * Send an INTENT and resolve when its post-clamp ECHO (or a NACK) returns.
   *
   * REQUIRES LIVE, mirroring Client::sendIntent's `_state != LIVE -> nullopt`:
   * a pre-READY intent is refused NOT_READY by the hub anyway (§11.5(2) — a
   * client that has not adopted the retained safety latch must not act), so
   * refusing it here is the same answer without the wire noise.
   *
   * @param {number} channelId an INTENT channel from the catalog
   * @param {Object<number, (number|boolean|string)>} valueMap field key -> value
   * @param {Object} [o] {timeoutMs, precondition (cfg_gen CAS), takeover}
   * @returns {Promise<{applied:Object, intentId:number, cfgGen:number}>}
   */
  function sendIntent(channelId, valueMap, o = {}) {
    if (state.phase !== SESSION_STATE.LIVE) {
      return Promise.reject(new Error('sendIntent: session is ' + state.phase +
        ', not LIVE — the hub would refuse this NOT_READY (§11.5(2))'));
    }
    const entry = channelMap && channelMap.get(channelId);
    if (!entry) {
      return Promise.reject(new Error('sendIntent: channel 0x' + channelId.toString(16) +
        ' is not in this hub\'s catalog'));
    }
    if (entry.cls !== CHANNEL_CLASS.INTENT) {
      return Promise.reject(new Error('sendIntent: channel 0x' + channelId.toString(16) +
        ' is ' + entry.clsName + ', not INTENT'));
    }

    intentSeq = (intentSeq % 0xffff) + 1; // 1..65535, session-scoped (§9.3)
    const intentId = intentSeq;

    let valuePairs;
    try {
      const byKey = schemaByKey(entry);
      const keys = Object.keys(valueMap).map(Number).sort((a, b) => a - b);
      valuePairs = keys.map((key) =>
        [key, encodeIntentValue(byKey.get(key), valueMap[key], channelId, key)]);
    } catch (e) {
      return Promise.reject(e);
    }

    // intent map: keys ascending — channel_id(15) < intent_id(18) < value(20)
    // [< precondition(30)] [< takeover(32)].
    const intentPairs = [
      [K.channel_id, cbUint(channelId)],
      [K.intent_id, cbUint(intentId)],
      [K.value, cbMap(valuePairs)],
    ];
    if (o.precondition != null) intentPairs.push([K.precondition, cbUint(o.precondition)]);
    if (o.takeover != null) intentPairs.push([K.takeover, cbBool(!!o.takeover)]);

    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        pending.delete(intentId);
        reject(new Error('intent ' + intentId + ' timeout (no ECHO/NACK)'));
      }, o.timeoutMs || 3000);
      pending.set(intentId, { channelId, resolve, reject, timer });
      // header.channel = target channel id for INTENT (redundant-but-authoritative);
      // header.seq = the intent id, so RFC-001's NACK `intent_seq` (the seq of
      // the frame being refused) correlates to the SAME number `intent_id` does.
      // A NACK that carries only one of the two still lands on the right promise.
      const okSent = sendFrame(FRAME.INTENT, channelId, cbMap(intentPairs), intentId);
      if (!okSent) {
        clearTimeout(timer);
        pending.delete(intentId);
        reject(new Error('intent ' + intentId + ' not sent (socket not open)'));
      }
    });
  }

  // convenience wrappers (all still explicit calls — nothing auto-fires)
  const sendMove = (positionMm, bypass = false, o) => sendIntent(CH_MOVE, { 1: positionMm, 2: bypass }, o);
  const sendConfigSet = (fields, o) => sendIntent(CH_CONFIG_SET, fields, o); // {1:window_min,...}
  const sendPatternCmd = (fields, o) => sendIntent(CH_PATTERN_CMD, fields, o);
  // M5b 0x0104 {1:blend_mode, 2:transport, 3:stream_speed_mode, 4:overshoot_clamp}.
  // Every key optional; the ECHO carries what the machine ACTUALLY took, which
  // for these is re-read post-apply rather than assumed (the hub clamps blend
  // and may refuse a transport this build cannot enter).
  const sendModesSet = (fields, o) => sendIntent(CH_MODES_SET, fields, o);
  const sendHome = (op = 1, o) => sendIntent(CH_HOME, { 1: op }, o);
  const sendSafetyIntent = (op, o) => sendIntent(CH_SAFETY_INTENTS, { 1: op }, o);

  /**
   * ASSERT E-STOP (RFC-010). `safety_ops::estop` (6) on 0x0005: the hub treats
   * it exactly as a valid 0xE5 frame — latch, cause=user, publish 0x0003 — so
   * this is a real e-stop and not the decel-stop the red button used to
   * silently degrade to.
   *
   * PRE-LIVE, IT FALLS BACK TO THE RAW 0xE5 FRAME, deliberately. The op rides
   * the INTENT plane, which the readiness gate holds shut; the raw frame is
   * matched by the hub BEFORE header decode and before any readiness or
   * occupancy gate (hub_impl.hpp::pumpSlot), so "the person in the room can
   * always stop the machine" survives a session that is still SYNCING. The raw
   * path repeats until the safety snapshot shows the latch (§11.2).
   *
   * @returns {Promise<Object>} resolves on ECHO (op path) or immediately (raw path)
   */
  function assertEstop() {
    if (state.phase === SESSION_STATE.LIVE) {
      return sendSafetyIntent(SAFETY_OP.estop).catch((e) => {
        // The op was refused/lost — do NOT let a red button end in a rejected
        // promise and nothing else. Escalate to the raw plane.
        assertEstopRaw();
        throw e;
      });
    }
    assertEstopRaw();
    return Promise.resolve({ raw: true });
  }

  /** §5.5/§11.2: the raw 12-byte ESTOP frame, repeated until the latch is observed. */
  function assertEstopRaw(cause = SAFETY_CAUSE.user) {
    estopActive = true;
    estopSentSeq = estopNextSeq++;
    if (estopNextSeq === 0) estopNextSeq = 1;
    estopAttempts = 1;
    lastEstopSendMs = Date.now();
    sendRaw(encodeEstopFrame(cause, state.roles || 0, estopSentSeq));
  }

  function pumpEstopRepeat(nowMs) {
    if (!estopActive) return;
    if (estopAttempts >= 20) { estopActive = false; return; } // limits::estop_repeat_max
    if (nowMs - lastEstopSendMs < 50) return; // limits::estop_repeat_interval_ms
    sendRaw(encodeEstopFrame(SAFETY_CAUSE.user, state.roles || 0, estopSentSeq));
    estopAttempts++;
    lastEstopSendMs = nowMs;
  }

  // ---- STATE decode dispatch ---------------------------------------------
  function layoutFor(channelId) {
    if (!channelMap) return null;
    const e = channelMap.get(channelId);
    return (e && e.layout) ? e.layout : null;
  }

  // ---- frame dispatch -----------------------------------------------------
  function handleWelcome(payload) {
    const w = cbDecodeFull(payload);
    state.welcomed = true;
    state.sessionId = w.get(K.session_id);
    state.bootId = w.get(K.boot_id);
    state.cfgGen = w.get(K.cfg_gen);
    state.catalogEtag = w.get(K.catalog_etag) || null;
    state.roles = w.get(K.roles);
    state.deadmanMs = w.get(K.deadman_ms) ?? LIMITS.deadman_default_ms;
    state.deadmanPolicy = w.get(K.deadman_policy);
    const lim = w.get(K.limits);
    state.limits = {};
    if (lim instanceof Map) for (const [name, key] of Object.entries(WELCOME_LIMITS_K)) state.limits[name] = lim.get(key);

    // RFC-016: hub identity in band. `fw_version` is why a client no longer has
    // to label a device "boot 0x…" from an mDNS TXT record.
    const idm = w.get(K.identity);
    state.identity = null;
    if (idm instanceof Map) {
      state.identity = {
        product: idm.get(IDENTITY_K.product) || null,
        fw_version: idm.get(IDENTITY_K.fw_version) || null,
        hub_name: idm.get(IDENTITY_K.hub_name) || null,
        info: idm.get(IDENTITY_K.info) || null,
      };
    }

    // §6.7: snapshot adoption is mandatory — discard everything and rebuild it
    // only from what follows this WELCOME.
    state.grants.clear();
    adoptedChannels = new Set();
    requiredRetained = state.limits.retained_pending || 0;
    for (const g of (w.get(K.grants) || [])) {
      const ch = g.get(K.channel_id);
      state.grants.set(ch, { channel: ch, rate: g.get(K.granted_rate_hz), priority: g.get(K.priority) });
    }

    const info = {
      sessionId: state.sessionId,
      bootId: state.bootId,
      cfgGen: state.cfgGen,
      catalogEtag: state.catalogEtag,
      identity: state.identity,
      roles: state.roles,
      deadmanMs: state.deadmanMs,
      deadmanPolicy: state.deadmanPolicy,
      limits: state.limits,
    };
    setPhase(SESSION_STATE.SYNCING);
    emit('welcome', info);

    // ---- RFC-015 readiness gate -------------------------------------------
    readyPending = false;
    readyAttempts = 0;
    blob.reset();
    const matched = !!(cachedCatalog && state.catalogEtag &&
      bytesEqual(cachedCatalog.etag, state.catalogEtag));
    if (matched) {
      // Proof of possession one round trip earlier: the hub already flipped our
      // ready bit when it read the HELLO etag, so retained STATE is already on
      // its way. NO CATALOG_READY frame — the zero-latency reconnect path.
      state.ready = true;
      state.readyEtag = cachedCatalog.etag;
      try {
        adoptCatalog(cachedCatalog.bytes, { cached: true, verified: true, etag: cachedCatalog.etag });
      } catch (e) {
        // A cached blob that no longer decodes is worse than none: drop it and
        // fetch, rather than operate against a decoder ring we cannot build.
        log('warn', 'cached catalog failed to decode — refetching', e);
        catalogStore.clear(host);
        cachedCatalog = null;
        state.ready = false;
        if (autoCatalog) requestCatalog();
      }
      if (state.ready) emit('ready', { etag: state.readyEtag, cached: true });
    } else {
      state.ready = false;
      if (cachedCatalog) catalogStore.clear(host); // stale firmware/catalog: forget it
      cachedCatalog = null;
      if (autoCatalog) requestCatalog();
    }

    // start the deadman-defeating PING/pump loop now that we know the window
    startPump();

    // read-plane bring-up (all safe, no intents): CLOCK, SUBSCRIBE.
    syncClock().catch(() => {});
    if (subscribeWishes.length) subscribe(subscribeWishes);

    checkLiveTransition();
  }

  function checkLiveTransition() {
    if (state.phase !== SESSION_STATE.SYNCING) return;
    if (!catalogEntries) return;
    if (adoptedChannels.size < requiredRetained) return;
    setPhase(SESSION_STATE.LIVE);
  }

  function handleGrant(payload) {
    const g = cbDecodeFull(payload);
    const arr = g.get(K.grants) || [];
    const applied = [];
    for (const entry of arr) {
      const ch = entry.get(K.channel_id);
      const rec = { channel: ch, rate: entry.get(K.granted_rate_hz), priority: entry.get(K.priority) };
      state.grants.set(ch, rec);
      applied.push(rec);
    }
    // The hub may re-issue `roles` (an AUTH upgrade); adopt it as ground truth.
    if (g.has(K.roles)) state.roles = g.get(K.roles);
    emit('grant', applied);
  }

  function handleState(header, payload) {
    // §8.4/RFC-015: STATE arriving is proof the hub opened our data plane, so
    // the CATALOG_READY re-declaration loop stops here.
    readyPending = false;
    state.ready = true;

    const layout = layoutFor(header.channel);
    let decoded;
    if (layout) {
      try { decoded = decodePacked(payload, layout); }
      catch (e) { log('warn', 'STATE decode failed ch=0x' + header.channel.toString(16), e); decoded = { _raw: payload }; }
    } else {
      // With the readiness gate this means an id the catalog does not describe
      // — pass the bytes through rather than guess a layout.
      decoded = { _raw: payload };
    }

    // §11.2 repeat-until-latched: the safety channel's own estop_seq field is
    // the acknowledgement assertEstopRaw() is waiting for.
    if (header.channel === CH_SAFETY && estopActive && decoded && decoded.word_bits) {
      if (decoded.word_bits.estop) { estopActive = false; }
    }

    if (!adoptedChannels.has(header.channel)) {
      adoptedChannels.add(header.channel);
      checkLiveTransition();
    }
    emit('state', header.channel, decoded, Date.now());
  }

  function handleEcho(payload) {
    const e = cbDecodeFull(payload);
    const intentId = e.get(K.intent_id);
    const appliedMap = e.get(K.applied);
    const applied = {};
    if (appliedMap instanceof Map) for (const [k, v] of appliedMap) applied[k] = v;
    const cfgGen = e.get(K.cfg_gen);
    if (cfgGen != null) state.cfgGen = cfgGen;
    const result = { intentId, cfgGen, applied, rebootInMs: applied[K.reboot_in_ms] };
    emit('echo', result);
    const p = pending.get(intentId);
    if (p) { clearTimeout(p.timer); pending.delete(intentId); p.resolve(result); }
  }

  function handleNack(header, payload) {
    const n = cbDecodeFull(payload);
    const code = n.get(K.code);
    const ch = n.has(K.channel_id) ? n.get(K.channel_id) : header.channel;
    const info = {
      code,
      name: nackName(code),
      channel: ch,
      detail: n.get(K.detail) || null,
      intentId: n.has(K.intent_id) ? n.get(K.intent_id) : null,
      intentSeq: n.has(K.intent_seq) ? n.get(K.intent_seq) : null,
      retryAfterMs: n.has(K.retry_after_ms) ? n.get(K.retry_after_ms) : null,
    };
    emit('nack', info);

    // ---- correlation, best evidence first ---------------------------------
    // 1) intent_id (18): the hub sets it whenever a decodable INTENT provoked
    //    the refusal — exact by construction.
    // 2) intent_seq (41, RFC-001): the header seq of the refused frame. We send
    //    intents with seq == intent_id, so this resolves the same promise; it
    //    also covers refusals raised before `intent_id` could be read.
    // 3) FALLBACK, documented and imprecise: the oldest pending intent on this
    //    channel. Correct for a one-at-a-time UI, wrong the moment anyone
    //    pipelines — kept only for a hub that populates neither key.
    let id = null;
    if (info.intentId != null && pending.has(info.intentId)) id = info.intentId;
    else if (info.intentSeq != null && pending.has(info.intentSeq)) id = info.intentSeq;
    else {
      for (const [pid, p] of pending) { if (p.channelId === ch) { id = pid; break; } }
    }
    if (id != null && pending.has(id)) {
      const p = pending.get(id);
      clearTimeout(p.timer);
      pending.delete(id);
      p.reject(Object.assign(new Error('intent NACK ' + info.name), info));
    }
  }

  function handleEvent(header, payload) {
    let decoded = null;
    try { decoded = cbDecodeFull(payload); } catch (e) { log('warn', 'EVENT decode failed', e); return; }
    if (!(decoded instanceof Map)) return;
    const entry = channelMap ? channelMap.get(header.channel) : null;
    // v1.0: kind-specific fields ride the SCOPED `body` (40) sub-map, keyed by
    // the channel's OWN catalog schema — exactly as INTENT's `value` (20) is.
    // With them at the top level, every device-authored EVENT channel would
    // have needed a registry PR to name its own fields.
    const evt = {
      channel: header.channel,
      channelName: entry ? entry.name : null,
      kind: decoded.get(K.event_kind),
      seqOfState: decoded.has(K.seq_of_state) ? decoded.get(K.seq_of_state) : null,
      timestamp: decoded.has(K.timestamp) ? decoded.get(K.timestamp) : null,
      body: decodeEventBody(decoded.get(K.body), entry),
    };
    emit('event', evt);
    emit('sessionEvent', evt); // legacy listener name, same payload
  }

  function handleClock(payload) {
    if (payload.length < 12 || !pendingClock) return;
    const dv = new DataView(payload.buffer, payload.byteOffset, 12);
    pendingClock.resolve({ t0e: dv.getUint32(0, true), t1: dv.getUint32(4, true), t2: dv.getUint32(8, true) });
  }

  function dispatch(header, payload) {
    switch (header.type) {
      case FRAME.WELCOME: handleWelcome(payload); break;
      case FRAME.BLOB_CHUNK: handleBlobChunk(payload); break;
      case FRAME.GRANT: handleGrant(payload); break;
      case FRAME.STATE: handleState(header, payload); break;
      case FRAME.STREAM: /* device does not stream to us on read channels */ break;
      case FRAME.ECHO: handleEcho(payload); break;
      case FRAME.EVENT: handleEvent(header, payload); break;
      case FRAME.NACK: handleNack(header, payload); break;
      case FRAME.CLOCK: handleClock(payload); break;
      case FRAME.PING: sendFrame(FRAME.PONG, header.channel, payload); break; // §6.5 echo
      case FRAME.PONG: break;
      case FRAME.BEACON: break; // §13.7 discovery beacon: nothing to do on WS
      case FRAME.PAIR_GRANT: // pairing UI is out of scope for this milestone —
      case FRAME.HUB_SIG: // ditto hub-signature verification (bearer + unverified).
        log('debug', 'trust-plane frame ignored', FRAME_NAME[header.type]);
        break;
      case FRAME.GOODBYE: {
        let code = null;
        try { code = cbDecodeFull(payload).get(K.code); } catch (e) { /* empty/none */ }
        emit('sessionEvent', { channel: header.channel, kind: 'goodbye', code, codeName: nackName(code) });
        break;
      }
      default:
        log('debug', 'unhandled frame', FRAME_NAME[header.type] || header.type);
    }
  }

  // ---- periodic pump: PING keepalive + READY / repair / estop cadences -----
  function startPump() {
    stopPump();
    pumpTimer = setInterval(() => {
      if (!ws || ws.readyState !== 1) return;
      const now = Date.now();
      pumpCatalogReady(now);
      pumpBlobRepair(now);
      pumpEstopRepeat(now);
      if (now - lastTxMs >= pingIntervalMs()) sendFrame(FRAME.PING, 0, new Uint8Array(0));
    }, 50);
  }
  function stopPump() {
    if (pumpTimer) { clearInterval(pumpTimer); pumpTimer = null; }
  }

  // ---- socket lifecycle ---------------------------------------------------
  function connect() {
    if (!WSImpl) throw new Error('no WebSocket implementation available');
    intentionalClose = false;
    if (!tokenProvider) { openSocket(); return; }
    // Resolve the credential BEFORE the socket opens, not inside onopen. The
    // hub starts a HELLO timer the instant the socket is accepted, and an async
    // onopen would spend that budget on an HTTP round trip. Fetching first
    // costs nothing (we are not connected yet) and keeps HELLO immediate.
    //
    // A provider that throws or hangs must not strand the session, so failure
    // degrades to a tokenless HELLO — the hub answers `watch` and the UI comes
    // up as a viewer, which is a working state.
    Promise.resolve()
      .then(() => tokenProvider(host))
      .then((t) => { liveToken = t || null; })
      .catch((e) => { log('warn', 'token provider failed — connecting as viewer', e); liveToken = null; })
      .then(() => {
        if (intentionalClose) return; // closed while we were fetching
        openSocket();
      });
  }

  function openSocket() {
    cachedCatalog = null;
    try {
      const c = catalogStore.load(host);
      // Re-verify the cache before trusting it: the etag we are about to CLAIM
      // in HELLO must actually be the hash of the bytes we hold, or readiness
      // would be a lie.
      if (c && c.bytes && c.etag && bytesEqual(catalogEtag(c.bytes, LIMITS.etag_bytes), c.etag)) {
        cachedCatalog = c;
      } else if (c) {
        catalogStore.clear(host);
      }
    } catch (e) { log('warn', 'catalog cache unreadable', e); }

    const url = 'ws://' + host + ':' + port + '/';
    log('info', 'connecting', url);
    ws = new WSImpl(url, [WS_SUBPROTOCOL]);
    ws.binaryType = 'arraybuffer';

    ws.onopen = () => {
      state.connected = true;
      backoffIdx = 0;
      lastTxMs = Date.now();
      setPhase(SESSION_STATE.HELLO_SENT);
      emit('open');
      sendFrame(FRAME.HELLO, 0, buildHello());
    };
    ws.onmessage = (ev) => {
      const data = ev.data;
      if (!(data instanceof ArrayBuffer)) return; // §13.2: text is a protocol error — ignore (§4.3)
      const buf = new Uint8Array(data);
      // §5.5: the ESTOP frame is matched BEFORE header decode — it is
      // deliberately outside the 8-byte header discipline.
      if (buf.length === ESTOP_FRAME_BYTES &&
          buf[0] === 0xe5 && buf[1] === 0xe5 && buf[2] === 0xe5 && buf[3] === 0xe5) {
        emit('sessionEvent', { kind: 'estop', cause: buf[4], origin: buf[5] });
        return;
      }
      for (const { header, payload } of parseFrames(buf)) {
        try { dispatch(header, payload); }
        catch (e) { log('warn', 'dropping frame type=' + header.type, e); } // §4.3 tolerance
      }
    };
    ws.onerror = (e) => log('warn', 'ws error', e && e.message);
    ws.onclose = (ev) => {
      state.connected = false;
      state.welcomed = false;
      state.ready = false;
      setPhase(SESSION_STATE.CLOSED);
      stopPump();
      estopActive = false;
      // fail every in-flight intent (§6.7: the library NEVER blind-retransmits;
      // the app reconciles against the truth the next session reports).
      for (const [id, p] of pending) { clearTimeout(p.timer); p.reject(new Error('socket closed')); pending.delete(id); }
      const willReconnect = autoReconnect && !intentionalClose;
      emit('close', { code: ev && ev.code, reason: ev && ev.reason, willReconnect });
      if (willReconnect) scheduleReconnect();
    };
  }

  function scheduleReconnect() {
    if (reconnectTimer) return;
    const delay = DEFAULT_BACKOFF_MS[Math.min(backoffIdx, DEFAULT_BACKOFF_MS.length - 1)];
    backoffIdx++;
    log('info', 'reconnect in', delay, 'ms');
    reconnectTimer = setTimeout(() => { reconnectTimer = null; connect(); }, delay);
  }

  /**
   * Close the session. Sends a courtesy GOODBYE (§6.8) so the hub releases this
   * session's source ownership promptly (field bug #3 — every teardown path
   * runs the same §11.3 loss policy, but a clean one is instant).
   * @param {number} [code] a GOODBYE_CODE (drawn from nack_codes, RFC-022.2)
   */
  function close(code = GOODBYE_CODE.NORMAL_CLOSURE) {
    intentionalClose = true;
    if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
    stopPump();
    try { if (ws && ws.readyState === 1) sendFrame(FRAME.GOODBYE, 0, cbMap([[K.code, cbUint(code)]])); } catch (e) { /* gone */ }
    try { if (ws) ws.close(); } catch (e) { /* gone */ }
  }

  return {
    // lifecycle
    connect,
    close,
    // events
    on,
    off,
    // read-plane actions
    subscribe,
    requestCatalog,
    syncClock,
    hubNowUs,
    // write-plane (explicit; never auto-fired)
    sendIntent,
    sendMove,
    sendConfigSet,
    sendPatternCmd,
    sendModesSet,
    sendHome,
    sendSafetyIntent,
    assertEstop,
    assertEstopRaw,
    // catalog introspection (RFC-009 helpers, resolved against THIS hub)
    schemaFor: (channelId) => schemaByKey(channelMap && channelMap.get(channelId)),
    optionAccessFor: (channelId, key, value) =>
      optionAccessFor(channelMap && channelMap.get(channelId), key, value),
    /**
     * May this session use `value` on field `key` of `channelId`? RFC-009's
     * grey-never-hide input. NOTE there is deliberately NO channel-id special
     * case here for the role-exempt safety ops: 0x0005 advertises
     * `option_access` (catalog key 17) with `stop` and `estop` at `watch`, and
     * the hub gates on that SAME data (Hub::requiredAccessFor), so the client's
     * greying and the hub's enforcement cannot disagree. Hardcoding the
     * exemption here would reintroduce exactly the drift the catalog removes.
     */
    canUse: (channelId, key, value) =>
      canUseOption(channelMap && channelMap.get(channelId), key, value, state.roles || 0),
    // introspection
    get state() { return state; },
    get phase() { return state.phase; },
    get isLive() { return state.phase === SESSION_STATE.LIVE; },
    get catalog() { return catalogEntries; },
    get catalogBytes() { return catalogBytes; },
    get channelMap() { return channelMap; },
    get instanceId() { return instanceId; },
    get identity() { return state.identity; },
  };
}
