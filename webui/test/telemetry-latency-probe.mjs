/**
 * telemetry-latency-probe.mjs — measure the ACTUAL STATE push cadence of the
 * position telemetry channel against a live device, to diagnose the "rail
 * position readout is laggy" complaint.
 *
 * This mirrors machine.svelte.js's subscription policy byte-for-byte (wish
 * min(catalog maxRateHz, DRAW_HZ=30) for every h2c STATE/EVENT channel,
 * batched to the hub's max_subscriptions_per_frame, dropped to
 * max_subscriptions by priority when oversubscribed) rather than subscribing
 * to just the one channel we care about — the whole point is to see whether
 * the motion channel gets shed or paced behind the other ~30 diagnostic
 * channels a real page opens, per SPEC 10.4 congestion shedding.
 *
 * The channel under measurement is found by ROLE (telemetry.position), the
 * same mechanism RailWidget uses — no hardcoded channel id.
 *
 * Read-only: subscribes, never sends an intent.
 *
 * Run: node webui/test/telemetry-latency-probe.mjs [host] [port] [seconds]
 */

import { createSession, CHANNEL_CLASS, PRIORITY } from '../src/core/slopsync/index.js';
import { acquireToken } from '../src/core/slopsync/credentials.js';
import { buildSettingsModel } from '../src/model/settings.js';
import { ROLE } from '../src/model/roles.js';

const HOST = process.argv[2] || '192.168.1.229';
const PORT = parseInt(process.argv[3] || '82', 10);
const DURATION_S = parseFloat(process.argv[4] || '20');
const DRAW_HZ = 30; // matches machine.svelte.js

if (typeof WebSocket === 'undefined') {
  console.error('No global WebSocket (need node >= 22). Aborting.');
  process.exit(1);
}

// ---- mirror of machine.svelte.js's subscriptionWishes()/subscribeInBatches() ----
function subscriptionWishes(entries, maxSubs) {
  const wishes = [];
  for (const e of entries) {
    if (e.dir !== 0) continue;
    if (e.cls !== CHANNEL_CLASS.STATE && e.cls !== CHANNEL_CLASS.EVENT) continue;
    const rate = (e.cls === CHANNEL_CLASS.EVENT || !e.maxRateHz) ? 0 : Math.min(e.maxRateHz, DRAW_HZ);
    wishes.push([e.id, rate, e.priority != null ? e.priority : PRIORITY.background]);
  }
  const cap = (typeof maxSubs === 'number' && maxSubs > 0) ? maxSubs : wishes.length;
  if (wishes.length <= cap) return wishes;
  const ranked = wishes.slice().sort((a, b) => b[2] - a[2]);
  return ranked.slice(0, cap);
}

const s = createSession({
  host: HOST, port: PORT, clientKind: 'webui', clientName: 'latency-probe',
  autoReconnect: false, WebSocketImpl: WebSocket,
  token: (h) => acquireToken(h),
});

let welcomeLimits = {};
let catalogEntries = null;
let posChannelId = null;
let grantedRate = null;
let grantedPriority = null;
const nacks = [];
const arrivals = []; // client Date.now() at each 'state' event for the pos channel
let totalStatePushesAllChannels = 0;
const perChannelCounts = new Map();

s.on('welcome', (w) => { welcomeLimits = w.limits || {}; });
s.on('nack', (n) => { nacks.push(n); });

const catalogP = new Promise((resolve, reject) => {
  const to = setTimeout(() => reject(new Error('timeout waiting for catalog')), 20000);
  s.on('catalog', (entries) => { clearTimeout(to); resolve(entries); });
  s.on('close', (c) => { clearTimeout(to); reject(new Error('closed before catalog: ' + (c.reason || c.code))); });
});

s.on('grant', (grants) => {
  for (const g of grants || []) {
    if (g.channel === posChannelId) { grantedRate = g.rate; grantedPriority = g.priority; }
  }
});

s.on('state', (channelId, sample, tsMs) => {
  totalStatePushesAllChannels++;
  perChannelCounts.set(channelId, (perChannelCounts.get(channelId) || 0) + 1);
  if (channelId === posChannelId) arrivals.push(tsMs);
});

s.connect();
catalogEntries = await catalogP;

const model = buildSettingsModel(catalogEntries);
const posList = model.byRole.get(ROLE.telemetryPosition);
if (!posList || !posList.length) {
  console.error('This catalog has no telemetry.position role — cannot measure. Aborting.');
  s.close();
  process.exit(1);
}
posChannelId = posList[0].channelId;
const posEntry = catalogEntries.find((e) => e.id === posChannelId);

