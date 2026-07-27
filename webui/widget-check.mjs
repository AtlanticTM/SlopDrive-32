import { chromium } from 'playwright';

const browser = await chromium.launch();
const ctx = await browser.newContext({ viewport: { width: 380, height: 900 } });
const page = await ctx.newPage();
const errors = [];
page.on('pageerror', (e) => errors.push('pageerror: ' + String(e)));
page.on('console', (m) => { if (m.type() === 'error') errors.push('console: ' + m.text()); });

await page.goto('http://127.0.0.1:8080/', { waitUntil: 'domcontentloaded', timeout: 20000 });

// wait for link to go live
await page.waitForFunction(() => {
  const p = document.body.innerText;
  return p.includes('link: live');
}, { timeout: 15000 }).catch(() => console.log('did not reach live within timeout'));

await new Promise((r) => setTimeout(r, 3000)); // let a few samples accumulate

const bodyText = await page.locator('p').first().innerText();
console.log('status line:', bodyText);

const tchart = await page.locator('.tchart').count();
const planstrip = await page.locator('.plan-strip').count();
console.log('TelemetryChart mounted (.tchart count):', tchart);
console.log('PlanStrip mounted (.plan-strip count, expect 0 - sim has no plan channel):', planstrip);

const legendText = tchart ? await page.locator('.tchart-legend').innerText() : '(none)';
console.log('legend:', JSON.stringify(legendText));

await page.screenshot({ path: 'C:/Users/Atlan/AppData/Local/Temp/claude/c--Users-Atlan-Documents-SlopDrive-32/04ee7035-7948-4c0e-9d2b-aa979a8b482f/scratchpad/widget-harness-380.png', fullPage: true });

// narrow 360px check for horizontal overflow
await page.setViewportSize({ width: 360, height: 800 });
await new Promise((r) => setTimeout(r, 500));
const overflow = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
console.log('horizontal overflow at 360px:', overflow, 'px');
await page.screenshot({ path: 'C:/Users/Atlan/AppData/Local/Temp/claude/c--Users-Atlan-Documents-SlopDrive-32/04ee7035-7948-4c0e-9d2b-aa979a8b482f/scratchpad/widget-harness-360.png', fullPage: true });

console.log('\nerrors:', errors.length ? errors.join('\n') : '(none)');

await browser.close();
process.exit(errors.length ? 1 : 0);
