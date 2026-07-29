/**
 * flagship-render-smoke.mjs — NO-MOTION render verification of the flagship
 * UI frame against the real device (or any live hub serving the bundle).
 *
 * Asserts, at a desktop viewport:
 *   - the nav rail renders with an Overview tab plus machine-derived entries,
 *     switching tabs swaps the pane, and the mini-rail collapse works;
 *   - TransportBar (top-of-page, operator ruling 2026-07-28) renders the
 *     hazard-striped e-stop, visible and fireable, while the safety dock's
 *     own copy is hidden at this width (the dock keeps it below 960px);
 *   - NO control anywhere in the dock is the RFC-034 value-0 placeholder (no
 *     "reserved" button — the flagship ruling); pause/stop/home no longer
 *     appear in the dock at all, having moved to TransportBar;
 *   - dashboard handles are hidden until "Edit layout" and hide again on Done;
 * and at a phone viewport, that the tab strip renders instead of the rail.
 *
 * FIRES NO INTENTS AND COMMANDS NO MOTION — tab clicks and layout-edit
 * toggles only. Safe to run unattended against a live machine (motion
 * verification is a bench activity, DOCTRINE build/test rules).
 *
 * The page must be loaded FROM THE DEVICE (same reason as browser-check.mjs:
 * /uitoken is same-origin-only; a localhost origin gets watch tier).
 *
 * Run: node webui/test/flagship-render-smoke.mjs [host]
 */

import { chromium } from 'playwright';
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
const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
const pageErrors = [];
page.on('pageerror', (e) => pageErrors.push(String(e)));

console.log('flagship render smoke @ ' + PAGE_URL);
await page.goto(PAGE_URL, { waitUntil: 'domcontentloaded', timeout: 20000 });

// ---- desktop: nav rail ------------------------------------------------------
const railUp = await page.waitForSelector('nav.rail [role="tab"]', { timeout: 25000 })
  .then(() => true).catch(() => false);
ok('nav rail renders (catalog adopted)', railUp);

const railTabs = await page.$$eval('nav.rail [role="tab"]', (els) => els.map((e) => e.textContent.trim()));
ok('rail has Overview + machine categories + console entries', railTabs.length >= 6,
   railTabs.join(' | '));
ok('no legacy top tab strip at desktop width', (await page.$('nav.tabs')) == null);

// Switching to the second rail entry must swap the pane to a settings grid.
const second = (await page.$$('nav.rail [role="tab"]'))[1];
if (second) {
  await second.click();
  const grid = await page.waitForSelector('.dash-grid', { timeout: 5000 }).then(() => true).catch(() => false);
  ok('category tab renders a dashboard grid', grid);
}

// Collapse to the mini rail and back — names hide, glyphs stay.
await page.click('.rail-collapse');
ok('mini rail hides names', (await page.$$('nav.rail .rail-name')).length === 0);
ok('mini rail keeps glyphs', (await page.$$('nav.rail .rail-glyph')).length >= 6);
await page.click('.rail-collapse');
ok('rail expands again', (await page.$$('nav.rail .rail-name')).length >= 6);

// ---- safety dock ------------------------------------------------------------
const dock = await page.waitForSelector('.safetydock .dock', { timeout: 10000 })
  .then(() => true).catch(() => false);
ok('safety dock renders', dock);

// TransportBar owns the visible e-stop at this (desktop) width now (operator
// ruling 2026-07-28) — the dock still renders its own copy in the DOM (never
// removed from either surface's logic) but keeps it CSS-hidden here.
const tbEstopUp = await page.waitForSelector('.transportbar .btn-estop', { timeout: 10000 })
  .then(() => true).catch(() => false);
ok('hazard-striped e-stop present in TransportBar', tbEstopUp);
const tbEstopVisible = await page.$eval('.transportbar .btn-estop',
  (b) => getComputedStyle(b).display !== 'none').catch(() => false);
ok('TransportBar e-stop visible at desktop', tbEstopVisible);
const estopEnabled = await page.$eval('.transportbar .btn-estop', (b) => !b.disabled).catch(() => false);
ok('e-stop is fireable for this session', estopEnabled);
const dockEstopHidden = await page.$eval('.safetydock .btn-estop',
  (b) => getComputedStyle(b).display === 'none').catch(() => false);
ok('safety dock e-stop CSS-hidden at desktop (TransportBar shows it here instead)', dockEstopHidden);

