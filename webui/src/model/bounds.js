/**
 * bounds.js -- window bounds and 0..1 normalization, one home.
 *
 * Constraints:
 * - travelBounds() is the ONLY place the rail extent's preference order lives.
 * - norm() returns null, never 0, for a non-number: a caller drawing a
 *   position must WITHHOLD on null (Ground Truth Doctrine).
 * - Device-agnostic: numbers and catalog bounds only, never a channel id, a
 *   field name or a unit.
 */

/** Value -> 0..1 across [lo, hi], clamped. null when there is nothing to draw. */
export function norm(v, lo, hi) {
  if (v == null || !isFinite(v)) return null;
  const span = hi - lo;
  if (!(span > 0)) return null;
  return Math.min(1, Math.max(0, (v - lo) / span));
}

/**
 * The rail's travel extent, as {lo, hi, span}.
 *
 * `measured` (RFC-041 geometry.measured_travel) is this session's own home and
 * outranks `configured` (geometry.max_travel), a number the operator typed.
 * `fallbackHi` is the window field's catalog `max`, the window SETTING's legal
 * ceiling rather than the rail's length, so it serves only a hub annotating
 * neither role. Zero is never a measurement; `span` carries a floor.
 */
export function travelBounds(lo, measured, configured, fallbackHi) {
  const usable = (n) => typeof n === 'number' && isFinite(n) && n > 0;
  let hi;
  if (usable(measured)) {
    hi = lo + measured;
  } else if (usable(configured)) {
    hi = lo + configured;
  } else if (fallbackHi != null) {
    hi = fallbackHi;
  } else {
    hi = lo + 1;
  }
  return { lo, hi, span: Math.max(hi - lo, 1e-9) };
}
