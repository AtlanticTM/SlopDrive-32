/**
 * settings-model.test.mjs — gold-standard claims #2, #3 and #6, tested with no
 * device present.
 *
 * The fixture below is A MACHINE THAT DOES NOT EXIST. It uses channel ids this
 * project has never allocated, field names nothing here publishes, a
 * device-defined category (>=128) whose name only that machine knows, and a
 * string setting no SlopDrive has ever shipped.
 *
 * If buildSettingsModel() produces a correct, complete, sensibly-widgeted page
 * for it, then the renderer is genuinely generic — and "a new firmware settings
 * channel needs no client change" stops being a hope and becomes a property
 * with a test behind it.
 *
 * Run: node webui/test/settings-model.test.mjs
 */

import { buildSettingsModel, isFieldEnabled, WIDGET, resolveWidget } from '../src/model/settings.js';
import { claimRoles, withoutClaimed, ROLE } from '../src/model/roles.js';
import {
  PACKED, CHANNEL_CLASS, UI_CATEGORY, UI_RANK, UI_ARCHETYPE,
} from '../../../SlopSync/clients/js/index.js';

let fails = 0;
const ok = (name, cond, extra) => {
  console.log('  [' + (cond ? 'PASS' : 'FAIL') + '] ' + name + (extra ? '  — ' + extra : ''));
  if (!cond) fails++;
};

// ---------------------------------------------------------------------------
// A machine we have never met.
// ---------------------------------------------------------------------------

const lf = (name, type, extra = {}) => ({
  name, type, typeName: String(type), unit: '', scale: 1, ...extra,
});

