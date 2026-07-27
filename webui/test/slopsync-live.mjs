/**
 * slopsync-live.mjs — READ-ONLY live handshake proof for slopsync-js.
 *
 * Connects to the real device hub, runs a full read-only session, and prints a
 * narrated transcript. NO intents, NO writes to the device — this only
 * observes. Runs the session TWICE back-to-back (mandatory regression pattern —
 * SlopSync field bug #3: source-ownership teardown must survive same-instance
 * re-HELLO without a reboot between runs).
 *
 * Run:  node webui/test/slopsync-live.mjs [--ip 192.168.1.229] [--port 82]
 * Uses node's built-in WebSocket (node ≥22). Exits 1 on any failure.
 */

import { createSession, CH, PRIORITY, nackName } from '../src/core/slopsync/index.js';

const args = process.argv.slice(2);
function argOf(flag, def) { const i = args.indexOf(flag); return i >= 0 ? args[i + 1] : def; }
const IP = argOf('--ip', '192.168.1.229');
const PORT = parseInt(argOf('--port', '82'), 10);

if (typeof WebSocket === 'undefined') {
  console.error('No global WebSocket (need node >= 22). Aborting.');
  process.exit(1);
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function runOnce(runLabel) {
  console.log('\n===== ' + runLabel + ' =====');
  const results = [];
  const pass = (m) => { console.log('  PASS ' + m); results.push(true); };
  const fail = (m) => { console.log('  FAIL ' + m); results.push(false); };
  const info = (m) => console.log('      ' + m);

  const seen = {
    welcome: null,
    catalog: null,
    grantChannels: new Set(),
    safetyState: null,
    motionSamples: [],
    hubStatus: null,
    nacks: [],
  };

  const s = createSession({
    host: IP,
    port: PORT,
    clientKind: 'webui',
    clientName: 'slopsync-js live-test',
    autoReconnect: false, // one clean shot per run
    subscriptions: [
      [CH.SAFETY, 0.0, PRIORITY.critical],
      [CH.MOTION, 20.0, PRIORITY.elevated],
      [CH.HUB_STATUS, 1.0, PRIORITY.background],
    ],
  });

  s.on('welcome', (w) => { seen.welcome = w; });
  s.on('catalog', (entries) => { seen.catalog = entries; });
  s.on('grant', (grants) => { for (const g of grants) seen.grantChannels.add(g.channel); });
  s.on('nack', (n) => { seen.nacks.push(n); });
  s.on('state', (ch, sample) => {
    if (ch === CH.SAFETY && !seen.safetyState) seen.safetyState = sample;
    else if (ch === CH.MOTION) seen.motionSamples.push(sample);
    else if (ch === CH.HUB_STATUS && !seen.hubStatus) seen.hubStatus = sample;
  });

  // connect + wait for WELCOME (timeout 6 s)
  let welcomeResolve;
  const welcomeP = new Promise((r) => { welcomeResolve = r; });
  s.on('welcome', () => welcomeResolve());
  let closed = null;
  s.on('close', (c) => { closed = c; });

  s.connect();
  await Promise.race([welcomeP, sleep(6000)]);

  if (closed) { fail('socket closed before WELCOME (device unreachable?)'); return { results, unreachable: true }; }
  if (!seen.welcome) { fail('no WELCOME within 6 s (device unreachable at ' + IP + ':' + PORT + '?)'); s.close(); return { results, unreachable: true }; }

  const w = seen.welcome;
  pass('WELCOME: session=' + w.sessionId + ' boot=0x' + Number(w.bootId).toString(16) +
       ' cfg_gen=' + w.cfgGen + ' roles=' + w.roles + ' deadman=' + w.deadmanMs + 'ms');
  info('limits=' + JSON.stringify(w.limits) + ' etag=' + (w.catalogEtag ? Buffer.from(w.catalogEtag).toString('hex') : '-'));

  // CLOCK sync (explicit; the session also fired one on WELCOME)
  const clk = await s.syncClock(3000);
  if (clk) pass('CLOCK sync: offset ~' + clk.offsetUs + 'us rtt ~' + clk.rttUs + 'us');
  else fail('CLOCK exchange got no reply');

  // let catalog + grants + retained + live STATE flow (~2.5 s)
  await sleep(2500);

  // CATALOG
  if (seen.catalog && seen.catalog.length) {
    pass('CATALOG decoded: ' + seen.catalog.length + ' channels');
    const names = seen.catalog.map((e) => '0x' + e.id.toString(16).padStart(4, '0') + ':' + e.name).join(', ');
    info(names);
    const motionEntry = seen.catalog.find((e) => e.id === CH.MOTION);
    if (motionEntry && motionEntry.layout) {
      info('catalog 0x0080 layout: ' + motionEntry.layout.map((f) => f.name + '(' + f.typeName + '/' + f.scale + ')').join(', '));
    }
  } else {
    fail('no CATALOG decoded within window');
  }

  // GRANTs
  if (seen.grantChannels.has(CH.SAFETY) && seen.grantChannels.has(CH.MOTION)) {
    pass('GRANTs applied for safety(0x0003) + motion(0x0080)' + (seen.grantChannels.has(CH.HUB_STATUS) ? ' + hub-status(0x0006)' : ''));
  } else {
    fail('missing GRANT — got channels: ' + [...seen.grantChannels].map((c) => '0x' + c.toString(16)).join(','));
  }

  // retained safety on grant (§9.1)
  if (seen.safetyState) {
    pass('retained safety(0x0003) STATE observed on grant (§9.1)');
    info('safety=' + JSON.stringify(seen.safetyState));
  } else {
    fail('safety(0x0003) retained STATE never arrived (violates §9.1)');
  }

  // ≥10 motion STATE samples, sane range
  if (seen.motionSamples.length >= 10) {
    pass('motion(0x0080) STATE: ' + seen.motionSamples.length + ' samples received (>=10)');
    // catalog field names (per include/comms/SlopSyncCatalog.h): pos_10um /
    // tgt_10um are scaled to mm, speed to mm/s. Value is mm despite the label.
    let sane = true;
    for (const m of seen.motionSamples) {
      if (!(m.pos_10um >= 0 && m.pos_10um <= 500 && m.tgt_10um >= 0 && m.tgt_10um <= 500)) sane = false;
    }
    if (sane) pass('all pos/tgt within sane range [0, 500] mm'); else fail('a pos/tgt sample out of [0,500] mm range');
    for (const m of seen.motionSamples.slice(0, 3)) {
      info('sample: pos=' + m.pos_10um.toFixed(2) + 'mm tgt=' + m.tgt_10um.toFixed(2) + 'mm speed=' +
           m.speed.toFixed(1) + 'mm/s flags=0x' + m.flags.toString(16).padStart(2, '0') +
           ' ' + JSON.stringify(m.flags_bits));
    }
  } else {
    fail('only ' + seen.motionSamples.length + ' motion STATE samples (<10)');
  }

  if (seen.hubStatus) info('hub-status: ' + JSON.stringify(seen.hubStatus));
  if (seen.nacks.length) info('NACKs seen: ' + seen.nacks.map((n) => n.name).join(', '));

  // GOODBYE + close
  s.close();
  await sleep(300);
  pass('GOODBYE sent + socket closed');

  return { results, unreachable: false };
}

(async () => {
  console.log('slopsync-js LIVE read-only handshake proof against ws://' + IP + ':' + PORT + ' (subprotocol slopsync.v1)');
  let allPass = true;
  let unreachable = false;

  for (const label of ['RUN 1', 'RUN 2 (back-to-back, no reboot — field-bug-#3 regression)']) {
    const { results, unreachable: ur } = await runOnce(label);
    if (ur) { unreachable = true; break; }
    if (results.some((r) => !r)) allPass = false;
    await sleep(600); // brief gap between sessions
  }

  console.log('');
  if (unreachable) {
    console.log('RESULT: device UNREACHABLE at ws://' + IP + ':' + PORT + ' — could not complete the live proof.');
    process.exit(2);
  }
  if (allPass) { console.log('RESULT: ALL PASS (both runs)'); process.exit(0); }
  console.log('RESULT: FAILURES present'); process.exit(1);
})();
