/**
 * slopsync-sim.mjs — end-to-end proof of slopsync-js v1.0 against SLOPSIM.
 *
 * slopsim (sim/slopsim) embeds the REAL slopsync::Hub, the REAL slopmotion
 * engine and the REAL device catalog behind a real WebSocket server, so this
 * exercises the same library the firmware runs — without touching the machine.
 *
 *   Build:  cmake --build sim/slopsim/build
 *   Run it: sim/slopsim/build/slopsim.exe machine --homed --headless \
 *             --duration 240 --port 82 --http 80 --no-mdns
 *   Then:   node webui/test/slopsync-sim.mjs [--host 127.0.0.1] [--port 82]
 *
 * KILL ANY STALE SLOPSIM ON 80/82 FIRST or you are testing the wrong binary.
 *
 * What it proves, in order:
 *   1. COLD session — HELLO with no etag → WELCOME → BLOB_REQ → BLOB_CHUNK
 *      reassembly → local SHA-256 verify → CATALOG_READY → retained STATE →
 *      LIVE. (RFC-015's readiness gate, the whole point of this milestone.)
 *   2. An INTENT encoded from the CATALOG's own schema, resolving on its
 *      post-clamp ECHO, then restored.
 *   3. NACK correlation by intent_id/intent_seq (a deliberate CAS conflict).
 *   4. Client-assertable E-STOP (RFC-010 safety_ops::estop) observed as a
 *      latch on the 0x0003 snapshot — then cleared, leaving the sim as found.
 *   5. WARM session — the cached etag rides HELLO, the hub is ready
 *      immediately, and NO catalog transfer happens at all (the 99% path).
 *      Run back-to-back with no sim restart, which is this project's mandatory
 *      session-lifecycle regression pattern (field bug #3).
 *
 * It also writes webui/test/fixtures/slopsim-catalog.{bin,etag} so the OFFLINE
 * wire test can decode the real catalog and cross-check the etag.
 */

import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createSession, CH, PRIORITY, SAFETY_OP, ACCESS, NACK } from '../src/core/slopsync/index.js';
import { toHex } from '../src/core/slopsync/sha256.js';

const args = process.argv.slice(2);
const argOf = (flag, def) => { const i = args.indexOf(flag); return i >= 0 ? args[i + 1] : def; };
const HOST = argOf('--host', '127.0.0.1');
const PORT = parseInt(argOf('--port', '82'), 10);

let failures = 0;
const ok = (name, cond, extra) => {
  console.log('  [' + (cond ? 'PASS' : 'FAIL') + '] ' + name + (extra ? '  — ' + extra : ''));
  if (!cond) failures++;
};
const info = (m) => console.log('        ' + m);
const delay = (ms) => new Promise((r) => setTimeout(r, ms));

// A shared in-process catalog cache: session 1 fills it, session 2 proves the
// etag fast path. (In a browser this is localStorage; the store interface is
// the same either way.)
const cache = new Map();
const catalogStore = {
  load: (h) => cache.get(h) || null,
  save: (h, etag, bytes) => cache.set(h, { etag, bytes }),
  clear: (h) => cache.delete(h),
};

function waitFor(session, event, pred, timeoutMs, label) {
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => { offFn(); reject(new Error('timeout waiting for ' + (label || event))); }, timeoutMs);
    const offFn = session.on(event, (...a) => {
      if (!pred || pred(...a)) { clearTimeout(t); offFn(); resolve(a); }
    });
  });
}

