---
title: Safety codes
description: Generated tables of SlopSync safety intent operations and safety cause codes.
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

# Safety codes

## Safety intent operations

These are the `value` map key 1 of the `safety-intents` channel
(`0x0005`).

**`stop` and `estop` are role-exempt. Any session may send them,
including a `watch` session.** Safety outranks authorization. The wrong
choice here means the person standing in the room cannot stop the
machine. Every other operation requires `control`.

| Op | Name | Meaning |
|---|---|---|
| `1` | `estop_clear` | clear the ESTOP latch (§11.2 conditions apply; NACK CLEAR_REFUSED otherwise). Requires `control`. |
| `2` | `stop` | controlled decel stop (§11.1). ROLE-EXEMPT. |
| `3` | `hold` | position hold (§11.1). Requires `control`. The HUB latches all four levels in 0x0003 — delegate acceptance is what triggers the latch; a hub whose delegate does not implement this NACKs UNSUPPORTED_OP, which is discoverable and honest (RFC-025a). |
| `4` | `pause` | pattern pause (§11.1). Requires `control`. |
| `5` | `resume` | resume from HOLD/PAUSE (§11.1). Requires `control`. |
| `6` | `estop` | ASSERT e-stop (RFC-010). ROLE-EXEMPT. The hub treats it exactly as a valid 0xE5 frame: latch, cause=user, publish 0x0003, EVENT twin. The raw 0xE5 frame stays as the deframed-path/relay guarantee; this op is the trivially-implementable client path — without it the red button silently degrades to a decel-stop, which is why this gated port-81 deletion. |
| `7` | `override_on` | engage manual override (RFC-025c). Requires `control`. Override/bypass are SAFETY-domain state, not rail-UI state: they render near the rail but other surfaces need them, so they live in the 0x0003 snapshot (appended byte) and are written here. |
| `8` | `override_off` | release manual override. Requires `control`. |
| `9` | `bypass_on` | engage limit bypass (RFC-025c). Requires `control`. The per-move `bypass` key on a motion INTENT is unaffected and stays as-is. |
| `10` | `bypass_off` | release limit bypass. Requires `control`. |

## Safety causes

One taxonomy, two wire homes: the ESTOP frame's `cause` byte, and the
`cause` field of the latched `safety` STATE snapshot (`0x0003`).

| Value | Cause | Meaning |
|---|---|---|
| `0` | `user` | operator-initiated (physical button, UI, safety-intents `estop`/`stop`) — §5.5 |
| `1` | `deadman` | §11.3 deadman window actually elapsed (silence timeout, not some other way the session ended — see session_loss) |
| `2` | `fault` | hub/driver-detected fault |
| `3` | `relay` | relay-originated (segment-local safety event) — §5.5 |
| `4` | `session_loss` | RFC-022.3: the owning session ended by ANY non-deadman teardown path (GOODBYE, rude detach, either eviction, slot reuse) — §6.8 / RFC-005's teardownSession() loss policy. Was misreported as cause=deadman before this value existed. |

`deadman` means the silence window actually elapsed. Every other way a
session ends latches `session_loss`. Reporting a closed browser tab as a
deadman timeout was a real bug; these are two different events.
