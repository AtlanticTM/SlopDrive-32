/**
 * tap-to-move-live.mjs — END-TO-END live verification of the rail tap-to-move
 * tape (RFC-032 command.position / telemetry.target), against the real device.
 *
 * Proves the full chain:
 *   tap → move INTENT (role command.position, via sendCommand's shadow
 *   lifecycle) → post-clamp ECHO (data-shadow pending → confirmed) → the
 *   device's own telemetry.target follows → the UI renders it (tape cursor
 *   aria-valuenow + commanded/lag hero numerals).
 *
 * An INDEPENDENT wire watcher (second read-only SlopSync session from node)
 * observes tgt_10um on the motion channel the whole time, so "the device
 * state changed" is confirmed off-UI, not inferred from the page.
 *
 * COMMANDS REAL MOVES within the reported stroke window. Bench use only:
 * device fake-homed (home_override), operator-authorized. Actual-is-actual
 * ruling applies: reported position IS actual; no hardware inference here.
 *
 * The page must be loaded FROM THE DEVICE (same reason as browser-check.mjs:
 * /uitoken is same-origin-only; a localhost origin gets watch tier and the
 * tape correctly disables).
 *
 * Run: node webui/test/tap-to-move-live.mjs [host]
 */

import { chromium } from 'playwright';
import { mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import { createSession, CH, PRIORITY } from '../../../SlopSync/clients/js/index.js';

const HOST = process.argv[2] || '192.168.1.229';
const PAGE_URL = 'http://' + HOST + '/';
const OUT = join(fileURLToPath(new URL('.', import.meta.url)), 'evidence');
mkdirSync(OUT, { recursive: true });

const TOL_MM = 2.0;      // applied-vs-expected tolerance (window taps should not clamp)
const SETTLE_MS = 8000;  // max wait for tgt to reach the commanded value
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let fails = 0;
const ok = (name, cond, extra) => {
  console.log('  [' + (cond ? 'PASS' : 'FAIL') + '] ' + name + (extra ? '  — ' + extra : ''));
  if (!cond) fails++;
};
const info = (m) => console.log('      ' + m);

// ---- wire watcher: independent read-only session ---------------------------
const wire = { samples: [], welcome: false };
const watcher = createSession({
  host: HOST,
  port: 82,
  clientKind: 'webui',
  clientName: 'tap-to-move wire watcher',
  autoReconnect: false,
  subscriptions: [[CH.MOTION, 20.0, PRIORITY.elevated]],
});
watcher.on('welcome', () => { wire.welcome = true; });
watcher.on('state', (ch, sample) => {
  if (ch === CH.MOTION) wire.samples.push({ t: Date.now(), pos: sample.pos_10um, tgt: sample.tgt_10um });
});
watcher.connect();
await sleep(2500);
ok('wire watcher holds a live read-only session', wire.welcome && wire.samples.length > 0,
   wire.samples.length + ' motion samples pre-tap');

// ---- browser ----------------------------------------------------------------
const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
const pageErrors = [];
page.on('pageerror', (e) => pageErrors.push(String(e)));

await page.goto(PAGE_URL, { waitUntil: 'domcontentloaded', timeout: 20000 });
await page.waitForSelector('nav.tabs button', { timeout: 25000 });

// The tape goes .live only when the catalog resolved command.position AND the
// session minted control tier — wait for it rather than sleeping.
const tapeLive = await page.waitForSelector('.rail-tape-track.live', { timeout: 15000 })
  .then(() => true).catch(() => false);
if (!tapeLive) {
  const reason = await page.$eval('.rail-reason', (el) => el.textContent).catch(() => '(no reason element)');
  ok('input tape is LIVE (command.position resolved + control tier)', false, 'reason shown: ' + reason);
} else {
  ok('input tape is LIVE (command.position resolved + control tier)', true);
}

// Handoff item 2's stale-copy check: the role-less-hub fallback must NOT
// render against this catalog (the path itself stays — it is Tier-1 graceful
// absence for hubs that never tag the role).
const staleCopy = await page.$$eval('.rail-tape-micro', (els) =>
  els.map((e) => e.textContent).filter((t) => /no move intent/i.test(t)).length);
ok('role-less fallback copy absent on this catalog', staleCopy === 0);

// telemetry.target resolved → commanded + lag numerals exist (4 items).
const hnItems = await page.$$eval('.hero-numerals .hn-item', (els) =>
  els.map((e) => e.querySelector('.hn-label').textContent.trim()));
ok('commanded + lag numerals present (telemetry.target resolved)',
   hnItems.length === 4 && hnItems.some((l) => /lag/.test(l)), hnItems.join(' | '));

async function numerals() {
  return page.$$eval('.hero-numerals .hn-item .hn-val', (els) =>
    els.map((e) => parseFloat(e.textContent)));
}

if (tapeLive) {
  const track = await page.$('.rail-tape-track.live');
  const lo = parseFloat(await track.getAttribute('aria-valuemin'));
  const hi = parseFloat(await track.getAttribute('aria-valuemax'));
  info('window [' + lo + ', ' + hi + '] mm (aria)');

  // Two taps at different fractions: proves repeatability and that the second
  // ECHO supersedes the first. Fractions chosen away from wherever tgt
  // currently sits.
  const strip = await page.$('.rail-tape.live');

  for (const frac of [0.75, 0.3]) {
    // Re-acquire the strip rect per tap: tap 1's motion expands the plan
    // strip, which can add a scrollbar and shift layout — a cached box then
    // aims the second tap at the wrong fraction (found live, first run).
    const box = await strip.boundingBox();
    const expected = lo + frac * (hi - lo);
    const lastTgt = wire.samples.length ? wire.samples[wire.samples.length - 1].tgt : NaN;
    info('tap @ ' + (frac * 100) + '% → expecting tgt → ' + expected.toFixed(1) + ' mm (tgt now ' + lastTgt.toFixed(1) + ')');
    const tapT = Date.now();

    // Real pointer tap on the strip; shadow states observed by polling.
    const seen = new Set();
    const poll = (async () => {
      for (let i = 0; i < 200; i++) {
        const s = await page.$eval('.rail-tape-assembly', (el) => el.dataset.shadow).catch(() => null);
        if (s) seen.add(s);
        await sleep(20);
      }
    })();
    await page.mouse.click(box.x + box.width * frac, box.y + box.height / 2);

    // Wait for the UI's own cursor (aria-valuenow = device-reported target
    // once the tap ends — never a local guess) to converge.
    let uiNow = NaN;
    const t0 = Date.now();
    while (Date.now() - t0 < SETTLE_MS) {
      uiNow = parseFloat(await track.getAttribute('aria-valuenow'));
      if (Math.abs(uiNow - expected) <= TOL_MM) break;
      await sleep(100);
    }
    await poll;

    ok('ECHO confirmed (shadow reached confirmed, no fault)',
       seen.has('confirmed') && !seen.has('fault'),
       'shadow states seen: ' + [...seen].join(' → '));
    ok('UI tape cursor follows device target to tapped value',
       Math.abs(uiNow - expected) <= TOL_MM,
       'aria-valuenow ' + uiNow + ' vs expected ' + expected.toFixed(1));

    // Independent wire confirmation: device tgt_10um moved to the tapped
    // value after the tap timestamp.
    const after = wire.samples.filter((s) => s.t >= tapT);
    const finalTgt = after.length ? after[after.length - 1].tgt : NaN;
    ok('WIRE: device tgt_10um followed (independent session)',
       after.length > 0 && Math.abs(finalTgt - expected) <= TOL_MM,
       'final tgt ' + (isNaN(finalTgt) ? 'n/a' : finalTgt.toFixed(2)) + ' mm over ' + after.length + ' samples');

    // Numerals: commanded ≈ expected; lag = commanded - actual (rounding tol).
    const [actual, commanded, lag] = await numerals();
    ok('commanded numeral shows the applied target',
       Math.abs(commanded - expected) <= TOL_MM, commanded + ' mm');
    ok('lag numeral = commanded − actual',
       Math.abs(lag - (commanded - actual)) <= 0.2,
       lag + ' vs ' + (commanded - actual).toFixed(2) + ' (actual ' + actual + ')');
  }

  await page.screenshot({ path: join(OUT, 'tap-to-move-live.png'), fullPage: false });
  info('screenshot: evidence/tap-to-move-live.png');
}

ok('no uncaught page errors', pageErrors.length === 0, pageErrors.slice(0, 3).join(' / '));

await browser.close();
watcher.close();
console.log('\n' + (fails ? 'FAILURES: ' + fails : 'ALL PASS'));
process.exit(fails ? 1 : 0);
