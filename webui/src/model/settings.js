/**
 * settings.js — the catalog -> renderable model transform (RFC-009).
 *
 * THIS FILE IS THE WHOLE POINT OF THE REFACTOR. Everything above
 * the SlopSync protocol client used to know what a SlopDrive-32 is: which channel held the
 * stroke window, which CBOR key wrote the user speed, what the blend-mode
 * options were called. That knowledge is what made our UI privileged and every
 * third-party client second-class, and it is why shipping 20 annotated tuning
 * settings on the wire produced exactly zero UI.
 *
 * So: NOTHING HERE KNOWS ANY DEVICE. No channel id, no field name, no option
 * label appears below. The input is a decoded catalog; the output is a tree of
 * tabs -> cards -> fields with a widget already chosen for each. Point it at a
 * machine that does not exist yet and it produces that machine's settings page.
 *
 * The one thing this file DOES know is the PROTOCOL's own vocabulary — packed
 * type numbers, the `role` strings from the registry, the `setting_key`
 * presence rule. That is not device knowledge: a role like `window.min` is
 * defined by the registry and means the same thing on every conforming hub,
 * whereas channel 0x1000 means something only here. Binding to the former is
 * portable; binding to the latter is the disease. See roles.js.
 *
 * Pure and synchronous — no session, no DOM, no reactivity. That makes it
 * testable against a fixture catalog with no device present, which is exactly
 * how the "renders a machine it has never met" claim gets checked in CI.
 */

import {
  PACKED, UI_RANK, UI_ARCHETYPE, UI_ARCHETYPE_NAME, VALUE_ASPECT,
} from '../../../../SlopSync/clients/js/index.js';
import { ROLE, isActionRole } from './roles.js';

// ---------------------------------------------------------------------------
// Archetype derivation (RENDERING.md §8.2) and its widget projection
// ---------------------------------------------------------------------------
//
// An ARCHETYPE is the interaction contract — normative, spec-derived, the same
// on every conforming client. A WIDGET is this client's pixels for it. Keeping
// them separate is what lets a `select` draw as a segmented row here and as a
// scroll wheel on a glance-class screen while both stay conformant (§8).

/**
 * WIDGET KINDS — this renderer's projections. Most are an archetype 1:1; the
 * three that are not (`segmented`, `bitfield`, `secret`) are pixel-level
 * variants chosen from facts §8.2 deliberately does not rank: how many options
 * fit on one row, whether the bits are named, whether the value is withheld.
 */
export const WIDGET = {
  readout: 'readout',     // §8.2 rows 12/13: no setting_key, display only
  indicator: 'indicator', // §8.2 row 14: read-only boolean/bitfield status
  toggle: 'toggle',       // row 7
  segmented: 'segmented', // row 8, small option set: all choices worth showing
  select: 'select',       // row 8, larger option set
  bitfield: 'bitfield',   // row 7 composed: named bits -> a toggle per bit
  slider: 'slider',       // row 9
  stepper: 'stepper',     // row 10
  text: 'text',           // row 11
  secret: 'secret',       // row 11 + flags.secret: value never on the wire
  action: 'action',       // row 6 (`trigger`), discovered by role in pass 2
};

const NUMERIC_TYPES = new Set([
  PACKED.u8, PACKED.i8, PACKED.u16, PACKED.i16,
  PACKED.u32, PACKED.i32, PACKED.f32,
]);
const STRING_TYPES = new Set([PACKED.str16, PACKED.str32, PACKED.str64]);

/**
 * §8.2 rows 9/10 say a bounded numeric is a `slider` when its range is "wide
 * enough for a drag gesture" and a `stepper` otherwise, without pinning a
 * number. THIS IS THAT PIN: at least 20 distinct positions to drag through.
 *
 * The number is a floor on drag RESOLUTION, not on the span — 0..1 by 0.05 is
 * a fine slider, 1..10 by 1 is not, and both are bounded. Raising it much
 * turns whole cards of 25-tick modulation controls into spinners; lowering it
 * lets a 9-position control pretend a drag can hit its middle value.
 */
export const DRAG_TICKS_MIN = 20;

/**
 * How many distinct values the range holds, or null when it has no bounds.
 *
 * An unannotated `step` is not "no quantization": an integer wire type is
 * quantized by its own representation, at 1/scale of a display unit (a u8
 * counting 1..10 has 9 ticks and belongs on a stepper even though it declares
 * no step). Only a float with no step is genuinely continuous — Infinity, so
 * it always reads as drag-worthy.
 */