const CATALOG = [
  // --- telemetry, uncategorized: not a settings tab, but carries roles ------
  {
    id: 0x0210, name: 'carriage', cls: CHANNEL_CLASS.STATE, dir: 0, access: 0,
    maxRateHz: 50, priority: 2, category: null, settingChannel: null,
    layout: [
      lf('carriage_mm', PACKED.u16, { unit: 'mm', scale: 100, role: ROLE.telemetryPosition }),
      lf('carriage_rate', PACKED.i16, { unit: 'mm/s', scale: 10, role: ROLE.telemetryVelocity }),
    ],
    schema: null,
  },

  // --- ui_categories.limits, channel A -------------------------------------
  // `categoryKnown` is what catalog.js sets when the id IS in the registry
  // vocabulary; the fixture mirrors decoder output, so it carries it too.
  {
    id: 0x0211, name: 'travel', cls: CHANNEL_CLASS.STATE, dir: 0, access: 0,
    maxRateHz: 0, priority: 1, category: UI_CATEGORY.limits, categoryKnown: true,
    categoryName: 'limits', settingChannel: 0x0290,
    layout: [
      lf('travel_lo', PACKED.f32, {
        unit: 'mm', min: 0, max: 900, step: 1, settingKey: 1,
        role: ROLE.windowMin, group: 'Travel', desc: 'Lower bound of travel.',
        default: 0,
      }),
      lf('travel_hi', PACKED.f32, {
        unit: 'mm', min: 0, max: 900, step: 1, settingKey: 2,
        role: ROLE.windowMax, group: 'Travel', default: 900,
      }),
      // read-only: no setting_key. Must render as a readout, never an input.
      lf('travel_measured', PACKED.f32, { unit: 'mm', group: 'Travel' }),
      lf('gate', PACKED.bitfield8, {
        role: ROLE.enabledMask,
        bits: ['travel_lo', 'travel_hi', '', '', '', '', '', ''],
      }),
    ],
    schema: null,
  },

  // --- SAME category, DIFFERENT channel ------------------------------------
  // SPEC 8.8: a category spans channels. These must MERGE into one tab.
  {
    id: 0x0212, name: 'ceilings', cls: CHANNEL_CLASS.STATE, dir: 0, access: 0,
    maxRateHz: 0, priority: 1, category: UI_CATEGORY.limits, categoryKnown: true,
    categoryName: 'limits', settingChannel: 0x0290,
    layout: [
      lf('hand_speed', PACKED.f32, {
        unit: 'mm/s', min: 1, max: 400, step: 1, settingKey: 7,
        role: ROLE.limitUserSpeed, group: 'Ceilings', default: 60,
      }),
      // rank=hidden AND a setting: never drawn, but it still CONSUMES mask bit
      // 1, so hand_accel below must land on bit 2. Ordered between two visible
      // settings on purpose — that is the arrangement a naive skip corrupts.
      lf('inert_knob', PACKED.f32, {
        unit: 'mm', min: 0, max: 1, settingKey: 9, group: 'Ceilings',
        rank: UI_RANK.hidden, rankName: 'hidden',
      }),
      lf('hand_accel', PACKED.f32, {
        unit: 'mm/s2', min: 10, max: 9000, step: 10, settingKey: 8,
        role: ROLE.limitUserAccel, group: 'Ceilings', default: 300,
      }),
      // rank=advanced with NO flag bit: the ladder alone must fold it away.
      lf('jerk_trim', PACKED.f32, {
        unit: 'mm/s3', min: 0, max: 100, step: 1, settingKey: 10,
        group: 'Ceilings', rank: UI_RANK.advanced, rankName: 'advanced',
      }),
      lf('gate', PACKED.bitfield8, {
        role: ROLE.enabledMask,
        bits: ['hand_speed', 'inert_knob', 'hand_accel', 'jerk_trim'],
      }),
    ],
    schema: null,
  },

  // --- a DEVICE-DEFINED category (>=128) with a label we cannot know --------
  {
    id: 0x0213, name: 'upkeep', cls: CHANNEL_CLASS.STATE, dir: 0, access: 0,
    maxRateHz: 0, priority: 0, category: 180, categoryLabel: 'Upkeep',
    settingChannel: 0x0290,
    layout: [
      lf('lube_mode', PACKED.u8, {
        settingKey: 20, group: 'Lubrication', default: 1,
        options: ['manual', 'every hour', 'every session'],
        desc: 'How often the machine pumps lubricant.',
      }),
      lf('warm_enable', PACKED.u8, {
        settingKey: 21, group: 'Warmup', default: 0, options: ['off', 'on'],
      }),
      lf('rig_name', PACKED.str16, {
        settingKey: 22, group: 'Identity', role: ROLE.identityName,
        desc: 'Name shown to clients.',
      }),
      lf('gate', PACKED.bitfield8, {
        role: ROLE.enabledMask, bits: ['lube_mode', 'warm_enable', 'rig_name'],
      }),
    ],
    schema: null,
  },

  // --- the INTENT writer, with an action verb ------------------------------
  {
    id: 0x0290, name: 'apply', cls: CHANNEL_CLASS.INTENT, dir: 1, access: 2,
    maxRateHz: 10, priority: 1, category: null, settingChannel: null,
    layout: null,
    schema: [
      { key: 1, name: 'travel_lo', type: 4, typeName: 'f32' },
      { key: 40, name: 'service_op', type: 1, typeName: 'uint',
        role: 'action.service', options: ['none', 'purge', 'recalibrate'],
        desc: 'Maintenance operations.' },
    ],
  },
];

// ---------------------------------------------------------------------------

console.log('settings model vs. a machine that does not exist\n');

const model = buildSettingsModel(CATALOG);

// ---- claim: categories become tabs, and a category SPANS channels ---------
const limits = model.categories.find((c) => c.id === UI_CATEGORY.limits);
ok('a categorized channel becomes a tab', !!limits);
ok('two channels sharing a category MERGE into one tab (SPEC 8.8)',
   limits && limits.groups.length === 2,
   limits ? 'groups: ' + limits.groups.map((g) => g.name).join(', ') : 'no tab');
ok('the merged tab holds fields from BOTH channels',
   limits && new Set(limits.groups.flatMap((g) => g.fields.map((f) => f.channelId))).size === 2);

ok('a known category is labeled from the registry vocabulary',
   limits && limits.label === 'Limits' && limits.name === 'limits',
   limits ? limits.label : '');

