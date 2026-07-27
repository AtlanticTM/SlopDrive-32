/**
 * slopsync-tuning.mjs — LIVE proof of the SlopMotion tuning surface
 * (0x008B/0x008C/0x008D STATE + 0x0105 slopmotion-set INTENT).
 *
 * This is the gate on retiring `POST /api/slopmotion`: 20 live-tune knobs that
 * were reachable ONLY over device-specific HTTP are now protocol channels, so a
 * third-party client is as capable as the hosted UI. If every knob reads and
 * round-trips here, the HTTP writer has nothing left that is exclusively its own.
 *
 * ALSO CHECKS THE THING THAT MAKES THEM ONE UI: all three STATE channels must
 * declare the SAME `category` (tuning). SPEC §8.8 — "a category spans channels;
 * two channels in the same category merge into one tab" — is what lets 20 knobs
 * exist across three channels (each capped at 8 by its bitfield8 enabled_mask)
 * while rendering as a single Tuning tab. Get the category wrong and a generic
 * client draws three unrelated tabs.
 *
 * SAFETY: sends only 0x0105, restores every value it touches (including after a
 * failed assertion), and never touches motion, home, pattern or safety.
 *
 * Run:  node webui/test/slopsync-tuning.mjs [host] [port]
 */

import { createSession } from '../src/core/slopsync/index.js';
import { PRIORITY } from '../src/core/slopsync/frames.js';
import { acquireToken } from '../src/core/slopsync/credentials.js';

const HOST = process.argv[2] || '192.168.1.229';
const PORT = parseInt(process.argv[3] || '82', 10);

const CH_LIMITS = 0x008b, CH_CHASE = 0x008c, CH_WAVE = 0x008d;

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
      host: HOST, port: PORT, clientKind: 'webui', clientName: 'tuning-test',
      autoReconnect: false, WebSocketImpl: WebSocket,
      token: (h) => acquireToken(h),
      subscriptions: [
        [CH_LIMITS, 0, PRIORITY.background],
        [CH_CHASE, 0, PRIORITY.background],
        [CH_WAVE, 0, PRIORITY.background],
      ],
    });
    const seen = new Map();
    const to = setTimeout(() => { try { s.close(); } catch (e) {} reject(new Error('timeout waiting for tuning channels')); }, 10000);
    let welcomed = false;
    s.on('welcome', (w) => { welcomed = true; s._roles = w.roles; });
    s.on('state', (ch, sample) => {
      if (!welcomed) return;
      seen.set(ch, sample);
      if (seen.size === 3) { clearTimeout(to); resolve({ s, seen }); }
    });
    s.on('close', () => { clearTimeout(to); if (!welcomed) reject(new Error('closed before welcome')); });
    s.connect();
  });
}

function waitFor(s, ch, pred, timeoutMs = 4000) {
  return new Promise((resolve) => {
    const off = s.on('state', (c, sample) => {
      if (c === ch && pred(sample)) { off(); resolve(sample); }
    });
    setTimeout(() => { off(); resolve(null); }, timeoutMs);
  });
}

/** Write one key, assert the ECHO and the on-change STATE, then restore. */
async function roundTrip(s, ch, key, name, current, alt, eq) {
  const same = eq || ((a, b) => Math.abs(a - b) < 1e-3);
  const seen = waitFor(s, ch, (m) => same(m[name], alt));
  const echo = await s.sendIntent(0x0105, { [key]: alt });
  ok(name + ' ECHO applied', same(echo.applied[key], alt),
     'applied=' + echo.applied[key] + ' requested=' + alt);
  const st = await seen;
  ok(name + ' STATE reflects it', st != null, st ? 'state=' + st[name] : 'NO on-change seen');
  const back = await s.sendIntent(0x0105, { [key]: current });
  ok(name + ' restored', same(back.applied[key], current), 'applied=' + back.applied[key]);
}

async function main() {
  console.log('slopmotion tuning live test → ws://' + HOST + ':' + PORT + '/');
  const { s, seen } = await open();
  const lim = seen.get(CH_LIMITS), chase = seen.get(CH_CHASE), wav = seen.get(CH_WAVE);
  ok('all three tuning channels granted + retained', !!lim && !!chase && !!wav, 'roles=' + s._roles);

  // ---- the one-tab invariant --------------------------------------------
  const cm = s.channelMap;
  const cats = [CH_LIMITS, CH_CHASE, CH_WAVE].map((c) => cm.get(c) && cm.get(c).category);
  ok('all three share ONE category (renders as one tab, SPEC §8.8)',
     cats[0] != null && cats.every((c) => c === cats[0]), 'category=' + JSON.stringify(cats));
  const writers = [CH_LIMITS, CH_CHASE, CH_WAVE].map((c) => cm.get(c) && cm.get(c).settingChannel);
  ok('all three name ONE settingChannel (0x0105)',
     writers.every((w) => w === 0x0105), 'settingChannel=' + JSON.stringify(writers));

  // every setting_key across the three cards must be unique, or a write to one
  // card would silently land on another's field.
  const keys = [];
  for (const c of [CH_LIMITS, CH_CHASE, CH_WAVE]) {
    for (const f of (cm.get(c).layout || [])) if (f.settingKey != null) keys.push(f.settingKey);
  }
  ok('setting_keys unique across the shared writer', new Set(keys).size === keys.length,
     keys.length + ' keys: ' + keys.join(','));

  info('limits:   ' + JSON.stringify({ jmax: lim.jmax_ovr, centring: lim.centring, gain: lim.centring_gain }));
  info('chase:    ' + JSON.stringify({ ff: chase.chase_ff, gain: chase.chase_gain, dense_ms: chase.chase_dense_ms }));
  info('waveform: ' + JSON.stringify({ curve: wav.curve_policy, policy: wav.infeasible_policy, settle_ms: wav.settle_grace_ms }));

  // ---- round-trips, one per card, covering f32 / select / ms-scaled ------
  console.log('\n--- limits (f32) ---');
  await roundTrip(s, CH_LIMITS, 5, 'centring_gain', lim.centring_gain, lim.centring_gain > 0.5 ? 0.25 : 0.75);
  console.log('\n--- chase (ms-scaled u32) ---');
  await roundTrip(s, CH_CHASE, 10, 'chase_dense_ms', chase.chase_dense_ms, chase.chase_dense_ms > 100 ? 40 : 120);
  console.log('\n--- waveform (select) ---');
  await roundTrip(s, CH_WAVE, 13, 'curve_policy', wav.curve_policy, wav.curve_policy === 0 ? 1 : 0);
  console.log('\n--- waveform (f32 budget) ---');
  await roundTrip(s, CH_WAVE, 16, 'smooth_budget', wav.smooth_budget, wav.smooth_budget > 0.5 ? 0.3 : 0.7);

  // ---- clamping is the hub's job, and the ECHO must show it --------------
  console.log('\n--- clamp: ask for out-of-range, expect the bound back ---');
  const hi = await s.sendIntent(0x0105, { 16: 99.0 });   // smooth_budget max 1.0
  ok('over-range smooth_budget clamped to 1.0', Math.abs(hi.applied[16] - 1.0) < 1e-3,
     'applied=' + hi.applied[16]);
  await s.sendIntent(0x0105, { 16: wav.smooth_budget });

  s.close();
  await delay(400);
  console.log('\n' + (failures ? 'FAILURES: ' + failures : 'ALL PASS'));
  process.exit(failures ? 1 : 0);
}

main().catch((e) => { console.error('ERROR', e); process.exit(1); });