function ticksOf(f) {
  if (f.min == null || f.max == null) return null;
  const quantum = f.step || (f.type === PACKED.f32 ? 0 : 1 / (f.scale || 1));
  if (!quantum) return Infinity;
  return (f.max - f.min) / quantum;
}

/**
 * Does a 2-option set read as a boolean? Purely a PRESENTATION upgrade — the
 * wire value stays the option index either way, so guessing wrong costs a
 * nicer-looking control, never a wrong value.
 */
const BOOLEAN_PAIRS = [
  ['off', 'on'], ['disabled', 'enabled'], ['no', 'yes'], ['false', 'true'],
];
function looksBoolean(options) {
  if (!options || options.length !== 2) return false;
  const lo = options.map((o) => String(o).trim().toLowerCase());
  return BOOLEAN_PAIRS.some((p) => p[0] === lo[0] && p[1] === lo[1]);
}

/**
 * There is no `bool` packed type, so a boolean arrives wearing one of two
 * disguises: an off/on option pair, or an integer bounded to exactly [0,1].
 * Both are what §8.2 rows 7 and 14 mean by "bool field".
 */
function looksBooleanField(f) {
  if (f.options && f.options.length) return looksBoolean(f.options);
  return f.min === 0 && f.max === 1 && f.type !== PACKED.f32 && NUMERIC_TYPES.has(f.type);
}

/**
 * RENDERING.md §8.2's decision table, evaluated top to bottom, first match
 * wins. Returns a `UI_ARCHETYPE` code.
 *
 * Rows 2-5 and 16-17 are absent BY DESIGN, not by omission: `stop` is bound to
 * safety-op identity (row 2) and lives in the safety UI; `axis`, `pad2d` and
 * `list` are claimed by hero widgets from roles before a field reaches here
 * (roles.js); `color`/`datetime` need an explicit hint no catalog key carries
 * yet. Row 6 is the action pass at the bottom of this file.
 */
export function resolveArchetype(f) {
  // Row 1. No catalog key carries an archetype override today, so this is
  // dormant — one line to honor the day a hub ships one, per §14d's rule that
  // an unrecognized annotation must never break a client that ignores it.
  if (f.archetypeHint != null && UI_ARCHETYPE_NAME[f.archetypeHint] != null) return f.archetypeHint;

  // Read-only wins over everything below it: a field with no setting_key is
  // effective truth and must never render as something you can push, no matter
  // how invitingly typed it is.
  if (f.readOnly) {
    if (f.aspect === VALUE_ASPECT.rate) return UI_ARCHETYPE.chart;         // row 15
    if (looksBooleanField(f) || f.type === PACKED.bitfield8) return UI_ARCHETYPE.indicator;  // row 14
    return UI_ARCHETYPE.readout;                                           // rows 12/13
  }

  if (looksBooleanField(f)) return UI_ARCHETYPE.toggle;                    // row 7
  if (f.options && f.options.length) return UI_ARCHETYPE.select;           // row 8
  // A writable named-bit bitfield8 is a SET of booleans — row 7 composed the
  // way §8.4 composes pad2d out of sliders. §8.2 has no row of its own for it;
  // if one ever lands, this line is where it goes.
  if (f.type === PACKED.bitfield8) return UI_ARCHETYPE.toggle;
  if (STRING_TYPES.has(f.type)) return UI_ARCHETYPE.text;                  // row 11

  // Rows 9/10, plus the case neither row names: a writable numeric with no
  // bounds at all. It cannot be dragged against a range it never published, so
  // it lands on the stepper with the rest of the type-in controls.
  const ticks = ticksOf(f);
  // `step` rides the wire as an f32, so an exactly-N-tick range computes a hair
  // under N (0..1 by 0.05 gives 19.9999997). Compare with float slack or every
  // round-decimal step lands on the wrong side of the pin.
  return (ticks != null && ticks >= DRAG_TICKS_MIN * (1 - 1e-6))
    ? UI_ARCHETYPE.slider
    : UI_ARCHETYPE.stepper;
}