// ---- claim: the ui_ranks ladder is honored (RENDERING.md §4) --------------
const ceilings = model.fields.filter((f) => f.channelId === 0x0212);
ok('rank=hidden NEVER renders',
   !ceilings.some((f) => f.name === 'inert_knob') && !model.byRole.has('inert_knob'),
   'drawn: ' + ceilings.map((f) => f.name).join(', '));
ok('...but its mask bit is still consumed: hand_accel keeps bit 2',
   ceilings.find((f) => f.name === 'hand_accel').maskBit === 2,
   'maskBit = ' + ceilings.find((f) => f.name === 'hand_accel').maskBit);
ok('...and the field after it too: jerk_trim keeps bit 3',
   ceilings.find((f) => f.name === 'jerk_trim').maskBit === 3);
ok('rank=advanced folds behind the affordance with no flag bit set',
   ceilings.find((f) => f.name === 'jerk_trim').advanced === true &&
   ceilings.find((f) => f.name === 'jerk_trim').flagBits.advanced === false);
ok('an ordinary field is not advanced',
   ceilings.find((f) => f.name === 'hand_speed').advanced === false);
// The absent-rank default (-> detail) is the DECODER's job, pinned in
// SlopSync's clients/js/test/slopsync-wire.test.mjs. Asserting it here against
// a hand-built fixture would only prove the fixture.

// ---- claim: a device-defined category renders with ITS OWN label ----------
const upkeep = model.categories.find((c) => c.id === 180);
ok('a device-defined category (>=128) still renders', !!upkeep);
ok('and uses the label only that machine knows', upkeep && upkeep.label === 'Upkeep',
   upkeep ? upkeep.label : '');

// ---- claim: widgets come from TYPE + constraints, never from names --------
const byName = new Map(model.fields.map((f) => [f.name, f]));
ok('bounded numeric -> slider', byName.get('travel_lo').widget === WIDGET.slider);
ok('3-option select -> segmented', byName.get('lube_mode').widget === WIDGET.segmented,
   byName.get('lube_mode').widget);
ok('off/on pair -> toggle', byName.get('warm_enable').widget === WIDGET.toggle);
ok('str16 -> text', byName.get('rig_name').widget === WIDGET.text);
ok('no setting_key -> readout, never an input',
   byName.get('travel_measured').widget === WIDGET.readout);
ok('read-only field carries no write target',
   byName.get('travel_measured').writeChannel === null);

// ---- claim: §8.2 rows 9/10 are PINNED, and the pin survives f32 step dust --
// The spec says "range wide enough for a drag gesture" and stops there;
// DRAG_TICKS_MIN is this client's number. These cases are the ones that flip
// if it moves, so they are also the record of what it was chosen to do.
const num = (extra) => resolveWidget(
  { readOnly: false, type: PACKED.f32, scale: 1, flagBits: {}, ...extra });
ok('a 100-tick range drags -> slider', num({ min: 0, max: 100, step: 1 }) === WIDGET.slider);
ok('a 16-tick range does not -> stepper', num({ min: 0, max: 8, step: 0.5 }) === WIDGET.stepper);
ok('exactly DRAG_TICKS_MIN ticks still drags, f32 step dust and all',
   num({ min: 0, max: 1, step: Math.fround(0.05) }) === WIDGET.slider);
ok('an unstepped integer is quantized by its own type: 9 ticks -> stepper',
   num({ type: PACKED.u8, min: 1, max: 10 }) === WIDGET.stepper);
ok('an unstepped float is genuinely continuous -> slider',
   num({ min: 0, max: 20 }) === WIDGET.slider);
ok('an unstepped integer scaled x1000 is finely quantized -> slider',
   num({ type: PACKED.u32, scale: 1000, min: 10, max: 500 }) === WIDGET.slider);
ok('a writable numeric with no bounds cannot be dragged -> stepper',
   num({ type: PACKED.u16 }) === WIDGET.stepper);
ok('an explicit archetype hint beats the whole table (§8.2 row 1)',
   num({ min: 0, max: 100, step: 1, archetypeHint: UI_ARCHETYPE.stepper }) === WIDGET.stepper);

