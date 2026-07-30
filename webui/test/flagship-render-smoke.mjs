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
await page.click('button:has-text("Terse instruments")');

// ---- settings descriptions: terse MOVES them, never drops them -------------
// Field.svelte: verbose prints a field's description inline under the control,
// terse holds it on the info button's hover tip. Exactly one carrier is ever
// live — both at once is the duplicate truth the density pass exists to stop.
const fieldTabs = await page.$$('nav.rail [role="tab"]');
for (const t of fieldTabs) {
  const txt = (await t.textContent()).trim().toLowerCase();
  if (txt.includes('tuning') || txt.includes('motion')) { await t.click(); break; }
}
// The affordance only renders in TERSE, so it has to be measured there. A
// display:none element reports an all-zero rect, which would make the
// centering check below pass without measuring anything.
await page.evaluate(() => document.documentElement.classList.add('terse'));
const infoUp = await page.waitForSelector('.info-wrap .info', { state: 'visible', timeout: 15000 })
  .then(() => true).catch(() => false);
ok('described settings fields render an info affordance', infoUp);

// Centering regression guard: a `button` carries UA padding, and under
// border-box that shrinks this 18px control's content box below the glyph's
// own width — which pins the glyph to the content edge instead of centering
// it. Measured 3.5px off before `padding: 0` landed.
const glyph = await page.$eval('.info-wrap .info', (el) => {
  const b = el.getBoundingClientRect(), g = el.querySelector('.glyph').getBoundingClientRect();
  return { w: b.width, h: b.height,
           off: Math.max(Math.abs((g.x + g.width / 2) - (b.x + b.width / 2)),
                         Math.abs((g.y + g.height / 2) - (b.y + b.height / 2))) };
});
ok('info button is actually laid out (guards a vacuous zero)', glyph.w > 0 && glyph.h > 0,
   glyph.w + 'x' + glyph.h);
ok('info glyph is centered in its button', glyph.off < 0.51, glyph.off.toFixed(3) + 'px off');

// Verbose hides the button outright — the description is already on screen,
// so there is nothing for it to reveal. Only terse can be hovered, and the
// hover must come AFTER the mode switch: terse hides every inline description
// at once, and that reflow slides the button out from under a pointer parked
// at fixed viewport coordinates.
const setTerse = async (on) => {
  await page.evaluate((v) => document.documentElement.classList.toggle('terse', v), on);
  if (on) await page.hover('.info-wrap .info');
  await page.waitForTimeout(220);            // clear the .12s tip transition
  return page.evaluate(() => {
    const d = document.querySelector('.field .field-desc');
    const t = document.querySelector('.info-wrap .tip');
    const w = document.querySelector('.info-wrap');
    return { inline: d ? getComputedStyle(d).display !== 'none' : null,
             tip: t ? getComputedStyle(t).visibility : null,
             affordance: w ? getComputedStyle(w).display !== 'none' : null };
  });
};
const verbose = await setTerse(false);
ok('verbose prints the description inline', verbose.inline === true, JSON.stringify(verbose));
ok('verbose hides the info button entirely', verbose.affordance === false, JSON.stringify(verbose));
ok('verbose does not also pop the hover tip', verbose.tip === 'hidden', JSON.stringify(verbose));
const terseMode = await setTerse(true);
ok('terse drops the inline description', terseMode.inline === false, JSON.stringify(terseMode));
ok('terse shows the info button', terseMode.affordance === true, JSON.stringify(terseMode));
ok('terse reveals the tip on hover', terseMode.tip === 'visible', JSON.stringify(terseMode));
await setTerse(false);

// ---- slider values are typeable --------------------------------------------
// A slider carries no numerals of its own, so its chip is the only place an
// exact value can be entered. A readout must never gain one: no setting_key
// means it is effective truth, not a control.
const typeable = await page.evaluate(() => {
  const q = (sel) => Array.from(document.querySelectorAll(sel));
  return {
    sliders: q('.field[data-widget="slider"]').length,
    typeableSliders: q('.field[data-widget="slider"] .field-value .chip-num').length,
    typeableReadouts: q('.field[data-widget="readout"] .chip-num').length,
    spinners: getComputedStyle(document.querySelector('.chip-num') || document.body).appearance,
  };
});
ok('every slider chip is typeable', typeable.sliders > 0 && typeable.sliders === typeable.typeableSliders,
   typeable.typeableSliders + '/' + typeable.sliders);
ok('no readout became typeable', typeable.typeableReadouts === 0);
ok('typeable chip keeps native spinners stripped', typeable.spinners === 'textfield', typeable.spinners);

