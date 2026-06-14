/**
 * UI framework — icon injection, toasts, tab switching, collapsible cards,
 * tooltips, slider gradient painting, and general DOM sugar.
 * Imported by main.js at boot; works on the static HTML shell.
 */

// ===================== SVG icon sprite (Lucide / MIT-licensed paths) =====================
const ICONS = {
  'i-bolt':     '<path d="M13 2L4.09 12.11a1 1 0 00.78 1.63H11l-1 8 8.91-10.11a1 1 0 00-.78-1.63H12l1-8z"/>',
  'i-range':    '<path d="M12 3v18"/><path d="M8 7l4-4 4 4"/><path d="M8 17l4 4 4-4"/>',
  'i-gauge':    '<path d="M12 14l4-4"/><path d="M3.34 19a10 10 0 1117.32 0"/>',
  'i-heart':    '<path d="M19 14c1.49-1.46 3-3.21 3-5.5A5.5 5.5 0 0016.5 3c-1.76 0-3 .5-4.5 2-1.5-1.5-2.74-2-4.5-2A5.5 5.5 0 002 8.5c0 2.29 1.51 4.04 3 5.5l7 7 7-7z"/>',
  'i-sliders':  '<path d="M4 21v-7"/><path d="M4 10V3"/><path d="M12 21v-9"/><path d="M12 8V3"/><path d="M20 21v-5"/><path d="M20 12V3"/><path d="M1 14h6"/><path d="M9 8h6"/><path d="M17 16h6"/>',
  'i-terminal': '<path d="M4 17l6-6-6-6"/><path d="M12 19h8"/>',
  'i-gear':     '<path d="M12.22 2h-.44a2 2 0 00-2 2v.18a2 2 0 01-1 1.73l-.43.25a2 2 0 01-2 0l-.15-.08a2 2 0 00-2.73.73l-.22.38a2 2 0 00.73 2.73l.15.1a2 2 0 011 1.72v.51a2 2 0 01-1 1.74l-.15.09a2 2 0 00-.73 2.73l.22.38a2 2 0 002.73.73l.15-.08a2 2 0 012 0l.43.25a2 2 0 011 1.73V20a2 2 0 002 2h.44a2 2 0 002-2v-.18a2 2 0 011-1.73l.43-.25a2 2 0 012 0l.15.08a2 2 0 002.73-.73l.22-.39a2 2 0 00-.73-2.73l-.15-.08a2 2 0 01-1-1.74v-.5a2 2 0 011-1.74l.15-.09a2 2 0 00.73-2.73l-.22-.38a2 2 0 00-2.73-.73l-.15.08a2 2 0 01-2 0l-.43-.25a2 2 0 01-1-1.73V4a2 2 0 00-2-2z"/><circle cx="12" cy="12" r="3"/>',
  'i-chevron':  '<path d="M6 9l6 6 6-6"/>',
  'i-info':     '<circle cx="12" cy="12" r="10"/><path d="M12 16v-4"/><path d="M12 8h.01"/>',
  'i-alert':    '<path d="M10.29 3.86L1.82 18a2 2 0 001.71 3h16.94a2 2 0 001.71-3L13.71 3.86a2 2 0 00-3.42 0z"/><path d="M12 9v4"/><path d="M12 17h.01"/>',
  'i-home':     '<path d="M3 9l9-7 9 7v11a2 2 0 01-2 2H5a2 2 0 01-2-2z"/><path d="M9 22V12h6v10"/>',
  'i-stop':     '<rect x="5" y="5" width="14" height="14" rx="2"/>',
  'i-pause':    '<rect x="6" y="4" width="4" height="16" rx="1"/><rect x="14" y="4" width="4" height="16" rx="1"/>',
  'i-play':     '<path d="M5 3l14 9-14 9V3z"/>',
  'i-wave':     '<path d="M2 12c2-7 4-7 6 0s4 7 6 0 4-7 6 0"/>',
  'i-up':       '<path d="M18 15l-6-6-6 6"/>',
  'i-down':     '<path d="M6 9l6 6 6-6"/>',
  'i-save':     '<path d="M19 21H5a2 2 0 01-2-2V5a2 2 0 012-2h11l5 5v11a2 2 0 01-2 2z"/><path d="M17 21v-8H7v8"/><path d="M7 3v5h8"/>',
  'i-reset':    '<path d="M3 2v6h6"/><path d="M3.51 15a9 9 0 102.13-9.36L3 8"/>',
  'i-check':    '<path d="M20 6L9 17l-5-5"/>',
  'i-thermo':   '<path d="M14 14.76V3.5a2.5 2.5 0 00-5 0v11.26a4.5 4.5 0 105 0z"/>',
  'i-zap-off':  '<path d="M12.41 6.75L13 2l-2.43 2.92"/><path d="M18.57 12.91L21 10h-5.34"/><path d="M8 8l-5 6h7l-1 8 5-6"/><path d="M1 1l22 22"/>',
  'i-cable':    '<path d="M4 9a4 4 0 014-4h0a4 4 0 014 4v6a4 4 0 004 4h0a4 4 0 004-4"/>',
  'i-shield':   '<path d="M12 2l8 4v6c0 5-3.5 8-8 10-4.5-2-8-5-8-10V6z"/>',
  'i-target':   '<circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="4"/><circle cx="12" cy="12" r="1"/>',
  'i-link':     '<path d="M10 13a5 5 0 007.07 0l3-3a5 5 0 10-7.07-7.07l-1.72 1.72"/><path d="M14 11a5 5 0 00-7.07 0l-3 3a5 5 0 107.07 7.07l1.72-1.72"/>',
};

