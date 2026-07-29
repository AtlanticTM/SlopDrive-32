/**
 * pairing-roundtrip.mjs — the FIRST end-to-end proof of the pairing flows
 * (SPEC §12.3): every layer was individually implemented and unit-tested
 * (lib M4B-12..26), but the full round trip had never run above the lib
 * level, and slopsim's `validateToken` was a control-for-all stub until
 * 2026-07-29 — paired tokens were unverifiable in-sim (recon + fix, same
 * date). Covers BOTH modes:
 *
 *   mode (c) push-to-pair: sim starts with --pairing-window (the sim twin of
 *     the device's three-power-cycle gesture); on a factory-fresh ledger the
 *     first bare knock is granted `configure` — possession is root.
 *   mode (a) knock-and-approve: the second joiner's knock parks; the
 *     configure session approves it over session-admin (0x0009).
 *
 * B is deliberately approved at CONFIGURE, not the UI's control default: in
 * the sim, bare sessions already float at `control`, so only a configure
 * grant PROVES the reconnect tier came from the trust-ledger rung of
 * validateToken rather than the convenience floor.
 *
 * Channel ids 0x0009/0x000A/0x000B are SPEC-CORE (CHANNEL-GRID.md), the same
 * standing as CH_SAFETY — not device knowledge.
 *
 * Run: node webui/test/pairing-roundtrip.mjs   (starts its own sim on :82)
 */
import { spawn } from 'node:child_process';
import { createSession, ACCESS, PRIORITY } from '../../../SlopSync/clients/js/index.js';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0;
const ok = (name, cond, extra) => {
  console.log('  [' + (cond ? 'PASS' : 'FAIL') + '] ' + name + (extra ? '  — ' + extra : ''));
  if (!cond) fails++;
};

const CH_ADMIN = 0x0009, CH_PENDING = 0x000A, CH_PAIR_EVENTS = 0x000B;

const sim = spawn('sim/slopsim/build/slopsim.exe',
  ['machine', '--headless', '--duration', '120', '--pairing-window'],
  { cwd: process.cwd(), stdio: 'ignore' });
await sleep(2500);

function mkSession(name, instByte, extra = {}) {
  return createSession({
    host: '127.0.0.1', port: 82, clientKind: 'webui', clientName: name,
    instanceId: new Uint8Array(8).fill(instByte), autoReconnect: false, ...extra,
  });
}
const connectAndWelcome = (s, label) => new Promise((resolve, reject) => {
  const t = setTimeout(() => reject(new Error(label + ': no WELCOME in 6s')), 6000);
  s.on('welcome', (w) => { clearTimeout(t); resolve(w); });
  s.connect();
});
const nextPairGrant = (s, label, ms = 5000) => new Promise((resolve, reject) => {
  const t = setTimeout(() => reject(new Error(label + ': no PAIR_GRANT in ' + ms + 'ms')), ms);
  s.on('pairGrant', (g) => { clearTimeout(t); resolve(g); });
});

try {
  // ---- mode (c): first knock in the presence window -------------------------
  const a1 = mkSession('pairing op (pre)', 0xA1);
  await connectAndWelcome(a1, 'A1');
  const grantA = nextPairGrant(a1, 'A push-to-pair');
  a1.sendPairReq();
  const gA = await grantA;
  ok('push-to-pair first knock on a fresh ledger grants configure', gA.role === ACCESS.configure,
     'role=' + gA.role);
  ok('grant carries a 16-byte token', gA.token instanceof Uint8Array && gA.token.length === 16);
  a1.close();
  await sleep(400);

  // ---- trust-ledger rung: A reconnects WITH the token -----------------------
  const a2 = mkSession('pairing op', 0xA1, {
    token: gA.token,
    subscriptions: [[CH_PENDING, 1.0, PRIORITY.normal], [CH_PAIR_EVENTS, 1.0, PRIORITY.normal]],
  });
  const wA2 = await connectAndWelcome(a2, 'A2');
  ok('token reconnect lands at configure (ledger rung, not the sim floor)',
     (wA2.roles | 0) === ACCESS.configure, 'roles=' + wA2.roles);

  let pending = null, knockEvt = null;
  a2.on('state', (ch, sample) => { if (ch === CH_PENDING) pending = sample; });
  // 'event' emits ONE argument with the channel inside it (session.js:1018).
  a2.on('event', (evt) => {
    const ch = evt && (evt.channelId ?? evt.channel ?? evt.ch);
    if (ch === CH_PAIR_EVENTS) knockEvt = evt;
  });

  // ---- mode (a): B knocks, parks --------------------------------------------
  const b1 = mkSession('joiner B', 0xB2);
  const wB1 = await connectAndWelcome(b1, 'B1');
  ok('bare session floats at the sim control floor', (wB1.roles | 0) === ACCESS.control,
     'roles=' + wB1.roles);
  const grantB = nextPairGrant(b1, 'B approve', 8000);
  b1.sendPairReq();
  await sleep(1200);
  ok('knock parked: pending-pairing STATE updated at the operator', pending != null,
     pending ? JSON.stringify(pending).slice(0, 120) : 'no sample');
  ok('knocked EVENT broadcast on pairing-events', knockEvt != null,
     knockEvt ? JSON.stringify(knockEvt).slice(0, 120) : 'none');

  // ---- operator approves over session-admin ---------------------------------
  const admin = a2.channelMap && a2.channelMap.get(CH_ADMIN);
  ok('session-admin advertised with schema', !!admin && Array.isArray(admin.schema));
  const fOp = admin.schema.find((f) => f.name === 'op');
  const fInst = admin.schema.find((f) => f.name === 'instance_id');
  const fRole = admin.schema.find((f) => f.name === 'role');
  const opIdx = fOp && fOp.options ? fOp.options.indexOf('pair_approve') : -1;
  ok('pair_approve op present', opIdx > 0,
     'ops=' + (fOp && fOp.options ? fOp.options.join(',') : '?'));
  const res = await a2.sendIntent(CH_ADMIN, {
    [fOp.key]: opIdx,
    [fInst.key]: new Uint8Array(8).fill(0xB2),
    [fRole.key]: ACCESS.configure, // see header: configure proves the ledger rung
  });
  ok('approve intent ECHO-confirmed', !!res && res.intentId != null);

  // ---- B granted + token reconnect ------------------------------------------
  const gB = await grantB;
  ok('B receives PAIR_GRANT at the approved tier', gB.role === ACCESS.configure, 'role=' + gB.role);
  b1.close();
  await sleep(400);
  const b2 = mkSession('joiner B', 0xB2, { token: gB.token });
  const wB2 = await connectAndWelcome(b2, 'B2');
  ok('B token reconnect lands at configure — the full round trip', (wB2.roles | 0) === ACCESS.configure,
     'roles=' + wB2.roles);
  a2.close(); b2.close();
} catch (e) {
  ok('round trip completed without harness error', false, String(e));
}

sim.kill();
console.log('\n' + (fails ? 'FAILURES: ' + fails
  : 'ALL PASS — knock-and-approve + push-to-pair round trips proven'));
process.exit(fails ? 1 : 0);
