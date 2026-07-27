# SlopSync RFC Queue — proposals targeting PUBLIC v1.0

*Companion to [SPEC.md](SPEC.md). This file is the accumulation buffer: when
implementation or field use surfaces something the spec got wrong, left
ambiguous, or never said, it gets an RFC entry here instead of an ad-hoc
patch. When enough have piled up, we review the queue in one sitting and
batch the accepted ones into the spec + registry.yaml (codegen + golden
vectors updated in the same commit, per CLAUDE.md §8 registry discipline).*

**RETARGET RULING (operator, 2026-07-25):** the current spec (v1-draft) was
the feasibility test — and it is feasible. The batch release this queue
feeds is therefore **public v1.0**, not v1.1. Entries written earlier that
say "v1.1" mean this same batch release. The base pass is the ENTIRE queue
(written as "001–026"; the queue had grown to **001–029** by the time it
landed): after it lands, RFCs should be few, small, flip-a-flag affairs —
never "rethink the core." **The batch landed 2026-07-26** — see the disposition
table below.

**Standing rulings recorded the same day:**
- **Breaking is allowed.** v1-draft was never public v1; the base pass MAY
  break existing SlopSync wire/code where a clean design beats a compat
  shim. The "frozen" conformance fixtures (mini-catalog, golden byte
  arrays) are regenerated once at the v1.0 tag and re-frozen THERE; the
  never-renumber rule binds from the v1.0 tag forward, not before it.
- **HTTP has exactly TWO permanent escapees: OTA and `/uitoken`**
  (amended 2026-07-25 after the feasibility pass surfaced the conflict).
  Goal state: "HTTP = static assets + OTA + uitoken, nothing else, ever."
  - **OTA** keeps its own token plane; OTA rights are NEVER derivable
    from SlopSync roles.
  - **`/uitoken`** (RFC-029 §4) escapes because its entire security
    property IS browser same-origin policy, which exists only over HTTP —
    it cannot be moved in-band without ceasing to work. Operator
    rationale: *it is a SIDEBAND, not a secondary cost* — a convenience
    for devices that happen to host a WebUI. A hub with no WebUI never
    implements it and loses nothing; no non-WebUI client ever needs it to
    connect; SlopSync's own surface is identical with or without it.
  - The distinction that makes these two different from `/api/log` and
    `/api/capabilities` (which are demoted to shims and deleted): those
    were carrying PROTOCOL DUTIES that belong in-band. These two carry
    duties SlopSync structurally cannot own.
- **Strings are required** (machine name and other info must be visible to
  clients). Identity/product strings ride the CBOR control plane where
  strings are already legal (RFC-016); string VALUES in packed STATE get
  fixed-width `str<N>` field types (RFC-026, resolving RFC-009's sub-
  decision 7 to option (a)). STREAM sample layouts stay string-free.
- The WebUI is the readiness yardstick — see
  [V1-READINESS.md](V1-READINESS.md) for the coverage ledger. Readiness is
  proven by coverage, not by migrating the UI now.

**Rules of the queue**
- Nothing here is normative. SPEC.md + registry.yaml remain the only wire
  truth until an RFC lands.
- Entries are append-only and numbered once — a rejected RFC keeps its
  number with Status: Rejected (so "why didn't we…" has a findable answer).
- Every entry names its origin (fw version / probe run / client) — proposals
  born from measured behavior outrank aesthetic ones.
- Statuses: **Draft** → **Accepted** / **Rejected** → **Landed (v1.0)**.
  Two honest qualifiers were needed at the v1.0 batch and are now part of the
  vocabulary: **Partially landed** (some sub-items shipped, others named and
  deferred) and **Deferred** (accepted in principle, deliberately not built).
  Nothing is marked Landed that is not actually in the tree.

---

## v1.0 BATCH DISPOSITION (2026-07-26)

*The whole queue was reviewed against `registry.yaml` AND against
`lib/slopsync/` while rewriting SPEC.md into public v1.0. This is the summary;
each entry's own Status line carries the receipts.*

| Disposition | RFCs |
|---|---|
| **Landed (v1.0)** — fully | 001, 002, 004, 005, 008\*, 009, 010, 011, 012, 013, 014, 015, 017, 021\*, 022\*, 023\*, 024, 025, 026, 027, 028\*, 029\* |
| **Partially landed** — named halves deferred | **016** (WELCOME `identity` codec), **018** (`0x0002` session-roster channel), **019** (reset action intent), **020** (spec + registry only; nothing emits it) |
| **Deferred** — accepted need, deliberately not built | **007** (planner-shape advert) |
| **Rejected** — superseded by RFC-009's mechanism, numbers retained | **003**, **006** |

\* carries a named deferred sub-item or an honest scope caveat inside its own
Status line — 008 (TCode passthrough mode), 021 (device preset backends),
022 (item 6 is unrepresentable rather than enforced), 023 (reference hub sheds
STATE only), 028/029 (default crypto stubs sign/verify).

**The deferred ledger, one line each — nothing here is marked landed:**

1. **RFC-007** — no planner-shape/ratio advert exists. RFC-008 resolved the
   same problem the other way (work moved to the hub), leaving an advisory
   field with no required consumer.
2. **RFC-016(a)** — WELCOME `identity` (key 37) is registered and specified but
   has **no codec**; `fw_version` therefore still has no in-band answer.
3. **RFC-018 roster** — `0x0002 session-roster` is allocated and specified but
   **no catalog builder declares it**. The registry note claiming
   "IMPLEMENTED at v1.0" is drift.
4. **RFC-019 reset verb** — `action.*` and `meta.reset_gen` shipped; no hub
   exposes a reset as an INTENT.
5. **RFC-020 procedures** — pattern, `procedure_phases`, `reboot_in_ms` (43)
   and `REBOOTING` all specified; **zero** implementations, and key 43 appears
   nowhere outside registry comments.
6. **RFC-021 device stores** — the blob/STORE mechanism ships (the trust ledger
   uses it); no `pattern.*` preset backend exists.
7. **RFC-008 TCode passthrough** — one of the three sanctioned motion modes, not
   implemented on the reference firmware.
8. **Real ECDSA** (RFC-028/029) — `ICrypto` is a working seam with a null-object
   default whose `signP256`/`verifyP256` are stubs. Hub authenticity exists only
   where an application injects a real primitive.

All eight are also recorded in SPEC §18 "Known limitations at v1.0", which is
the copy a third-party implementer reads.

---

## RFC-001 — NACK cannot be correlated to a specific in-flight intent

- **Status:** **Landed (v1.0).** `intent_seq` is CBOR key 41 (SPEC §16.1).
  Implemented MORE broadly than proposed: the reference hub stamps it centrally
  in `sendNack`/`sendNackTracked` from the seq of whatever frame is being
  dispatched, so essentially every NACK it emits carries one — not only
  intent-provoked NACKs. The "clients MUST tolerate its absence" half stands and
  is now covered by §4.3 tolerance.
- **Origin:** slopsync-js core build (fw 2.1.45, 2026-07-24). Found while
  implementing browser-side intent promises.
- **Problem:** The NACK payload carries `code`, `channel_id`, `detail`,
  `retry_after_ms`, `precondition` — but no intent id. A client with more
  than one intent in flight on the SAME channel cannot know which one was
  rejected. slopsync-js works around it by rejecting the oldest pending
  intent on the NACK's channel; correct for one-at-a-time UIs, wrong the
  moment anyone pipelines.
