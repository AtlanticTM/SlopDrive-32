/**
 * format.js — value presentation, driven entirely by catalog metadata.
 *
 * No unit table, no per-field precision map. The catalog says a field is in
 * `mm` with step 1, or `mm/s3` with step 1000, and that is enough to render it
 * sensibly on a machine we have never met.
 */

import { ROLE_LABEL } from './roles.js';

/**
 * PROVENANCE (RFC-048 key 22) -> the adjective that goes in front of a label.
 *
 * Registry vocabulary, so this is protocol wording, not device knowledge:
 * `demand` is what was asked for, `planned` is what the planner is aiming at,
 * `actual` is what was measured. `actual` maps to NOTHING on purpose — it is
 * the wire default, so qualifying every unannotated field with "actual" would
 * put a measurement claim on machines that never made one, which is the exact
 * lie this table exists to prevent in the other direction.
 */
const PROVENANCE_QUALIFIER = { demand: 'Demand', planned: 'Planned' };

/**
 * The display label for ANY field, anywhere in the UI. Resolution order:
 *   1. ROLE_LABEL[field.role] when the field carries a known registry role
 *      (roles.js) — a human-standardized label for machine vocabulary.
 *   2. descLabel(field): the catalog author's own words, used VERBATIM.
 *   3. field.label, which buildSettingsModel already set to
 *      humanize(field.name) at construction time — the honest fallback for a
 *      field this project has no opinion about.
 * The field's own PROVENANCE is composed onto a ROLE label only. A role label
 * names a generic quantity, so demand/planned is what says which pipeline
 * stage the number is; a desc label already carries the author's wording, and
 * qualifying it reads "Demand asked position".
 *
 * The provenance half is not decoration. A hub whose planner renders position
 * publishes `telemetry.position` with provenance `planned`, and a label that
 * read "Actual" over it would be the UI asserting a measurement nobody made —
 * a ground-truth defect, not a wording preference. The word comes from the
 * catalog every time; nothing here assumes anything about which field is
 * measured on which machine.
 *
 * This is the ONLY function in the UI layer that may special-case a role for
 * display text; every component reads through it rather than field.label
 * directly, so a role gets its human label wherever it appears (hero
 * numerals, the rail, the generic Field control, the hero widgets) and an
 * unroled field keeps today's behavior everywhere too.
 */
export function labelFor(field) {
  if (!field) return '';
  const role = field.role && ROLE_LABEL[field.role];
  if (!role) return descLabel(field) || (field.label != null ? field.label : '');
  const q = PROVENANCE_QUALIFIER[field.provenanceName];
  if (!q) return role;
  return q + ' ' + role.charAt(0).toLowerCase() + role.slice(1);
}

/**
 * A wire name is machine vocabulary: `raw_10um` humanizes to "Raw 10um",
 * which names nothing a human asked about. A catalog `desc` is written for a
 * human, so its leading clause (everything before the first `.,;:`) is
 * preferred, but only while it is shaped like a label: 2-3 words, at most 30
 * characters. Longer is prose, and prose in a label column is worse than the
 * raw name. Never a per-device name table: the catalog author owns the words.
 */
function descLabel(field) {
  if (!field.desc) return '';
  const clause = String(field.desc).split(/[.,;:]/)[0].trim();
  if (!clause || clause.length > 30) return '';
  const words = clause.split(/\s+/);
  if (words.length < 2 || words.length > 3) return '';
  return clause.charAt(0).toUpperCase() + clause.slice(1);
}

/** Decimal places implied by a step. step 0.05 -> 2, step 1 -> 0, absent -> 2. */
export function precisionFor(field) {
  const step = field && field.step;
  if (step == null || !isFinite(step) || step <= 0) {
    // No step published. Integers read better without a false ".00"; floats
    // need some precision or every value looks quantized.
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
 * inventing a name — an unlabeled option is the machine's omission to show,
 * not ours to paper over.
 */
export function optionLabel(field, value) {
  // No value yet is NOT the zeroth option. Rendering String(undefined) put the
  // literal text "undefined" in front of the operator where a setting's state
  // belonged; showing option 0 instead would have been worse, because it would
  // have asserted a machine state nobody reported.
  if (value == null || value === '' || Number.isNaN(Number(value))) return '--';
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

/** Elapsed ms -> h:mm:ss, or h:mm:ss.mmm when `withMs`. */
export function clock(ms, withMs) {
  if (ms == null || !isFinite(ms) || ms < 0) return '--';
  const t = Math.floor(ms / 1000);
  const two = (n) => String(n).padStart(2, '0');
  const base = Math.floor(t / 3600) + ':' + two(Math.floor((t % 3600) / 60)) + ':' + two(t % 60);
  return withMs ? base + '.' + String(Math.floor(ms % 1000)).padStart(3, '0') : base;
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
