---
title: Rendering vocabulary
description: Generated tables of the RFC-048 rendering vocabulary: categories, ranks, value axes, units, action tags, archetypes, regions, renderer classes and widget patterns.
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

# Rendering vocabulary

These are the numbers behind [RENDERING.md](../../spec/rendering.md), the normative UI-rendering companion to the specification. Every vocabulary below is frozen at the v1.0 tag. None is wired onto a real catalog entry yet. See the specification's [known limitations](../../spec/limitations.md).

## Categories

`category` answers WHERE a catalog entry lives. Ids 1 to 14 are the frozen, complete spec set, in canonical menu order. An unrecognized id, including an untaught vendor id, MUST render under `other`. It keeps the catalog-provided label. It is never dropped.

| Id | Category | Notes |
|---|---|---|
| `1` | `control` | driving the machine now: move, pattern run/speed/depth, streams |
| `2` | `motion` | live physical telemetry |
| `3` | `safety` | faults, interlocks, e-stop state: the stop affordance itself is rank-pinned (ui_ranks), not a menu item here |
| `4` | `limits` | window + ceilings |
| `5` | `library` | stored content: presets, patterns, scripts, positions, profiles |
| `6` | `playback` | hub-local content transport: play/pause/seek/queue |
| `7` | `auxiliary` | secondary actuators: heat, lube, suction, inflation |
| `8` | `automation` | routines, schedules, scenes |
| `9` | `tuning` | engine internals, calibration |
| `10` | `hardware` | geometry, drive config, sensors, homing |
| `11` | `network` | WiFi/BLE state, endpoints, provisioning |
| `12` | `session` | clients, roles, ownership, pairing/trust |
| `13` | `system` | power, thermals, memory, firmware, logs |
| `14` | `other` | the defined overflow: every unrecognized category id (including an untaught vendor id) renders here, per the graceful-extension rule |

`0x40` to `0x7E` is the vendor/device range. A hub that declares one MUST supply a label. `15` to `0x3F` is reserved for future spec-registered categories. `0x7F` and above is reserved.

## Ranks

`rank` answers HOW MUCH a catalog entry or field matters by default. Unknown rank, or no rank annotation at all, renders as `detail`.

| Id | Rank | Notes |
|---|---|---|
| `0` | `hero` | the machine's face; surfaced by default on every renderer class |
| `1` | `control` | everyday driving controls; surfaced by default on handheld/full, one navigation step away on glance |
| `2` | `detail` | useful but not primary; reachable, not surfaced |
| `3` | `advanced` | the `setting_flags.advanced` bit's migration into this ladder; hidden behind an advanced affordance by default, NEVER removed from the surface |
| `4` | `diagnostic` | reachable on every class; a renderer MAY choose to omit it under its own display budget, but that is the renderer's choice, not this rank's |
| `5` | `hidden` | carried on the wire for compatibility, NEVER rendered: the honest home for an inert-but-released field |

## Value axes

Three small, orthogonal vocabularies tag what statistic a field is. The default, when none is given, is `live` / `session` / `actual`.

### Aspect

| Id | Aspect | Notes |
|---|---|---|
| `0` | `live` | the value now (default) |
| `1` | `peak` | highest observed |
| `2` | `min` | lowest observed |
| `3` | `mean` | average observed |
| `4` | `total` | cumulative (an odometer figure) |
| `5` | `rate` | a derivative/frequency quantity |

### Scope

| Id | Scope | Notes |
|---|---|---|
| `0` | `session` | since this client connected (default) |
| `1` | `lifetime` | since the device was manufactured/last factory-reset |
| `2` | `window` | a bounded rolling window, device-defined width |

### Provenance

| Id | Provenance | Notes |
|---|---|---|
| `0` | `demand` | the raw requested value |
| `1` | `planned` | the target the planner is currently aiming for |
| `2` | `actual` | the measured value (default). demand/planned/actual are one quantity at three stages; the axis archetype's commanded-vs-actual overlay is their companion composition. |

## Units

