---
title: Channel catalog reference
description: >-
  What a real SlopSync device catalog looks like, entry by entry, with every field explained.
register: STE
status: stub
---

# Channel catalog reference

!!! warning "This page is not written yet"

    It is a stub. It names what belongs here and where the source
    material is. It does not fake content.

    Content follows the v1.0 tag on purpose: the normative section must
    describe what shipped, not what was planned.

## What belongs on this page

The [spec-core channels](registry/channels.md#spec-core-channels) are already
generated and authoritative. This page is the other half: a **real device
catalog**, entry by entry, as a worked example of catalog authoring.

For each entry: its class, its access level, its layout or schema field by
field, the units and scales, and why it is one channel rather than two.

It should also cover the authoring rules that are easy to get wrong:

- A STATE payload must fit 242 bytes unfragmented. A group that does not fit
  is split at authoring time, not at runtime.
- Layouts evolve append-only. A changed or removed field means a new channel
  id.
- A category spans channels, which is how a settings tab outgrows one
  snapshot.
- [Field roles](registry/catalog-vocabulary.md#field-roles) are opportunities,
  never requirements. Generic rendering must always work.

## Source material

- [Appendix D](../spec/appendices.md#appendix-d) of `docs/slopsync/SPEC.md`: the initial device catalog sketch.
- `include/comms/SlopSyncCatalog.h`: the catalog a real device actually
  publishes. Prefer this over the appendix where they differ. It is what
  ships.
- [Catalog schema (CDDL)](../spec/schema.md): the normative catalog encoding,
  published from `docs/slopsync/schema/catalog.cddl`.
- `lib/slopsync/include/slopsync/conformance/catalog_check.hpp`: the rules a
  catalog is mechanically checked against. Every check there is a rule this
  page should explain.

> DEMO-CANDIDATE: fetch a live device's catalog over the wire and render it
> entry by entry next to this page's worked example, field for field.
