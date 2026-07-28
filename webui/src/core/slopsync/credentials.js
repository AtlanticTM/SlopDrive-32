/**
 * credentials.js — how this client obtains the token it presents in HELLO.
 *
 * THE POINT OF THIS FILE: it is the ONLY difference between the WebUI the
 * device serves and the same WebUI inside a Tauri shell. Everything else —
 * every card, every intent, every renderer — is byte-identical between the two
 * builds. The shell supplies one function (`httpGet`) and, if it wants, a host;
 * that is the whole delta.
 *
 * ── The ladder (tried in order, first hit wins) ─────────────────────────────
 *
 *   1. STORED PAIRING TOKEN (identity.js). Durable, survives reboots on both
 *      ends, and is the ONLY rung that still works once the operator disables
 *      /uitoken. This is the real credential; the rest is bootstrap.
 *
 *   2. /uitoken MINT. A single-use, 60 s, control-tier token the device hands
 *      to anything that can HTTP GET it. See the honesty note below.
 *
 *   3. NOTHING. HELLO carries no token and the hub answers `watch`: the UI
 *      renders, telemetry flows, controls gray out, and the e-stop STILL WORKS
 *      (safety ops are role-exempt by catalog `option_access`, RFC-025b — the
 *      person standing next to the machine can always stop it). Degraded is a
 *      designed state here, not a failure.
 *
 * ── HONESTY ABOUT /uitoken ──────────────────────────────────────────────────
 *
 * The endpoint's only defense is the ABSENCE of CORS headers, which stops a
 * malicious *web page* in the operator's browser from reading the response. It
 * stops nothing else: `curl http://<device>/uitoken` works from anywhere on the
 * LAN and yields control tier. So while /uitoken is enabled, "authentication is
 * on" means *LAN trust, formalised* — every client presents a credential and
 * the hub has a real chokepoint — NOT "the machine is locked down". The lockdown
 * posture is /uitoken DISABLED plus paired tokens, and this ladder walks into
 * that posture without a code change: rung 2 simply stops answering and rung 1
 * carries everyone who paired.
 *
 * That is also exactly why rung 1 is tried FIRST rather than last. A paired
 * client should never spend a mint it does not need, and should never depend on
 * an endpoint the operator may have turned off.
 *
 * ── Tauri (the ~15 lines) ───────────────────────────────────────────────────
 *
 *   import { fetch as tauriFetch } from '@tauri-apps/plugin-http';
 *   setHttpGet(async (url) => {
 *     const r = await tauriFetch(url, { method: 'GET' });   // Rust-side: no CORS
 *     return r.ok ? await r.text() : null;
 *   });
 *
 * The Rust-side fetch is not subject to browser CORS, so the shell gets the same
 * frictionless first-run the device-served page gets. If a shell would rather
 * force explicit pairing, it just does not call setHttpGet — rung 2 goes dark
 * and rung 1/3 behave correctly with no other change.
 */

import { LIMITS } from './frames.js';
import { fromHex } from './sha256.js';
import { getPairedToken } from './identity.js';

// ---- the injectable transport (THE Tauri seam) -----------------------------

/**
 * Default: the browser's own fetch. Same-origin when the device served this
 * page, which is the case the no-CORS design is built around.
 * @param {string} url
 * @returns {Promise<string|null>} response body, or null on any failure
 */
async function browserGet(url) {
  if (typeof fetch !== 'function') return null;
  const res = await fetch(url, { method: 'GET', cache: 'no-store', credentials: 'omit' });
  // 429 is reported, not swallowed, so the retry loop above can tell a
  // rate-limit apart from a refusal. Anything else non-OK is a plain miss.
  if (res.status === 429) return RATE_LIMITED;
  if (!res.ok) return null;
  return await res.text();
}

/**
 * Sentinel for "try again shortly". A distinct object rather than a magic
 * string so a shell's custom httpGet can never collide with it by returning
 * ordinary body text.
 */
export const RATE_LIMITED = Symbol('slopsync.rate_limited');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let _httpGet = browserGet;

