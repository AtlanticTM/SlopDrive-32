---
title: C++ client guide
description: >-
  How to write a SlopSync client in C++.
register: STE
status: stub
---

# C++ client guide

!!! warning "This page is not written yet"

    It is a stub. It names what belongs here and where the source
    material is. It does not fake content.

    Content follows the v1.0 tag on purpose: the normative section must
    describe what shipped, not what was planned.

## What belongs on this page

The embedded and native client. Cover the header-only library, the injected clock and randomness, and the transport interface a caller implements.

Source material: `lib/slopsync/README.md` (the vendorable front door), `lib/slopsync/include/slopsync/client.hpp`, and `examples/slopsync_demo/demo.cpp`.

> DEMO-CANDIDATE: `examples/slopsync_demo/demo.cpp` walked step by step as a
> live connect-and-print session.

## Every client guide covers the same seven things

Keep the order identical across languages, so a reader who knows one guide can
skim another.

1. Open the transport and complete the handshake.
2. Handle the catalog: fetch, cache by
   [etag](../../reference/dictionary.md#etag), or pin it.
3. Pass the [ready gate](../../reference/dictionary.md#ready-gate).
4. Adopt retained STATE before rendering anything.
5. Send an [intent](../../reference/dictionary.md#intent) and render the
   [echo](../../reference/dictionary.md#echo), never the request.
6. Keep the session alive, and honor the
   [deadman](../../reference/dictionary.md#deadman) if you own a source.
7. Handle NACKs, including the range fallback for a code you do not know.

## Where to go next

- [Quickstart](../quickstart.md) — the same seven steps, worked end to end.
- [Hub implementer guide](../hub.md) — the other side of this library: making
  firmware a conforming hub instead of a client.
- [CLI guide](../cli.md) — watch what your client did to the motion.
