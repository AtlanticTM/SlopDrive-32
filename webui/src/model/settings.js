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

import { PACKED, SETTING_CATEGORY_NAME } from '../../../../SlopSync/clients/js/index.js';
import { ROLE, isActionRole } from './roles.js';

// ---------------------------------------------------------------------------
// Widget resolution
// ---------------------------------------------------------------------------

/**
 * WIDGET KINDS. Deliberately few. Every one is driven by the field's TYPE plus
 * its RFC-009 constraints — never by its name. If a machine adds a field we
 * have never seen, it still lands on one of these.
 */
export const WIDGET = {
  readout: 'readout',     // no setting_key: effective truth, display only
  toggle: 'toggle',       // 2 options that read as off/on, or a 0..1 integer
  segmented: 'segmented', // small option set, all choices worth showing at once
  select: 'select',       // larger option set
  bitfield: 'bitfield',   // bitfield8 with named bits -> checkbox group
  slider: 'slider',       // numeric with BOTH bounds known
  number: 'number',       // numeric with unknown bounds
  text: 'text',           // str16/32/64
  secret: 'secret',       // flags.secret: write-only, value never on the wire
  action: 'action',       // an INTENT verb (role action.*)
};

const NUMERIC_TYPES = new Set([
  PACKED.u8, PACKED.i8, PACKED.u16, PACKED.i16,
  PACKED.u32, PACKED.i32, PACKED.f32,
]);
const STRING_TYPES = new Set([PACKED.str16, PACKED.str32, PACKED.str64]);

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
 * Choose a widget from type + annotations alone.
 *
 * Order matters: read-only wins over everything (a field with no setting_key
 * must never render as an input, no matter how invitingly typed it is), and
 * `secret` wins over `text` (never render a value the wire deliberately
 * withholds).
 */
export function resolveWidget(f) {
  if (f.readOnly) return WIDGET.readout;
  if (f.flagBits && f.flagBits.secret) return WIDGET.secret;
  if (f.options && f.options.length) {
    if (looksBoolean(f.options)) return WIDGET.toggle;
    return f.options.length <= 4 ? WIDGET.segmented : WIDGET.select;
  }
  if (f.type === PACKED.bitfield8) return f.bits ? WIDGET.bitfield : WIDGET.number;
  if (STRING_TYPES.has(f.type)) return WIDGET.text;
  if (NUMERIC_TYPES.has(f.type)) {
    // An integer bounded to exactly [0,1] is a toggle wearing a number's
    // clothes. Common enough on real machines to be worth the special case.
    if (f.min === 0 && f.max === 1 && f.type !== PACKED.f32) return WIDGET.toggle;
    return (f.min != null && f.max != null) ? WIDGET.slider : WIDGET.number;
  }
  return WIDGET.number;
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

/** Category display name: registry name if known, device label if not. */
function categoryLabel(entry) {
  if (entry.categoryLabel) return entry.categoryLabel;
  const n = SETTING_CATEGORY_NAME[entry.category];
  if (n) return n.charAt(0).toUpperCase() + n.slice(1);
  // A device-defined category (>=128) that shipped no label. Render it rather
  // than dropping its settings on the floor — SPEC 8.8 item 8.
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
    bits: f.bits || null,
    settingKey: readOnly ? null : f.settingKey,
    writeChannel: readOnly ? null : entry.settingChannel,
    // RFC-009 item 3: bit i of the mask gates the i-th SETTING-annotated field
    // of this layout, in layout order. Read-only fields do not consume a bit.
    maskFieldName: (!readOnly && maskField) ? maskField.name : null,
    maskBit: readOnly ? null : settingIndex,
    readOnly,
  };
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
      if (isSetting) settingIndex++;
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
        name: SETTING_CATEGORY_NAME[entry.category] || ('category' + entry.category),
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
