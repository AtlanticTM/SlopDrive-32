---
title: Foundations
description: >-
  SlopSync clauses 1-4: purpose and non-goals, design philosophy, RFC 2119
  conventions, the honesty clauses, terminology, roles, architecture, and the
  versioning and compatibility model.
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

# Foundations

## 1. Introduction *(informative except §1.4 and §1.5)* {#s1}

### 1.1 Purpose, scope, non-goals {#s1-1}

SlopSync is a hub-and-spoke **device-shadow protocol**: one hub (a machine's main controller) holds the single canonical machine state; any number of clients — browser UIs, hardware remotes, mobile apps, bridges, simulators, streaming-application plugins — connect over heterogeneous transports, announce who they are and what they can do, and thereafter remain in continuous, truthful sync with that state. Clients submit **intents**; the hub applies, clamps, and echoes what was *actually applied*; every subscriber observes the same reality.

**In scope:** session establishment and identity; a self-describing channel catalog carrying enough semantics for a generic client to build its entire settings and control surface from the hub; five channel classes (state, stream, intent, event, store); per-subscriber rate grants with priorities and congestion adaptation; safety semantics (e-stop, deadman, control arbitration, stop taxonomy); a namespaced blob-transfer verb serving the catalog and device stores alike; a tiered pairing and trust model with hub authenticity; parser-totality obligations; bindings for WebSocket, ESP-NOW, BLE GATT, serial, and in-process transports; a relay role; migration from the legacy SlopDrive port-81 protocol.

**Non-goals:**

- **Cloud anything.** SlopSync is LAN/offline-first. There is no broker but the hub, no account system, no telemetry leaving the site.
- **Server-Sent Events.** SSE was evaluated as a telemetry channel (browser-native reconnect is attractive) and rejected: it is text-only (≈+33 % base64 overhead on packed samples), strictly one-way (intents would need a side channel), and its reconnect advantage evaporates once one client library implements reconnection for every consumer. The browser binding is WebSocket ([§13.2](transports.md#s13-2)).
- **Replacing TCode as an ecosystem interface.** Existing TCode text edges remain supported as compatibility ingest ([§15.1](legacy.md#s15-1)).
- **Peer-to-peer sync.** Clients never talk to each other; all truth flows through the hub.
- **Being a general-purpose RPC or file-transfer protocol.** The blob verb ([§8.7](catalog.md#s8-7)) exists to move a catalog and bounded device documents, not arbitrary data.

### 1.2 Design philosophy {#s1-2}

1. **Ground truth, hub-authoritative.** The hub's state is the only state. A client never displays machine state that differs from the device's, in either direction. Connecting *adopts* device state — it never pushes defaults onto a live session. Echoes report **applied (post-clamp)** values, never requests. Optimistic client state is prohibited.
2. **Loss-tolerance by construction, tiered by class.** State frames are idempotent full snapshots — any drop is harmless because the next frame supersedes it. Sample streams are timestamped and sequenced — late data is discardable data. Only intents and timed segments demand delivery care, and each gets exactly the mechanism it needs ([§9.3](channels.md#s9-3), [§10.4](qos.md#s10-4)).
3. **Declare, then trust.** Everything negotiable is negotiated once, at the edges of the session (handshake, subscribe, publish), and then the steady state is dumb and fast. No per-frame capability checks, no per-sample acknowledgements.
4. **The weakest transport writes the rules.** Every guarantee here is stated against unordered, lossy, 250-byte datagrams (ESP-NOW). Anything that works there works everywhere; TCP transports enjoy stronger behavior for free.
5. **Unknown means ignore.** Unknown channels, keys, frame types, roles, flags, trailing bytes: skip them, never disconnect. This single rule is why a v1 remote still works against a v4 hub.
6. **The machine owns motion processing, not the client** ([§9.6](channels.md#s9-6)). If every conforming client would otherwise have to implement a piece of kinematic work, that work belongs on the hub — written once, verifiable, identical for all clients. A client ships its content *as authored* and gets good motion.
7. **The machine owns UI description, not the client** ([§8.8](catalog.md#s8-8)). A control added in firmware appears on every client's next connect, with its label, grouping, units, constraints and explanation coming from the hub. But SlopSync describes what things **are**, never how they **look**: no widget hints, no layout, no ordering metadata, no styling, ever.
8. **One surface.** SlopSync is intended to be a machine's sole application-level communication surface. On the reference device exactly two HTTP duties are permanently exempt, because SlopSync structurally cannot own them: firmware/asset **OTA** (its rights are never derivable from a SlopSync role) and the optional **served-page token sideband** ([§12.8](security.md#s12-8), whose entire security property is browser same-origin policy). Static asset serving is not an API and is not in scope either way.

### 1.3 Prior art and provenance {#s1-3}

SlopSync deliberately steals from systems that survived contact with production, after a research pass confirmed none could be adopted whole ([Appendix H](rationale.md#appendix-h)):

- **ThingSet** — the self-describing catalog: clients discover channels, types, units, and access rights from the device itself ([§8](catalog.md#s8)).
- **ESPHome native API** — the versioned Hello handshake with identity + entity discovery + subscription streaming ([§6](session.md#s6)).
- **Micro XRCE-DDS** — the minimal transport abstraction: a binding is four operations plus declared properties ([§13.1](transports.md#s13-1)).
- **SlopDrive port-81 protocol** — the working ancestor: `cfg_gen` epochs, CLOCK t0/t1/t2 sync, batched samples, CMD/ECHO idempotency all originate there and are generalized here ([§15.2](legacy.md#s15-2)).
- **MQTT retained messages** — the retained-value-on-subscribe rule ([§9.1](channels.md#s9-1)), implemented at the channel layer since no embeddable broker provides it.
- **Matter / Chromecast commissioning** — possession-is-root bootstrap and the physical-presence window ([§12.3](security.md#s12-3)).
- **BLE Secure Simple Pairing association models** — one ceremony, several association modes chosen by joiner I/O capability ([§12.3](security.md#s12-3)).

### 1.4 Conventions *(normative)* {#s1-4}

The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT**, **SHOULD**, **SHOULD NOT**, **MAY** are to be interpreted as in RFC 2119.

All multi-byte integers on the wire are **little-endian**. Bit 0 is the least-significant bit. Sizes are in bytes unless stated. `u8/u16/u32/i8/i16/i32/f32` denote fixed-width integers and IEEE-754 binary32. Hex literals are `0x`-prefixed. Field diagrams read left-to-right in transmission order. Where this document names a wire number in prose, it is quoting the registry ([§5.7](wire-format.md#s5-7)); where it names a limit by identifier (e.g. `deadman_default_ms`), the registry holds the value and [Appendix G](appendices.md#appendix-g) reproduces it.

Access tiers are named **watch**, **control** and **configure** throughout ([§12.2](security.md#s12-2)). The v1-draft names *viewer*, *controller* and *admin* denote the same three wire values `0/1/2` and appear in this document only where a legacy name is being retired.

### 1.5 Index of honesty clauses *(normative)* {#s1-5}

Each of the following is a normative limitation of `slopsync/1`. An implementation MUST NOT present a user-facing claim that contradicts one, and SHOULD surface the limitation where a user could reasonably assume otherwise.

| # | Clause | Where |
|---|---|---|
| H1 | The protocol ESTOP is a **software convenience layered above** the hardware e-stop path, never a substitute for it. | [§11.2](safety.md#s11-2) |
| H2 | ESTOP queue preemption is a **per-hop** guarantee, not end-to-end latency: bytes already in flight ahead of it still drain first. | [§11.2](safety.md#s11-2), [§14.3](transports.md#s14-3) |
| H3 | The PIN pairing proof is **offline brute-forceable** by a passive observer of the exchange (4 digits = 10⁴ HMACs). It prevents casual and drive-by pairing; it is not a cryptographic access control. | [§12.3](security.md#s12-3) |
| H4 | v1 transports are **cleartext**. A passive LAN observer is outside the threat model; a bearer token presented in HELLO is sniffable, which is why proof presentation exists and is RECOMMENDED. | [§12.1](security.md#s12-1), [§12.4](security.md#s12-4) |
| H5 | Active LAN MITM (including a clone page proxying a PIN to the real hub) is **outside the v1 threat model**. | [§12.1](security.md#s12-1), [§12.8](security.md#s12-8) |
| H6 | The client-version change tripwire is a **tripwire, not attestation**. A deliberately malicious update lies about its version and keeps its token. | [§12.6](security.md#s12-6) |
| H7 | A device that never reports a `client_ver` can **never trip** the version tripwire. | [§12.6](security.md#s12-6) |
| H8 | The served-page token ([§12.8](security.md#s12-8)) closes the browser-borne mass-automatable class only. A **native process on the LAN** can request it — that attacker class already defeats the cleartext ceiling (H4), so nothing is newly lost, but nothing is protected from it either. | [§12.8](security.md#s12-8) |
| H9 | The hub signature proves **which machine**, not that the machine is uncompromised; and a hub with no keypair answering with silence is conformant, so only a client holding a **pinned** key may read silence as failure. | [§12.5](security.md#s12-5) |
| H10 | Relay reliability is **hop-by-hop**. The hub knowing a frame reached the relay does not mean the client got it. | [§14.2](transports.md#s14-2) |
| H11 | The hub's handoff sanity bound ([§9.6](channels.md#s9-6)) is **lookahead-bounded**: it can only act when a segment's successor is already scheduled, which is not guaranteed for long segments. | [§9.6](channels.md#s9-6), [§18](limitations.md#s18) |
| H12 | Denial of service is out of scope. A LAN attacker can jam the radio regardless of anything this document says. | [§12.1](security.md#s12-1) |

## 2. Terminology, Roles, and State Machines *(normative)* {#s2}

### 2.1 Glossary {#s2-1}

- **Hub** — the single authoritative endpoint; owns machine state, the catalog, and all grants. Exactly one per machine.
- **Client** — any endpoint that establishes a session with the hub.
- **Relay** — a forwarding node between the hub and clients on transports the hub cannot reach directly ([§14](transports.md#s14)). A relay is not a session peer; it is invisible to the session layer except where [§14](transports.md#s14) says otherwise.
- **Session** — the stateful association between one client and the hub, created by HELLO/WELCOME, destroyed by GOODBYE, eviction, reaping, or transport loss.
- **Channel** — a named, numbered, typed data flow declared in the catalog.
- **Channel class** — STATE, STREAM, INTENT, EVENT or STORE ([§9](channels.md#s9)). Note: "STREAM" is a channel class. Transports are described as *stream-oriented* (ordered byte pipes: TCP, serial) or *datagram-oriented* (discrete, possibly lossy/unordered: ESP-NOW, BLE notifications) — never as "stream transports", to avoid collision.
- **Grant** — the hub's applied answer to a subscription or publication request: which channel, at what rate, at what priority, with what burst. Grants are truth; requests are wishes.
- **Shadow** — the client-side replica of subscribed state, maintained exclusively from hub frames.
- **Access tier** — `watch` (0), `control` (1) or `configure` (2); [§12.2](security.md#s12-2).
- **Controller** — colloquially, a session holding `control` or above. Where authorization is meant precisely this document names the tier.
- **Active source** — the motion-arbitration input source currently driving motion ([§11.3](safety.md#s11-3), [§11.4](safety.md#s11-4)).
- **Constrained client** — a client using the etag-pinned static profile ([§8.5](catalog.md#s8-5)): compiled-in catalog, canned CBOR templates, no dynamic parsing.
- **Setting** — a catalog layout field carrying `setting_key` ([§8.8](catalog.md#s8-8)): stored configuration, adoptable into a control. A field without `setting_key` is read-only *effective state* or telemetry and MUST NOT be written back into a setting's shadow.
- **Store** — a catalog-declared collection of opaque, slot-addressed items moved by the blob verb ([§8.7](catalog.md#s8-7)).
- **Ready** — a per-session flag meaning "this client possesses the catalog it will decode with" ([§6.4](session.md#s6-4)). Both data and intent planes are gated on it.

### 2.2 Roles and their state machines {#s2-2}

**Client session state machine (normative):**

```
CLOSED → (transport up) → CONNECTING → (send HELLO) → HELLO_SENT
HELLO_SENT → (WELCOME) → SYNCING            # catalog possession, then readiness
HELLO_SENT → (NACK)    → CLOSED
SYNCING → (etag already matched, or catalog assembled+verified and
           CATALOG_READY sent) → READY
READY → (all subscribed STATE channels received once) → LIVE
LIVE|READY|SYNCING → (transport loss / GOODBYE / eviction) → CLOSED
any state → (send/observe ESTOP) → same state   # ESTOP is orthogonal to session state
```

A client MUST NOT act on user input that requires hub state before reaching LIVE, and MUST visually distinguish SYNCING/READY from LIVE (a UI showing stale-or-absent data as fresh violates [§1.2-1](#s1-2)).

**Hub, per session:** `ACCEPTING → VALIDATING (HELLO) → GRANTED (WELCOME sent) → READY → LIVE → CLOSED`. The hub MUST bound VALIDATING (RECOMMENDED 2 s) and drop clients that stall mid-handshake. A session that reaches GRANTED but never READY is closed at `catalog_ready_timeout_ms` ([§6.4](session.md#s6-4)). RFC-042 ([§6.6](session.md#s6-6)) inserts a fifth, library-internal state between LIVE and CLOSED: `LIVE → (silence past its liveness window, or an out-of-band transport loss) → STALE → (any frame on its transport, or a reattaching HELLO) → LIVE`. `STALE` is never itself wire-visible; a client observes only that its own session resumed (or, once evicted under slot pressure, that it did not).

**Relay:** `IDLE → PAIRED → FORWARDING`, with the ESTOP fast-path obligation ([§14.2](transports.md#s14-2)) active in every state after PAIRED.

## 3. Architecture Overview *(informative)* {#s3}

### 3.1 Topology and layering {#s3-1}

```
                        ┌────────────────────────────┐
  browser UI ──WS──────►│                            │
  mobile app ──WS──────►│           HUB              │──── motion arbiter ──► motor driver
  desktop sim ─in-proc─►│  (device firmware /        │         ▲
  BLE remote ──BLE─────►│   hub role of the library) │   (sole caller — no
  TCode app ───legacy──►│                            │    SlopSync session
                        └──────────▲─────────────────┘    touches the driver)
                                   │ UART
                             ┌─────┴─────┐
                             │   RELAY   │
                             └─────▲─────┘
                                   │ ESP-NOW (250-byte datagrams)
                          remote displays, dongle clients
```

Layering, bottom-up: **transport binding** ([§13](transports.md#s13): open/close/write/read + declared MTU/ordering/reliability/`max_frame`) → **framing** ([§5](wire-format.md#s5): 8-byte header + payload, fragmentation if unavoidable) → **channel layer** ([§9](channels.md#s9): class semantics per channel) → **session layer** ([§6](session.md#s6): identity, readiness, grants, liveness, reconnect) → **trust layer** ([§12](security.md#s12): tiers, pairing, authenticity).

**Normative architectural rule** (restated normatively in [§11.4](safety.md#s11-4)): SlopSync sessions terminate at the hub's session engine, which submits intents to the machine's **motion arbiter** — the only component permitted to command the motor driver. No SlopSync-originated data reaches the driver by any other path. A hub implementation that lets a session bypass its arbiter is non-conformant.

### 3.2 Worked narratives {#s3-2}

*(Full annotated traces are in [`examples/session-traces.md`](traces.md); [Appendix E](appendices.md#appendix-e) indexes them.)*

- **A browser connects:** WS upgrade with subprotocol `slopsync.v1` → HELLO (identity, token, subscription and publication wishes) → WELCOME (session id, boot id, roles, grants, catalog etag, hub identity, limits) → client's cached etag matches, so it is READY on the spot and downloads nothing → hub pushes retained STATE for every granted channel → client reaches LIVE and renders, entirely from device truth.
- **A remote nudges speed:** INTENT {channel: config-set, value: 420, intent_id: 17} → hub clamps to 400 (its ceiling), applies via the arbiter, bumps `cfg_gen` because the applied value actually changed → ECHO {intent_id: 17, applied: 400, cfg_gen} to the sender → STATE update to *every* subscriber including the sender. Every screen now shows 400. Nobody shows 420, including the remote that asked for it.
- **The wifi dies mid-stroke:** streaming client vanishes → hub deadman fires at 600 ms → the session is marked `STALE` (RFC-042 — its slot, `session_id` and grants are RETAINED, not torn down) and the streaming source's ownership is released, unconditionally → the machine has no fresh command to execute and settles to rest on its own, latching nothing ([§11.3](safety.md#s11-3)/RFC-045) → `control-owner` updates so any authorized session may claim the source next → if the client's WiFi recovers, a fresh HELLO on the new connection REATTACHES the same session identity ([§6.3](session.md#s6-3)) rather than starting over. Had a hub-autonomous pattern been driving instead, its `source.background_run` setting would have governed (default `false`: the generator stops; `true`: it keeps running, unowned, stoppable only by the role-exempt `stop`/`estop` ops) — either way, nothing latches, and it never depended on any client staying connected.
- **A new remote is adopted:** the remote has one button and no screen. It knocks (bare PAIR_REQ). The knock appears as protocol state on the pending-pairing channel and as an event; the operator's phone — any `configure` session, not "the WebUI" — approves it at the `control` tier; PAIR_GRANT delivers a token and the hub's public key. From then the remote can verify it is talking to *that* machine.

## 4. Versioning and Compatibility Model *(normative)* {#s4}

### 4.1 Protocol version negotiation {#s4-1}

HELLO carries `proto_ver` (key 1), the highest major version the client speaks. The hub replies WELCOME with the version it will serve — the highest common version ≤ its own. If none is servable: NACK `UNSUPPORTED_VERSION` and close. Within a major version all evolution is additive and governed by [§4.3](#s4-3); there are no minor versions on the wire.

### 4.2 The four tokens {#s4-2}

Four version-like tokens coexist. They answer different questions and MUST NOT be conflated:

| Token | Question it answers | Changes when | Carried in |
|---|---|---|---|
| `proto_ver` | "What wire grammar are we speaking?" | Spec major revision | HELLO, WELCOME |
| `catalog_etag` | "What channels/schemas/annotations does this hub expose?" | Firmware update, or any catalog content change | WELCOME, HELLO, CATALOG_READY, channel `catalog` |
| `cfg_gen` | "Which generation of *config content* is current?" | Any **effective** config change, at runtime | WELCOME, ECHO, config STATE frames, INTENT `precondition` |
| `fw_version` | "What software is this machine running?" | Hub firmware update | WELCOME `identity` ([§6.3](session.md#s6-3)) |

Rules:

1. A `cfg_gen` bump MUST NOT change `catalog_etag`, and a catalog content change MUST change the etag ([§8.3](catalog.md#s8-3)).
2. **`cfg_gen` advances if and only if at least one applied configuration value actually changed, regardless of who or what changed it.** An accepted but value-identical write still receives its post-clamp ECHO (ground truth is unaffected) but MUST NOT bump `cfg_gen` and MUST NOT trigger an on-change STATE republish. Symmetrically, a configuration change originating *on the machine* — a physical control, boot adoption, an internal recalculation — MUST bump `cfg_gen`; otherwise a client's `precondition` compare-and-set passes against config that has already moved. Both directions are required; either alone is a bug.
3. A hub whose catalog can change without reboot MUST emit an updated `catalog` STATE frame carrying the new etag, and clients MUST treat an observed etag change as a demand to re-enter SYNCING ([§6.4](session.md#s6-4)). On hubs where a firmware update implies reboot, the new `boot_id` forces a full reconnect anyway — but the etag path MUST still be correct, because simulators and host hubs exercise it.
4. `fw_version` has exactly one wire home: WELCOME `identity` ([§6.3](session.md#s6-3)). It MUST NOT be duplicated onto a STATE channel; two homes drift.

### 4.3 Tolerance rules {#s4-3}

A conformant endpoint, on receiving:

- an **unknown frame type** — MUST ignore the frame (length is always in the header, so skipping is safe);
- an **unknown channel id** — MUST ignore the frame;
- an **unknown CBOR map key**, at any nesting level including scoped sub-maps — MUST ignore the pair;
- **trailing bytes** beyond a known packed layout — MUST ignore them ([§5.4](wire-format.md#s5-4) append-only rule);
- an unknown **NACK/GOODBYE code** — MUST treat it as the generic code of its range (high byte);
- an unknown **field role, setting flag, category, stream kind, pairing mode bit, procedure phase, event kind, or blob namespace** — MUST fall back to the generic behavior its section defines, never reject.

Endpoints MUST NOT disconnect, NACK, or log-spam over any of the above. The *sender* of novelty carries the compatibility burden of making it ignorable.

Tolerance is not permissiveness: ignoring an unknown key is required, whereas accepting a **structurally invalid** payload is forbidden ([§5.8](wire-format.md#s5-8)). "I do not know what key 99 means" and "this string claims to be 2⁶⁴ bytes long" are different questions with different answers.

### 4.4 Evolution policy and reserved ranges {#s4-4}

Additions (new frame types, keys, channels, codes, roles, categories) land in `registry.yaml` by PR and appear in the next tagged spec.

**Numbers are never reused or renumbered after a tagged release.** This rule binds from the **v1.0 tag forward**. The preceding v1-draft was a feasibility exercise and never a public release; the v1.0 base pass therefore restructured freely, retiring frame types `0x09` (CATALOG_REQ) and `0x0A` (CATALOG_CHUNK) without reallocating them — a stale draft-era peer meets an unknown type and fails loudly rather than misreading a BLOB frame ([§4.3](#s4-3) makes "loudly" mean "ignored", which is the correct failure).

Reserved ranges: frame types `0x02` and `0x21–0x3F` spec/core, `0x40–0x7F` future spec, `0x80–0xDF` experimental, `0xE0–0xFF` reserved except `0xE5`. CBOR keys 1–63 core, 64–127 reserved, 128+ experimental. Channel ids per `channel_id_ranges`. Blob namespaces 0–127 spec, 128–255 device. Setting categories 0–127 spec, 128–255 device. Procedure phases 0–127 spec, 128–255 device.

**Experimental ranges MUST NOT appear in tagged releases.**

Breaking the wire grammar requires a `proto_ver` bump, which requires exceptional justification. The intended lifetime of `slopsync/1` is the lifetime of the hardware.

### 4.5 Every refusal is answered {#s4-5}

**A conforming implementation never declines silently: every frame or transfer it cannot honor has a wire signal.** [§6.7](session.md#s6-7)'s SUBSCRIBE rule is the instance that was paid for in debugging time; the principle is general, and it binds clients as well as hubs. Three named applications beyond SUBSCRIBE:

1. A **client** that cannot accept a declared blob (`total_bytes` over its reassembly budget) MUST GOODBYE with `BLOB_REFUSED` (0x0503) rather than idle in a half-session ([§8.4](catalog.md#s8-4)). The observed failure: a refused catalog transfer produced a session that went LIVE with no catalog, no error anywhere, and a `READY_TIMEOUT` fifteen seconds later that blamed the client. Refusal is legal; silent refusal is not.
2. A HELLO whose `token` field is **present but malformed** (wrong length or type) fails decode and MUST be answered NACK `MALFORMED` — never silently demoted to watch tier. A *tokenless* HELLO keeps its legitimate watch-tier path; only present-but-broken credentials become loud. Under enforcement, silent demotion presents as "connects, plays nothing".
3. Idle reaping ([§6.6](session.md#s6-6)) sends GOODBYE `IDLE_REAPED` (0x010C). It is distinct from `DEADMAN_TIMEOUT` on purpose: `DEADMAN_TIMEOUT` now means ONLY a [§11.3](safety.md#s11-3) deadman firing on a source-owning session. Reaping a dark viewer is housekeeping with zero motion consequence, and before the code existed it was reported with the motion-safety code in every log and client.
