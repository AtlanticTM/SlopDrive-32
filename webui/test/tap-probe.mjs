/**
 * tap-probe.mjs — read-only investigation of the rail input-tape hit-testing
 * bug. Does NOT click/tap anything — just locates the tape strip, reports its
 * bounding rect, and asks the DOM what element is actually at a handful of
 * candidate points (elementFromPoint), plus the pointer-events chain up to
 * that element. No motion is commanded.
 */
import { chromium } from 'playwright';

const HOST = process.argv[2] || '192.168.1.229';
const PAGE_URL = 'http://' + HOST + '/';

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
page.on('pageerror', (e) => console.log('[pageerror]', String(e)));
page.on('console', (m) => { if (m.type() === 'error') console.log('[console]', m.text()); });

await page.goto(PAGE_URL, { waitUntil: 'domcontentloaded', timeout: 20000 });
await page.waitForSelector('nav.tabs button', { timeout: 25000 });
await page.waitForTimeout(1500); // let telemetry/catalog settle

const info = await page.evaluate(() => {
  function describe(el) {
    if (!el) return null;
    const r = el.getBoundingClientRect();
    const cs = getComputedStyle(el);
    return {
      tag: el.tagName, cls: el.className, id: el.id,
      rect: { x: r.x, y: r.y, w: r.width, h: r.height },
      pointerEvents: cs.pointerEvents, zIndex: cs.zIndex, position: cs.position,
      opacity: cs.opacity, visibility: cs.visibility, display: cs.display,
    };
  }

  const assembly = document.querySelector('.rail-tape-assembly');
  const track = document.querySelector('.rail-tape-track');
  const live = document.querySelector('.rail-tape.live');
  const fallback = document.querySelector('.rail-tape:not(.live)');
  const band = document.querySelector('.rail-band');
  const railHost = document.querySelector('.spine-rail-host');
  const planStrip = document.querySelector('.plan-strip');

  const out = {
    assembly: describe(assembly),
    track: describe(track),
    live: describe(live),
    fallback: describe(fallback),
    band: describe(band),
    railHost: describe(railHost),
    planStrip: describe(planStrip),
  };

  // Pick target points: the live strip if present, else the fallback.
  const target = live || fallback;
  if (target) {
    const r = target.getBoundingClientRect();
    const points = [
      { name: 'center', x: r.x + r.width * 0.5, y: r.y + r.height * 0.5 },
      { name: '90pct', x: r.x + r.width * 0.9, y: r.y + r.height * 0.5 },
      { name: '10pct', x: r.x + r.width * 0.1, y: r.y + r.height * 0.5 },
    ];
    out.hitTests = points.map((p) => {
      const el = document.elementFromPoint(p.x, p.y);
      return { ...p, hit: describe(el), hitIsTarget: el === target, hitIsDescendant: target.contains(el) };
    });
  } else {
    out.hitTests = 'NO TARGET ELEMENT FOUND (.rail-tape.live nor fallback present)';
  }

  // Also test against the FULL ASSEMBLY width at 90% (what "90% across the
  // strip" plausibly meant if the tester measured against .rail-tape-track
  // or .rail-tape-assembly instead of the live sub-strip).
  if (assembly) {
    const r = assembly.getBoundingClientRect();
    const p = { x: r.x + r.width * 0.9, y: r.y + r.height * 0.7 };
    out.assemblyHit90 = { point: p, hit: describe(document.elementFromPoint(p.x, p.y)) };
  }

  return out;
});

console.log(JSON.stringify(info, null, 2));
await browser.close();