Units are a frozen numeric companion to the existing free-string `unit` field. Both exist side by side. This table is not yet wired onto real catalog fields; that is next-phase work. The list is deliberately larger than current needs, to cover future actuators. An unrecognized unit id renders the catalog's own label string verbatim.

| Id | Unit | Quantity |
|---|---|---|
| `0` | `mm` | length |
| `1` | `mm_s` | speed (mm/s) |
| `2` | `mm_s2` | acceleration (mm/s²) |
| `3` | `mm_s3` | jerk (mm/s³) |
| `4` | `normalized` | 0-1 |
| `5` | `percent` | % |
| `6` | `hz` | Hz |
| `7` | `ms` | milliseconds |
| `8` | `s` | seconds |
| `9` | `v` | volts |
| `10` | `a` | amps |
| `11` | `w` | watts |
| `12` | `wh` | watt-hours |
| `13` | `deg_c` | °C |
| `14` | `count` | dimensionless count |
| `15` | `bytes` | data size |
| `16` | `db` | decibels |
| `17` | `n` | force, over-provisioned: force-feedback actuators |
| `18` | `kpa` | pressure, over-provisioned: suction/inflation |
| `19` | `ml` | volume, over-provisioned: lube dosing |
| `20` | `ml_min` | flow rate, over-provisioned: lube dosing |
| `21` | `rpm` | rotational speed, over-provisioned: rotary actuators |
| `22` | `bpm` | beats per minute, over-provisioned: bio-sync accessories |

## Action tags

A conformant client MAY special-case the specific `action.<name>` suffixes below. This lets it upgrade a generic `trigger` archetype into a purpose-specific rendering. An unregistered suffix remains legal. An unrecognized one renders as a generic trigger.

