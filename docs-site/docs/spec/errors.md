---
title: Errors and diagnostics
description: >-
  SlopSync clause 16: the NACK and GOODBYE code taxonomy, and the
  observability channels a hub exposes about itself.
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

# 16. Errors and Diagnostics *(normative)* {#s16}

## 16.1 NACK and GOODBYE {#s16-1}

NACK (CBOR): `code` (16) from the registry's ranged taxonomy, optional `channel_id` (15), `intent_id` (18), `intent_seq` (41), `detail` (17), and `retry_after_ms` (31) with BUSY.

- **Ranges:** `0x00xx` protocol, `0x01xx` session/auth, `0x02xx` subscription/QoS, `0x03xx` intent, **`0x04xx` safety refusals**, `0x05xx` transfer. UIs SHOULD render `0x04xx` distinctly: a refusal because the machine is e-stopped is user-meaningful, not an "error".
- **Unknown code → treat as its range generic** ([§4.3](foundations.md#s4-3)).
- **`intent_seq` correlates a NACK to the frame that provoked it.** Hubs SHOULD populate it whenever a specific inbound frame provoked the NACK; clients MUST tolerate its absence. Without it, a client with two intents in flight **on the same channel** cannot tell which one was refused, and must guess.
- **`detail` is diagnostic, never required for machine handling.** A sender truncates it to `nack_detail_max_bytes` (48); an over-length detail MUST NOT cause the NACK itself to vanish ([§5.8-4](wire-format.md#s5-8)).
- **NACK never closes the session by itself; GOODBYE does.**

**GOODBYE draws its `code` from the same `nack_codes` table.** A separate code space was considered and rejected: [§4.3](foundations.md#s4-3)'s unknown-code handling is a *range* fallback, and two overlapping spaces would make the range of an unknown code ambiguous, so a forward-compatible receiver could not classify it. Codes usable as a GOODBYE reason are marked as such in the registry: `NORMAL_CLOSURE`, `SESSION_EVICTED`, `DUPLICATE_INSTANCE`, `DEADMAN_TIMEOUT`, `READY_TIMEOUT`, `REBOOTING`, `UNAUTHORIZED`.

## 16.2 Observability {#s16-2}

The hub exposes its own health as ordinary channels, dogfooding the protocol:

- **`hub-status`** — heap, uptime, per-binding client counts, `events_dropped`, shed and eviction counters. It carries **no firmware version**: identity has exactly one home ([§4.2-4](foundations.md#s4-2), [§6.3](session.md#s6-3)).
- **`session-roster`** and the session-events channel — [§12.7](security.md#s12-7).
- **The log channel** — a spec-core EVENT channel carrying `{level, tag, hub-ms, message}` in its `body` sub-map, with `level` from the registry's `log_levels`. Bounded drop-oldest with the [§9.4](channels.md#s9-4) visible counter, `background` priority, `watch` access. It declares a `replay_depth` (default `log_replay_depth_default`, 32), which is the **named exception** to [§9.4](channels.md#s9-4)'s no-replay rule: on grant the hub MAY replay its ring tail, so "what went wrong just before I connected" is answerable. A hub whose logging back-end drops records *before* they become wire events SHOULD report that loss as a field on the next published entry rather than inventing a second event kind — there is one home for drop counters.
  Where a hub previously demoted its serial console on first diagnostic HTTP fetch, that handoff re-binds to the **first log-channel grant**.

Diagnostic verbosity beyond these channels is hub-implementation territory.
