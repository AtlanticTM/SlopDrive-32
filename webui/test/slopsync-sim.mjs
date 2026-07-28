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
import { createSession, CH, PRIORITY, SAFETY_OP, ACCESS, NACK } from '../../../SlopSync/clients/js/index.js';
import { toHex } from '../../../SlopSync/clients/js/sha256.js';

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
  // 19-CHANNEL SIM-FIDELITY FOLLOW-ON (2026-07-28) — the SlopDeck milestone-1
  // gap: machine-modes/modes-set, the 3 SlopMotion tuning cards + sm-set,
  // fray-d Advanced pattern + its 6 modifier lanes + writer, the preset
  // roster/store/cmd trio, machine-admin. Each family below: subscribe once,
  // write -> post-clamp ECHO -> STATE reflect, plus one invalid-write ->
  // correct-NACK case. Channel ids are the catalog's own (unchanged by this
  // pass) — no CH.* constants exist for most of these in webui/src yet, so
  // they're referenced directly; `sendIntent` is schema-driven from the
  // catalog either way (see session.js's own doc comment on it).
  // ========================================================================
  console.log('\n19-channel sim-fidelity write plane (2026-07-28 follow-on):');
  const CH19 = {
    MACHINE_MODES: 0x1030, MODES_SET: 0x3030,
    SM_LIMITS: 0x1120, SM_CHASE: 0x1121, SM_WAVEFORM: 0x1122, SM_SET: 0x3120,
    PATTERN_ADVANCED: 0x1210,
    AP_MOD_SPEEDIN: 0x1211, AP_MOD_SPEEDOUT: 0x1212, AP_MOD_ACCELIN: 0x1213,
    AP_MOD_ACCELOUT: 0x1214, AP_MOD_DEPTH1: 0x1215, AP_MOD_DEPTH2: 0x1216,
    PATTERN_ADVANCED_CMD: 0x3210,
    PATTERN_PRESETS_ROSTER: 0x1220, PATTERN_PRESETS: 0x5220, PATTERN_PRESETS_CMD: 0x3220,
    MACHINE_ADMIN: 0x30f0,
  };
  s1.subscribe([
    [CH19.MACHINE_MODES, 0, PRIORITY.background],
    [CH19.SM_LIMITS, 0, PRIORITY.background],
    [CH19.SM_CHASE, 0, PRIORITY.background],
    [CH19.SM_WAVEFORM, 0, PRIORITY.background],
    [CH19.PATTERN_ADVANCED, 0, PRIORITY.background],
    [CH19.AP_MOD_SPEEDIN, 0, PRIORITY.background],
    [CH19.AP_MOD_SPEEDOUT, 0, PRIORITY.background],
    [CH19.AP_MOD_ACCELIN, 0, PRIORITY.background],
    [CH19.AP_MOD_ACCELOUT, 0, PRIORITY.background],
    [CH19.AP_MOD_DEPTH1, 0, PRIORITY.background],
    [CH19.AP_MOD_DEPTH2, 0, PRIORITY.background],
    [CH19.PATTERN_PRESETS_ROSTER, 0, PRIORITY.background],
  ]);
  const want19 = [CH19.MACHINE_MODES, CH19.SM_LIMITS, CH19.SM_CHASE, CH19.SM_WAVEFORM,
    CH19.PATTERN_ADVANCED, CH19.PATTERN_PRESETS_ROSTER];
  for (let i = 0; i < 60 && !want19.every((c) => seen1.states.has(c)); i++) await delay(50);
  ok('every newly-subscribed 19-channel STATE delivered its initial push (was UNKNOWN_CHANNEL-only before this pass)',
    want19.every((c) => seen1.states.has(c)),
    want19.filter((c) => !seen1.states.has(c)).map((c) => '0x' + c.toString(16)).join(',') || 'all present');

  // ---- machine-modes (0x1030) / modes-set (0x3030) ------------------------
  {
    const echo = await s1.sendModesSet({ 3: 1, 4: 1 });
    ok('modes-set ECHO carries post-clamp APPLIED values (stream_speed_mode, overshoot_clamp)',
      echo.applied[3] === 1 && echo.applied[4] === 1, JSON.stringify(echo.applied));
    const reflected = await waitFor(s1, 'state',
      (ch, sm) => ch === CH19.MACHINE_MODES && sm.stream_speed_mode === 1 && sm.overshoot_clamp === 1,
      2000, 'machine-modes reflect').then(() => true).catch(() => false);
    ok('0x1030 machine-modes STATE reflects the applied modes', reflected);
    const restore = await s1.sendModesSet({ 3: 0, 4: 0 });
    ok('modes-set RESTORED', restore.applied[3] === 0 && restore.applied[4] === 0);

    // Keys 1/2 (blend_mode/transport) are PERMANENT GAPS — the catalog
    // declares no schema field for them at all anymore (see addModesSet's own
    // comment), so a client-side encode of {1:...} throws locally before a
    // frame is even sent; that is the client-side half of the same "no live
    // setting left to write" fact. The wire-level half — a request that
    // touches NO recognized key at all — is what actually reaches the hub's
    // `anyApplied` check, so that is what this NACK case exercises.
    let nacked = null;
    try { await s1.sendIntent(CH19.MODES_SET, {}); } catch (e) { nacked = e; }
    ok('modes-set: a write touching no recognized key NACKs INVALID_VALUE',
      !!nacked && nacked.name === 'INVALID_VALUE', nacked ? nacked.name : 'no NACK!');
  }

  // ---- SlopMotion tuning: sm-limits/chase/waveform (0x1120-2) / sm-set (0x3120)
  {
    // One sm-set write touches all THREE STATE cards in the same hub tick, so
    // every waitFor listener is armed BEFORE the write goes out (not chained
    // after each other) — the sim's WS client can deliver a tick's several
    // STATE frames as one synchronous burst, and a listener registered only
    // after an earlier await already resolved can miss a sibling frame from
    // the SAME burst (a real race, not a hypothetical one — this is exactly
    // what an earlier draft of this test hit).
    const limP = waitFor(s1, 'state',
      (ch, sm) => ch === CH19.SM_LIMITS && sm.centering === 0 && Math.abs(sm.vmax_ovr - 20) < 0.01,
      2000, 'sm-limits reflect').then(() => true).catch(() => false);
    const chaseP = waitFor(s1, 'state',
      (ch, sm) => ch === CH19.SM_CHASE && Math.abs(sm.handoff_k - 2.5) < 0.01,
      2000, 'sm-chase reflect').then(() => true).catch(() => false);
    const wavP = waitFor(s1, 'state',
      (ch, sm) => ch === CH19.SM_WAVEFORM && sm.blend_steps === 3,
      2000, 'sm-waveform reflect').then(() => true).catch(() => false);

    const echo = await s1.sendIntent(CH19.SM_SET, { 2: 999, 4: 0, 5: 0.5, 12: 2.5, 18: 3 });
    ok('sm-set ECHO carries post-clamp APPLIED values',
      echo.applied[4] === 0 && Math.abs(echo.applied[5] - 0.5) < 0.01 &&
      Math.abs(echo.applied[12] - 2.5) < 0.01 && echo.applied[18] === 3,
      JSON.stringify(echo.applied));
    ok('sm-set CLAMPS an out-of-range override (vmax_ovr=999 -> 20, the catalog\'s own max)',
      Math.abs(echo.applied[2] - 20) < 0.01, 'applied[2]=' + echo.applied[2]);

    const [reflectedLim, reflectedChase, reflectedWav] = await Promise.all([limP, chaseP, wavP]);
    ok('0x1120 slopmotion-limits STATE reflects centering + the clamped vmax_ovr', reflectedLim);
    ok('0x1121 slopmotion-chase STATE reflects handoff_k', reflectedChase);
    ok('0x1122 slopmotion-waveform STATE reflects blend_steps', reflectedWav);

    await s1.sendIntent(CH19.SM_SET, { 2: 0, 4: 1, 5: 1.0, 12: 1.5, 18: 6 }); // restore factory defaults

    let nacked = null;
    try { await s1.sendIntent(CH19.SM_SET, {}); } catch (e) { nacked = e; }
    ok('sm-set: an empty write (no keys touched) NACKs INVALID_VALUE',
      !!nacked && nacked.name === 'INVALID_VALUE', nacked ? nacked.name : 'no NACK!');
  }

  // ---- fray-d Advanced pattern: pattern-advanced + 6 modifiers (0x1210 family)
  // writer pattern-advanced-cmd (0x3210) ------------------------------------
  {
    // Same "arm every listener before the write" rule as the sm-set block
    // above — this one write touches BOTH pattern-advanced and its
    // pattern-adv-mod-speedin lane in the same tick.
    const baseP = waitFor(s1, 'state',
      (ch, sm) => ch === CH19.PATTERN_ADVANCED && sm.ap_mode === 1 && sm.master === 50 &&
        sm.max_depth === 80 && sm.min_depth === 10,
      2000, 'pattern-advanced reflect').then(() => true).catch(() => false);
    const modP = waitFor(s1, 'state',
      (ch, sm) => ch === CH19.AP_MOD_SPEEDIN && sm.amplitude === 40 && sm.in_step === 25,
      2000, 'pattern-adv-mod-speedin reflect').then(() => true).catch(() => false);

    const echo = await s1.sendIntent(CH19.PATTERN_ADVANCED_CMD,
      { 1: true, 2: 50, 3: 80, 4: 10, 21: 40, 22: 999 });
    ok('pattern-advanced-cmd ECHO carries post-clamp APPLIED base + modifier values',
      echo.applied[1] === true && echo.applied[2] === 50 && echo.applied[3] === 80 &&
      echo.applied[4] === 10 && echo.applied[21] === 40,
      JSON.stringify(echo.applied));
    ok('pattern-advanced-cmd CLAMPS an out-of-range modifier sub-field (speedin in_step=999 -> 25)',
      echo.applied[22] === 25, 'applied[22]=' + echo.applied[22]);

    const [reflectedBase, reflectedMod] = await Promise.all([baseP, modP]);
    ok('0x1210 pattern-advanced STATE reflects the applied base controls (depth pair re-coupled)', reflectedBase);
    ok('0x1211 pattern-adv-mod-speedin STATE reflects the applied + clamped modifier', reflectedMod);

    let nacked = null;
    try { await s1.sendIntent(CH19.PATTERN_ADVANCED_CMD, {}); } catch (e) { nacked = e; }
    ok('pattern-advanced-cmd: an empty write NACKs INVALID_VALUE',
      !!nacked && nacked.name === 'INVALID_VALUE', nacked ? nacked.name : 'no NACK!');
  }

  // ---- preset roster/store/cmd trio (0x1220 / 0x5220 / 0x3220) ------------
  {
    const rosterBefore = seen1.states.get(CH19.PATTERN_PRESETS_ROSTER);
    const countBefore = rosterBefore ? rosterBefore.count : 0;
    info('preset roster before: count=' + countBefore + ' capacity=' + (rosterBefore && rosterBefore.capacity));

    const saveEcho = await s1.sendIntent(CH19.PATTERN_PRESETS_CMD, { 1: 1, 2: 0, 3: 'sim-test-preset' });
    ok('pattern-presets-cmd save ECHO carries op/slot/name',
      saveEcho.applied[1] === 1 && saveEcho.applied[2] === 0 && saveEcho.applied[3] === 'sim-test-preset',
      JSON.stringify(saveEcho.applied));
    const rosterAfterSave = await waitFor(s1, 'state',
      (ch, sm) => ch === CH19.PATTERN_PRESETS_ROSTER && sm.count === countBefore + 1,
      2000, 'preset roster count+1').then(() => true).catch(() => false);
    ok('0x1220 pattern-presets-roster STATE count increments after save (0x5220 STORE backs it)', rosterAfterSave);

    // ap_mode is already true from the pattern-advanced-cmd test just above,
    // so `load` re-asserting true would NOT change the STATE bytes and would
    // NEVER produce a fresh push to wait on (diff-what-we-sent, same as the
    // firmware) — flip it off first so `load` turning it back on is a REAL,
    // observable diff, not a no-op this test would hang on.
    await s1.sendIntent(CH19.PATTERN_ADVANCED_CMD, { 1: false });
    const apModeP = waitFor(s1, 'state',
      (ch, sm) => ch === CH19.PATTERN_ADVANCED && sm.ap_mode === 1,
      2000, 'ap_mode after load').then(() => true).catch(() => false);
    const loadEcho = await s1.sendIntent(CH19.PATTERN_PRESETS_CMD, { 1: 2, 2: 0 });
    ok('pattern-presets-cmd load ECHO carries op/slot',
      loadEcho.applied[1] === 2 && loadEcho.applied[2] === 0, JSON.stringify(loadEcho.applied));
    const apModeAfterLoad = await apModeP;
    ok('preset load engages Advanced mode (0x1210 ap_mode reflects on)', apModeAfterLoad);

    const renameEcho = await s1.sendIntent(CH19.PATTERN_PRESETS_CMD, { 1: 4, 2: 0, 3: 'renamed' });
    ok('pattern-presets-cmd rename ECHO carries op/slot/name',
      renameEcho.applied[1] === 4 && renameEcho.applied[3] === 'renamed', JSON.stringify(renameEcho.applied));

    const deleteEcho = await s1.sendIntent(CH19.PATTERN_PRESETS_CMD, { 1: 3, 2: 0 });
    ok('pattern-presets-cmd delete ECHO carries op/slot',
      deleteEcho.applied[1] === 3 && deleteEcho.applied[2] === 0, JSON.stringify(deleteEcho.applied));
    const rosterAfterDelete = await waitFor(s1, 'state',
      (ch, sm) => ch === CH19.PATTERN_PRESETS_ROSTER && sm.count === countBefore,
      2000, 'preset roster count restored').then(() => true).catch(() => false);
    ok('0x1220 roster count decrements back after delete', rosterAfterDelete);

    let nacked = null;
    try { await s1.sendIntent(CH19.PATTERN_PRESETS_CMD, { 1: 1, 2: 99, 3: 'oob' }); } catch (e) { nacked = e; }
    ok('pattern-presets-cmd: an out-of-range slot (99 >= capacity 24) NACKs INVALID_VALUE',
      !!nacked && nacked.name === 'INVALID_VALUE', nacked ? nacked.name : 'no NACK!');
  }

  // ---- machine-admin (0x30F0) ----------------------------------------------
  {
    const clearFault = await s1.sendIntent(CH19.MACHINE_ADMIN, { 1: 1 });
    ok('machine-admin clear_fault ECHOes the accepted op', clearFault.applied[1] === 1);
    const saveConfig = await s1.sendIntent(CH19.MACHINE_ADMIN, { 1: 2 });
    ok('machine-admin save_config ECHOes the accepted op', saveConfig.applied[1] === 2);
    const servoScan = await s1.sendIntent(CH19.MACHINE_ADMIN, { 1: 3 });
    ok('machine-admin servo_scan ECHOes the accepted op', servoScan.applied[1] === 3);

    let nacked = null;
    try { await s1.sendIntent(CH19.MACHINE_ADMIN, { 1: 99 }); } catch (e) { nacked = e; }
    ok('machine-admin: an unknown op (99) NACKs UNSUPPORTED_OP',
      !!nacked && nacked.name === 'UNSUPPORTED_OP', nacked ? nacked.name : 'no NACK!');
  }

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
