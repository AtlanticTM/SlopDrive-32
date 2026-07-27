import { chromium } from 'playwright';
const HOST = process.argv[2] || '192.168.1.229';
const OUTNAME = process.argv[3] || 'rail-tick-before.png';
const browser = await chromium.launch();
const ctx = await browser.newContext({ viewport: { width: 1280, height: 900 } });
const page = await ctx.newPage();
await page.goto('http://' + HOST + '/', { waitUntil: 'domcontentloaded', timeout: 20000 });
await page.waitForSelector('nav.tabs button', { timeout: 25000 }).catch(() => {});
await page.waitForTimeout(1500);
const rail = await page.$('.spine-rail-host');
if (rail) {
  await rail.screenshot({ path: 'test/evidence/' + OUTNAME });
  console.log('rail shot saved: ' + OUTNAME);
} else {
  console.log('no rail host found');
}
await browser.close();