/**
 * Build an inline SVG referencing the icon sprite (inline paths, no external defs needed).
 */
export function icon(id) {
  const p = ICONS[id];
  if (!p) return '';
  return `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">${p}</svg>`;
}

/**
 * Inject icons into every [data-ico] element. Call once after DOM is ready.
 */
export function injectIcons() {
  document.querySelectorAll('[data-ico]').forEach(el => {
    el.innerHTML = icon(el.dataset.ico);
  });
}

// ===================== DOM helpers =====================

/** Shorthand getElementById. */
// $ accepts both 'myId' and '#myId' — strips the leading # so callers
// can use either form without blowing up. :3
export const $ = (id) => document.getElementById(id.startsWith('#') ? id.slice(1) : id);

/** Clamp a value between a and b. */
export const clamp = (v, a, b) => Math.max(a, Math.min(b, v));

/** Set textContent of an element by id. */
export function setRead(id, v) {
  const el = $(id);
  if (el) el.textContent = v;
}

// ===================== Toasts =====================

/**
 * Show a floating toast notification. Slides in from the top, auto-dismisses.
 * @param {string} msg - Message text
 * @param {'good'|'warn'|'bad'|'info'} kind
 * @param {string} ico - Icon id (e.g. 'i-check')
 * @param {number} ms - Duration in ms
 */
export function toast(msg, kind = 'info', ico = 'i-info', ms = 4000) {
  const wrap = $('toasts');
  if (!wrap) return;
  const t = document.createElement('div');
  t.className = 'toast ' + kind;
  t.innerHTML = '<span class="ti">' + icon(ico) + '</span><span>' + msg + '</span>';
  wrap.appendChild(t);
  setTimeout(() => { t.classList.add('out'); setTimeout(() => t.remove(), 260); }, ms);
}

// ===================== Tab switching =====================

const TAB_LABELS = { drive: 'Drive', health: 'Health', settings: 'Settings', log: 'Log' };

export function initTabs() {
  // Label injection
  document.querySelectorAll('.tab').forEach(t => {
    if (!t.querySelector('span')) {
      t.insertAdjacentHTML('beforeend', '<span>' + (TAB_LABELS[t.dataset.tab] || '') + '</span>');
    }
  });

  // Tab switching — toggleable: clicking an active tab deselects it,
  // collapsing the content area so nothing is shown. Clicking an inactive
  // tab shows its content. Only one tab can be active at a time. :3
  document.querySelectorAll('.tab').forEach(btn => {
    btn.addEventListener('click', () => {
      const target = document.getElementById(btn.dataset.tab);
      btn.classList.toggle('active');
      if (target) target.classList.toggle('active');
    });
  });
}

// ===================== Collapsible cards =====================

function syncCardBody(card) {
  const body = card.querySelector('.card-body');
  if (!body) return;
  body.style.maxHeight = card.classList.contains('collapsed')
    ? '0px'
    : (body.scrollHeight + 40) + 'px';
}

