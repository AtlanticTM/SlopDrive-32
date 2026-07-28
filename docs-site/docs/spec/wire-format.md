---
title: Wire format
description: >-
  SlopSync clause 5: the 8-byte frame header, the deterministic CBOR profile,
  packed data-plane layouts, the ESTOP frame, fragmentation, and parser
  totality.
register: IEEE
generated: true
---

<!-- ==========================================================
     GENERATED FILE — DO NOT EDIT.
     Source of truth: docs/slopsync/SPEC.md
     Generator:       docs-site/tools/gen_spec_pages.py
     Regenerate:      python docs-site/tools/gen_spec_pages.py
     CI gate:         python docs-site/tools/gen_spec_pages.py --check
     Normative text is copied verbatim. Hand edits are overwritten
     and fail the docs build. Edit the specification instead.
     ========================================================== -->

# 5. Wire Format *(normative)* {#s5}

## 5.1 Frame header {#s5-1}

Every SlopSync frame begins with the same 8 bytes:

```
offset  size  field     notes
0       1     type      frame type (registry `frame_types`)
1       1     flags     bit0 FRAG_START, bit1 FRAG_MORE; others zero on send, ignored on receive
2       2     channel   u16 channel id; 0x0000 for session-scoped frames
4       2     seq       u16 sequence number (§7.3); 0 where the class is unsequenced
6       2     len       u16 payload length in bytes (excluding this header)
```

