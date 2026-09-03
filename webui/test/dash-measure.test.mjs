/**
 * dash-measure.test.mjs -- is a dashboard card's content column readable?
 *
 * The operator's report: "cards too wide to track left-to-right at 1440px."
 * DashGrid arranges 12 columns and dashboard.svelte.js's DEFAULT_SPAN is 12,
 * so a settings card is FULL WIDTH by default and its `.card-body` column is
 * whatever the pane happens to be.
 *
 * No device is needed and none is used: this reads the `.card-body` rule out
 * of the shipped stylesheet, applies it to a probe element at the pane widths
 * a 12-span card actually gets, and measures the resolved track against the
 * page's own font. A change to the rule changes the measurement.
 *
 * Deliberately NOT part of `npm run check`, same reason as
 * shell-chrome-geometry.test.mjs: that script runs inside every firmware
 * build and must not launch a browser.
 *
 * Run: node webui/test/dash-measure.test.mjs   (no device needed)
 */
import { chromium } from 'playwright';
import { createServer } from 'node:http';
import { readFileSync } from 'node:fs';

const HTML = readFileSync(new URL('../dist/index.html', import.meta.url));
const srv = createServer((_q, s) => { s.writeHead(200, { 'Content-Type': 'text/html' }); s.end(HTML); });
await new Promise((r) => srv.listen(0, '127.0.0.1', r));
const PORT = srv.address().port;

let fails = 0;
const ok = (n, c, extra) => { console.log('  [' + (c ? 'PASS' : 'FAIL') + '] ' + n + (extra ? '  -- ' + extra : '')); if (!c) fails++; };

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
await page.goto('http://127.0.0.1:' + PORT + '/', { waitUntil: 'domcontentloaded' });
await page.waitForSelector('.linkbar', { timeout: 15000 });
await page.waitForTimeout(500);

// A 12-span card's own content width, from the layout constants each term
// comes from: .app padding (--gap each side), the nav rail plus its gap,
// .dash-grid padding, the card border, and .dash-body padding.
const RAIL_PX = 188;
const paneCases = [
  { label: '1440', appW: 1440 },
  { label: '1920', appW: 1680 },   // .app max-width caps here
];

const res = await page.evaluate(({ cases, railPx }) => {
  let tpl = null, sel = null;
  for (const sheet of document.styleSheets) {
    let rules;
    try { rules = sheet.cssRules; } catch (e) { continue; }
    for (const r of rules) {
      if (r.selectorText && /\.card-body\b/.test(r.selectorText)
          && r.style && r.style.gridTemplateColumns) {
        tpl = r.style.gridTemplateColumns;
        sel = r.selectorText;
      }
    }
  }
  const gap = parseFloat(getComputedStyle(document.documentElement).getPropertyValue('--gap')) || 12;

  const ruler = document.createElement('span');
  ruler.style.cssText = 'position:absolute;visibility:hidden;white-space:pre;font-family:var(--font);font-size:1rem';
  ruler.textContent = 'The quick brown fox jumps over the lazy dog and settles at window minimum.';
  document.body.appendChild(ruler);
  const avgCharPx = ruler.getBoundingClientRect().width / ruler.textContent.length;
  ruler.remove();

  const out = [];
  for (const c of cases) {
    const inner = c.appW - 2 * gap - railPx - gap - 10 - 2 - 2 * gap;
    const probe = document.createElement('div');
    probe.style.cssText = 'position:absolute;visibility:hidden;display:grid;gap:' + gap
      + 'px;width:' + inner + 'px;grid-template-columns:' + tpl;
    const cell = document.createElement('div');
    probe.appendChild(cell);
    document.body.appendChild(probe);
    const trackPx = cell.getBoundingClientRect().width;
    const tracks = getComputedStyle(probe).gridTemplateColumns.split(' ').length;
    probe.remove();
    out.push({ label: c.label, inner, trackPx, tracks, chars: trackPx / avgCharPx });
  }
  return { tpl, sel, avgCharPx, out };
}, { cases: paneCases, railPx: RAIL_PX });

ok('the .card-body rule constrains its columns', !!res.tpl,
   res.sel + ' { grid-template-columns: ' + res.tpl + ' }');

for (const r of res.out) {
  console.log('  at ' + r.label + ': card content ' + r.inner.toFixed(0) + 'px -> '
    + r.tracks + ' column(s) of ' + r.trackPx.toFixed(0) + 'px = ' + r.chars.toFixed(0) + ' characters');
  // 65 characters is the target measure; the band is what "near 65" means
  // before a column reads as either a stretched row or a cramped gutter.
  ok('at ' + r.label + ': a card column stays near 65 characters of measure',
     r.chars >= 45 && r.chars <= 75, r.chars.toFixed(0) + ' chars');
  ok('at ' + r.label + ': the card still fills its pane',
     r.tracks * r.trackPx > r.inner * 0.55,
     r.tracks + ' x ' + r.trackPx.toFixed(0) + 'px in ' + r.inner.toFixed(0) + 'px');
}

// A narrow card must collapse to ONE column, never overflow its own box.
const narrow = await page.evaluate((tpl) => {
  const probe = document.createElement('div');
  probe.style.cssText = 'position:absolute;visibility:hidden;display:grid;width:300px;grid-template-columns:' + tpl;
  const cell = document.createElement('div');
  probe.appendChild(cell);
  document.body.appendChild(probe);
  const w = cell.getBoundingClientRect().width;
  const n = getComputedStyle(probe).gridTemplateColumns.split(' ').length;
  probe.remove();
  return { w, n };
}, res.tpl);
ok('a 300px card is one column and does not overflow', narrow.n === 1 && narrow.w <= 300.5,
   narrow.n + ' x ' + narrow.w.toFixed(0) + 'px');

await browser.close();
srv.close();
console.log('\n' + (fails ? 'FAILURES: ' + fails : 'ALL PASS -- card columns hold the measure.'));
process.exit(fails ? 1 : 0);
