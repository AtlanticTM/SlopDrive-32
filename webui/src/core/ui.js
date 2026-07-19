/**
 * UI framework — icon injection, toasts, tab switching, collapsible cards,
 * tooltips, slider gradient painting, and general DOM sugar.
 * Imported by main.js at boot; works on the static HTML shell.
 */
import { ACCENT } from './theme.js';

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

// ===================== pad() — constant-length numeral formatter =====================
/**
 * Zero-pad a value to a constant-length string. Negative numbers steal a
 * leading digit slot for the sign so the total length never changes.
 *
 *   pad(72,   3, 1)    → "072.0"
 *   pad(0.42, 2, 2)    → "00.42"
 *   pad(-52,  4, 0)    → "-052"   (sign steals one of the 4 int slots)
 *   pad(52,   4, 0)    → "0052"   (same length as -052)
 *   pad(8.3,  2, 1,'A')→ "08.3A"
 *
 * @param {number} value
 * @param {number} intDigits  — total integer-column width (incl. sign if negative)
 * @param {number} fracDigits — decimal places (0 = no decimal point)
 * @param {string} [unit]     — optional unit suffix appended after the digits
 * @returns {string}
 */
export function pad(value, intDigits, fracDigits, unit) {
  var neg = value < 0;
  var abs = Math.abs(value);
  var fixed = abs.toFixed(fracDigits || 0);
  var parts = fixed.split('.');
  var intStr = neg
    ? '-' + parts[0].padStart(intDigits - 1, '0')
    : parts[0].padStart(intDigits, '0');
  var fracStr = (fracDigits > 0 && parts[1]) ? '.' + parts[1] : '';
  return intStr + fracStr + (unit || '');
}

// ===================== .vv measurement — fix character width at wdth 112 =====================
// The .vv component needs a FIXED box width so the variable-axis alarm swell
// (wdth 95→112) grows leftward into reserved space with zero layout shift.
// We measure one character of Martian Mono at "wdth" 112 once at init and
// store it as --mm-ch on :root. Then .vv uses width: calc(var(--vv-chars) * var(--mm-ch)).

function _measureNow() {
  var probe = document.createElement('span');
  probe.style.cssText = 'position:absolute;visibility:hidden;font-family:"Martian Mono",monospace;font-size:1em;font-variation-settings:"wght" 400,"wdth" 112;white-space:pre;';
  probe.textContent = '0';
  document.body.appendChild(probe);
  var w = probe.getBoundingClientRect().width;
  // Guard: fallback fonts can measure narrow; use em-relative floor
  document.documentElement.style.setProperty('--mm-ch', w + 'px');
  document.body.removeChild(probe);
}

export function measureMartianMonoCh() {
  _measureNow();
  // Fonts load async (font-display:block + base64, but still async decode).
  // A pre-font measurement uses the fallback monospace metrics, which are
  // narrower than Martian Mono at wdth 112 — that's the "numbers cut off"
  // bug. Re-measure once all fonts are ready.
  if (document.fonts && document.fonts.ready) {
    document.fonts.ready.then(function() { _measureNow(); });
  }
}

// ===================== setVV — set a .vv element's text + char count =====================
/**
 * Format a value with pad() and write it into a .vv element, auto-setting
 * the --vv-chars custom property so the fixed-width box is sized correctly.
 *
 * @param {string} id — element id (with or without #)
 * @param {number} value
 * @param {number} intDigits
 * @param {number} fracDigits
 * @param {string} [unit]
 */
export function setVV(id, value, intDigits, fracDigits, unit) {
  var el = $(id);
  if (!el) return;
  var s = pad(value, intDigits, fracDigits, unit);
  el.textContent = s;
  // Set --vv-chars based on the formatted string length (excluding unit suffix)
  var digitsLen = pad(value, intDigits, fracDigits).length;
  el.style.setProperty('--vv-chars', digitsLen);
}

/**
 * Apply alarm state class (.w1/.w2) to a .vv element based on a threshold.
 * @param {string} id — element id
 * @param {number} level — 0=nominal, 1=warn, 2=bad
 */
