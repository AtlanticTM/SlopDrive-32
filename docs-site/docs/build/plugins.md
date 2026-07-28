---
title: Plugin guide
description: >-
  How to write an app integration that speaks SlopSync, using a shipped plugin as the worked example.
register: STE
status: stub
---

# Plugin guide

!!! warning "This page is not written yet"

    It is a stub. It names what belongs here and where the source
    material is. It does not fake content.

    Content follows the v1.0 tag on purpose: the normative section must
    describe what shipped, not what was planned.

## What belongs on this page

A worked example, start to finish, from a plugin that actually shipped.

Topics the example already exercises, all worth writing up:

- [Discovery](../reference/registry/discovery.md), and why a manual address
  must always still work.
- Choosing between dense
  [samples](../reference/dictionary.md#sample) and sparse
  [segments](../reference/dictionary.md#segment), and what each costs.
- Holding a session open through a [pause](../reference/dictionary.md#pause):
  the segments stop, the
  [liveness pings](../reference/dictionary.md#liveness-ping) continue, and the
  machine settles instead of tripping its
  [deadman](../reference/dictionary.md#deadman).
- A divergence watchdog that **warns and never silently switches** when the
  host app bends the axis underneath you.
- Byte-for-byte wire parity with [the reference verifier](cli.md#the-probe),
  checked by a test rather than by reading.
- Running the same session twice back to back **without rebooting the device**.
  That pattern is what caught a real ownership-release bug, and it is
  mandatory for [anything touching session lifecycle](local-testing.md#the-pattern-that-is-mandatory).

## Source material

- `clients/mfp-slopsync/` — the shipped plugin. Note which files actually
  ship and which are development-only; a guide that tells people to copy the
  development files is a bug report waiting to happen.
- `docs/slopsync/V1-READINESS.md` §2.5 — the client work items, including the
  honest account of what the host application's own extension surface does and
  does not allow.

## Where to go next

- [Quickstart](quickstart.md) — the seven-step session this guide's example
  builds on.
- [CLI guide](cli.md) — verify wire parity with the probe before you trust it.
- [Local testing](local-testing.md) — the back-to-back-sessions pattern that
  caught the ownership-release bug mentioned above.
