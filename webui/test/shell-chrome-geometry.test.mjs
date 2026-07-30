/**
 * chrome-geom.mjs — does shell chrome stack correctly above the LinkBar?
 *
 * The ShellBar itself is not in the device bundle, so this simulates it: a
 * fixed bar at top:0 plus the --shell-chrome-top reserve it publishes. That is
 * exactly the geometry contract the two changed rules implement, so if the
 * LinkBar lands anywhere but flush under the fake bar, the shell is broken.
 *
 * Deliberately NOT part of `npm run check`: that script runs inside every
 * firmware build (build_webui.py), and launching a browser there would put a
 * headless Chromium in the path of `pio run`. Run it via `npm run check:shell`
 * when chrome layout, the LinkBar, or the safe-area insets move.
 *
 * Run: node webui/test/shell-chrome-geometry.test.mjs   (no device needed)
 */
import { chromium } from 'playwright';
import { createServer } from 'node:http';
import { readFileSync } from 'node:fs';

const HTML = readFileSync(new URL('../dist/index.html', import.meta.url));
const srv = createServer((_q, s) => { s.writeHead(200, { 'Content-Type': 'text/html' }); s.end(HTML); });
await new Promise((r) => srv.listen(0, '127.0.0.1', r));
const PORT = srv.address().port;

let fails = 0;
const ok = (n, c, extra) => { console.log('  [' + (c ? 'PASS' : 'FAIL') + '] ' + n + (extra ? '  — ' + extra : '')); if (!c) fails++; };

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
await page.goto('http://127.0.0.1:' + PORT + '/', { waitUntil: 'domcontentloaded' });
await page.waitForSelector('.linkbar', { timeout: 15000 });

const BAR = 34;   // a plausible ShellBar height

// ---- baseline: no shell chrome ---------------------------------------------
let g = await page.evaluate(() => {
  const lb = document.querySelector('.linkbar').getBoundingClientRect();
  const app = document.querySelector('.app').getBoundingClientRect();
  return { lbTop: lb.top, appTop: app.top, appH: app.height, vh: window.innerHeight,
           scrolls: document.scrollingElement.scrollHeight > window.innerHeight + 2 };
});
ok('no shell: LinkBar is flush at the viewport top', Math.abs(g.lbTop) < 1, 'top=' + g.lbTop);
ok('no shell: desktop column is exactly one viewport', Math.abs(g.appH - g.vh) < 2, g.appH + ' vs ' + g.vh);
ok('no shell: page does not scroll', !g.scrolls);

// ---- with simulated shell chrome -------------------------------------------
await page.evaluate((h) => {
  const bar = document.createElement('div');
  bar.id = 'fake-shell';
  bar.style.cssText = 'position:fixed;top:0;left:0;right:0;height:' + h
    + 'px;z-index:21;background:#123';
  document.body.appendChild(bar);
  document.documentElement.style.setProperty('--shell-chrome-top', h + 'px');
  document.documentElement.style.setProperty('--chrome-inset-top', '0px');
}, BAR);
await page.waitForTimeout(300);

g = await page.evaluate(() => {
  const lb = document.querySelector('.linkbar').getBoundingClientRect();
  const fb = document.querySelector('#fake-shell').getBoundingClientRect();
  const app = document.querySelector('.app').getBoundingClientRect();
  return { lbTop: lb.top, lbBottom: lb.bottom, fbBottom: fb.bottom, appH: app.height,
           vh: window.innerHeight,
           scrolls: document.scrollingElement.scrollHeight > window.innerHeight + 2 };
});
ok('shell: LinkBar starts exactly where the shell chrome ends (no gap)',
   Math.abs(g.lbTop - g.fbBottom) < 1, 'lbTop=' + g.lbTop + ' barBottom=' + g.fbBottom);
ok('shell: LinkBar does not slide under the shell chrome (no overlap)',
   g.lbTop >= g.fbBottom - 0.5);
ok('shell: no dead space above the chrome', Math.abs(g.fbBottom - BAR) < 1);
ok('shell: desktop column still exactly one viewport',
   Math.abs(g.appH - g.vh) < 2, g.appH + ' vs ' + g.vh);
ok('shell: page still does not scroll', !g.scrolls);

// ---- mobile: the LinkBar must stick BELOW the chrome, not under it ---------
await page.setViewportSize({ width: 420, height: 800 });
await page.waitForTimeout(300);
await page.evaluate(() => window.scrollTo(0, 600));
await page.waitForTimeout(300);
g = await page.evaluate(() => {
  const lb = document.querySelector('.linkbar').getBoundingClientRect();
  const fb = document.querySelector('#fake-shell').getBoundingClientRect();
  return { lbTop: lb.top, fbBottom: fb.bottom };
});
ok('mobile scrolled: sticky LinkBar parks below the chrome, not beneath it',
   g.lbTop >= g.fbBottom - 0.5, 'lbTop=' + g.lbTop + ' barBottom=' + g.fbBottom);

await browser.close();
srv.close();
console.log('\n' + (fails ? 'FAILURES: ' + fails : 'ALL PASS — shell chrome stacks.'));
process.exit(fails ? 1 : 0);
