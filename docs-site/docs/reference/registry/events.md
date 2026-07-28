---
title: Event kinds
description: Generated tables of event kind values for the spec-core EVENT channels, plus log severity levels.
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

# Event kinds

An EVENT reports an edge: something happened. A device-authored EVENT
channel enumerates its own kinds in its catalog entry. A spec-core
channel's kinds are agreed here, because hub and client cannot negotiate
them.

Events are best-effort and are not replayed. Anything a client cannot
afford to miss also has a latched STATE twin.

## session-events (`0x0007`)

Session lifecycle and control-ownership transfer.

| Kind | Name | Meaning |
|---|---|---|
| `1` | `takeover` | control source ownership transferred (§11.4) |
| `2` | `session_joined` | a session reached GRANTED |
| `3` | `session_left` | a session ended (any reason) |
| `4` | `session_stale` | RFC-042: a session's silence exceeded its liveness window (deadman for a source-owner, idle reaping otherwise) and it was marked STALE rather than torn down: slot, session_id, and grants are RETAINED; any owned source was released (unconditionally, latching nothing per RFC-045). Body carries the affected session_id, same shape as `takeover`. |
| `5` | `session_resumed` | RFC-042: a STALE session returned to LIVE, either by any frame arriving on its still-attached transport (§6.6: any received frame is proof of life) or by a fresh HELLO reattaching a new transport to the same session identity (§6.3 migration path). Body carries the affected session_id. |

## log (`0x0008`)

One kind. The per-line content rides the `body` sub-map, schema'd by the channel's own catalog entry.

| Kind | Name | Meaning |
|---|---|---|
| `1` | `entry` | a log line was published (fields ride `body`: level, tag, hub-ms, message) |

## pairing-events (`0x000B`)

The EVENT twin of the [pending-pairing STATE channel](channels.md#spec-core-channels). None of these is a safety latch.

| Kind | Name | Meaning |
|---|---|---|
| `1` | `knocked` | a PAIR_REQ joined the pending list (0x000A): knock-and-approve or PIN mode (RFC-027.2) |
| `2` | `granted` | a pending knock (or an existing device's re-approval) was granted a role, via PAIR_GRANT or the 0x0009 admin surface |
| `3` | `denied` | a pending knock was denied by a `configure` session |
| `4` | `expired` | a pending knock's window elapsed unanswered (pairing_window_default_s) |
| `5` | `window_opened` | a pairing association window opened (push-to-pair boot gesture, or a mode newly advertised in `trust`.pairing_modes) |
| `6` | `window_closed` | the pairing association window closed |
| `7` | `revoked` | a paired device's token was revoked from the trust ledger (RFC-018 admin surface, store 0x000C) |
| `8` | `recognized_pending` | RFC-029 item 2: a paired device's observed `client_ver` changed; state dropped trusted -> RECOGNIZED-PENDING (admitted at watch, granted role suspended pending re-approval) |

## safety-events (`0x000E`)

The EVENT twin of the [safety STATE channel](channels.md#spec-core-channels). It fires only on a transition. A repeated e-stop re-broadcasts the latch. This is how loss recovery works. It does not re-announce an edge that did not happen.

| Kind | Name | Meaning |
|---|---|---|
| `1` | `estop_latched` | the ESTOP bit went 0 -> 1 (§5.5). `body` carries word/cause/owner_session/estop_seq. Cause is a `safety_causes` value; `estop_seq` is the §5.5 per-INITIATION sequence, so repeats of one initiation share it. |
| `2` | `estop_cleared` | the ESTOP bit went 1 -> 0 via §11.2's guarded clear (`safety_ops::estop_clear` + the hub's and delegate's preconditions). Clearing never restarts motion; this edge says the latch is gone, never that the machine moved. |
| `3` | `stop_latched` | one or more of STOP / HOLD / PAUSE went 0 -> 1. `body.level` is the bitmask of the bits that NEWLY set (safety word bits 1/2/3), so one edge reports one operator action even when it sets several. Cause distinguishes an operator `stop` (user) from a §11.3 deadman (deadman) from a teardown loss policy (session_loss). That is the whole reason this edge is worth having: all three look identical in the snapshot. |
| `4` | `stop_cleared` | one or more of STOP / HOLD / PAUSE went 1 -> 0 (`resume`, or a STOP cleared by an accepted new motion intent per §11.1). `body.level` is the bitmask of the bits that NEWLY cleared. |

## Log severity levels

The [log channel](channels.md#spec-core-channels)'s `body.level` field.
These values mirror the firmware logging library number for number, so
the bridge is a cast and never a translation table.

| Value | Level | Notes |
|---|---|---|
| `0` | `trace` | sloplog::Level::Trace (SLOGT) |
| `1` | `debug` | sloplog::Level::Debug (SLOGD) |
| `2` | `info` | sloplog::Level::Info (SLOGI) |
| `3` | `warn` | sloplog::Level::Warn (SLOGW) |
| `4` | `error` | sloplog::Level::Error (SLOGE) |
| `5` | `fatal` | sloplog::Level::Fatal (SLOGF) |

There is no wire value for `off`. `off` is a floor sentinel, so no record
can arrive at that level.
