---
title: Registry reference
description: Generated index of the SlopSync protocol registry: frame types, CBOR keys, channels, error codes, limits.
register: IEEE
generated: true
---

<!-- ==========================================================
     GENERATED FILE — DO NOT EDIT.
     Source of truth: docs/slopsync/registry/registry.yaml
     Generator:       docs-site/tools/gen_docs_tables.py
     Regenerate:      python docs-site/tools/gen_docs_tables.py
     CI gate:         python docs-site/tools/gen_docs_tables.py --check
     Hand edits are overwritten and fail the docs build.
     ========================================================== -->

# Registry reference

The registry is the single source of truth for every number SlopSync
puts on the wire. These pages are generated from it. No number on this
site is typed by a human.

If a page here disagrees with prose elsewhere, this page wins.

## Protocol identity

| Property | Value |
|---|---|
| Protocol name | `slopsync` |
| Protocol version | `1` |
| Byte order | `little-endian` |
| Header size | `8 bytes` |

## Pages

| Page | Covers |
|---|---|
| [Frame types](frames.md) | The `type` byte and the header flag bits. |
| [Channels](channels.md) | Channel classes, stream kinds, access levels, priorities, id ranges, spec-core channels. |
| [CBOR keys](cbor-keys.md) | The global control-plane key space and every scoped sub-map key space. |
| [Catalog vocabulary](catalog-vocabulary.md) | Packed field types, field roles, setting categories and flags, procedure phases. |
| [Event kinds](events.md) | Event kind values for the spec-core EVENT channels, and log severity levels. |
| [Safety codes](safety.md) | Safety intent operations and safety cause codes. |
| [Pairing modes](pairing.md) | The pairing mode bitmask advertised in WELCOME. |
| [NACK codes](errors.md) | Every NACK and GOODBYE reason code, by range. |
| [Limits and defaults](limits.md) | Well-known sizes, timeouts, caps and defaults. |
| [Discovery](discovery.md) | BLE GATT identity and advertising flags, and the UDP discovery probe/reply. |
| [Rendering vocabulary](rendering.md) | Categories, ranks, value axes, units, action tags, archetypes, regions, renderer classes and widget patterns — the numbers behind RENDERING.md (RFC-048). |

## How to change a number

1. Edit `registry.yaml`. It is the only place a number is decided.
2. Run the C++ generator. It rewrites `registry_constants.hpp`.
3. Run the docs generator. It rewrites these pages.
4. Commit the registry and both generated outputs together.

Released numbers are never reused and never renumbered.
