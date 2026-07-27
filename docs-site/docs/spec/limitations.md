---
title: Known limitations
description: >-
  SlopSync clause 18: every known limitation of v1.0, stated so that nobody
  rediscovers one as a surprise.
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

# 18. Known Limitations at v1.0 *(normative in the sense that they MUST NOT be denied)* {#s18}

These are real, found during implementation, and stated so nobody rediscovers them as surprises. Each is either accepted for v1.0 or has a named future path.

1. **Handoff-guard coverage is lookahead-bounded** (H11, [§9.6](channels.md#s9-6)). The hub's end-velocity bound needs the *next* segment already scheduled. A client's scheduling lookahead therefore sets the coverage: the bound can act only while the current segment is shorter than that lookahead. It correlates usefully with the pathology it targets — an oversized spline tangent implies a steep chord, and a steep chord over bounded displacement implies a short segment — but that is a correlation, not a guarantee. Long segments are not bounded. Raising a client's lookahead is the direct widener; the hub's own legality checks are the backstop.
2. **The catalog has no vocabulary for labelling EVENT kinds.** A channel's `schema` names its `body` fields, but `event_kind` values themselves are unlabelled: a generic client can render an anomaly event's *fields* and cannot render its *name*. Spec-core channels' kinds are registered, so only device-authored channels are affected. The fix is an additive entry-level annotation and is deliberately deferred rather than guessed at.
3. **No safety-EVENT kind exists for the override/bypass modes** ([§11.1](safety.md#s11-1)). This is by decision, not omission: they are latched modes, not stop edges, and an edge kind would imply an operator action that hub-side reconciliation did not have. If a mode edge is genuinely wanted it is an additive kind.
4. **Reboot-commit is specified but unproven.** `reboot_in_ms` and the `REBOOTING` GOODBYE code are allocated and normatively described ([§9.3](channels.md#s9-3)), but no reference implementation emits them yet. Treat as a specified extension point, not as field-tested behavior.
5. **The reference shedding implementation exercises only the STATE rows.** The [§10.4](qos.md#s10-4) table is normative in full, but the reference hub currently applies it to STATE pushes only; the STREAM decimation rows and the segment-class rows are specified and unit-tested rather than field-exercised. Implementers writing a STREAM-shedding hub are the first users of those rows.
6. **Event replay is catalog-gated but single-ring in the reference hub.** A device declaring `replay_depth` on an EVENT channel other than the log channel gets no replay from the reference implementation unless it wires its own ring. The rule ([§9.4](channels.md#s9-4)) is general; the reference coverage is not.
7. **The frozen conformance mini-catalog carries no [§8.8](catalog.md#s8-8) annotations and no safety INTENT channel.** Consequently the golden vectors do **not** cover per-op `access`, `option_access`, the role exemption, or any settings-metamodel annotation. Those are covered by behavioral tests and device catalogs only. Extending fixture coverage is a v1.1 candidate and would, by construction, move the frozen pins — which is why it was not done at the tag.
8. **Blob namespace validity is not checked as a value.** An unregistered `ns` is not rejected by the grammar; it falls through to the hub's store backend and is answered `CHUNK_UNAVAILABLE`. The observable behavior is correct and the failure is safe, but a receiver MUST NOT assume `ns` has been range-validated for it.
9. **"A full BLOB_REQ carrying `chunks` is malformed" is unrepresentable, not enforced.** "Full" is *derived* from the absence of `chunks`, so the illegal combination cannot be encoded and no decoder rejects it. What decoders do reject is an **empty** `chunks` array, and a catalog-namespace request carrying `store_id` or `slot`. Encoders SHOULD still refuse the combination at their API boundary.
10. **Packed layouts have no 64-bit integer type.** An 8-byte identifier in a packed slot is two `u32` fields ([§12.7](security.md#s12-7)). Documentation that says "u64" in a packed context means exactly that.
11. **Crypto is a seam, not a battery.** A conforming library MAY ship with stub sign/verify. Hub signing ([§12.5](security.md#s12-5)) and therefore evil-twin detection exist only where the application injects a real implementation; a client MUST treat an absent signature per H9 and MUST NOT assume the capability is present because the protocol defines it.
12. **The trust ledger has no wall clock** (H7, [§7.2](time.md#s7-2)). `first_seen`/`last_seen` are frequently zero, and a device that never reports a version can never trip the tripwire.
13. **Cleartext transport bounds everything** (H4). Every trust mechanism here is designed to be useful without confidentiality — which is why the hub signature works without secrecy and why proof presentation exists — but the ceiling is "honest LAN" until a secure transport lands in v2.
14. **One relay hop only** ([§14.3](transports.md#s14-3)).
15. **Preset/store device backends are optional and largely unimplemented.** The blob verb, the STORE class and the trust-ledger store are specified and implemented; general device preset stores are a specified mechanism with no reference device backend yet.
16. **WELCOME `identity` (37) has no reference codec yet.** The key and its sub-key space are registered and [§6.3](session.md#s6-3) specifies them, but the reference WELCOME encoder does not emit them and no reference hub populates them. A client MUST therefore be able to run with no hub identity at all — which [§4.3](foundations.md#s4-3) already required, but which is worth stating plainly because [§4.2-4](foundations.md#s4-2) makes this the *only* home for `fw_version`. Until it ships, "what firmware is this machine running" has no in-band answer.
17. **The `session-roster` channel is allocated and described but not built.** No reference catalog builder declares it, so the roster snapshot of [§12.7](security.md#s12-7) — and with it the "a late joiner learns existing sessions' names" property that offsets [§9.4](channels.md#s9-4)'s no-replay rule — is specification, not shipped behavior. The session-events channel and the administration ops around it are implemented; the roster STATE they complement is not.
18. **Action-intent resets have vocabulary but no reference verb.** `action.<name>` and `meta.reset_gen` are registered and specified ([§9.3](channels.md#s9-3)); no reference hub exposes a reset as an INTENT yet, so the observable-reset rule is untested in the field.
19. **Registry `ref:` fields lag this document's numbering.** Inserting [§6.4](session.md#s6-4) (readiness) and splitting [§12.2](security.md#s12-2) into [§12.3](security.md#s12-3)–[§12.5](security.md#s12-5) shifted several pointers: entries citing [§6.4](session.md#s6-4)/[§6.5](session.md#s6-5)/[§6.6](session.md#s6-6)/[§6.8](session.md#s6-8) mean [§6.5](session.md#s6-5)/[§6.6](session.md#s6-6)/[§6.7](session.md#s6-7)/[§6.9](session.md#s6-9), and entries citing [§12.2](security.md#s12-2) for pairing, token presentation or signing mean [§12.3](security.md#s12-3), [§12.4](security.md#s12-4) and [§12.5](security.md#s12-5) respectively. Values are unaffected ([§5.7](wire-format.md#s5-7)). Correcting the `ref:` strings requires regenerating the constants header, so it is a follow-up commit rather than part of this document.
