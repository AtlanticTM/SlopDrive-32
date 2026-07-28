---
title: Reference
description: >-
  Lookup material: the SlopSync Dictionary, the generated registry tables, and the channel catalog reference.
register: STE
---

# Reference

Material you look things up in.

| Page | Contains |
|---|---|
| [The Dictionary](dictionary.md) | Every term, exactly one definition each |
| [Registry reference](registry/index.md) | Every wire number, generated from `registry.yaml` |
| [Channel catalog](channel-catalog.md) | What a real device's catalog looks like, entry by entry |

## Two things worth knowing

**The registry pages are generated.** No number on this site is typed by a
human. If a generated table and any prose disagree, the table wins, and the
prose is a bug. A stale table fails the build.

**The Dictionary auto-links itself.** A term defined there shows its
definition on hover wherever it appears on this site. The tooltips and the
Dictionary page come from one source, so a term cannot acquire a second
meaning.

## Quick lookups

- [Frame types](registry/frames.md): what is this `type` byte?
- [NACK codes](registry/errors.md): why was my frame refused?
- [Limits and defaults](registry/limits.md): how big, how fast, how long?
- [CBOR keys](registry/cbor-keys.md): what is key 24?
