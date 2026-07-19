/**
 * Theme engine — accent-pair palettes over the frozen neutral chassis.
 *
 * A theme swaps ONLY the two accent hues (reality + intent) and their derived
 * glow/gradient colors. The dark neutral ramp (ink/screen/line/tx tokens)
 * and the SAFETY colors (--warn amber, --bad red) are identical in every
 * theme on purpose: hazard hatching, overdue escalation, and e-stop styling
 * must read the same regardless of how the operator dresses the instrument.
 *
 * Single source of truth: the THEMES table below (+ the custom pair). CSS
 * never hardcodes a theme — applyTheme() injects <style> blocks generated
 * from this table (`:root[data-theme=X]{...}` overriding the accent custom
 * properties), and the canvas renderers (rail comet, wave scope, sparkline,
 * plan strip, activity grid, diag graph) read the same data through
 * ACCENT/ac() so painted pixels always match the CSS.
 *
 * CUSTOM theme: two operator-picked hexes (reality + intent) via native
 * color inputs in the Settings Theme card; the deep-intent and core-dot
 * derivatives are computed (darken / lighten toward white). Stored as JSON
 * in localStorage alongside the selected-theme key.
 *
 * Persistence: localStorage (client preference, per browser). This is UI
 * chrome, not machine state — the ground-truth doctrine (device echoes,
 * shadow lifecycle) does not apply; nothing is sent to the device.
 *
 * ac(kind, alpha): cached rgba() string builder for canvas draw calls —
 * 'r' = reality, 'i' = intent, 'd' = deep intent. Strings are memoized per
 * theme so per-frame reads are plain object lookups (no allocation after
 * first use); the cache resets on theme switch.
 */

const STORAGE_KEY = 'sd32.theme';
const CUSTOM_KEY = 'sd32.theme.customColors';

export const THEMES = [
  { id: 'phosphor', name: 'Phosphor', reality: '#4DA6FF', intent: '#A78BFA' },
  { id: 'tracer',   name: 'Tracer',   reality: '#52E88C', intent: '#E85CFF' },
  { id: 'synth',    name: 'Synth',    reality: '#FF5CA8', intent: '#5CE8FF' },
  { id: 'ember',    name: 'Ember',    reality: '#FF8A4D', intent: '#FFD24D' },
  { id: 'arctic',   name: 'Arctic',   reality: '#7DE8FF', intent: '#C4B5FD' },
  { id: 'vapor',    name: 'Vapor',    reality: '#B78BFF', intent: '#FF8BD1' },
  { id: 'ultra',    name: 'Ultra',    reality: '#8B7BFF', intent: '#4DFFC4' },
  { id: 'sakura',   name: 'Sakura',   reality: '#FFA8C5', intent: '#A8D8FF' },
  { id: 'stealth',  name: 'Stealth',  reality: '#D8DEE8', intent: '#8A93A6' },
];

// ---- Color math (tiny, exact-enough) --------------------------------------
function hexToArr(hex) {
  const h = hex.replace('#', '');
  return [parseInt(h.slice(0, 2), 16), parseInt(h.slice(2, 4), 16), parseInt(h.slice(4, 6), 16)];
}
function arrToRgbStr(a) { return a[0] + ',' + a[1] + ',' + a[2]; }
function arrToHex(a) {
  return '#' + a.map(v => Math.round(Math.max(0, Math.min(255, v))).toString(16).padStart(2, '0')).join('').toUpperCase();
}
function lighten(a, f) { return a.map(v => v + (255 - v) * f); }
function darken(a, f) { return a.map(v => v * (1 - f)); }

/** Full derived palette from a reality/intent hex pair. */
function derivePalette(realityHex, intentHex) {
  const r = hexToArr(realityHex), i = hexToArr(intentHex);
  return {
    reality: realityHex.toUpperCase(),
    realityRgb: arrToRgbStr(r),
    realityArr: r,
    intent: intentHex.toUpperCase(),
    intentRgb: arrToRgbStr(i),
    intentDeepRgb: arrToRgbStr(darken(i, 0.15).map(Math.round)),
    core: arrToHex(lighten(r, 0.65)),
    intentBright: arrToHex(lighten(i, 0.45)),
  };
}

