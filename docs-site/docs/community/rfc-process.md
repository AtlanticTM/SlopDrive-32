---
title: RFC process
description: >-
  How a change to the SlopSync protocol is proposed, argued, and either bound into the specification or refused.
register: STE
status: stub
---

# RFC process

!!! warning "This page is not written yet"

    It is a stub. It names what belongs here and where the source
    material is. It does not fake content.

    Content follows the v1.0 tag on purpose: the normative section must
    describe what shipped, not what was planned.

## What belongs on this page

The process is already being run; this page has to write it down.

- **What needs an RFC.** Any new wire number. Any change to normative
  behavior. Any change to the [governance stance](governance.md).
- **What does not.** Authoring a new device channel needs no specification
  change at all. That is the point of a
  [self-describing catalog](../reference/dictionary.md#catalog), and this page
  should say so loudly, because it is the single biggest misconception a new
  implementer arrives with.
- **The lifecycle.** Proposed, argued, amended, bound into a base pass, or
  refused with recorded reasoning.
- **The spec-gap ritual.** When an implementation needs a number
  [the specification](../spec/index.md) lacks: fix the registry and the
  specification **first**, regenerate, then write code against the generated
  constant. Never invent a code-local magic number for anything wire-visible.
  This happened eleven times while the protocol was being built, and it will
  happen to the next implementer too.
- **The allocation policy.** Additions by pull request. Released numbers are
  never reused and never renumbered.
- **The alarm.** After the base pass, RFCs should be rare, small and
  flip-a-flag. If a proposal requires rethinking core structure, the base pass
  failed. Treat that as the alarm it is, rather than quietly absorbing it.

## Source material

- `docs/slopsync/RFC-QUEUE.md` — twenty-nine worked RFCs and a feasibility
  pass. This is the process, already executed; the page is mostly a
  description of what is visibly there.
- `docs/slopsync/V1-READINESS.md` — the readiness ledger and its dispositions,
  which is what "argued to a decision" looks like in practice.
- `docs/slopsync/SPEC.md` §4.4 and §5.7 — evolution policy, reserved ranges,
  and registry governance.

## See also

- [Errata](../spec/errata.md) — for corrections that are not protocol changes.
- [Governance](governance.md).
- [Contributing](contributing.md) — building this site, if the RFC needs new
  pages.
