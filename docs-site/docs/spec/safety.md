---
title: Safety
description: >-
  SlopSync clause 11: the stop taxonomy and safety snapshot, ESTOP end to end,
  the deadman, control arbitration, and the invariants under partial failure.
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

# 11. Safety *(normative)* {#s11}

## 11.1 The stop taxonomy and the safety snapshot {#s11-1}

Four distinct levels, all latched or gated in the `safety` STATE channel, all initiable via the `safety-intents` INTENT channel:

| Level | Meaning | Motion behavior | Clears by |
|---|---|---|---|
| **ESTOP** | Emergency stop, latched | Immediate driver-level stop; motion prohibited while latched | Explicit authorized clear ([§11.2](#s11-2)) |
| **STOP** | Controlled stop | Decelerate to zero at configured decel; source deactivated | Any new accepted motion intent from an authorized source |
| **HOLD** | Position hold | Decelerate, then actively hold position; source suspended | RESUME by an authorized session |
| **PAUSE** | Generator pause | The hub-autonomous generator suspends at a safe phase; position parked | RESUME |

**The hub latches all four levels.** Delegate/application acceptance is what triggers the latch; a hub whose application does not implement a level MUST NACK `UNSUPPORTED_OP` and latch **nothing**. That is discoverable and honest — without it, a generic client could not know whether sending HOLD to an arbitrary hub did anything at all.

The `safety` snapshot carries: the active level bits, `cause` (a `safety_causes` value: `user`/`deadman`/`fault`/`relay`/`session_loss`), the initiating `origin` tier, the owning `session_id` where applicable, `estop_seq`, and an appended **modes** bitfield carrying `manual_override` and `bypass_limits`.

**Override and bypass are safety-domain state.** They are written by `safety-intents` ops (`override_on`/`override_off`/`bypass_on`/`bypass_off`, all `control`) and read from the safety snapshot. They typically render near a machine's manual controls, but they are safety state and other surfaces need them; putting them anywhere else would have made "what is currently bypassed" a per-UI secret. A per-move bypass flag on a motion intent, where a hub offers one, is a separate and unaffected thing.

They are latched **modes**, not stop edges, and deliberately have no event kind: giving them one would imply an operator action that a hub-side reconciliation (an e-stop dropping override as a side effect) did not have.

## 11.2 ESTOP end-to-end {#s11-2}

- **Initiation:** any endpoint, any tier, any session state — including *no* session (a paired relay may originate). **Safety outranks authorization by design: you may always stop the machine; you may not always start it.**
- **Two initiation paths, one behavior:**
  1. the raw **ESTOP frame** ([§5.5](wire-format.md#s5-5)) — the deframed-path and relay guarantee, recognizable by a byte scanner without a session;
  2. the **`estop` op** on the `safety-intents` channel — the trivially-implementable client path. A hub MUST treat it **exactly as a valid ESTOP frame**: same latch, `cause = user`, same publish, same edge event. Implementations SHOULD dispatch it through the same function as the frame path, so that "exactly as" is true by construction rather than by a parallel implementation.
  Without path 2, a client's red button silently degrades to a decel-stop — a ground-truth violation on a machine where the difference matters.
- **Latch is the acknowledgement.** The initiator MUST repeat its ESTOP every `estop_repeat_interval_ms` (50 ms), up to `estop_repeat_max` (20), until it observes `safety` STATE with the ESTOP bit latched and `estop_seq ≥ ` its sent seq — or exhausts retries and surfaces a **loud local failure**. There is no ESTOP-ACK frame; the observable latch is the only acknowledgement that means anything. All repeats of one initiation carry the same `estop_seq` ([§5.5](wire-format.md#s5-5)).
- **Hub obligations:** on first valid ESTOP (CRC-checked), stop motion via the driver's e-stop path **before** any protocol bookkeeping; latch; publish `safety` STATE at critical priority to all subscribers; emit the `estop_latched` edge on the safety-events channel.
- **Relay obligation:** forward ESTOP ahead of all buffered traffic, immediately, on all attached segments ([§14.2](transports.md#s14-2)) — including *upstream* if relay-originated.
- **HONESTY CLAUSE (H2) — preemption scope.** "Jumps the queue" is a **per-hop** guarantee: each hop's transmit queue admits ESTOP at the front. It is not magic end-to-end latency — TCP bytes already in flight ahead of it still drain first. Worst-case added latency per binding is declared in [§13.1](transports.md#s13-1).
- **HONESTY CLAUSE (H1).** The **hardware** e-stop path remains the guarantee of last resort. SlopSync's ESTOP is a software convenience layered above it and MUST NOT be presented to a user as a substitute for it.
- **Clearing:** the `estop_clear` op requires `control`; the hub MUST refuse with `CLEAR_REFUSED` unless (a) the latched cause is resolved (deadman: the lost source is confirmed detached or re-owned; fault: the fault flag is gone), (b) motion is at zero velocity, and (c) no other stop level is pending escalation. **Clearing never restarts motion** — it only re-arms the ability to start. The `estop_cleared` edge says the latch is gone, never that the machine moved.
- **A hub MUST NOT let a catalog authoring error widen safety authorization.** The `control` floor on `estop_clear` (and on any op whose effect is to re-arm motion) is a hub obligation independent of what the hub's own catalog declares about it. The catalog is the *discovery* surface for per-op access; it is not the only enforcement point for the ops that can start a machine moving again.

## 11.3 Deadman {#s11-3}

The deadman binds to the **active motion source**, not to sessions in general ([§6.6](session.md#s6-6) gives the other regime).

- Every session that owns an active source has a deadman window: `deadman_ms`, default 600, clamped to `[deadman_min_ms, deadman_max_ms]` = 250–5000, negotiated at WELCOME. Silence beyond the window — no frame of any kind, [§6.6](session.md#s6-6) — fires the source's **loss policy**.
- **Initiator-bound sources** (a motion stream, a manual jog, a live remote): loss policy default **STOP** (decel). The machine must not continue executing a stream whose author is gone.
- **Hub-autonomous sources** (a pattern generator running on the hub): the generator runs *on the hub*; the vanished client was merely the finger that pressed start. Default policy **continue**, with ownership released so any authorized session may now stop, adjust, or take over. Configurable to STOP per hub setting. This is a deliberate product decision: a phone screen-lock must not interrupt a self-driving session, while a vanished *streamer* must stop motion in under a second.
- Deadman firing latches **STOP** (not ESTOP) with `cause = deadman`, and releases source ownership ([§11.4](#s11-4)).
- **Legacy edges get synthetic sessions with equivalent timeouts** ([§15.1](legacy.md#s15-1)). There is **no unmonitored path to motion.**

## 11.4 Control arbitration {#s11-4}

A machine's arbiter assigns priorities *between source types*. SlopSync adds the layer an arbiter cannot provide: arbitration *within* a type.

- **The sole-caller rule is a protocol obligation.** SlopSync sessions submit intents to the machine's motion arbiter, which is the only component permitted to command the driver. A hub that lets any session reach the driver by another path is non-conformant.
- **Exclusive ownership.** Each source has at most one owning session at a time, published in the `control-owner` STATE channel. The first authorized session to activate a source owns it; a second session's activating intent gets NACK `SOURCE_CONFLICT`.
- **STREAM channels mapped to a source** participate on the same machinery: the **first accepted bundle** acquires the source, each subsequent accepted bundle refreshes the deadman window ([§6.6](session.md#s6-6): any received frame is proof of life), and a bundle from a non-owner while the source is owned is dropped — with the [§9.2](channels.md#s9-2) `SOURCE_CONFLICT` signal so the producer is not left guessing. Data-plane bundles carry no takeover flag; a would-be taker acquires through an intent.
- **TAKEOVER:** re-issuing the activating intent with `takeover: true` (32) transfers ownership if the requester's tier ≥ the owner's. The hub emits a takeover EVENT and a `control-owner` STATE update; the dispossessed session's UI MUST reflect loss of control immediately — it is subscribed to the same channel as everyone else, so this requires no message addressed to it. Takeover *between* source types remains the arbiter's existing priority logic, unchanged.
- **Release** happens on every teardown path identically ([§6.9](session.md#s6-9)), on deadman fire, and on an explicit release intent. **Post-deadman reacquisition requires a fresh activating intent** ([§6.8](session.md#s6-8)) — never a silent resume.
- **Tiers gate the door.** Activating any source requires `control`. A `watch` session cannot own a source, full stop — but it can still stop the machine ([§11.2](#s11-2)).

## 11.5 Invariants under partial failure {#s11-5}

Whatever dies — a client, a relay, a transport, the network — all of the following MUST hold:

1. Motion driven by a vanished initiator-bound source stops within its deadman window.
2. The ESTOP latch, once set, survives every reconnect and is adopted by every arriving client **before it can act** — retained STATE plus the readiness gate ([§6.4](session.md#s6-4)) plus the LIVE gate ([§2.2](foundations.md#s2-2)) together guarantee this, which is why the readiness gate covers the intent plane and not only the data plane.
3. A relay's death makes its clients *silent*, which triggers the same deadman path as client death. The hub cannot distinguish them and does not need to.
4. No failure mode results in a client displaying motion as stopped while the machine moves, because displays render only adopted hub state and go visibly stale when the link dies.
5. Every session-end path releases ownership identically ([§6.9](session.md#s6-9)), so no departed session can hold a source hostage.