/**
 * Project a derived archetype onto this renderer's controls.
 *
 * `chart` has no generic control — the only time-series drawing we own is a
 * hero widget with a subscription behind it, and a lone catalog field has
 * neither. §14d's fallback rule and §8.4's "glance degrades to sparkline/value"
 * both make the plain numeral a conformant answer, so it takes one.
 */
export function resolveWidget(f) {
  const a = f.archetype != null ? f.archetype : resolveArchetype(f);
  switch (a) {
    case UI_ARCHETYPE.indicator:
      return WIDGET.indicator;
    case UI_ARCHETYPE.toggle:
      return (f.type === PACKED.bitfield8 && f.bits) ? WIDGET.bitfield : WIDGET.toggle;
    case UI_ARCHETYPE.select:
      return f.options.length <= 4 ? WIDGET.segmented : WIDGET.select;
    case UI_ARCHETYPE.slider:
      return WIDGET.slider;
    case UI_ARCHETYPE.stepper:
      return WIDGET.stepper;
    case UI_ARCHETYPE.text:
      // `secret` beats `text`: never render a value the wire deliberately
      // withholds (RFC-009.4).
      return (f.flagBits && f.flagBits.secret) ? WIDGET.secret : WIDGET.text;
    case UI_ARCHETYPE.trigger:
      return WIDGET.action;
    default:
      return WIDGET.readout;
  }
}

// ---------------------------------------------------------------------------
// Labels
// ---------------------------------------------------------------------------

/**
 * Humanize a wire field name for display. The catalog gives us machine names
 * (`window_min`, `chase_dense_ms`); `desc` carries the prose. We are NOT
 * translating known names to pretty ones — that would be a device-knowledge
 * table by another name, and it would leave an unknown machine's fields
 * looking second-class next to ours. Same treatment for everybody.
 */
export function humanize(name) {
  if (!name) return '';
  const s = String(name)
    .replace(/_/g, ' ')
    .replace(/\bovr\b/g, 'override')
    .replace(/\bcfg\b/g, 'config')
    .replace(/\bff\b/g, 'feedforward')
    .trim();
  return s.charAt(0).toUpperCase() + s.slice(1);
}

/**
 * Category display name: device label if it shipped one, else the registry name
 * catalog.js already resolved.
 *
 * The resolution (including unknown -> `other`) lives in catalog.js, not here —
 * this only decides how to WRITE it. An unrecognized id with no label falls
 * back to its number rather than "Other", because the raw id is what actually
 * distinguishes two untaught categories from each other; either way the tab
 * renders and its settings are never dropped on the floor (SPEC 8.8 item 8).
 */
function categoryLabel(entry) {
  if (entry.categoryLabel) return entry.categoryLabel;
  if (entry.categoryKnown && entry.categoryName) {
    return entry.categoryName.charAt(0).toUpperCase() + entry.categoryName.slice(1);
  }
  return 'Category ' + entry.category;
}

// ---------------------------------------------------------------------------
// Field construction
// ---------------------------------------------------------------------------

/**
 * Find the enabled_mask field of a layout by ROLE, not by name.
 *
 * `meta.enabled_mask` is registry vocabulary, so this works on a machine that
 * calls its mask something else entirely. If a hub ships a mask without the
 * role we simply do not gate — graying nothing is a safe failure; graying the
 * WRONG control because we pattern-matched a name would not be.
 */
function findMaskField(layout) {
  if (!layout) return null;
  return layout.find((f) => f.role === ROLE.enabledMask) || null;
}

