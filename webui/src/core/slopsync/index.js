/**
 * slopsync-js — browser-side SlopSync client core (wire + session layer).
 *
 * This is the THIRD reference client of the SlopSync protocol (after
 * tools/slopsync_probe.py and clients/mfp-slopsync/SlopSync.cs). It speaks the
 * device's native hub plane — ws://<host>:82, subprotocol 'slopsync.v1',
 * 8-byte LE frame header + deterministic CBOR (SPEC §5.3) — and is the
 * foundation the WebUI refactor (docs/REFACTOR-ROADMAP.md §5) wires its cards
 * onto, replacing the legacy :81 UiSocket plane.
 *
 * DISCIPLINE (CLAUDE.md §8): every wire number comes from the generated
 * registry constants (frames.js), byte layouts mirror the probe / C# client,
 * and the golden-byte test (webui/test/slopsync-wire.test.mjs) proves this
 * codec is byte-identical to them. Never invent bytes.
 *
 * GROUND TRUTH (CLAUDE.md §3): STATE events carry the device's REPORTED values;
 * ECHO carries post-clamp APPLIED values. Intents are only sent when the
 * integrator explicitly calls a sender — nothing here auto-commands motion.
 *
 * ── Integrator quick-start (Phase A, read plane) ────────────────────────────
 *
 *   import { createSession, CH } from './core/slopsync/index.js';
 *
 *   const s = createSession({
 *     host: location.hostname,           // device serves this bundle
 *     clientKind: 'webui',
 *     clientName: 'SlopDrive WebUI',
 *     subscriptions: [                    // [channelId, rateHz, priority]
 *       [CH.SAFETY,     0,  PRIORITY.critical],  // on-change, never shed
 *       [CH.MOTION,     20, PRIORITY.elevated],  // live carriage feed
 *       [CH.HUB_STATUS, 1,  PRIORITY.background],
 *     ],
 *   });
 *
 *   s.on('welcome', (w)   => { ... w.sessionId, w.roles, w.deadmanMs, w.cfgGen ... });
 *   s.on('catalog', (entries, map) => { ... build UI cards from the catalog ... });
 *   s.on('state', (channelId, sample, tsMs) => {
 *     // motion field names come from the catalog verbatim: pos_10um / tgt_10um
 *     // are scaled to mm, speed to mm/s, flags_bits is the decoded bitfield.
 *     if (channelId === CH.MOTION) render(sample.pos_10um, sample.tgt_10um, sample.flags_bits);
 *   });
 *   s.on('nack',  (n) => console.warn('NACK', n.name, n.detail));
 *   s.on('close', (c) => { ... c.willReconnect ... });
 *   s.connect();
 *
 * ── Phase B (write plane) ───────────────────────────────────────────────────
 * Intents resolve on their post-clamp ECHO (the ground-truth confirm the shadow
 * layer adopts) or reject on NACK/timeout:
 *
 *   const { applied, cfgGen } = await s.sendConfigSet({ 1: windowMinMm, 2: windowMaxMm });
 *   // applied[1] / applied[2] are the DEVICE's clamped values — render those.
 *   await s.sendMove(targetMm);            // 0x0100 move
 *   await s.sendHome(1);                    // 0x0103 home op 1
 *   await s.sendSafetyIntent(SAFETY_OP.stop);
 *   await s.assertEstop();   // RFC-010: a REAL e-stop, not a decel-stop —
 *                            // falls back to the raw 0xE5 frame pre-LIVE
 *
 * Nothing above fires until you call it; page load should ADOPT device state
 * from STATE/ECHO, never push defaults.
 */

export { createSession, SESSION_STATE } from './session.js';

// wire codec (for tests / advanced integrators)
export {
  cbUint, cbInt, cbBool, cbF32, cbTstr, cbBstr, cbArray, cbMap,
  cbDecode, cbDecodeFull, concatBytes,
} from './cbor.js';

