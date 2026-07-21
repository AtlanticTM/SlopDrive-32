/**
 * Meter — reusable recessed instrument: label + live value + threshold track.
 *
 * label + min/max/decimals + hazard zones (fraction-of-range spans) + an
 * optional peak-hold ratchet caret + a data-tip. One instantiation = one
 * row. Built for the Power card's five bus/thermal readouts (§2.2) but
 * intentionally generic — the next RS485/Modbus register bank reuses this
 * verbatim instead of hand-rolling another .stat block.
 *
 * DOM shape:
 *   .mtr[data-tip]
 *     .mtr-row        .mtr-label / .mtr-value
 *     .mtr-track      .mtr-hazard* / .mtr-fill / .mtr-peak?
 *
 * Namespaced .mtr-* (not .meter/.m-*) because a legacy, unrelated .meter
 * progress-bar widget already exists in style.css — same-specificity, later-
 * wins collision silently clamped every instance to 26px/overflow:hidden
 * until this was renamed (DELTA D1). Do not rename back without checking the
 * legacy widget is gone.
 *
 * No per-frame allocation: .set() only ever touches textContent/style on
 * elements captured once at construction.
 */

/**
 * @param {HTMLElement} host - container to append the meter into
 * @param {Object} opts
 * @param {string} opts.label
 * @param {number} opts.min
 * @param {number} opts.max
 * @param {number} [opts.decimals=1]
 * @param {Array<[number,number]>} [opts.hazards] - [fracStart, fracWidth] spans, 0..1
 * @param {boolean|'ratchet'} [opts.peakHold] - true: live caret at value; 'ratchet': holds max seen
 * @param {string} [opts.tip] - data-tip text
 */
export function Meter(host, opts) {
  var min = opts.min, max = opts.max;
  var decimals = opts.decimals !== undefined ? opts.decimals : 1;

  var el = document.createElement('div');
  el.className = 'mtr';
  if (opts.tip) el.setAttribute('data-tip', opts.tip);

  var hazardHtml = '';
  (opts.hazards || []).forEach(function (z) {
    // Which end does the zone hug? Low-end zones (undervoltage) fade out
    // rightward toward safe territory; high-end zones fade leftward — the
    // hatch is dense at the rail end and dissolves gradually instead of
    // stopping at a hard threshold line (CSS mask, .mtr-hz-lo/-hi).
    var edge = z[0] <= 0.001 ? 'lo' : 'hi';
    hazardHtml += '<div class="mtr-hazard mtr-hz-' + edge + '" style="left:' + (z[0] * 100) + '%;width:' + (z[1] * 100) + '%"></div>';
  });

  // Ratchet meters carry a little numeric readout riding above the amber
  // peak caret (replaces the old dedicated PEAK row) — the row/track gap
  // widens via .mtr-haspeak to give the number its own lane.
  if (opts.peakHold === 'ratchet') el.classList.add('mtr-haspeak');
  el.innerHTML =
    '<div class="mtr-row"><span class="mtr-label">' + opts.label + '</span><span class="mtr-value">--</span></div>' +
    '<div class="mtr-track">' + hazardHtml + '<div class="mtr-fill" style="width:0%"></div>' +
    (opts.peakHold ? '<div class="mtr-peak" style="left:0%"></div>' : '') +
    (opts.peakHold === 'ratchet' ? '<div class="mtr-peak-val" style="display:none"></div>' : '') +
    '</div>';
  host.appendChild(el);

  var valueEl = el.querySelector('.mtr-value');
  var fillEl = el.querySelector('.mtr-fill');
  var peakEl = el.querySelector('.mtr-peak');
  var peakValEl = el.querySelector('.mtr-peak-val');
  var peakFrac = 0;
  var peakVal = null;   // highest value seen (or device-reported via setPeak)

  function frToPct(fr) { return (fr * 98) + '%'; }
  function paintPeak() {
    if (!peakEl) return;
    peakEl.style.left = frToPct(peakFrac);
    if (!peakValEl) return;
    if (peakVal === null) { peakValEl.style.display = 'none'; return; }
    peakValEl.style.display = '';
    peakValEl.textContent = peakVal.toFixed(decimals);
    // Keep the label on the track — clamp so it can't hang off either rail.
    peakValEl.style.left = Math.max(4, Math.min(94, peakFrac * 98)) + '%';
  }

  this.set = function (v) {
    valueEl.textContent = v.toFixed(decimals);
    var frac = Math.max(0, Math.min(1, (v - min) / (max - min)));
    fillEl.style.width = frToPct(frac);
    if (peakEl) {
      if (opts.peakHold === 'ratchet') {
        if (peakVal === null || v > peakVal) { peakVal = v; peakFrac = frac; paintPeak(); }
      } else {
        peakEl.style.left = frToPct(frac);
      }
    }
    var inHazard = (opts.hazards || []).some(function (z) { return frac >= z[0] && frac <= z[0] + z[1]; });
    valueEl.classList.toggle('mtr-in-hazard', inHazard);
  };

  /** Authoritative (device-reported) peak — overrides the local ratchet in
   *  BOTH directions, so a device-side peak reset actually lowers the caret
   *  (Ground Truth: the machine's accumulator wins, not the UI's memory). */
  this.setPeak = function (v) {
    if (opts.peakHold !== 'ratchet') return;
    peakVal = v;
    peakFrac = Math.max(0, Math.min(1, (v - min) / (max - min)));
    paintPeak();
  };

  this.resetPeak = function () { peakFrac = 0; peakVal = null; paintPeak(); };
}