async function openSession(label, extra = {}) {
  const seen = { catalogMeta: null, ready: null, states: new Map(), nacks: [], events: [] };
  const s = createSession({
    host: HOST,
    port: PORT,
    clientKind: 'webui',
    clientName: 'slopsync-js sim-test',
    autoReconnect: false,
    catalogStore,
    subscriptions: [
      [CH.SAFETY, 0, PRIORITY.critical],
      [CH.MACHINE_CONFIG, 0, PRIORITY.elevated],
      [CH.MOTION, 20, PRIORITY.elevated],
      [CH.MOTION_DIAG, 1, PRIORITY.background],
      [CH.MOTION_ANOMALY, 0, PRIORITY.background],
    ],
    ...extra,
  });
  s.on('catalog', (entries, map, meta) => { seen.catalogMeta = meta; });
  s.on('ready', (r) => { seen.ready = r; });
  s.on('state', (ch, sample) => { seen.states.set(ch, sample); });
  s.on('nack', (n) => seen.nacks.push(n));
  s.on('event', (e) => seen.events.push(e));

  const live = waitFor(s, 'live', null, 8000, label + ' LIVE');
  s.connect();
  await live;
  // LIVE means "catalog adopted + the retained pushes WELCOME promised are in".
  // This client SUBSCRIBEs after WELCOME (so WELCOME promises none), and the
  // hub's retained push for a fresh grant flows on its next pacing walk — give
  // the subscribed channels a moment to land before asserting on them.
  const wantAll = [CH.SAFETY, CH.MACHINE_CONFIG, CH.MOTION_DIAG];
  for (let i = 0; i < 60 && !wantAll.every((c) => seen.states.has(c)); i++) await delay(50);
  return { s, seen };
}

