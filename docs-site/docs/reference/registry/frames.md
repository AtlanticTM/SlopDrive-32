---
title: Frame types
description: Generated table of every SlopSync frame type byte and header flag.
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

# Frame types

The `type` byte is the first discriminator in the 8-byte header.

`dir` reads `c2h` for client to hub, `h2c` for hub to client, `any` for
either direction. `plane` reads `control` for a CBOR payload, `data` for
a packed payload, `raw` for a fixed layout defined in its own clause.

An endpoint that receives an unknown frame type ignores the frame. The
header always carries the length, so skipping is safe.

| Code | Name | Direction | Plane | Clause |
|---|---|---|---|---|
| `0x00` | `HELLO` | `c2h` | `control` | §6.2 |
| `0x01` | `WELCOME` | `h2c` | `control` | §6.3 |
| `0x03` | `PING` | `any` | `raw` | §6.5 |
| `0x04` | `PONG` | `any` | `raw` | §6.5 |
| `0x05` | `CLOCK` | `any` | `raw` | §7.1 |
| `0x06` | `SUBSCRIBE` | `c2h` | `control` | §6.6 |
| `0x07` | `UNSUBSCRIBE` | `c2h` | `control` | §6.6 |
| `0x08` | `GRANT` | `h2c` | `control` | §10.2 |
| `0x0B` | `STATE` | `h2c` | `data` | §9.1 |
| `0x0C` | `STREAM` | `any` | `data` | §9.2 |
| `0x0D` | `INTENT` | `c2h` | `control` | §9.3 |
| `0x0E` | `ECHO` | `h2c` | `control` | §9.3 |
| `0x0F` | `EVENT` | `h2c` | `control` | §9.4 |
| `0x10` | `NACK` | `h2c` | `control` | §16.1 |
| `0x11` | `GOODBYE` | `any` | `control` | §6.8 |
| `0x12` | `PROBE` | `any` | `raw` | §6.4 |
| `0x13` | `PROBE_REPORT` | `c2h` | `control` | §6.4 |
| `0x14` | `PAIR_REQ` | `c2h` | `control` | §12.2 |
| `0x15` | `PAIR_GRANT` | `h2c` | `control` | §12.2 |
| `0x16` | `ACKMASK` | `any` | `raw` | §13.3 |
| `0x17` | `BEACON` | `h2c` | `raw` | §13.7 |
| `0x18` | `PUBLISH` | `c2h` | `control` | §6.6 |
| `0x19` | `CATALOG_READY` | `c2h` | `raw` | §8.4 |
| `0x1A` | `BLOB_REQ` | `c2h` | `control` | §8.4 |
| `0x1B` | `BLOB_CHUNK` | `h2c` | `raw` | §8.4 |
| `0x1C` | `AUTH` | `c2h` | `control` | §12.2 |
| `0x1D` | `HUB_SIG` | `h2c` | `control` | §12.2 |
| `0xE5` | `ESTOP` | `any` | `raw` | §5.5, §11.2 |

## Burned and reserved ranges

Frame types `0x09` and `0x0A` are **burned**. They carried the retired
`CATALOG_REQ` and `CATALOG_CHUNK` verbs. They are never reallocated, so a
stale peer meets an unknown type and fails loudly instead of misreading a
blob transfer.

Types `0x80`–`0xDF` are experimental. They never appear in a tagged
release. Types `0xE0`–`0xFF` are reserved, except `0xE5` (ESTOP).

## Header flags

Bits not listed are zero on send and ignored on receive.

| Mask | Bit | Name | Clause |
|---|---|---|---|
| `0x01` | `bit 0` | `FRAG_START` | §5.6 |
| `0x02` | `bit 1` | `FRAG_MORE` | §5.6 |

`FRAG_START` plus `FRAG_MORE` marks the first fragment. `FRAG_MORE` alone
marks a middle fragment. `FRAG_START` alone marks an unfragmented frame.
Neither flag, after prior fragments, marks the last fragment.
