/**
 * wire-forensics.mjs — hunts WRONG VALUES in the motion STATE feed itself.
 *
 * Operator evidence (2026-07-29): the telemetry chart — which plots RAW wire
 * samples, no interpolation — shows square pulses: pos jumps to a flat wrong
 * level for a tick and returns. That is a decode- or firmware-side defect,
 * not a rendering one. This records COMPLETE decoded samples during motion
 * and correlates each outlier against every other field of its own and
 * neighboring samples to identify WHAT the wrong value actually is.
 *
 * Waits up to 15 minutes for motion to start, then records 15 s.
 * COMMANDS NO MOTION. Run: node webui/test/wire-forensics.mjs [host]
 */
import { createSession, CH, PRIORITY } from '../../../SlopSync/clients/js/index.js';

const HOST = process.argv[2] || '192.168.1.229';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const samples = [];
const watcher = createSession({
  host: HOST, port: 82, clientKind: 'webui', clientName: 'wire forensics',
  autoReconnect: false, subscriptions: [[CH.MOTION, 25.0, PRIORITY.elevated]],
});
watcher.on('state', (ch, sample) => {
  if (ch === CH.MOTION) samples.push({ t: Date.now(), ...sample });
});
watcher.connect();

console.log('waiting for motion (up to 15 min)…');
let movingSince = 0;
const t0 = Date.now();
while (Date.now() - t0 < 900000) {
  await sleep(500);
  const recent = samples.filter((s) => s.t > Date.now() - 1500);
  if (recent.length >= 5) {
    const posz = recent.map((s) => s.pos_10um);
    if (Math.max(...posz) - Math.min(...posz) > 2000) { movingSince = Date.now(); break; }
  }
}
if (!movingSince) { console.log('NO MOTION SEEN — start a pattern and rerun.'); process.exit(2); }

console.log('motion detected — recording 15 s…');
samples.length = 0;
await sleep(15000);
watcher.close?.();

const N = samples.length;
console.log('recorded', N, 'samples. fields:', Object.keys(samples[0] || {}).join(', '));

// Robust outlier hunt on pos and tgt: compare to the median of ±3 neighbors.
function median(a) { const s = [...a].sort((x, y) => x - y); return s[Math.floor(s.length / 2)]; }
const OUT_10UM = 3000; // 3 mm from local median = outlier
let outliers = 0;
for (let i = 3; i < N - 3; i++) {
  for (const key of ['pos_10um', 'tgt_10um']) {
    const v = samples[i][key];
    if (typeof v !== 'number') continue;
    const neigh = [];
    for (let k = i - 3; k <= i + 3; k++) if (k !== i) neigh.push(samples[k][key]);
    const med = median(neigh);
    if (Math.abs(v - med) > OUT_10UM) {
      outliers++;
      if (outliers <= 12) {
        console.log('\nOUTLIER #' + outliers, key, 'sample', i, 'value', v, 'localMedian', med, 'delta', v - med);
        console.log('  full sample:', JSON.stringify(samples[i]));
        console.log('  prev:', JSON.stringify(samples[i - 1]));
        console.log('  next:', JSON.stringify(samples[i + 1]));
        // What does the wrong value equal? Check every numeric field nearby.
        const suspects = [];
        for (let k = Math.max(0, i - 5); k <= Math.min(N - 1, i + 1); k++) {
          for (const [f, fv] of Object.entries(samples[k])) {
            if (f === key || f === 't' || typeof fv !== 'number') continue;
            if (Math.abs(fv - v) < 200) suspects.push(`${f}@${k - i}`);
          }
        }
        console.log('  value matches (±2mm):', suspects.length ? suspects.join(', ') : 'nothing nearby');
      }
    }
  }
}
console.log('\nTOTAL outliers:', outliers, 'of', N, 'samples');
process.exit(0);
