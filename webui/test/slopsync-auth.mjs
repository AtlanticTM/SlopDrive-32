/**
 * slopsync-auth.mjs — live proof of the credential ladder (credentials.js).
 *
 * Answers one question against the real device: DOES THE TOKEN CHANGE THE
 * ANSWER? It opens three sessions and prints the role each was granted:
 *
 *   1. tokenless          — the viewer case
 *   2. /uitoken mint      — the WebUI's normal path
 *   3. replay of #2's token — must FAIL, /uitoken is single-use
 *
 * BEFORE the enforcement flip, 1 and 2 both come back `control` and that is
 * correct-and-expected: validateToken() returns control unconditionally today.
 * AFTER the flip, 1 must be `watch` and 2 must be `control`. That difference IS
 * the test — run it on both sides of the change.
 *
 * Read-only: no intents, no motion, nothing written to the device.
 *
 * Run:  node webui/test/slopsync-auth.mjs [--ip 192.168.1.229] [--port 82]
 */

import { createSession, ACCESS, ACCESS_NAME } from '../../../SlopSync/clients/js/index.js';
import { mintUiToken, acquireToken } from '../../../SlopSync/clients/js/credentials.js';
import { getInstanceId } from '../../../SlopSync/clients/js/identity.js';

const args = process.argv.slice(2);
const argOf = (f, d) => { const i = args.indexOf(f); return i >= 0 ? args[i + 1] : d; };
const IP = argOf('--ip', '192.168.1.229');
const PORT = parseInt(argOf('--port', '82'), 10);
// Which posture are we asserting? Pre-flip, validateToken() returns control
// unconditionally, so a tokenless HELLO and a replayed token BOTH come back
// `control` and that is correct behavior to assert. Post-flip both must be
// `watch`. Same script, same device, one flag — so the flip is provable rather
// than assumed.
const ENFORCED = args.includes('--expect-enforced');

if (typeof WebSocket === 'undefined') {
  console.error('No global WebSocket (need node >= 22). Aborting.');
  process.exit(1);
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const results = [];
const pass = (m) => { console.log('  PASS ' + m); results.push(true); };
const fail = (m) => { console.log('  FAIL ' + m); results.push(false); };
const info = (m) => console.log('       ' + m);

/**
 * Open one session with an explicit credential and report the granted role.
 * autoReconnect is OFF: a reconnect would mint a SECOND token and muddy the
 * single-use replay check below.
 */
function roleFor(label, tokenOrProvider) {
  return new Promise((resolve) => {
    const s = createSession({
      host: IP,
      port: PORT,
      clientKind: 'test',
      clientName: 'slopsync-auth ' + label,
      instanceId: getInstanceId(),
      token: tokenOrProvider,
      autoReconnect: false,
      subscriptions: [],
    });
    let done = false;
    const finish = (r) => {
      if (done) return;
      done = true;
      try { s.close(); } catch (e) { /* already gone */ }
      resolve(r);
    };
    s.on('welcome', (w) => finish({ ok: true, roles: w.roles }));
    s.on('close', () => finish({ ok: false, reason: 'closed before WELCOME' }));
    setTimeout(() => finish({ ok: false, reason: 'timeout' }), 6000);
    s.connect();
  });
}

const roleName = (r) => (ACCESS_NAME && ACCESS_NAME[r]) || String(r);

async function main() {
  console.log('slopsync-auth  device=' + IP + ':' + PORT);
  console.log('instance_id=' + Buffer.from(getInstanceId()).toString('hex'));

  // ---- 1. tokenless -------------------------------------------------------
  console.log('\n--- 1. tokenless HELLO ---');
  const r1 = await roleFor('tokenless', null);
  if (!r1.ok) {
    fail('tokenless session: ' + r1.reason);
  } else {
    info('roles = ' + r1.roles + ' (' + roleName(r1.roles) + ')');
    pass('tokenless session established');
    // A tokenless client must ALWAYS be able to connect and watch. Refusing it
    // outright would break the "anyone in the room can e-stop" property, which
    // outranks authorization by design (RFC-025b).
    if (ENFORCED) {
      if (r1.roles < ACCESS.control) pass('tokenless demoted to ' + roleName(r1.roles) + ' — enforcement is on');
      else fail('tokenless still got ' + roleName(r1.roles) + ' — enforcement did NOT take');
    } else if (r1.roles >= ACCESS.control) {
      info('pre-enforcement: control-for-all, as expected');
    }
  }

  await sleep(1200); // /uitoken is rate limited device-wide (kMinIntervalMs)

  // ---- 2. /uitoken --------------------------------------------------------
  console.log('\n--- 2. /uitoken mint ---');
  const tok = await mintUiToken(IP);
  if (!tok) {
    fail('/uitoken mint failed (endpoint disabled, rate limited, or unreachable)');
  } else {
    info('minted token = ' + Buffer.from(tok).toString('hex'));
    pass('/uitoken minted 16 bytes');
    const r2 = await roleFor('uitoken', tok);
    if (!r2.ok) fail('uitoken session: ' + r2.reason);
    else {
      info('roles = ' + r2.roles + ' (' + roleName(r2.roles) + ')');
      if (r2.roles >= ACCESS.control) pass('/uitoken granted control tier');
      else fail('/uitoken did NOT grant control (got ' + roleName(r2.roles) + ')');
    }

    // ---- 3. replay ---------------------------------------------------------
    // The mint is consumed by the hub the moment it validates. Replaying it
    // must NOT grant control again, or a page could hoard one credential and
    // keep using it after the LAN-trust posture is tightened.
    console.log('\n--- 3. replay of the SAME token (must not re-grant) ---');
    const r3 = await roleFor('replay', tok);
    if (!r3.ok) {
      pass('replayed token was refused outright (' + r3.reason + ')');
    } else {
      info('roles = ' + r3.roles + ' (' + roleName(r3.roles) + ')');
      if (r3.roles >= ACCESS.control) {
        // Pre-flip this is EXPECTED: the unconditional-control fallback catches
        // the burnt token, so single-use is real but not OBSERVABLE from the
        // wire. Post-flip it is a real failure.
        if (ENFORCED) fail('replayed token still granted control — single-use is NOT holding');
        else { info('pre-enforcement: the control-for-all fallback masks this'); pass('replay reached WELCOME (single-use not observable pre-flip)'); }
      } else {
        pass('replayed token demoted to ' + roleName(r3.roles) + ' — single-use holds');
      }
    }
  }

  // ---- 4. the ladder end to end -------------------------------------------
  await sleep(1200);
  console.log('\n--- 4. acquireToken() ladder ---');
  const laddered = await acquireToken(IP, { log: (lvl, ...a) => info(lvl + ': ' + a.join(' ')) });
  if (laddered) pass('ladder produced a credential (' + laddered.length + ' bytes)');
  else fail('ladder produced nothing');

  const bad = results.filter((r) => !r).length;
  console.log('\n' + (bad ? 'FAIL' : 'PASS') + ' — ' + (results.length - bad) + '/' + results.length);
  process.exit(bad ? 1 : 0);
}

main().catch((e) => { console.error(e); process.exit(1); });