export function customColors() {
  try {
    const c = JSON.parse(localStorage.getItem(CUSTOM_KEY));
    if (c && /^#[0-9a-fA-F]{6}$/.test(c.reality) && /^#[0-9a-fA-F]{6}$/.test(c.intent)) return c;
  } catch (e) {}
  return { reality: '#4DA6FF', intent: '#A78BFA' };
}

function resolvePalette(id) {
  if (id === 'custom') {
    const c = customColors();
    return derivePalette(c.reality, c.intent);
  }
  const t = THEMES.find(x => x.id === id) || THEMES[0];
  return derivePalette(t.reality, t.intent);
}

// Live accent view for the canvas renderers — mutated in place on theme
// switch so importers can hold the reference forever.
export const ACCENT = Object.assign({}, derivePalette('#4DA6FF', '#A78BFA'));

let _cache = {};
export function ac(kind, alpha) {
  const k = kind + alpha;
  let s = _cache[k];
  if (s) return s;
  const rgb = kind === 'r' ? ACCENT.realityRgb : kind === 'i' ? ACCENT.intentRgb : ACCENT.intentDeepRgb;
  s = 'rgba(' + rgb + ',' + alpha + ')';
  _cache[k] = s;
  return s;
}

function cssBlockFor(id, p) {
  return ':root[data-theme="' + id + '"]{' +
    '--reality:' + p.reality + ';' +
    '--reality-rgb:' + p.realityRgb + ';' +
    '--intent:' + p.intent + ';' +
    '--intent-rgb:' + p.intentRgb + ';' +
    '--intent-deep-rgb:' + p.intentDeepRgb + ';' +
    '--glow-reality:0 0 18px rgba(' + p.realityRgb + ',.5),0 0 42px rgba(' + p.realityRgb + ',.16);' +
    '--glow-intent:0 0 14px rgba(' + p.intentDeepRgb + ',.16);' +
    '}';
}

// Preset override blocks — injected once. The default (phosphor) needs no
// block: it IS the :root values in style.css.
function injectThemeCss() {
  if (document.getElementById('themeCss')) return;
  let css = '';
  THEMES.forEach(t => {
    if (t.id === 'phosphor') return;
    css += cssBlockFor(t.id, derivePalette(t.reality, t.intent));
  });
  const el = document.createElement('style');
  el.id = 'themeCss';
  el.textContent = css;
  document.head.appendChild(el);
}

// Custom block — regenerated on every custom apply (colors are user-picked).
function injectCustomCss(p) {
  let el = document.getElementById('themeCssCustom');
  if (!el) {
    el = document.createElement('style');
    el.id = 'themeCssCustom';
    document.head.appendChild(el);
  }
  el.textContent = cssBlockFor('custom', p);
}

export function currentThemeId() {
  try { return localStorage.getItem(STORAGE_KEY) || 'phosphor'; } catch (e) { return 'phosphor'; }
}

export function applyTheme(id) {
  const known = id === 'custom' || THEMES.some(t => t.id === id);
  if (!known) id = 'phosphor';
  injectThemeCss();
  const p = resolvePalette(id);
  if (id === 'custom') injectCustomCss(p);
  if (id === 'phosphor') delete document.documentElement.dataset.theme;
  else document.documentElement.dataset.theme = id;
  try { localStorage.setItem(STORAGE_KEY, id); } catch (e) {}

  Object.assign(ACCENT, p);
  _cache = {};

  document.querySelectorAll('#themeRow .theme-chip').forEach(b => {
    b.classList.toggle('active', b.dataset.theme === id);
  });
  return p;
}

export function setCustomColors(realityHex, intentHex) {
  try { localStorage.setItem(CUSTOM_KEY, JSON.stringify({ reality: realityHex, intent: intentHex })); } catch (e) {}
  applyTheme('custom');
}

/** Build the swatch row + custom picker inside the Settings Theme card.
 *  Call once at init, after applyTheme(currentThemeId()). */
export function initThemeUI() {
  const host = document.getElementById('themeRow');
  if (!host) return;
  const cur = currentThemeId();
  host.innerHTML = '';
  THEMES.forEach(t => {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'theme-chip' + (t.id === cur ? ' active' : '');
    b.dataset.theme = t.id;
    b.setAttribute('data-tip', t.name + ' · reality ' + t.reality + ' / intent ' + t.intent);
    b.innerHTML =
      '<span class="tdot" style="background:' + t.reality + ';box-shadow:0 0 6px ' + t.reality + '"></span>' +
      '<span class="tdot" style="background:' + t.intent + '"></span>' +
      t.name.toUpperCase();
    b.addEventListener('click', () => applyTheme(t.id));
    host.appendChild(b);
  });

  // Custom chip + the two native color pickers (reality, intent). Picking a
  // color live-applies — the native input fires `input` continuously while
  // dragging inside the picker, so the whole page previews in real time.
  const c = customColors();
  const wrap = document.createElement('div');
  wrap.className = 'theme-custom';
  wrap.innerHTML =
    '<button type="button" class="theme-chip' + (cur === 'custom' ? ' active' : '') + '" data-theme="custom" ' +
    'data-tip="Your own accent pair — left picker = reality (measured truth), right = intent (commanded)">CUSTOM</button>' +
    '<input type="color" id="themeCustReality" value="' + c.reality + '" data-tip="Reality accent — live position, active controls">' +
    '<input type="color" id="themeCustIntent" value="' + c.intent + '" data-tip="Intent accent — commanded / window band">';
  host.appendChild(wrap);

  const chipBtn = wrap.querySelector('.theme-chip');
  const rIn = wrap.querySelector('#themeCustReality');
  const iIn = wrap.querySelector('#themeCustIntent');
  const commit = () => setCustomColors(rIn.value, iIn.value);
  chipBtn.addEventListener('click', commit);
  rIn.addEventListener('input', commit);
  iIn.addEventListener('input', commit);
}
