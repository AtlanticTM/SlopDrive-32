/**
 * roles.js — the protocol's SEMANTIC vocabulary, and the claim mechanism that
 * turns it into bespoke widgets.
 *
 * ── Why this is not "device knowledge" ─────────────────────────────────────
 *
 * The rule for the layer above core/slopsync/ is: know nothing about THIS
 * machine. Channel 0x0081, the field name `window_min`, the option label
 * "Half'n'Half" — all of those describe one device and hardcoding them is what
 * made our UI privileged.
 *
 * A ROLE is the opposite kind of fact. `window.min` is defined in
 * docs/slopsync/registry/registry.yaml and means the same thing on every
 * conforming hub, forever. Binding a rail widget to `window.min` + `window.max`
 * + `telemetry.position` means it draws correctly on a machine built by someone
 * we have never met, provided they annotated their catalog — and if they did
 * not, the fields still render as ordinary sliders and nothing is lost.
 *
 * That is the whole trick behind "don't gimp our UI, just make it as nice on
 * another machine": the nice widgets are not ours, they are the PROTOCOL's, and
 * any client library can ship the same ones.
 *
 * ── Roles are OPPORTUNITIES, never REQUIREMENTS ────────────────────────────
 *
 * Registry doctrine, verbatim: "nothing is hardcoded as a REQUIREMENT; roles
 * are hardcoded as OPPORTUNITIES." A hero widget that cannot find its roles
 * must decline to render and let the generic path handle those fields. An
 * unknown role must never be an error. Both rules are enforced by claimRoles()
 * below rather than left to each widget's good manners.
 *
 * SOURCE OF TRUTH: docs/slopsync/registry/registry.yaml, `field_roles`. This
 * file mirrors it by hand, the same way frames.js mirrors the wire constants.
 * If the two ever disagree, the registry wins.
 */

/** Registry `field_roles` vocabulary. */
export const ROLE = {
  // kinematic limits — CEILINGS, never targets
  limitUserSpeed: 'limit.user.speed',
  limitUserAccel: 'limit.user.accel',
  limitInputSpeed: 'limit.input.speed',
  limitInputAccel: 'limit.input.accel',
  limitInputJerk: 'limit.input.jerk',

  // the stroke window
  windowMin: 'window.min',
  windowMax: 'window.max',

  // RFC-041: how far the machine can actually travel, as opposed to
  // window.min/window.max's OWN catalog min/max annotations (which bound the
  // legal WINDOW SETTING VALUE, not the physical rail). Neither field is a
  // substitute for these — see RFC-041 for why. Both are lengths in the
  // window fields' own unit, measured/configured from the low end of travel.
  geometryMaxTravel: 'geometry.max_travel',
  geometryMeasuredTravel: 'geometry.measured_travel',

  // live telemetry
  telemetryPosition: 'telemetry.position',
  telemetryVelocity: 'telemetry.velocity',
  telemetryCurrent: 'telemetry.current',
  telemetryPowerBus: 'telemetry.power.bus',
  telemetryTemp: 'telemetry.temp',
  telemetryUptime: 'telemetry.uptime',
  // RFC-032: where the machine is currently COMMANDED to, as opposed to
  // telemetryPosition (measured truth). Lag is deliberately not its own
  // role — a hero widget computes target - position client-side.
  telemetryTarget: 'telemetry.target',

  // identity
  identityName: 'identity.name',

  // RFC-032: value-bearing INTENT fields (as opposed to action.* verbs).
  // A schema field carrying this role is a SETPOINT — render a positional
  // control (rail, tape, slider) and write it via sendIntent, never
  // writeSetting (it is not a RFC-009 setting).
  commandPosition: 'command.position',

  // RFC-035: in-flight motion-plan telemetry. No registered role existed
  // when PlanStrip.svelte was first written, so it discovered its channel
  // and fields by name/prose regex; these are the durable fix.
  planStart: 'plan.start',
  planEnd: 'plan.end',
  planCurrent: 'plan.current',
  planVelocity: 'plan.velocity',
  planElapsed: 'plan.elapsed',
  planDuration: 'plan.duration',
  planStyle: 'plan.style',

  // machinery
  enabledMask: 'meta.enabled_mask',
  resetGen: 'meta.reset_gen',

  // built-in pattern generator
  patternRunning: 'pattern.running',
  patternSelect: 'pattern.select',
  patternSpeed: 'pattern.speed',
  patternDepth: 'pattern.depth',
  patternStroke: 'pattern.stroke',
  patternSensation: 'pattern.sensation',
};

