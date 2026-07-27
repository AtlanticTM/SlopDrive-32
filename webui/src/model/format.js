/**
 * format.js — value presentation, driven entirely by catalog metadata.
 *
 * No unit table, no per-field precision map. The catalog says a field is in
 * `mm` with step 1, or `mm/s3` with step 1000, and that is enough to render it
 * sensibly on a machine we have never met.
 */

/** Decimal places implied by a step. step 0.05 -> 2, step 1 -> 0, absent -> 2. */
export function precisionFor(field) {
  const step = field && field.step;
  if (step == null || !isFinite(step) || step <= 0) {
    // No step published. Integers read better without a false ".00"; floats
    // need some precision or every value looks quantised.
    return field && field.typeName && /^(u|i)\d/.test(field.typeName) ? 0 : 2;
  }
  if (step >= 1) return 0;
  const d = Math.ceil(-Math.log10(step));
  return Math.min(Math.max(d, 0), 4);
}

/** Format a numeric value for display, without the unit. */
export function formatValue(field, value) {
  if (value == null || value === '') return '--';
  if (typeof value === 'boolean') return value ? 'on' : 'off';
  if (typeof value === 'string') return value;
  if (!isFinite(value)) return '--';
  // Large magnitudes get thin-space grouping so 2000000 is readable at a glance.
  const p = precisionFor(field);
  const n = Number(value).toFixed(p);
  return Math.abs(value) >= 10000 ? groupThousands(n) : n;
}

function groupThousands(s) {
  const [i, f] = String(s).split('.');
  return i.replace(/\B(?=(\d{3})+(?!\d))/g, ' ') + (f ? '.' + f : '');
}

/** Unit suffix, or '' when the catalog gave none. */
export function unitOf(field) {
  const u = field && field.unit;
  if (!u || u === 'flag' || u === 'count' || u === '-') return '';
  return u;
}

/** Full "value unit" string. */
export function formatWithUnit(field, value) {
  const v = formatValue(field, value);
  const u = unitOf(field);
  return u ? v + ' ' + u : v;
}

/**
 * Option label for a select-ish field. Falls back to the raw index rather than
 * inventing a name — an unlabelled option is the machine's omission to show,
 * not ours to paper over.
 */
export function optionLabel(field, value) {
  if (!field || !field.options) return String(value);
  const i = Number(value);
  const l = field.options[i];
  return (l == null || l === '') ? String(value) : l;
}

/** Humane elapsed time from a ms epoch. */
export function since(ms) {
  if (!ms) return '--';
  const s = Math.max(0, Math.round((Date.now() - ms) / 1000));
  if (s < 60) return s + 's';
  if (s < 3600) return Math.floor(s / 60) + 'm ' + (s % 60) + 's';
  return Math.floor(s / 3600) + 'h ' + Math.floor((s % 3600) / 60) + 'm';
}

/** Seconds -> compact uptime. */
export function uptime(sec) {
  if (sec == null || !isFinite(sec)) return '--';
  const d = Math.floor(sec / 86400);
  const h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  if (d) return d + 'd ' + h + 'h';
  if (h) return h + 'h ' + m + 'm';
  return m + 'm ' + Math.floor(sec % 60) + 's';
}

/** Bytes -> KB/MB with one decimal. */
export function bytes(n) {
  if (n == null || !isFinite(n)) return '--';
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
  return (n / (1024 * 1024)).toFixed(1) + ' MB';
}
