---
title: Hub implementer guide
description: >-
  How to make your firmware a conforming SlopSync hub.
register: STE
status: stub
---

# Hub implementer guide

!!! warning "This page is not written yet"

    It is a stub. It names what belongs here and where the source
    material is. It does not fake content.

    Content follows the v1.0 tag on purpose: the normative section must
    describe what shipped, not what was planned.

## What belongs on this page

The guide a firmware author reads before deciding whether to adopt SlopSync.
It must be honest about cost, or it is useless.

- **Author your catalog.** This is most of the work and most of the value.
  Cover the 242-byte snapshot budget, the per-entry size cap, and splitting a
  state group across channels when it does not fit.
- **Wire the delegate.** Apply [intents](../reference/dictionary.md#intent)
  through your existing arbitration and safety gates. **Never bypass them.**
  The protocol is a client of your motion authority, not a second one.
- **[Echo](../reference/dictionary.md#echo) applied values.**
  Post-[clamp](../reference/dictionary.md#clamp), from the driver. Not the
  request.
- **Retain and seed.** Keep the latest snapshot of every STATE channel, and
  seed the safety snapshot at construction. A fresh boot that retained nothing
  hands a connecting client an empty [latch](../reference/dictionary.md#latch).
- **One [teardown](../reference/dictionary.md#teardown) path.** Every way a
  session can end runs the same loss policy. This is where the hard bugs live.
- **Implement a transport** by writing an adapter, outside the library.

## Traps this project paid for

These belong on the page as a named section, because every one of them cost
real debugging time:

- A `reset()` written as whole-object assignment built a nine-kilobyte stack
  temporary and blew the task stack on every client connect. Host tests never
  see it: host stacks are megabytes.
- A large service placed in static memory starved the internal heap and killed
  the network stack, while looking fine in the linker report.
- Ownership release that hangs off a liveness pump alone leaks ownership on
  every other exit path, and stays invisible while your deploys reboot the
  device between tests.

## Source material

- `docs/slopsync/SPEC.md` §13.1 — the binding contract every transport must
  satisfy.
- `lib/slopsync/README.md` — the vendorable front door and the layering rules.
- `src/comms/SlopSyncHubService.*` and `SlopSyncWsTransport.*` — a working
  composition root and a real transport adapter.

## Where to go next

- [CLI guide](cli.md) — verify a hub with the probe and SlopScope, before you
  trust it.
- [Local testing](local-testing.md) — the simulator, the golden vectors, and
  the back-to-back-sessions pattern that finds teardown bugs like the ones
  above.