console.log('host                : ' + HOST + ':' + PORT);
console.log('entries             : ' + catalogEntries.length);
console.log('position channel    : id 0x' + posChannelId.toString(16).padStart(4, '0')
  + '  name=' + posEntry.name + '  catalog maxRateHz=' + posEntry.maxRateHz
  + '  priority=' + posEntry.priority);
console.log('WELCOME limits      : max_subscriptions=' + welcomeLimits.max_subscriptions
  + '  max_subscriptions_per_frame=' + welcomeLimits.max_subscriptions_per_frame);

const wishes = subscriptionWishes(catalogEntries, welcomeLimits.max_subscriptions);
const posWish = wishes.find((w) => w[0] === posChannelId);
console.log('subscribing to      : ' + wishes.length + ' channels (mirrors machine.svelte.js policy)');
console.log('position wish       : ' + (posWish ? ('rate=' + posWish[1] + ' priority=' + posWish[2]) : 'DROPPED BY CAP (not in wish batch!)'));

const perFrame = (typeof welcomeLimits.max_subscriptions_per_frame === 'number' && welcomeLimits.max_subscriptions_per_frame > 0)
  ? welcomeLimits.max_subscriptions_per_frame : 8;
for (let i = 0; i < wishes.length; i += perFrame) {
  s.subscribe(wishes.slice(i, i + perFrame));
}

console.log('\nmeasuring for ' + DURATION_S + 's ...\n');
await new Promise((r) => setTimeout(r, DURATION_S * 1000));

console.log('=== RESULT ===');
console.log('granted rate for position channel : ' + (grantedRate == null ? 'NO GRANT SEEN' : grantedRate + ' Hz') + '  priority=' + grantedPriority);
console.log('total STATE pushes (all channels)  : ' + totalStatePushesAllChannels);
console.log('distinct channels pushed           : ' + perChannelCounts.size);
console.log('SUBSCRIBE_REJECTED / other NACKs   : ' + nacks.length);
for (const n of nacks) console.log('  NACK ' + JSON.stringify(n));

if (arrivals.length < 3) {
  console.log('\nposition samples received: ' + arrivals.length + ' — TOO FEW to measure cadence.');
} else {
  const intervals = [];
  for (let i = 1; i < arrivals.length; i++) intervals.push(arrivals[i] - arrivals[i - 1]);
  intervals.sort((a, b) => a - b);
  const sum = intervals.reduce((a, b) => a + b, 0);
  const mean = sum / intervals.length;
  const p50 = intervals[Math.floor(intervals.length * 0.5)];
  const p95 = intervals[Math.floor(intervals.length * 0.95)];
  const min = intervals[0];
  const max = intervals[intervals.length - 1];
  const wallSpanS = (arrivals[arrivals.length - 1] - arrivals[0]) / 1000;
  const observedHz = (arrivals.length - 1) / wallSpanS;

  console.log('\nposition channel STATE cadence (n=' + arrivals.length + ' samples over ' + wallSpanS.toFixed(2) + 's):');
  console.log('  observed rate        : ' + observedHz.toFixed(2) + ' Hz');
  console.log('  mean inter-arrival   : ' + mean.toFixed(1) + ' ms');
  console.log('  median (p50)         : ' + p50.toFixed(1) + ' ms');
  console.log('  p95                  : ' + p95.toFixed(1) + ' ms');
  console.log('  min / max            : ' + min.toFixed(1) + ' / ' + max.toFixed(1) + ' ms');
  console.log('  expected @ granted   : ' + (grantedRate ? (1000 / grantedRate).toFixed(1) + ' ms' : 'n/a'));

  // per-channel breakdown, to see whether pos is being paced behind others
  console.log('\nper-channel push counts (top 10):');
  const ranked = [...perChannelCounts.entries()].sort((a, b) => b[1] - a[1]).slice(0, 10);
  for (const [chId, cnt] of ranked) {
    const e = catalogEntries.find((x) => x.id === chId);
    const mark = chId === posChannelId ? '  <-- position' : '';
    console.log('  0x' + chId.toString(16).padStart(4, '0') + '  ' + (e ? e.name : '?').padEnd(20) + ' ' + cnt + mark);
  }
}

s.close();
setTimeout(() => process.exit(0), 300);
