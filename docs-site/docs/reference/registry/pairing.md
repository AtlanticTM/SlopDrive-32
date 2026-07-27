---
title: Pairing modes
description: Generated table of the SlopSync pairing mode bitmask advertised in WELCOME.
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

# Pairing modes

A hub advertises the modes it currently offers as a bitmask in the
WELCOME `trust` sub-map. The trust ledger records the single bit a paired
device used.

All three modes end in the same `PAIR_GRANT {token, role}`. **The role is
an attribute of the grant, never of the ceremony.** A hub has zero or one
PIN, never one secret per tier.

| Mask | Bit | Name | Notes |
|---|---|---|---|
| `0x01` | `bit 0` | `knock_approve` | PRIMARY and capability-agnostic: bare PAIR_REQ with no proof -> bounded pending list (pairing_pending_max) exposed as protocol state (0x000A/0x000B) -> ANY `configure` session approves {instance_id, role}. The joiner needs one button and no display; the trusted surface is any configure client, which kills the circular 'the WebUI is trusted because it's the WebUI' dependency. RECOMMENDED partly because its approval surface shows the knocker's identity on hardware the attacker does not control. |
| `0x02` | `bit 1` | `pin_proof` | the HMAC-PIN flow, for keyboard-bearing joiners when no configure session exists. HONESTY CLAUSE (normative): 4 digits = 10^4 offline HMACs, so a passive observer of the exchange can brute-force it. Acceptable for the v1 threat model (casual/drive-by prevention) and MUST be stated plainly. SPAKE2 is the reserved v2 upgrade — not v1, because WebCrypto has no PAKE and mandating it would exile the browser client. |
| `0x04` | `bit 2` | `push_to_pair` | PHYSICAL-PRESENCE proof opens a short SINGLE-GRANT window. The spec requires the PROOF, not a GPIO — minimum hardware is NONE, because the power cord is the button: factory-fresh (zero configure tokens) boots claimable and the first knock gets `configure` (possession is root); later, N=3 consecutive boots with uptime <10 s opens the window (NVS counter only; cannot collide with a session, since any power loss already stops motion and forces re-home). A hub with a real button MAY bind it — UX upgrade, never required. FACTORY RESET MUST BE A HARDER GESTURE than opening pairing. |


## Administration operations

These are the `value` map key 1 of the `session-admin` channel
(`0x0009`). The channel requires `configure`.

**The trusted surface is a tier, not an app.** Any `configure` session
reaches every operation here. Nothing in the protocol knows or cares
whether that session is the machine's own web page, a phone, or a
command line.

A `configure` session may grant up to its own tier, `configure`
included. The paired-device roster is the audit trail, rather than a
ceiling that would leave the first administrator unable to appoint a
second.

| Op | Name | Meaning |
|---|---|---|
| `1` | `evict` | RFC-018: GOODBYE the session named by `session_id` with SESSION_EVICTED. Runs the full §6.8/RFC-005 teardown — the evicted session's source ownership is released under its §11.3 loss policy exactly as if it had crashed, because 'no unmonitored path to motion' does not get an exception for admin actions. Evicting your OWN session is legal and is just a rude GOODBYE to yourself. |
| `2` | `pair_approve` | RFC-027(a): approve the pending knock named by `instance_id` at `role`, issuing PAIR_GRANT {token, role} to the knocker. ALSO the RFC-029 item-2 RE-APPROVAL verb: applied to a device in `recognized_pending` it re-records the observed version and restores the suspended role — approving a knock and re-trusting a changed device are the same decision ('this identity may do this'), so they are the same op rather than two that could drift. |
| `3` | `pair_deny` | RFC-027(a): drop the pending knock named by `instance_id` without issuing a token; emits pairing_events `denied`. On a `recognized_pending` device this is a REVOKE (op 4) in effect — deny means 'no', and leaving a suspended entry in the ledger after an operator said no would be a lie the roster tells forever. |
| `4` | `revoke` | RFC-027(4)/029: delete `instance_id` from the trust ledger. Revocation is PROTOCOL, not a WebUI feature — that is the entire point of putting it here. Takes effect at the next HELLO (an already-live session keeps the role it was admitted with until it reconnects or is evicted; use evict to end it now). Emits pairing_events `revoked`. |

## Trust ledger states

The `state` field of a paired-devices item. A revoked device has no
entry at all, so revocation is an absence and never a third state.

| Value | State | Meaning |
|---|---|---|
| `0` | `trusted` | paired, and the version observed at the last HELLO matches the version recorded when the role was approved. The granted role applies in full. |
| `1` | `recognized_pending` | RFC-029 item 2's tripwire fired: this device presented a DIFFERENT `client_ver` than the ledger recorded. The session is admitted at `watch`, the granted role is SUSPENDED (not revoked), and a re-approval is surfaced to configure sessions via 0x000A/0x000B. HONESTY CLAUSE, normative: the version is SELF-REPORTED, so this catches an honest update and nothing else — a deliberately malicious update lies about its version and keeps its token. The real bounds on a hostile client are role scoping, instant revocation, roster visibility and the role-exempt safety ops. Do not let a UI imply this is attestation. |


## Token presentation modes

The `trust` sub-map's `presentation_mode`, recorded per device in the
ledger so that security posture is something an operator can see.

`bearer` is the floor and the default. `proof` is recommended for any
client that already has SHA-256, and is never required of anyone.

| Value | Mode | Meaning |
|---|---|---|
| `0` | `bearer` | raw 16-byte token in HELLO. LEGAL, DEFAULT, and the potato floor — a coin-cell client does exactly this and nothing more. v1 transports are cleartext, so a passive LAN observer who captures one HELLO owns the credential until it is revoked; §12.1 excludes that attacker, and this mode accepts that ceiling knowingly. |
| `1` | `proof` | HMAC-SHA256(key = token, message = the WELCOME nonce) truncated to 16 B, presented in an AUTH (0x1C) frame after WELCOME. The token itself NEVER crosses the wire, so a sniffer captures a one-time proof and not the credential. Costs exactly one extra round trip per connect (HELLO -> WELCOME -> AUTH -> GRANT); the client is at `watch` in between, which is the correct posture for a client that has not yet proved anything. The 'reuse the previous session's nonce to skip the round trip' shortcut was DROPPED as replay-unsafe — see trust_keys.token_proof. |

A reported client version is a tripwire, not an attestation. It catches
an honest update. A deliberately malicious one reports whatever version
it likes and keeps its token. What actually bounds a hostile client is
role scoping, immediate revocation, its visibility in the roster, and
the fact that safety operations are role-exempt for everyone.