/** Open convention (RFC-019): `action.<name>` marks an INTENT field as a verb. */
export const ACTION_PREFIX = 'action.';

/** @param {string} role @returns {boolean} */
export function isActionRole(role) {
  return typeof role === 'string' && role.startsWith(ACTION_PREFIX);
}

/**
 * Open convention: `<role>.peak` is the peak-hold companion of `<role>`.
 * @param {string} role @returns {boolean}
 */
export function isPeakRole(role) {
  return typeof role === 'string' && role.endsWith('.peak');
}

// ---------------------------------------------------------------------------
// Claiming
// ---------------------------------------------------------------------------

/**
 * Attempt to satisfy a hero widget's role requirements against the live model.
 *
 * A claim spec looks like:
 *   { require: { min: ROLE.windowMin, max: ROLE.windowMax },
 *     optional: { pos: ROLE.telemetryPosition } }
 *
 * Returns null if ANY required role is missing — the caller then renders
 * nothing bespoke and the generic path picks the fields up as normal controls.
 * That "return null" is the load-bearing line in this file: it is what stops a
 * hero widget from half-rendering against a machine that does not have what it
 * needs, which is how a UI ends up lying.
 *
 * @param {Map<string, Array>} byRole from buildSettingsModel
 * @param {{require?: Object, optional?: Object}} spec
 * @returns {Object|null} { key: field, ... , claimed: Set<uid> }
 */
export function claimRoles(byRole, spec) {
  const out = { claimed: new Set() };
  const take = (role) => {
    const list = byRole.get(role);
    if (!list || !list.length) return null;
    // Ambiguity is possible in principle (two channels both claiming
    // window.min). First-authored wins, deterministically, rather than
    // guessing which one is "the real" one.
    return list[0];
  };

  for (const [name, role] of Object.entries(spec.require || {})) {
    const f = take(role);
    if (!f) return null;                  // requirement unmet -> decline entirely
    out[name] = f;
    out.claimed.add(f.uid);
  }
  for (const [name, role] of Object.entries(spec.optional || {})) {
    const f = take(role);
    if (f) {
      out[name] = f;
      out.claimed.add(f.uid);
    } else {
      out[name] = null;
    }
  }
  return out;
}

/**
 * Remove claimed fields from the generic settings tree.
 *
 * A field drawn twice — once inside the rail widget and again as a loose
 * slider below it — is two controls fighting over one truth, and the loser
 * shows a stale value. So a hero widget CONSUMES its fields.
 *
 * Groups and categories that end up empty are dropped, which is what makes the
 * page shrink honestly when a hero absorbs a whole card.
 *
 * @param {Array<Object>} categories from buildSettingsModel
 * @param {Set<string>} claimedUids
 * @returns {Array<Object>} a new category array; the input is not mutated
 */
export function withoutClaimed(categories, claimedUids) {
  if (!claimedUids || !claimedUids.size) return categories;
  const out = [];
  for (const cat of categories) {
    const groups = [];
    for (const g of cat.groups) {
      const fields = g.fields.filter((f) => !claimedUids.has(f.uid));
      if (fields.length) groups.push({ ...g, fields });
    }
    if (groups.length) out.push({ ...cat, groups });
  }
  return out;
}
