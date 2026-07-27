/**
 * slopsync-modes.mjs — LIVE proof of the 0x008A / 0x0104 MODE channel pair.
 *
 * These settings (blend, stream-speed, overshoot-clamp) were reachable ONLY over
 * the legacy HTTP/`:81` plane until M5b. They are the last SETTINGS that made
 * `:81` load-bearing, so this test is part of the gate on cutting it: if each
 * one reads and writes over SlopSync, the HTTP control surface has no settings
 * left that are exclusively its own.
 *
 * Flow, per setting: read device truth from the retained 0x008A → write a
 * DIFFERENT legal value via 0x0104 → assert the ECHO carries the applied value
 * → assert an on-change 0x008A reflects it → RESTORE the original → assert.
 *
 * SAFETY: sends only 0x0104. No move, no home, no pattern, no safety intent.
 * Every setting is restored, including after a failed assertion.
 *
 * `transport` (the old WS/SER/BT/DONGLE/OSSM input-source selector) was RETIRED
 * rather than tested: SlopSync is the only way in now and the hub listens on
 * WebSocket and BLE by default, so there is no mode left for an operator to
 * pick. Key 2 on 0x0104 is a permanent gap.
 *
 * Run:  node webui/test/slopsync-modes.mjs [host] [port]
 */

import { createSession, CH } from '../src/core/slopsync/index.js';
import { PRIORITY } from '../src/core/slopsync/frames.js';
import { acquireToken } from '../src/core/slopsync/credentials.js';

const HOST = process.argv[2] || '192.168.1.229';
const PORT = parseInt(process.argv[3] || '82', 10);

let failures = 0;
const ok = (name, cond, extra) => {
  console.log('  [' + (cond ? 'PASS' : 'FAIL') + '] ' + name + (extra ? '  ' + extra : ''));
  if (!cond) failures++;
};
const info = (m) => console.log('  [info] ' + m);
const delay = (ms) => new Promise((r) => setTimeout(r, ms));

function open() {
  return new Promise((resolve, reject) => {
    const s = createSession({
      host: HOST, port: PORT, clientKind: 'webui', clientName: 'modes-test',
      autoReconnect: false, WebSocketImpl: WebSocket,
      token: (h) => acquireToken(h),
      subscriptions: [[CH.MACHINE_MODES, 0, PRIORITY.elevated]],
    });
    const to = setTimeout(() => { try { s.close(); } catch (e) {} reject(new Error('timeout waiting for 0x008A')); }, 8000);
    let welcomed = false;
    s.on('welcome', (w) => { welcomed = true; s._roles = w.roles; });
    s.on('state', (ch, sample) => {
      if (ch === CH.MACHINE_MODES && welcomed) { clearTimeout(to); resolve({ s, modes: sample }); }
    });
    s.on('close', () => { clearTimeout(to); if (!welcomed) reject(new Error('closed before welcome')); });
    s.connect();
  });
}

function waitModes(s, pred, timeoutMs = 4000) {
  return new Promise((resolve) => {
    const off = s.on('state', (ch, sample) => {
      if (ch === CH.MACHINE_MODES && pred(sample)) { off(); resolve(sample); }
    });
    setTimeout(() => { off(); resolve(null); }, timeoutMs);
  });
}

async function roundTrip(s, key, name, current, alt) {
  console.log('\n--- ' + name + ' (key ' + key + ') ---');
  info('device reports ' + name + ' = ' + current);
  const seen = waitModes(s, (m) => m[name] === alt);
  const echo = await s.sendModesSet({ [key]: alt });
  ok(name + ' ECHO carries the applied value', echo.applied[key] === alt,
     'applied=' + echo.applied[key] + ' requested=' + alt);
  const st = await seen;
  ok(name + ' on-change STATE reflects it', st != null && st[name] === alt,
     st ? 'state=' + st[name] : 'NO on-change 0x008A seen');
  // RESTORE — always, even if the assertions above failed, so a red test never
  // leaves the machine in a mode the operator did not choose.
  const back = await s.sendModesSet({ [key]: current });
  ok(name + ' restored to the original', back.applied[key] === current,
     'applied=' + back.applied[key]);
}

async function main() {
  console.log('slopsync modes live test → ws://' + HOST + ':' + PORT + '/  (0x008A / 0x0104)');
  const { s, modes } = await open();
  ok('WELCOME + retained 0x008A adopted', typeof modes.blend_mode === 'number',
     'roles=' + s._roles + ' blend=' + modes.blend_mode +
     ' stream=' + modes.stream_speed_mode + ' overshoot=' + modes.overshoot_clamp);
  ok('all three mode fields decoded from the catalog',
     typeof modes.stream_speed_mode === 'number' && typeof modes.overshoot_clamp === 'number');
  ok('retired `transport` field is GONE from the channel', modes.transport === undefined);
  ok('enabled_mask decoded as a bitfield', modes.enabled_mask_bits != null,
     JSON.stringify(modes.enabled_mask_bits));

  // blend is 1..3; pick any legal value that is not the current one.
  await roundTrip(s, 1, 'blend_mode', modes.blend_mode, modes.blend_mode === 1 ? 2 : 1);
  await roundTrip(s, 3, 'stream_speed_mode', modes.stream_speed_mode, modes.stream_speed_mode ? 0 : 1);
  await roundTrip(s, 4, 'overshoot_clamp', modes.overshoot_clamp, modes.overshoot_clamp ? 0 : 1);

  s.close();
  await delay(500);
  console.log('\n' + (failures ? 'FAILURES: ' + failures : 'ALL PASS'));
  process.exit(failures ? 1 : 0);
}

main().catch((e) => { console.error('ERROR', e); process.exit(1); });
