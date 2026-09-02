---
paths:
  - "**"
---

# SlopCanon -- governance law (C-1..C-12)

The rule system every agent, every commit, and every document in this repo
answers to. It exists because the project's knowledge rotted once: status was
appended as prose across many files, facts had many homes, claims were never
stamped with how they were verified, and agents resolved contradictions
silently by picking whichever source they read first. A fleet audit in July
2026 un-rotted it. These rules make that a one-time event.

The model is the SlopSync registry: one source of truth, everything else
derived, a check mode, and a ritual that fixes the doc BEFORE the code.

The operator is not CS-trained. That is a design input, not a caveat: rules
relying on the operator catching subtle drift in review do not work. Drift is
caught mechanically or flagged explicitly, never absorbed silently.

## 1. The Map of Truth -- one fact, one home

Every fact lives in exactly ONE home. Everywhere else refers to it by name or
link and never restates it in full. The same fact stated twice with different
values is not information, it is a flag (§3).

| Domain | Sole home |
|---|---|
| Wire numbers (frames, CBOR keys, NACK codes, channels, limits) | SlopSync repo `spec/registry/registry.yaml` (sibling checkout, pinned by `slopsync.pin`) |
| SlopSync protocol behavior | SlopSync repo `spec/SPEC.md` |
| This machine's device-channel allocation | `docs/slopsync/CHANNEL-MAP.md` (machine-specific, not protocol spec) |
| Governance law | this file |
| Engineering doctrine | the other files in `.claude/rules/` |
| Volatile project/device state (fw versions, deployment, milestones) | the Beads dev board (`bd`, `sd-` prefix) |
| Operator preferences | `CLAUDE.md` (repo root, gitignored) |
| Firmware version constant | `FIRMWARE_VERSION` in `include/system/config_api.h` |
| Subsystem deep detail | that subsystem's own README / spec |
| Public docs site content | SlopSync repo docs-site, generated from its spec homes, never hand-forked |

`CLAUDE.md` is the auto-loaded entry point: operator preferences plus binding
pointers. It holds NO rules and NO status.

## 2. The Laws

**C-1 ONE HOME PER FACT.** See the map. Adding a fact means deciding its home
first. Restating a fact elsewhere requires a pointer, not a copy.

**C-2 STATUS IS NOT PROSE.** Anything that changes over time (versions,
live/deployed/landed claims, milestone state, known bugs, deferred work) lives
on the dev board as discrete stamped issues, never as narrative paragraphs
appended into doctrine or architecture docs.

**C-3 SAME-COMMIT TRUTH.** A commit that changes behavior, removes a feature,
or completes planned work MUST update the affected fact's home in that same
commit. Doc drift is a defect of the same severity as a failing test.

**C-4 STAMP OR IT IS HEARSAY.** Status claims carry a verification stamp:
`[verified YYYY-MM-DD -- method]` (`-- /api/capabilities`, `-- pio test -e
native exit 0`, `-- live probe 8/8`). An unstamped claim, or one whose subject
has been touched by commits since its stamp, is hearsay: re-verify before
building on it, or flag.

**C-5 THE FLAG DUTY.** Contradictions between sources, between a doc and the
code, or between a request and doctrine are NEVER resolved silently. Raise a
Canon Flag (§3) and stop that thread until the operator rules. This is the
core rule; everything else exists to make flags rare.

**C-6 FROZEN MEANS FROZEN.** The frozen list (conformance artifacts, golden
vectors, frozen public APIs -- SlopSync's own list now, hash-pinned from this
side in `tools/canon_lint.py`) is touched only after a flag and an explicit
operator "yes, break compatibility". No exceptions for "it is just a comment".

**C-7 DOCTRINE CHANGES ARE MINI-RFCS.** Changing a rule in this file requires
proposed text plus rationale presented to the operator, explicit approval, and
a dated entry in the Amendments log (§6). Agents never edit this file
unilaterally. Generalized spec-gap ritual: when work needs a rule no doc
defines, write it into the correct home FIRST, then code against it.

