/**
 * gap-probe.mjs — measures the dead vertical space between the rail canvas
 * (.spine-rail-host) and the rail-hint text, and dumps the PlanStrip's own
 * box model while idle, so the "~100px gap" claim can be checked against
 * real numbers instead of eyeballing a screenshot.
 */
import { chromium } from 'playwright';

const HOST = process.argv[2] || '192.168.1.229';
const PAGE_URL = 'http://' + HOST + '/';

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
await page.goto(PAGE_URL, { waitUntil: 'domcontentloaded', timeout: 20000 });
await page.waitForSelector('nav.tabs button', { timeout: 25000 });
await page.waitForTimeout(1500);

const info = await page.evaluate(() => {
  function rectOf(sel) {
    const el = document.querySelector(sel);
    if (!el) return null;
    const r = el.getBoundingClientRect();
    const cs = getComputedStyle(el);
    return { sel, y: r.y, h: r.height, bottom: r.bottom, top: r.top,
             display: cs.display, opacity: cs.opacity, marginTop: cs.marginTop, marginBottom: cs.marginBottom };
  }
  const heroRect = rectOf('.hero.rail-hero');
  const heroCs = document.querySelector('.hero.rail-hero') ? getComputedStyle(document.querySelector('.hero.rail-hero')) : null;
  const railHost = rectOf('.spine-rail-host');
  const planStrip = rectOf('.plan-strip');
  const planLane = rectOf('.plan-lane');
  const planMeta = rectOf('.plan-meta');
  const railHint = rectOf('.rail-hint');
  return {
    heroGap: heroCs ? heroCs.gap : null,
    railHost, planStrip, planLane, planMeta, railHint,
    gapHostToHint: railHost && railHint ? (railHint.top - railHost.bottom) : null,
    gapHostToPlanStrip: railHost && planStrip ? (planStrip.top - railHost.bottom) : null,
    gapPlanStripToHint: planStrip && railHint ? (railHint.top - planStrip.bottom) : null,
  };
});

console.log(JSON.stringify(info, null, 2));
await browser.close();