let collapsibleInited = false;
export function initCollapsibleCards() {
  // Guard against double-init — cloneSidebarCards() also calls this after
  // moving DOM nodes, and duplicate click handlers cancel each other out. :3
  if (collapsibleInited) {
    // Just re-measure card bodies at their new positions
    document.querySelectorAll('.card.collapsible:not(.collapsed)').forEach(syncCardBody);
    return;
  }
  collapsibleInited = true;

  document.querySelectorAll('.card.collapsible').forEach(card => {
    const head = card.querySelector('.card-head');
    if (!head) return;
    head.addEventListener('click', e => {
      if (e.target.closest('.info')) return;
      card.classList.toggle('collapsed');
      syncCardBody(card);
    });
    requestAnimationFrame(() => syncCardBody(card));
  });
  window.addEventListener('resize', () => {
    document.querySelectorAll('.card.collapsible:not(.collapsed)').forEach(syncCardBody);
  });
}

// ===================== Info tooltips =====================

export function initTooltips() {
  document.querySelectorAll('.info').forEach(b => {
    const tip = document.createElement('span');
    tip.className = 'tip';
    tip.innerHTML = b.dataset.tip || '';
    b.appendChild(tip);
    b.addEventListener('click', e => {
      e.preventDefault();
      e.stopPropagation();
      const wasOpen = b.classList.contains('open');
      document.querySelectorAll('.info.open').forEach(o => o.classList.remove('open'));
      if (!wasOpen) b.classList.add('open');
    });
  });
  document.addEventListener('click', () => {
    document.querySelectorAll('.info.open').forEach(o => o.classList.remove('open'));
  });
}

// ===================== Slider gradient painting =====================

function lerp(a, b, t) { return a + (b - a) * t; }
function hex(c) { return '#' + c.map(x => Math.round(clamp(x, 0, 255)).toString(16).padStart(2, '0')).join(''); }
const C_ACCENT = [108, 140, 255], C_WARN = [251, 191, 36], C_BAD = [251, 111, 132];

function colorAt(frac) {
  frac = clamp(frac, 0, 1);
  if (frac < 0.5) {
    const t = frac / 0.5;
    return hex([lerp(C_ACCENT[0], C_WARN[0], t), lerp(C_ACCENT[1], C_WARN[1], t), lerp(C_ACCENT[2], C_WARN[2], t)]);
  }
  const t = (frac - 0.5) / 0.5;
  return hex([lerp(C_WARN[0], C_BAD[0], t), lerp(C_WARN[1], C_BAD[1], t), lerp(C_WARN[2], C_BAD[2], t)]);
}

// All sliders that get the gradient fill treatment. The accel/speed sliders
// are in both the Live Overrides card and the Default Motion card — both IDs
// are listed here so paintSlider() colours them correctly regardless of which
// card the operator is looking at. :3
const GRADIENT_SLIDERS = ['maxSpeed', 'accel', 'lookahead', 'overshoot', 'genRate', 'runCurrent', 'defMaxSpeed', 'defAccel', 'defLookahead', 'defOvershoot', 'modRate', 'modAmp'];

export function paintSlider(slider) {
  if (!slider || !GRADIENT_SLIDERS.includes(slider.id)) return;
  const min = parseFloat(slider.min), max = parseFloat(slider.max), v = parseFloat(slider.value);
  const frac = (v - min) / (max - min || 1);
  const thumb = colorAt(frac);
  const pct = (frac * 100).toFixed(1) + '%';
  slider.style.background = 'linear-gradient(90deg, ' + colorAt(0) + ' 0%, ' + thumb + ' ' + pct + ', var(--card-2) ' + pct + ')';
  slider.style.setProperty('--thumb', thumb);
}

export function onLiveSlider(readId, slider, key) {
  setRead(readId, slider.value);
  paintSlider(slider);
}

// ===================== Data-action attribute wiring =====================

/**
 * Wire up buttons with [data-action] attributes to call window[action] with
 * comma-split args. This de-couples inline onclick from the HTML shell.
 */
export function wireActions() {
  document.querySelectorAll('[data-action]').forEach(el => {
    const action = el.dataset.action;
    const args = el.dataset.arg ? el.dataset.arg.split(',') : [];
    el.addEventListener('click', () => {
      const fn = window[action];
      if (typeof fn === 'function') fn(...args);
    });
  });
}