- **Proposed change:** Add an optional CBOR key `intent_seq` (the seq of the
  frame being NACK'd) to the NACK payload. Hubs SHOULD populate it whenever
  the NACK was provoked by a specific inbound frame. Clients MUST tolerate
  its absence (v1.0 hubs).
- **Compatibility:** Additive (new optional key from the registry's NACK key
  space). No renumbering. Golden vectors gain one NACK-with-seq fixture.

## RFC-002 — `cfg_gen` bumps on value-identical config-sets

- **Status:** **Landed (v1.0).** SPEC §4.2-2. Reference hub bumps only when an
  applied value actually changed; an accepted but value-identical write still
  gets its post-clamp ECHO and does not bump, and does not re-arm on-change
  republish. Landed jointly with RFC-011 as ONE two-directional rule, because
  either half alone leaves `precondition` CAS lying.
- **Origin:** The 2026-07-24 dual-plane config storm (fw 2.1.45,
  docs/webui-legacy-diagnosis.md layers 1–3). The wire-side accomplice: an
  accepted config-set bumps `cfg_gen` even when every applied value is
  unchanged, which re-arms on-change publications and legacy resync cycles.
  The browser-side loop is fixed, but the spec let the wire amplify it.
- **Problem:** SPEC does not say whether `cfg_gen` advances on *accepted
  writes* or on *effective state changes*. The reference hub does the
  former; every observer treating `cfg_gen` as "config changed, go resync"
  does redundant work for no-op writes.
- **Proposed change:** Specify: `cfg_gen` MUST advance only when at least
  one applied configuration value actually changed. A value-identical
  accepted intent still gets its post-clamp ECHO (ground truth is
  unaffected) but MUST NOT bump `cfg_gen` nor trigger on-change STATE
  republish.
- **Compatibility:** Behavioral tightening, no wire-format change. Client
  code that tolerates spurious bumps keeps working. Hub change + new SI test
  (set same value twice → one bump).

## RFC-003 — STATE channels must declare stored-config vs effective-state semantics

- **Status:** **REJECTED — superseded by RFC-009** (number retained per the
  queue's own rule, so "why didn't we add a stored/effective flag?" has a
  findable answer). The DISTINCTION is real and is now normative (SPEC §8.8);
  the MECHANISM is `setting_key` PRESENCE, not a separate flag. A layout field
  carrying `setting_key` is stored config; a field without one is
  effective/telemetry and MUST NOT be adopted into a setting's shadow. What was
  worth rejecting is the registry growing two ways to express one thing.
- **Origin:** Live write-plane verification (fw 2.1.45): on an unhomed
  machine, config-set window ECHO returns the STORED config (e.g. [5,495])
  while machine-config 0x0081 STATE publishes the EFFECTIVE window
  ([0,max_rail]) — legitimately different values, both true. slopsync-js
  initially misadopted effective as stored and stomped fresh operator input
  (diagnosis doc, layer "window doesn't stick").
- **Problem:** The spec has no vocabulary for this distinction. A client
  reading a STATE channel cannot know whether a field is a setting (adopt
  into controls) or a derived effective value (display as machine truth,
  never write back into the setting's shadow).
- **Proposed change:** Add a per-field (or per-channel) semantic flag to the
  catalog layout entry — `stored` vs `effective` — and one normative
  paragraph: ECHO always confirms stored config; STATE fields marked
  effective may lawfully differ; clients MUST NOT adopt effective fields as
  setting values.
- **Compatibility:** Catalog schema addition (catalog.cddl + etag bump on
  devices that adopt it — fine; the FROZEN conformance mini-catalog is
  untouched until the flag is versioned in properly at 1.1).

## RFC-004 — Appendix D sketch collides with real device allocations

- **Status:** **Landed (v1.0)** — editorial, done in the v1.0 rewrite. SPEC
  Appendix D is rebuilt on ids from the RESERVED range `0x8000-0xFFFF`
  (`0xEE00+`), which no conforming hub may ever allocate, under an explicit
  "EXAMPLE ONLY — NEVER ALLOCATE THESE IDS" banner that records this RFC's own
  origin as the reason. `examples/session-traces.md` E1-E5 moved to the same
  ids. Spec-core ids in the traces are deliberately unchanged: those ARE real
  allocations and using them is correct.
- **Origin:** fw 2.1.42 authoring of motion-input: Appendix D sketches
  0x0081 as "motion-input", but this device had already spent 0x0081 on
  machine-config; the real allocation is 0x0084. The catalog is
  self-describing and authoritative (Appendix D's own disclaimer), but the
  sketch reads like an assignment and has now misled once.
- **Proposed change:** Rework Appendix D examples to use ids from a clearly
  fictitious range (or an explicit "EXAMPLE ONLY, never allocate these"
  banner), so no sketch id can be mistaken for a registry assignment.
- **Compatibility:** Editorial only. No wire impact.

## RFC-005 — Specify hub teardown equivalence for all session-end paths

- **Status:** **Landed (v1.0).** Promoted to a numbered normative rule: SPEC
  §6.9 "Teardown: one path, six doors" — every session-end path (GOODBYE,
  transport loss, slow-consumer eviction, admin eviction, slot reuse, idle
  reaping, READY timeout, deadman) runs the same §11.3 loss policy,
  unconditionally and independently of HOW the end was detected.
  `safety_causes::session_loss` (4) now distinguishes a closed browser tab from
  a deadman TIMEOUT, which was previously misreported to every subscriber.
  SI-11/12/13 are the behavioural vectors, and SPEC §17.3 makes back-to-back
  sessions with no restart between them a REQUIRED test pattern.
- **Origin:** Field bug #3 (source-ownership teardown leak): ownership was
  released only by the deadman pump; GOODBYE, rude detach, evictions and
  same-slot re-HELLO leaked a dead session's ownership until reboot.
- **Problem:** v1.0 describes the deadman's loss policy but only §6.8 now
  gestures at the other five teardown paths. The invariant deserves
  normative statement: *every* way a session can end runs the same §11.3
  loss policy ("no unmonitored path to motion").
- **Proposed change:** Promote to a numbered normative rule: session
  teardown (GOODBYE, transport loss, either eviction, slot reuse, deadman)
  MUST be behaviorally identical w.r.t. source ownership and safety
  latching. Reference implementation: `teardownSession()`; conformance:
  SI-11/12/13 become spec-cited test vectors.
- **Compatibility:** Normative clarification of already-shipped behavior.

## RFC-006 — Motion-producing clients have no portable way to learn the machine's kinematic limits

- **Status:** **REJECTED — superseded by RFC-009** (number retained per the
  queue's own rule). The NEED is met; this entry as a separate change is not
  taken. Option (a) — a reserved `machine-limits` channel id — was never
  adopted, because a fixed channel number is exactly the coupling the
  self-describing catalog exists to prevent. Option (b)'s MECHANISM landed
  INSIDE RFC-009 as the registry's `field_roles` vocabulary (`limit.user.*`,
  `limit.input.*`, `window.min|max`, `telemetry.*`), so a client locates limits
  on ANY hub by role rather than by id. The framing correction from RFC-008 is
  normative in SPEC §9.6: limits discovery exists for DISPLAY and OPTIONAL
  pre-adaptation, and the word is MAY, never SHOULD.
- **Origin:** MFP plugin v0.2.1–v0.2.3 (2026-07-25), measured against slopsim
  with the real engine. A funscript axis using MFP's default **Makima**
  interpolation produced a handoff velocity of **1.816 norm/s into a span
  whose own mean velocity is 0.050 norm/s — 36×**. SlopMotion's legality scan
  rejected the quintic, the Ruckig guard took it, and a "slow, simple" script
  rendered as straight-line strokes with flat-topped velocity. The plugin was
  computing a *mathematically correct* spline tangent and shipping it to a
  machine that could not possibly honour it.
- **Problem:** A client that PUBLISHES motion (0x0084 motion-input, 0x0085
  motion-segment) is flying blind. Three distinct gaps:
  1. **No normative obligation.** SPEC tells a client how to send samples and
     segments but never says a motion producer SHOULD learn what the machine
     can do, so the reference client (this plugin) shipped publish-only wishes
     in HELLO and never subscribed to anything.
  2. **No portable discovery.** This device advertises geometry and ceilings
     on `0x0081 machine-config` (window min/max, user + input speed/accel,
     max_rail, and input_jerk since fw 2.1.47) — but 0x0081 is in the
     **device's own ≥0x0080 allocation**, not the registry's reserved range.
     A generic client cannot find "the kinematic limits" on an arbitrary hub;
     it would have to hardcode this device's catalog, which is exactly the
     coupling the self-describing catalog exists to prevent.
  3. **Limits are window-relative.** The ceilings that matter to a stream are
     normalized by the stroke window (`vmax_norm = input_speed / span`), so
     they change whenever the window changes. A one-shot value in WELCOME
     would go stale; this genuinely wants a STATE channel.
- **Proposed change:** Make kinematic limits portably discoverable, either by
  (a) a reserved registry channel id for `machine-limits` that any hub driving
  a physical actuator SHOULD publish, or preferably (b) **catalog field
  roles** — an optional per-field semantic tag in the layout entry (e.g.
  `role: limit.speed | limit.accel | limit.jerk | window.min | window.max`)
  so a client can locate the limits on *any* device's catalog without knowing
  its channel numbering. (b) composes with RFC-003's stored/effective flag —
  both are per-field semantics the catalog currently cannot express.
  **Framing corrected by RFC-008:** limits discovery exists for DISPLAY and
  OPTIONAL pre-adaptation. A client MUST NOT be required to reason about
  feasibility in order to produce good motion — the hub owns that. Normative
  language here is MAY, never SHOULD.
- **Compatibility:** Additive. Option (b) is a catalog schema addition
  (catalog.cddl + etag bump on adopting devices; the FROZEN conformance
  mini-catalog stays untouched until 1.1 versions it in). Clients that ignore
  the tags behave exactly as today. Note for implementers: adding a
  `subscribes` wish to a client's HELLO changes its HELLO bytes, so
  `tools/slopsync_probe.py` and any golden-byte mirror (the MFP plugin's
  `WireSelfTest.cs`) must move in lockstep — that coupling is why the plugin
  fixed its own tangent geometrically (Fritsch–Carlson bound, k = 1.5) rather
  than reaching for limits it could not portably obtain.

## RFC-007 — Feasibility cannot be predicted without the hub's planner shape

- **Status:** **DEFERRED — not implemented, not registered.** No planner-shape
  advert (a `min_jerk_quintic` enum, or the `(kv, ka, kj)` peak/mean ratios)
  exists in registry.yaml or in any implementation. Reason, recorded honestly:
  RFC-008 landed the opposite resolution of the same problem. Once feasibility
  reasoning is explicitly OPTIONAL for clients (SPEC §9.6-4) and the concrete
  pathology is bounded machine-side by the handoff guard, an advisory advert has
  no required consumer — and a wire field nobody must read is how a registry
  accumulates dead weight. The MATH is preserved as prose in SPEC §9.6 (a
  min-jerk quintic peaks at 1.875·d/T) precisely so an implementer understands
  why the naive `d/T <= vmax` test is wrong, without the protocol growing a
  field for it. Revisit if a real client wants to pre-adapt and demonstrably
  cannot.
- **Origin:** Same investigation. Even a fully limits-aware client could not
  have predicted the failure in RFC-006.
- **Problem:** Knowing `vmax/amax/jmax` is not sufficient to decide whether a
  `{target, duration}` segment is executable, because that depends on the
  *shape* the hub plans. SlopMotion renders timed segments as a C2 min-jerk
  quintic, whose peak velocity is `1.875·d/T`, peak accel `5.7735·d/T²` and
  peak jerk `60·d/T³`. A client applying the naive `d/T ≤ vmax` test concludes
  a stroke is fine when the actual profile needs **1.875×** that — measured
  live: a 140 mm stroke in 167 ms needs a mean of 838 mm/s (comfortably under
  a 1000 mm/s machine) but a quintic peak of 1572 mm/s (57 % over). The hub
  handles it correctly, but the client cannot warn, adapt, or choose a
  friendlier duration, and the operator sees unexplained shape changes.
- **Proposed change:** Let a hub advertise its waveform profile cost — either
  a named profile enum (`min_jerk_quintic`, `double_s`, `trapezoid`, `linear`)
  or, more robustly, the three dimensionless peak/mean ratios `(kv, ka, kj)`
  so a client can evaluate `d ≤ min(vmax·T/kv, amax·T²/ka, jmax·T³/kj)`
  without hardcoding any planner's internals. Ratios are preferable: they stay
  meaningful if a hub changes planners, and they are what a client actually
  needs to compute. Place alongside the RFC-006 limits.
- **Compatibility:** Additive and purely advisory — the hub remains the sole
  authority on what it will execute, and a client that ignores the hint gets
  today's behavior (hub clamps/reshapes and reports the anomaly). Encourages
  senders to pre-adapt rather than relying on machine-side rescue, which is
  strictly better for feel: a client that shortens its own stroke keeps its
  authored timing, whereas machine-side rescue has to guess.

## RFC-008 — DOCTRINE: the machine owns motion processing, not the client

- **Status:** **Landed (v1.0)** as normative doctrine — SPEC §9.6 "The motion
  input surface": the CLOSED three-mode list, the write-once rule, the
  identity-blind motion plane (scoped by the feasibility pass so authorization
  remains the named carve-out), the MAY-never-SHOULD framing of limits
  discovery, and the machine-side handoff sanity guard with honesty clause H11.
  **One named sub-item stays DEFERRED:** TCode passthrough is one of the three
  sanctioned modes but is NOT implemented on the reference firmware (TCodeParser
  cross-task race, no consumer value at the time). It is specified as a mode,
  not shipped as one.
- **Origin:** Operator ruling, 2026-07-25, after the MFP plugin needed a
  Fritsch–Carlson handoff limiter (v0.2.3) to stop Makima tangents driving
  the engine infeasible: *"I intend the machine to handle all of this, bare
  minimum motion processing on the streamer plugin side. I want a dev to WANT
  to implement this, not dread it."*
- **Problem:** The natural pull when a client sends bad motion is to make the
  client smarter. That is a trap for an ecosystem protocol. Every kinematic
  rule pushed into clients is (a) re-implemented, subtly differently, by every
  integrator, (b) unverifiable by the device, (c) a reason not to adopt
  SlopSync at all. It also cannot be right in general: a client cannot know
  the hub's planner shape, its live limit set, or its window — and those
  change at runtime. Today the MFP plugin carries a spline-tangent limiter
  that is really the *machine's* job.
- **Proposed change:** State a normative doctrine in SPEC:
  1. **The motion input surface is CLOSED and small.** A hub accepts motion in
     exactly three modes: **native samples** (0x0084 dense points), **native
     segments** (0x0085 timed `{target, duration, end_vel}`), and **TCode
     passthrough** (v4, with v3 covered by v4's backwards compatibility).
     Everything a client does is adapting ITS source material into one of
     those three. Adding a fourth mode is a deliberate spec act, not something
     that accretes.
  2. **Write-once rule.** If EVERY conforming client would otherwise have to
     implement a given piece of kinematic work, that work belongs on the
     machine — written once, verifiable, and identical for all clients. A
     client SHALL be able to send its content **as authored** within one of
     the three modes and receive good motion, with no feasibility analysis of
     its own.
  3. **No per-client case logic on the hub — this is the hard line.** The hub
     MUST NOT branch on who is talking or on a client's quirks. If a hub ever
     needs such a branch, the specification is underspecified and the fix is a
     spec rule, not a device-side special case. (Corollary: the hub cannot be
     expected to know a client's spline type, source format, or scaling — so
     the wire must carry the client's *intent* in spec units, and the client
     is responsible for that translation and nothing more.)
  4. Client-side feasibility adaptation is always OPTIONAL
     (quality-of-implementation), never required for correctness. No
     conformance test may demand it.
  5. Corollary for wire design: prefer carrying the sender's INTENT
     (`{target, duration, end_vel}` as authored) over pre-chewed motion. The
     hub can always degrade intent; it can never recover information the
     client threw away.
- **Concrete first consequence — machine-side handoff sanity:** the
  pathological input that motivated this (an end velocity 36× the next span's
  mean speed) is catchable *on the hub*. The pacing ring already holds segments
  scheduled up to ~120 ms ahead, so the engine can look one segment forward and
  bound an accepted `end_vel` against the FOLLOWING segment's chord — the same
  Fritsch–Carlson bound the plugin now applies, applied where it belongs.
  (`end_vel` bounding against the *current* segment's chord alone is NOT
  sufficient: the measured pathology was sane relative to its own span and only
  absurd relative to the next one.)
  **LANDED — milestone M4d, fw 2.1.53 / slopmotion 0.7.0.**
  `slopmotion::boundHandoffVelocity` is the bound; `Command::next_chord` /
  `has_next_chord` is the lookahead; `SlopSyncHubService::drainMotionStream`
  supplies it from `PacingRing::peekOldest()` at the latest possible moment
  before the command crosses to Core 1. `chord_in` is measured from the
  machine's ACTUAL position (`|target − p| / T`), which is better ground truth
  than any sender's script geometry. Every bounded handoff is a
  `HandoffBounded` (kind 8) anomaly: SlopLog `motion` tag via the existing
  Core-1 drain, a per-kind counter on 0x0088 `anom_handoff_bounded` and in
  `GET /api/slopmotion`, and an EVENT on 0x0089 — so a client can SEE that its
  content is being reshaped. `handoff_k` (POST /api/slopmotion, 0 = off) is the
  live A/B switch. Verified: 561,599 new property-sweep assertions, all six
  `slopmotion_traces` scenarios byte-identical (the guard cannot fire without a
  lookahead, so no existing motion changed), and an end-to-end run against
  slopsim producing `event_kind=8 target=0.900 detail=0.0083`.
  **Two honest limits of the landed guard**, recorded so nobody re-discovers
  them: (a) the TAIL CASE — a segment with no successor in the ring is accepted
  unchanged, deliberately, because guessing a chord we do not have would trim
  well-behaved senders and the segment is already DUE; the legality scan +
  Ruckig guard remain the backstop they always were. (b) COVERAGE is bounded by
  how far ahead the client schedules: the successor must already be in the ring
  when its predecessor comes due, i.e. the current segment must be shorter than
  the client's scheduling lookahead (MFP: 120 ms; the wire's own `t_off` clamp
  allows up to 250 ms). That correlates usefully with the pathology — an
  oversized Akima tangent implies a STEEP current chord, and a steep chord over
  a bounded displacement is a SHORT segment — but it is a correlation, not a
  guarantee, and raising the client's lookahead widens it.
- **Compatibility:** Doctrine + hub-side behaviour; no wire change. Existing
  clients get strictly better motion. The MFP plugin's v0.2.3 limiter stays
  for now as a bench-testing stopgap and is flagged in-code for removal once
  the hub-side guard lands.
- **Test of the doctrine (use this when reviewing any future proposal):** ask
  *"would every conforming client have to write this?"* If yes → machine. Ask
  *"does this depend on which client, or on that client's source format?"* If
  yes → client, and if the hub seems to need it, the spec is missing a rule.
  The handoff-sanity guard above passes both tests: any client emitting
  segments can produce an unreachable handoff, and bounding it needs nothing
  about who sent it.
- **Known open mode:** TCode passthrough is currently DEFERRED on this
  firmware (TCodeParser cross-task race, no MFP value at the time). It is
  named here as one of the three so it is understood as a planned part of the
  closed surface rather than a future fourth mode.

## RFC-009 — Settings metamodel: per-field catalog annotations for generic, self-building UIs

- **Status:** **Landed (v1.0).** The whole metamodel: SPEC §8.8 (annotation
  block, roles, categories, `meta.enabled_mask`, the secrets rule, hub-side
  validation with no client regex requirement, applied-within-advertised-range)
  and §8.9 (the normative rendering checklist, including grey-never-hide and
  mandatory generic fallback). `catalog.cddl` carries the annotation keys on
  both `layout-field` and `schema-field`; the registry gained
  `setting_categories`, `setting_flags`, `field_roles`, `desc_max_bytes`,
  `option_label_max_bytes` and `catalog_max_entry_bytes`. Sub-decision 7
  resolved to option (a) by RFC-026. Its own out-of-scope note became
  RFC-021.
- **Origin:** Design sessions 2026-07-25 (fw 2.1.47 era), operator goal
  statement: SlopSync is the machine's SOLE communication surface (HTTP serves
  static web assets, nothing else), and any client — WebUI, phone app, desktop
  app, hardware controller, streaming-client side panel — must build its entire
  settings/control surface from what the hub transmits. One-and-done clients:
  a control added in firmware populates on every client's next connect, with
  the label, grouping, and explanation coming from the hub. The user learns
  what a setting does from the hub's own description, not from the client
  developer. Receipts for the gap: slopsync-js adopting the effective window
  as stored config and stomping operator input (RFC-003's origin), and the MFP
  plugin flying blind on limits (RFC-006's origin) — both are instances of
  "the catalog describes values, not meaning."
- **Problem:** Five gaps block a generic settings renderer:
  1. **No STATE↔INTENT linkage.** Nothing machine-readable says "STATE field
     `user_speed` (0x0081) is written via INTENT key 3 (0x0101)" — the pairing
     lives only in the WebUI's hand-written JS.
  2. **No defaults.** min/max exist; the factory value does not.
  3. **No option labels.** A u8-backed single-select cannot name its choices.
     (Multi-select already works: `bitfield8` with catalog-enumerated bits.)
  4. **No categories or groups.** Nothing organizes channels/fields into a
     navigable settings surface.
  5. **No dynamic enabled state.** "Greyed out right now" depends on live
     machine state, so it cannot live in static metadata at all.
  Plus two second-order gaps: strings (packed layouts ban variable-length
  fields) and secrets (a WiFi password must NEVER ride a retained STATE
  snapshot that open-access viewers receive).
- **Doctrine line (binding for review of this and future proposals):** SlopSync
  describes what things ARE, never how they LOOK. No widget hints, no layout,
  no ordering metadata, no styling, ever. A phone renders a range as a slider,
  an OLED remote as a click-wheel value, a streaming plugin as a numeric box —
  same bytes, three honest UIs. Corollary: **nothing is hardcoded as a
  requirement; roles are hardcoded as opportunities** — a client that
  recognizes a spec-registered role MAY upgrade to a bespoke widget (position
  scope, dual-limit editor); a client that doesn't MUST fall back to generic
  rendering. Fallback is mandatory, upgrades are optional.
- **Proposed change:**
  1. **Per-field annotation block** (all keys optional; exact CBOR keys
     assigned in registry.yaml + catalog.cddl at landing, per the spec-gap
     ritual) on catalog layout fields:
     - `setting_key: u8` — the CBOR key in the paired INTENT channel that
       writes this field. **Present = setting (stored); absent = read-only
       (effective/telemetry).** RFC-003's stored/effective distinction falls
       out with no separate flag — 0x0081 `max_rail` (no config-set key) is
       the live worked example.
     - `default` — factory value, same type as the field.
     - `options: [tstr]` — labels for single-select (wire value = u8 index).
     - `group: tstr` — free-form card heading within the category tab.
     - `desc: tstr` — user-facing description/tooltip, cap 128 B/field
       (registry limit). Flash-resident on the hub; travels once, etag-cached.
     - `role` — RFC-006's semantic vocabulary, registry-governed, grown to
       include `telemetry.*`; the `<role>` + `<role>.peak` suffix convention
       pairs current/peak stats (client MAY render as one tile).
     - `step` — range granularity hint.
     - `flags` — `advanced`, `restart_required`, `secret`.
  2. **Two-tier categories, mirroring the channel-id range split:** per-entry
     `category: u8`. **0–127 spec-registered** in registry.yaml with name and
     canonical order (initial: `device`, `user`, `limits`, `tuning`,
     `diagnostics`) — consistent placement/iconography/translation across all
     clients. **128–255 device-defined**, hub MUST supply a `category_label`
     string; clients render these as additional tabs after the spec set. A
     category spans channels (`user` + `user-2` merge into one tab — the
     answer to a category outgrowing one 242 B snapshot).
  3. **Presentation order = authoring order.** Tabs: registry order, then
     device categories by id. Within a tab: channels ascending by id, fields
     in layout order. No ordering metadata on the wire.
  4. **Dynamic enabled:** a settings STATE channel carries `enabled_mask`
     bitfield8 field(s) in its own snapshot; bit i gates the i-th
     setting-annotated field of that layout. On-change push, retained,
     conflated — every client greys from the same ground truth.
  5. **Secrets rule (normative):** a `secret`-flagged field's value NEVER
     appears in STATE; the snapshot carries only a set/unset presence bit.
     Writes ride the paired INTENT normally; ECHO confirms application
     without echoing the value.
  6. **Validation is hub-side.** Constraints (min/max/step, `max_len`) are UI
     hints; the hub is the referee (NACK `INVALID_VALUE`). NO regex
     requirement on clients — an optional pattern hint MAY be included and
     MAY be ignored (a C5-class client must never need a regex engine).
  7. **Strings (sub-decision; preferred option first):** (a) new fixed-width
     padded `str<N>` packed field type(s) — register-map style, offsets
     static, append-only evolution preserved — for non-secret strings
     (device name, SSID); (b) secret strings use the presence-bit rule
     regardless. Rejecting (a) leaves string settings write-only with a
     presence flag, which lies to the UI about non-secret current values.
  8. **Normative rendering checklist** (what "compliant client library"
     means; spec text, not wire):
     - tabs = spec categories present (registry order) + device categories
       (hub labels);
     - cards = `group` strings in authoring order; ungrouped → default card;
     - widget chosen by type + constraints, never by hint (there is no
       widget field, deliberately): bool-u8→toggle, u8+options→select,
       bitfield8→checkbox group, numeric+min/max→slider, str→text,
       no setting_key→read-only display with unit;
     - disabled bit → **grey, never hide**;
     - `desc` → discoverable help affordance appropriate to the form factor;
     - writes show pending until ECHO; controls display APPLIED values only
       (ground-truth doctrine restated for settings);
     - unknown role/flag/annotation key → render generically (fallback
       mandatory, upgrade optional).
  9. **Settings are fixed per firmware** — enumerated at connect, never
     created or destroyed at runtime. This is catalog invariance (§8.6)
     verbatim: the set changes only with the etag, which is already the
     client resync trigger.
- **Limits & footprint (measured/derived, informing the numbers we tag):**
  - The three limit tiers MUST stay distinguished when tagging v1.1:
    **wire-frozen** (u16 channel ids ⇒ 32,640 device channels; 242 B STATE;
    bundle caps), **registry policy** (`catalog_max_entries` 256 is a
    conformance floor, not a wire cap — raisable by PR), **library knobs**
    (`Catalog32`'s 32 entries / 8 fields per entry are compile-time RAM
    constants; action item: make them template parameters `Catalog<N, F>` so
    a Wroom32D hub and an S3 pick their own sizes from identical code).
  - ~60 numeric settings fit one category snapshot (242 B); more = second
    channel, same category (see 2).
  - A lavish catalog (150 settings, full tooltips) ≈ 15–25 KB: flash-resident
    (rodata) on the hub, ~50–100 ms transfer over WS, ~0.5–2 s over BLE with
    MTU 247 + DLE, one time per firmware version per client, then etag-cached
    forever. Steady-state traffic and RAM are UNCHANGED by this entire RFC.
  - Consequence for §13 (BLE binding): SHOULD mandate MTU exchange + data
    length extension before catalog transfer; a client stuck at the legacy
    23 B MTU pays ~6–12 s once (acceptable, visibly SYNCING) or ships the
    §8.5 static profile like any other potato.
  - Targets ruling (operator, 2026-07-25): S3 is the standard and trivially
    fine; Wroom32D is a big goal (fits: catalog costs flash, which 32D has;
    its RAM squeeze is session-count/ring knobs, which are per-hub Tier-3);
    C5 is client/relay and uses the static profile — never downloads the
    catalog at all.
- **Compatibility:** Additive catalog schema change (catalog.cddl + etag bump
  on adopting devices; the FROZEN conformance mini-catalog untouched until
  versioned in at 1.1 — same posture as RFC-003/006). Registry additions:
  `setting_categories` table (+ device range rule), annotation keys, flag
  bits, `field_roles` growth, `desc` cap, optional `str<N>` packed types.
  Clients ignoring every annotation behave exactly as today. Depth check:
  entry map → layout array → field map → options array = 4, inside §5.3.
- **Out of scope, named so it isn't forgotten:** preset/saved-pattern
  management (list/save/load/delete named parameter sets) does NOT fit any
  existing channel class — a roster is a variable-length list of names
  (fights the 242 B full-snapshot rule) and enumeration isn't INTENT's shape.
  It wants its own small mechanism (compact roster STATE with count +
  generation, chunked fetch like the catalog, save/load/delete intents, a
  registry per-preset byte cap) and its own RFC. Sizing note from the same
  session: 32 pattern presets ≈ 1.5 KB NVS — the mechanism should not blink
  at 256.

## RFC-010 — Client-assertable E-STOP over SlopSync

- **Status:** **Landed (v1.0).** `safety_ops::estop` (6) on 0x0005, dispatched
  THROUGH the same handler as a valid 0xE5 frame, so "exactly as" is true by
  construction rather than by a parallel implementation. Wire-proven: latch +
  cause=user + estop_seq + critical-priority 0x0003 push. **The open sub-item is
  now CLOSED:** the "EVENT twin" that §5.5/§11.2 ask for has a registry home —
  spec-core channel `0x000E safety-events` with its own `safety_event_kinds`
  table (estop_latched / estop_cleared / stop_latched / stop_cleared),
  `critical` priority and `watch` access matching its STATE twin exactly, and
  emitted on TRANSITIONS ONLY. Because the op routes through the one function,
  registering the kinds fixed both paths at once, as predicted.
- **Origin:** 2026-07-25 coverage audit. `webui/src/main.js:239-243`: *"No
  slopsync 'assert e-stop' op exists … a hard e-stop stays on the legacy
  op."* `safety_ops` = clear/stop/hold/pause/resume — no assert; SlopSync
  `stop` maps to HALT (stays homed, `SlopSyncHubService.cpp:221-223`); the
  raw 0xE5 frame's WS binding + repeat-until-latch obligation is implemented
  by no client. Under sole-surface doctrine the red button silently degrades
  to a decel-stop.
- **Proposed change:** registry `safety_ops: 6 = estop` on 0x0005 — hub
  treats it exactly as a valid 0xE5 (latch, cause=user, publish, EVENT
  twin). Role-exempt together with `stop` (see RFC-025). The raw 0xE5 frame
  remains the deframed-path/relay guarantee; the op is the trivially-
  implementable client path.
- **Compatibility:** additive registry op + one normative paragraph.

## RFC-011 — Hub-side `cfg_gen` advancement (RFC-002's mirror twin)

- **Status:** **Landed (v1.0).** `Hub::bumpConfigGeneration()` exists and the
  firmware calls it for machine-originated changes. The unified rule is SPEC
  §4.2-2 and is deliberately stated in BOTH directions: no bump on a
  value-identical accepted write, and a MANDATORY bump on a change no client
  asked for.
- **Origin:** slopsim's own spec-gap ledger (`sim/slopsim/README.md:455-460`,
  `MachineSim.h:221-223`): `slopsync::Hub` has no bump API — `cfg_gen` moves
  only via intents. Same asymmetry in firmware. A machine-side config change
  (physical control, boot adoption, internal recalc) leaves the generation
  stale, so a client's `precondition` CAS passes against config that already
  changed.
- **Proposed change:** Hub gains `bumpConfigGeneration()`; unified normative
  rule combining with RFC-002: **`cfg_gen` advances iff at least one applied
  configuration value actually changed, regardless of who changed it.**
- **Compatibility:** behavioral + library API; no wire change.

## RFC-012 — Ownership signaling for c2h STREAM producers

- **Status:** **Landed (v1.0).** SPEC §9.2 gives "STREAM is never NACKed" an
  explicit `SOURCE_CONFLICT` carve-out, throttled like §10.5's RATE_LIMITED,
  once per (session, source). Takeover remains intent-only, deliberately —
  data-plane bundles carry no takeover flag and are not going to grow one.
  Producers SHOULD also subscribe `control-owner` for the full picture.
- **Origin:** `hub_impl.hpp:793-801`: data-plane bundles carry no takeover
  flag (§11.4) and are never NACKed (§9.2), so a producer whose source is
  owned by another LIVE session is silently dead — every bundle dropped,
  zero wire signal. (The stale-owner case is fixed by RFC-005/teardown; the
  two-live-clients collision is not.)
- **Proposed change:** hub sends NACK `SOURCE_CONFLICT` (carrying
  `channel_id`) on the FIRST dropped-for-ownership bundle per (session,
  source), throttled like §10.5's RATE_LIMITED NACK. Takeover remains
  intent-only, deliberately. Producers SHOULD subscribe `control-owner`
  0x0004 for the full picture.
- **Compatibility:** additive hub behavior; reuses existing code + key.

## RFC-013 — Publish grants: burst capacity + mid-session renegotiation

- **Status:** **Landed (v1.0).** Both halves. (a) `burst` is CBOR key 42 on
  `publishes` / `granted_publishes` ENTRY maps, defaulting to the granted rate,
  clamped into `[rate, rate x max_burst_multiple(4)]` and echoed like every
  other wish (SPEC §10.5) — so a sparse-but-bursty segment sender stops having
  to misrepresent its rate to admission control. (b) PUBLISH is frame `0x18`,
  sharing the grant path verbatim with HELLO so the two cannot drift; it answers
  with a GRANT even when nothing was granted, because an empty result IS the
  answer.
- **Origin:** MFP plugin measured (`SlopSync.cs:187-196, 341-355, 613-617`):
  §10.5 makes granted rate double as bucket depth, so a sparse-but-bursty
  segment sender (2–4/s mean, ~25/s peak) declares 30 Hz to buy burst budget
  — misrepresenting itself to admission control — and mirrors the hub's
  entire token bucket client-side (fails RFC-008's write-once test). Also
  `session.hpp:59-61`: no c2h counterpart to SUBSCRIBE — adding a publish
  wish mid-session requires a full reconnect.
- **Proposed change:** (a) `publishes` wish and `granted_publishes` echo
  gain optional `burst` (bucket capacity; default = granted rate, today's
  behavior). (b) New control frame PUBLISH (c2h, from the reserved type
  range) carrying a `publishes` array — mid-session add/change/drop,
  answered with grant results like SUBSCRIBE/GRANT.
- **Compatibility:** additive key + one new frame type. Break-allowed
  ruling permits assigning it a clean number now.

## RFC-014 — Timed-segment scheduling contract

- **Status:** **Landed (v1.0).** SPEC §5.4: for `segments`-kind STREAM channels
  `t_base + t_off[i]` IS the intended execution start of sample i, resolved
  through §7.2's nearest-window rule; registry limit `max_future_schedule_ms`
  (250), enforced by the reference hub as a CLAMP rather than a rejection;
  recommended client lookahead <= half of it. Interop by folklore is over.
  Landed together with the `stream_kinds` property RFC-023 needed anyway.
- **Origin:** `SlopSync.cs:624-630, 1056-1078` — the plugin schedules
  segment starts on `t_base` (§5.4 pins `t_off[0]`=0 and caps span at
  20 ms, so scheduling cannot ride `t_off`) against a hub future-clamp of
  250 ms that is registered NOWHERE, with a private `SegLookaheadMs = 120`
  constant. Interop by folklore.
- **Proposed change:** normative: for segment-class STREAM channels, bundle
  `t_base + t_off[i]` IS the intended execution start of sample i (resolved
  via §7.2 nearest-window). Registry limit `max_future_schedule_ms` (250);
  recommended client lookahead ≤ half of it.
- **Compatibility:** codifies shipped fw 2.1.45+ behavior.

## RFC-015 — SYNCING order: catalog completes before retained STATE

- **Status:** **Landed (v1.0).** CATALOG_READY is frame `0x19` (raw, c2h,
  payload = the 8-byte etag). SPEC §6.4 is the dual-plane gate: not ready means
  no STATE, no STREAM, no retained push — and inbound INTENTs are NACK'd
  `NOT_READY` (0x010B), never queued, so a client cannot act before adopting the
  retained safety latch (§11.5-2). `READY_TIMEOUT` (0x010A) plus
  `catalog_ready_timeout_ms` (15000) close the "PINGs happily, never adopts"
  hole that liveness reaping structurally cannot see. Both sides implemented,
  including the client's idempotent re-send at `catalog_chunk_gap_timeout_ms`
  terminating on the first STATE frame. Landed together with RFC-021: the
  transfer verbs are BLOB_*, and only the catalog namespace has a READY concept.
  `FALLBACK_LAYOUTS` can go.
- **Origin:** `webui/src/core/slopsync/catalog.js:11-13, 269-335` — nothing
  orders the retained-STATE push (§6.3) against catalog transfer (§8.4), so
  slopsync-js ships `FALLBACK_LAYOUTS`, a hand-copied table of THIS device's
  layouts, to decode state that arrives before the decoder ring — precisely
  the coupling the self-describing catalog exists to prevent.
- **Proposed change (operator-directed 2026-07-25: "ready tag" — reliable,
  non-blocking, no buffers, built into the LIBRARY, not client/firmware):**
  **CATALOG_READY.** The etag already makes the transfer self-verifying
  (SHA-256 over the exact bytes) — so the hash IS the acknowledgement:
  1. Per-session `ready` bit gates the ENTIRE data plane to that session
     (retained push + all STATE/STREAM emission). Not ready = nothing
     emitted. Nothing is queued or buffered anywhere — retained values
     already live once in the channel table; the gate is one flag, zero
     RAM, never blocks.
  2. HELLO with a MATCHING etag = proof of possession = ready immediately.
     The 99% reconnect case keeps today's zero-latency push.
  3. Absent/mismatched etag: WELCOME advertises the current etag; catalog
     chunks flow (and get the whole pipe — no telemetry competing, a free
     win on BLE/ESP-NOW); the client assembles, verifies the hash LOCALLY
     (zero round trips), then sends new raw c2h frame **CATALOG_READY**
     (core-reserved type from 0x18–0x3F; payload = the 8-byte etag it now
     operates against). Hub flips the bit; retained state flows; client
     reaches LIVE.
  4. Loss-proofing: READY is idempotent — the client re-sends every ~500 ms
     (chunk-repair cadence) until the first retained STATE arrives. No
     handshake state machine, no hub timer; a session that never sends
     READY just idles out under normal reaping (RFC-024).
  5. Degraded static clients (§8.5) send READY with their stale etag —
     append-only layouts make their prefix-parse safe; the hub serves them
     and MAY log the session as degraded.
  Rationale vs alternatives: hub-side "defer until transfer complete" is
  ambiguous (the hub knows it SENT chunks, not that they arrived — true on
  TCP, false on ESP-NOW); client-side "discard undecodable frames" wastes
  airtime shipping frames into a bin. The gate means undecodable state is
  never transmitted at all.
- **Compatibility:** one new raw frame type + session-layer behavior in
  hub AND client cores (firmware/JS/C# inherit it). Deletes
  `FALLBACK_LAYOUTS`. Break-allowed ruling: clean frame number now,
  re-frozen at the v1.0 tag.

## RFC-016 — In-band hub identity; capabilities = catalog introspection

- **Status:** **LANDED IN FULL (a+b+c; (a) closed 2026-07-27).** (b) LANDED and
  normative: capability discovery IS catalog introspection (SPEC §6.3) — a
  feature exists iff its channels exist, and there is no parallel capability
  list to drift. (c) LANDED: `fw version` is struck from 0x0006's registry
  note, so identity has exactly one home. **(a) LANDED with the RFC-030..040
  batch, promoted off the deferred ledger by the operator's HTTP ruling** ("a
  device does not need to support HTTP at all" — and fw_version had NO in-band
  answer, the poster child for a feature stranded in HTTP-land):
  `welcome.hpp` gained the `IdentityInfo` codec on key 37
  (product/fw_version/hub_name, emit-only-when-set so an identity-less
  WELCOME stays byte-identical), `Hub::setIdentity()` is the additive API
  (caller-owned rodata strings, no heap), and the firmware populates
  `("slopdrive-32", FIRMWARE_VERSION, "")`. Test SI-24. Honest remainder: the
  `info` (key 4) device-defined extras sub-map is still codec-less — decoders
  skip it per §4.3; register interest before building it.
- **Origin:** slopsim spec-gap ledger (`README.md:465-467`);
  `SlopSync.cs:395-397` labels devices `"boot 0x…"` because fw version
  exists only in mDNS TXT; `/api/capabilities` audit — feature gates and
  `fw_version` are HTTP-only.
- **Proposed change:** (a) WELCOME gains identity keys (CBOR — strings
  already legal): `product`, `fw_version`, `hub_name`, optional
  device-defined info map. (b) Normative sentence: **capability discovery
  is catalog introspection** — a feature exists iff its channels exist
  (`has_rs485` ⇔ servo channels present; ceilings ⇔ RFC-009 role-tagged
  limit fields). No parallel capability list to drift. (c)
  `/api/capabilities` demoted to legacy shim, deleted with the API plane;
  "where is SlopSync" bootstrap = mDNS / default port / same-host.
- **Compatibility:** additive WELCOME keys; shim removal is post-migration.

## RFC-017 — Device log channel

- **Status:** **Landed (v1.0).** Spec-core EVENT channel `0x0008 log`, fields on
  the `body` (40) sub-map, `log_levels` mirroring the firmware logger
  number-for-number (so the bridge is a cast, not a translation table),
  `background` priority, `watch` access, bounded drop-oldest with the §9.4
  visible counter. The backfill got a real rule instead of a MAY-shaped hole:
  `replay_depth` is a catalog ENTRY key and its PRESENCE is THE named exception
  to §9.4's no-replay rule (`log_replay_depth_default` 32). The serial-quiet
  handoff re-binds from "first HTTP GET" to "first log grant" (SPEC §16.2).
  Honest scope note: the reference hub keeps exactly ONE replay ring and gates
  it on the log channel id, so a device declaring `replay_depth` on another
  EVENT channel must wire its own — SPEC §18-6.
- **Origin:** slopsim ledger (`README.md:462-464`); `/api/log` audit
  including its side effect (first fetch triggers `applogSerialQuiet()`).
- **Proposed change:** spec-core EVENT channel `log` (reserved id, e.g.
  0x0008): `{level u8, tag, hub-ms, message ≤128 B}`, bounded drop-oldest
  with the §9.4 visible counter, `background` priority, viewer access.
  Backfill: on grant the hub MAY replay its ring tail (count declared in the
  grant). The serial-quiet handoff re-binds from "first HTTP GET" to "first
  log grant." `/api/log` lingers as a dev shim, then dies with the API
  plane.
- **Compatibility:** new spec-core channel id + registry entry.

## RFC-018 — Session roster + admin eviction

- **Status:** **PARTIALLY LANDED (v1.0) — the roster is DEFERRED.** LANDED:
  spec-core INTENT `0x0009 session-admin` carrying the whole admin verb space
  (`evict`, `pair_approve`, `pair_deny`, `revoke`) at `configure` access, with
  `evict` running the full §6.9 teardown because "no unmonitored path to motion"
  does not get an exception for admin actions, and with the operator ceiling
  ruling recorded (a configure session may grant up to its own tier; the audit
  trail is the paired-device roster, not a hard ceiling that would stop the
  first admin making a second). **DEFERRED: the `0x0002 session-roster` STATE
  channel is allocated and specified but NOT implemented** — no reference
  catalog builder declares it. The registry note on 0x0002 says "IMPLEMENTED at
  v1.0"; that is DRIFT, and the note overstates reality. Consequence: the
  property that offsets §9.4's no-replay rule — a late joiner learning existing
  sessions' names from a snapshot rather than from join events it missed — is
  specification, not shipped behaviour. SPEC §18-17.
- **Origin:** `/api/clients` audit: the Health-tab roster/kick enumerates
  legacy :81 slots; spec reserves `session-roster` 0x0002 but this device
  never implemented it, and no evict intent exists anywhere
  (`SESSION_EVICTED` is hub-initiated only).
- **Proposed change:** implement 0x0002 as packed STATE: generation +
  fixed-size slots `{session_id u32, role u8, flags u8}` (8 slots fits
  242 B with room); names resolve via 0x0007 join events (CBOR, carries
  `client_name`) so the roster stays string-free. New spec-core INTENT
  `session-admin` (admin role): `{op: evict, session_id}` → hub GOODBYEs
  the target with `SESSION_EVICTED`.
- **Compatibility:** implements a reserved id; one new spec-core intent
  channel. Admin remains hub-UI-granted only (§12.2 unchanged).

## RFC-019 — Action intents + observable resets

- **Status:** **PARTIALLY LANDED (v1.0) — the reset ACTION INTENT is
  DEFERRED.** LANDED: the op-style intent is a first-class catalog pattern,
  `action.<name>` is a registered `field_roles` CONVENTION carrying a
  device-chosen suffix (which is exactly why `role` is a tstr and not an enum),
  `meta.reset_gen` is a registered role, and the observable-reset rule plus the
  RFC-011 classification (an action that restores CONFIGURATION bumps cfg_gen
  AND reset_gen; one that only clears COUNTERS bumps reset_gen alone) are
  normative in SPEC §9.3. **DEFERRED: no reference hub exposes a reset as an
  INTENT** — the reference device's counter resets still ride their legacy HTTP
  keys and feed `reset_gen` from there. The vocabulary shipped; the verb did
  not. SPEC §18-18.
- **Origin:** coverage audit: `reset_stats`, `reset_peaks`, slopmotion
  `reset_stats`, `ap_reset` are ACTIONS, not values — RFC-009 is explicitly
  a value metamodel, and today three different reset verbs ride bespoke
  HTTP keys.
- **Proposed change:** formalize the op-style intent as a first-class
  catalog pattern (it already exists: `home`, `safety-intents`): an action
  is an INTENT schema field role-tagged `action.<name>`; ECHO echoes the
  op. Normative reset rule (ground truth for resets): a resettable counter
  group's twin STATE carries a `reset_gen` field that increments on every
  applied reset, so ALL subscribers observe the reset, not just the sender.
- **Compatibility:** additive; role vocabulary rides RFC-009's table.

## RFC-020 — Procedures: long-running guarded operations + reboot-commit

- **Status:** **Landed (v1.0) AS SPECIFICATION AND REGISTRY ONLY —
  implementation DEFERRED; nothing anywhere emits it.** LANDED on paper: the
  procedure PATTERN is normative (SPEC §9.3 — an action intent starts it, ECHO
  means ACCEPTED and not complete, a twin STATE channel carries
  `{procedure, phase, progress, result}` whose full-snapshot semantics make it
  reconnect-safe by construction, completion also EVENTs, and there is one
  channel per CONCURRENTLY-RUNNABLE procedure); `procedure_phases` is registered
  with the two-tier device range; `reboot_in_ms` is cbor key 43 and `REBOOTING`
  is GOODBYE code 0x0109. **DEFERRED:** key 43 appears NOWHERE outside registry
  comments — `encodeEcho` writes a fixed three-key map, no hub path emits a
  `REBOOTING` GOODBYE, and no procedure channel exists in any catalog. Treat as
  a specified extension point, NOT as field-tested behaviour. SPEC §18-4.
- **Origin:** servo programming (`WebUI.cpp:1060-1225`: modbus-enable →
  output off → write ×3 → save → rescan-verify — a sequenced transaction
  whose real ECHO is a later readback) and the motion-backend switch
  (`WebUI.cpp:1576-1620`: sole writer of `machcfg` NVS + deferred reboot).
  Intent+ECHO cannot express either; deadman/teardown semantics assume the
  hub survives its own accepted intent.
- **Proposed change:** no new frame types — a documented catalog PATTERN:
  a procedure is started by an action intent (ECHO = accepted); progress
  and outcome ride a twin STATE channel `{procedure u8, phase u8, progress
  u8, result u16}` (full snapshots → reconnect-safe by construction);
  completion also EVENTs. Reboot-commit: ECHO's applied map carries
  `reboot_in_ms`; the hub then GOODBYEs all sessions with new code
  `REBOOTING` before going down; `boot_id` change handles the rest.
- **Compatibility:** one new GOODBYE code + spec text; procedure channels
  are device-authored.

## RFC-021 — SlopSync Presets (operator-ordered)

- **Status:** **Landed (v1.0) for the MECHANISM; device preset BACKENDS
  DEFERRED.** LANDED: chunked transfer generalized into the namespaced blob verb
  BLOB_REQ (0x1A) / BLOB_CHUNK (0x1B), retiring and BURNING 0x09/0x0A so a stale
  draft-era peer meets an unknown type instead of silently misreading; the
  catalog became namespace 0; `class: STORE(4)` is an ORDINARY catalog entry
  carrying a store descriptor (a parallel top-level array would have broken the
  root shape, the id sort, the etag computation and the depth rules, all four);
  the store PAIR — static descriptor plus a tiny dynamic roster STATE — is the
  shape every store uses; payloads are opaque `bstr` the protocol never decodes;
  caps are hub-declared with generous floors. SPEC §8.7. **The first real user
  is the trust ledger** (RFC-027/029), which the feasibility pass ruled a blob
  store rather than a packed roster, and which carries the ONE
  registered-grammar carve-out. **DEFERRED: no device preset store backend
  exists yet** — the mechanism ships with no `pattern.*` store behind it.
  SPEC §18-15.
- **Origin:** `/api/pattern/presets` (NVS `advpreset`, 24 × `{name, def}`
  opaque blobs, 3600 B total); RFC-009's out-of-scope note; the ap-editor's
  import/export flow.
- **Proposed change — the preset STORE model:**
  1. A device declares one or more **stores** in its catalog: `{store_id,
     kind (tstr, namespaced — e.g. "pattern.frayd"), capacity, per_item_max,
     name_max}`. Multiple stores compose: saved positions, limit profiles,
     recordings — all the same machinery later, for free.
  2. **Roster:** per-store STATE `{generation u16, count u8, capacity u8}`
     — tiny, on-change. Generation bump = "re-enumerate."
  3. **Enumeration/fetch:** the catalog's chunked-transfer machinery
     generalized to a namespaced BLOB_REQ/BLOB_CHUNK (catalog becomes
     namespace 0; preset stores get their own) — one transfer verb for the
     whole protocol instead of a per-feature clone. Break-allowed ruling
     makes this clean now.
  4. **Items:** `{slot, name, kind, payload}`. Payload is a CBOR document
     the SPEC treats as OPAQUE — device-defined per kind. Fray-d fits
     natively: baseline scalars + six modifier blocks as a CBOR array of
     maps (control-plane encoding, so the repeated-group problem packed
     layouts have simply does not exist here).
  5. **CRUD intents** per store: `save` (hub captures CURRENT live state by
     default; client MAY supply a payload = import), `load` (hub applies;
     resulting truth arrives via the normal STATE broadcasts — ground
     truth, no special echo), `delete`, `rename`. Export = read the item;
     import = save-with-payload (hub validates kind + size, else
     `INVALID_VALUE`).
  6. **Caps are hub-declared, spec floors generous:** capacity ≥ 32
     conformance floor, per_item_max default 4096 B — unused is unproblem;
     small hubs declare less, the catalog says so, clients render
     accordingly.
- **Compatibility:** new frame generalization (BLOB_*), catalog store
  descriptors, per-store channels. Sized against reality: 32 fray-d presets
  ≈ 1.5 KB NVS; the mechanism doesn't blink at 256.

## RFC-022 — Registry hygiene omnibus

- **Status:** **Landed (v1.0)** — all ten, with one correction that had to be
  written down. Landed: 1 `probe_result_keys` into the registry; 2 ONE code
  space for NACK and GOODBYE (a separate space would have broken §4.3's
  range-based unknown-code fallback) plus `REBOOTING`; 3
  `safety_causes::session_loss`; 4 strictly-increasing `t_off` moved INTO the
  parser, so CLIENTS are covered and not just the device; 5
  `nack_detail_max_bytes` 48 with truncate-never-suppress; 7 the catalog field
  type authoritative over the CBOR major type; 8 encode-failure closes the
  session instead of silently dropping; 9 tracking capacity must exceed session
  capacity so the BUSY race is servable; 10 applied-within-advertised-range,
  scoped by the feasibility pass to ECHO `applied` and `setting_key`-bearing
  fields. **Item 6 needs the correction now recorded as SPEC §18-9:** "a
  BLOB_REQ carrying both a full request and `chunks` is MALFORMED" is
  UNREPRESENTABLE rather than enforced — `full` is DERIVED from the absence of
  `chunks`, so the illegal combination cannot be encoded and no decoder rejects
  it. What decoders actually reject is an EMPTY `chunks` array, and a
  catalog-namespace request carrying `store_id`/`slot`; the encoder-side refusal
  is an API guard only. The registry note on key 27 overstates it.
- **Origin:** 2026-07-25 grievance audit (file:line receipts inline).
- **Proposed changes:**
  1. `probe_result_keys` sub-key space INTO registry.yaml
     (`probe_report.hpp:8-15` allocated them in a C++ header — interop
     landmine).
  2. `goodbye_codes` gets its own space (or one normative "GOODBYE uses
     nack_codes" sentence) — today two clients hand-reuse 0x0107
     (`SlopSync.cs:1479`, `session.js:527`). Add `REBOOTING` (RFC-020).
  3. Safety `cause` enum gains `session_loss` — deadman is currently blamed
     for GOODBYEs/evictions (`hub_impl.hpp:1342`). **LANDED (M4a):**
     `releaseSessionSources()` derives the cause from the §11.4 release
     reason (3 deadman-release -> `deadman`, everything else ->
     `session_loss`), and `wire/estop_frame.hpp`'s hand-rolled `EstopCause`
     enum is DELETED in favour of the generated `safety_causes` — one
     spelling of one wire enum, regenerable from registry.yaml.
  4. `t_off` wording: "monotonic" → **strictly increasing, t_off[0]=0**.
     **LANDED (M4a) and the receiver claim was WRONG when written:**
     `BundleView::parse` bounded only the span; the hub re-derived the
     ordering at ingress, so the DEVICE was covered and every CLIENT was not.
     The walk now lives in the parser (one pass, ≤32 u16s, does t_off[0]==0 +
     strict increase + the span cap together), the hub's duplicate is gone,
     and the fuzz corpus replays clean over the changed decoder.
  5. NACK `detail` length: registered cap (48 B) + rule that over-length is
     TRUNCATED, never suppressed (today `encodeNack` returns 0 and the NACK
     silently vanishes, `nack.hpp:24-27`).
  6. CATALOG_REQ carrying both full + `chunks`: define as MALFORMED
     (`catalog_req.hpp:34` refuses on encode; decode side is undefined).
  7. Integer signedness: **the catalog field type is authoritative over the
     CBOR major type** (I64 with non-negative value round-trips as U64 —
     `intent.hpp:16-26`'s "only send negative values" advice is not a rule).
  8. Encode-failure rule: a hub that cannot encode a mandatory response
     (WELCOME/ECHO) MUST close the session (GOODBYE if possible) — never
     silently drop it (`hub_impl.hpp:697` "nothing sane to send" black
     hole).
  9. Transport-vs-session capacity sentence: tracking capacity MUST exceed
     session capacity (≥ +1) so the §6.3 BUSY race is servable
     (`hub.hpp:238-250` inferred it).
  10. Echo-vs-advertised-range rule: applied values MUST lie within the
      catalog field's declared min/max — a hub whose internal clamp can
      exceed them (the window +5 mm rail quirk, `MachineSim.cpp:397-402`)
      must widen its advertised max, not lie past it. Generic RFC-009
      renderers depend on this.
- **Compatibility:** registry additions + wording; item 5/8 change failure
  behavior (strictly better); item 10 may bump this device's advertised
  window max.

## RFC-023 — Congestion shedding table becomes normative

- **Status:** **Landed (v1.0).** The implemented decision matrix is now
  normative SPEC §10.4, written as ORDERED rows with first-match-wins so two
  conforming hubs shed identically under identical load. It carries the RFC-014
  segment exception (rows 4-6: whole-source or nothing, never decimate), the
  never-shed exemption, and the first-push-after-grant exemption that keeps a
  session from being stranded mid-adoption. Honest scope note: the reference hub
  consults the table for STATE pushes only, so the STREAM decimation rows and
  the entire segment branch are specified and unit-tested rather than
  field-exercised — SPEC §18-5.
- **Origin:** `shedding.hpp:8-10` — the implemented decision matrix comes
  from the M5 milestone brief, admittedly "more prescriptive than SPEC
  §10.4's prose." Two conforming hubs would shed differently under
  identical load; clients cannot predict either.
- **Proposed change:** adopt the implemented table into §10.4 as normative
  text (it is the reference behavior, already field-tested).
- **Compatibility:** codifies shipped behavior.

## RFC-024 — Idle-session reaping for non-owning sessions

- **Status:** **Landed (v1.0).** Promoted from MAY to SHOULD with registry
  default `idle_reap_multiplier` (3) and implemented. SPEC §6.6 states the two
  liveness regimes in one table: source OWNERS get the deadman window and its
  loss policy; everyone else gets idle reaping with NO motion consequence.
  Complemented by RFC-015's `READY_TIMEOUT`, which covers the one case reaping
  structurally cannot see — a client that PINGs forever and never adopts.
- **Origin:** `hub_impl.hpp:1293-1300` — §11.3's deadman binds to active
  sources; §6.5's "MAY reap at 3× idle interval" was left unimplemented, so
  a viewer session that goes dark holds a slot forever (until a BUSY-range
  eviction pressure exists, which it doesn't).
- **Proposed change:** promote to SHOULD with a registry default
  (`idle_reap_multiplier: 3`); clarify the two liveness regimes in one
  table: source-owners → deadman window + loss policy; everyone else →
  idle reaping, no motion consequence.
- **Compatibility:** behavioral; frees slots on real hubs.

## RFC-025 — Safety semantics completion (incl. override/bypass ruling)

- **Status:** **Landed (v1.0)** — all three parts (recorded as landed in
  milestone M4a; re-confirmed against the v1.0 rewrite, which carries (a) as
  "the HUB latches all four levels" in SPEC §11.1, (b) as the role-exempt
  `stop`/`estop` rule in §11.2, and (c) as the safety-domain modes byte in
  §11.1).
- **Origin:** three underspecified edges found live: (a) HOLD/PAUSE have
  registry codes + wire bits but no rule on WHO latches them
  (`hub_impl.hpp:583-586`, `safety.hpp:10-12`) — a generic client cannot
  know if sending HOLD does anything on an arbitrary hub; (b) whether
  viewers may send stop-class ops is unstated — slopsync-js guessed
  restrictive (`bridge.js:252-257`), and the wrong guess means "the person
  in the room cannot stop the machine"; (c) override/bypass currently ride
  a legacy HTTP endpoint with no SlopSync home.
- **Proposed change:** (a) the HUB latches all four levels in 0x0003 —
  delegate acceptance is what triggers the latch; a hub whose delegate
  doesn't implement HOLD/PAUSE NACKs `UNSUPPORTED_OP` (discoverable,
  honest). (b) Role exemption rule: `estop` (RFC-010) and `stop` are
  role-EXEMPT on 0x0005 — anyone may stop the machine, §11.2's "safety
  outranks authorization" generalized; `hold/pause/resume/estop_clear`
  require controller. (c) `manual_override` and `bypass_limits` become
  safety-domain state: represented in the 0x0003 snapshot (appended byte —
  append-only legal) and written via 0x0005 ops (`override_on/off`,
  `bypass_on/off`, controller role); the per-move `bypass` key on 0x0100
  stays as-is. Also fold in: `home` 0x0103 gains bench ops
  `2 = force_home {stroke}` / `3 = clear_override` (controller; noting op 2
  clears an e-stop latch, so it lives HERE under safety review, not in a
  convenience bucket).
- **Compatibility:** registry ops + one appended STATE byte + spec text.

## RFC-026 — Strings on the wire (operator-ordered)

- **Status:** **Landed (v1.0).** All three tiers. (1) identity/product strings
  ride the CBOR control plane and are REGISTERED as WELCOME `identity` — though
  see RFC-016 for the deferred codec, which is the one place this RFC's promise
  is not yet cashed. (2) `str16`/`str32`/`str64` are `packed_field_types`
  8/9/10, fixed-width zero-padded UTF-8, implemented in the layout codec and
  validated by the catalog codec; a reader stops at the first NUL or the width.
  (3) STREAM sample layouts remain string-free, normatively (SPEC §5.4). The
  `secret str32` warning survived into the registry note.
- **Origin:** RFC-009 sub-decision 7 (now resolved to option (a) by
  ruling); the §3.1 link-status gap (BSSID/IP are strings, hence the
  WebUI's 30 s HTTP poll that exists ONLY to fetch a string); device-name
  setting.
- **Proposed change:** three tiers, each in its natural home:
  1. **Identity/product strings** → CBOR control plane (WELCOME, RFC-016).
     Already legal; zero new machinery.
  2. **String VALUES in packed STATE/settings** → new `packed_field_types`:
     `str16 / str32 / str64` — fixed-width, zero-padded UTF-8, register-map
     style. Offsets stay static; append-only evolution preserved; RFC-009
     renders them as text inputs (`max_len` = width), `secret` flag
     composes (presence-bit rule unchanged).
  3. **STREAM sample layouts remain string-free** — the motion hot path
     never pays for text.
  Immediate consumers: `hub_name` as a writable setting, the link-status
  channel (BSSID/IP) that kills the WebUI's last status poll.
- **Compatibility:** new packed field types (registry + catalog.cddl +
  codegen); break-allowed ruling lets the type ids slot in cleanly.

## RFC-027 — Capability-agnostic pairing + tiered access (operator-ordered)

- **Status:** **Landed (v1.0).** Tiers renamed at UNCHANGED wire values
  (`watch`/`control`/`configure` = 0/1/2). All three association modes
  implemented: knock-and-approve with the bounded pending list exposed as
  protocol state (0x000A/0x000B) and no frame answering the knocker; PIN proof
  with constant-time compare through the injected crypto delegate and the
  three-strike window close; push-to-pair with the power-cycle presence gesture
  and the possession-is-root factory-fresh rule. `pairing_modes` is advertised
  as a bitmask in WELCOME `trust`, RE-EVALUATED PER SESSION so a transient
  window is advertised only while it is genuinely open. Revocation is protocol
  (0x0009 `revoke`), not a WebUI feature. The H3 honesty clause on offline PIN
  brute-forcing is normative TEXT (SPEC §12.3), not a footnote. **§12.2's "admin
  granted only via the hub's own UI" sentence is STRUCK**, and SPEC §12.3 states
  the deliberate consequence out loud: the admin surface, eviction included, is
  reachable through pairing.
- **Origin:** §12.2's single ceremony assumes joiner keyboard + trusted
  display — a 6-button coin-cell remote can do neither; the trusted
  surface is implicitly the WebUI (circular); no bootstrap story for the
  first admin. Prior art: BLE SSP association models (IO-capability
  adaptive), WPS-PBC, Matter commissioning.
- **Proposed change:**
  1. **Tiers renamed, wire values unchanged:** `watch(0)` / `control(1)`
     / `configure(2)`. Control includes STREAM publishing (a motion
     producer is a controller). Composes with standing rules: safety
     estop/stop role-EXEMPT (RFC-025); OTA never derivable from any tier
     (standing ruling); serial/in-process remain implicitly configure
     (physical possession, §12.3).
  2. **One ceremony, three association modes, all ending in PAIR_GRANT
     `{token, role}`.** Role is an attribute of the GRANT, never of the
     ceremony; zero-or-one PIN exists, never per-tier secrets.
     - **(a) Knock-and-approve (primary, capability-agnostic):** bare
       PAIR_REQ (no proof) → bounded pending list (≤4) exposed as
       protocol state (pending-pairing STATE + EVENT twin) → any
       configure-tier session approves `{instance_id, role}` via intent
       (or denies; window per knock, e.g. 120 s). Joiner needs one button
       and no display. Trusted surface = ANY configure client (phone,
       CLI, WebUI), killing the WebUI dependency.
     - **(b) Numeric proof (self-service):** today's HMAC-PIN flow, kept
       for keyboard-bearing joiners when no admin session exists.
     - **(c) Push-to-pair (bootstrap + potato fallback):** a PHYSICAL-
       PRESENCE PROOF opens a short SINGLE-GRANT window; first knock is
       granted without approval. The spec requires the *proof*, not a
       GPIO — **bare-minimum hardware is NONE, because the power cord is
       the button:**
       * *Factory-fresh (zero configure tokens): no gesture needed* — the
         hub boots claimable; first knock gets configure. Whoever unboxed
         and powered it possesses it (Matter/Chromecast commissioning
         semantics).
       * *Re-open later:* the **power-cycle gesture** — N (default 3)
         consecutive boots each with uptime < ~10 s → next boot opens the
         window. NVS boot-counter only; cannot collide with a session
         (any power loss already stops motion and forces re-home).
       * A hub with ANY real button MAY bind it as the pairing control —
         UX upgrade, never required. A hub with SlopGlow hardware SHOULD
         show a pairing glow-state; window state is also observable
         in-band by any watch session regardless.
       * Factory reset (token-store wipe) MUST be a deliberately HARDER
         gesture (longer cycle sequence or serial console, which is
         implicitly configure per §12.3) — never the same gesture as
         opening pairing.
       **Grant rule: if zero configure tokens exist, the window grants
       configure — physical possession is root.** Thereafter it grants
       the configured default (control), and knock-and-approve does the
       rest.
  3. Hub advertises available modes (WELCOME `limits`/identity map);
     registry gains a pairing-modes enum + defaults.
  4. **Revocation is protocol, not WebUI:** the paired-device roster
     (instance_id, name, role, last-seen) is readable and revocable from
     any configure session — rides RFC-018's admin surface.
  5. **Honesty clause (normative text):** the HMAC-PIN proof is
     offline-brute-forceable by a passive observer of the pairing
     exchange (4 digits = 10⁴ HMACs); acceptable for the v1 threat model
     (casual/drive-by prevention), MUST be stated plainly. PAKE (SPAKE2)
     remains the reserved v2 upgrade — not in v1 because WebCrypto has no
     PAKE and mandating it would exile the browser client.
- **Compatibility:** PAIR_* CBOR is already extensible; new pending-
  pairing state surface + approve intent; registry additions. Break-
  allowed ruling permits reshaping PAIR_REQ cleanly now.

## RFC-028 — Parser robustness + fuzz conformance gate (anti-CVE)

- **Status:** **Landed (v1.0)** — all five obligations, including the one
  previously open. 1/2/4/5 landed with the fuzz gate (7 libFuzzer targets,
  ASan+UBSan, committed corpus, CI workflow) and three real bugs fixed; they are
  now SPEC §5.8 (parser totality, explicitly SYMMETRIC for clients) and §17.4
  (the totality gate, with both institutional lessons written into the normative
  document — the `declared <= remaining` rule and the ASan-invisible
  intra-object overflow). **Obligation 3 is now CLOSED structurally:** `ICrypto`
  exists in `core/crypto.hpp` and is the 5th Hub constructor parameter with a
  null-object default, mirroring IClock/IRandom exactly; `hmacSha256` and a
  volatile-accumulator `constantTimeEqual` are fully implemented, and
  constant-time compare for every token/proof/signature check is a normative
  requirement (SPEC §5.8-7). **Honest caveat that belongs with it:** the DEFAULT
  `SoftwareCrypto` inherits `ICrypto`'s stub `signP256`/`verifyP256`/`publicKey`,
  which return 0/false/0. Signing is a working SEAM, not a shipped capability —
  see RFC-029 and SPEC §18-11.
- **Origin:** the wire parser is the attack surface in BOTH directions:
  the hub parses HELLO/INTENT/bundles from untrusted clients, and CLIENTS
  parse WELCOME/catalog/STATE from possibly-untrusted hubs — a client
  auto-connecting to any discovered `_slopsync._tcp` beacon is one
  malicious hub away from parsing hostile bytes, and the catalog (rich in
  variable-length strings, growing via RFC-009/026) is the fattest
  client-side surface. Today's conformance = golden vectors only; no fuzz
  requirement exists anywhere.
- **Proposed change:**
  1. **Parser totality (normative):** every conforming parser, hub or
     client, MUST map ANY byte string to accept-or-reject — no OOB reads,
     no unbounded allocation or recursion, no UB. The deterministic CBOR
     profile + depth-4 cap + definite lengths already do the heavy
     lifting; this makes it a conformance obligation, not a style.
  2. **Length fields are never trusted past the enclosing buffer**;
     registry string caps (names 32/24/8, desc 128, NACK detail 48,
     option labels) are enforced at parse — reject, never truncate-and-
     continue, on structural payloads.
  3. **Crypto via injected delegate** (`ICrypto`: hmac_sha256, random,
     constant-time compare — same pattern as IClock/IRandom, preserving
     the zero-dependency library). Constant-time compare REQUIRED for all
     token/proof checks (the OTA plane already does this; make it
     protocol-wide).
  4. **Fuzz corpus ships with the conformance suite:** structure-aware
     seeds per frame type (valid vectors + mutations), run under
     libFuzzer/AFL on the native builds of BOTH reference cores (hub and
     client). Release gate for reference implementations: N CPU-hours,
     zero crashes/sanitizer findings. Golden vectors prove correctness;
     fuzzing proves totality.
  5. **Client obligations are symmetric** (normative sentence): a hostile
     hub MUST NOT be able to crash a conforming client. slopsync-js,
     the MFP plugin, and the C++ client core all carry the same totality
     duty as the hub.
- **Compatibility:** spec text + conformance tooling; zero wire change.
  Cheapest insurance in the queue.
- **STATUS — obligations 1/2/4/5 LANDED (fuzz gate built and run, 2026-07-25).**
  `test/fuzz/` (7 libFuzzer targets, ASan+UBSan, `-fno-sanitize-recover`),
  a committed encoder-generated seed corpus, and `.github/workflows/fuzz.yml`
  (the repo's FIRST CI workflow: per-target matrix, deterministic corpus
  replay + short PR budget, 30-min-per-target nightly). Built and run under
  WSL2 clang — the Windows MinGW host has no clang and no libFuzzer, so the
  README documents the exact invocation.
  **THREE REAL BUGS, all fixed, all with regression doctests:**
  1. `CborReader::readTstr/readBstr` bounded strings with `start + len >
     size()`, which OVERFLOWS: a head of `7B FF*8` (tstr claiming 2^64-1
     bytes) wrapped the sum past the check and returned a 2^64-1-byte view
     into a 9-byte buffer, while rewinding `_pos` backwards. Reachable from
     EVERY message decoder and from `skipValue()` — the §4.3 unknown-key
     path, i.e. the bytes a decoder does not understand. This was the
     CVE-shaped one. Fix: `arg > remaining`.
  2. `ChunkReassembler::begin()` correctly REFUSED an over-capacity transfer
     but still stored the attacker's `chunk_count`/`total_bytes`;
     `missingIndices()`/`assembled()` then used them without checking
     `active()`. The lesson: refusing a transfer must refuse its NUMBERS —
     a guard that leaves attacker sizes in members only moves the bug one
     call to the right.
  3. `Reassembler::accept()` had an UNBOUNDED `memcpy` into the 504-byte
     `pendingLastBytes` — the one write in the class that did not go through
     the bounds-checking `placeFragment()`.
  **The trap worth institutional memory:** #3 survived a 7.8-MILLION-execution
  fuzz run and was found by hand. Its spill lands in the very next member of
  the same struct, and an INTRA-OBJECT overflow is invisible to ASan — only a
  write long enough to leave the whole enclosing object reports. Hence
  `-max_len=8192` in CI, and hence: never conclude "the fuzzer would have
  caught it" for a bug between two arrays of one struct.
  **Obligation 3 (`ICrypto` injected delegate) is NOT done** — it is an API
  change, not a fuzzing deliverable, and remains open.
  **Honest coverage note:** the gate proves DECODER totality. It does not
  drive Hub/Client through stateful protocol sequences, does not touch the
  firmware transports in `src/comms/`, and says nothing about semantic
  correctness (that is the golden vectors' job). `test/fuzz/README.md` has
  the full not-covered list.

## RFC-029 — Trust lifecycle: hub authenticity, change tripwires, own-UI trust

- **Status:** **Landed (v1.0)** — all six items, with one capability caveat.
  (1) durable hub identity: signature material is fixed at exactly
  `client_nonce(8) || session_id(u32 LE) || boot_id(u32 LE)` = 16 bytes, the
  client nonce being the feasibility pass's replay fix; delivered inline in
  WELCOME or deferred in HUB_SIG (0x1D) with identical material and identical
  client handling, first valid answer winning, and `hub_sig_timeout_ms` (3000)
  bounding ONLY a client that pinned a key (silence from a hub with no keypair
  is conformant — honesty clause H9). (2) the version tripwire with
  `trust_states` and the RECOGNIZED-PENDING suspension, its honesty clause
  normative (SPEC §12.6, H6/H7 — including the real gap that a device reporting
  NO version can never trip it). (3) the symmetric hub-change signal. (4)
  `/uitoken` — IMPLEMENTED (`src/comms/SlopSyncUiToken.cpp`), sanctioned as HTTP
  escapee #2, and normatively NOT a connection prerequisite (SPEC §12.8, H8).
  (5) the phish note, as honesty clause H5. (6) token presentation modes with
  AUTH (0x1C), `auth_attempts_max` (3), and the previous-session-nonce shortcut
  staying dropped. **CAVEAT, stated because a reader will otherwise assume a
  battery where there is a socket:** real ECDSA sign/verify come from an
  INJECTED `ICrypto`; the library's default implementation stubs them, so
  evil-twin detection exists only where an application supplies the primitive.
  SPEC §18-11.
- **Origin:** the token store trusts a DEVICE identity forever regardless
  of the code behind it; nothing distinguishes the real hub from an evil
  twin replaying its identity strings; the machine's own served WebUI has
  no defined trust status.
- **Proposed change:**
  1. **Durable hub identity (the primitive everything else hangs on):**
     hub generates a P-256 keypair at first boot (NVS; P-256 chosen
     because WebCrypto can verify it — the browser participates). The
     pubkey fingerprint is the machine's durable id. PAIR_GRANT delivers
     the pubkey — trust is anchored at the pairing ceremony, the moment
     physical presence was proven (TOFU at a verified moment). On session
     establishment the hub signs the session nonce; clients verify
     against their pinned key. A clone machine copies every string but
     fails the signature → client MUST surface "not your machine" and
     withhold intents. Potato clients paired by physical ceremony MAY
     skip verification. Sign cost: once per session, off the hot path.
     Crypto rides RFC-028's injected ICrypto delegate.
  2. **Client-change tripwire ("untrusted but I recognize you"):** HELLO
     gains `client_ver` (tstr). Trust-ledger entry per paired device:
     {instance_id, kind, name, version, first_seen, last_seen, role,
     state}. Observed version change ⇒ state drops trusted →
     RECOGNIZED-PENDING: session admitted at watch, granted role
     suspended, re-approval intent surfaced to configure sessions
     ("plugin 0.2.3→0.3.0 — keep trusting?"). Default policy: watch
     auto-rekeeps; control/configure require re-approval; hub-
     configurable. **Honesty clause (normative): self-reported version is
     a TRIPWIRE, not attestation** — a deliberately malicious update lies
     and keeps its token; the real bounds on a hostile client are role
     scoping, instant revocation, roster visibility (RFC-018 + version
     history), and the role-exempt safety ops (RFC-025).
  3. **Hub-change signal (symmetric):** hub fw_version change (visible
     via RFC-016 WELCOME identity + etag/boot_id) SHOULD be surfaced by
     clients ("machine updated to X.Y.Z"); clients MAY gate configure-
     tier actions on user acknowledgement after a change. Hub code
     changes only via the OTA plane, which is outside SlopSync trust by
     standing ruling — a configure-tier compromise cannot flash firmware.
     A hostile hub's ceiling against conforming clients is well-formed
     lies, per RFC-028 symmetric parser totality.
  4. **Own-UI default trust via SERVED-PAGE TOKENS (operator design,
     2026-07-25 — supersedes the earlier Origin-only draft), capped:**
     the hub's own served WebUI is trusted by default through a
     browser-enforced one-time token, not a forgeable header:
     * The served page does a SAME-ORIGIN `fetch('/uitoken')`; the hub
       mints a single-use token (TTL ~60 s, rate-limited, minted only —
       never templated into the static gzip); the page presents it in
       HELLO → **control tier (never configure)**.
     * The boundary is the browser's same-origin policy: the endpoint
       sets NO CORS headers, so any cross-origin page (clone UI,
       malvertising LAN scan — the mass-automatable vector) can send the
       request but cannot READ the token. Manufactured tokens fail the
       single-use server mint; a stolen-in-the-gap token makes the real
       page's HELLO fail LOUDLY (visible race, never silent compromise).
     * Beats Origin-checking on webview compatibility (absent/null
       Origin breaks legit embedded shells; a token fetch works wherever
       the page runs) and auditability (each token maps a page-serve to
       a session in the roster). Where an Origin header IS present it
       MAY still be used as a second independent filter — free.
     * Honesty clause: a NATIVE process on the LAN can curl the endpoint
       — but that attacker class already defeats the cleartext-token
       ceiling (§6), so this mechanism loses nothing to it while fully
       closing the browser-borne class. Threat-model ruling (operator):
       optimize against automatable mass vectors; accept the ceiling on
       individually-targeted LAN-resident attackers.
     * Toggleable off for shared spaces. **Configure always pairs, no
       exceptions.** Deployment commandment: the SlopSync port is NEVER
       exposed to WAN — LAN-first is a security property.
     * **NOT a connection prerequisite (normative).** A WebUI never
       NEEDS `/uitoken` to connect: with the endpoint absent, disabled,
       or failed it is an ordinary client — viewer by default (§12.2
       open viewing), and control/configure via any RFC-027 association
       mode (knock-and-approve, PIN, push-to-pair), with its token
       persisted against its `instance_id` like any other client's.
       Since configure ALWAYS pairs, a WebUI already exercises the
       normal ceremony regardless. `/uitoken` only removes ceremony for
       the control tier on the machine's OWN page; it grants no
       capability that pairing cannot, and clients MUST implement the
       pairing path irrespective of it. This is precisely why it is a
       sideband and not a second plane (standing ruling).
  5. **Phish note (normative, informative tone):** clone-page attacks
     that proxy a PIN to the real hub are active MITM, excluded from the
     v1 threat model (§12.1) and stated as such; knock-and-approve is
     the RECOMMENDED ceremony partly because its approval surface shows
     the knocker's identity on hardware the attacker doesn't control.
  6. **Token presentation modes (passive-theft plug, floor unchanged):**
     v1 transports are cleartext (`ws://`), so a raw bearer token in
     HELLO is sniffable by a passive LAN observer (§12.1 excludes that
     attacker, but the plug is near-free). Two presentation modes:
     **(a) bearer** — raw 16-byte token in HELLO; remains legal (the
     potato floor stays a memcpy, zero crypto). **(b) proof** —
     `HMAC-SHA256(token, welcome-nonce)` truncated 16 B, RECOMMENDED for
     every client that has SHA-256 (browser/WebCrypto, C#, all ESP32s —
     i.e., everyone but coin cells): a sniffer captures a one-time proof,
     never the credential. Hub accepts both; roster records which mode a
     device uses (visible security posture). Note: proof mode requires
     HELLO→nonce→proof, so it rides the existing WELCOME nonce with one
     added round-trip only for proof-mode clients, or the nonce from the
     PREVIOUS session (hub keeps last-issued nonce per instance_id —
     zero extra round trips on reconnect, replay-fenced by nonce
     rotation).
- **Weight audit (recorded so the lightweight covenant is checkable):**
  mandatory client floor after 027/028/029 is UNCHANGED from v1-draft —
  same parser, zero required crypto, 24 bytes of stored identity. All
  cryptographic weight lands hub-side (mbedtls already linked for WiFi)
  or in CI (fuzzing ships zero bytes). The named residual holes, chosen
  with eyes open: the own-UI token endpoint is curl-able by NATIVE LAN
  processes (browser-borne attacks are CORS-blocked; that native class
  already defeats the cleartext ceiling, so nothing is newly lost —
  capped at control, toggleable); push-to-pair windows can be raced (single-grant, visible, revocable);
  cleartext transport bounds everything at "honest LAN" until wss/v2
  (which is why the hub signature is designed to work WITHOUT secrecy).
- **Compatibility:** new HELLO key (client_ver), PAIR_GRANT pubkey field,
  WELCOME signature field, token-proof presentation key, trust-ledger
  states + re-approval intent on the RFC-018/027 admin surface.
  Break-allowed: fields land clean. ICrypto delegate gains sign/verify
  (hub sign: mbedtls; client verify: WebCrypto / System.Security /
  mbedtls).

---

## FEASIBILITY PASS (2026-07-25) — amendments bound into the base pass

*Two bounded audits before green light: protocol consistency (38 findings,
10 blockers) and on-target feasibility (S3/32D/toolchain). Every amendment
below is part of its parent RFC as if written there. One item needs an
operator ruling — it is at the bottom, alone.*

### Wire grammar & number space
- **Frame assignments:** PUBLISH=0x18 (013), CATALOG_READY=0x19 (015),
  BLOB_REQ=0x1A / BLOB_CHUNK=0x1B (021, retiring 0x09/0x0A), AUTH=0x1C
  (029, below). 36 core slots remain free.
- **CBOR key conservation:** per-feature keys nest in scoped sub-maps the
  way `limits`(22)/`probe_result`(26) already do — one `identity` key
  (016), one `blob` key (021), one `trust` key (029). Global core demand
  drops from ~18 keys to ~8 of the 27 remaining; the 1–63 space survives
  the protocol's stated lifetime.
- **EVENT bodies get scoped (grammar fix, rides 022):** kind-specific
  fields move into a `body` sub-map whose integer keys come from the
  channel's catalog `schema` — mirroring INTENT's `value`. Without this,
  every device-authored EVENT channel (the anomaly channel!) would need a
  registry PR for its field keys — the exact coupling the catalog exists
  to prevent.
- **GOODBYE codes: single space.** GOODBYE draws from `nack_codes` (one
  normative sentence); `REBOOTING = 0x0109`. A separate space would break
  §4.3's unknown-code range fallback.
- **Preset stores are catalog entries of new `class: STORE(4)`** — keeps
  the catalog root shape, id sort, etag computation, and per-entry-
  document depth rules intact (a parallel top-level array would break all
  four).

### Payload & catalog math
- **018 roster slots gain `name: str16`** → 8 × 22 B + 4 = 180 B ✓ (the
  exact sweet spot at `default_max_clients_ws` 8). Names longer than 16 B
  truncate in the roster; the full name arrives via 0x0007 while the
  session lives. This is also the FIX for the blocker that join-events
  are never replayed (§9.4) — without it a late joiner could never learn
  existing sessions' names.
- **009 capacity restated honestly:** ≈58 f32 or ≈115 u16 settings per
  242 B snapshot, INCLUSIVE of enabled_mask bytes (the old "~60" ignored
  the mask).
- **New registry limit `catalog_max_entry_bytes` (4096):** a 50-field
  fully-annotated entry encodes to ~8–10 KB, which violates 028's
  no-unbounded-allocation rule for per-entry decode buffers. Oversize
  entries are a catalog-authoring error caught by conformance tooling —
  the ap_* channel splits or trims descs to fit.
- **027/029's paired-device / trust ledger is a BLOB store** (021
  machinery: tiny `{generation,count,capacity}` STATE + chunked
  enumeration) — NEVER a packed STATE roster; the math fails at 2–7
  entries per snapshot depending on field set.
- **026 note:** secret string settings SHOULD be `str16` or write-only
  with a presence bit — a `secret str32` burns 13% of a snapshot to say
  one bit.
- **020:** one procedure STATE channel per concurrently-runnable
  procedure (full-snapshot semantics can represent exactly one).

### Cross-RFC reconciliations (the blockers)
- **015 READY gates BOTH planes.** Pre-READY INTENTs are NACK'd —
  otherwise a client could act before adopting the safety latch,
  breaking §11.5(2). §6.3's "immediately push" and §10.1's never-shed
  wording are amended to make READY the precondition of both directions.
  New registry limit `catalog_ready_timeout_ms` (15000): a session that
  PINGs but never READYs is GOODBYE'd (liveness reaping alone never
  fires on a pinging client). 015 and 021 land TOGETHER: transfer verbs
  become BLOB_*, only the catalog namespace has a READY concept.
- **029 signature replay fix:** HELLO gains `client_nonce` (inside the
  `trust` sub-map); the hub signs `client_nonce ‖ session_id ‖ boot_id`.
  Without client entropy the signature was replayable from one captured
  WELCOME — evil twin passes verification. BLOCKER, now dead.
- **029 proof mode gets a home:** new AUTH frame (c2h, 0x1C) carries the
  token proof after WELCOME and re-issues `roles` — the session grammar
  had no "upgrade role mid-session" path (a second HELLO would
  self-evict via §6.3's duplicate rule). **The "previous-session nonce"
  reconnect shortcut is DROPPED** — it was replay-unsafe (undefined
  rotation point; §6.3 makes a successful replay EVICT the real client;
  honest retransmits vs single-use nonces are irreconcilable on lossy
  bindings). Proof mode costs one extra round trip per connect. That is
  the honest price; bearer mode remains the potato path.
- **025/010 per-op access is now expressible:** catalog `schema-field`
  gains optional `access` (per-op minimum role); channel `access` is the
  floor, per-op overrides. Without it, role-exempt estop/stop forced
  0x0005 to viewer-access and a generic 009 renderer would show
  hold/pause/takeover to every viewer — discovering otherwise only by
  NACK, violating grey-never-hide. Exempt ops ARE §9.3-rate-limited
  (viewer loop-stop spam is a named, limited, accepted risk in §12.1 —
  the person in the room stopping the machine outranks it).
- **014/023 segments are NON-DECIMABLE.** §9.2's shedding rationale
  ("dropped samples recoverable by interpolation") is TRUE for dense
  position samples and FALSE for timed segments — a shed segment is a
  permanently lost command. Segment-class STREAM channels shed
  whole-source or not at all; 023's normative table carries the
  exception.
- **008.3 scoped, not violated:** "no per-client case logic" now reads
  "the hub MUST NOT branch on client identity when planning or executing
  MOTION"; authorization is identity-branching by definition and is the
  named carve-out. (027/029's ledger, tiers, and own-UI trust are
  authorization; the motion plane stays identity-blind.)
- **016/027 capability split:** session-layer capabilities (pairing
  modes) live in WELCOME; CHANNEL capabilities are catalog
  introspection. `fw version` is struck from 0x0006's registry note —
  identity lives in WELCOME, one home, no drift.
- **012:** §9.2's "never NACKed" gains the explicit SOURCE_CONFLICT
  carve-out (same precedent as §10.5's RATE_LIMITED NACK).
- **013:** new registry `max_burst_multiple` (4); `burst` is clamped and
  echoed like every wish — an unbounded client-declared burst would
  reintroduce the flood the limiter exists to stop.
- **021:** preset payloads are `bstr` the protocol layer NEVER decodes —
  028's depth/allocation budget explicitly does not extend inside them;
  a client that decodes one does so above the protocol boundary. Imports
  larger than `max_frame` ride BLOB chunks, never fragmented INTENTs.
  Registry gains per-binding `max_frame` defaults (it never had any).
- **019/011 reset classification:** an action that restores
  configuration values bumps `cfg_gen` AND `reset_gen` (`ap_reset` is
  this); an action that clears counters bumps `reset_gen` only
  (`reset_stats`/`reset_peaks`/slopmotion).
- **022.10 scoped:** applied-within-advertised-range applies to ECHO
  `applied` maps and `setting_key`-bearing fields; effective/read-only
  fields lawfully exceed a paired setting's range and declare their own
  display bounds (this was colliding head-on with RFC-003's origin
  case).
- **022.5 vs 028.2 reconciled:** SENDERS truncate diagnostic strings to
  the registered cap; RECEIVERS reject over-cap strings in structural
  payloads. NACK `detail` is diagnostic.
- **017 vs §9.4:** the no-replay rule gains "except where a channel's
  catalog entry declares a replay depth"; the log channel declares one.
- **027 vs §12.2 (was a blocker nobody wrote down):** §12.2's "admin
  granted only via the hub's own UI" sentence is STRUCK in the 027
  landing commit — configure is obtainable by ceremony now, which means
  **018's evict power is reachable through pairing**; 018's gate is
  restated against the new configure definition, deliberately.
- **At batch time:** 003 and 006(b) are marked *Rejected — superseded by
  009* (numbers kept, per queue rules) so the registry never grows two
  ways to express one thing.

### On-target reality (measured, not assumed)
- **ECDSA P-256: feasible, with placement rules.** mbedtls is compiled
  into the pinned framework with ECDSA enabled — and it is DETERMINISTIC
  ECDSA (RFC 6979), removing RNG quality from signing. The S3 has NO ECC
  accelerator (that peripheral is C3/C6/H2): sign ≈30–80 ms, keygen
  ≈40–100 ms, software with hardware-bignum assist, one uninterruptible
  call. Rules: keygen at first boot only; sign on a low-priority task;
  NEVER inline in an HTTP/WS handler; signature is ON-REQUEST (a HELLO
  `trust` flag) so potato handshakes stay instant.
- **HMAC costs nothing:** the library already ships self-contained,
  test-vectored `hmacSha256` (the PIN proof path). Token proofs reuse
  it; `ICrypto` shrinks to sign/verify only, added as a 5th Hub ctor
  param with a null-object default, mirroring IClock/IRandom exactly
  (pattern confirmed present, lib confirmed crypto-include-free).
- **THE CATALOG RAM CATCH (the pass's biggest find):** uniform
  `Catalog<48,50>` = **320 KiB** (entry = 24 + 136·F bytes; every entry
  carries BOTH layout and schema arrays though only one is ever
  populated; the bitfield-names array is 64 of LayoutField's 100 bytes).
  Mandated fixes: (1) `buildSlopDriveCatalog()` becomes an OUT-PARAM —
  the current by-value return at F=50 is a 320 KiB stack temporary, the
  HubSession stack bomb, act two; (2) layout/schema become a
  union/variant; (3) field capacity is PER-ENTRY (exactly one entry —
  the flattened ap_* — needs ~50 fields) → whole catalog ≈59 KiB; (4)
  `static_assert` on total catalog size + documented PSRAM residency
  (the service is already placement-new'd into the 8 MB PSRAM; the
  catalog must stay a by-value member of it).
- **Fuzz gate:** no clang on this host, no CI exists at all. The honest
  setup: one GitHub Actions workflow (ubuntu, system clang,
  `-fsanitize=fuzzer,address,undefined`) compiling the header-only
  decode surfaces DIRECTLY — no PlatformIO — targeting the catalog
  codec, the CBOR reader, and fragmentation/frame-header parsing; 60 s
  per target on PR + nightly with a checked-in corpus. Local substitute:
  deterministic random-input doctest loop over the same surfaces
  (coverage-blind, zero new toolchain).
- **NVS:** 20 KiB partition, ~16 KiB usable; ledger + keypair ≈1.1 KiB
  fits. Rules: the trust ledger is ONE blob in the existing `slopsync`
  namespace, kept under ~1900 B (single-page), written only on change,
  and gated on `ota_active` exactly like `savePairing()` (flash-cache
  writes during OTA reset the chip). Correction: `kMaxPaired` is 8
  today, not 16.

### IMPLEMENTATION DECISIONS (recorded during the base-pass build)
- **CONFIRMED (orchestrator ruling, asked for by the M6 spec rewrite): the
  hub's hardcoded floor on `estop_clear` is CORRECT and stays.** The hub
  resolves per-op access generically from `option_access`, and then ALSO
  applies a hardcoded minimum role to `estop_clear` specifically. That looks
  like the channel-id special-casing RFC-025 forbids. It is not, and the
  distinction matters:
  * `option_access` is CATALOG DATA, authored by a human. If someone
    mis-authors channel 0x0005 — omits the vector, or marks `estop_clear`
    as `watch` — then clearing an e-stop latch becomes reachable by any
    anonymous LAN client. A safety-critical authorization would be derived
    from a data file with no floor under it.
  * The prohibitions this appears to violate are both about something else:
    RFC-008.3 forbids branching on WHO is talking when PLANNING MOTION;
    RFC-025 forbids per-channel logic that duplicates what the catalog
    already expresses. A hardcoded FLOOR under a safety op branches on
    neither identity nor client behaviour — it bounds the damage a bad
    catalog can do.
  Promoted to normative text by M6 as: *a hub MUST NOT let a catalog
  authoring error widen safety authorization* (SPEC §11.2). The general
  principle for future review: generic resolution handles the general case;
  safety operations additionally get a floor that data cannot lower.
- **`option_access` is SCHEMA-FIELD ONLY; layout fields do not get it.**
  Raised during M2b: a layout select (e.g. a motion-backend dropdown) might
  seem to want per-option gating. Ruling: no. A layout field is the READ
  side — a STATE snapshot value — and ALL write authorization flows through
  the paired INTENT channel named by `settingChannel` + `setting_key`. A
  client needing per-option access resolves that join (which it must do
  anyway to encode a write) and reads `option_access` on the schema field
  there. This also keeps the field map inside the §5.3 depth-4 cap, which
  is already at its limit. If a genuine layout-side case appears
  post-v1.0, it is an additive catalog key at the ENTRY level (not the
  field level, which has no depth budget left) — normal additive
  evolution, not a break.
- **`replay_depth` (catalog entry key 13, RFC-017)** exists in
  `catalog.cddl` but is NOT yet in the data model — the one remaining
  CDDL↔struct gap after M2b. It lands with the log channel work, since
  that is its only consumer.
- **Deferred cleanups, recorded so they are not lost:** (a) `CborWriter`
  wants a MEASURING mode (count bytes, no buffer) — it would remove
  `checkCatalog`'s scratch-overload wart and serve RFC-028's
  know-the-size-before-you-allocate rule generally; (b) `BasicCatalog`'s
  five positional capacity parameters should become a single
  `CatalogCaps` class-type NTTP if a sixth pool ever appears; (c)
  `estop_frame.hpp` hand-rolls an `EstopCause` enum that the registry now
  owns as `safety_causes` — **DONE (M4a): the enum is deleted; callers use
  `slopsync::safety_causes::`.**

### OPERATOR DECISION — RESOLVED 2026-07-25
- **`/uitoken` is the SECOND sanctioned HTTP escapee.** Ruling: it is a
  sideband, not a secondary cost — a convenience for devices that host a
  WebUI, never a requirement, never a connection prerequisite for any
  other client, and it does not break SlopSync for a hub with no WebUI.
  The standing ruling at the head of this file is amended accordingly
  (static assets + OTA + uitoken, nothing else, ever). No RFC text
  changes: 029 §4 stands as written.
- **This pass has no remaining open questions.**

---

## RFC-030 — Curve family on the stream: say WHICH spline the segments describe

- **Status:** **LANDED (2026-07-27)** — as a **publishes-wish key, not the
  stream_meta INTENT** (operator-approved variant: RFC-013's PUBLISH frame
  already provides mid-session renegotiation, deleting the RFC's only argument
  against the wish-key home). Shipped: registry `curve_families` table + CBOR
  key 45 on publishes/granted_publishes entries; the GRANT echoes the
  **EFFECTIVE** family via `HubDelegate::effectiveCurveFamily` (answers M-2's
  "honoured vs silently downgraded" open question — a ForceC1/C2 machine
  reports the forced family, never parrots); `slopmotion::Command::
  client_curve_family` resolves `CurvePolicy::FollowClient` at last (c1_cubic
  → cubic reconstruction; everything else = pre-RFC quintic); firmware stamps
  each pacing-ring segment with its session's granted family. Test SI-23.
  Honest scope notes: `step` (3) is declarable but renders as quintic (no
  step renderer exists); the MFP plugin's declaration + WireSelfTest lockstep
  needs its mandatory twice-back-to-back bench run before the plugin side
  counts as verified.
- **Origin:** Operator, 2026-07-25/27. The `main`-branch firmware treated TCode
  v4 as the gold standard because it passed an interval `I` and a slope `G`
  alongside each segment, letting the device reconstruct the sender's
  interpolation instead of inventing one. Segments mode (`0x0085`) restored the
  data but not the *declaration*, and fw 2.1.70 shipped a machine-side
  `curve_policy` override (`follow client` / `force C1` / `force C2`) with
  nothing on the wire for `follow client` to actually follow.
- **Problem:** `{target, duration_ms, end_vel}` uniquely determines a cubic
  Hermite, so a segment stream is a COMPLETE encoding of the sender's curve —
  but only if both ends agree on the curve FAMILY. Pchip and Makima differ only
  in their knot-tangent rule, so given endpoint positions and tangents they
  produce the same cubic; a C2 quintic, however, **cannot** reproduce a C1 cubic
  across a knot, because the script's acceleration genuinely STEPS there. The
  device currently estimates `af` as a backward difference of consecutive
  handoff velocities — an estimate of a quantity that is two-valued at the knot.
  When the sender is C1 (Linear/Pchip/Makima/Step in MultiFunPlayer) that
  estimate is not merely imprecise, it is estimating something that does not
  exist, and the machine smooths a corner the author put there on purpose.
  Measured: forcing C1 gives a stepped `a(t0)` with alternating sign and 3-6x
  lower in-span jerk than the quintic reconstruction of the same script.
- **Proposed change:**
  1. **A `curve_family` declaration, per stream, not per sample.** It is a
     property of the SOURCE, changes only when the user changes interpolator,
     and putting it in every 4-8 byte sample would be a per-sample tax on a
     per-session fact. Two candidate homes, and the second is preferred:
     (a) a HELLO/`publishes` wish annotation, or
     (b) **a `stream_meta` field on the GRANT-side channel descriptor**, set by
     a small c2h INTENT so it can change mid-session without a reconnect (a user
     switching Pchip -> Makima in MFP mid-scene must not drop the stream).
  2. **Registry `curve_families` enum**, small and honest about what it can
     express: `unspecified` (0, the compatible default — behave exactly as
     today), `c1_cubic` (1), `c2_quintic` (2), `step` (3). NOT a taxonomy of
     every interpolator anyone has ever written: the wire needs the SMOOTHNESS
     CLASS the reconstruction must honour, not the vendor's algorithm name.
     Pchip and Makima are both `c1_cubic` and that is the correct answer.
  3. **`unspecified` MUST behave as v1.0 does today**, so every existing client
     keeps working and this is purely additive.
  4. **The machine override outranks the declaration** (`follow client` /
     `force C1` / `force C2` on 0x008D `curve_policy`, already shipping). A
     machine is allowed to say "I don't care what you sent, do it this way" —
     that is a safety and feel decision belonging to whoever is strapped to it.
  5. **NO CLAMPING SEMANTICS ARE IMPLIED.** Operator ruling, verbatim: *"why
     bother clamping, at that point we'd just make makima pchip again, there's 2
     settings, allow the user to pick, don't dictate how they should use it."*
     Overshoot handling stays the machine's existing window/feasibility
     machinery; this RFC only declares the family.
- **Compatibility:** Fully additive — a new registry enum, one optional
  descriptor field, one optional INTENT. Absent = `unspecified` = current
  behaviour, so no existing client, catalog or golden vector changes. The
  device-side consumer already exists (`slopmotion::CurvePolicy`), which is why
  this is a wire proposal and not a feature proposal.

---

## RFC-031 — Servo register configuration: the last HTTP writer

- **Status:** Draft — feature PARKED, mechanism REQUIRED. Original deferral
  (operator, M5c): *"I don't use the servo tuning at the moment, we'll
  re-introduce later as it was always broken lol."* **Amended by operator
  ruling 2026-07-27:** register read/write-STYLE communication is a shape
  SlopSync must support. The servo pane itself stays parked, but item 5 below
  is accepted-in-principle and waits only for a consumer.
- **Origin:** M5c (fw 2.1.72). The ruling is **"no controls outside SlopSync,
  HTTP is read only"**, and `POST /api/servo` was the last writer standing after
  the motion, mode, tuning and admin surfaces moved. It is retired (410 Gone)
  rather than ported, because porting a surface nobody uses and that never
  worked properly would have meant designing its protocol shape under time
  pressure, for a feature with no user.
- **Problem:** Servo config is NOT shaped like the other writers. `clear_fault`
  and `save_config` are verbs and fit an op-select exactly (0x0106). But
  `/api/servo` accepted `{"live":{"<reg>":val,...}}` and `{"program":{...}}` —
  an **arbitrary register->value map** over a Modbus device. That is not a fixed
  INTENT schema, and forcing it into one would either pin every register number
  into the catalog forever or reintroduce an untyped escape hatch, which is the
  thing SlopSync exists to avoid.
- **Proposed change:** Split it by what the data actually IS, rather than by
  which endpoint it used to share.
  1. **The `live` whitelist becomes real settings.** It is a bounded, known set
     of tunable registers, so it becomes a STATE+INTENT settings pair with
     RFC-009 annotations (`min`/`max`/`step`/`desc`/`group`). That makes it
     render generically, validate client-side, and echo post-clamp like every
     other setting — and it makes the Configure pane buildable by a third-party
     client, which the HTTP version never allowed.
  2. **`program` (the full gold-motor sequence) is a DOCUMENT, not a form**, and
     belongs on the RFC-021 blob store: one `writeBlob` on the delegate seam
     inherits chunking, selective repair, `total_bytes` pre-sizing and
     `CHUNK_UNAVAILABLE` for free. A register dump is exactly the shape that
     seam was generalized for.
  3. **`scan` is already done** — `0x0106 machine-admin` op 3, shipping.
  4. **Gate on `has_rs485`**, per RFC-016: a machine with no Modbus servo must
     not advertise these channels at all. Their ABSENCE is the honest answer to
     "can this device configure a servo?", exactly as 0x0087 power already
     works.
  5. **(Operator ruling 2026-07-27) Raw register access is a bounded
     DIAGNOSTIC plane, distinct from settings.** Item 1 covers KNOWN tunables;
     this covers the engineering case item 1 cannot: reading or poking an
     arbitrary register during bring-up or fault hunting. Shape:
     - A device INTENT channel at `configure` access: op-select
       `{read, write}` + `addr u16` + `value u16`. The catalog's `addr`
       min/max is the RENDERING hint; the hub is the referee for the real
       (possibly disjoint) whitelist ranges via NACK `INVALID_VALUE` —
       exactly RFC-009.6, no new mechanism. A generic client already renders
       this: op-select + two bounded numeric boxes.
     - **Results ride a paired EVENT channel `{addr, value, status}`, never
       the ECHO — structurally, not stylistically.** The hub emits ECHO the
       moment `applyIntent` returns (§9.3 path, `hub_impl.hpp`), but the bus
       transaction is QUEUED to servoBusTask (`ServoModbus::queueWrite`
       already exists) and the wire has not been touched yet when that echo
       leaves. So the ECHO honestly means "accepted and queued"; the EVENT
       carries the bus truth when the transaction completes. Making the
       delegate block on a Modbus round trip inside the hub task's 5 ms tick
       is the alternative, and it is prohibited by construction.
     - Writes report the post-READBACK value in the EVENT (write, then read
       the register back, publish what the hardware answered) — the
       ground-truth doctrine extended to a plane the settings shadow cannot
       reach.
     - No new registry vocabulary is needed; this is a spec AUTHORING PATTERN
       (an appendix worked example), not a new channel class. It is
       peripheral-agnostic by design: any register-file device a hub fronts
       (Modbus today, an I2C peripheral tomorrow) reuses the same shape.
- **Compatibility:** Additive when it lands. `POST /api/servo` is ALREADY gone
  as of fw 2.1.72 — this RFC does not remove anything, it describes what
  replaces it. Until then the servo surface is read-only (`GET /api/servo`
  survives as a diagnostic per the read-only rule), which is an accurate
  reflection of a feature the operator does not currently use.


## RFC-032 — `command.*` and `telemetry.target`: make commanded motion discoverable

- **Status:** **LANDED (2026-07-27)** as written. Registry `field_roles` gained
  `command.position` (opening the `command.<quantity>` family) and
  `telemetry.target`; the device catalog tags `position` on 0x0100 and
  `tgt_10um` on 0x0080. Client side needs zero code (`model/settings.js`
  already indexes roles) — live verification of the rail tape + commanded/lag
  numerals is on the WebUI agent (WEBUI-HANDOFF-RFC-BATCH.md item 2).
- **Origin:** WebUI rebuild, 2026-07-27. The rebuilt page renders entirely from
  the catalog and is forbidden from naming a channel id. When the rail widget
  went to wire up tap-to-move it found nothing it could bind to and — correctly —
  REFUSED, rendering its input tape disabled with the text "this catalog does not
  tag a move INTENT by role, so a generic client cannot find it safely". The same
  search killed the `commanded` and `lag` hero numerals.
- **Problem:** Two related holes, both in the `field_roles` vocabulary.
  1. **No role names a value-bearing COMMAND.** `action.<name>` (RFC-019) marks
     VERBS — home, e-stop, clear-fault — and a client renders them as buttons.
     `0x0100 move`'s field is `position`: a VALUE. Tagging it `action.move` would
     be actively wrong, telling every generic client to draw a button where a
     position control belongs (the device catalog's own comment says exactly
     this, and declining to tag it was the right call). So there is no honest way
     to annotate it, and therefore no way for any client but ours to command a
     move.
  2. **No role names the COMMANDED position.** `telemetry.position` is measured
     truth; nothing names the setpoint. The device publishes `tgt_10um` directly
     beside `pos_10um` on 0x0080 and it is unannotated, so a generic client can
     show where the carriage IS but never where it was ASKED to be — and so
     cannot show lag either. Deriving "commanded" from the stroke window would be
     fabrication, which the Ground Truth Doctrine forbids.

  Net effect: the most-used control on the machine — put the carriage there — is
  reachable only by a client that hardcodes `0x0100`. That is precisely the
  privilege this project exists to delete.
- **Proposed change:** Two additive `field_roles` entries. No new frames, no new
  keys, no channel changes.
  1. **`command.position`** — an INTENT field carrying a commanded ABSOLUTE
     target position in the channel's own unit. A client that finds it MAY render
     a positional control (rail, tape, slider) and send the value on that field's
     channel. Deliberately a VALUE role, not an `action.*`, so RFC-019's
     verb/value distinction stays intact.
  2. **`telemetry.target`** — the position the machine is currently commanded to,
     as opposed to `telemetry.position` which is where it measurably is. LAG IS
     NOT A SEPARATE ROLE: it is target − position, computed client-side.
     Registering a third field for a subtraction would invite two sources of
     truth for one number.
  3. Tag the reference device: `position` on `0x0100 move` gets
     `command.position`; `tgt_10um` on `0x0080 motion` gets `telemetry.target`.
- **Why a `command.*` family rather than a one-off:** the shape recurs the moment
  anyone adds a second commandable quantity (a commanded velocity; a commanded
  force on a machine that has one). Opening the namespace now, with
  `command.position` as its first member, costs nothing and avoids a rename
  later. Convention: `command.<quantity>` names an INTENT field whose value IS
  the setpoint, and it generally has a `telemetry.*` counterpart to pair with.
- **Compatibility:** Purely additive vocabulary. Unknown roles must already be
  ignored, so a client that does not know these is unaffected, and a machine that
  does not tag them behaves exactly as today (the rail degrades to a window
  editor with no tape — what ships now). No wire-number changes, no frozen
  artifact touched. Client support already exists: `webui/src/model/settings.js`
  indexes non-action schema-field roles into `byRole`, so both light up the
  moment a device advertises them.

---

## RFC-033 — An unacceptable SUBSCRIBE MUST be answered, never silently dropped

- **Status:** **LANDED (2026-07-27).** Root cause found in review: BOTH night
  failures were one bug — `handleSubscribe`'s silent `return` on decode
  failure, hit through the never-registered 16-wish decoder cap
  (`kSubscribeMaxWishes`); the "mixed STATE+EVENT" theory was a red herring
  (the concatenated list simply exceeded 16). Shipped: NACK
  `SUBSCRIBE_REJECTED` (0x0204) with reason in `detail`;
  `max_subscriptions_per_frame` (16) registered and advertised in WELCOME
  `limits` key 4; item 4's ruling recorded — **mixing classes is LEGAL and
  always was**; negative vector SI-21 (17 wishes → NACK, then a legal
  subscribe still grants). The probe's subscribe-everything case rides the
  tooling pass.
- **Origin:** WebUI rebuild, 2026-07-27, live against fw 2.1.73 then 2.1.74.
- **Problem:** A SUBSCRIBE the hub will not accept produces **nothing** — no
  GRANT, no NACK, no EVENT. The session completes HELLO/WELCOME, adopts the
  catalog, reaches LIVE and looks perfectly healthy, while zero STATE ever
  arrives. Every readout renders `--` and every control correctly greys out (a
  control cannot be enabled without a snapshot to gate it against). It presents
  as a CLIENT RENDERING BUG and is a protocol-etiquette failure.

  Two distinct triggers were hit, both invisible:
  1. **A frame mixing STATE and EVENT subscriptions** was dropped wholesale.
     Splitting them into separate frames fixed it.
  2. **A frame with too many entries.** After the catalog grew from 33 to 44
     entries, batches sized from the advertised `max_frame` grew with it and the
     drop returned. A fixed conservative batch of 8 fixed it.

  Note the second failure was introduced BY THE FIX FOR THE FIRST. That is how
  easy this is to get wrong when the protocol gives no feedback.

  The reference probe caught neither: it subscribes to 9 STATE channels and has
  always sat inside both limits. The simulator hid them too. **A conformance
  suite that only exercises the happy path cannot find this class of bug.**
- **Proposed change:**
  1. **Normative:** a hub that cannot honour a SUBSCRIBE MUST respond — either
     GRANT what it accepted and NACK the remainder, or NACK the frame. Silence is
     non-conformant. Partial acceptance is already the observed behaviour for
     individually unauthorized channels (a `configure` channel requested at
     `control` is denied per-channel, not fatally), so this mostly makes existing
     good behaviour mandatory and closes the fatal cases.
  2. **A registered NACK code** — `SUBSCRIBE_REJECTED` — with `detail` carrying
     the reason (too many entries / frame too large / mixed classes).
  3. **Register the actual constraints.** If a hub limits entries-per-frame or
     forbids mixing channel classes, that MUST be discoverable — e.g.
     `max_subscriptions_per_frame` in WELCOME `limits`, beside the existing
     `max_frame` and `max_subscriptions`. Today a client can only find the limit
     by binary-searching against a live machine.
  4. **Decide the mixed-class question.** Either mixing STATE and EVENT in one
     SUBSCRIBE is legal (and the reference hub has a bug) or it is illegal (and
     the spec must say so). Right now it is neither.
  5. **Conformance:** add a negative vector — subscribe to more channels than the
     hub allows, assert a NACK. The probe should also grow a subscribe-everything
     case, since "subscribe to every channel the catalog advertises" is the
     natural thing a generic client does and is exactly what nothing tested.
- **Compatibility:** Additive (one NACK code, one optional limits key) plus a
  behavioural requirement on hubs. Clients ignoring the new NACK are no worse off
  than today. The reference hub needs the fix; that is the point.

---

## RFC-034 — Placeholder entries in `options` lists

- **Status:** **LANDED (2026-07-27) via option 3, not option 1** — review found
  option 1's "gate at a level nobody holds" cannot deliver: `AccessLevel` tops
  out at `configure`, which real admin sessions hold, so a configure-tier
  client still saw an enabled "reserved" button on 0x0009. The normative rule
  is now: for a select field carrying an `action.*` role, wire value 0 is
  NEVER an operation unless the governing op table defines op 0; clients MUST
  NOT render index 0 as actionable. Strict `option_access` on index 0 stays as
  defense-in-depth (already shipped on every device op-select). Reference
  client swaps its English-guessing regex for the index-0 rule
  (WEBUI-HANDOFF-RFC-BATCH.md item 5).
- **Origin:** WebUI rebuild, 2026-07-27, seen live in the safety bar.
- **Problem:** Op-select INTENT fields are index-aligned with their wire value,
  and every registry op table starts numbering at 1. Index 0 therefore exists
  only to keep the array aligned and carries a filler label — `"reserved"`. A
  generic client renders `options` faithfully and so draws a **pressable button
  labelled "reserved"** that means nothing and, if pressed, earns a NACK. The
  reference client currently filters it with a label heuristic
  (`/^(reserved|none|unused)$/i`), which is a guess about English, not protocol.
- **Proposed change:** One of, in preference order:
  1. **Gate it with `option_access`** at a level nobody holds. `0x0009
     session-admin` ALREADY does exactly this for its own index 0 — so this is an
     existing pattern the reference device applies inconsistently, not a new
     mechanism. Needs no wire change, just discipline plus a normative SHOULD so
     other implementers do it too.
  2. A registered sentinel label the spec blesses, so filtering is conformant
     rather than a guess about English.
  3. Explicitly bless index 0 as never-an-operation for op-select fields.

  (1) is preferred: it reuses shipped machinery and renders the control GREYED
  rather than vanished, matching the "grey, never hide" doctrine.
- **Compatibility:** Fully additive. Option (1) is a catalog authoring change on
  the device with no wire-format impact at all.

---

## RFC-035 — A role vocabulary for motion-plan telemetry

- **Status:** **LANDED (2026-07-27).** Registry `plan.*` family
  (start/end/current/velocity/elapsed/duration/style) + all seven 0x0086
  fields tagged. The reference client's `/plan/i` heuristic demotes to a
  fallback-for-roleless-hubs (WEBUI-HANDOFF-RFC-BATCH.md item 3).
- **Origin:** WebUI rebuild, 2026-07-27, building the plan-strip widget.
- **Problem:** `0x0086 plan-strip` publishes genuinely useful data (the segment
  in flight: start/end/current normalized position, velocity, elapsed and total
  duration, style). None of it carries a role, and no vocabulary could describe
  it. A generic widget therefore cannot find it. The reference implementation
  resorts to matching the catalog ENTRY NAME against `/plan/i` and classifying
  sub-fields by regex over their `name` and `desc` — which works, is documented
  in the source as a heuristic, and is exactly the guessing this protocol exists
  to eliminate. It will silently fail on a machine that names the concept
  differently.
- **Proposed change:** A small `plan.*` role family covering what is genuinely
  portable across jerk-limited planners — e.g. `plan.start`, `plan.end`,
  `plan.current`, `plan.velocity`, `plan.elapsed`, `plan.duration`, `plan.style`.
  Deliberately NOT a description of any one planner's internals: the test for
  inclusion is "would a different machine's motion planner have this concept?",
  the same test that kept Advanced-pattern internals out of `pattern.*`.
- **Compatibility:** Additive vocabulary; absent roles keep today's behaviour
  (the widget renders nothing, which is correct for a machine with no planner).

---

## RFC-036 — Renderability of string settings

- **Status:** **LANDED items 1+3 (2026-07-27); item 2 (`max_len`) DEFERRED** —
  registering an annotation nothing emits or needs yet is exactly how RFC-007
  said registries accrete dead weight. The probe's `cat_renderable` now treats
  str16/32/64 as renderable by type (width = the bound); the exercised fixture
  is the SIMULATOR's divergent catalog, never the frozen mini-catalog (whose
  etag pin a string setting would break).
- **Origin:** Found by `tools/slopsync_probe.py` against the divergent simulator
  catalog, 2026-07-27 — the FIRST time a `str16` setting field was ever
  exercised. RFC-026 landed the packed string types and nothing had used one.
- **Problem:** The probe's `cat_renderable` check requires every setting to carry
  either `options` or numeric `min`/`max`, and fails a string field that has
  neither. A string's bound is its fixed packed width (16/32/64 B), implied by
  its TYPE and not expressible as a numeric min/max. So a perfectly conformant
  string setting fails conformance.
- **Proposed change:**
  1. Fix the check: a `str16`/`str32`/`str64` field is renderable by virtue of
     its type; its length bound is the type's width.
  2. Consider a `max_len` annotation for a device wanting a SHORTER logical limit
     than the field's physical width — RFC-009 item 5 already mentions `max_len`
     as a UI hint, but nothing registers or emits it.
  3. Add a string setting to the conformance fixtures so this path stays
     exercised rather than being rediscovered by the next implementer.
- **Compatibility:** Tooling and optional-annotation only; no wire impact.

---

## RFC-037 — Forward-decodable packed layouts: explicit per-field width

- **Status:** **PARTIALLY LANDED (2026-07-27)** — the vocabulary half: catalog
  key 18 `size` registered (registry + catalog.cddl + SPEC), decode rule
  specified (prefer declared width; unknown type + declared size = skippable
  hole), client decode rule handed to the WebUI agent. **The named follow-up:
  the reference catalog ENCODER does not emit key 18 yet** — emission is a
  per-field byte cost the encoder should take in one deliberate pass (with the
  conformance declared==derived check landing alongside), not a rider on this
  batch. Until then the key is registered, decodable, and unexercised —
  exactly the state RFC-036.3 warns about, so the follow-up carries a "add an
  emitting fixture" obligation with it.
- **Origin:** Grievance sweep 2026-07-27. `webui/src/core/slopsync/catalog.js:470`
  (*"unknown packed type: offsets are unknowable past here"*) and
  `clients/mfp-slopsync/SlopSync.cs:2859` (*"An UNKNOWN packed type makes every
  later offset unknowable, so we stop there rather than silently mis-decoding
  the tail"*) carry the identical defensive truncation. The probe's 0x0088
  misread (80 B struct silently accepted an 84 B grown payload, every field
  after the growth point read one slot early) is the hardcoded-client face of
  the same disease.
- **Problem:** A packed field's byte width is derivable ONLY from its `type`.
  The moment the registry adds packed type 11, every existing client that meets
  it must stop decoding the layout THERE — not just the unknown field, the
  entire tail — because later offsets are unknowable. Append-only evolution is
  the protocol's own growth mechanism, and it strands exactly the conforming,
  catalog-decoding clients it was designed for.
- **Proposed change:** catalog layout fields gain an explicit `size` key
  (u8, bytes). Decoders prefer the declared size and fall back to type-derived
  width when absent; an unknown TYPE with a declared SIZE is a skippable hole
  instead of a decode wall. Conformance checks declared-vs-type width
  agreement for known types (a mismatch is an authoring error). One uint per
  field against a 4096 B entry cap is noise.
- **Compatibility:** Additive catalog key (catalog.cddl + registry). Absent =
  today's behaviour. This is the single highest-leverage "works everywhere"
  change in the sweep: it makes every FUTURE registry addition non-breaking
  for every PAST client.

---

## RFC-038 — Client-negotiated deadman window

- **Status:** **LANDED (2026-07-27).** HELLO key 44 `deadman_wish_ms`; hub
  clamps into the registry bounds and applies PER SESSION
  (`HubSession::deadmanMs`, enforced by pumpDeadman); WELCOME key 24 echoes
  the applied value exactly as it always did. Test SI-22 (over-max clamps
  down, under-min clamps up, absent = default). The browser client's wish is
  on the WebUI agent (handoff item 7).
- **Origin:** Grievance sweep 2026-07-27. `webui/src/model/machine.svelte.js:341-360`
  ("The alt-tab problem"): browsers throttle background-tab timers, PINGs stop,
  the 600 ms deadman evicts the session — *"to the operator this reads as
  'alt-tabbing kills the page'"* — and the only client-side remedy is a
  `visibilitychange` reconnect hack.
- **Problem:** The deadman window is hub-dictated. WELCOME key 24 already
  echoes the APPLIED per-session deadman and the registry already bounds it
  (`deadman_min_ms` 250 / `deadman_max_ms` 5000) — but HELLO carries no wish,
  so a client that KNOWS its liveness cadence is coarse (a browser, a BLE
  client on a slow connection interval) cannot ask for the window it can
  actually honour. Every such client either hacks around eviction or floods
  PINGs.
- **Proposed change:** optional HELLO key `deadman_wish_ms`; hub clamps into
  `[deadman_min_ms, deadman_max_ms]` (a hub MAY clamp tighter) and echoes the
  applied value via the EXISTING key 24 — post-clamp echo, ground-truth
  doctrine, zero new response plumbing. §11.3's loss policy is untouched: this
  negotiates WHEN the deadman fires, never WHAT it does. A source-owning
  session's wish is still bounded by the registry max the operator already
  accepted.
- **Compatibility:** One additive HELLO key. Absent = hub default = today.

---

## RFC-039 — Every refusal is answered (RFC-033's principle, generalized)

- **Status:** **LANDED (2026-07-27), one honest asymmetry.** Codes
  `BLOB_REFUSED` (0x0503) and `IDLE_REAPED` (0x010C) registered; idle reaping
  now GOODBYEs with its own code (the hub_impl comment that argued against a
  distinct code is rewritten with the counter-argument that won: observers,
  not the client, needed the distinction). Item 3 turned out narrower than
  drafted: a wrong-shape token already fails HELLO decode, and hub_impl was
  ALREADY answering NACK MALFORMED there — the silent-demotion case is a
  well-FORMED but unrecognized token, which is RFC-029's deliberate
  admit-at-watch tripwire behaviour and stays. The asymmetry: BLOB_REFUSED is
  a CLIENT obligation and only slopsync-js has a reassembler cap to refuse
  with — that emission is on the WebUI agent (handoff item 8); the C++ client
  core sizes its scratch from its own build and structurally cannot hit it.
- **Origin:** Grievance sweep 2026-07-27, three receipts:
  1. `webui/src/core/slopsync/catalog.js:131-137` — the client's blob
     reassembler cap refused a grown catalog's transfer header and the session
     *"then went LIVE WITH NO CATALOG… No error, no NACK, no dropped-frame
     warning: a refused blob header just stops."* (RFC-015's READY_TIMEOUT
     eventually kills the session 15 s later — and blames the client.)
  2. `clients/mfp-slopsync/SlopSync.cs:536` — a HELLO token of the wrong
     shape (a PIN typed where a 16 B token belongs) is silently ignored and
     the session downgraded to viewer tier: *"Under enforcement that would
     present as 'connects, plays nothing'."*
  3. `lib/slopsync/hub/hub_impl.hpp:3056-3058` — idle reaping (RFC-024) has
     no GOODBYE code of its own, so a reaped VIEWER is labelled
     `DEADMAN_TIMEOUT` — the motion-safety code — in every log and client.
     The comment says *"flagged rather than invented"*; this RFC invents it
     properly.
- **Proposed change:**
  1. Normative umbrella sentence in SPEC §4: silence is never a conforming
     response to a frame or transfer an implementation cannot honour — this
     generalizes RFC-033.1 from SUBSCRIBE to the whole surface.
  2. A client that cannot accept a declared blob (`total_bytes` over its cap)
     MUST GOODBYE with new code `BLOB_REFUSED` rather than idle in a
     half-session; hubs SHOULD log it with the declared size.
  3. A HELLO carrying a token field that is PRESENT but malformed (wrong
     length/type) is NACK'd `UNAUTHORIZED` — never silently demoted.
     Tokenless HELLO keeps its legitimate watch-tier path; only present-but-
     broken credentials become loud.
  4. New GOODBYE code `IDLE_REAPED`, distinct from `DEADMAN_TIMEOUT`, so a
     motion-safety timeout is never confused with housekeeping.
- **Compatibility:** Two additive registry codes + normative text + small hub
  behaviour changes. Clients ignoring the new codes see today's behaviour.

---

## RFC-040 — Spec says what the reference implementation knows (editorial batch)

- **Status:** **LANDED (2026-07-27)** — spec text for all four rules (frame-
  header channel table, WS subprotocol-echo MUST, ECHO key-completeness,
  role cardinality). Zero wire numbers, as designed.
- **Origin:** Grievance sweep 2026-07-27, receipts inline.
- **Proposed change:**
  1. **Frame-header channel table.** Which frame types carry
     `header.channel == 0` vs a target channel id is normative routing that
     exists only in the reference implementation
     (`tools/slopsync_probe.py:33-41`: *"confirmed against the reference C++
     impl, not spelled out explicitly in SPEC.md prose"*). SPEC §4 gains the
     per-frame-type table.
  2. **WS subprotocol selection is an obligation.** §13.2 names `slopsync.v1`
     but never says the server MUST perform RFC 6455 selection and echo it —
     two independent WS libraries (firmware's vendored ESP32Async patch, the
     sim's IXWebSocket patch) had to be patched because strict clients
     hard-fail without the echo. One MUST sentence.
  3. **ECHO key-completeness.** ECHO carries every key from the intent's value
     map that the hub applied; a key ABSENT from the ECHO means NOT applied,
     and clients MUST fall back to reported truth for it
     (`webui/src/model/shadow.svelte.js:114` already behaves this way —
     codify it so "silently accepted" and "silently ignored" are
     distinguishable on every hub).
  4. **Role cardinality.** A registered role SHOULD appear on at most one
     field per catalog; a client meeting duplicates binds the first in
     catalog order, deterministically (`webui/src/model/roles.js:115` already
     does; make the tiebreak conformant rather than client-local).
- **Compatibility:** Editorial + conformance notes. No wire change anywhere.

---

*Add new entries below. Keep the shape: Status / Origin / Problem / Proposed
change / Compatibility — and if it was found by a probe or a live failure,
say exactly which, future-us will want the receipts.*
