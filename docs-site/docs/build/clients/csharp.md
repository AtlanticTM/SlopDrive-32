---
title: C# client guide
description: >-
  How to write a SlopSync client in C#.
register: STE
status: stub
---

# C# client guide

!!! warning "This page is not written yet"

    It is a stub. It names what belongs here and where the source
    material is. It does not fake content.

    Content follows the v1.0 tag on purpose: the normative section must
    describe what shipped, not what was planned.

## What belongs on this page

The desktop-app client. Cover `ClientWebSocket`, the byte-for-byte builders, and running a session against a real device.

Source material: `clients/mfp-slopsync/SlopSync.cs`, which is a shipped external client; and its `LiveWireTest` harness, which compiles the plugin file itself into a console program and runs it against hardware.

> DEMO-CANDIDATE: a minimal HELLO/WELCOME round trip built from
> `LiveWireTest`'s own session code, run live against a device.

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
- [Plugin guide](../plugins.md) — the shipped-plugin worked example this
  guide's source material comes from.
- [CLI guide](../cli.md) — watch what your client did to the motion.