| Tag | Meaning |
|---|---|
| `move` | the primary positional command: usually already the axis archetype's own binding, rarely a separate trigger |
| `safety` | a safety-adjacent action outside the law-bound stop archetype itself (e.g. an override/bypass toggle's companion action) |
| `home` | a homing-cycle trigger |
| `calibrate` | a calibration-cycle trigger; commonly the entry point to a `wizard` widget pattern |
| `reset` | aspect-group reset linkage (value_aspects/RENDERING.md §5.4); co-located with the group it resets, confirm-gated |
| `save` | persist current state (settings, not a preset item) |
| `persist` | commit a value past a reboot: distinct from `setting_flags.restart_required`, which is about WHEN a change takes effect, not whether it survives one |
| `pair` | enters a pairing ceremony affordance |
| `identify` | blink-to-find: every device ecosystem needs one |
| `admin` | a generic administrative action not covered by a more specific tag |
| `reboot` | firmware reboot; SHOULD always confirm (cbor_keys.reboot_in_ms) |
| `preset_save` | save-to-store, part of the generator-advanced preset roster |
| `preset_recall` | load-from-store, same roster |

## Archetypes

An archetype is the control style and interaction contract a catalog field or channel renders with. A normative decision table derives it in the common case (RENDERING.md §8.2). An explicit `archetype` hint overrides that table. `Fallback` is the mandatory composition of frozen primitives every archetype declares. A primitive lists itself.

| Id | Archetype | Semantic | Fallback |
|---|---|---|---|
| `0` | `readout` | display of a value; bounds present -> bar/gauge projection | `readout` |
| `1` | `indicator` | status lamp; never the sole carrier of a safety fact | `indicator` |
| `2` | `slider` | bounded numeric intent; commit-on-release | `slider` |
| `3` | `stepper` | precision numeric, increments in `step`-sized ticks | `stepper` |
| `4` | `toggle` | boolean | `toggle` |
| `5` | `select` | enum + options; wire value is the array index | `select` |
| `6` | `trigger` | payload-less intent (button); destructive flag -> mandatory confirm on every class | `trigger` |
| `7` | `axis` | 1-D positional hero control (role command.position) with commanded-vs-actual overlay | `axis` |
| `8` | `chart` | time-series; glance degrades to sparkline/value; missing samples render as gaps | `chart` |
| `9` | `list` | roster/store items + item actions; pending is a THIRD state distinct from success/failure | `list` |
| `10` | `text` | constrained string; glance projects a digit/char wheel: the pairing-PIN path | `text` |
| `11` | `stop` | the safety stop affordance: bound BY LAW to safety-op identity, never derived from annotation; reachable at every rank on every class, never role-gated, never hidden | `stop` |
| `12` | `pad2d` | two-axis control: the multi-axis runway | `slider` + `slider` |
| `13` | `color` | chromatic actuator setpoint (lighting/glow accessories) | `slider` + `slider` + `slider` |
| `14` | `datetime` | moment/interval input (automation schedules) | `text` |

## Regions

There are four abstract placement zones, plus one modal layer. Geometry, position, size and style within a region are the renderer author's craft. What lives in each region is normative.

| Id | Region | Contents |
|---|---|---|
| `0` | `primary` | hero-rank patterns (axis-hero and peers); the machine's face, exactly one per axis/capability instance |
| `1` | `persistent` | the safety-strip: latch state, ownership, the stop archetype. MUST remain visible in every navigation state of every class |
| `2` | `content` | the category tree's territory: cards/panes/menu screens in canonical category order; diagnostic-rank material last or collapsed by default |
| `3` | `utility` | connection/session status, identity, theme: chrome about the CLIENT, kept out of the machine's way |
| `4` | `overlay` | the modal layer: confirms, pairing knocks, alerts. Only ceremony/confirmation content may use it; nothing persistent lives here |

## Renderer classes

All classes render one category tree. They differ in projection and default surfacing, never in reachable content. A device between budgets adopts the nearer class.

| Id | Class | Notes |
|---|---|---|
| `0` | `glance` | OLED remote: home screen = hero-rank; categories = a menu stack; everything except diagnostic reachable by navigation; stop affordance reachable from every screen |
| `1` | `handheld` | phone: categories as sections/tabs; hero+control surfaced, detail one tap away |
| `2` | `full` | desktop: categories as panes, everything visible |

## Widget patterns

These are proven compositions extracted from the reference client. `Required` marks a pattern that a handheld or full client MUST provide when its capability is present. A glance-class device may reach it through the category tree instead.

| Id | Pattern | Composition | Required |
|---|---|---|---|
| `0` | `axis-hero` | rail + window band + command tape + live position/velocity numerals + nested plan-view; command-tape domain is the REPORTED window | `yes` |
| `1` | `plan-view` | in-flight plan lane; visibility gated by data freshness, collapses to zero height when idle |  |
| `2` | `link-strip` | utility region: connection phase, rx liveness, catalog state; safety-relevant chips pinned, rest may scroll |  |
| `3` | `safety-strip` | persistent region; ops sorted role-EXEMPT-first; stop sticky-visible within any overflow |  |
| `4` | `settings-card` | a subgroup's fields via the archetype decision table; every disabled control shows WHICH gate disabled it |  |
| `5` | `scope` | multi-lane strip chart: missing samples render as GAPS never zeros; series colors stable by declaration order across reconnects |  |
| `6` | `event-stream` | bounded rings; unknown body fields render generically as key=value, never dropped; auto-scroll with user-scroll override |  |
| `7` | `roster` | list + item actions; pending is a THIRD state distinct from success/failure; locked-by-role honestly distinct from empty |  |
| `8` | `protocol-pane` | the one deliberately device-aware diagnostic surface: wire ids visible by design |  |
| `9` | `pattern-panel` | the standard generator surface: run/stop with live state, pattern selection as an exclusive-choice group, knob sliders for whichever optional roles exist, source.background_run co-located with run/stop | `yes` |
| `10` | `generator-advanced` | the fray-d surface: master controls + the four modifier lanes rendered as parallel lane groups + preset save/recall via the store, source.background_run co-located with the master run/stop | `yes` |
| `11` | `transport` | playback: play/pause/seek/queue cluster for hubs that play content |  |
| `12` | `wizard` | stepped ceremony flow: pairing, calibration, provisioning; glance-class projects it as sequential menu screens |  |