One SlopSync frame maps to exactly one transport datagram/message where the binding allows ([§13](transports.md#s13)); `len` makes frames self-delimiting on byte-pipe bindings.

**`max_frame` is header-inclusive**: it bounds `8 + len`. Per-binding defaults are in the registry (`max_frame_ws`, `max_frame_espnow`, `max_frame_ble`, `max_frame_serial`; [Appendix G](appendices.md#appendix-g)). A hub advertises its own value in WELCOME `limits.max_frame` and MAY advertise less than its binding permits; it MUST NOT advertise more. A frame exceeding the negotiated maximum is answered with NACK `FRAME_TOO_LARGE` (if a session exists) and discarded.

**What `header.channel` carries, per frame type** *(normative routing — previously discoverable only by reading the reference implementation)*:

| Frame types | `header.channel` |
|---|---|
| HELLO, WELCOME, SUBSCRIBE, UNSUBSCRIBE, PUBLISH, GRANT, NACK, GOODBYE, PING, PONG, CLOCK, PROBE, PROBE_REPORT, PAIR_REQ, PAIR_GRANT, AUTH, HUB_SIG, CATALOG_READY, BLOB_REQ, ACKMASK, BEACON | `0x0000` (session-scoped). Any addressing rides the payload — NACK's `channel_id` (15), BLOB_REQ's `blob` (38) sub-map |
| STATE, STREAM, INTENT, ECHO, EVENT | the **target channel id** |
| BLOB_CHUNK | the id of the channel whose blob it carries (the `catalog` channel for namespace 0) |
| ESTOP | not applicable — the [§5.5](#s5-5) fixed 12-byte layout has no conventional header |

INTENT and ECHO *also* carry `channel_id` (15) inside the CBOR payload; the header copy is redundant-but-authoritative routing — the two name the same channel, and the header is what routes.

## 5.2 Frame type registry {#s5-2}

The full table lives in `registry.yaml` (`frame_types`) and is reproduced in [Appendix A](appendices.md#appendix-a). Core points:

- **Control-plane** frames carry CBOR payloads ([§5.3](#s5-3)): HELLO, WELCOME, SUBSCRIBE, UNSUBSCRIBE, PUBLISH, GRANT, INTENT, ECHO, EVENT, NACK, GOODBYE, PROBE_REPORT, PAIR_REQ, PAIR_GRANT, BLOB_REQ, AUTH, HUB_SIG.
- **Data-plane** frames carry packed payloads ([§5.4](#s5-4)): STATE, STREAM.
- **Raw** frames have fixed layouts defined in their own sections: PING, PONG, CLOCK, PROBE, ACKMASK, BEACON, CATALOG_READY, BLOB_CHUNK, ESTOP.

## 5.3 Control-plane encoding: the CBOR profile {#s5-3}

Control payloads are CBOR maps with **integer keys** from the global key registry (`cbor_keys`, [Appendix B](appendices.md#appendix-b)), restricted to a deterministic subset of RFC 8949:

- definite-length everything (no indefinite strings/arrays/maps);
- integers in shortest form; map keys sorted ascending by encoded bytes;
- floats as binary32 only (never binary16/64); values that are semantically integers encoded as integers, not floats;
- no tags, no bignums, no simple values other than `false`/`true`/`null`;
- **maximum nesting depth 4 per decoded document.** One structure legally exceeds this as a whole: the catalog, whose entries nest to 5 counting the outer array. [§8.4](catalog.md#s8-4) therefore defines the catalog as an outer array header followed by independently-decodable *entry documents*, each within the depth-4 cap.
- an **absent** optional key is simply not emitted. There is no "null means absent" encoding; a key that is present with value `null` is a distinct (and, unless a section says otherwise, invalid) thing.

**Scoped sub-map key conservation (normative).** Per-feature keys do NOT each take a global key. A feature takes **one** global key holding a sub-map whose interior keys come from that feature's own small space, registered separately. The registered scoped spaces at v1.0 are `limits` (22) → `welcome_limits_keys`, `probe_result` (26) → `probe_result_keys`, `identity` (37) → `identity_keys`, `blob` (38) → `blob_keys`, `trust` (39) → `trust_keys`, and `body` (40) → **the channel's own catalog `schema`**, exactly as INTENT's `value` (20) does. A sub-map's key space is local: key 1 in `blob` and key 1 in `trust` are unrelated, and neither is `proto_ver`. A new feature that wants five keys gets one global key plus a sub-key section.

**Signedness (normative).** For a value described by a catalog field, **the catalog's declared type is authoritative over the CBOR major type**. A field declared `int` whose value happens to be non-negative round-trips legally as CBOR unsigned; a decoder MUST accept either major type and interpret per the catalog. Encoders MUST NOT be required to force a negative-looking encoding to signal signedness.

*Rationale (informative):* exactly one valid encoding exists for any message, which makes golden vectors byte-exact and lets constrained clients ship **pre-encoded templates** — a canned HELLO with value bytes patched in at runtime is guaranteed to be the same bytes a full encoder would produce. A decoder MAY reject profile violations with NACK `PROFILE_VIOLATION`; it MUST NOT crash on them ([§5.8](#s5-8)).

## 5.4 Data-plane encoding: packed layouts {#s5-4}

STATE and STREAM payloads are **packed little-endian structs**. There is no encoder: the layout *is* the catalog entry's `layout` array ([§8.1](catalog.md#s8-1)) — an ordered list of fields using `packed_field_types` from the registry. Scaled integers are the norm (wire = physical × `scale`); `f32` is permitted where dynamic range demands it; `str16`/`str32`/`str64` are fixed-width, zero-padded UTF-8 (a reader stops at the first NUL or the declared width, whichever comes first).

**STREAM sample layouts MUST NOT contain string fields.** The motion hot path never pays for text.

**Append-only evolution rule:** a layout, once released, may only grow at the tail. Readers MUST parse the prefix they know and ignore trailing bytes; writers MUST NOT reorder, resize, or remove released fields. Consequence: a constrained client compiled against catalog etag *E* still reads every field it knows from a hub whose catalog moved to *E′* by appending — the etag check ([§8.5](catalog.md#s8-5)) then decides *policy* (warn/degrade), not parseability. Removing or changing a field requires allocating a **new channel id** and retiring the old one, which keeps its id forever ([§4.4](foundations.md#s4-4)).

**Explicit field width (forward decodability).** A layout field MAY carry `size` (catalog key 18) — the field's packed width in **bytes**, stated explicitly. Decoders MUST prefer the declared size over the type-derived width; an unknown TYPE with a declared SIZE is then a **skippable hole** rather than a decode wall. Without it, the first registry-added packed type strands every existing client at the first field that uses it: later offsets become unknowable and the entire layout tail goes dark — both shipped generic clients independently carried the identical defensive truncation, which is why this key exists. For known types the declared width MUST equal the type-derived width; conformance checks the agreement, and a mismatch is a catalog-authoring error, never a runtime override.

**STREAM bundle payload layout** (applies to every STREAM channel; the catalog defines only the per-sample struct):

```
offset  size      field
0       4         t_base     u32 hub-time µs of sample[0] (§7.2)
4       1         n          sample count, 1..32
5       1         reserved   zero on send, ignored on receive
6       2×n       t_off[n]   u16 µs offset of sample[i] from t_base
6+2n    S×n       samples    n packed sample structs of catalog-declared size S
```

A bundle MUST satisfy all of:

1. `n` in `1..bundle_max_samples` (32);
2. `t_off[0] == 0`, and `t_off` **strictly increasing** thereafter;
3. span `t_off[n-1] ≤ bundle_max_span_ms` (20 ms);
4. payload length exactly `6 + 2n + S·n`;
5. total frame ≤ the binding's `max_frame`.

A bundle violating any of these is **malformed** and MUST be rejected **whole** — never parsed part-way ([§5.8](#s5-8)). Senders SHOULD fill toward whichever cap binds first; the caps exist to bound latency, buffers, and fragmentation respectively — a bundle never fragments.

**Timestamp meaning depends on `stream_kind`** ([§9.2](channels.md#s9-2)):

- `samples` (0, the default): `t_base + t_off[i]` is the instant sample *i* **describes**. It is observational.
- `segments` (1): `t_base + t_off[i]` is the intended **execution start** of sample *i*, resolved through the [§7.2](time.md#s7-2) nearest-window rule. It is a schedule. A hub MUST clamp scheduling to at most `max_future_schedule_ms` (250 ms) ahead of its own current time; a sample scheduled further out is clamped, not rejected. Clients SHOULD schedule no further ahead than half that budget.

## 5.5 The ESTOP frame {#s5-5}

The ESTOP frame is 12 bytes total and deliberately violates the normal header discipline so that it can be recognized **without deframing**:

```
E5 E5 E5 E5  |  cause:u8  origin:u8  seq:u16  |  crc32:u32
```

- `type` = `0xE5` and the three payload-leading `0xE5` bytes form the 4-byte magic `E5 E5 E5 E5` at frame start. A byte-serial scanner (serial ISR, relay hot path) matching four consecutive `0xE5` bytes MUST treat the following 8 bytes as a candidate ESTOP and validate the CRC-32 (IEEE, over the first 8 bytes) before acting. False-trigger probability with CRC: 2⁻³².
- `cause` is a `safety_causes` value ([§11.1](safety.md#s11-1)) — **one taxonomy, two wire homes**: this byte and the latched `cause` field of the `safety` STATE channel. `origin` is the access tier of the initiator. `seq` increments per **initiation event**: every repeat of one initiation carries the same `seq` ([§11.2](safety.md#s11-2)).
- End-to-end semantics — repeat-until-latched, clearing, and relay obligations — are in [§11.2](safety.md#s11-2). This section defines only the bytes.
- `0xE5` does not collide with the CBOR profile (no simple values beyond `false`/`true`/`null` are legal, [§5.3](#s5-3)) and COBS handling on serial is specified in [§13.5](transports.md#s13-5).

## 5.6 Fragmentation and reassembly {#s5-6}

Fragmentation exists **only** for control-plane frames that cannot fit the binding MTU (in practice: large ECHOs and blob requests on constrained bindings). Data-plane frames MUST NOT fragment: STATE payloads fit `min_transport_payload` (242 B) by catalog design ([§9.1](channels.md#s9-1)), and STREAM bundles size themselves to the MTU ([§5.4](#s5-4)). Blob transfer is chunked at the application layer ([§8.7](catalog.md#s8-7)), not fragmented; store imports larger than `max_frame` ride blob chunks, **never** fragmented INTENTs.

Fragments carry the same `type`/`channel`/`seq` with flags: first = `FRAG_START|FRAG_MORE`, middle = `FRAG_MORE`, last = neither (the reassembler knows it is mid-stream), single = `FRAG_START`. Fragment payloads carry a 2-byte prefix `frag_index:u16`. Reassembly is keyed per (session, type, seq); timeout `frag_reassembly_timeout_ms` (5 s) then discard and NACK `REASSEMBLY_TIMEOUT`; at most `frag_max_concurrent_per_session` (2) concurrent reassemblies, excess discards the oldest. Bindings whose MTU exceeds every control message never emit fragments; receivers MUST still implement reassembly, because relays may downgrade the path MTU.

A reassembler MUST refuse an over-capacity transfer **and its numbers**: declared counts and lengths from a refused transfer MUST NOT be retained or subsequently used ([§5.8](#s5-8)).

## 5.7 Registries and governance {#s5-7}

`registry/registry.yaml` is the single source of truth for frame types, flags, CBOR keys and every scoped sub-key space, channel-id ranges, core channels, channel classes, stream kinds, access levels, priority classes, NACK/GOODBYE codes, packed field types, setting categories and flags, field roles, event kinds, safety ops and causes, admin ops, pairing modes, presentation modes, trust states, blob namespaces, log levels, procedure phases, and every numeric limit.

Appendices [A](appendices.md#appendix-a), [B](appendices.md#appendix-b) and [G](appendices.md#appendix-g) are **generated views** of it. Spec text citing a number that disagrees with the registry is a spec bug; the registry wins. Allocation is by PR; the experimental ranges are the sandbox; nothing is renumbered post-tag ([§4.4](foundations.md#s4-4)).

Implementations MUST NOT hand-maintain a second copy of any registry table. Where a language needs constants, they are generated from the registry.

**Scope of "the registry wins".** The registry is authoritative for **values**: numbers, names, ranges, enumerations, limits, and the semantics recorded in their notes. It is *not* authoritative for the **location of prose**: the `ref:` fields on registry entries are convenience pointers into this document and MAY lag a section renumber. Where a `ref:` points at a section whose content has moved, the section headings of this document are the authority for *where a rule is written*, and the registry remains the authority for *what the number is*. A stale `ref:` is an editorial defect, never a wire conflict. **Known instance at v1.0:** the v1.0 rewrite inserted [§6.4](session.md#s6-4) (readiness) and split [§12.2](security.md#s12-2), shifting several `ref:` targets by one subsection; [§18-19](limitations.md#s18) records it.

## 5.8 Parser totality and defensive decoding *(normative)* {#s5-8}

Every conforming parser — hub **and client** — MUST map **any** byte string to accept-or-reject. Specifically:

1. **Totality.** No out-of-bounds read or write, no unbounded allocation, no unbounded recursion, no undefined behavior, for any input including adversarial input. The deterministic profile ([§5.3](#s5-3)), the depth-4 cap, and definite lengths make this achievable; this clause makes it an obligation.
2. **Length fields are never trusted past the enclosing buffer.** A declared length MUST be validated as `declared ≤ remaining`, never as `start + declared ≤ size` — the latter can overflow and pass. Registry string caps (`client_ver_max_bytes`, `desc_max_bytes`, `option_label_max_bytes`, name and kind caps) are **parse-time obligations** in structural payloads: a receiver **rejects** an over-cap string, it does not truncate and continue.
3. **Refusing a transfer refuses its numbers** ([§5.6](#s5-6)).
4. **Diagnostic strings are the sender's problem, structural strings are the receiver's.** A **sender** MUST truncate a diagnostic string (NACK `detail`) to `nack_detail_max_bytes` rather than dropping the message — an over-length detail MUST NOT cause the whole NACK to vanish. A **receiver** MUST reject an over-cap string in a *structural* payload.
5. **Client obligations are symmetric.** A hostile hub MUST NOT be able to crash a conforming client. A client that auto-connects to a discovered service is one malicious hub away from parsing hostile bytes, and the catalog — rich in variable-length strings — is the fattest client-side surface in the protocol. Every clause above binds clients exactly as it binds hubs.
6. **Encode failure is never silence.** A hub that cannot encode a mandatory response (WELCOME, ECHO) MUST close the session — GOODBYE if it can encode one, transport close otherwise. Silently dropping a mandatory response is non-conformant: the peer is left waiting on a message that will never come.
7. **Constant-time comparison** is REQUIRED for every token, proof, and signature comparison.

Conformance evidence for this section is fuzzing, not vectors ([§17.4](conformance.md#s17-4)).
