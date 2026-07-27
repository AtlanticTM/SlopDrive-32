/**
 * heroes.js — the hero-widget registry.
 *
 * A "hero" is a bespoke control that renders a GROUP of fields better than the
 * generic widgets can: a rail with a draggable stroke window instead of two
 * loose min/max sliders, a generator card instead of six unrelated percentages.
 *
 * The rule that keeps this from re-privileging our UI: a hero binds to ROLES,
 * never to channel ids or field names. `window.min` + `window.max` is a
 * registry-defined concept, so the rail widget draws correctly on anyone's
 * machine that annotates those roles — and any other client library can ship
 * the identical widget by reading the identical roles. The nice controls belong
 * to the protocol, not to us.
 *
 * If a machine does not publish the roles a hero needs, claimRoles() returns
 * null, the hero silently declines, and those fields fall through to the
 * generic renderer. Nothing breaks; the page just gets plainer. That is the
 * "opportunities, not requirements" doctrine as executable code.
 */

import { ROLE, claimRoles } from '../model/roles.js';
import RailWidget from './hero/RailWidget.svelte';
import PatternWidget from './hero/PatternWidget.svelte';
import LimitsWidget from './hero/LimitsWidget.svelte';

/**
 * Registered heroes, in render order.
 *
 * `require` roles must ALL resolve or the hero declines entirely — a rail that
 * knows its window but not its position would draw a carriage that is always
 * at zero, which is worse than no rail at all.
 */
const HEROES = [
  {
    id: 'rail',
    component: RailWidget,
    spec: {
      require: { min: ROLE.windowMin, max: ROLE.windowMax },
      // `move` (RFC-032 command.position) and `target` (telemetry.target) are
      // both optional: a machine with only the window roles still gets a
      // correct, read-only window editor. Only when BOTH are present does
      // the input tape become a live command surface (see RailWidget).
      optional: {
        pos: ROLE.telemetryPosition, vel: ROLE.telemetryVelocity,
        move: ROLE.commandPosition, target: ROLE.telemetryTarget,
      },
    },
  },
  {
    id: 'pattern',
    component: PatternWidget,
    spec: {
      require: { running: ROLE.patternRunning, select: ROLE.patternSelect },
      optional: {
        speed: ROLE.patternSpeed,
        depth: ROLE.patternDepth,
        stroke: ROLE.patternStroke,
        sensation: ROLE.patternSensation,
      },
    },
  },
  {
    id: 'limits',
    component: LimitsWidget,
    spec: {
      // A machine with only a user limit set still gets the widget; the input
      // set is optional because not every machine HAS machine-driven motion.
      require: { userSpeed: ROLE.limitUserSpeed, userAccel: ROLE.limitUserAccel },
      optional: {
        inputSpeed: ROLE.limitInputSpeed,
        inputAccel: ROLE.limitInputAccel,
        inputJerk: ROLE.limitInputJerk,
      },
    },
  },
];

/**
 * Resolve every hero against the live role index.
 *
 * @param {Map<string, Array>} byRole from buildSettingsModel
 * @returns {{widgets: Array<{id, component, fields}>, claimed: Set<string>}}
 */
export function heroClaims(byRole) {
  const widgets = [];
  const claimed = new Set();
  if (!byRole) return { widgets, claimed };
  for (const h of HEROES) {
    const fields = claimRoles(byRole, h.spec);
    if (!fields) continue;             // machine lacks the roles: decline
    widgets.push({ id: h.id, component: h.component, fields });
    for (const uid of fields.claimed) claimed.add(uid);
  }
  return { widgets, claimed };
}
