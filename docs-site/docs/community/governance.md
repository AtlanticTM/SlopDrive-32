---
title: Governance
description: >-
  SlopSync favours no firmware, no vendor and no product. It is provided to
  the community as a tool.
register: STE
---

# Governance

## The stance

**SlopSync favours no firmware, no vendor and no product. It is provided to
the community as a tool.**

That sentence is the whole stance. Everything below is what it means in
practice.

## What that means in practice

**No firmware is the reference firmware.** SlopDrive-32 happens to be where
SlopSync was written and where it is proven on hardware. That makes it the
first implementation. It does not make it the privileged one. A conforming hub
is a conforming hub.

**No vendor gets a reserved number.** The registry allocates by pull request,
in the open, on technical merit. There is no vendor block, no paid range and
no private extension space that anyone else cannot read. Device-defined ranges
exist and are open to every device equally.

**No product is designed for.** A feature lands because the protocol needs it,
not because one machine wants it. When a proposal is really "make my product
easier", it is refused, and the refusal is recorded with its reasoning.

**[The specification](../spec/index.md) is the product.** The
C++ library is its reference implementation, not its definition. If the
library and the specification disagree, the specification wins and the
library has a bug.

**The registry is the single source of truth.** Where any document, any
library and any table disagree about a number, the registry wins. That rule is
mechanically enforced: the C++ constants and every table on this site are
generated from it, and a stale copy fails CI.

**Released numbers are never reused and never renumbered.** Not for tidiness,
not for a better name, not for a nicer grouping. A number burned by a retired
feature stays burned, so a stale peer meets an unknown value and fails loudly
instead of misreading a live one.

**Honest defects are published.** The fuzz campaign found three memory-safety
bugs before release, and they are documented rather than quietly patched.
Publishing found-and-fixed bugs is a credibility asset. A protocol that claims
a clean history is either very young or not telling you something.

## What SlopSync will not do

- It will not require a specific motor driver, motion planner, transport or
  cloud service.
- It will not gate any part of the wire protocol behind registration,
  certification or a fee.
- It will not add a capability that only one machine can implement, unless the
  capability is optional and its absence is discoverable.
- It will not describe how a client should *look*. It describes what things
  **are**. There is no widget field in the catalog, deliberately.

## Changing this page

This page states a stance, so it changes only by explicit decision, never as a
side effect of an unrelated edit. A change here is an RFC like any other. See
[the RFC process](rfc-process.md).

## See also

- [Ecosystem and compatibility](../understand/ecosystem.md) — where this
  stance is either honored or broken in practice.
- [Contributing](contributing.md).