// frame layer + all wire-number constants
export {
  FRAME, FRAME_NAME, K, IDENTITY_K, BLOB_K, BLOB_NS, TRUST_K, WELCOME_LIMITS_K,
  PRIORITY, ACCESS, ACCESS_NAME, CHANNEL_CLASS, CHANNEL_CLASS_NAME,
  PACKED, PACKED_NAME, PACKED_SIZE, CBOR_FIELD, NACK, NACK_NAME, GOODBYE_CODE,
  SAFETY_OP, SAFETY_OP_ROLE_EXEMPT, SAFETY_CAUSE, SAFETY_CAUSE_NAME, HOME_OP,
  SESSION_EVENT_KIND, SAFETY_EVENT_KIND, LOG_EVENT_KIND, LOG_LEVEL_NAME,
  SETTING_CATEGORY, SETTING_CATEGORY_NAME, SETTING_FLAG, STREAM_KIND,
  LIMITS, nackName,
  PROTO_VER, WS_SUBPROTOCOL, MDNS_SERVICE, HEADER_BYTES,
  encodeFrame, decodeFrameHeader, parseFrames, encodeEstopFrame, crc32,
  // channel ids
  CH_CATALOG, CH_SESSION_ROSTER, CH_SAFETY, CH_CONTROL_OWNER, CH_SAFETY_INTENTS,
  CH_HUB_STATUS, CH_SESSION_EVENTS, CH_LOG, CH_SESSION_ADMIN, CH_SAFETY_EVENTS,
  CH_MOTION, CH_MACHINE_CONFIG, CH_MACHINE_MODES, CH_PATTERN_STATE, CH_ODOMETER,
  CH_MOTION_INPUT, CH_MOTION_SEGMENT,
  CH_PLAN_STRIP, CH_POWER, CH_MOTION_DIAG, CH_MOTION_ANOMALY,
  CH_MOVE, CH_CONFIG_SET, CH_PATTERN_CMD, CH_HOME, CH_MODES_SET,
} from './frames.js';

// blob transfer + catalog decode + packed-STATE decode
export {
  buildBlobReq, buildCatalogRequest, buildCatalogRepair,
  BlobReassembler, parseBlobChunk, BLOB_CHUNK_HEADER_BYTES, BLOB_CHUNK_PAYLOAD,
  decodeCatalog, catalogChannelMap, decodePacked, decodeEventBody,
  schemaByKey, optionAccessFor, canUseOption, entriesOfClass, hasChannel,
} from './catalog.js';

// etag verification (§8.3) — SubtleCrypto is unavailable on a plain-http LAN
// origin, so the catalog's hash check is done with this synchronous SHA-256.
export { sha256, catalogEtag, bytesEqual, toHex, fromHex } from './sha256.js';

// identity + credentials. `setHttpGet` is the ONLY thing a Tauri shell has to
// touch to make this bundle behave exactly like the device-served page.
export {
  getInstanceId, resetIdentity,
  getPairedToken, setPairedToken, clearPairedToken, isPaired,
} from './identity.js';
export { acquireToken, mintUiToken, setHttpGet } from './credentials.js';

// Convenience grouping of the device channel ids the UI cares about, so an
// integrator can write CH.MOTION instead of importing each constant.
import {
  CH_CATALOG as _CATALOG, CH_SAFETY as _SAFETY, CH_CONTROL_OWNER as _CTRL,
  CH_SAFETY_INTENTS as _SI, CH_HUB_STATUS as _HUB, CH_SESSION_EVENTS as _EVT,
  CH_LOG as _LOG, CH_SESSION_ADMIN as _ADMIN, CH_SAFETY_EVENTS as _SAFEVT,
  CH_MOTION as _MOTION, CH_MACHINE_CONFIG as _CFG, CH_MACHINE_MODES as _MODES,
  CH_PATTERN_STATE as _PAT,
  CH_ODOMETER as _ODO, CH_MOTION_INPUT as _IN, CH_MOTION_SEGMENT as _SEG,
  CH_PLAN_STRIP as _PLAN, CH_POWER as _POWER, CH_MOTION_DIAG as _DIAG,
  CH_MOTION_ANOMALY as _ANOM,
  CH_MOVE as _MOVE, CH_CONFIG_SET as _CFGSET, CH_PATTERN_CMD as _PCMD, CH_HOME as _HOME,
  CH_MODES_SET as _MODESSET,
} from './frames.js';

export const CH = {
  CATALOG: _CATALOG,
  SAFETY: _SAFETY,
  CONTROL_OWNER: _CTRL,
  SAFETY_INTENTS: _SI,
  HUB_STATUS: _HUB,
  SESSION_EVENTS: _EVT,
  LOG: _LOG,
  SESSION_ADMIN: _ADMIN,
  SAFETY_EVENTS: _SAFEVT,
  MOTION: _MOTION,
  MACHINE_CONFIG: _CFG,
  MACHINE_MODES: _MODES,
  PATTERN_STATE: _PAT,
  ODOMETER: _ODO,
  MOTION_INPUT: _IN,
  MOTION_SEGMENT: _SEG,
  PLAN_STRIP: _PLAN,
  POWER: _POWER,
  MOTION_DIAG: _DIAG,
  MOTION_ANOMALY: _ANOM,
  MOVE: _MOVE,
  CONFIG_SET: _CFGSET,
  PATTERN_CMD: _PCMD,
  HOME: _HOME,
  MODES_SET: _MODESSET,
};