// Pause/stop/home moved to TransportBar (operator ruling 2026-07-28) and no
// longer render as dock buttons at all.
const dockLabels = await page.$$eval('.safetydock button', (els) => els.map((e) => e.textContent.trim().toLowerCase()));
ok('no value-0 "reserved" placeholder rendered', !dockLabels.some((t) => /reserved|unused|none/.test(t)),
   dockLabels.join(' | '));
ok('op groups carry role labels', (await page.$$('.safetydock .grp-lbl')).length >= 1);

// ---- fixed-viewport architecture (UX maturity pass) ------------------------
// Desktop must never scroll as a page: the pane region is the only scroll
// area, so the safety dock and footer are on screen by construction.
const pageScrolls = await page.evaluate(() =>
  document.scrollingElement.scrollHeight > window.innerHeight + 2);
ok('desktop page does not scroll (fixed-viewport column)', !pageScrolls);
const dockBox = await page.$eval('.safetydock', (el) => {
  const r = el.getBoundingClientRect();
  return r.top >= 0 && r.bottom <= window.innerHeight + 1 && r.height > 0;
});
ok('safety dock fully on screen without scrolling', dockBox);

// ---- edit-layout mode -------------------------------------------------------
await page.click('nav.rail [role="tab"]');            // back to Overview
await page.waitForSelector('.dash-grid', { timeout: 5000 });
// Card-zone heroes (pattern, limits) now live in the Overview grid beside
// the telemetry card — the instrument zone holds only the rail.
ok('Overview holds telemetry + card-zone hero cards', (await page.$$('.dash-item')).length >= 3,
   (await page.$$('.dash-item')).length + ' cards');
ok('handles hidden while reading', (await page.$$('.dash-item .handle')).length === 0);
const editBtn = await page.$$('.dash-toolbar button');
await editBtn[0].click();                              // Edit layout
ok('handles appear in edit mode', (await page.$$('.dash-item .handle')).length > 0);
const editButtons = await page.$$eval('.dash-toolbar button', (els) => els.map((e) => e.textContent.trim()));
ok('edit mode offers Reset + Done', editButtons.join(',').includes('Reset') && editButtons.join(',').includes('Done'));
await page.click('.dash-toolbar button:last-child');   // Done
ok('handles hide again on Done', (await page.$$('.dash-item .handle')).length === 0);

// ---- terse mode -------------------------------------------------------------
// Hero teaching copy hides; settings descriptions (.field-desc) never do.
const railTabsEls = await page.$$('nav.rail [role="tab"]');
await railTabsEls[railTabsEls.length - 1].click();     // Display (last console entry)
await page.waitForSelector('button:has-text("Terse instruments")', { timeout: 5000 });
const explainCount = await page.$$eval('.explain', (els) => els.length);
ok('hero instruments carry explain copy', explainCount > 0, explainCount + ' elements');
await page.click('button:has-text("Terse instruments")');
const allHidden = await page.$$eval('.explain', (els) => els.every((e) => getComputedStyle(e).display === 'none'));
ok('terse hides instrument explanations', allHidden);
const fieldDescVisible = await page.$$eval('.field-desc', (els) =>
  els.length === 0 || els.some((e) => getComputedStyle(e).display !== 'none'));
ok('settings descriptions survive terse', fieldDescVisible);
await page.click('button:has-text("Terse instruments")');

await page.click('nav.rail [role="tab"]');             // back to Overview for the shot
await page.evaluate(() => window.scrollTo(0, 0));
await page.screenshot({ path: join(OUT, 'flagship-desktop.png'), fullPage: false });

// ---- phone viewport: tab strip, no rail ------------------------------------
await page.setViewportSize({ width: 390, height: 844 });
const strip = await page.waitForSelector('nav.tabs [role="tab"]', { timeout: 5000 })
  .then(() => true).catch(() => false);
ok('phone width renders the tab strip', strip);
ok('phone width drops the rail', (await page.$('nav.rail')) == null);
await page.screenshot({ path: join(OUT, 'flagship-phone.png'), fullPage: false });

ok('no page errors', pageErrors.length === 0, pageErrors.join(' ; '));

await browser.close();
console.log(fails === 0 ? 'ALL PASS' : fails + ' FAILURES');
process.exit(fails === 0 ? 0 : 1);