function makeField(entry, f, settingIndex, maskField) {
  const readOnly = f.settingKey == null || entry.settingChannel == null;
  const out = {
    uid: entry.id + ':' + f.name,
    channelId: entry.id,
    channelName: entry.name,
    name: f.name,
    label: humanize(f.name),
    type: f.type,
    typeName: f.typeName,
    unit: f.unit || '',
    scale: f.scale || 1,
    min: f.min,
    max: f.max,
    step: f.step,
    dflt: f.default,
    options: f.options || null,
    desc: f.desc || '',
    group: f.group || '',
    role: f.role || '',
    flags: f.flags || 0,
    flagBits: f.flagBits || { advanced: false, restart_required: false, secret: false },
    rank: f.rank,
    rankName: f.rankName,
    aspect: f.aspect,
    // §8.2 row 1's override. Nothing sets it yet — no catalog key carries an
    // archetype — but the derivation honors it the moment one does.
    archetypeHint: f.archetype,
    // ONE truth for the disclosure affordance. RENDERING.md §4 calls ui_ranks
    // `advanced` the migration of the setting_flags.advanced BIT into the rank
    // ladder, so both spellings mean the same thing and a machine may ship
    // either. Deciding it once here keeps every consumer from re-ORing it.
    advanced: f.rank === UI_RANK.advanced || !!(f.flagBits && f.flagBits.advanced),
    bits: f.bits || null,
    settingKey: readOnly ? null : f.settingKey,
    writeChannel: readOnly ? null : entry.settingChannel,
    // RFC-009 item 3: bit i of the mask gates the i-th SETTING-annotated field
    // of this layout, in layout order. Read-only fields do not consume a bit.
    maskFieldName: (!readOnly && maskField) ? maskField.name : null,
    maskBit: readOnly ? null : settingIndex,
    readOnly,
  };
  out.archetype = resolveArchetype(out);
  out.widget = resolveWidget(out);
  return out;
}

// ---------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------

/**
 * Build the full renderable settings model from a decoded catalog.
 *
 * @param {Array<Object>} entries decoded catalog entries
 * @returns {{
 *   categories: Array<Object>,   // tabs, each with groups -> fields
 *   actions: Array<Object>,      // INTENT verbs discovered by role
 *   byRole: Map<string, Array>,  // role -> fields, for hero widgets to claim
 *   fields: Array<Object>,       // flat list of every field, settings + readouts
 * }}
 */
