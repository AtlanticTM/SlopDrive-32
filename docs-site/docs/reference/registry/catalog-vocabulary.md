---
title: Catalog vocabulary
description: Generated tables of packed field types, field roles, setting categories, setting flags and procedure phases.
register: IEEE
generated: true
---

<!-- ==========================================================
     GENERATED FILE — DO NOT EDIT.
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
recognises a role may render a bespoke widget. A client that does not
must fall back to generic rendering by type and constraints. An unknown
role is never an error.

| Role | Meaning |
|---|---|
| `limit.user.speed` | speed ceiling of the USER (manual) limit set. CEILING, never a target. |
| `limit.user.accel` | accel ceiling of the user limit set |
| `limit.input.speed` | speed ceiling of the INPUT (machine-driven: patterns, streams, TCode) limit set |
| `limit.input.accel` | accel ceiling of the input limit set |
| `limit.input.jerk` | jerk ceiling of the input limit set |
| `window.min` | stroke window lower bound. Limits normalized against the window are window-relative and therefore MOVE when it does — which is exactly why this is a STATE field and not a one-shot WELCOME value. |
| `window.max` | stroke window upper bound |
| `telemetry.position` | live actuator position |
| `telemetry.velocity` | live actuator velocity |
| `telemetry.current` | motor/drive current |
| `telemetry.power.bus` | DC bus voltage or power |
| `telemetry.temp` | a temperature reading; the field's own name/unit says which |
| `telemetry.uptime` | hub uptime |
| `identity.name` | the writable machine-name setting (RFC-026 tier 2, str16/str32). Its READ-ONLY twin is WELCOME identity.hub_name. |
| `meta.enabled_mask` | RFC-009.4: a bitfield8 field whose bit i gates the i-th setting-annotated field of the SAME layout. On-change, retained, conflated — every client greys from one ground truth. Disabled means GREY, never hide. |
| `meta.reset_gen` | RFC-019: increments on every applied reset in this counter group, so ALL subscribers observe the reset, not just the sender who asked for it. |

Two conventions extend the list without registering entries:

- `<role>.peak` is the peak companion of any telemetry role.
- `action.<name>` marks an INTENT field as a verb, not a value.

## Setting categories

Values 0–127 are registered here and have a canonical order. Values
128–255 are device-defined and the hub supplies the label.

A category spans channels: `user` and `user-2` merge into one tab.

| Value | Category | Notes |
|---|---|---|
| `0` | `device` | identity, network, storage, firmware — what the machine IS |
| `1` | `user` | everyday operating preferences |
| `2` | `limits` | safety envelope: windows, ceilings, e-stop behaviour |
| `3` | `tuning` | motion/planner internals; typically `advanced`-flagged |
| `4` | `diagnostics` | counters, telemetry, resets — mostly read-only fields |

## Setting flags

| Mask | Bit | Name | Notes |
|---|---|---|---|
| `0x01` | `bit 0` | `advanced` | hide behind an 'advanced' affordance by default; NEVER remove from the surface |
| `0x02` | `bit 1` | `restart_required` | the applied value takes effect on the next boot (distinct from RFC-020's reboot_in_ms, which is the hub rebooting ITSELF to commit) |
| `0x04` | `bit 2` | `secret` | NORMATIVE (RFC-009.5): the value NEVER appears in STATE. The snapshot carries only a set/unset presence bit. Writes ride the paired INTENT normally and ECHO confirms application WITHOUT echoing the value. A WiFi password must never ride a retained snapshot that open-access `watch` sessions receive. |

## Procedure phases

Only the lifecycle phases are registered. Any generic client can render
these without knowing the procedure. Values 128–255 are device-defined
intermediate steps; a client that does not recognise one renders it as
`running`.

| Value | Phase | Notes |
|---|---|---|
| `0` | `idle` | not running; the reconnect-safe resting value |
| `1` | `running` | started and in progress; `progress` 0–100 is advisory |
| `2` | `succeeded` | terminal, ok. Also EVENTed (RFC-020). |
| `3` | `failed` | terminal, error — `result` u16 carries a nack_codes value or a device code |
| `4` | `aborted` | terminal, cancelled or superseded |