/**
 * Replace the HTTP GET used for token minting. A Tauri shell passes its
 * Rust-side fetch here; nothing else in the bundle changes.
 * @param {(url: string) => Promise<string|null>} fn
 */
export function setHttpGet(fn) { _httpGet = typeof fn === 'function' ? fn : browserGet; }

// ---- /uitoken --------------------------------------------------------------

/**
 * Mint a single-use control-tier token from `host`.
 *
 * Failure is NORMAL and quiet: 403 (operator disabled it), 429 (the device-wide
 * one-mint-per-interval rate limit), an unreachable host, or a cross-origin
 * block all return null, and null just means "try the next rung". This must
 * never throw into the connect path — a client that cannot get a token should
 * still connect as a viewer.
 *
 * @param {string} host
 * @returns {Promise<Uint8Array|null>}
 */
/**
 * Where to ask for a mint.
 *
 * When the page is served BY the machine — the normal case — use a
 * same-origin relative URL. Building `http://<host>/uitoken` by hand assumes
 * the HTTP server is on port 80 and silently fails with a connection refusal
 * anywhere it is not, which is exactly what happened against a simulator
 * serving on :8080: the socket connected, the catalog rendered, and the client
 * quietly dropped to `watch` with three console errors as the only clue.
 *
 * Same-origin is also the safer request: it is the shape the endpoint's
 * no-CORS defense is designed around.
 */
function mintUrl(host) {
  if (typeof location !== 'undefined' && location.hostname === host) return '/uitoken';
  return 'http://' + host + '/uitoken';
}

export async function mintUiToken(host, attempts = 4) {
  for (let i = 0; i < attempts; i++) {
    try {
      const body = await _httpGet(mintUrl(host));
      if (body === RATE_LIMITED) {
        // RETRYING THIS IS NOT OPTIONAL. The mint is capped at one per 250 ms
        // DEVICE-WIDE, not per client, so a second browser tab, a running
        // slopsoak, or a reconnect storm can all 429 a perfectly entitled page.
        // Treating that as "no credential" would silently demote the UI to
        // viewer — every control dead, no error, until someone reloads. Jitter
        // keeps concurrent callers from colliding again in lockstep.
        await sleep(300 + Math.random() * 250);
        continue;
      }
      if (!body) return null;
      const j = JSON.parse(body);
      if (!j || j.ok !== true || typeof j.token !== 'string') return null;
      if (j.token.length !== LIMITS.token_bytes * 2) return null;
      const b = fromHex(j.token);
      return b.length === LIMITS.token_bytes ? b : null;
    } catch (e) {
      await sleep(200);
    }
  }
  return null;
}

// ---- the ladder ------------------------------------------------------------

/**
 * Produce the token for one HELLO, walking the ladder above.
 *
 * CALLED PER CONNECT, NOT PER SESSION — and that is load-bearing. A /uitoken is
 * consumed by the hub the moment it validates (single-use, by design, so a page
 * cannot hoard one and replay it later). A token captured once at construction
 * would therefore authorize exactly the FIRST connect and silently demote every
 * reconnect to `watch`: the UI would come back after a blip looking connected
 * with every control dead. Minting per connect is what makes reconnection
 * transparent.
 *
 * @param {string} host
 * @param {Object} [opts]
 * @param {boolean} [opts.allowMint=true] set false to require a paired token
 * @param {Function} [opts.log]
 * @returns {Promise<Uint8Array|null>} null → connect as a viewer
 */
export async function acquireToken(host, opts = {}) {
  const log = opts.log || (() => {});

  const paired = getPairedToken(host);
  if (paired) { log('info', 'credential: stored pairing token'); return paired; }

  if (opts.allowMint === false) { log('info', 'credential: none (mint disabled)'); return null; }

  const minted = await mintUiToken(host);
  if (minted) { log('info', 'credential: /uitoken mint'); return minted; }

  log('warn', 'credential: none — connecting as viewer (watch tier)');
  return null;
}