export function setVVState(id, level) {
  var el = $(id);
  if (!el) return;
  el.classList.remove('w1', 'w2');
  if (level === 1) el.classList.add('w1');
  else if (level === 2) el.classList.add('w2');
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
  // Announce toasts to assistive tech without stealing focus.
  if (!wrap.hasAttribute('aria-live')) {
    wrap.setAttribute('aria-live', 'polite');
    wrap.setAttribute('role', 'status');
  }
  const t = document.createElement('div');
  t.className = 'toast ' + kind;
  t.innerHTML = '<span class="ti">' + icon(ico) + '</span><span>' + msg + '</span>';
  wrap.appendChild(t);
  setTimeout(() => { t.classList.add('out'); setTimeout(() => t.remove(), 260); }, ms);
}

// ===================== Tab switching =====================

const TAB_LABELS = { drive: 'Drive', settings: 'Settings', log: 'Log' };
const PANE_BREAKPOINT = '(max-width: 760px)';

export function initTabs() {
  // Label injection + ARIA tab semantics
  document.querySelectorAll('.tab').forEach(t => {
    if (!t.querySelector('span')) {
      t.insertAdjacentHTML('beforeend', '<span>' + (TAB_LABELS[t.dataset.tab] || '') + '</span>');
    }
    t.setAttribute('role', 'tab');
    t.setAttribute('aria-selected', t.classList.contains('active') ? 'true' : 'false');
  });

  // ===== Split-pane doctrine (§1.6b) =====
  // Desktop (>760px): Drive is always present; clicking SETTINGS/LOG opens a
  // sticky pane beside it (mainGrid.split + .paneView.active), click again
  // closes it back to Drive-only. Below 760px: classic three-tab — driveMain/
  // sidePane get .m-active, the DRIVE pill reappears, the pane goes static.
  // This REPLACES the old single-active-tab-content model entirely (both tab
  // bars still share data-tab ids, so one activateTab() drives both).
  const mainGrid  = document.getElementById('mainGrid');
  const driveMain = document.getElementById('driveMain');
  const sidePane  = document.getElementById('sidePane');
  const isSplitUi = !!(mainGrid && driveMain && sidePane);
  const isMobile  = () => window.matchMedia(PANE_BREAKPOINT).matches;
  let openPane = null;

  function showPaneView(name) {
    document.querySelectorAll('.paneView').forEach(v => v.classList.remove('active'));
    if (name) {
      const v = document.getElementById('pv-' + name);
      if (v) v.classList.add('active');
    }
  }
  function setTabActive(name) {
    document.querySelectorAll('.tab').forEach(t => {
      const on = t.dataset.tab === name;
      t.classList.toggle('active', on);
      t.setAttribute('aria-selected', on ? 'true' : 'false');
    });
  }
  function scrollToTop() {
    // Fresh panel → start scrolled at the top so the operator always sees the
    // head of the list, never a random mid-scroll position from last visit.
    const host = sidePane || document.querySelector('.tab-contents-host');
    if (host) host.scrollTop = 0;
    if (window.scrollTo) window.scrollTo(0, 0);
  }

  function activateTab(id) {
    // Buttons without data-tab (the DIAG toggle) share .tab styling but are
    // NOT panes — ignore them entirely or they'd clear every active state.
    if (!id) return;
    if (!isSplitUi) {
      // Defensive fallback for the old single-tab-content model, in case any
      // markup still uses it — keeps this function harmless if ever reused.
      document.querySelectorAll('.tab-content').forEach(c => c.classList.toggle('active', c.id === id));
      setTabActive(id);
      scrollToTop();
      return;
    }
    if (isMobile()) {
      driveMain.classList.toggle('m-active', id === 'drive');
      sidePane.classList.toggle('m-active', id !== 'drive');
      showPaneView(id === 'drive' ? null : id);
      setTabActive(id);
      openPane = null;
    } else {
      if (id === 'drive') return; // Drive pill is hidden above the breakpoint
      if (openPane === id) {
        openPane = null;
        mainGrid.classList.remove('split');
        showPaneView(null);
        setTabActive('drive');
      } else {
        openPane = id;
        mainGrid.classList.add('split');
        showPaneView(id);
        setTabActive(id);
      }
    }
    scrollToTop();
    // Newly-visible canvases (scope/spark/heatmap) need a resize pass to pick
    // up their now-correct clientWidth/clientHeight — they no-op if unaffected.
    window.dispatchEvent(new Event('resize'));
  }
  window.__activateTab = activateTab;

  document.querySelectorAll('.tab').forEach(btn => {
    btn.addEventListener('click', () => activateTab(btn.dataset.tab));
  });

  // Live breakpoint crossing — mirrors the click-time isMobile() branch above
  // so a mid-session resize (or DevTools device toggle) lands in a consistent
  // state instead of stranding an open desktop pane under the mobile layout.
  if (isSplitUi) {
    window.matchMedia(PANE_BREAKPOINT).addEventListener('change', m => {
      if (m.matches) {
        mainGrid.classList.remove('split');
        driveMain.classList.add('m-active');
        sidePane.classList.remove('m-active');
        showPaneView(null);
        setTabActive('drive');
        openPane = null;
      } else {
        driveMain.classList.add('m-active');
        sidePane.classList.remove('m-active');
      }
      window.dispatchEvent(new Event('resize'));
    });
  }

  // Arrow-key navigation within each tab bar (roving between sibling tabs).
  document.querySelectorAll('[role="tablist"], .tabs').forEach(bar => {
    bar.addEventListener('keydown', e => {
      if (e.key !== 'ArrowRight' && e.key !== 'ArrowLeft') return;
      const tabs = Array.from(bar.querySelectorAll('.tab'));
      const idx = tabs.indexOf(document.activeElement);
      if (idx === -1) return;
      e.preventDefault();
      const next = tabs[(idx + (e.key === 'ArrowRight' ? 1 : tabs.length - 1)) % tabs.length];
      next.focus();
      activateTab(next.dataset.tab);
    });
  });
}

