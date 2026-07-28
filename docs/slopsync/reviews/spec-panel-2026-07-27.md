# Spec Fresh-Eyes Panel — 2026-07-27

Method: 15 independent vacuum reads of [SPEC.md](../SPEC.md) (readers forbidden any other repo context), haiku-class reviewers, sonnet convergence with a >=4-reader bar for 'consistent.' Raw reader reports retained off-repo. Evidence document — feeds RFC drafting, is not itself normative.

**Resolution status (added after the fact — the panel text below is untouched):**
every item in "Consistently hated" was closed by
[RFC-049](../RFC-QUEUE.md#rfc-049--spec-fresh-eyes-panel-omnibus-small-normative-fixes)
(the omnibus this panel fed) or [RFC-050](../RFC-QUEUE.md#rfc-050--blob-transfer-backpressure--completion-acknowledgement),
except the H11 scheduling-depth backstop (evaluated, reverted — see
[SPEC.md](../SPEC.md)'s `commitWaveform()` note) and `source.background_run`,
which shipped separately as Phase D. See each finding's heading below for its
specific fix.

## Convergence summary

Note on the input: the task framing says 16 readers, but the REPORTS array actually contains 13 distinct report objects (verified by parsing) — all counts below are out of 13, and the >=4 threshold is applied as an absolute count per the instructions regardless. The convergence is unusually strong for a document this dense: the segment-exception shedding rule (§10.4) is loved by literally every reader, and the honesty-clause mechanism plus the closed three-mode motion surface are each cited by 12 of 13 — this spec's core safety/extensibility architecture is not in dispute. What's consistently hated is not the architecture but its follow-through: a cluster of features that are specified, numbered, and made to sound normative but are not actually built in the reference hub (curve_family=step, source.background_run, and by extension the broader pattern of §18 'known limitations' items), plus a second cluster of accepted-but-incomplete safety guards whose coverage is narrower than their framing implies (the H11 handoff lookahead bound, per-hop relay ESTOP buffering with no cap, permissive blob-namespace grammar), plus a third cluster of just-plain-vague normative language (pairing-ceremony thresholds, backpressure signaling) that the spec's own registry discipline and honesty-clause pattern could trivially fix if applied consistently. The improvement path readers themselves point to is not new invention — it's applying practices they already praised elsewhere (name the gap as an honesty clause or known limitation; pin the constant in the registry; make the fallback behavior a deterministic table like shedding; let the catalog self-describe hub behavior) to the handful of spots where that discipline lapsed.

## Consistently hated

### curve_family=step is registered/declarable on segment streams, but the reference hub has no step renderer and silently echoes quintic instead  
**6/15 readers** — sections: §9.6, §18-20/§18-21

> **RESOLVED** by [RFC-049](../RFC-QUEUE.md#rfc-049--spec-fresh-eyes-panel-omnibus-small-normative-fixes)(b): `requested_curve_family` (CBOR key 48) now echoes the client's ask verbatim alongside the effective, possibly-downgraded `curve_family` (45) — exactly the fix this section's improvement lead names.

> curve_family=step is allocated and declarable but reference hub has no step renderer... Why allocate a value that cannot be acted on or tested? This feels like premature reservation that should either be implemented or deferred.

> A client that declares curve_family: step and receives echo granted_publishes with effective c2_quintic knows its declaration was downgraded, but the spec does not clearly say whether this is a machine-policy override... or a transient state.

> A client sends curve_family: step, the hub echoes back c2_quintic... The client is left wondering if its declaration was honored or downgraded.

**Improvement leads (mined from the liked list):** Readers praise the Honesty Clauses (H1-H12, liked by 12/13) and the §18 Known-Limitations listing precisely for naming gaps instead of hiding them — apply that same discipline consistently here: either implement the step renderer before tagging v1.0 normative, or explicitly demote curve_family=step to experimental/informative status. Also borrow the widely-loved ground-truth ECHO pattern (10/13, 'ECHO reports applied/post-clamp values, never requests') by adding an explicit requested_curve_family alongside effective_curve_family so a downgrade is visible rather than inferred.

### source.background_run (RFC-045/048 deadman setting for hub-autonomous sources) is specified but not shipped by the reference firmware, and the command-driven/autonomous split feels unintuitive or unverified  
**6/15 readers** — sections: §11.3, [RFC-045](../RFC-QUEUE.md#rfc-045--retire-deadman-as-safety-session-liveness-is-bookkeeping-not-motion-control), [RFC-048](../RFC-QUEUE.md#rfc-048--the-rendering-constitution-catalog-vocabulary-capability-interfaces-renderer-law), §18-21

> **RESOLVED**: `source.background_run` shipped on the reference firmware's `pattern-state` channel (Phase D), discoverable from the catalog exactly as the improvement lead below asks.

> The source.background_run setting (RFC-048) should handle this but is unimplemented (§18-21).

> §18-21 admits the reference hub doesn't expose it and behaves as `true` unconditionally. This is a spec-implementation gap that makes conformance ambiguous.

> Clients cannot discover whether a hub's generator actually stops on deadman or continues. Creates a conformance cliff: specification vs implementation diverge.

**Improvement leads (mined from the liked list):** Same Honesty-Clause/Known-Limitations discipline readers loved, plus the widely-liked settings-metamodel/field-roles pattern (§8.8, part of the near-universally liked catalog design) — that pattern already lets any hub-added setting self-describe to clients with zero hardcoding. Reviewers explicitly want source.background_run exposed the same way: discoverable from the catalog, not something a client has to assume or read a companion doc to understand.

### Segment handoff sanity bound (H11) only catches pathological end-velocity when the successor segment is already scheduled — long, sparse segments have no backstop  
**6/15 readers** — sections: §9.6, H11, §18-1

> **PARTIALLY RESOLVED** by [RFC-049](../RFC-QUEUE.md#rfc-049--spec-fresh-eyes-panel-omnibus-small-normative-fixes)(c): the `k = 1.5` constant is now the registry-pinned `segment_handoff_k` (first half, landed). The scheduling-depth backstop itself (second half) was implemented, then reverted — it measurably worsened a characterized motion defect via an unverified control-loop interaction. Left open; see `slopmotion.hpp`'s `commitWaveform()` comment.

> This is a real correctness gap (long segments from sparse senders can violate the guard) accepted because the alternative is worse... Stating it is good; accepting it still feels like a limitation left in place.

> The `k = 1.5` constant for end-velocity bounding is in the reference implementation, not the registry... A competing implementation trying to match behavior has no authoritative source for the clamping constant.

> Long segments bypass the end-velocity guard. The backup ('the hub's own legality checks') is not as tight as detecting the problem upfront.

**Improvement leads (mined from the liked list):** Readers loved the transport-binding matrix (§13.1) for stating worst-case guarantees explicitly and normatively, and loved the registry discipline ('numbers never renumbered, hand-maintaining a second copy is forbidden') — apply both: pin the k=1.5 clamp constant in registry.yaml like every other wire-visible number, and/or add a hub-side per-source scheduling-depth backstop that doesn't depend on client lookahead discipline.

### Trust ledger first_seen/last_seen are frequently zero because the protocol has no wall clock — an operator gets no useful pairing history  
**4/15 readers** — sections: §7.2, §12.6, §18-12

> **RESOLVED** by [RFC-049](../RFC-QUEUE.md#rfc-049--spec-fresh-eyes-panel-omnibus-small-normative-fixes): §7.2/§12.6 now carry the SHOULD-populate-when-available rule plus an explicit non-audit-grade honesty note — the exact improvement lead below.

> An operator looking at 'when did this device pair' gets boot-relative milliseconds or nothing, not human time... Zero is a valid answer but it's not useful.

> Either mandate that hubs with a real-time source populate these (making the field actually informative) or remove them. Zero is honest but defeats the point of having them.

> A hub with NTP records wall-clock seconds; one without records zero. Multi-hub audits become messy when some entries are wall-clock and others are zero.

**Improvement leads (mined from the liked list):** Apply the same Honesty-Clause naming discipline directly to this field (state plainly it is not audit-grade rather than implying it might be), or borrow the graceful-degradation pattern readers praised in the static-client profile (§8.5, 'degraded-mode rules are explicit') — a SHOULD-populate-when-available rule with an explicit UI signal for which entries lack timestamp context.

### Blob namespace/grammar is too permissive — unregistered namespaces and illegal full+chunks combinations aren't rejected at the grammar level, just fall through silently  
**4/15 readers** — sections: §18-8/9, §8.7

> **RESOLVED** by [RFC-049](../RFC-QUEUE.md#rfc-049--spec-fresh-eyes-panel-omnibus-small-normative-fixes)(e): grammar-level rejection is now stated precisely (an empty `chunks` array, or `ns=0` carrying `store_id`/`slot`, are MALFORMED) and a namespace outside the registered table gets its own NACK `INVALID_NAMESPACE`, split from `CHUNK_UNAVAILABLE` — exactly the distinction this finding asked for.

> An unregistered ns falls through to CHUNK_UNAVAILABLE (correct behavior) but the grammar doesn't reject it... these are edge cases but they're sloppiness—grammar too permissive.

> The grammar doesn't explicitly forbid the illegal combination; a decoder can't reject what cannot be encoded... A union type or explicit boolean flag would be clearer.

> A client sending ns=255 (unregistered) gets the same treatment as ns=0 (catalog)... The client cannot distinguish 'namespace doesn't exist' from 'no chunks available yet'.

**Improvement leads (mined from the liked list):** Readers explicitly praised 'every refusal is answered' (§4.5 NACK philosophy) and parser totality as a hard MUST rather than advisory guidance (liked by 9/13) — apply both here: reject illegal ns/chunk combinations at the grammar level and add an explicit NACK code (e.g. INVALID_NAMESPACE) instead of silent fallthrough.

### Blob transfer pacing and backpressure are advisory/vague, and there's no positive application-level acknowledgement that a transfer completed  
**4/15 readers** — sections: §8.4, §5.6

> **RESOLVED** by [RFC-050](../RFC-QUEUE.md#rfc-050--blob-transfer-backpressure--completion-acknowledgement): §8.4 now carries a normative send/hold/resume/abort decision table keyed to the binding's own congestion signal, plus `BLOB_DONE` as the positive completion signal this finding says was missing — reapplying the shedding-table template the improvement lead names.

> The spec says relays 'MUST respect transport backpressure while pacing BLOB_CHUNK emission,' but there's no normative definition of what that signal is—is it a return code, an exception, a callback?

> Pacing is advisory ('MAY pace')... There's no registry knob for rate or advertised pacing budget. Exponential backoff is unspecified.

> A sender emits chunks, a receiver reassembles and verifies the SHA-256. But the sender never gets a signal that reassembly completed... the sender just... stops and hopes.

**Improvement leads (mined from the liked list):** The near-universally loved shedding table (§10.4, liked by all 13 readers) is exactly the template reviewers want reapplied here — turn backpressure into a deterministic, normative decision table or return-code vocabulary (accepted/retry-later/drop) instead of leaving it binding-specific and advisory.

### Relays 'MUST NOT chain' (one hop max) with no stated architectural reason, and per-hop relay buffering can silently stack up to degrade the end-to-end ESTOP guarantee  
**4/15 readers** — sections: §14.3, §13.1, H2

> **RESOLVED** by [RFC-049](../RFC-QUEUE.md#rfc-049--spec-fresh-eyes-panel-omnibus-small-normative-fixes)(f): §14.3 now states the architectural reason (a chain compounds worst-case latency with no stated ceiling and no routing/loop-protection machinery to bound it) and adds a normative one-relay-hop ESTOP latency budget.

> A 250 ms buffered relay could turn a 50 ms ESTOP guarantee into 300 ms end-to-end without violating the spec. Practical consequence: a relay can silently degrade ESTOP's safety property.

> If a relay's job is frame forwarding + buffer management + optional timestamp correction, why can't two relays chain with each maintaining its own queues? The limitation feels operational... rather than architectural.

> 'One relay hop maximum in v1; multi-hop is a v2 problem nobody currently has.' ... Feels arbitrary.

**Improvement leads (mined from the liked list):** Readers specifically loved the transport binding matrix (§13.1) for declaring worst-case ESTOP delay per binding explicitly and normatively ('H2 is unusual and valuable — it admits that preemption is per-hop, not magic') — extend that exact rigor to relay chaining: either name the compounding-latency rationale explicitly or cap total added relay latency normatively instead of an unexplained MUST NOT.

### Pairing-ceremony edge cases are underspecified — vague thresholds ('~10s' power-cycle gesture), an unjustified 3-strike AUTH limit, and no timeout/fallback when a knock is never approved  
**5/15 readers** — sections: §12.3, §12.3c, §12.4

> **RESOLVED** by [RFC-049](../RFC-QUEUE.md#rfc-049--spec-fresh-eyes-panel-omnibus-small-normative-fixes)(g): `pairing_gesture_boot_count` (3) and `pairing_gesture_max_uptime_ms` (10000) are now registered constants pinning the former "~10 s" hedge; `auth_attempts_max` (3) is likewise a named registry constant shared by both the PIN window and AUTH's strike limit, not two independent magic numbers.

> The specification says N consecutive boots with uptime below '~10 s' arm the pairing window. The tilde is not a normative value—it's hedge language in a normative section.

> A client gets max 3 failed AUTH proofs before the hub stops answering... Why not exponential backoff? Why 3 specifically?

> What if the hub boots with pairing open and no `configure` session ever connects? Do unapproved knocks expire?

**Improvement leads (mined from the liked list):** Readers loved that the registry is the single source of truth and that 'numbers are never reused or renumbered, hand-maintaining a second copy is forbidden' (§5.7/§4.4) — reviewers explicitly suggest pinning '~10s' and '3 strikes' as registered constants with documented rationale, exactly like frame IDs and CBOR keys already are, instead of leaving them as prose hedges.

## Consistently liked

- **Deterministic, normative shedding table with the segment exception (segments shed whole-source-or-not-at-all, never decimated, because a dropped segment is a lost command, not a recoverable sample)** (13/15) — §10.1, §10.4
  
  > The segment exception—segments shed whole-source-or-not-at-all, never decimated—is the right call because a dropped segment is a permanently lost *command*, not recoverable by interpolation.

- **Honesty Clauses (H1-H12) as an indexed, normative mechanism that names what the protocol does NOT guarantee instead of hiding it** (12/15) — §1.5
  
  > Instead of hiding limitations, 12 explicit clauses enumerate what the protocol does NOT guarantee... This is trust-building and prevents users from discovering failure modes in production.

- **Motion input surface is CLOSED to exactly three modes (native samples, native segments, TCode passthrough) — all kinematic work lives on the hub, never re-implemented per-client** (12/15) — §9.6
  
  > Prevents protocol creep. Every kinematic rule that *every* client would otherwise re-implement lives on the hub. This boundary discipline is rare and correct.

- **Dual-plane readiness gate — one flag, zero buffering, blocks STATE/STREAM/INTENT until the client has the catalog and the safety latch** (11/15) — §6.4
  
  > One `ready` flag prevents a client from executing before adopting the safety latch... exactly right.

- **Ground-truth doctrine — ECHO/STATE report only applied, post-clamp values, never the client's request; no optimistic UI** (10/15) — §1.2-1, §9.1, §9.3
  
  > Clients never display device state that differs from reality; ECHO reports post-clamp values not requests. This prevents a whole class of safety bugs where a UI lies about what the machine actually did.

- **RFC-045 deadman redesign — command-driven sources release ownership but are NOT forced to STOP, since the plan runs out and settles on its own** (9/15) — §11.3, RFC-045
  
  > A stream that stops getting commands already stops by construction... Forcing a STOP on top of that manufactures a spurious safety edge on a machine that was never out of control.

- **Parser totality as a hard conformance obligation (both roles), backed by a fuzzing gate and concrete overflow-check guidance** (9/15) — §5.8, §17.4
  
  > The `declared ≤ remaining` not `start + declared ≤ size` guidance will save implementers from a real class of buffer bugs.

- **Append-only packed-layout evolution — layouts only grow at the tail, unknown/trailing bytes are skipped, so old clients keep working against newer hubs** (8/15) — §5.4, §8.1
  
  > Readers parse what they know and ignore trailing bytes; writers never remove or reorder fields. This is a mature versioning strategy that lets old clients work against new hubs indefinitely.

- **ESTOP frame magic bytes (0xE5 0xE5 0xE5 0xE5 + CRC) — recognizable by a byte-scanner without deframing or session state** (6/15) — §5.5, §13.5
  
  > A relay can recognize ESTOP without parsing the full frame, which is critical for the safety fast-path.


## Interesting

- (5) The §6.9 field-bug story (ownership leaked on 5-of-6 teardown paths because only the deadman pump did cleanup; caught only by testing back-to-back sessions without a reboot) is repeatedly singled out as the exemplary proof of the spec's hard-won maturity, not just as a rule.
- (6) ESP-NOW's 250-byte/242-byte payload as the deliberate 'weakest transport writes the rules' floor that every other binding and catalog-design constraint cascades from is flagged independently as unusually disciplined, constraint-driven design.
- (7) Role-exempt safety ops — even a watch-tier (read-only) session may send STOP/ESTOP, rate-limited but never blocked by authorization tier — is repeatedly flagged as a sharp, deliberate 'physical safety outranks access control' choice.
- (1) One reviewer flagged a genuinely underappreciated insight that no one else surfaced: {target, duration, end_velocity} does not uniquely determine a C1-vs-C2 spline at the knot, which is why curve_family had to become a wire-visible declaration at all — worth someone checking whether this deserves more prominence in the spec's own rationale text.