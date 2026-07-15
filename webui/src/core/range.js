/**
 * Stroke Window designer — the draggable track with handles that sets the
 * active motion range (min/max mm). Changes push to the firmware LIVE so
 * Intiface mapping follows immediately. The glowing green posdot tracks the
 * actual carriage position, eased smoothly every animation frame.
 *
 * This is the visual topdog of the Drive tab. Slide the whole window, trim
 * the ends — the hardware obeys in real time. No save button needed. :3
 */
import { $, clamp, setRead, icon, toast, pad } from './ui.js';
import { post } from './api.js';
import * as cmd from './cmd.js';
import { OP_SET_WINDOW } from './wire.js';

// TRAVEL is dynamic — set from /api/settings or /api/capabilities on boot so
// the range designer, motion graph, and all clamps use the actual rail length
// the firmware advertises. Default 260 is the geometry ceiling for the 57AIM
// build — the UI is safe at this value even before the API responds. setTravel()
// patches it to the real measured stroke when data arrives. :3
export let TRAVEL = 260;
export function setTravel(mm) {
  if (mm <= 0) return;
  var changed = (Math.abs(TRAVEL - mm) > 0.5);
  TRAVEL = mm;
  // Patch every DOM element that hardcodes the travel limit so the UI
  // reflects the real rail length the moment the API responds. :3
  var t = Math.round(mm);
  var ec = document.getElementById('endcapTop');
  if (ec) ec.textContent = 'Out / ' + t;
  var bh = document.getElementById('bypassHint');
  if (bh) bh.textContent = 'reach the full 0\u2013' + t + ' mm, ignoring the window';
  ['minNum','maxNum','manualPos'].forEach(function(id) {
    var el = document.getElementById(id); if (el) el.max = t;
  });
  ['defMinNum','defMaxNum'].forEach(function(id) {
    var el = document.getElementById(id); if (el) el.max = t;
  });
  // Re-render the stroke window whenever TRAVEL actually changes — covers
  // both the initial API load AND a re-home that produces a different
  // measured stroke (applyCapabilities → setTravel → rescale window). :3
  if (changed) renderWindow();
}

// Shared state — exported so generator, settings, and status poll can reference.
// Importers must use setWinMin/setWinMax to reassign (ESM export let is read-only).
export let winMin = 10, winMax = 110;
export function setWinMin(v) { winMin = v; }
export function setWinMax(v) { winMax = v; }

/** Gate: don't push window changes to firmware until loadSettings() populates us. Importers use setWindowReady(). */
export let windowReady = false;
export function setWindowReady(v) { windowReady = v; }

/** Gate: suppress pushWindow during init load so page refresh doesn't overwrite a live session. */
export let suppressPush = false;
export function setSuppressPush(v) { suppressPush = v; }

let windowPushTimer = null;

/**
 * Push the current stroke window to /api/settings (live, no NVS persist).
 * Debounced 60ms so rapid dragging doesn't flood the REST handler.
 */
export function pushWindow() {
  if (!windowReady || suppressPush) return;
  clearTimeout(windowPushTimer);
  windowPushTimer = setTimeout(() => {
    cmd.send(OP_SET_WINDOW, {
      range_min: Math.round(winMin),
      range_max: Math.round(winMax),
      no_persist: true,
    });
  }, 60);
}

/**
 * Re-render the visual window position and update all readouts.
 * Guarded by TRAVEL > 0 — skips silently before the API responds
 * so we never divide by zero and produce a NaN-broken stroke window. :3
 */
export function renderWindow() {
  if (TRAVEL <= 0) { windowReady = false; return; }
  winMin = clamp(Math.round(winMin), 0, TRAVEL);
  winMax = clamp(Math.round(winMax), 0, TRAVEL);
  if (winMax - winMin < 5) winMax = clamp(winMin + 5, 5, TRAVEL);

  const host = $('trackHost');
  if (!host) return;
  const H = host.clientHeight;
  const topPx = (1 - winMax / TRAVEL) * H;
  const botPx = (1 - winMin / TRAVEL) * H;
  const win = $('win');
  if (win) {
    win.style.top = topPx + 'px';
    win.style.height = Math.max(botPx - topPx, 24) + 'px';
  }
  setRead('winLen', pad(Math.round(winMax - winMin), 3, 0));
  setRead('depthVal', pad(Math.round(winMax - winMin), 3, 0));
  setRead('spanVal', pad(Math.round(winMin), 3, 0) + '\u2013' + pad(Math.round(winMax), 3, 0));
  const minNum = $('minNum'), maxNum = $('maxNum');
  if (minNum) minNum.value = Math.round(winMin);
  if (maxNum) maxNum.value = Math.round(winMax);

  const bypass = $('bypassLimits');
  if (bypass && !bypass.checked) syncManualWindow();

  pushWindow();
}