async function main() {
  console.log('slopsync-js ⇆ slopsim  ws://' + HOST + ':' + PORT + '/\n');

  // ========================================================================
  // SESSION 1 — cold: no cached catalog, so the full RFC-015 gate runs.
  // ========================================================================
  console.log('Session 1 (COLD — no cached etag: fetch, verify, declare):');
  const { s: s1, seen: seen1 } = await openSession('s1');

  const w = s1.state;
  ok('WELCOME adopted (session id + boot id + catalog etag)',
    !!w.sessionId && !!w.bootId && !!w.catalogEtag,
    'session=' + w.sessionId + ' etag=' + toHex(w.catalogEtag));
  if (w.identity) info('identity: ' + JSON.stringify(w.identity));
  ok('roles granted', typeof w.roles === 'number', 'roles=' + w.roles + ' (' + ['watch', 'control', 'configure'][w.roles] + ')');

  ok('catalog FETCHED over BLOB_REQ/BLOB_CHUNK (not cached)',
    seen1.catalogMeta && seen1.catalogMeta.cached === false);
  ok('reassembled catalog VERIFIES against the WELCOME etag (local SHA-256)',
    seen1.catalogMeta && seen1.catalogMeta.verified === true);
  ok('CATALOG_READY declared the verified etag',
    !!seen1.ready && toHex(seen1.ready.etag) === toHex(w.catalogEtag), toHex(seen1.ready.etag));
  ok('catalog decoded', Array.isArray(s1.catalog) && s1.catalog.length > 5,
    s1.catalog.length + ' channels, ' + s1.catalogBytes.length + ' B');

  // the gate opened: retained STATE only flows to a READY session
  ok('retained 0x0003 safety STATE received after readiness', seen1.states.has(CH.SAFETY));
  ok('retained 0x1000 machine-config STATE received', seen1.states.has(CH.MACHINE_CONFIG));
  ok('session reached LIVE (§2.2 SYNCING → LIVE)', s1.isLive);

  // ---- catalog-driven decode of the v1.0 device channels -------------------
  const motionEntry = s1.channelMap.get(CH.MOTION);
  ok('0x1100 motion decodes raw_10um from the CATALOG (no fallback table)',
    !!motionEntry && motionEntry.layout.some((f) => f.name === 'raw_10um'));
  const safetySample = seen1.states.get(CH.SAFETY);
  ok('0x0003 safety decodes the RFC-025c `modes` byte',
    safetySample && typeof safetySample.modes_bits === 'object',
    JSON.stringify(safetySample && safetySample.modes_bits));
  for (const id of [CH.PLAN_STRIP, CH.POWER, CH.MOTION_DIAG, CH.MOTION_ANOMALY]) {
    const e = s1.channelMap.get(id);
    info('0x' + id.toString(16) + ' ' + (e ? e.name + ' (' + e.clsName + ', ' +
      (e.layout ? e.layout.length + ' fields' : e.schema.length + ' schema keys') + ')' : 'NOT ADVERTISED'));
  }
  ok('0x1111 slopmotion-diag decodes live', seen1.states.has(CH.MOTION_DIAG),
    seen1.states.has(CH.MOTION_DIAG)
      ? 'plans=' + seen1.states.get(CH.MOTION_DIAG).plans + ' anomalies=' + seen1.states.get(CH.MOTION_DIAG).anomalies
      : '');

  // ---- per-op access (RFC-009 gray-never-hide) ----------------------------
  ok('0x0005 option_access read from the catalog: estop role-exempt, hold needs control',
    s1.optionAccessFor(CH.SAFETY_INTENTS, 1, SAFETY_OP.estop) === ACCESS.watch &&
    s1.optionAccessFor(CH.SAFETY_INTENTS, 1, SAFETY_OP.hold) === ACCESS.control);

  // ========================================================================
  // Write the golden fixture for the offline test.
  // ========================================================================
  const here = dirname(fileURLToPath(import.meta.url));
  mkdirSync(here + '/fixtures', { recursive: true });
  writeFileSync(here + '/fixtures/slopsim-catalog.bin', Buffer.from(s1.catalogBytes));
  writeFileSync(here + '/fixtures/slopsim-catalog.etag', toHex(w.catalogEtag) + '\n');
  info('fixture written: webui/test/fixtures/slopsim-catalog.{bin,etag}');

  // ========================================================================
  // INTENT → post-clamp ECHO, encoded from the catalog's own schema.
  // ========================================================================
  console.log('\nWrite plane (config-set 0x3000, catalog-typed, restored):');
  const cfg0 = seen1.states.get(CH.MACHINE_CONFIG);
  const origMin = cfg0.window_min, origMax = cfg0.window_max;
  info('device window = [' + origMin + ', ' + origMax + '] mm, max_rail ' + cfg0.max_rail);
  const testMin = Math.round(origMin + 3), testMax = Math.round(origMax - 3);
  const echo = await s1.sendConfigSet({ 1: testMin, 2: testMax });
  ok('config-set ECHO carries post-clamp APPLIED values',
    typeof echo.applied[1] === 'number' && typeof echo.applied[2] === 'number',
    'applied [' + echo.applied[1] + ', ' + echo.applied[2] + '] cfg_gen=' + echo.cfgGen);
  ok('APPLIED matches what was asked (inside the window\'s own bounds)',
    Math.abs(echo.applied[1] - testMin) < 0.6 && Math.abs(echo.applied[2] - testMax) < 0.6);
  const reflected = await waitFor(s1, 'state',
    (ch, sm) => ch === CH.MACHINE_CONFIG && Math.abs(sm.window_min - testMin) < 0.6, 3000,
    'machine-config reflect').then(() => true).catch(() => false);
  ok('0x1000 STATE reflects the applied window (ground truth, not our request)', reflected);
  const restore = await s1.sendConfigSet({ 1: origMin, 2: origMax });
  ok('window RESTORED', Math.abs(restore.applied[1] - origMin) < 0.6 &&
    Math.abs(restore.applied[2] - origMax) < 0.6);

  // ---- NACK correlation (RFC-001) -----------------------------------------
  console.log('\nNACK correlation (RFC-001 intent_seq / intent_id):');
  const nacksBefore = seen1.nacks.length;
  let rejected = null;
  try {
    await s1.sendConfigSet({ 1: origMin, 2: origMax }, { precondition: (s1.state.cfgGen + 777) & 0xffff });
    rejected = null;
  } catch (e) { rejected = e; }
  ok('a doomed cfg_gen CAS rejects THAT intent\'s promise', !!rejected,
    rejected ? rejected.name + ' ' + rejected.message : 'no rejection!');
  const lastNack = seen1.nacks[seen1.nacks.length - 1];
  ok('the hub NACK arrived', seen1.nacks.length > nacksBefore,
    lastNack ? lastNack.name + ' ch=0x' + (lastNack.channel || 0).toString(16) : '');
  ok('NACK carried a correlation key (intent_id and/or intent_seq)',
    !!lastNack && (lastNack.intentId != null || lastNack.intentSeq != null),
    lastNack ? 'intent_id=' + lastNack.intentId + ' intent_seq=' + lastNack.intentSeq : '');
  ok('the rejection is the SAME intent the hub named',
    !!rejected && !!lastNack &&
    (rejected.intentId === lastNack.intentId || rejected.intentSeq === lastNack.intentSeq));

  // ========================================================================
  // CLIENT-ASSERTABLE E-STOP (RFC-010) — the headline.
  // ========================================================================
  console.log('\nClient-assertable E-STOP (safety_ops::estop = 6 on 0x0005):');
  if (!args.includes('--estop')) {
    // Same opt-in as tools/slopsync_probe.py --estop, and for the same reason:
    // asserting it LATCHES the machine, and clearing the latch UN-HOMES it, so
    // a default run would leave the sim in a state where every later
    // motion-dependent check skips.
    console.log('  [SKIP] client-asserted ESTOP (pass --estop; it latches and un-homes the machine)');
  } else {
  const latched = waitFor(s1, 'state',
    (ch, sm) => ch === CH.SAFETY && sm.word_bits && sm.word_bits.estop === true, 4000, 'estop latch');
  await s1.assertEstop().catch((e) => info('estop op error: ' + e.message));
  const latchArr = await latched.then((a) => a).catch(() => null);
  ok('the machine LATCHED e-stop (0x0003 word bit 0)', !!latchArr,
    latchArr ? 'cause=' + latchArr[1].cause + ' estop_seq=' + latchArr[1].estop_seq : 'never latched');
  ok('the latch names cause=user (an operator asserted it)',
    !!latchArr && latchArr[1].cause === 0);

  // leave the sim as we found it
  const cleared = waitFor(s1, 'state',
    (ch, sm) => ch === CH.SAFETY && sm.word_bits && sm.word_bits.estop === false, 4000, 'estop clear');
  await s1.sendSafetyIntent(SAFETY_OP.estop_clear).catch((e) => info('estop_clear: ' + e.message));
  const clearedOk = await cleared.then(() => true).catch(() => false);
  ok('e-stop CLEARED again (latch gone; the machine still needs a re-home)', clearedOk);
  }

  s1.close();
  await delay(400);

  // ========================================================================
  // SESSION 2 — warm cache, back-to-back, NO sim restart (field bug #3).
  // ========================================================================
  console.log('\nSession 2 (WARM — cached etag rides HELLO; back-to-back, no restart):');
  const { s: s2, seen: seen2 } = await openSession('s2');
  ok('catalog came from the CACHE — zero transfer frames',
    !!seen2.catalogMeta && seen2.catalogMeta.cached === true);
  ok('readiness was INHERITED from the HELLO etag (no CATALOG_READY needed)',
    !!seen2.ready && seen2.ready.cached === true);
  ok('same etag as the hub still advertises',
    toHex(s2.state.catalogEtag) === toHex(w.catalogEtag));
  ok('retained STATE flowed anyway (the gate was already open)', seen2.states.has(CH.SAFETY));
  ok('session 2 reached LIVE', s2.isLive);
  const echo2 = await s2.sendConfigSet({ 1: origMin, 2: origMax });
  ok('a second back-to-back session can still write (ownership teardown clean)',
    typeof echo2.applied[1] === 'number');
  s2.close();
  await delay(300);

  console.log('');
  if (failures === 0) { console.log('ALL PASS'); process.exit(0); }
  console.log(failures + ' FAILED');
  process.exit(1);
}

main().catch((e) => { console.error('\nFATAL: ' + e.message); process.exit(1); });