// ---- intent echo is wired --------------------------------------------------
// Drives the lifecycle by setting data-shadow directly. Fires NO intent and
// writes nothing to the machine — the point is the CSS wiring, which dies
// silently in three ways: Svelte renames scoped @keyframes, `@property --pr`
// can go unregistered (then the radius SNAPS instead of spreading), and the
// mask can be pruned as unused.
const echo = await page.evaluate(async () => {
  const f = document.querySelector('.field[data-widget="slider"]');
  if (!f) return null;
  const read = () => {
    const cs = getComputedStyle(f, '::after');
    return { anim: cs.animationName, dur: cs.animationDuration,
             iter: cs.animationIterationCount, mask: cs.maskImage || cs.webkitMaskImage || 'none' };
  };
  const orig = f.getAttribute('data-shadow');
  const res = { originX: f.style.getPropertyValue('--pulse-x'), originY: f.style.getPropertyValue('--pulse-y') };
  f.setAttribute('data-shadow', 'pending'); res.pending = read();
  f.setAttribute('data-shadow', 'fault');   res.fault = read();
  f.setAttribute('data-shadow', orig ?? 'confirmed');
  f.classList.add('settled');
  await new Promise((r) => setTimeout(r, 250));
  res.settled = read();
  res.pr = getComputedStyle(f, '::after').getPropertyValue('--pr');
  f.classList.remove('settled');
  return res;
});
ok('a slider field exists to carry the echo', echo !== null);
if (echo) {
  ok('pending pulses on a repeating wavefront',
     echo.pending.anim.includes('intent-echo') && echo.pending.iter === 'infinite'
     && echo.pending.dur === '0.5s', JSON.stringify(echo.pending));
  ok('pending echo carries a radial mask', echo.pending.mask.includes('radial-gradient'));
  ok('confirm echo runs once over the settle window',
     echo.settled.anim.includes('confirm-echo') && echo.settled.iter === '1'
     && echo.settled.dur === '0.9s', JSON.stringify(echo.settled));
  // The registration guard: an unregistered --pr never interpolates, so a
  // mid-flight sample would read 0% (or the raw token) instead of a radius.
  const pr = parseFloat(echo.pr);
  ok('--pr interpolates, so the pulse spreads instead of snapping',
     isFinite(pr) && pr > 5 && pr < 165, 'mid-flight --pr = ' + echo.pr);
  // Doctrine: a refused write has finished failing; a pulsing failure reads
  // as "still trying".
  ok('fault does not pulse', echo.fault.anim === 'none', JSON.stringify(echo.fault));
  ok('echo origin tracks the handle, not a hardcoded guess',
     echo.originX.length > 0 && echo.originY.length > 0, echo.originX + ' / ' + echo.originY);
}

// The echo box is cropped to the control's band, but a range input's own box is
// only the 2px hairline track — the thumb is a pseudo-element that overflows
// it. If the crop follows the input's rect alone, the HANDLE ends up outside
// the outline that is supposed to enclose it.
const encl = await page.evaluate(() => {
  const f = document.querySelector('.field[data-widget="slider"]');
  if (!f) return null;
  const ctrl = f.querySelector('input[type=range]');
  const fr = f.getBoundingClientRect(), cr = ctrl.getBoundingClientRect();
  const was = f.getAttribute('data-shadow');
  f.setAttribute('data-shadow', 'pending');
  const cs = getComputedStyle(f, '::after');
  const top = parseFloat(cs.top), bottom = parseFloat(cs.bottom);
  f.setAttribute('data-shadow', was ?? 'confirmed');
  const probe = document.createElement('div');
  probe.style.height = 'var(--slider-thumb-h)';
  f.appendChild(probe);
  const thumbH = probe.getBoundingClientRect().height;
  probe.remove();
  const cy = cr.top + cr.height / 2 - fr.top;
  return { boxTop: top, boxBottom: fr.height - bottom,
           thumbTop: cy - thumbH / 2, thumbBottom: cy + thumbH / 2,
           fieldH: fr.height, thumbH };
});
if (encl) {
  ok('echo encloses the slider handle', encl.boxTop <= encl.thumbTop && encl.boxBottom >= encl.thumbBottom,
     `box ${encl.boxTop.toFixed(1)}..${encl.boxBottom.toFixed(1)} vs thumb ${encl.thumbTop.toFixed(1)}..${encl.thumbBottom.toFixed(1)}`);
  ok('echo is cropped to the control, not the whole field',
     (encl.boxBottom - encl.boxTop) < encl.fieldH * 0.75,
     `${(encl.boxBottom - encl.boxTop).toFixed(1)}px of a ${encl.fieldH.toFixed(1)}px field`);
  ok('thumb height comes from the shared var, not a literal', encl.thumbH > 0, encl.thumbH.toFixed(2) + 'px');
}

// ---- activity heatmap keeps its scroll history -----------------------------
// A reactive read made SYNCHRONOUSLY inside LinkBar's $effect becomes that
// effect's dependency, so a telemetry frame re-runs the body and refills the
// history buffer with zeros ~25x a second: the grid still scrolls, with
// nothing in it. Signature of that bug is every column but the newest sitting
// at the v=0 baseline alpha, so that is what this counts.
await page.waitForTimeout(3600);            // ~16 ticks at 220ms
const staleCols = await page.$eval('canvas.act-grid', (c) => {
  const ctx = c.getContext('2d');
  const d = ctx.getImageData(0, 0, c.width, c.height).data;
  const dpr = c.width / parseFloat(c.style.width);
  const cols = 14, cell = 4, gap = 1;
  const rows = Math.round(parseFloat(c.style.height) / (cell + gap + 1));
  const y = Math.round(((rows - 1) * (cell + gap + 1) + 1) * dpr);   // link-activity row
  let atBaseline = 0;
  for (let col = 0; col < cols; col++) {
    const x = Math.round((col * (cell + gap) + 1) * dpr);
    if (d[(y * c.width + x) * 4 + 3] <= 16) atBaseline++;            // alpha of v=0
  }
  return atBaseline;
});
ok('activity grid retains scroll history', staleCols <= 2,
   staleCols + '/14 columns at the empty baseline');

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
