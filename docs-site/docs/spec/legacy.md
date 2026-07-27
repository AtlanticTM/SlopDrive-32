---
title: Legacy interop
description: >-
  SlopSync clause 15: legacy text-protocol edges as synthetic sessions, and
  the predecessor-protocol migration map.
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

# 15. Legacy Interop *(normative rules, informative mapping)* {#s15}

## 15.1 Text-protocol edges as synthetic sessions *(normative)* {#s15-1}

A hub MAY continue to accept legacy text-protocol ingest (TCode over serial, BLE-NUS, a raw-text socket, an outbound bridge client, a radio dongle chain). Where it does, **the hub MUST wrap each active legacy edge in a synthetic session**: an internal session object with its own `client_kind`, capability scoped to that edge's arbiter source only, ownership per [§11.4](safety.md#s11-4), and a deadman equal to the edge's stream-quiet timeout (hub-configurable within [§11.3](safety.md#s11-3)'s clamp).

Effect: legacy clients appear in the session roster, their motion obeys the same deadman, ownership and safety rules as native sessions, and there is **no unmonitored path to motion**. They receive no SlopSync frames; the synthesis is entirely hub-side bookkeeping.

TCode passthrough is one of the three sanctioned motion input modes ([§9.6-1](channels.md#s9-6)), named there so it is understood as a planned part of a closed surface rather than a future fourth mode.

## 15.2 Predecessor-protocol migration *(informative)* {#s15-2}

The reference implementation's legacy binary UI protocol is SlopSync's direct ancestor; every concept maps:

| Legacy | SlopSync successor |
|---|---|
| HELLO `{proto_ver, cfg_gen}` | HELLO/WELCOME ([§6.2](session.md#s6-2)–6.3) — adds identity, tiers, grants, etag, `boot_id`, readiness |
| Telemetry frame: fixed header + n samples | STREAM bundle ([§5.4](wire-format.md#s5-4)) on a catalog-declared channel; flag bits become `safety` and status STATE channels |
| Periodic status frame | `hub-status` STATE plus power/link STATE channels |
| CLOCK t0/t1/t2 | CLOCK ([§7.1](time.md#s7-1)) — byte-identical exchange, new frame type id |
| Interpolator/stats frames | device-defined STATE channels |
| Anomaly event ring | an EVENT channel plus a latched summary STATE channel ([§9.4](channels.md#s9-4) duality) |
| Command frame + id + JSON | INTENT channels ([§9.3](channels.md#s9-3)) — ids become `intent_id`, JSON becomes schema'd CBOR, op codes become channel ids |
| Echo frame `{id, ok, cfg_gen, JSON}` | ECHO `{intent_id, applied, cfg_gen}` — same idempotency ring semantics, now specified |
| Full-snapshot config fetch | retained STATE push ([§9.1](channels.md#s9-1)) — the resync *is* the connect path now |
| `cfg_gen` threading | unchanged in meaning; formalized in [§4.2](foundations.md#s4-2) and tightened in both directions |
| per-client 32-deep idempotency ring | unchanged; normative in [§9.3](channels.md#s9-3) |
| HTTP diagnostic endpoints (log, capabilities, clients, settings) | in-band: the log EVENT channel ([§16.2](errors.md#s16-2)), WELCOME `identity` ([§6.3](session.md#s6-3)), the roster and admin channels ([§12.7](security.md#s12-7)), and the settings metamodel ([§8.8](catalog.md#s8-8)) |

Cutover: a hub serves both protocols during migration on different endpoints or subprotocols; the legacy plane is retired when nothing speaks it. No flag day.
