---
title: Catalog vocabulary
description: Generated tables of packed field types, field roles, setting categories, setting flags and procedure phases.
register: IEEE
generated: true
---

<!-- ==========================================================
     GENERATED FILE. DO NOT EDIT.
     Source of truth: docs/slopsync/registry/registry.yaml
     Generator:       docs-site/tools/gen_docs_tables.py
     Regenerate:      python docs-site/tools/gen_docs_tables.py
     CI gate:         python docs-site/tools/gen_docs_tables.py --check
     Hand edits are overwritten and fail the docs build.
     ========================================================== -->

# Catalog vocabulary

The catalog describes what a hub's channels **are**. These are the
registered words it uses to do that.

## Packed field types

A packed layout has static field offsets. Every type below is
fixed-width, which is what makes append-only evolution safe.

| Value | Type | Notes |
|---|---|---|
| `0` | `u8` |  |
| `1` | `i8` |  |
| `2` | `u16` |  |
| `3` | `i16` |  |
| `4` | `u32` |  |
| `5` | `i32` |  |
| `6` | `f32` |  |
| `7` | `bitfield8` | bit meanings enumerated in catalog entry |
| `8` | `str16` | 16 bytes, zero-padded UTF-8 (RFC-026). The default string width — session-roster names, device names, secret settings. |
| `9` | `str32` | 32 bytes, zero-padded UTF-8 (RFC-026) |
| `10` | `str64` | 64 bytes, zero-padded UTF-8 (RFC-026). 26% of a 242 B snapshot — use deliberately. |

STREAM sample layouts stay string-free. The motion path never pays for
text.

## Field roles

A role is the semantic tag on a catalog field. It is a text string, not
a number, because action roles carry a device-chosen suffix.

Roles are **opportunities, never requirements**. A client that
recognizes a role may render a bespoke widget. A client that does not
must render it generically instead, by type and constraints. An unknown
role is never an error.

| Role | Meaning |
|---|---|
| `limit.user.speed` | speed ceiling of the USER (manual) limit set. CEILING, never a target. |
| `limit.user.accel` | accel ceiling of the user limit set |
| `limit.input.speed` | speed ceiling of the INPUT (machine-driven: patterns, streams, TCode) limit set |
| `limit.input.accel` | accel ceiling of the input limit set |
| `limit.input.jerk` | jerk ceiling of the input limit set |
| `geometry.max_travel` | the configured travel ceiling — how far the machine's rail geometry allows it to search/move (0x0081 max_rail is the worked example: also the sensorless-homing search sweep bound). Distinct from window.min/max, which is the operator-chosen SUB-range within this travel. |
| `geometry.measured_travel` | the usable travel a real home actually measured between the two hard stops, as opposed to geometry.max_travel's configured ceiling. Zero/absent-of-meaning until the first successful home this session; a client MUST NOT treat zero as a real measurement. |
| `window.min` | stroke window lower bound. Limits normalized against the window are window-relative and therefore MOVE when it does — which is exactly why this is a STATE field and not a one-shot WELCOME value. |
| `window.max` | stroke window upper bound |
| `telemetry.position` | live actuator position |
| `telemetry.target` | RFC-032: the position the machine is currently COMMANDED to, as opposed to telemetry.position which is where it measurably is. Lag is deliberately NOT a role: it is target - position, computed client-side — registering a third field for a subtraction would invite two sources of truth for one number. |
| `telemetry.velocity` | live actuator velocity |
| `telemetry.current` | motor/drive current |
| `telemetry.power.bus` | DC bus voltage or power |
| `telemetry.temp` | a temperature reading; the field's own name/unit says which |
| `telemetry.uptime` | hub uptime |
| `identity.name` | the writable machine-name setting (RFC-026 tier 2, str16/str32). Its READ-ONLY twin is WELCOME identity.hub_name. |
| `meta.enabled_mask` | RFC-009.4: a bitfield8 field whose bit i gates the i-th setting-annotated field of the SAME layout. On-change, retained, conflated — every client grays from one ground truth. Disabled means GRAY, never hide. |
| `meta.reset_gen` | RFC-019: increments on every applied reset in this counter group, so ALL subscribers observe the reset, not just the sender who asked for it. |
| `pattern.running` | whether the built-in pattern generator is currently driving the machine |
| `pattern.select` | which built-in pattern the generator plays; options are the device's pattern names, index-aligned with the wire value |
| `pattern.speed` | pattern generator speed knob, as a percentage of its own range |
| `pattern.depth` | pattern generator depth knob: how far into the stroke window it reaches |
| `pattern.stroke` | pattern generator stroke-length knob, as a percentage of the available depth |
| `pattern.sensation` | pattern generator character knob; what it changes depends on the selected pattern |
| `command.position` | RFC-032: INTENT field carrying a commanded ABSOLUTE target position in the channel's own unit. A client that finds it MAY render a positional control (rail, tape, slider) and send the value on that field's channel. |
| `plan.start` | normalized start position of the segment in flight |
| `plan.end` | normalized end position of the segment in flight |
| `plan.current` | normalized current position along the plan |
| `plan.velocity` | current planned velocity |
| `plan.elapsed` | elapsed time within the segment in flight |
| `plan.duration` | total duration of the segment in flight |
| `plan.style` | which planning style produced the segment; options are the device's style names, index-aligned with the wire value |
| `source.background_run` | bool, `setting_key`-annotated: whether THIS autonomous source keeps running when its owning session ends. false (DEFAULT) = the source stops when its controlling session ends. true = the source deliberately continues in the background, reachable only by the role-exempt stop/estop ops (§11.2) from any session. Applies to any hub-autonomous source, never to a command-driven one. |

