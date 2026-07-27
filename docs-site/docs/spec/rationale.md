---
title: Rationale and history
description: >-
  SlopSync appendices H-J: design rationale and rejected alternatives, the
  design-review gap-closure map, and what changed since the v1 draft.
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

# Appendices H-J: rationale and history

## Appendix H — Design rationale and rejected alternatives *(informative)* {#appendix-h}

**Stacks evaluated and not adopted.**

- **On-device MQTT broker:** available brokers ignore retained messages and wills and offer QoS 0 only — the device-shadow primitive would be rebuilt application-side anyway; transports are TCP-shaped; licensing was hostile. The glue equalled this document's hard parts with none of its fit.
- **zenoh-pico:** runs on the target but peer-unicast nodes do not route (no hub role on-device); no datagram-radio or browser-server transport; custom-transport API unresolved upstream.
- **MQTT-SN:** the gateway side has no MCU implementation, and reference gateways are host programs that themselves need an upstream broker.
- **Micro XRCE-DDS:** the Agent (the hub role) is host-only. Its transport abstraction was adopted ([§13.1](transports.md#s13-1)); the stack was not.
- **ThingSet:** the best conceptual match, but the node library is RTOS-bound, the radio binding is unlisted, and subscription-rate negotiation does not exist. Its self-describing data model was adopted in spirit ([§8](catalog.md#s8)).
- **Matter:** ~1.5 MB flash and ~195 KB RAM before application logic; a cluster model aimed at smart-home semantics; commissioning UX wrong for this product. Its *commissioning* semantics were adopted ([§12.3c](security.md#s12-3)).

**Design decisions.**

- **Why not SSE for telemetry:** [§1.1](foundations.md#s1-1).
- **Why not deltas on STATE:** loss becomes corruption; [§9.1](channels.md#s9-1).
- **Why not per-sample ACKs on motion input:** an ACK storm at 333 Hz, and the observable applied position is the meaningful confirmation anyway; [§9.3](channels.md#s9-3).
- **Why hybrid CBOR + packed structs:** exactly-one-encoding CBOR gives byte-exact vectors and canned templates for constrained clients; packed structs give a zero-cost hot path at 240–333 Hz. Pure CBOR taxes the hot path; pure protobuf taxes every client with a codegen toolchain and varint decode.
- **Why a pairing baseline rather than open or TLS-everywhere:** an open LAN made tier grants fiction on a product where unauthorized control is a safety issue; TLS costs tens of KB of RAM per session and has hostile self-signed-certificate UX on a LAN. HMAC-PIN plus knock-and-approve is the cheapest mechanism that makes "control" mean something (H3 bounds the claim).
- **Why the pattern-continues deadman default:** a hub-autonomous generator is not a client's puppet; a phone screen-lock must not interrupt a session, while a vanished *streamer* must stop motion in under a second. [§11.3](safety.md#s11-3).
- **Why the readiness gate rather than buffering:** a hub knows it *sent* chunks, not that they *arrived* — true on TCP, false on a radio. And shipping undecodable frames into a client's bin wastes airtime. The gate is one flag and zero RAM. [§6.4](session.md#s6-4).
- **Why one code space for NACK and GOODBYE:** [§4.3](foundations.md#s4-3)'s unknown-code fallback is range-based, and two overlapping spaces make an unknown code unclassifiable. [§16.1](errors.md#s16-1).
- **Why `role` is a string and `category` is a number:** action roles carry a device-chosen suffix that no integer enum can express, while categories need a canonical cross-hub order that only a registered enum gives. [§8.8](catalog.md#s8-8).
- **Why `option_access` is schema-only:** a layout field is the read side; all write authorization flows through the paired INTENT channel. It also keeps the field map inside the depth-4 cap, which is already at its limit. [§8.8](catalog.md#s8-8).
- **Why stores are catalog entries rather than a parallel array:** a second top-level array would break the catalog root shape, the id sort, the etag computation and the per-entry depth rules — all four. [§8.7](catalog.md#s8-7).
- **Why the trust ledger is a store and not a packed roster:** a ledger entry does not fit a 242-byte snapshot at useful capacity; measurement put it at 2–7 entries depending on field set. [§12.6](security.md#s12-6).
- **Why signing is deferrable:** a software ECDSA on an accelerator-less controller is one uninterruptible tens-of-milliseconds call, and doing it inline would stall the hub's own tick for every connecting client. [§12.5](security.md#s12-5).
- **Why proof mode costs a round trip:** the shortcut that would have saved it (reusing the previous session's nonce) is replay-unsafe, and a successful replay *evicts the real client*. [§12.4](security.md#s12-4).
- **Why segments are non-decimable:** shedding's whole justification is that a dropped sample is recoverable by interpolation. For a timed command that is simply false. [§10.4](qos.md#s10-4).
- **Why `stream_kind` is registered rather than inferred:** the inference available was a free-form unit string, so two conforming hubs could shed differently under identical load. [§9.2](channels.md#s9-2).

## Appendix I — Design-review gap closure map *(informative, audit artifact)* {#appendix-i}

Findings from the pre-specification adversarial design review and from implementation, mapped to their resolving sections.

| Finding | Resolution |
|---|---|
| G1 idempotency vs reconnect | [§9.3](channels.md#s9-3) session-scoped ids + [§6.8](session.md#s6-8) reconcile-don't-retransmit + absolute-values rule |
| G2 stable client identity | [§6.1](session.md#s6-1) `instance_id` |
| G3 grant-reacquisition race | [§6.8](session.md#s6-8) + [§11.4](safety.md#s11-4) (no silent control resume post-deadman) |
| G4 mid-session subscriptions | [§6.7](session.md#s6-7) SUBSCRIBE/UNSUBSCRIBE, and PUBLISH for the c2h side |
| G5 snapshot vs delta vs MTU | [§9.1](channels.md#s9-1) full-snapshot + the 242 B fit rule |
| G6 STATE ordering on unordered transports | [§7.3](time.md#s7-3) per-channel seq, newest-wins |
| G7 retained-value rule | [§9.1](channels.md#s9-1) retained push; [§6.3](session.md#s6-3) |
| G8 event/state duality | [§9.4](channels.md#s9-4) duality rule; the safety edge channel is its literal instance |
| V1 three version tokens | [§4.2](foundations.md#s4-2), now four, scoped rather than unified |
| V2 packed-struct evolution vs pinned clients | [§5.4](wire-format.md#s5-4) append-only + prefix parsing; [§8.5](catalog.md#s8-5) |
| V3 catalog client-invariance | [§8.6](catalog.md#s8-6) |
| Q1 re-grant signaling | [§10.2](qos.md#s10-2) unsolicited GRANT |
| Q2 probe delays connect | [§6.5](session.md#s6-5) optional, post-READY |
| Q3 congestion signal per binding | [§10.3](qos.md#s10-3) + [§13.1](transports.md#s13-1) matrix |
| Q4 broadcast vs per-subscriber rates | [§10.6](qos.md#s10-6) highest-grant rule |
| Q5 never-shed overflow | [§10.4](qos.md#s10-4) bounded queues + slow-consumer eviction |
| Q6 shed semantics per class | [§10.4](qos.md#s10-4) normative table |
| S1 ESTOP clear authorization | [§11.2](safety.md#s11-2) clearing rules + `CLEAR_REFUSED` + the catalog-error clause |
| S2 ESTOP over lossy links | [§11.2](safety.md#s11-2) repeat-until-latch; [§14.2](transports.md#s14-2) fast path |
| S3 preemption honesty | [§11.2](safety.md#s11-2) H2 + [§13.1](transports.md#s13-1) delay column |
| S4 deadman scope | [§11.3](safety.md#s11-3) source-bound; [§6.6](session.md#s6-6) the second regime |
| S5 hold-vs-stop taxonomy | [§11.1](safety.md#s11-1) four levels, hub-latched |
| S6 same-source contention | [§11.4](safety.md#s11-4) exclusive ownership + TAKEOVER |
| S7 grants vs open LAN | [§12.3](security.md#s12-3) pairing baseline |
| S8 legacy edges bypass deadman | [§15.1](legacy.md#s15-1) synthetic sessions |
| T1 relay ACK semantics | [§14.2](transports.md#s14-2) hop-by-hop, H10 |
| T2 STREAM degradation matrix | [§13.1](transports.md#s13-1) + [§9.2](channels.md#s9-2) weakest-binding rule |
| T3 catalog transfer repair | [§8.4](catalog.md#s8-4) selective repair + timeout + fallback |
| T4 constrained clients and CBOR | [§8.5](catalog.md#s8-5) static profile + [§5.3](wire-format.md#s5-3) canned templates |
| T5 serial framing + ESTOP scan | [§13.5](transports.md#s13-5) COBS + scanner rule |
| T6 SSE | [§1.1](foundations.md#s1-1) non-goal |
| T7 WS binding details | [§13.2](transports.md#s13-2) |
| T8 clock through relays | [§7.4](time.md#s7-4) + [§14.3](transports.md#s14-3) a/b/c rule |
| T9 sim binding teeth | [§13.6](transports.md#s13-6) fault injection + deterministic mode |
| T10 pairing ceremonies per transport | [§12.3](security.md#s12-3), [§12.9](security.md#s12-9) |
| T11 admission control | [§6.3](session.md#s6-3) BUSY + retry_after + the capacity-exceeds-sessions rule |
| X1 which classes ECHO | [§9.3](channels.md#s9-3) |
| X2 config write races | [§9.3](channels.md#s9-3) `precondition` CAS |
| X3 liveness definition | [§6.6](session.md#s6-6) any-frame liveness |
| X4 vectors vs nondeterminism | [§17.2](conformance.md#s17-2) injected clock/RNG/crypto |
| X5 STREAM terminology collision | [§2.1](foundations.md#s2-1) stream-/datagram-oriented wording |
| **F1** NACK uncorrelatable to a pipelined intent | [§16.1](errors.md#s16-1) `intent_seq` |
| **F2** `cfg_gen` bumped on no-op writes; never bumped machine-side | [§4.2-2](foundations.md#s4-2), both directions |
| **F3** sketch ids mistaken for allocations | [Appendix D](appendices.md#appendix-d) reserved-range banner |
| **F4** ownership leaked on five of six teardown paths | [§6.9](session.md#s6-9) equivalence rule |
| **F5** motion producers flying blind on limits | [§8.8](catalog.md#s8-8) field roles, framed MAY by [§9.6-4](channels.md#s9-6) |
| **F6** feasibility unpredictable from limits alone | [§9.6](channels.md#s9-6), work placed on the hub |
| **F7** STATE arriving before its decoder ring | [§6.4](session.md#s6-4) readiness gate |
| **F8** identity and capabilities only over HTTP | [§6.3](session.md#s6-3) `identity`; capabilities = catalog introspection |
| **F9** device log only over HTTP | [§16.2](errors.md#s16-2) log channel + replay exception |
| **F10** no session roster, no eviction verb | [§12.7](security.md#s12-7) |
| **F11** silent ownership conflict for a c2h producer | [§9.2](channels.md#s9-2) `SOURCE_CONFLICT` carve-out |
| **F12** rate doubling as burst depth | [§10.5](qos.md#s10-5) `burst` |
| **F13** segment scheduling by folklore | [§5.4](wire-format.md#s5-4) `max_future_schedule_ms` |
| **F14** shedding divergence between conforming hubs | [§10.4](qos.md#s10-4) normative table |
| **F15** idle sessions holding slots forever | [§6.6](session.md#s6-6) idle reaping |
| **F16** HOLD/PAUSE latched by nobody in particular | [§11.1](safety.md#s11-1) hub latches all four |
| **F17** unclear whether a watcher may stop the machine | [§11.2](safety.md#s11-2) role-exempt `stop`/`estop` |
| **F18** override/bypass with no in-band home | [§11.1](safety.md#s11-1) safety-domain modes |
| **F19** no way to see the machine's name | [§5.4](wire-format.md#s5-4) `str<N>` + [§6.3](session.md#s6-3) `identity` |
| **F20** pairing assumed a keyboard and a trusted display | [§12.3](security.md#s12-3) three association modes |
| **F21** decoder crashes reachable from unknown-key skip paths | [§5.8](wire-format.md#s5-8), [§17.4](conformance.md#s17-4) |
| **F22** no way to tell the real hub from a clone | [§12.5](security.md#s12-5) |
| **F23** paired identity trusted forever regardless of code | [§12.6](security.md#s12-6) tripwire (bounded by H6/H7) |
| **F24** bearer token sniffable on cleartext | [§12.4](security.md#s12-4) proof mode |

## Appendix J — What changed since v1-draft *(informative)* {#appendix-j}

For implementers of the draft. This is a summary; the reasoning lives in `RFC-QUEUE.md`.

**Wire changes (breaking, permitted because the draft was never public):**

- CATALOG_REQ `0x09` / CATALOG_CHUNK `0x0A` **retired and burned**; chunked transfer generalized into BLOB_REQ `0x1A` / BLOB_CHUNK `0x1B` with the catalog as **namespace 0** ([§8.4](catalog.md#s8-4), [§8.7](catalog.md#s8-7)).
- New frames: PUBLISH `0x18`, CATALOG_READY `0x19`, AUTH `0x1C`, HUB_SIG `0x1D`.
- New global CBOR keys 37–43: `identity`, `blob`, `trust`, `body`, `intent_seq`, `burst`, `reboot_in_ms` — plus five scoped sub-key spaces, under the conservation rule of [§5.3](wire-format.md#s5-3).
- **EVENT kind-specific fields moved into `body` (40)**, keyed by the channel's own catalog schema.
- New channel class **STORE (4)**; new packed field types **`str16`/`str32`/`str64`** (8/9/10); new catalog entry keys `category`, `category_label`, `replay_depth`, `setting_channel`, `stream_kind`, `store`; the whole [§8.8](catalog.md#s8-8) annotation block on layout and schema fields, including schema-field `access` and `option_access`.
- Spec-core channels added: `log`, `session-admin`, `pending-pairing`, `pairing-events`, `paired-devices`, `paired-devices-roster`, `safety-events`. `session-roster` implemented rather than merely reserved.
- New NACK/GOODBYE codes `DEADMAN_TIMEOUT`, `REBOOTING`, `READY_TIMEOUT`, `NOT_READY`; new safety cause `session_loss`; new safety ops `estop`, `override_on/off`, `bypass_on/off`.
- Per-binding `max_frame` defaults registered (the registry previously had none, so every binding invented one).

**Renames with unchanged wire values:** access tiers `viewer/controller/admin` → **`watch`/`control`/`configure`** (0/1/2).

**Behavioral changes:**

- The **dual-plane readiness gate** ([§6.4](session.md#s6-4)) — a session receives no data and may send no intents until it demonstrably holds the catalog.
- `cfg_gen` advances **iff an applied value actually changed**, in both directions ([§4.2-2](foundations.md#s4-2)).
- **Teardown equivalence** across all six session-end paths ([§6.9](session.md#s6-9)), with `session_loss` distinguished from `deadman`.
- Idle reaping promoted from MAY to SHOULD with a registered multiplier ([§6.6](session.md#s6-6)).
- The [§10.4](qos.md#s10-4) shedding table is normative, with the **segment exception**.
- Safety: the hub latches all four levels; `stop`/`estop` are role-exempt; override/bypass join the safety snapshot.
- **[§12.2](security.md#s12-2)'s "admin only via the hub's own UI" is struck** — `configure` is obtainable by ceremony, which makes the administration surface pairing-reachable by design.
- Segment scheduling semantics for `t_base` ([§5.4](wire-format.md#s5-4)) and `max_future_schedule_ms`.
- Parser totality ([§5.8](wire-format.md#s5-8)) and the fuzz gate ([§17.4](conformance.md#s17-4)) became conformance obligations for **both** roles.

**Rejected, numbers retained:** a separate stored-vs-effective field flag, and a reserved `machine-limits` channel id — both **superseded by the [§8.8](catalog.md#s8-8) mechanism**, where `setting_key` presence answers the first and `field_roles` answers the second, so the registry never grows two ways to express one thing.

**Fixture re-freeze:** the conformance mini-catalog is now **775 bytes** with etag **`F4 A2 8F BB 58 CE D1 6A`** (was 733 / `21 CB 26 C9 4F B3 88 B5`), re-frozen at this tag ([§17.2](conformance.md#s17-2)).

---

*End of SPEC.md — `slopsync/1`, document version v1.0.*