export function buildSettingsModel(entries) {
  const fields = [];
  const byRole = new Map();
  const actions = [];

  const addRole = (role, item) => {
    if (!role) return;
    if (!byRole.has(role)) byRole.set(role, []);
    byRole.get(role).push(item);
  };

  // ---- pass 1: every layout field of every CATEGORIZED channel -----------
  //
  // Categorization is the device's own statement that a channel belongs on the
  // settings surface. An uncategorized channel is protocol plumbing (safety,
  // control-owner, the session/trust channels) and is rendered by purpose-built
  // UI, not by the generic settings renderer.
  for (const entry of entries) {
    if (!entry.layout) continue;
    const maskField = findMaskField(entry.layout);
    let settingIndex = 0;
    for (const f of entry.layout) {
      // The mask itself is machinery, not a setting. It gates other fields; it
      // is never drawn.
      if (maskField && f === maskField) continue;
      const isSetting = f.settingKey != null && entry.settingChannel != null;
      const field = makeField(entry, f, isSetting ? settingIndex : null, maskField);
      // COUNT BEFORE SKIPPING. The enabled_mask's bit i gates the i-th
      // SETTING-annotated field in layout order (RFC-009 item 3) — a rank the
      // client chose not to draw does not remove the field from the hub's own
      // numbering. Skipping before this increment shifts every later field's
      // bit by one and grays the WRONG controls, silently: the reference device
      // ships exactly this shape (a rank=hidden field holding settingKey 4).
      if (isSetting) settingIndex++;
      // rank=hidden: carried on the wire for compatibility, NEVER rendered
      // (RENDERING.md §4). Dropped before it reaches `fields` OR `byRole`, so a
      // hero widget cannot resurrect it by claiming its role either. This is
      // the machine's own statement that a released-but-inert field should stop
      // showing up — do not add an "unhide" affordance, that defeats the point.
      if (f.rank === UI_RANK.hidden) continue;
      // Roles are indexed for EVERY field, categorized or not — a hero widget
      // wants `telemetry.position` from the motion channel, which carries no
      // category because it is not a setting.
      addRole(field.role, field);
      fields.push(field);
    }
  }

  // ---- pass 2: INTENT schema fields that are ACTIONS ----------------------
  //
  // RFC-019: `action.<name>` is an open role convention. A schema field tagged
  // with one is a verb — home, clear fault, save — and renders as a button
  // rather than a value editor. Without this, actions are undiscoverable and
  // every client hardcodes them, which is precisely what we are killing.
  for (const entry of entries) {
    if (!entry.schema) continue;
    for (const f of entry.schema) {
      if (f.rank === UI_RANK.hidden) continue;   // same law as pass 1
      // Index EVERY roled schema field, not just the verbs.
      //
      // An INTENT field can carry a role that names a VALUE rather than an
      // action — the target position of a move, say. Those are not buttons and
      // must not become actions, but a widget still has to be able to FIND
      // them: without this, the rail's tap-to-move had no generic way to reach
      // the move channel and correctly refused to guess one, leaving the
      // instrument read-only on every machine.
      if (f.role && !isActionRole(f.role)) {
        addRole(f.role, {
          uid: entry.id + ':' + f.key,
          channelId: entry.id,
          channelName: entry.name,
          key: f.key,
          name: f.name,
          label: humanize(f.name),
          desc: f.desc || '',
          role: f.role,
          unit: f.unit || '',
          min: f.min,
          max: f.max,
          access: f.access != null ? f.access : entry.access,
          isIntentField: true,      // write with sendIntent, NOT writeSetting
        });
        continue;
      }
      if (!isActionRole(f.role)) continue;
      const act = {
        uid: entry.id + ':' + f.key,
        channelId: entry.id,
        channelName: entry.name,
        key: f.key,
        name: f.name,
        label: humanize(f.name),
        desc: f.desc || '',
        role: f.role,
        options: f.options || null,
        optionAccess: f.optionAccess || null,
        access: f.access != null ? f.access : entry.access,
        group: f.group || '',
        archetype: UI_ARCHETYPE.trigger,   // §8.2 row 6
        widget: WIDGET.action,
      };
      addRole(act.role, act);
      actions.push(act);
    }
  }

  // ---- pass 3: group into tabs and cards ----------------------------------
  //
  // SPEC 8.8: a CATEGORY SPANS CHANNELS. Two channels sharing a category merge
  // into one tab. That is what lets 20 tuning knobs live across three channels
  // (each capped at 8 settings by its bitfield8 mask) and still present as a
  // single Tuning tab. Keying the map on the category NUMBER is what makes the
  // merge happen; keying it on the channel would draw three unrelated tabs.
  const catMap = new Map();
  for (const field of fields) {
    const entry = entries.find((e) => e.id === field.channelId);
    if (entry.category == null) continue;   // uncategorized: not a settings tab
    const key = entry.category;
    if (!catMap.has(key)) {
      catMap.set(key, {
        key,
        id: entry.category,
        name: entry.categoryKnown ? entry.categoryName : ('category' + entry.category),
        label: categoryLabel(entry),
        groups: new Map(),
        // A category is writable if ANY of its channels names a settingChannel.
        // A purely read-only category (diagnostics) still gets a tab — telemetry
        // is worth showing — it just contains no inputs.
        writable: false,
      });
    }
    const cat = catMap.get(key);
    if (!field.readOnly) cat.writable = true;
    const gname = field.group || '';
    if (!cat.groups.has(gname)) cat.groups.set(gname, { name: gname, fields: [] });
    cat.groups.get(gname).fields.push(field);
  }

  // Presentation order is AUTHORING order — the device chose it deliberately
  // (SPEC 8.9) and re-sorting alphabetically would scramble a curated page.
  const categories = [...catMap.values()].map((c) => ({
    ...c,
    groups: [...c.groups.values()],
  }));

  return { categories, actions, byRole, fields };
}

// ---------------------------------------------------------------------------
// Live-state helpers
// ---------------------------------------------------------------------------

/**
 * Is this field currently writable, per the device's own live enabled_mask?
 *
 * Ground truth, not a guess: the mask arrives in the same retained STATE
 * snapshot as the values, so the client grays from exactly what the hub would
 * refuse. RFC-009 item 3.
 *
 * @param {Object} field from buildSettingsModel
 * @param {Object} sample the decoded STATE sample for field.channelId
 * @returns {boolean}
 */
export function isFieldEnabled(field, sample) {
  if (field.readOnly) return false;
  if (!field.maskFieldName || field.maskBit == null) return true;
  if (!sample) return false;            // no snapshot yet: not yet writable
  const mask = sample[field.maskFieldName];
  if (typeof mask !== 'number') return true;   // machine did not publish it
  return (mask & (1 << field.maskBit)) !== 0;
}

/**
 * The device's reported value for a field, straight from its channel's
 * retained STATE. This is the ONLY source a control may display — never a
 * locally remembered request. CLAUDE.md 3, Ground Truth Doctrine.
 */
export function reportedValue(field, sample) {
  if (!sample) return undefined;
  return sample[field.name];
}
