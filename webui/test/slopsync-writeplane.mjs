/**
 * slopsync-writeplane.mjs — LIVE end-to-end proof of the stroke-window write
 * path (the reported defect) against the real device, at the wire level.
 *
 * Flow (config-only, fully restored — NO motion-adjacent intents):
 *   connect → WELCOME → subscribe machine-config → record device window
 *   → config-set 0x3000 {1:min,2:max} a few mm off → assert ECHO applied
 *   → assert a machine-config 0x1000 on-change reflects the applied values
 *   → RESTORE the original window → assert restore ECHO + state
 *   → close cleanly (GOODBYE), then a SECOND session re-reads to prove the
 *     restore stuck AND a back-to-back session (field bug #3) works.
 *
 * SAFETY: this sends ONLY config-set (0x3000). It never sends 0x3100 move,
 * 0x3200 pattern-cmd, 0x3101 home, or 0x0005 safety intents. The machine is
 * unhomed + latched STOP; window config is not motion and is restored.
 *
 * Run:  node webui/test/slopsync-writeplane.mjs           (exits 1 on any failure)
 *       node webui/test/slopsync-writeplane.mjs 192.168.1.229 82
 */

import { createSession, CH } from '../../../SlopSync/clients/js/index.js';
import { PRIORITY } from '../../../SlopSync/clients/js/frames.js';
import { acquireToken } from '../../../SlopSync/clients/js/credentials.js';

const HOST = process.argv[2] || '192.168.1.229';
const PORT = parseInt(process.argv[3] || '82', 10);

let failures = 0;
function ok(name, cond, extra) {
  console.log('  [' + (cond ? 'PASS' : 'FAIL') + '] ' + name + (extra ? '  ' + extra : ''));
  if (!cond) failures++;
}
const near = (a, b, eps = 0.6) => typeof a === 'number' && typeof b === 'number' && Math.abs(a - b) <= eps;
const delay = (ms) => new Promise((r) => setTimeout(r, ms));

/** Open a session, wait for the first machine-config STATE, return {session, cfg}. */
function openAndRead(tag) {
  return new Promise((resolve, reject) => {
    const s = createSession({
      host: HOST, port: PORT, clientKind: 'webui', clientName: 'writeplane-test-' + tag,
      autoReconnect: false, WebSocketImpl: WebSocket,
      // Since fw 2.1.59 the write plane needs a credential — a tokenless
      // session is `watch` and every config-set below would NACK. Use the SAME
      // ladder the real UI uses, so this test exercises the shipped path rather
      // than a special one.
      token: (h) => acquireToken(h),
      subscriptions: [[CH.MACHINE_CONFIG, 0, PRIORITY.elevated]],
    });
    const to = setTimeout(() => { try { s.close(); } catch (e) {} reject(new Error('timeout waiting for machine-config')); }, 6000);
    let welcomed = false;
    s.on('welcome', (w) => { welcomed = true; s._roles = w.roles; });
    s.on('state', (ch, sample) => {
      if (ch === CH.MACHINE_CONFIG && welcomed) { clearTimeout(to); resolve({ session: s, cfg: sample }); }
    });
    s.on('close', () => { clearTimeout(to); if (!welcomed) reject(new Error('closed before welcome')); });
    s.connect();
  });
}

/** Wait for a machine-config STATE for which pred(sample) is true, or timeout. */
function waitConfig(session, pred, timeoutMs = 4000) {
  return new Promise((resolve) => {
    const off = session.on('state', (ch, sample) => {
      if (ch === CH.MACHINE_CONFIG && pred(sample)) { off(); resolve(sample); }
    });
    setTimeout(() => { off(); resolve(null); }, timeoutMs);
  });
}