Two conventions extend the list without registering entries:

- `<role>.peak` is the peak companion of any telemetry role.
- `action.<name>` marks an INTENT field as a verb, not a value.

## Setting flags

| Mask | Bit | Name | Notes |
|---|---|---|---|
| `0x01` | `bit 0` | `advanced` | hide behind an 'advanced' affordance by default; NEVER remove from the surface |
| `0x02` | `bit 1` | `restart_required` | the applied value takes effect on the next boot (distinct from RFC-020's reboot_in_ms, which is the hub rebooting ITSELF to commit) |
| `0x04` | `bit 2` | `secret` | NORMATIVE (RFC-009.5): the value NEVER appears in STATE. The snapshot carries only a set/unset presence bit. Writes ride the paired INTENT normally and ECHO confirms application WITHOUT echoing the value. A WiFi password must never ride a retained snapshot that open-access `watch` sessions receive. |

## Procedure phases

Only the lifecycle phases are registered. Any generic client can render
these without knowing the procedure. Values 128 to 255 are device-defined
intermediate steps. A client that does not recognize one renders it as
`running`.

| Value | Phase | Notes |
|---|---|---|
| `0` | `idle` | not running; the reconnect-safe resting value |
| `1` | `running` | started and in progress; `progress` 0–100 is advisory |
| `2` | `succeeded` | terminal, ok. Also EVENTed (RFC-020). |
| `3` | `failed` | terminal, error — `result` u16 carries a nack_codes value or a device code |
| `4` | `aborted` | terminal, canceled or superseded |

## Curve families

The `curve_family` sub-key is CBOR key 45, inside a `publishes` or `granted_publishes` entry. It names which smoothness class a segment stream's sender means. The wish rides on HELLO or PUBLISH. The grant echoes the effective family, so a client can tell honored from downgraded.

| Value | Family | Notes |
|---|---|---|
| `0` | `unspecified` | the compatible default — the hub behaves exactly as it did before RFC-030. What every pre-RFC-030 client is. |
| `1` | `c1_cubic` | velocity-continuous cubic (Linear/Pchip/Makima/monotone-cubic senders). Acceleration lawfully STEPS at knots; a follow-client hub reconstructs C1 and does NOT smooth the corner the author put there. |
| `2` | `c2_quintic` | curvature-continuous; the sender means the smoothness. A follow-client hub may use its C2 reconstruction (backward-difference af estimation is valid here — the quantity exists). |
| `3` | `step` | held value with instantaneous transitions (step/none interpolation). The family says intent, the machine owns feasibility as always. RFC-049a: NUMBER KEPT, never renumbered, but status is `reserved` — the reference engine has no step renderer, so a `step` declaration renders as `c2_quintic` and the GRANT echo reports exactly that effective family (§9.6, §18-20). Declarable again when a step renderer exists in the reference engine; only the delegate's mapping changes when it does. |

