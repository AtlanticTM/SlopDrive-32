---
title: Errata
description: >-
  Corrections to published SlopSync specification versions, and the process that produces them.
register: IEEE
---

# Errata

!!! note "This page is hand-written"

    It is the one page in the Specification tier that is **not** generated
    from `docs/slopsync/SPEC.md`. An erratum is a record about a published
    document, not a clause of it.

## Status

**No errata have been raised against v1.0.**

An erratum is not the same thing as a known limitation. The limitations of
v1.0 are in the specification itself, at
[§18](limitations.md#s18) — they are stated, deliberate, and correct as
published. An erratum records text that is **wrong**.

## The record

One section per published version, and one entry per correction:

| Field | Content |
|---|---|
| Id | `E-<version>-<nn>` |
| Clause | The clause the correction applies to |
| Reported | Date, and by whom |
| Defect | What the published text says, quoted |
| Correction | What it should say |
| Impact | Whether a conforming implementation must change |

## Rules

- An erratum **never** renumbers anything. A number released in a tagged
  version is permanent.
- An erratum fixes wording, closes an ambiguity, or records a clarification.
  A change to the wire grammar is not an erratum; it is a protocol version
  bump, and it needs exceptional justification.
- Each erratum links to the pull request that applied it.

## See also

- [The RFC process](../community/rfc-process.md).
