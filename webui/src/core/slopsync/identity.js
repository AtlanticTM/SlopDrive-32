/**
 * identity.js — this client's persistent SlopSync identity and its per-hub
 * credentials.
 *
 * WHY THIS EXISTS: `createSession()` defaults `instanceId` to fresh random
 * bytes. That is correct for a probe and WRONG for anything that pairs — the
 * trust ledger (RFC-029) is keyed on instance_id, so a client that re-rolls its
 * identity every page load appears as a brand-new device to the hub on every
 * refresh. It would never be recognized, it would burn a ledger slot per reload
 * until the ledger evicted real devices, and "pair once" would become "pair
 * forever". The identity is the thing being trusted, so it has to outlive the
 * page.
 *
 * WHAT IS STORED, AND WHERE
 *   slopsync.instance   — 8 random bytes, hex. This install's identity.
 *   slopsync.tok.<host> — 16-byte pairing token, hex, ONE PER HUB.
 *
 * Tokens are per-host on purpose: a token is a credential issued BY one hub and
 * meaningless at another, and this same bundle is expected to talk to several
 * machines (Tauri shell, multi-hub). Keying them together would leak one
 * machine's credential at another machine's HELLO.
 *
 * STORAGE HONESTY: localStorage is origin-scoped, unencrypted, and readable by
 * any script on this origin. That is an accurate match for what these
 * credentials are worth — a LAN control token for a machine whose HTTP plane
 * already hands one to anything that asks (see credentials.js). It is NOT a
 * place for anything that would matter off this LAN. When storage is
 * unavailable (private mode, a webview with it disabled) everything degrades to
 * process memory: the session still works, it just re-pairs next launch.
 */

import { LIMITS } from './frames.js';
import { toHex, fromHex } from './sha256.js';

const KEY_INSTANCE = 'slopsync.instance';
const KEY_TOKEN_PREFIX = 'slopsync.tok.';

// ---- storage ---------------------------------------------------------------
// Same probe-before-trust shape session.js uses for its catalog cache: node
// defines `localStorage` but THROWS on access without --localstorage-file, and
// a browser in private mode can too, so availability is only knowable by
// touching it.
const _mem = new Map();

const _store = (() => {
  try {
    if (typeof localStorage === 'undefined') return null;
    localStorage.getItem('slopsync.probe');
    return localStorage;
  } catch (e) {
    return null;
  }
})();

function readRaw(key) {
  if (_store) { try { return _store.getItem(key); } catch (e) { return null; } }
  return _mem.has(key) ? _mem.get(key) : null;
}

function writeRaw(key, val) {
  if (_store) { try { _store.setItem(key, val); return; } catch (e) { /* quota */ } }
  _mem.set(key, val);
}

function removeRaw(key) {
  if (_store) { try { _store.removeItem(key); } catch (e) { /* ignore */ } }
  _mem.delete(key);
}

function randomBytes(n) {
  const b = new Uint8Array(n);
  if (typeof crypto !== 'undefined' && crypto.getRandomValues) crypto.getRandomValues(b);
  else for (let i = 0; i < n; i++) b[i] = (Math.random() * 256) | 0;
  return b;
}

// ---- instance id -----------------------------------------------------------

let _instanceId = null;

/**
 * This install's stable 8-byte instance_id, minted once and persisted.
 * Cached in module scope so every session in this page agrees even if storage
 * is unavailable and the value only lives in memory.
 * @returns {Uint8Array}
 */
export function getInstanceId() {
  if (_instanceId) return _instanceId;
  const hex = readRaw(KEY_INSTANCE);
  if (hex && hex.length === LIMITS.instance_id_bytes * 2) {
    try {
      const b = fromHex(hex);
      if (b.length === LIMITS.instance_id_bytes) { _instanceId = b; return _instanceId; }
    } catch (e) { /* corrupt — fall through and re-mint */ }
  }
  _instanceId = randomBytes(LIMITS.instance_id_bytes);
  writeRaw(KEY_INSTANCE, toHex(_instanceId));
  return _instanceId;
}

/**
 * Forget this identity and every credential bound to it. This is the "unpair
 * everything, start clean" button — the hub keeps its ledger entry (only the
 * hub can revoke that), we simply stop being able to prove we are that device.
 */
export function resetIdentity() {
  removeRaw(KEY_INSTANCE);
  _instanceId = null;
  const prefix = KEY_TOKEN_PREFIX;
  if (_store) {
    try {
      const doomed = [];
      for (let i = 0; i < _store.length; i++) {
        const k = _store.key(i);
        if (k && k.indexOf(prefix) === 0) doomed.push(k);
      }
      for (const k of doomed) _store.removeItem(k);
    } catch (e) { /* ignore */ }
  }
  for (const k of Array.from(_mem.keys())) if (k.indexOf(prefix) === 0) _mem.delete(k);
}

// ---- per-hub pairing tokens ------------------------------------------------

function tokenKey(host) { return KEY_TOKEN_PREFIX + String(host); }

/**
 * The stored pairing token for `host`, or null.
 * @returns {Uint8Array|null}
 */
export function getPairedToken(host) {
  const hex = readRaw(tokenKey(host));
  if (!hex || hex.length !== LIMITS.token_bytes * 2) return null;
  try {
    const b = fromHex(hex);
    return b.length === LIMITS.token_bytes ? b : null;
  } catch (e) { return null; }
}

/** Persist the token a PAIR_GRANT just issued for `host`. */
export function setPairedToken(host, token) {
  if (!token || token.length !== LIMITS.token_bytes) return false;
  writeRaw(tokenKey(host), toHex(token));
  return true;
}

/** Drop the stored token for `host` (revoked, or the operator unpaired). */
export function clearPairedToken(host) { removeRaw(tokenKey(host)); }

/** Do we already hold a credential for this hub? */
export function isPaired(host) { return getPairedToken(host) !== null; }
