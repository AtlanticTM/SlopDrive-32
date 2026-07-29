/**
 * og-reference-shots.mjs — captures the OG (main-branch) page as VISUAL ground
 * truth for the fidelity pass. The OG bundle is static-served (no device: the
 * link is down, but the chrome renders), screenshotted whole and per zone.
 *
 * Run: node webui/test/og-reference-shots.mjs [baseUrl] [outDir]
 */
import { chromium } from 'playwright';
import { mkdirSync } from 'node:fs';
import { join } from 'node:path';

const BASE = process.argv[2] || 'http://127.0.0.1:8123/';
const OUT = process.argv[3] || join(process.cwd(), 'webui', 'test', 'evidence', 'og-ref');
mkdirSync(OUT, { recursive: true });

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
page.on('pageerror', (e) => console.log('pageerror:', String(e).slice(0, 120)));
await page.goto(BASE, { waitUntil: 'load', timeout: 20000 });
await page.waitForTimeout(2500); // let boot JS settle into its no-link state

await page.screenshot({ path: join(OUT, 'og-full.png'), fullPage: false });

const zones = [
  ['og-hero-transport', '.hero-strip'],
  ['og-rail', '.rail-panel'],
  ['og-tabs', '.tabs-desktop'],
  ['og-first-cards', '.main'],
  ['og-footer', 'footer'],
];
for (const [name, sel] of zones) {
  const el = await page.$(sel);
  if (!el) { console.log('MISSING zone', name, sel); continue; }
  await el.screenshot({ path: join(OUT, name + '.png') }).catch((e) => console.log('shot fail', name, String(e).slice(0, 80)));
}
console.log('shots written to', OUT);
await browser.close();