**C-8 "WORKING" REQUIRES EVIDENCE.** No claim of live/working/deployed/fixed
-- in a doc, an issue, a commit message, or chat -- without naming the
evidence: the command run and the observed result. "Deployed" specifically
means version-verified on-device, not "upload completed".

**C-9 DELETE LOUDLY, DEPRECATE VISIBLY.** Removing code requires proof of no
remaining references (state the searches run: src/, include/, lib/, test/,
webui/, sim/, examples/, tools/, platformio.ini, docs) recorded in the commit
message. A doc describing a removed system gets a dated deprecation banner on
the same commit that removes the system, never left to read as current.

**C-10 PERIODIC SCRUB.** The truth-scrub audit (multi-agent contradiction
hunt, `.claude/workflows/`) runs at every milestone merge and whenever the
operator smells drift. Confirmed findings feed fixes AND, where they reveal a
missing rule, a C-7 amendment proposal. Note the mechanism itself lives in a
gitignored directory and does not survive a clone.

**C-11 AMERICAN ENGLISH ONLY.** All prose, comments, identifiers, and UI
strings use American spellings (behavior, color, center, license, gray,
initialize). Exceptions: vendored third-party code verbatim, legal license
texts verbatim, and frozen wire artifacts -- a British spelling baked into a
frozen byte sequence or a released wire string is FLAGGED, never silently
respelled, because respelling it is a protocol break. Enforced by
`.claude/hooks/style_check.py` at write time and `tools/canon_lint.py`
tree-wide.

**C-12 COMMENTS ARE CONSTRAINTS, NOT STORIES.** See `.claude/rules/cpp-style.md`
for the mechanized form. A comment states what the code cannot show: an
invariant, a trap, a unit, a contract. Comments NEVER reference removed code
or libraries -- what is gone is gone, and the past lives in git history.

## 3. The Canon Flag protocol

An agent MUST flag, and stop that thread of work, when any of these hits:

1. Two authoritative sources contradict (doc/doc, doc/code, comment/code,
   board/device).
2. Work would touch anything on the frozen list, or modify or delete code on a
   motion or safety path whose liveness the agent cannot prove statically.
3. The operator's request conflicts with a Law, with a NON-NEGOTIABLE in any
   rules file, or with how the rest of the codebase works.
4. A fact the work depends on is unstamped or stale (C-4) and cannot be
   re-verified without hardware the agent should not drive unasked.

Flag format, verbatim structure:

```
CANON FLAG -- <one line: what conflicts>
Source A: <file:line> "<quote>"
Source B: <file:line> "<quote>"   (or: <the request> / <observed device state>)
My read: <which I believe is correct and why, or "cannot determine statically">
Your call: <the single specific question the operator must answer>
```

Rules of engagement: unrelated work may continue, the flagged thread may not.
Never "fix" one side to match the other before the ruling. After the ruling
the resolution is written into the fact's ONE home, stamped, and the losing
statements are corrected or deleted, in the same commit.

**Operator override.** When the operator explicitly asks for something
doctrine forbids, the agent flags it (trigger 3), explains the conflict and
its consequences plainly, and the discussion runs to a conclusion. If the
ruling stands against current doctrine that is not an exception to be quietly
carved out, it is a C-7 amendment: the rule is updated to say what the
operator actually wants, dated in the Amendments log. Doctrine follows the
operator; it is never silently violated AND never silently diverges from
operator intent.

## 4. Volatile truth lives on the board

All volatile truth lives on the Beads dev board as stamped issues, never as
prose in a rules file. Agents read the board at the start of substantive work
(`bd ready`, `bd list --status=open`) and update it in the same commit as the
change (C-3). Claude's private memory files mirror the board, never contradict
it; on conflict the board wins and the memory gets fixed.

## 5. The mechanical floor -- canon_lint

