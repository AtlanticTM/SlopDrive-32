---
title: Channel classes
description: >-
  SlopSync clause 9: STATE, STREAM, INTENT/ECHO, EVENT and STORE semantics,
  and the closed motion input surface.
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

# 9. Channel Classes *(normative)* {#s9}

## 9.1 STATE — the shadow {#s9-1}

STATE channels carry **idempotent full snapshots** of a coherent group of fields.

- **Full-snapshot rule:** every STATE frame contains the complete current value of its channel. There are no deltas in `slopsync/1` — a delta would make frame loss corrupting, destroying the property the whole design leans on.
- **MTU rule:** a STATE payload MUST fit `min_transport_payload` (242 B) unfragmented. This is a *catalog design constraint*: a state group that does not fit is split into multiple channels at catalog-design time (and, if they are settings, given the same `category` so they render as one tab — [§8.8](catalog.md#s8-8)). Conformance tooling SHOULD flag violations mechanically, since layout size is statically known.
- **Retained value:** the hub keeps the latest value of every STATE channel and MUST push it immediately upon grant — connect, re-subscribe, reconnect — subject only to the readiness gate ([§6.4](session.md#s6-4)). This is the device-shadow primitive; it is what "page load adopts device state" compiles to.
- **Conflation:** the hub maintains at most a depth-1 queue per (channel, subscriber) — a newer snapshot replaces a queued unsent one. Subscribers therefore see the freshest state their link can carry, never a backlog. Newest-wins by seq on receive ([§7.3](time.md#s7-3)).
- **Rate:** `granted_rate_hz` is a *ceiling* on push frequency. On-change channels (rate 0) push at most once per change, conflated. Periodic channels push at `min(grant, change rate)`.
- **Bitfields:** flag-word channels use `bitfield8` fields with catalog-enumerated bit meanings. A latched safety word is still a full snapshot like everything else.
- **First push after a grant is never shed** ([§10.4](qos.md#s10-4)). A subscriber's very first snapshot is what takes it from READY to LIVE; shedding it would strand the session.

## 9.2 STREAM — the data plane {#s9-2}

STREAM channels carry timestamped sample bundles ([§5.4](wire-format.md#s5-4)) in either direction: telemetry h2c, motion input c2h.

- **`stream_kind` says what a sample IS**, and everything else follows from it:
  - **`samples` (0, default)** — dense points reporting a value **at an instant**. A dropped sample is recoverable by interpolation from its neighbors. Decimable.
  - **`segments` (1)** — each sample **commands a time extent**: it carries its own duration and is not a point on a continuous curve. A dropped segment is a permanently lost **command**, not a recoverable interpolation gap. **Not decimable** ([§10.4](qos.md#s10-4)).
  This is an explicit registered property, not an inference. An earlier heuristic classified segment channels by looking for a time unit in the layout — but `unit` is a free-form string, so two conforming hubs could disagree (`ms` vs `msec` vs `millis`) and therefore **shed differently under identical congestion**, which is exactly the divergence [§10.4](qos.md#s10-4) exists to eliminate.
- **Ordering:** guaranteed only on ordered bindings. On datagram bindings the consumer rules of [§7.3](time.md#s7-3) — drop-not-newer, timestamp-driven consumption — are the whole contract. STREAM consumers MUST be written against the weakest line of the [§13.1](transports.md#s13-1) matrix.
- **No per-sample acknowledgements**, in either direction. At 333 Hz an ACK would be a storm; [§9.3](#s9-3) explains why motion *input* correctness does not need one.
- **Grants bound sample rate, not frame rate.** A 240 Hz grant delivered as ~48 fps × 5-sample bundles is conformant and expected. (At 240 Hz five samples span 16.7 ms; a sixth would exceed the 20 ms span cap — the caps interlock.)
- **Inbound (c2h) ingress validation.** A hub accepts a bundle only on a channel the sending session was granted as a publication ([§6.2](session.md#s6-2), [§6.7](session.md#s6-7)). A bundle on an unknown, ungranted, wrong-class, or wrong-direction channel is **silently dropped and counted**. Before acting on an accepted bundle the hub MUST re-validate the [§5.4](wire-format.md#s5-4) caps against its **own** catalog; a bundle violating any cap is dropped **whole**, never parsed part-way. A bundle from a session that is not yet ready ([§6.4](session.md#s6-4)) is likewise dropped and counted.
- **Two sanctioned NACK carve-outs.** STREAM is otherwise never NACKed, but silence is a bad answer to a client that is structurally broken rather than merely fast:
  1. **`RATE_LIMITED`** for sustained ingress overage ([§10.5](qos.md#s10-5)), throttled;
  2. **`SOURCE_CONFLICT`** on the first bundle dropped because another **live** session owns the source ([§11.4](safety.md#s11-4)), throttled the same way, once per (session, source). Without it, a producer whose source is owned by someone else is silently dead: every bundle dropped, zero wire signal. Producers SHOULD also subscribe the `control-owner` channel for the full picture.

## 9.3 INTENT / ECHO — the control plane {#s9-3}

INTENT is the only way a client changes anything. CBOR: `channel_id` (15) naming an INTENT-class channel, `intent_id` (18), `value` (20) per the channel's `schema`, optional `precondition` (30), optional `takeover` (32).

- **ECHO is mandatory and truthful.** The hub replies ECHO `{intent_id, applied (19), cfg_gen}` — or NACK. `applied` carries the **post-clamp values actually in effect**, which MAY differ from what was requested. The client's shadow updates from ECHO and the ensuing STATE broadcast, **never from its own request**. All *other* subscribers learn of the change via STATE; ECHO goes only to the sender. **ECHO is key-complete over what was applied:** `applied` carries every key from the intent's `value` map that the hub applied; a key **absent** from the ECHO means NOT applied, and the client MUST fall back to reported truth (the ensuing STATE) for it. This is what makes "silently accepted" and "silently ignored" distinguishable on every hub.
- **Idempotency:** `intent_id` is session-scoped, client-assigned, monotonically increasing. The hub keeps a ring of the last `idempotency_ring_depth` (32) `id → ECHO` pairs per session; a duplicate id re-emits the stored ECHO and MUST NOT re-apply. The ring dies with the session ([§6.8](session.md#s6-8)) — which is safe *because*:
- **Absolute values only.** Intent schemas MUST express target state ("set speed 400"), never operations on current state ("add 20"). A client wanting an increment computes the absolute target from its shadow and MAY guard against races with `precondition` = expected `cfg_gen`; mismatch → NACK `CONFLICT`, client re-reads and retries. This one rule is what makes the reconnect story ([§6.8](session.md#s6-8)) sound and two-operator racing merely annoying instead of corrupting.
- **Rate limiting:** hub-enforced per session, `intent_ingress_default_per_s` (50) by default; excess → NACK `RATE_LIMITED`. Generous for UIs, hostile to accidental loops. **Role-exempt safety ops are rate-limited too** ([§11.2](safety.md#s11-2)).
- **Streams are not intents.** High-rate motion *input* rides STREAM and is never echoed per-sample. Its observable truth is the position telemetry the hub publishes — you see what the machine actually did, which is the only truth that matters. Only discrete state changes ride INTENT.
- **Actions.** A schema field whose `role` is `action.<name>` is a **verb**, not a value: `action.home`, `action.reset_stats`. ECHO echoes the op. Two rules make an action observable rather than private to its sender:
  1. a resettable counter group's twin STATE channel carries a field tagged `meta.reset_gen`, incremented on every applied reset, so **all** subscribers observe the reset;
  2. **classification:** an action that restores *configuration* values bumps `cfg_gen` **and** `reset_gen`; an action that only clears *counters* bumps `reset_gen` alone.
- **Procedures.** A long-running guarded operation — a multi-step device programming sequence, a verified write-then-readback — cannot be expressed by intent-and-echo alone, because its real result arrives later. It is not a new frame type; it is a **documented catalog pattern**:
  - start it with an action intent; ECHO means **accepted**, not complete;
  - progress and outcome ride a twin STATE channel carrying `{procedure, phase, progress, result}`, where `phase` is a `procedure_phases` value (`idle`/`running`/`succeeded`/`failed`/`aborted` registered; 128+ device-defined intermediate steps that a client renders as `running` if it does not recognize them). Full snapshots make it reconnect-safe by construction;
  - completion also emits an EVENT;
  - **one procedure STATE channel per concurrently-runnable procedure** — full-snapshot semantics can represent exactly one;
  - **reboot-commit:** where accepting the intent commits by rebooting, the ECHO's `applied` map carries `reboot_in_ms` (43); the hub then GOODBYEs every session with `REBOOTING` before going down, and the changed `boot_id` tells returning clients what happened.

## 9.4 EVENT — edges, not levels {#s9-4}

EVENT channels carry discrete occurrences. CBOR: `event_kind` (33), `timestamp` (21), optional `seq_of_state` (34), and `body` (40) — a sub-map whose integer keys come from **the channel's own catalog `schema`**, exactly as INTENT's `value` does.

The `body` sub-map is what makes device-authored EVENT channels possible at all. With kind-specific fields at the top level, every device wanting an event channel would have needed a registry PR to name its own fields — the precise coupling the self-describing catalog exists to prevent. `event_kind` and `seq_of_state` stay at the top level because they are protocol framing, not payload.

- **Best-effort.** Events are conflated and bounded like everything else and are **NOT replayed on reconnect** — *except* where a channel's catalog entry declares a `replay_depth`, in which case the hub MAY replay up to that many entries from its ring tail when the channel is granted. The log channel ([§16.2](errors.md#s16-2)) is the sanctioned use; the exception exists so "what went wrong just before I connected" is answerable without making every grant a burst.
- **The event/state duality rule (safety-critical).** Any event a client could not afford to have missed MUST have a **latched STATE twin**: the event says "this just happened", the state says "this is (still) true". E-stop is the canonical pair — the safety-events channel for the edge, the `safety` channel for the latch. A reconnecting client adopts the latch and needs no history. **No safety behavior may depend on EVENT delivery.** Events are UX (toasts, logs, timelines); states are truth.
- **Edges are emitted on transitions only.** A repeated ESTOP frame re-broadcasts the STATE — that is [§11.2](safety.md#s11-2)'s only loss-recovery mechanism and it must keep working — but it does **not** re-emit the edge. An edge that did not happen is a lie.
- **Overflow:** per-subscriber event queues are bounded (`event_queue_depth_per_subscriber`, 16); overflow drops **oldest** and increments a visible `events_dropped` counter on the hub-status channel. There is exactly one home for that counter; a per-channel duplicate would drift.

## 9.5 STORE — collections {#s9-5}

STORE-class entries declare blob stores; their semantics are [§8.7](catalog.md#s8-7). A STORE entry carries no layout and no schema, is never subscribed, and never emits frames: its dynamic half is an ordinary STATE channel and its items move over the blob verb.

## 9.6 The motion input surface *(normative)* {#s9-6}

This section states, as protocol obligation, where kinematic work lives. It exists because the natural pull when a client sends bad motion is to make the client smarter — and for an ecosystem protocol that is a trap. Every kinematic rule pushed into clients is re-implemented subtly differently by every integrator, is unverifiable by the device, and is a reason not to adopt the protocol at all. It also cannot be right in general: a client cannot know the hub's planner shape, its live limit set, or its stroke window, and all three change at runtime.

1. **The motion input surface is CLOSED and small.** A hub accepts motion in exactly three modes: **native samples** (a `samples`-kind STREAM of dense points), **native segments** (a `segments`-kind STREAM of timed `{target, duration, end_velocity}` commands), and **TCode passthrough** ([§15.1](legacy.md#s15-1)). Everything a client does is adapting *its* source material into one of those three. Adding a fourth mode is a deliberate specification act, not something that accretes.
2. **Write-once rule.** If **every** conforming client would otherwise have to implement a given piece of kinematic work, that work belongs on the machine — written once, verifiable, identical for all clients. A client SHALL be able to send its content **as authored** within one of the three modes and receive good motion, with no feasibility analysis of its own.
3. **No per-client case logic on the motion plane.** A hub MUST NOT branch on **client identity** when planning or executing motion. If a hub appears to need such a branch, this specification is underspecified and the fix is a rule here, not a device-side special case. *Scope:* authorization is identity-branching by definition and is the named carve-out — tiers, the trust ledger and the served-page sideband are authorization. The **motion plane** stays identity-blind.
4. **Client-side feasibility adaptation is always OPTIONAL** — quality of implementation, never required for correctness. **No conformance test may demand it.**
5. **Carry intent, not pre-chewed motion.** Wire design prefers the sender's authored `{target, duration, end_velocity}` over a pre-rendered approximation. A hub can always degrade intent; it can never recover information the client threw away.

**Limits discovery is for display and optional pre-adaptation.** A hub SHOULD tag its kinematic ceilings and window bounds with `field_roles` (`limit.*`, `window.*`) so a client can find them on *any* hub without hardcoding a channel number. But the normative word for a client acting on them is **MAY, never SHOULD**: a client MUST NOT be required to reason about feasibility in order to produce good motion. Limits are shown to the operator; the machine's job is to play back whatever it is fed as well as it possibly can.

Note in particular that knowing `vmax/amax/jmax` is **not sufficient** to predict feasibility, because peak-versus-mean depends on the shape the hub plans. A minimum-jerk quintic over a chord `d` in time `T` peaks at `1.875·d/T` in velocity — a client applying the naive `d/T ≤ vmax` test concludes a stroke is fine when the profile actually needs 1.875× that. This is precisely why clause 4 exists and why clause 2 puts the work on the hub.

**Curve family declaration (segment streams).** A segment-class publish MAY declare a `curve_family` (key 45; registry `curve_families`: 0 `unspecified`, 1 `c1_cubic`, 2 `c2_quintic`, 3 `step`) on its `publishes`/`granted_publishes` entry map. The declaration rides HELLO **and** PUBLISH, so a sender switching interpolators mid-session renegotiates without dropping the stream. It exists because `{target, duration, end_velocity}` uniquely determines a cubic — a segment stream is a *complete* encoding of the sender's curve — but only if both ends agree on the smoothness class: a C2 quintic cannot reproduce a C1 cubic across a knot, because the script's acceleration genuinely steps there, and smoothing that corner erases something the author put there on purpose. The grant echo carries the **EFFECTIVE** family — the declaration after the machine's own curve policy — and MAY additionally carry `requested_curve_family` (48), the original wish echoed verbatim, so a client can see the honored-vs-downgraded fact directly instead of inferring it from what it remembers sending (RFC-049b; implementation: Phase D). The machine override outranks the declaration; `unspecified` MUST behave exactly as pre-declaration behavior, so the key is purely additive; an **unknown** family value is treated as `unspecified`, never parroted back. **No clamping semantics are implied by the family** — overshoot, feasibility and window legality stay the machine's existing machinery. **`step` (3) is `status: reserved`** (RFC-049a): the number is allocated and never renumbered, but no reference engine has a step renderer, so it is declarable and not yet actionable — [§18-20](limitations.md#s18)'s honesty note is now stated machine-checkably by the registry's `status` field, not only in prose.

**The client onramp (RFC-044, corrected 2026-07-27).** The onramp is ordered by how little an *existing* ecosystem client must change to reach a SlopSync hub at all, and its easiest rung is a **CLIENT-SIDE adapter, not a hub-side mode**. **TCode passthrough** means a client that already generates TCode pipes it through a small local shim — a reference implementation ships as a SlopDeck kernel module, with a C# helper planned for MFP-class apps — that translates the client's own TCode into native segments (or samples) locally, before anything reaches the wire. **The hub never parses TCode and no SlopSync channel carries it.** From the hub's side the adapted traffic is ordinary native motion, so the gain — a session's identity, deadman bookkeeping, source ownership and the safety taxonomy — comes for free with zero protocol surface. This is unrelated to [§15.1](legacy.md#s15-1)'s legacy text-edge synthetic-session mechanism, which stays the only place a hub itself ever sees TCode bytes, and only because those bytes arrive over a transport (serial, BLE-NUS) that was never a SlopSync frame to begin with. **Native segments** is the next rung, trading a small format change for `{target, duration, end_velocity}`'s deadline-honoring precision. **Native samples** is the dense-streaming rung a client graduates to only when it wants that. The strategy this encodes: SlopSync is meant to win by being the easiest protocol in the room to adopt, never by requiring a client to rewrite its motion pipeline before it is allowed to connect. A hub MUST NOT require a client to skip a rung to participate at all.

**Machine-side handoff sanity.** A hub that accepts an end-velocity with a scheduled successor SHOULD bound it against **both** adjoining chords, not just the current one: a pathological handoff is typically sane relative to its own span and absurd relative to the next. The reference bound is `|end_vel| ≤ k · min(|chord_in|, |chord_out|)` with `k = segment_handoff_k` (registry `limits`, 1.5 — the shape-preserving value, RFC-049c), where `chord_in` is measured from the machine's **actual** position rather than the sender's geometry. Pinning `k` in the registry, rather than leaving it reference-implementation-only, is what lets a second implementation match the reference's handoff shape without reverse-engineering it. Every bounded handoff SHOULD be surfaced — a counter, a log line, and an EVENT — so a client can *see* its content being reshaped. **HONESTY CLAUSE (H11):** this guard is lookahead-bounded. It can only act when the successor is already scheduled, i.e. when the current segment is shorter than the client's scheduling lookahead; a segment with no successor in hand is accepted unchanged, deliberately, because guessing a chord the hub does not have would trim well-behaved senders. The hub's own legality checks remain the backstop. A hub-side per-source scheduling-depth backstop that does not depend on client lookahead discipline is a named future direction (RFC-049c), not yet specified. See [§18](limitations.md#s18).
