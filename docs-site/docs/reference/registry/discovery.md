---
title: Discovery
description: Generated tables of the SlopSync BLE GATT identity, its advertising flags, and the UDP discovery probe/reply (RFC-046).
register: IEEE
generated: true
---

<!-- ==========================================================
     GENERATED FILE. DO NOT EDIT.
     Source of truth: docs/slopsync/registry/registry.yaml
     Generator:       docs-site/tools/gen_docs_tables.py
     Regenerate:      python docs-site/tools/gen_docs_tables.py
     CI gate:         python docs-site/tools/gen_docs_tables.py --check
     Hand edits are overwritten and fail the docs build.
     ========================================================== -->

# Discovery

A client finds a hub two ways before it has a session. One is a pinned
BLE GATT identity. The other is a UDP broadcast probe for WS-side
clients without BLE. Both are read-only identity surfaces. Neither
carries a control plane.

## BLE GATT identity

Every conformant BLE hub advertises the **same** service UUID, so a client
scans for exactly one thing. The first three groups spell the project name
in ASCII, deliberately, so the UUID is greppable rather than an opaque v4.

| Role | UUID |
|---|---|
| Service | `534C4F50-5359-4E43-8000-000000000001` |
| Write characteristic (c2h) | `534C4F50-5359-4E43-8000-000000000002` |
| Notify characteristic (h2c) | `534C4F50-5359-4E43-8000-000000000003` |

## BLE advertising flags

A legacy (≤31 B) advertising payload can spare one byte for flags,
after the service UUID and a shortened hub name. Bits not listed are
zero.

| Mask | Bit | Name | Notes |
|---|---|---|---|
| `0x01` | `bit 0` | `pairing_window_open` | a §12.3 association window is open right now (same meaning as the 0x17 BEACON pairing-open flag, ESP-NOW's equivalent) |
| `0x02` | `bit 1` | `ws_available` | the hub currently has a live IP and a listening WebSocket port — RFC-043's signal that a BLE-connected client SHOULD auto-upgrade to WS. The endpoint itself rides WELCOME `ws_port`/`ipv4` (cbor_keys 46/47), not this byte — a single bit cannot carry a port and an address, and the upgrade hop happens post-HELLO anyway. |

## UDP discovery

This is the canonical WS-side discovery path for a LAN client without
BLE. It uses plain UDP sockets on both ends. It is immune to the
multicast, mesh-AP and Android failure modes that make mDNS unreliable
in real homes.

| Property | Value |
|---|---|
| Port | `21328` |
| Magic | `SLOP` |
| Reply rate limit | 1 / source / second |

The probe and reply frames themselves, `DISCOVER_PROBE` (`0x1E`) and
`DISCOVER_REPLY` (`0x1F`), are frame types. See [Frame types](frames.md).
A reply carries `magic + nonce + hub_name + hub_id + proto_ver + ws_port +
fw_version + catalog_etag + flags`. A passive observer of a normal
WELCOME could already learn all of it.

> DEMO-CANDIDATE: send a live UDP probe to a real hub and decode its reply on the page.
