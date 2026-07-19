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

  el.innerHTML =
    '<div class="mtr-row"><span class="mtr-label">' + opts.label + '</span><span class="mtr-value">--</span></div>' +
    '<div class="mtr-track">' + hazardHtml + '<div class="mtr-fill" style="width:0%"></div>' +
    (opts.peakHold ? '<div class="mtr-peak" style="left:0%"></div>' : '') +
    '</div>';
  host.appendChild(el);

  var valueEl = el.querySelector('.mtr-value');
  var fillEl = el.querySelector('.mtr-fill');
  var peakEl = el.querySelector('.mtr-peak');
  var peakFrac = 0;

  this.set = function (v) {
    valueEl.textContent = v.toFixed(decimals);
    var frac = Math.max(0, Math.min(1, (v - min) / (max - min)));
    fillEl.style.width = (frac * 98) + '%';
    if (peakEl) {
      if (opts.peakHold === 'ratchet') {
        peakFrac = Math.max(peakFrac, frac);
        peakEl.style.left = (peakFrac * 98) + '%';
      } else {
        peakEl.style.left = (frac * 98) + '%';
      }
    }
    var inHazard = (opts.hazards || []).some(function (z) { return frac >= z[0] && frac <= z[0] + z[1]; });
    valueEl.classList.toggle('mtr-in-hazard', inHazard);
  };

  this.resetPeak = function () { peakFrac = 0; };
}
