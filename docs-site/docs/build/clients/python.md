---
title: Python client guide
description: >-
  How to write a SlopSync client in Python.
register: STE
status: stub
---

# Python client guide

!!! warning "This page is not written yet"

    It is a stub. It names what belongs here and where the source
    material is. It does not fake content.

    Content follows the v1.0 tag on purpose: the normative section must
    describe what shipped, not what was planned.

## What belongs on this page

The tooling and test client. Cover a minimal session, and reading generated constants rather than typing numbers.

Source material: `tools/slopsync_probe.py`, the reference verifier.

> DEMO-CANDIDATE: the probe's own minimal session (see the
> [Quickstart](../quickstart.md#python)) run live, one decoded frame printed
> per line.

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
- [CLI guide](../cli.md) — the probe and SlopScope, in depth.
- [Local testing](../local-testing.md) — run this against the simulator
  before you run it against hardware.
