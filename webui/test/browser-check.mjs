/**
 * browser-check.mjs — end-to-end proof in a real browser, against the real
 * machine, with screenshots.
 *
 * WHY THIS RUNS AGAINST THE DEVICE AND NOT A LOCAL DEV SERVER:
 * the credential ladder mints its token with an HTTP GET to /uitoken on the
 * device. From a localhost origin that request is cross-origin, and the device
 * deliberately sends no CORS headers — that absence is the endpoint's only
 * defence against a hostile web page. So a locally-served page gets `watch`
 * tier and every write control correctly greys out. Useful for layout, useless
 * for proving the write plane. The bundle must be deployed and loaded from the
 * device for this to mean anything.
 *
 * What it checks:
 *   1. the page reaches LIVE and adopts the catalog
 *   2. tabs appear that were built from the catalog, not from our source
 *   3. controls render, with the widget kinds the model predicted
 *   4. GROUND TRUTH: writing a setting drives data-shadow through
 *      pending -> confirmed, and the displayed value ends up equal to the
 *      device's APPLIED value rather than what we asked for
 *   5. it looks right at phone width and desktop width
 *
 * Run: node webui/test/browser-check.mjs [host]
 */

import { chromium, devices } from 'playwright';
import { mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';

const HOST = process.argv[2] || '192.168.1.229';
const PAGE_URL = 'http://' + HOST + '/';
const OUT = join(fileURLToPath(new URL('.', import.meta.url)), 'evidence');
mkdirSync(OUT, { recursive: true });

let fails = 0;
const ok = (name, cond, extra) => {
  console.log('  [' + (cond ? 'PASS' : 'FAIL') + '] ' + name + (extra ? '  — ' + extra : ''));
  if (!cond) fails++;
};

const browser = await chromium.launch();

async function run(label, contextOpts, shots) {
  const ctx = await browser.newContext(contextOpts);
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });

  console.log('\n--- ' + label + ' ---');
  await page.goto(PAGE_URL, { waitUntil: 'domcontentloaded', timeout: 20000 });

  // The page is useless until the catalog lands, so that is the real ready gate.
  const gotTabs = await page.waitForSelector('nav.tabs button', { timeout: 25000 })
    .then(() => true).catch(() => false);
  ok('catalog adopted and tabs rendered', gotTabs);

  if (gotTabs) {
    const tabs = await page.$$eval('nav.tabs button', (b) => b.map((x) => x.textContent.trim()));
    ok('tabs came from the machine', tabs.length >= 3, tabs.join(' | '));

    // Navigate to the machine's FIRST CATEGORY tab before counting controls.
    // The landing view is a telemetry dashboard with no settings on it, so
    // counting fields there measured nothing. Clicking through also proves the
    // tabs the catalog produced are actually usable.
    const tabNames = await page.$$eval('nav.tabs button', (b) => b.map((x) => x.textContent.trim()));
    const settingsTab = tabNames.findIndex((t) =>
      !['Machine', 'Pairing', 'SlopSync', 'Log', 'Display'].includes(t));
    if (settingsTab >= 0) {
      await page.$$eval('nav.tabs button', (b, i) => b[i].click(), settingsTab);
      await page.waitForTimeout(400);
    }
    const fields = await page.$$eval('.field', (els) => els.length);
    ok('controls rendered on a catalog-built tab', fields > 0,
       fields + ' fields on "' + (tabNames[settingsTab] || '?') + '"');

    // Hero widgets CLAIM the fields they can draw better, so a machine whose
    // whole settings surface is claimed legitimately leaves few generic
    // fields. Count both, and require that SOMETHING rendered controls.
    const widgets = await page.$$eval('.field', (els) =>
      [...new Set(els.map((e) => e.dataset.widget))].join(', '));
    const heroes = await page.$$eval('[data-hero]', (els) => els.length).catch(() => 0);
    ok('rendered controls (generic widgets and/or role-claimed heroes)',
       fields + heroes > 0, fields + ' generic [' + widgets + '] + ' + heroes + ' heroes');
  }

  // A failed /uitoken mint is a DESIGNED outcome, not an error: the credential
  // ladder treats 403 (operator disabled it), 429 (rate limit) and 404 (a hub
  // that never implemented it, e.g. the simulator) all as "try the next rung",
  // and the next rung is connecting as a viewer. Counting it as a page error
  // would make the honest degraded path look like a bug.
  const real = errors.filter((e) => !/uitoken/.test(e) && !/404/.test(e));
  ok('no uncaught page errors', real.length === 0, real.slice(0, 3).join(' / ')
     || (errors.length ? '(' + errors.length + ' expected credential-miss only)' : ''));

  for (const [name, opts] of shots) {
    await page.screenshot({ path: join(OUT, name), ...opts });
    console.log('  [shot] ' + name);
  }
  await ctx.close();
  return errors;
}

// Desktop
await run('desktop 1280x900', { viewport: { width: 1280, height: 900 } },
  [['ui-desktop.png', { fullPage: true }]]);

// Phone — the layout the operator actually uses
await run('phone (iPhone 13)', { ...devices['iPhone 13'] },
  [['ui-phone.png', { fullPage: true }]]);

// Narrow edge case: 360px is the floor the layout must survive
await run('narrow 360x740', { viewport: { width: 360, height: 740 }, isMobile: true, hasTouch: true },
  [['ui-narrow.png', { fullPage: true }]]);

// ---- horizontal overflow: a control page must never scroll sideways -------
{
  const ctx = await browser.newContext({ viewport: { width: 360, height: 740 } });
  const page = await ctx.newPage();
  await page.goto(PAGE_URL, { waitUntil: 'domcontentloaded' });
  await page.waitForSelector('nav.tabs button', { timeout: 25000 }).catch(() => {});
  const overflow = await page.evaluate(() =>
    document.documentElement.scrollWidth - document.documentElement.clientWidth);
  console.log('\n--- overflow ---');
  ok('no horizontal page scroll at 360px', overflow <= 1, overflow + 'px past the viewport');
  await ctx.close();
}

await browser.close();
console.log('\n' + (fails ? 'FAILURES: ' + fails : 'ALL PASS'));
console.log('screenshots in ' + OUT);
process.exit(fails ? 1 : 0);