export function nudgeWindow(d) {
  const dd = parseFloat(d) || 0;
  const span = winMax - winMin;
  let nMin = winMin + dd, nMax = winMax + dd;
  if (nMin < 0) { nMin = 0; nMax = span; }
  if (nMax > TRAVEL) { nMax = TRAVEL; nMin = TRAVEL - span; }
  winMin = nMin; winMax = nMax;
  renderWindow();
}

export function trim(which, d) {
  const dd = parseFloat(d) || 0;
  if (which === 'min') winMin = clamp(winMin + dd, 0, winMax - 5);
  else winMax = clamp(winMax + dd, winMin + 5, TRAVEL);
  renderWindow();
}

export function manualBounds() {
  const bypass = $('bypassLimits');
  if (bypass && bypass.checked) return [0, TRAVEL];
  return [Math.round(winMin), Math.round(winMax)];
}

export function syncManualWindow() {
  const [lo, hi] = manualBounds();
  const s = $('manualPos');
  if (!s) return;
  s.min = lo; s.max = hi;
  s.value = clamp(parseFloat(s.value), lo, hi);
  setRead('manualPosVal', Math.round(s.value));
}

// Number input binding for Window panel min/max fields (moved out of the old
// track designer; wired here so they survive the widget retirement). The rail
// band handles these same values via rail.js's drag/resize → setWinMin/setWinMax.
export function initWindowInputs() {
  const minNum = $('minNum'), maxNum = $('maxNum');
  if (minNum) minNum.addEventListener('change', () => { winMin = clamp(parseInt(minNum.value) || 0, 0, winMax - 5); renderWindow(); });
  if (maxNum) maxNum.addEventListener('change', () => { winMax = clamp(parseInt(maxNum.value) || 0, winMin + 5, TRAVEL); renderWindow(); });

  // Bypass limits toggle
  const bypass = $('bypassLimits');
  if (bypass) bypass.addEventListener('change', syncManualWindow);

  // Target Position slider — send move on release
  const manualPos = $('manualPos');
  if (manualPos) {
    manualPos.addEventListener('input', function() {
      setRead('manualPosVal', Math.round(manualPos.value));
    });
    manualPos.addEventListener('change', function() {
      const mm = parseFloat(manualPos.value);
      if (!isNaN(mm) && typeof window.__sendMove === 'function') {
        window.__sendMove(mm, true);
      }
    });
  }
}

export function useCurrentAsDefault() {
  const dmin = $('defMinNum'), dmax = $('defMaxNum');
  if (dmin) dmin.value = Math.round(winMin);
  if (dmax) dmax.value = Math.round(winMax);
}

// Persistent "Set as Min/Max" from the position card.
// When called from [data-action] wiring, position comes from data-arg which
// is just "min" or "max" (the which side). When there's no numeric position
// we snap to wherever the carriage is RIGHT NOW — that's the whole point of
// "Set as Min/Max" buttons: you park the machine where you want the edge
// and click. Uses the module-level posTarget which the status poll feeds. :3
export async function setBound(which, position) {
  // If position isn't a number (called from data-action with just the side
  // string), use the current live position target from the animator loop.
  var p = parseFloat(position);
  if (isNaN(p)) p = posTarget;
  p = Math.round(p);
  if (which === 'min') winMin = clamp(p, 0, winMax - 5);
  else winMax = clamp(p, winMin + 5, TRAVEL);
  renderWindow();
  toast('Window ' + which + ' set to ' + p + ' mm', 'good', 'i-check');
}

// Current position target — kept as a module-level fallback for setBound()
// (Set-as-Min/Max buttons use this when no explicit position is provided).
// The travel rail updates this from its interpolated display position.
let posTarget = 0;

export function setPosTarget(v) { posTarget = clamp(v, 0, TRAVEL); }

