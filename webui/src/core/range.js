/**
 * Stroke Window designer — the draggable track with handles that sets the
 * active motion range (min/max mm). Changes push to the firmware LIVE so
 * Intiface mapping follows immediately. The glowing green posdot tracks the
 * actual carriage position, eased smoothly every animation frame.
 *
 * This is the visual topdog of the Drive tab. Slide the whole window, trim
 * the ends — the hardware obeys in real time. No save button needed. :3
 */
import { $, clamp, setRead, icon, toast } from './ui.js';
import { post } from './api.js';

export const TRAVEL = 240;

// Shared state — exported so generator, settings, and status poll can reference.
// Importers must use setWinMin/setWinMax to reassign (ESM export let is read-only).
export let winMin = 10, winMax = 110;
export function setWinMin(v) { winMin = v; }
export function setWinMax(v) { winMax = v; }

/** Gate: don't push window changes to firmware until loadSettings() populates us. Importers use setWindowReady(). */
export let windowReady = false;
export function setWindowReady(v) { windowReady = v; }

let windowPushTimer = null;

/**
 * Push the current stroke window to /api/settings (live, no NVS persist).
 * Debounced 60ms so rapid dragging doesn't flood the REST handler.
 */
export function pushWindow() {
  if (!windowReady) return;
  clearTimeout(windowPushTimer);
  windowPushTimer = setTimeout(() => {
    post('/api/settings', {
      range_min: Math.round(winMin),
      range_max: Math.round(winMax),
      no_persist: true,
    });
  }, 60);
}

/**
 * Re-render the visual window position and update all readouts.
 */
export function renderWindow() {
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
  setRead('winLen', Math.round(winMax - winMin));
  setRead('depthVal', Math.round(winMax - winMin));
  setRead('spanVal', Math.round(winMin) + '\u2013' + Math.round(winMax));
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

/* ---- Pointer drag state for the range designer ---- */
let drag = null, dragStart = 0, startMin = 0, startMax = 0;

function pxToMm(clientY) {
  const host = $('trackHost');
  if (!host) return 0;
  const r = host.getBoundingClientRect();
  const frac = clamp((clientY - r.top) / r.height, 0, 1);
  return (1 - frac) * TRAVEL;
}

function startDrag(mode, e) {
  drag = mode;
  dragStart = pxToMm(e.clientY);
  startMin = winMin;
  startMax = winMax;
  e.target.setPointerCapture && e.target.setPointerCapture(e.pointerId);
  e.preventDefault();
}

function onDrag(e) {
  if (!drag) return;
  const mm = pxToMm(e.clientY);
  if (drag === 'move') {
    const span = startMax - startMin;
    let delta = mm - dragStart;
    let nMin = clamp(startMin + delta, 0, TRAVEL - span);
    winMin = nMin; winMax = nMin + span;
  } else if (drag === 'top') {
    winMax = clamp(mm, winMin + 5, TRAVEL);
  } else if (drag === 'bot') {
    winMin = clamp(mm, 0, winMax - 5);
  }
  renderWindow();
  e.preventDefault();
}

function endDrag() { drag = null; }

export function initRangeDesigner() {
  const win = $('win'), top = $('handleTop'), bot = $('handleBot');
  if (win) win.addEventListener('pointerdown', e => { if (e.target.closest('.handle')) return; startDrag('move', e); });
  if (top) top.addEventListener('pointerdown', e => startDrag('top', e));
  if (bot) bot.addEventListener('pointerdown', e => startDrag('bot', e));
  window.addEventListener('pointermove', onDrag);
  window.addEventListener('pointerup', endDrag);
  window.addEventListener('pointercancel', endDrag);
  window.addEventListener('resize', renderWindow);

  // Number input binding
  const minNum = $('minNum'), maxNum = $('maxNum');
  if (minNum) minNum.addEventListener('change', () => { winMin = clamp(parseInt(minNum.value) || 0, 0, winMax - 5); renderWindow(); });
  if (maxNum) maxNum.addEventListener('change', () => { winMax = clamp(parseInt(maxNum.value) || 0, winMin + 5, TRAVEL); renderWindow(); });

  // Bypass limits toggle
  const bypass = $('bypassLimits');
  if (bypass) bypass.addEventListener('change', syncManualWindow);
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

// ===================== Smooth position line animator =====================
// The status poll updates posTarget; this RAF loop eases the glowing green
// dot toward the latest target so it glides instead of snapping 1/sec.
let posTarget = 0, posDisplay = 0, posAnimStarted = false;

export function setPosTarget(v) { posTarget = clamp(v, 0, TRAVEL); }

function animatePosLine() {
  posDisplay += (posTarget - posDisplay) * 0.18;
  if (Math.abs(posTarget - posDisplay) < 0.05) posDisplay = posTarget;
  const host = $('trackHost'), dot = $('posDot');
  if (host && dot) {
    const H = host.clientHeight;
    dot.style.top = ((1 - clamp(posDisplay, 0, TRAVEL) / TRAVEL) * H) + 'px';
  }
  requestAnimationFrame(animatePosLine);
}

export function startPosAnim() {
  if (!posAnimStarted) {
    posAnimStarted = true;
    requestAnimationFrame(animatePosLine);
  }
}