async function main() {
  console.log('slopsync-js write-plane live test → ws://' + HOST + ':' + PORT + '/  (config-only, restored)');

  // ---- Session 1: read → change → verify → restore → verify ----------------
  const { session: s, cfg: cfg0 } = await openAndRead('1');
  const origMin = cfg0.window_min, origMax = cfg0.window_max, maxRail = cfg0.max_rail;
  const origSpd = cfg0.user_speed;
  ok('WELCOME + retained machine-config adopted', typeof origMin === 'number' && typeof origMax === 'number',
     'window=[' + origMin + ', ' + origMax + '] user_speed=' + origSpd + ' max_rail=' + maxRail + ' roles=' + s._roles);

  // ===== Part 1: STROKE WINDOW write (0x3000 {1,2}) — the reported defect =====
  // Pick a legal nudge: shrink both ends a few mm, staying inside [0, max_rail], min<max.
  const rail = (typeof maxRail === 'number' && maxRail > 0) ? maxRail : 500;
  let newMin = Math.min(rail - 10, Math.max(0, Math.round(origMin) + 5));
  let newMax = Math.max(newMin + 10, Math.min(rail, Math.round(origMax) - 5));
  ok('picked legal test window inside [0, ' + rail + ']', newMin >= 0 && newMax <= rail && newMin < newMax,
     'new=[' + newMin + ', ' + newMax + ']');

  // config-set 0x3000 → ECHO (post-clamp APPLIED) — the ground-truth confirm the
  // bridge adopts into the rail band. This IS the write-plane defect path.
  const wConfirm = waitConfig(s, () => true); // any re-publish proves the device processed it
  const echo = await s.sendConfigSet({ 1: newMin, 2: newMax });
  const a1 = echo.applied[1], a2 = echo.applied[2];
  ok('window config-set ECHO carries applied [min,max]', typeof a1 === 'number' && typeof a2 === 'number',
     'applied=[' + a1 + ', ' + a2 + '] cfg_gen=' + echo.cfgGen);
  ok('window ECHO applied == request (legal → unclamped)', near(a1, newMin) && near(a2, newMax));
  const wSt = await wConfirm;
  // NUANCE (found live, not a bug): the 0x1000 STATE publishes the EFFECTIVE
  // motion window, which while UNHOMED is clamped to the full rail [0,max_rail]
  // regardless of the stored config the ECHO confirms. So the STATE re-publishes
  // (proving the device processed the set) but shows effective bounds, not the
  // configured [min,max]. Correct ground-truth: STATE = what the machine will DO.
  // INFORMATIONAL, not asserted: while unhomed the effective window is
  // constant [0,max_rail], so an on-change channel may legitimately NOT
  // republish after a stored-window set. The window path is proven by its
  // ECHO above; the STATE round-trip is asserted via user_speed below
  // (not homing-gated, always reflects).
  console.log('  [info] machine-config STATE after window set: ' +
    (wSt ? 'republished, effective=[' + wSt.window_min + ', ' + wSt.window_max + ']'
         : 'no republish observed (acceptable while unhomed — effective window unchanged)'));

  // RESTORE the window.
  const rEcho = await s.sendConfigSet({ 1: Math.round(origMin), 2: Math.round(origMax) });
  ok('window restore ECHO applied original', near(rEcho.applied[1], origMin) && near(rEcho.applied[2], origMax),
     'applied=[' + rEcho.applied[1] + ', ' + rEcho.applied[2] + ']');

  // ===== Part 2: config-set → machine-config STATE reflects APPLIED (user_speed)
  // user_speed (field 3) is NOT homing-gated, so its 0x1000 STATE field DOES
  // reflect the applied value — proving the full request→echo→on-change-STATE
  // adoption loop the bridge depends on, end-to-end at the wire level. =========
  const newSpd = Math.round((typeof origSpd === 'number' ? origSpd : 50)) + 15;
  const wSpd = waitConfig(s, (x) => near(x.user_speed, newSpd));
  const sEcho = await s.sendConfigSet({ 3: newSpd });
  ok('user_speed config-set ECHO carries applied value', near(sEcho.applied[3], newSpd),
     'applied user_speed=' + sEcho.applied[3]);
  const spdSt = await wSpd;
  ok('machine-config STATE reflects APPLIED user_speed', spdSt != null,
     spdSt ? 'state user_speed=' + spdSt.user_speed : 'NO on-change update seen');
  // RESTORE user_speed.
  const rSpd = await s.sendConfigSet({ 3: Math.round(typeof origSpd === 'number' ? origSpd : 50) });
  ok('user_speed restore ECHO applied original', near(rSpd.applied[3], origSpd),
     'applied user_speed=' + rSpd.applied[3]);

  s.close(); // clean GOODBYE — releases the session cleanly (field bug #3)
  await delay(600);

  // ---- Session 2: back-to-back (no reboot) — prove restore stuck + relive ---
  const { session: s2, cfg: cfg2 } = await openAndRead('2');
  ok('back-to-back session re-welcomes + reads', typeof cfg2.window_min === 'number');
  ok('window restored across sessions', near(cfg2.window_min, origMin) && near(cfg2.window_max, origMax),
     'window=[' + cfg2.window_min + ', ' + cfg2.window_max + ']  orig=[' + origMin + ', ' + origMax + ']');
  s2.close();
  await delay(300);

  console.log('\n' + (failures === 0 ? 'ALL PASS' : failures + ' FAILURE(S)'));
  process.exit(failures === 0 ? 0 : 1);
}

main().catch((e) => { console.error('  [FAIL] test threw:', e && e.message); process.exit(1); });