`tools/canon_lint.py` greps the codebase for known doctrine violations (the
classes that have actually bitten this project) and exits nonzero on a hit.
Agents run it before declaring any substantive change done. It is
judgment-free by design: everything it catches is a hard rule, so a hit is a
defect, not a conversation. Current checks: `serial-print`, `slopsync-purity`,
`this-assign`, `links2004-ghost`, the `slopsync.pin` rule, and the frozen
conformance-artifact hash cross-check. The check list grows via C-7 when new
violation classes are confirmed, typically by a C-10 scrub.

## 6. Amendments

| Date | Change | Approved by |
|---|---|---|
| 2026-07-27 | SlopCanon established (C-1..C-10, flag protocol, ledger, lint). | operator |
| 2026-07-27 | C-11: American English only, British spellings purged (frozen wire bytes flag-only). | operator |
| 2026-07-27 | C-12: comments are constraints plus pointers; narrative lives in docs. | operator |
| 2026-07-27 | Operator-override path added to §3: overrides become amendments, never silent exceptions. | operator |
| 2026-07-27 | C-11 ruling: pre-release wire strings ARE respelled (one atomic catalog-evolution pass, fixtures and goldens regenerated). The frozen-wire-artifact carve-out applies only to released/frozen bytes. | operator |
| 2026-07-27 | Dead-code ruling: all proven-dead code is deleted (tests before and after). CLAUDE.md restructured: preferences only. | operator |
| 2026-07-28 | Map of Truth repointed for the SlopSync repo split: wire numbers and protocol behavior rows name the sibling checkout (pinned by `slopsync.pin`); CHANNEL-MAP.md row added. (Recorded retroactively 2026-07-29 by the C-10 scrub.) | operator |
| 2026-08-03 | LEDGER.md retired: volatile truth moved to the Beads dev board (C-2/§4 repointed). | operator |
| 2026-08-03 | The three flat canon files were retired; their content migrated into `.claude/rules/` and the skills. This file is the governance home; the Map of Truth row for engineering doctrine now names the rules directory. | operator |
| 2026-09-02 | Three-board split ratified in `architecture.md` §2: the RP2350 owns motion (slopmotion plus pulse generation, position truth), the S3 is hub and policy owner, the C5 is the network peripheral. Commands cross the link as anchored intents, event-driven, never as rendered chunks. The sole-caller rule now names the motion processor; the dual-core row carries a PLANNED CHANGE. Rationale: every sd-ar3/sd-dxy bug family lived in the S3-side re-render seam. | operator |
| 2026-08-06 | T31 (a gate must never disable the transport carrying what it gates) and T32 (fix a two-ended link at both ends; pace retransmits on progress, and a retransmit timer becomes the transfer rate) added to `transport.md`. T10 gained the Git-Bash-127 and filter-name traps. Serial OTA documented as a second deploy path in `build-test-deploy.md`. `memory-budget.md` gained a current-free-heap section because every `int_free` figure in it predates the 2026-08-01 NimBLE removal and misled a whole session. All from the serial-OTA bring-up. | operator |
| 2026-08-06 | sd-emy item (b) amended: OTA STOPS motion via the one shared gate rather than refusing when moving. A second gate duplicated the one OtaService is built around, and refusing locks the update path behind the state a bad image may be causing. | operator |
| 2026-08-04 | canon_lint scope narrowed so the zero-findings bar (§5) is reachable: `src/quad_probe/` exempt from `serial-print` (bench sketch, excluded from every firmware env by `build_src_filter`), and `.claude/hooks/style_check.py` exempt from the spelling scan (it holds the en-GB wordlist, same standing as `tools/canon_lint.py`), and `.beads/` exempt from it too (generated append-only log that quotes issue text). No check was removed. The four genuine C-11 hits the noise was hiding were FIXED, not exempted. Baseline 213 -> 1, and that 1 is a live pin-mismatch. | operator |
