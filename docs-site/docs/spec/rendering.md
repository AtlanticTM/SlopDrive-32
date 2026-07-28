---
title: Rendering
description: >-
  SlopSync clause 19: the rendering constitution, the three-tier channel
  taxonomy, and capability interfaces — establishing RENDERING.md as the
  normative client-rendering companion.
register: IEEE
generated: true
---

<!-- ==========================================================
     GENERATED FILE — DO NOT EDIT.
     Source of truth: docs/slopsync/SPEC.md
     Generator:       docs-site/tools/gen_spec_pages.py
     Regenerate:      python docs-site/tools/gen_spec_pages.py
     CI gate:         python docs-site/tools/gen_spec_pages.py --check
     Normative text is copied verbatim. Hand edits are overwritten
     and fail the docs build. Edit the specification instead.
     ========================================================== -->

# 19. Rendering *(normative)* {#s19}

## 19.1 The rendering constitution {#s19-1}

How a conformant client turns a hub's catalog into an actual user interface — categories, ranks, archetypes, widget patterns, regions, page composition, and the renderer conformance laws — is specified in full in `RENDERING.md`, the normative companion to this document (RFC-048). RENDERING.md is normative in the sense [§8.8](catalog.md#s8-8)/[§8.9](catalog.md#s8-9) already are: a client claiming conformance to it MUST follow its MUST clauses exactly as it must follow this document's, and its enumerable vocabularies are frozen at the v1.0 tag under the same no-reuse/no-renumber discipline [§5.7](wire-format.md#s5-7) applies to the wire registry.

This section states only what belongs in SPEC proper — channel-level semantics — and stops there by design: **SlopSync describes what things *are*, never how they *look*** ([§1-7](foundations.md#s1)) is unchanged. No widget hint, layout rule, color, or pixel is ever wire-visible; RENDERING.md's entire vocabulary is either registry-numbered metadata (categories, ranks, units, ...) or purely client-side derivation and behavior. Nothing in RENDERING.md is wired onto a real catalog entry as of this landing — [§18-23](limitations.md#s18) records that plainly.

## 19.2 The three-tier channel taxonomy {#s19-2}

Every channel a hub declares is exactly one of:

- **CORE** (`0x0001`–`0x007F`) — machine-unspecific protocol machinery. A channel that assumes a motor, an actuator, or any physical capability MUST NOT be CORE.
- **STANDARD** — machine-agnostic **capability** channels, declared per capability the hub actually has rather than per machine kind. A hub SHOULD expose the well-known channel for any capability it has (an axis's `motion`, a metered `power`, usage `odometer`, and the two standardized generator **capability interfaces** — a built-in pattern generator's role set, and the fray-d-shaped advanced-generator surface) carrying at minimum the field set RENDERING.md [§2.2](foundations.md#s2-2) names for that capability. This is SHOULD-level for existing hubs, MUST-level for hardware-hub-profile conformance from the v1.0 tag forward.
- **DEVICE** — everything else, wholly catalog-described, exactly as today.

The taxonomy is a classification of intent, not a new wire mechanism: **no frame changes, no core-channel changes.** It answers "does a client that has never met this hub still render a *good* instrument for a capability it has," which pure catalog description ([§8](catalog.md#s8)) cannot guarantee on its own — two axis-bearing hubs may otherwise expose their position, target and speed under names, groupings and units a generic client cannot correlate.

**Capability discovery is unaffected.** A STANDARD channel is discovered exactly like any other: its presence in the catalog *is* the capability advertisement ([§6.3](session.md#s6-3)'s "capability discovery is catalog introspection" is unchanged). STANDARD is a naming and minimum-field-set convention layered on top, never a new discovery mechanism.

Full capability-interface field tables, the derivation chain, and every rendering rule built on top of this taxonomy are RENDERING.md [§2-14](foundations.md#s2).