// ---- claim: read-only status bits are lamps, not numerals (§8.2 row 14) ----
const ro = (extra) => resolveWidget({ readOnly: true, scale: 1, flagBits: {}, ...extra });
ok('a read-only bitfield -> indicator',
   ro({ type: PACKED.bitfield8, bits: ['armed', 'homed'] }) === WIDGET.indicator);
ok('a read-only off/on pair -> indicator',
   ro({ type: PACKED.u8, min: 0, max: 1 }) === WIDGET.indicator);
ok('a read-only unbounded numeric is still a plain readout',
   ro({ type: PACKED.f32 }) === WIDGET.readout);

// ---- claim: writes are addressed by the catalog, not by our guesswork -----
ok('a setting knows its INTENT channel and key',
   byName.get('hand_speed').writeChannel === 0x0290 && byName.get('hand_speed').settingKey === 7);

// ---- claim: the enabled_mask gates the right field ------------------------
// gate bit 0 -> travel_lo, bit 1 -> travel_hi. Publish only bit 0 set.
const sample = { travel_lo: 10, travel_hi: 800, travel_measured: 812, gate: 0b01 };
ok('mask bit set -> field writable', isFieldEnabled(byName.get('travel_lo'), sample));
ok('mask bit clear -> field grayed', !isFieldEnabled(byName.get('travel_hi'), sample));
ok('the mask field itself is never drawn as a control',
   !model.fields.some((f) => f.role === ROLE.enabledMask));

// ---- claim: action verbs are discovered by role ---------------------------
ok('an action.* schema field becomes an action', model.actions.length === 1,
   'found ' + model.actions.length);
ok('the action carries its option list', model.actions[0]
   && model.actions[0].options.length === 3);
ok('a non-action schema field is NOT an action',
   !model.actions.some((a) => a.name === 'travel_lo'));

// ---- claim: heroes claim by role, and decline when roles are absent -------
const railClaim = claimRoles(model.byRole, {
  require: { min: ROLE.windowMin, max: ROLE.windowMax },
  optional: { pos: ROLE.telemetryPosition, vel: ROLE.telemetryVelocity },
});
ok('rail hero claims this unknown machine\'s window by ROLE', !!railClaim);
ok('and binds to fields whose names it could not have known',
   railClaim && railClaim.min.name === 'travel_lo' && railClaim.pos.name === 'carriage_mm');

const patternClaim = claimRoles(model.byRole, {
  require: { running: ROLE.patternRunning, select: ROLE.patternSelect },
});
ok('a hero whose required roles are ABSENT declines entirely', patternClaim === null,
   'this machine has no pattern generator, so no generator card is drawn');

// ---- claim: claimed fields are not ALSO drawn generically -----------------
const pruned = withoutClaimed(model.categories, railClaim.claimed);
const stillThere = pruned.flatMap((c) => c.groups.flatMap((g) => g.fields))
  .some((f) => railClaim.claimed.has(f.uid));
ok('fields absorbed by a hero vanish from the generic tree', !stillThere);

// ---- claim: unknown things degrade, never crash --------------------------
const weird = buildSettingsModel([{
  id: 0x0999, name: 'mystery', cls: CHANNEL_CLASS.STATE, dir: 0, access: 0,
  maxRateHz: 0, priority: 0, category: 250, settingChannel: 0x0998,
  layout: [
    lf('unknown_thing', 99, { settingKey: 1, role: 'some.future.role', group: 'Odd' }),
  ],
  schema: null,
}]);
ok('a field of an unknown packed type still renders (fallback widget)',
   weird.fields.length === 1 && weird.fields[0].widget === WIDGET.stepper,
   weird.fields[0] && weird.fields[0].widget);
ok('an unlabeled device category gets a generated label',
   weird.categories[0] && /250/.test(weird.categories[0].label), weird.categories[0].label);
ok('an unknown role is carried, not rejected', weird.fields[0].role === 'some.future.role');

console.log('\n' + (fails ? 'FAILURES: ' + fails : 'ALL PASS — the renderer is machine-agnostic.'));
process.exit(fails ? 1 : 0);