// ===================== Panel index numbering (§3.1) =========================
// Assigns "01", "02", … to each .card-head h2 in DOM order, continuing across
// Drive/Settings/Log. A JS walk (not a CSS counter) because querySelectorAll
// sees every element regardless of display:none — a CSS counter would skip
// (and silently renumber everything after) any card sitting inside whichever
// .paneView/#driveMain happens to be hidden at the moment. Call once at boot
// and again whenever the DOM's card set can change (capabilities resolving
// builds Power/RS485 cards). Cheap enough to not need debouncing.
export function renumberPanels() {
  let i = 1;
  document.querySelectorAll('.panel, .card').forEach(el => {
    // The rail panel has no visible numeral (matches the mock's bare .pn
    // rail assembly, no .pidx) and retired hidden cards (Stroke Window /
    // Controls — kept only so range.js/main.js have elements to read/write)
    // must not consume a slot ahead of Pattern=01.
    if (el.classList.contains('rail-panel')) return;
    if (el.getAttribute('aria-hidden') === 'true') return;
    const h2 = el.querySelector('.card-head h2');
    if (!h2) return; // e.g. #loadCard before capabilities resolves — no head yet
    h2.setAttribute('data-pidx', i < 10 ? '0' + i : String(i));
    i++;
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
    // Keyboard-operable collapse: the head acts as a disclosure button.
    head.setAttribute('tabindex', '0');
    head.setAttribute('role', 'button');
    head.setAttribute('aria-expanded', card.classList.contains('collapsed') ? 'false' : 'true');
    const toggle = () => {
      card.classList.toggle('collapsed');
      head.setAttribute('aria-expanded', card.classList.contains('collapsed') ? 'false' : 'true');
      syncCardBody(card);
    };
    head.addEventListener('click', e => {
      if (e.target.closest('.info')) return;
      toggle();
    });
    head.addEventListener('keydown', e => {
      if (e.key !== 'Enter' && e.key !== ' ') return;
      if (e.target.closest('.info')) return;
      e.preventDefault();
      toggle();
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
    if (!b.hasAttribute('aria-label')) b.setAttribute('aria-label', 'More info');
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
// Accent endpoint follows the live theme (ACCENT.realityArr); warn/bad are
// safety colors and stay literal in every theme.
const C_WARN = [251, 191, 36], C_BAD = [251, 111, 132];

function colorAt(frac) {
  frac = clamp(frac, 0, 1);
  if (frac < 0.5) {
    const t = frac / 0.5;
    const C_ACCENT = ACCENT.realityArr;
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