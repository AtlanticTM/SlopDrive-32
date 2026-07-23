# SlopSync Protocol Specification

**Version:** slopsync/1 (v1-draft)
**Status:** Draft for implementation — normative once tagged `v1.0`
**Registry of record:** [`registry/registry.yaml`](registry/registry.yaml) — Appendices A/B/G are views of it; on any conflict the registry wins.

---

## 1. Introduction *(informative except §1.4)*

### 1.1 Purpose, scope, non-goals

SlopSync is a hub-and-spoke **device-shadow protocol**: one hub (the machine's main controller) holds the single canonical machine state; any number of clients — browser UIs, hardware remotes, mobile apps, bridges, simulators — connect over heterogeneous transports, announce who they are and what they can do, and thereafter remain in continuous, truthful sync with that state. Clients submit **intents**; the hub applies, clamps, and echoes what was *actually applied*; every subscriber observes the same reality.

**In scope:** session establishment and identity; a self-describing channel catalog; four channel classes (state, stream, intent, event); per-subscriber rate grants with priorities and congestion adaptation; safety semantics (e-stop, deadman, control arbitration); a PIN-pairing security baseline; bindings for WebSocket, ESP-NOW, BLE GATT, serial, and in-process transports; a relay role; migration from the legacy SlopDrive port-81 protocol.

**Non-goals:**

- **Cloud anything.** SlopSync is LAN/offline-first. There is no broker but the hub, no account system, no telemetry leaving the site.
- **Server-Sent Events.** SSE was evaluated as a telemetry channel (browser-native reconnect is attractive) and rejected: it is text-only (≈+33 % base64 overhead on packed samples), strictly one-way (intents would need a side channel), and its reconnect advantage evaporates once `slopsync-js` implements reconnection once for every consumer. The browser binding is WebSocket (§13.2).
- **Replacing TCode as an ecosystem interface.** Existing TCode text edges remain supported as compatibility ingest (§15.1). SlopSync-native motion streams are the upgrade path, not a flag day.
- **Peer-to-peer sync.** Clients never talk to each other; all truth flows through the hub.

### 1.2 Design philosophy

1. **Ground truth, hub-authoritative.** The hub's state is the only state. A UI never displays machine state that differs from the device's, in either direction. Connecting *adopts* device state — it never pushes defaults onto a live session. Echoes report **applied (post-clamp)** values, never requests. Optimistic client state is prohibited.
2. **Loss-tolerance by construction, tiered by class.** State frames are idempotent full snapshots — any drop is harmless because the next frame supersedes it. Streams are timestamped and sequenced — late data is discardable data. Only intents demand end-to-end confirmation, and they get it (ECHO).
3. **Declare, then trust.** Everything negotiable is negotiated once, at the edges of the session (handshake, subscribe), and then the steady state is dumb and fast. No per-frame capability checks, no per-sample acknowledgements.
4. **The weakest transport writes the rules.** Every guarantee in this spec is stated against unordered, lossy, 250-byte datagrams (ESP-NOW). Anything that works there works everywhere; TCP transports simply enjoy stronger behavior for free.
5. **Unknown means ignore.** Unknown channels, unknown CBOR keys, unknown frame types, unknown trailing bytes: skip them, never disconnect. This single rule is why a v1 remote still works against a v4 hub.

### 1.3 Prior art and provenance

SlopSync deliberately steals from systems that survived contact with production, after a research pass confirmed none of them could be adopted whole (see Appendix H):

- **ThingSet** — the self-describing catalog: clients discover channels, types, units, and access rights from the device itself (§8).
- **ESPHome native API** — the versioned Hello handshake with identity + entity discovery + subscription streaming (§6).
- **Micro XRCE-DDS** — the minimal transport abstraction: a binding is four operations (open/close/write/read) plus declared properties (§13.1).
- **SlopDrive port-81 protocol** — the working ancestor: `cfg_gen` epochs, CLOCK t0/t1/t2 sync, batched samples, CMD/ECHO idempotency all originate there and are generalized here (§15.2).
- **MQTT retained messages** — the retained-value-on-subscribe rule (§9.1), implemented at the channel layer since no embeddable broker provides it.

### 1.4 Conventions *(normative)*

The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT**, **SHOULD**, **SHOULD NOT**, **MAY** are to be interpreted as in RFC 2119.

All multi-byte integers on the wire are **little-endian**. Bit 0 is the least-significant bit. Sizes are in bytes unless stated. `u8/u16/u32/i8/i16/i32/f32` denote fixed-width integers and IEEE-754 binary32. Hex literals are `0x`-prefixed. Field diagrams read left-to-right in transmission order.

---

## 2. Terminology, Roles, and State Machines *(normative)*

### 2.1 Glossary

- **Hub** — the single authoritative endpoint; owns machine state, the catalog, and all grants. Exactly one per machine.
- **Client** — any endpoint that establishes a session with the hub.
- **Relay** — a forwarding node between the hub and clients on transports the hub cannot reach directly (§14). A relay is not a session peer; it is invisible to the session layer except where §14 says otherwise.
- **Session** — the stateful association between one client and the hub, created by HELLO/WELCOME, destroyed by GOODBYE/eviction/timeout.
- **Channel** — a named, numbered, typed data flow declared in the catalog.
- **Channel class** — STATE, STREAM, INTENT, or EVENT (§9). Note: "STREAM" is a channel class. Transports are described as *stream-oriented* (ordered byte pipes: TCP, serial) or *datagram-oriented* (discrete, possibly lossy/unordered: ESP-NOW, BLE notifications) — never as "stream transports", to avoid collision.
- **Grant** — the hub's applied answer to a subscription request: which channel, at what rate, at what priority. Grants are truth; requests are wishes.
- **Shadow** — the client-side replica of subscribed state, maintained exclusively from hub frames.
- **Controller** — a session holding `controller` (or `admin`) role via a pairing token (§12).
- **Active source** — the MotionArbiter input source currently driving motion (§11.3).
- **Constrained client** — a client using the etag-pinned static profile (§8.5): compiled-in catalog, canned CBOR templates, no dynamic parsing.

### 2.2 Roles and their state machines

**Client session state machine (normative):**

```
CLOSED → (transport up) → CONNECTING → (send HELLO) → HELLO_SENT
HELLO_SENT → (WELCOME) → SYNCING          # adopting snapshots, catalog check
HELLO_SENT → (NACK)    → CLOSED
SYNCING → (all subscribed STATE channels received once) → LIVE
LIVE → (transport loss / GOODBYE / eviction) → CLOSED
any state → (send/observe ESTOP) → same state   # ESTOP is orthogonal to session state
```

A client MUST NOT act on user input that requires hub state before reaching LIVE, and MUST visually distinguish SYNCING from LIVE (a UI showing stale-or-absent data as fresh violates §1.2-1).

**Hub, per session:** `ACCEPTING → VALIDATING (HELLO) → GRANTED (WELCOME sent) → LIVE → CLOSED`. The hub MUST bound VALIDATING (recommended 2 s) and drop clients that stall mid-handshake.

**Relay:** `IDLE → PAIRED → FORWARDING`, with the ESTOP fast-path obligation (§14.2) active in every state after PAIRED.

---

## 3. Architecture Overview *(informative)*

### 3.1 Topology and layering

```
                        ┌────────────────────────────┐
  browser UI ──WS──────►│                            │
  mobile app ──WS──────►│           HUB              │──── MotionArbiter ──► motor driver
  desktop sim ─in-proc─►│  (ESP32-S3 firmware /      │         ▲
  BLE remote ──BLE─────►│   slopsync-core hub role)  │   (sole caller — no
  TCode app ───legacy──►│                            │    SlopSync session
                        └──────────▲─────────────────┘    touches the driver)
                                   │ UART
                             ┌─────┴─────┐
                             │   RELAY   │  (C5 node)
                             └─────▲─────┘
                                   │ ESP-NOW (250-byte datagrams)
                          OLED remote, dongle clients
```

Layering, bottom-up: **transport binding** (§13: open/close/write/read + declared MTU/ordering/reliability) → **framing** (§5: 8-byte header + payload, fragmentation if unavoidable) → **channel layer** (§9: class semantics per channel) → **session layer** (§6: identity, grants, liveness, reconnect).

**Normative architectural rule:** SlopSync sessions terminate at the hub's session engine, which submits intents to the **MotionArbiter** — the only component permitted to command the motor driver. No SlopSync-originated data reaches the driver by any other path. This restates the firmware's sole-caller doctrine as a protocol obligation: a hub implementation that lets a session bypass its arbiter is non-conformant.

### 3.2 Worked narratives

*(Full annotated byte-level traces are in [`examples/session-traces.md`](examples/session-traces.md); Appendix E indexes them.)*

- **A browser connects:** WS upgrade with subprotocol `slopsync.v1` → HELLO (identity, token, subscription wishes) → WELCOME (session id, boot id, roles, grants, catalog etag, clock bootstrap, limits) → client sees its cached etag matches, skips catalog download → hub pushes retained STATE for every granted channel → client reaches LIVE and renders — entirely from device truth.
- **A remote nudges speed:** INTENT {channel: speed-config, value: 420, intent_id: 17} → hub clamps to 400 (its ceiling), applies via arbiter, bumps `cfg_gen` → ECHO {intent_id: 17, applied: 400, cfg_gen} to the sender → STATE update to *every* subscriber, including the sender and the WebUI across the room. Every screen now shows 400. Nobody shows 420, including the remote that asked for it.
- **The wifi dies mid-stroke:** streaming client vanishes → hub deadman fires at 600 ms → active STREAMING source decel-stops → `safety` STATE latches STOP with cause=deadman → every surviving subscriber renders it. The pattern engine, had it been driving, would have kept running (§11.3) — it never depended on any client.

---

## 4. Versioning and Compatibility Model *(normative)*

### 4.1 Protocol version negotiation

HELLO carries `proto_ver` (key 1), the highest major version the client speaks. The hub replies WELCOME with the version it will serve — the highest common ≤ its own. If none is servable: NACK `UNSUPPORTED_VERSION` and close. Within a major version, all evolution is additive and governed by §4.3; there are no minor versions on the wire.

### 4.2 The three tokens

Three version-like tokens coexist. They answer different questions and MUST NOT be conflated:

| Token | Question it answers | Changes when | Carried in |
|---|---|---|---|
| `proto_ver` | "What wire grammar are we speaking?" | Spec major revision | HELLO, WELCOME |
| `catalog_etag` | "What channels/schemas does this hub expose?" | Firmware update (or hub restart with different features) | WELCOME, channel 0x0001 |
| `cfg_gen` | "Which generation of *config content* is current?" | Any applied config change, at runtime | WELCOME, ECHO, config STATE frames |

Rules: a `cfg_gen` bump MUST NOT change `catalog_etag`. A catalog change MUST change the etag (§8.3). A hub whose catalog can change without reboot (e.g. the desktop sim) MUST emit updated channel 0x0001 STATE (new etag), and clients MUST treat an observed etag change as a demand to re-run SYNCING. On the ESP32 hub, firmware update implies reboot, which implies a new `boot_id` and full reconnect — the etag path still MUST be correct, because the sim exercises it.

### 4.3 Tolerance rules

A conformant endpoint, on receiving:

- an **unknown frame type** — MUST ignore the frame (length is always in the header, so skipping is safe);
- an **unknown channel id** — MUST ignore the frame;
- an **unknown CBOR map key** — MUST ignore the pair;
- **trailing bytes** beyond a known packed layout — MUST ignore them (§5.4 append-only rule);
- an unknown **NACK code** — MUST treat it as the generic code of its range (high byte).

Endpoints MUST NOT disconnect, NACK, or log-spam over any of the above. The *sender* of novelty carries the compatibility burden of making it ignorable.

### 4.4 Evolution policy and reserved ranges

Additions (new frame types, keys, channels, codes) land in `registry.yaml` by PR and appear in the next tagged spec. Numbers are never reused or renumbered after a tagged release. Experimental ranges (frame types 0x80–0xDF, CBOR keys 128+) MUST NOT ship in tagged releases. Breaking the wire grammar requires a `proto_ver` bump, which requires exceptional justification — the intended lifetime of `slopsync/1` is "the lifetime of the hardware".

---

## 5. Wire Format *(normative)*

### 5.1 Frame header

Every SlopSync frame begins with the same 8 bytes:

```
offset  size  field     notes
0       1     type      frame type (registry `frame_types`)
1       1     flags     bit0 FRAG_START, bit1 FRAG_MORE; others zero
2       2     channel   u16 channel id; 0x0000 for session-scoped frames
4       2     seq       u16 sequence number (§7.3); 0 where the class is unsequenced
6       2     len       u16 payload length in bytes (excluding this header)
```

One SlopSync frame maps to exactly one transport datagram/message where the binding allows (§13); `len` makes frames self-delimiting on byte-pipe bindings. A frame whose `len` exceeds the negotiated maximum is answered with NACK `FRAME_TOO_LARGE` (if a session exists) and discarded.

### 5.2 Frame type registry

The full table lives in `registry.yaml` (`frame_types`) and is reproduced in Appendix A. Core points: HELLO/WELCOME/SUBSCRIBE/GRANT/INTENT/ECHO/EVENT/NACK/GOODBYE/PAIR_* are **control-plane** frames (CBOR payloads, §5.3); STATE/STREAM are **data-plane** frames (packed payloads, §5.4); PING/PONG/CLOCK/PROBE/CATALOG_CHUNK/ESTOP are **raw** frames with fixed layouts defined in their sections.

### 5.3 Control-plane encoding: the CBOR profile

Control payloads are CBOR maps with **integer keys** from the global key registry (`cbor_keys`, Appendix B), restricted to a deterministic subset of RFC 8949:

- definite-length everything (no indefinite strings/arrays/maps);
- integers in shortest form; map keys sorted ascending by encoded bytes;
- floats as binary32 only (never binary16/64); integral values that are semantically integers encoded as integers, not floats;
- no tags, no bignums, no simple values other than `false`/`true`/`null`;
- maximum nesting depth 4.

Rationale (informative): exactly one valid encoding exists for any message, which makes golden vectors byte-exact and lets constrained clients ship **pre-encoded templates** — a canned HELLO with value bytes patched in at runtime is guaranteed to be the same bytes a full encoder would produce. A decoder MAY reject profile violations with NACK `PROFILE_VIOLATION`; it MUST NOT crash on them.

### 5.4 Data-plane encoding: packed layouts

STATE and STREAM payloads are **packed little-endian structs**. There is no encoder: the layout *is* the catalog entry's `layout` array (§8.2) — an ordered list of `(name, type, unit, scale)` fields using `packed_field_types` from the registry. Scaled integers are the norm (`pos_10um: u16` = mm × 100); f32 is permitted where dynamic range demands it.

**Append-only evolution rule:** a layout, once released, may only grow at the tail. Readers MUST parse the prefix they know and ignore trailing bytes; writers MUST NOT reorder, resize, or remove released fields. Consequence: a constrained client compiled against catalog etag *E* still reads every field it knows from a hub whose catalog moved to *E′* by appending — the etag check (§8.5) then decides *policy* (warn/degrade), not parseability. Removing or changing a field requires allocating a **new channel id** and retiring the old one (which keeps its id forever, per §4.4).

STREAM bundle payload layout (applies to every STREAM channel; the catalog defines only the per-sample struct):

```
offset  size      field
0       4         t_base     u32 hub-time µs of sample[0] (§7.2)
4       1         n          sample count, 1..32
5       1         reserved   zero
6       2×n       t_off[n]   u16 µs offset of sample[i] from t_base (t_off[0]=0)
6+2n    S×n       samples    n packed sample structs of catalog-declared size S
```

A bundle MUST satisfy: `n ≤ 32`, span (`t_off[n-1]`) ≤ 20 ms, and total frame ≤ the binding's MTU. Senders SHOULD fill toward whichever cap binds first; the three caps exist to bound latency, buffers, and fragmentation respectively — a bundle never fragments.

### 5.5 The ESTOP frame

The ESTOP frame is 12 bytes total and deliberately violates the normal header discipline so that it can be recognized **without deframing**:

```
E5 E5 E5 E5  |  cause:u8  origin:u8  seq:u16  |  crc32:u32
```

- `type`=0xE5 and the three payload-leading 0xE5 bytes form the 4-byte magic `E5 E5 E5 E5` at frame start. A byte-serial scanner (serial ISR, relay hot path) matching four consecutive 0xE5 bytes MUST treat the following 8 bytes as a candidate ESTOP and validate the CRC-32 (IEEE, over the first 8 bytes) before acting. False-trigger probability with CRC: 2⁻³².
- `cause`: 0 = user, 1 = deadman, 2 = fault, 3 = relay-originated. `origin`: access level of initiator. `seq` increments per initiation event.
- End-to-end semantics — repeat-until-latched, clearing, and relay obligations — are in §11.2. This section defines only the bytes.
- 0xE5 does not collide with the CBOR profile (no simple values beyond false/true/null are legal, §5.3) and COBS handling on serial is specified in §13.5.

### 5.6 Fragmentation and reassembly

Fragmentation exists **only** for control-plane frames that cannot fit the binding MTU (in practice: catalog transfer fallback and large ECHOs on ESP-NOW). Data-plane frames MUST NOT fragment: STATE frames must fit `min_transport_payload` = 242 bytes by catalog design (§9.1), and STREAM bundles size themselves to the MTU (§5.4).

Fragments carry the same `type/channel/seq` with flags: first = FRAG_START|FRAG_MORE, middle = FRAG_MORE, last = neither (the reassembler knows it is mid-stream), single = FRAG_START. Fragment payloads carry a 2-byte prefix: `frag_index:u16`. Reassembly: per (session, type, seq); timeout 5 s then discard and NACK `REASSEMBLY_TIMEOUT`; at most 2 concurrent reassemblies per session (excess: discard oldest). Bindings whose MTU exceeds every control message (WS, serial-COBS, in-process) never emit fragments; receivers MUST still implement reassembly (relays may downgrade the path MTU).

### 5.7 Registries and governance

`registry/registry.yaml` is the single source of truth for frame types, flags, CBOR keys, channel-id ranges, core channels, NACK codes, packed field types, and every numeric limit. Appendices A, B, and G are generated views. Spec text citing a number that disagrees with the registry is a spec bug; the registry wins. Allocation is by PR; the experimental ranges are the sandbox; nothing is ever renumbered post-tag (§4.4).

---

## 6. Session Layer *(normative)*

### 6.1 Identity: three numbers, three jobs

- **`instance_id`** (8 bytes, client-generated once and persisted) — *who this client durably is.* Distinguishes "the same phone reconnecting" from "a second phone". Generated randomly at first run; a client that cannot persist (incognito browser) generates per-load and simply enjoys weaker reconnect semantics.
- **`session_id`** (u32, hub-assigned, random non-zero, unique within a hub boot) — *this particular association.* Not a secret; authorization lives in tokens (§12).
- **`boot_id`** (u32, hub-generated randomly at every boot) — *which incarnation of the hub.* All hub timestamps, seqs, session ids, and idempotency state are scoped to a boot_id; observing a new one invalidates every cached assumption except the catalog etag and pairing tokens.

### 6.2 HELLO (client → hub)

CBOR map: `proto_ver` (1), `client_kind` (2), `client_name` (3), `instance_id` (4), optional `token` (5), optional `catalog_etag` (8) — the etag the client has cached — optional `subscriptions` (10) and `publishes` (11) wish-lists so that simple clients can complete setup in one round trip. Wish-list entries: `{channel_id, rate_hz, priority}`.

### 6.3 WELCOME (hub → client)

CBOR map: `proto_ver` (served version), `session_id`, `boot_id`, `catalog_etag`, `cfg_gen`, `roles` (granted access level: viewer unless a valid token raised it), `limits` (22: at minimum `max_frame`, `max_subscriptions`, `retained_pending` count), `deadman_ms` + `deadman_policy` (as applied to this session), `nonce` (29, for a subsequent PAIR_REQ), and per-wish **grant results** embedded as the same structure GRANT uses (§10.2). WELCOME is the moment grants become truth; anything not granted here needs SUBSCRIBE.

After WELCOME the hub MUST immediately push the **retained value** of every granted STATE channel (§9.1). The client reaches LIVE when all have arrived (§2.2).

**Duplicate identity:** if a HELLO arrives bearing the `instance_id` of a live session, the hub MUST evict the old session (GOODBYE `DUPLICATE_INSTANCE` if its transport still functions) and honor the new HELLO. Half-open zombies die here; two genuinely distinct clients never share an instance_id by construction.

**Admission:** a hub at its client limit answers HELLO with NACK `BUSY` carrying `retry_after_ms` (31). Advertised limits and the conformance floor (≥ 4 concurrent) are in Appendix G.

### 6.4 Network probe (optional, post-WELCOME)

Grants at WELCOME are deliberately conservative defaults — a controller reconnecting mid-motion must not wait on a bandwidth measurement. A client wanting refinement runs the probe *after* going LIVE:

1. Client sends PROBE (raw, empty payload) → hub replies with a timed burst of PROBE frames (raw payload: `probe_index:u16` + padding) totaling `probe_default_bytes` over at most `probe_max_duration_ms`.
2. Client measures received bytes/span/loss and reports PROBE_REPORT (CBOR: `probe_result` map, key 26).
3. Hub MAY raise grants accordingly, announced via unsolicited GRANT (§10.2).

The probe measures the hub→client direction (the telemetry-heavy one). Runtime congestion adaptation (§10.3) continues regardless — the probe sets a better starting point, nothing more.

### 6.5 Liveness

**Any received frame is proof of life.** Dedicated PING (raw, empty; answered by PONG echoing payload) is sent only when a side has been otherwise silent for its interval: 200 ms while the session holds active control (§11.3), 1 s otherwise. A session silent for its deadman window triggers §11.3; a session silent for 3× its idle interval MAY be considered dead and reaped. A 240 Hz streamer therefore never sends PING, and never idles out while streaming.

### 6.6 Mid-session subscription management

SUBSCRIBE (c→h): CBOR `subscriptions` array as in HELLO; answered by GRANT per entry (or NACK with the offending channel in `channel_id`). UNSUBSCRIBE: array of `channel_id`. Rate changes are a re-SUBSCRIBE of the same channel (the grant replaces the old one). This is how a UI opens a 240 Hz scope view for thirty seconds without reconnecting. Subscriptions are capped per session (`max_subscriptions_per_session`, NACK `SUB_LIMIT`).

### 6.7 Reconnect

On transport restoration a client sends a fresh HELLO (same `instance_id`, same `token`, cached `catalog_etag`, its standing wish-list). Then:

- **Etag matches** → skip catalog transfer entirely. **Etag differs or boot_id changed** → full SYNCING including catalog (§8.4).
- **Snapshot adoption is mandatory:** the retained-STATE push (§6.3) *is* the resync; the client MUST discard its shadow and rebuild from it. No client-side state survives a reconnect on its own authority.
- **Idempotency reset:** intent ids are session-scoped (§9.3). Pending unACKed intents from the dead session are *gone* — the client MUST NOT blind-retransmit them; it reconciles by comparing its intended value against the adopted snapshot and re-issuing only if still wanted and still different. This is why relative intents are forbidden (§9.3): "increment by 5" cannot be reconciled against a snapshot; "set to 405" can.
- **Grant reacquisition is not control reacquisition.** Subscriptions re-grant freely. But if the disconnect triggered the deadman and motion stopped, the returning session does NOT silently resume as active source — it must issue a fresh control-taking intent (§11.4). Motion never restarts because a socket reopened.

### 6.8 Teardown

GOODBYE (either direction, CBOR: `code`, optional `detail`) is a courtesy, not a requirement — transports die rudely and every rule above already tolerates it. Hub-initiated GOODBYE codes of note: `SESSION_EVICTED` (slow consumer, §10.4; admin kick), `DUPLICATE_INSTANCE` (§6.3). After GOODBYE the hub frees the session and releases any control ownership per §11.4's loss rules (identical to deadman).

---

## 7. Time and Sequencing *(normative)*

### 7.1 Clock: the hub is the timebase

All protocol timestamps are **hub time**: microseconds (streams) or milliseconds (state/events) since hub boot. Clients never send their own clock in data frames; they *convert* using an offset learned from CLOCK exchanges.

CLOCK (raw, 13 bytes, unchanged from the port-81 ancestor): client sends `0x05` + `t0:u32` (client µs); hub replies `0x05` + `t0:u32` (echo) + `t1:u32` (hub µs at receipt) + `t2:u32` (hub µs at send). Client computes offset = `((t1 − t0) + (t2 − t3))/2` and RTT = `(t3 − t0) − (t2 − t1)` with `t3` = client µs at reply receipt. Clients holding stream subscriptions SHOULD resync every `clock_resync_interval_s` (10 s) and on every RTT spike > 2× median; drift between resyncs is assumed linear and ignored (µs-class drift over 10 s is below sample-offset resolution).

CLOCK exchanges MUST NOT traverse buffering relays unless the relay performs timestamp correction (§14.3); a relay that cannot correct MUST drop CLOCK frames, forcing clients behind it to rely on WELCOME's coarse bootstrap (informative accuracy: ±bundle-interval).

### 7.2 Timestamp formats and wraparound

- STREAM: `t_base` u32 hub-µs (wraps every ~71.6 min) + per-sample u16 µs offsets. Wraparound rule: samples are always near-now; a receiver interprets `t_base` in the ±35.8 min window around its current hub-time estimate. Ancient or far-future values indicate a missed resync, not time travel — resync, don't extrapolate.
- STATE/EVENT: u32 hub-ms (wraps ~49.7 days) with the same nearest-window rule.
- `boot_id` (§6.1) fences all of it: new boot_id ⇒ all prior timestamps, seqs, and offsets are void.

### 7.3 Sequence numbers

`seq` is u16, **per channel per direction**, incrementing by 1, wrapping mod 2¹⁶, compared by serial arithmetic: `a` is newer than `b` iff `0 < (a − b) mod 2¹⁶ < 2¹⁵`. Class-specific rules:

- **STATE:** newest-wins by seq — a frame older than the shadow's seq is silently dropped (this, not arrival order, defeats reordering on datagram bindings). Gaps are meaningless (conflation is legal and expected).
- **STREAM:** bundles carry seq; consumers drop any bundle not newer than the last accepted, and MAY drop individual samples older than the newest rendered timestamp. Gap tolerance is the consumer's business — timestamps, not seqs, drive interpolation.
- **INTENT/ECHO:** seq unused (0); ordering is per-intent via `intent_id`.
- **EVENT:** seq present; used only for duplicate suppression on at-least-once delivery paths.

### 7.4 Time through relays

See §14.3. Summary: a relay MUST either correct timestamps for its buffering delay (it already timestamps arrivals for scheduling — the ESP-NOW ancestor's `rel_ms` mechanism, generalized) or be transparent to CLOCK (zero added asymmetry). Bundling relays satisfying neither MUST drop CLOCK per §7.1.

---

## 8. Catalog *(normative)*

### 8.1 The channel entry

The catalog is the hub's machine-readable self-description: an array of channel entries, each

```
{ id: u16, name: tstr, class: u8 (STATE|STREAM|INTENT|EVENT), dir: u8 (h2c|c2h),
  access: u8 (viewer|controller|admin — level required to SUBSCRIBE or, for
  INTENT channels, to send), max_rate_hz: f32, default_priority: u8,
  layout: [ {name, type, unit, scale, min, max, bits?} ... ]      # packed classes
  schema: { int-key: {name, type, unit, min, max} ... }           # CBOR classes
}
```

`layout` describes packed payloads (STATE/STREAM) field-by-field in wire order; `schema` describes CBOR payloads (INTENT/EVENT) key-by-key. `bits` enumerates bitfield8 meanings. Exactly one of the two is present, per class. The normative encoding of the catalog itself is CDDL-defined in [`schema/catalog.cddl`](schema/catalog.cddl) (Appendix C).

### 8.2 Schema language scope

The layout/schema vocabulary is deliberately small: fixed-width numeric types (registry `packed_field_types`), a scale factor (wire = physical × scale), SI-ish unit strings (`mm`, `mm/s`, `mA`, `degC`, `%`, `count`, `flag`), and min/max for UI slider construction. It describes *values*, not behavior — semantics live in this spec and in channel documentation. Nesting, variable-length fields, and conditionals are out of scope by design; a channel that seems to need them is two channels.

### 8.3 Etag computation

`catalog_etag` = first 8 bytes of SHA-256 over the catalog encoded in the §5.3 deterministic CBOR profile, entries sorted ascending by id. Deterministic encoding makes the hash reproducible from the catalog *content* alone — any implementation, any language, same bytes, same etag. The etag covers ids, names, classes, access, rates, layouts, schemas — everything in §8.1; it does not cover retained *values* (that's `cfg_gen`'s and seq's job).

### 8.4 Transfer

Catalog transfer uses CATALOG_REQ / CATALOG_CHUNK:

- CATALOG_REQ (CBOR): empty map = send everything; `{chunks: [indices]}` = selective repair.
- CATALOG_CHUNK (raw): `chunk_index:u16, chunk_count:u16, payload ≤ 192 bytes` — a byte-range of the deterministic catalog encoding. 192 fits every binding unfragmented; WS MAY carry multiple chunks back-to-back in one message.
- Receiver reassembles by index, requests missing indices after a gap timeout (recommended 500 ms), abandons after `frag_reassembly_timeout_ms` (5 s) total, then either retries from scratch or falls back to the static profile (§8.5). Hub bounds concurrent transfers per its RAM; beyond that, CATALOG_REQ gets NACK `BUSY`.

### 8.5 The static-client profile (etag-pinned)

A constrained client (C5 remote, minimal BLE device) MAY ship with a **compiled-in catalog** and pre-encoded CBOR templates instead of a CBOR stack. Requirements:

- It sends its compiled-in etag in HELLO. If the hub's etag matches: full speed ahead.
- On mismatch it MUST choose a declared behavior: (a) proceed **degraded** — the §5.4 append-only rule guarantees its known prefix of every layout still parses; it MUST suppress any *control* function whose schema it cannot re-verify, or (b) refuse with a user-visible "update me" indication. Silent full operation on a mismatched etag is non-conformant.
- The hub treats static clients identically to dynamic ones; the profile is client-internal except for the etag check. NACK `ETAG_MISMATCH` exists for hubs configured to refuse degraded operation outright (a hub policy, not the default).

### 8.6 Catalog invariance and mid-session change

The catalog is **client-invariant**: every session sees the same entries and the same etag; access control acts at SUBSCRIBE/INTENT time (NACK `ACCESS_DENIED`), never by filtering the catalog. (Per-client catalogs would fracture etag caching and static profiles.) Mid-session catalog change is signaled by channel 0x0001's STATE update (§4.2); clients re-enter SYNCING.

---

## 9. Channel Classes *(normative)*

### 9.1 STATE — the shadow

STATE channels carry **idempotent full snapshots** of a coherent group of fields.

- **Full-snapshot rule:** every STATE frame contains the complete current value of its channel. There are no deltas in slopsync/1 — a delta would make frame loss corrupting, destroying the property the whole design leans on.
- **MTU rule:** a STATE payload MUST fit `min_transport_payload` (242 bytes) unfragmented. This is a *catalog design constraint*: a state group that doesn't fit is split into multiple channels at catalog-design time. Conformance tooling SHOULD flag violations mechanically (layout size is statically known).
- **Retained value:** the hub keeps the latest value of every STATE channel and MUST push it immediately upon grant (connect, re-subscribe, reconnect). This is the device-shadow primitive; it is what "page load adopts device state" compiles to.
- **Conflation:** the hub maintains at most a depth-1 queue per (channel, subscriber) — a newer snapshot replaces a queued unsent one. Subscribers therefore see the freshest state their link can carry, never a backlog. Newest-wins by seq on receive (§7.3).
- **Rate:** `rate_hz` in the grant is a *ceiling* on push frequency; on-change channels (`rate_hz` 0) push at most once per change, conflated. Periodic channels (telemetry) push at min(grant, change rate).
- **Bitfields:** flag-word channels (e.g. `safety`, 0x0003) use `bitfield8` fields with catalog-enumerated bits; a latched safety word is still a full snapshot like everything else.

### 9.2 STREAM — the data plane

STREAM channels carry timestamped sample bundles (§5.4) in either direction (position telemetry h2c; motion input c2h).

- **Ordering:** guaranteed only on ordered bindings. On datagram bindings the consumer rules of §7.3 (drop-not-newer, timestamp-driven consumption) are the whole contract. The per-binding guarantee matrix is §13.1; STREAM consumers MUST be written against the weakest line of that table.
- **Shedding = decimation, newest-biased:** under congestion the hub drops whole bundles or thins samples within bundles, always preserving the most recent samples. It MUST NOT delay-and-burst (a stale motion sample is worse than a missing one — the timestamps make dropped samples recoverable by interpolation, stale delivery is a lie).
- **No acknowledgements.** STREAM frames are never ACKed at the protocol level, in either direction (X-ref §9.3 for why motion *input* correctness doesn't need it).
- **Grants bound sample rate**, not frame rate: a 240 Hz grant delivered as 30 fps × 8-sample bundles is conformant and expected.

### 9.3 INTENT / ECHO — the control plane

INTENT is the only way a client changes anything. CBOR: `channel_id` (an INTENT-class channel), `intent_id` (18), `value` (20, per the channel's schema), optional `precondition` (30).

- **ECHO is mandatory and truthful:** the hub replies ECHO {`intent_id`, `applied` (19) = the post-clamp values actually in effect, `cfg_gen`} — or NACK {`intent_id` in detail, code}. `applied` MAY differ from requested (clamps); the client's shadow updates from ECHO and the ensuing STATE broadcast, never from its own request. All *other* subscribers learn of the change via STATE — ECHO goes only to the sender.
- **Idempotency:** `intent_id` is session-scoped, client-assigned, monotonically increasing. The hub keeps a ring of the last 32 (id → ECHO) per session; a duplicate id re-emits the stored ECHO and MUST NOT re-apply. The ring dies with the session (§6.7) — which is safe *because*:
- **Absolute values only.** Intent schemas MUST express target state ("set speed 400"), never operations on current state ("add 20"). A client wanting increments computes the absolute target from its shadow and MAY guard against races with `precondition` = expected `cfg_gen`; mismatch → NACK `CONFLICT`, client re-reads and retries. This single rule is what makes the reconnect story (§6.7) sound and two-admin racing (X2) merely annoying instead of corrupting.
- **Rate limiting:** hub-enforced per session (NACK `RATE_LIMITED`); Appendix G default 50 intents/s — generous for UIs, hostile to accidental loops.
- **Streams are not intents:** high-rate motion *input* rides STREAM channels and is never ECHOed per-sample (at 333 Hz that would be an ACK storm). Its observable truth is the position STATE/STREAM the hub publishes — you see what the machine actually did, which is the only truth that matters. Only discrete state changes ride INTENT.

### 9.4 EVENT — edges, not levels

EVENT channels carry discrete occurrences (anomaly detected, session joined, takeover happened). CBOR: `event_kind` (33), `timestamp` (21), kind-specific fields per schema, optional `seq_of_state` (34).

- **Best-effort:** events are conflated/bounded like everything else and are NOT replayed on reconnect. Therefore:
- **The event/state duality rule (safety-critical):** any event a client could not afford to have missed MUST have a **latched STATE twin** — the event says "this just happened", the state says "this is (still) true". E-stop is the canonical pair: EVENT for the edge, `safety` channel 0x0003 for the latch. A reconnecting client adopts the latch and needs no history. Conformance: no safety behavior may depend on EVENT delivery; events are UX garnish (toasts, logs), states are truth.
- **Overflow:** per-subscriber event queues are bounded (Appendix G); overflow drops oldest and sets an `events_dropped` counter in the hub-status channel — visible, never silent.

---

## 10. QoS, Flow Control, Congestion *(normative)*

### 10.1 Priorities and the never-shed set

Subscriptions carry a priority class (registry `priority_classes`): `background(0)` sheds first, then `normal(1)`, then `elevated(2)`. Class `critical(3)` is the **never-shed set**: INTENT, ECHO, ESTOP, NACK, GRANT, and any STATE channel the catalog marks critical (minimum: `safety` 0x0003, `control-owner` 0x0004). Never-shed traffic is tiny by design; §10.4 defines what happens when even that can't drain.

### 10.2 The grant model

A grant is `{channel_id, granted_rate_hz (14), priority}` — the hub's applied answer, communicated in WELCOME (batch) or GRANT frames. Rules:

- The hub MUST echo **granted** values; it MUST NOT silently deliver less than it granted for longer than a congestion transient (that's what re-granting is for). Wishes are clamped by: catalog `max_rate_hz`, per-session role, hub capacity, link estimate.
- **Unsolicited GRANT** (same frame, hub-initiated) re-states current grants whenever the hub changes them: a new high-priority client joined and the pie re-split; the probe (§6.4) justified a raise; sustained congestion forced a cut. Clients MUST comply immediately and SHOULD reflect grant changes in UI (a scope view showing 60 Hz when granted 20 is lying — §1.2-1 applies to meta-state too).
- Grant changes never apply to the never-shed set (its "rate" is intrinsic).

### 10.3 Congestion signals are per-binding

The hub detects congestion with the signal native to each binding (declared in the §13.1 matrix): TCP-backed bindings (WS, serial-over-USB) use **per-client egress queue watermarks**; ESP-NOW uses **ACK-bitmask loss rate** (§13.3); BLE uses notification-queue depth. Thresholds: sustained > 50 % watermark or > 10 % loss over 1 s ⇒ congested; < 20 % / < 2 % for 5 s ⇒ recovered. On congestion: shed per §10.4, and if sustained > 5 s, re-grant downward (§10.2) so the truth matches the throughput.

### 10.4 The shedding algorithm

Per subscriber, in order, until the link drains:

1. **Decimate STREAM** subscriptions, lowest priority first, halving effective sample rate per step (newest-biased per §9.2).
2. **Conflate STATE** harder (depth-1 queues already conflate; under congestion, periodic pushes stretch toward on-change-only), lowest priority first.
3. **Bound EVENT** queues drop-oldest (with the visible counter, §9.4).
4. **Slow-consumer eviction:** if the *never-shed* queue itself cannot drain for > 2 s, the subscriber is broken; the hub sends GOODBYE `SESSION_EVICTED` and closes. One incurable client MUST NOT be allowed to consume hub RAM or airtime indefinitely.

ESTOP is exempt from even step 4's queue: it is written ahead of every queue at the binding layer (§11.2) and is 12 bytes — a link that cannot carry 12 bytes is a dead link, and eviction of a dead link is not a safety event because the latch (§11.2) does not depend on any one subscriber observing it.

### 10.5 Ingress rate limiting

The hub bounds client→hub traffic: intents per §9.3 (default 50/s), STREAM input per its grant (`publishes` wish → granted rate; sustained overage ⇒ NACK `RATE_LIMITED`, persistent overage ⇒ eviction). A misbehaving client cannot starve Core-1 by flooding Core-0.

### 10.6 Broadcast media

On broadcast bindings (ESP-NOW), one transmission serves all peers; per-subscriber rate limiting is physically meaningless downstream of the radio. Rule: the effective channel rate on a broadcast segment is the **highest grant among its subscribers**; per-subscriber grants remain meaningful hub-side (they still drive what the hub *offers* the segment) and on unicast bindings. Relays MAY further decimate per §14.1.

---

## 11. Safety *(normative)*

### 11.1 The stop taxonomy

Four distinct levels, all latched or gated in the `safety` STATE channel (0x0003), all initiable via the `safety-intents` channel (0x0005):

| Level | Meaning | Motion behavior | Clears by |
|---|---|---|---|
| **ESTOP** | Emergency stop, latched | Immediate driver-level stop; motion prohibited while latched | Explicit authorized clear (§11.2) |
| **STOP** | Controlled stop | Decelerate to zero at configured decel; source deactivated | Any new motion intent from an authorized source |
| **HOLD** | Position hold | Decelerate, then actively hold position; source suspended | RESUME intent by the owning session |
| **PAUSE** | Pattern pause | Pattern generator suspends at a safe phase; position parked | RESUME intent |

The `safety` snapshot carries: active level bits, `cause` (user / deadman / fault / relay), initiating `origin` level, owning `session_id` where applicable, and `estop_seq`. Mapping to the existing firmware fields (`estop_latched`, `paused`, arbiter halt) is the hub implementation's job; the wire contract is this table.

### 11.2 ESTOP end-to-end

- **Initiation:** any endpoint, any role, any session state — including *no* session (a paired relay may originate). The frame is §5.5; safety outranks authorization by design (you may always stop the machine; you may not always start it).
- **Latch is the acknowledgement.** The initiator MUST repeat the ESTOP frame every `estop_repeat_interval_ms` (50 ms, up to `estop_repeat_max` = 20×) until it observes `safety` STATE with the ESTOP bit latched and `estop_seq` ≥ its sent seq — or exhausts retries and surfaces a loud local failure. There is no ESTOP-ACK frame; the observable latch is the only acknowledgement that means anything.
- **Hub obligations:** on first valid ESTOP (CRC-checked), stop motion via the arbiter's e-stop path *before* any protocol bookkeeping; latch; publish `safety` STATE at critical priority to all subscribers; emit the EVENT twin.
- **Relay obligation:** forward ESTOP ahead of all buffered traffic, immediately, on all attached segments (§14.2) — including *upstream* if relay-originated.
- **Preemption scope (honesty clause):** "jumps the queue" is a per-hop guarantee — each hop's transmit queue admits ESTOP at the front. It is not magic end-to-end latency: TCP in-flight bytes ahead of it still drain first. Worst-case added latency per binding is declared in the §13.1 matrix; system-level worst case (WS with one full 242-byte frame in flight + relay hop) is informative Appendix G material, and the *hardware* e-stop path remains the guarantee of last resort — SlopSync's ESTOP is a software convenience layered above it, never a substitute.
- **Clearing:** ESTOP_CLEAR intent on channel 0x0005; requires `controller`+ role; the hub MUST refuse (`CLEAR_REFUSED`) unless (a) the latched cause is resolved (deadman: the lost source is confirmed detached or re-owned; fault: the fault flag is gone), (b) motion is at zero velocity, and (c) no other stop level is pending escalation. Clearing never restarts motion — it only re-arms the ability to start.

### 11.3 Deadman

The deadman binds to the **active MotionArbiter source**, not to sessions in general:

- Every session that *owns an active source* (§11.4) has a deadman window (`deadman_ms`, default 600, clamp 250–5000, negotiated at WELCOME). Silence (no frame — §6.5) beyond the window fires the source's **loss policy**.
- **Initiator-bound sources** (STREAMING/TCode, manual jog, OSSM-remote live control): loss policy default **STOP** (decel). The machine must not continue executing a stream whose author is gone.
- **Hub-autonomous sources** (PATTERN): the generator runs *on the hub*; the vanished client was merely the finger that pressed start. Default policy: **continue**, ownership released (any controller may now stop/adjust/take over). Configurable to STOP per hub setting for the cautious. This is a deliberate product decision: a phone screen-lock must not interrupt a self-driving session, while a vanished *streamer* must stop motion in under a second.
- Deadman firing latches STOP (not ESTOP) in `safety` with cause=deadman, and releases source ownership (§11.4). Legacy TCode edges get synthetic sessions with equivalent timeouts (§15.1) — there is no unmonitored path to motion.

### 11.4 Control arbitration

The MotionArbiter's source priorities (MANUAL / TCODE / PATTERN / OSSM) arbitrate *between source types*. SlopSync adds the layer the arbiter cannot provide — arbitration *within* a type:

- **Exclusive ownership:** each arbiter source has at most one owning session at a time, tracked in `control-owner` STATE (0x0004). The first authorized session to activate a source owns it; a second session's activating intent gets NACK `SOURCE_CONFLICT`.
- **TAKEOVER:** re-issuing the activating intent with `takeover: true` (32) transfers ownership if the requester's role ≥ owner's role. The hub emits a takeover EVENT + `control-owner` STATE update; the dispossessed session's UI MUST reflect loss of control immediately (it's subscribed to 0x0004 like everyone else). Takeover between *types* remains the arbiter's existing priority logic, unchanged.
- **Release:** ownership releases on GOODBYE, eviction, deadman fire, or an explicit release intent. Post-deadman reacquisition requires a fresh activating intent (§6.7) — never silent resume.
- **Grants gate the door:** activating any source requires `controller` role (§12). Viewer sessions cannot own sources, full stop.

### 11.5 Invariants under partial failure

Whatever dies — a client, a relay, a transport, the WiFi — the following MUST hold: (1) motion driven by a vanished initiator-bound source stops within its deadman window; (2) the ESTOP latch, once set, survives every reconnect and is adopted by every arriving client before it can act (retained STATE + §2.2's LIVE gate); (3) a relay's death makes its clients *silent*, which triggers the same deadman path as client death — the hub cannot distinguish them and doesn't need to; (4) no failure mode results in a client displaying motion as stopped while the machine moves, because displays render only adopted hub state and go visibly stale (SYNCING) when the link dies.

---

## 12. Security *(normative except §12.4)*

### 12.1 Threat model

On this product category, **unauthorized control is a physical-safety issue**, and privacy of presence/telemetry is a real secondary concern. In scope: an untrusted device on the same LAN/radio range attempting control; a well-meaning but wrong client (stale app) issuing bad intents; accidental cross-machine control (two hubs in range). Out of scope for v1: a hostile actor with LAN packet capture and active MITM tooling (see §12.4), physical access, and DoS (a LAN attacker can jam WiFi regardless of anything this spec says).

### 12.2 v1 baseline: open viewing, paired control

- **Viewer is open.** Any client may connect, browse the catalog, and subscribe to channels marked `access: viewer`. Watching requires no ceremony. Hubs MAY offer a lock-down setting (viewer also requires token) for shared-space deployments.
- **Controller/admin require a pairing token.** The ceremony: user puts the hub in **pairing mode** (WebUI button / physical control), which opens a `pairing_window_default_s` (120 s) window and displays a `pairing_pin_digits` (4) PIN on a trusted surface (WebUI over the existing session, OLED). The new device sends PAIR_REQ containing `pin_proof` (28) = HMAC-SHA256(key = PIN as ASCII, message = the 8-byte `nonce` from its WELCOME), truncated to 16 bytes. Correct proof within the window ⇒ PAIR_GRANT carrying a random 16-byte `token` bound to the client's `instance_id`, persisted on both ends. Wrong proof or closed window ⇒ NACK `PAIRING_DENIED`; three failures close the window.
- **Token use:** presented in every HELLO (key 5); hub validates against its store (instance_id ↔ token ↔ role) and sets `roles` in WELCOME. Control ops without the role: NACK `NOT_CONTROLLER` / `ACCESS_DENIED`. Admin role is granted only via the hub's own UI (promote a paired device), never self-asserted.
- **Revocation:** the hub's UI lists paired devices (instance_id + client_name + last seen) and revokes individually; revoked tokens NACK `UNAUTHORIZED` at next HELLO. Tokens survive hub reboots (NVS-persisted) and firmware updates.
- The PIN proof never transmits the PIN; the nonce binds the proof to this session (no replay across sessions). This is deliberately *not* claimed to resist an active LAN MITM (§12.1) — it robustly prevents casual/accidental control and drive-by pairing, which is the v1 bar.

### 12.3 Per-transport mapping

- **WS:** plain `ws://` on LAN by default. Hubs MAY offer `wss://` (self-signed) — informatively: browser trust UX for self-signed LAN certs is hostile; this is why TLS is optional, not baseline.
- **BLE:** transports SHOULD use LE Secure Connections pairing/bonding where the client stack allows; SlopSync's token layer applies identically above it.
- **ESP-NOW:** the pairing ceremony doubles as key distribution — PAIR_GRANT MAY carry segment keys (PMK/LMK) enabling ESP-NOW's native encryption; relays store them like clients store tokens. Unencrypted ESP-NOW remains permitted for viewer-class traffic.
- **Serial/in-process:** physically-attached transports are implicitly `controller`-capable (possession of the cable/process is the credential); hubs MAY still require pairing on serial.

### 12.4 Future work *(informative)*

Hooks already in the wire format for v2+: `token` is a bstr (room for signed/expiring tokens), PAIR_* is extensible CBOR (room for SPAKE2-style PAKE replacing HMAC-PIN, per-session channel encryption keys), NACK auth range has space. Nothing in v2 security should require a wire-grammar break.

---

## 13. Transport Bindings *(normative)*

### 13.1 The binding contract

A binding implements four operations — `open`, `close`, `write(frame)`, `read → frame` — and declares its properties. SlopSync above the binding line is transport-blind. The matrix every implementation codes against:

| Binding | Payload MTU | Ordered | Reliable | Congestion signal | ESTOP preempt point | Worst-case ESTOP delay* |
|---|---|---|---|---|---|---|
| WebSocket | 65535 (practical) | yes | yes (TCP) | egress queue watermark | front of egress queue | in-flight TCP bytes |
| ESP-NOW | **242** | **no** | **no** | ACK-bitmask loss % | front of radio queue | one airtime slot (~1 ms) |
| BLE GATT | negotiated ATT−3 (23–512) | notifications: yes | no (notify) / yes (write-rsp) | notify queue depth | front of notify queue | one connection interval |
| Serial (COBS) | 242 (virtual) | yes | yes† | TX buffer watermark | byte-level injection | one frame length |
| In-process | unbounded (default 242) | configurable | configurable | simulated | simulated | simulated |

\* added by the binding, beyond queue-front admission — see §11.2's honesty clause. † USB CDC; raw UART is reliable-in-practice, CRC-carrying frames (ESTOP) self-protect, and STATE/STREAM classes tolerate loss by design.

The 242-byte ESP-NOW line is the **normative floor**: every mandatory control message and every STATE payload MUST fit it (§9.1); anything relying on more is a per-binding luxury.

### 13.2 WebSocket

Subprotocol **`slopsync.v1`** in the upgrade handshake (this is version negotiation for free, and lets the legacy port-81 protocol coexist on a different path/subprotocol during migration). One SlopSync frame = one WS **binary** message; no batching at the WS layer (bundles already amortize). Text messages on a `slopsync.v1` socket are a protocol error (close 1002). Server = hub. Recommended endpoint: `/slopsync` on the primary HTTP port.

### 13.3 ESP-NOW

Datagram binding, 250-byte ESP-NOW payload − 8-byte header = 242. Unicast per-peer where peers are few; broadcast segments follow §10.6. Reliability layer (inherited from the proven dongle ancestor, now normative): every data frame carries its header seq; receivers emit a batched **ACKMASK** frame (type 0x16, raw, channel 0) every 10 ms — payload `base_seq:u16, mask:u32` — acking seqs `base..base+31`. Senders use loss rate as the §10.3 congestion signal; there is **no retransmission** of STATE/STREAM (the classes don't need it); control-plane frames (INTENT et al.) on ESP-NOW use stop-and-wait retransmit (3×, 100 ms) keyed on the ACK mask. Discovery/pairing broadcast: §13.7.

### 13.4 BLE GATT

NUS-shaped service (one write characteristic c→h, one notify characteristic h→c), SlopSync frames as characteristic values; frames ≤ (ATT_MTU − 3). Clients SHOULD negotiate MTU ≥ 250 where the stack allows; below that, the binding declares its real MTU and the hub's STATE-fit rule still holds (242 governs catalog design; a 100-byte BLE link simply fragments *control* frames per §5.6 — data frames are sized to the declared MTU at grant time by bundling less). Static-profile clients (§8.5) are the expected BLE norm.

### 13.5 Serial

Byte pipe → **COBS** framing, delimiter 0x00: encode each SlopSync frame with COBS, append 0x00. Virtual MTU 242 pre-encoding (keeps one shared catalog-fit rule). ESTOP scanning: the §5.5 magic is matched on the **decoded** stream; additionally, because COBS never produces 0x00 inside a frame and re-synchronizes at every delimiter, a receiver in unsynced/corrupt state MUST still run the 4×0xE5 scanner on raw bytes between delimiters (0xE5 survives COBS encoding unchanged when no zero bytes occur in the window — the CRC validates any candidate either way).

### 13.6 In-process (the sim binding)

The in-process binding connects `slopsync-core` hub and client roles inside one process (desktop simulator, unit tests). It is a **first-class conformance instrument**, and therefore MUST support: configurable MTU (down to 242 and below), injected loss/reorder/duplication rates, injected latency and jitter, and a **deterministic mode** (seeded fault schedule + injected clock) in which a test run is bit-reproducible. The golden behavioral tests (§17) run against this binding; an implementation without fault injection cannot claim conformance testing.

### 13.7 Discovery

- **mDNS/DNS-SD (WS clients):** service `_slopsync._tcp`, TXT records: `v=1`, `name=<hub name>`, `etag=<hex>`, `pairing=<open|closed>`. Browsers can't mDNS-browse; the WebUI is served *by the hub* so it connects to its origin — mDNS serves native apps and sims.
- **BLE:** advertise the SlopSync service UUID with hub name; `pairing` flag in adv data while the window is open.
- **ESP-NOW:** hub (or its relay) broadcasts a **BEACON** frame (type 0x17, raw, channel 0; payload: hub boot_id, catalog etag, pairing-open flag) every 500 ms **only while the pairing window is open**; new peers respond to beacons, then run PAIR_REQ over unicast. Outside the window, ESP-NOW peers must already know the segment (persisted from pairing).

---

## 14. Relay Role *(normative)*

### 14.1 Forwarding

A relay bridges the hub's reachable transports to segments it cannot reach (canonical instance: the C5 node bridging hub-UART ↔ ESP-NOW remotes). Rules:

- A relay forwards **frames**, not sessions: it does not parse control-plane CBOR, does not hold grants, and is invisible to the session layer except as specified here. Clients behind a relay hold ordinary sessions with the hub.
- **Priority-aware buffering:** a relay MUST maintain at least two queues per direction — critical (never-shed set + ESTOP fast path) and everything else — and MUST apply §10.4-style shedding (decimate STREAM first, conflate STATE by replacing queued frames for the same channel with newer ones) when its downstream is slower than its upstream. A relay that blindly FIFOs is non-conformant: it converts congestion into latency, which for motion data is the worst outcome (§9.2).
- A relay MAY further decimate STREAM traffic below granted rates when its segment demands it; the hub's congestion machinery observes the resulting ACK loss and re-grants honestly (§10.3) — the system converges without the relay speaking the grant protocol.

### 14.2 ACK aggregation and the ESTOP fast path

- Reliability is **hop-by-hop**: the relay ACKs (per §13.3) what it receives from its segment and takes responsibility for upstream delivery, and vice versa. There are no end-to-end transport ACKs across a relay; consequence, stated plainly: the hub knowing a frame reached the relay does NOT mean the client got it. This is safe because no protocol correctness depends on transport delivery — STATE re-pushes, STREAM tolerates loss, and the only end-to-end confirmations that exist are protocol-level: **INTENT ⇒ ECHO** (hub-originated) and **ESTOP ⇒ observed latch** (§11.2).
- **ESTOP fast path:** on matching the 4×0xE5 magic (raw scanner, §5.5 — no deframing, no queueing), a relay MUST transmit the frame onward on **all** attached segments ahead of every queued frame, then resume normal operation. CRC validation MAY be deferred to endpoints when the relay's budget is tight; forwarding a corrupt candidate costs 12 bytes, dropping a real one costs much more.

### 14.3 Timestamp correction and limits

A relay that buffers (adds > 1 ms asymmetric delay) MUST either (a) correct: stamp arrival, and on transmit rewrite STREAM `t_base` by its holding time — the generalization of the ESP-NOW ancestor's `rel_ms` replay scheduling — or (b) be CLOCK-transparent by forwarding CLOCK frames with strict priority (delay < 1 ms), or (c) drop CLOCK frames entirely (§7.1), degrading its clients to WELCOME-bootstrap accuracy. Exactly one of a/b/c MUST hold; silent uncorrected buffering of CLOCK is non-conformant. Relays MUST NOT chain (one relay hop maximum, v1) — multi-hop is a v2 problem nobody currently has.

---

## 15. Legacy Interop *(normative rules, informative mapping)*

### 15.1 TCode text edges as synthetic sessions *(normative)*

The legacy TCode ingest paths (USB serial, BLE-NUS, WS:55555 raw text, the outbound Intiface/WSDM client, the ESP-NOW dongle chain) remain supported. The hub MUST wrap each active legacy edge in a **synthetic session**: an internal session object with `client_kind: "tcode-bridge"`, controller-class capability scoped to the TCODE arbiter source only, ownership per §11.4, and a deadman equal to the edge's existing stream-quiet timeout (2 s today, hub-configurable within §11.3's clamp). Effect: legacy clients appear in the session roster (0x0002), their motion obeys the same deadman/ownership/safety rules as native sessions, and there is **no unmonitored path to motion**. They receive no SlopSync frames; the synthesis is entirely hub-side bookkeeping.

### 15.2 Port-81 → SlopSync migration *(informative)*

The legacy binary UI protocol is SlopSync's direct ancestor; every concept maps:

| Legacy (port 81) | SlopSync successor |
|---|---|
| HELLO `{proto_ver, cfg_gen}` (0x00) | HELLO/WELCOME (§6.2–6.3) — adds identity, roles, grants, etag, boot_id |
| TELE (0x01): 10-B header + n×6-B samples @45 fps | STREAM bundle (§5.4) on a `position` channel — same 6-B sample struct, now catalog-declared; flags bits → `safety` + status STATE channels |
| STATUS (0x02) @2 Hz | `hub-status` STATE (0x0006) + power/wifi STATE channels |
| CLOCK (0x03) t0/t1/t2 | CLOCK (§7.1) — byte-identical exchange, new frame type id |
| INTERP (0x04), STATS (0x06) | device-defined STATE channels (Appendix D) |
| ANOMALY (0x05) event ring | EVENT channel + latched anomaly-summary STATE (the §9.4 duality applied) |
| CMD (0x10) ops 0x01–0x14 + id + JSON | INTENT channels (§9.3) — ids become `intent_id`, JSON becomes schema'd CBOR, op codes become channel ids |
| ECHO (0x11) `{id, ok, cfg_gen, JSON}` | ECHO (§9.3) `{intent_id, applied, cfg_gen}` — same idempotency ring semantics, now spec'd |
| GET_CFG full-snapshot resync | Retained STATE push (§9.1) — the resync *is* the connect path now |
| `cfg_gen` threading | Unchanged in meaning; formalized in §4.2 |
| per-client 32-deep idempotency ring | Unchanged; normative in §9.3 |

Cutover plan: the hub serves both protocols during migration (different WS endpoints/subprotocols, §13.2); the WebUI moves to `slopsync-js`; port-81 is retired when nothing speaks it. No flag day.

### 15.3 Intiface/WSDM boundary *(informative)*

The outbound WSDM client (hub dials Intiface) is an *adapter the hub owns*, not a SlopSync client; it materializes as a synthetic session per §15.1. Exposing SlopSync to Intiface directly is out of scope.

---

## 16. Errors and Diagnostics *(normative)*

### 16.1 NACK

NACK (CBOR): `code` (16, from the registry's ranged taxonomy), optional `channel_id`, `intent_id`, `detail` (17, human-readable, never required for machine handling), `retry_after_ms` (31, with BUSY). Ranges: 0x00xx protocol, 0x01xx session/auth, 0x02xx subscription, 0x03xx intent, **0x04xx safety refusals** — UIs SHOULD render 0x04xx distinctly (a refusal because the machine is e-stopped is user-meaningful, not an "error"), 0x05xx transfer. Unknown code → treat as its range generic (§4.3). NACK never closes the session by itself; GOODBYE does.

### 16.2 Observability

The hub exposes its own health as ordinary channels (dogfooding the protocol): `hub-status` (0x0006) includes heap, uptime, per-binding client counts, `events_dropped`, sheds/evictions counters; `session-roster` (0x0002) lists sessions (id, kind, name, roles, transport, rtt estimate). Diagnostic verbosity beyond this is hub-implementation territory (the existing `/api/log` stays).

---

## 17. Conformance *(normative)*

### 17.1 Profiles

| Profile | MUST implement | MAY omit |
|---|---|---|
| **hub** | everything not explicitly optional; ≥ 4 concurrent sessions; all four channel classes; retained STATE; grants + shedding; §11 complete; pairing | probe; WSS; ESP-NOW binding (if the hardware lacks it) |
| **client-viewer** | HELLO/WELCOME, catalog (dynamic or static), STATE adoption, seq rules, SYNCING/LIVE distinction, ESTOP *send* | intents, streams, probe |
| **client-controller** | viewer + INTENT/ECHO with idempotent retry + absolute-value rule, pairing, deadman-aware liveness, §11.4 ownership behavior | probe |
| **constrained-client** | static profile (§8.5) incl. mismatch behavior, prefix parsing, canned-template correctness | dynamic catalog, CBOR general decode |
| **relay** | §14 complete: dual-queue forwarding, ESTOP fast path, timestamp rule (a, b, or c) | everything session-layer |

### 17.2 Golden vectors

Byte-exact test vectors live in [`vectors/`](vectors/) (manifest + generated bytes; see `vectors/manifest.yaml` for the generation plan). Determinism requirements this places on implementations — stated here because they constrain library API design *before* any library exists: `slopsync-core` MUST accept an **injected clock** and an **injected RNG** (session ids, boot ids, nonces, tokens); the deterministic CBOR profile (§5.3) does the rest. A vector is: fixed inputs → exact expected bytes (encode direction) and exact expected decoded model + actions (decode direction). Implementations MUST pass every vector for their profile.

### 17.3 Behavioral checklists

Beyond byte vectors, per-profile behavioral tests run against the in-process binding (§13.6) with fault injection: the reconnect-reconcile flow (§6.7), newest-wins under reorder (§7.3), retained-push-then-LIVE gating (§2.2), duplicate-intent re-echo (§9.3), shed-order correctness (§10.4), deadman policies per source type (§11.3), takeover flows (§11.4), ESTOP repeat-until-latch under 30 % loss (§11.2), static-profile degraded mode (§8.5). The five annotated traces in Appendix E double as the narrative form of this checklist; `examples/session-traces.md` is their source.

---

# Appendices

## Appendix A — Frame type table *(normative, generated view of `registry.yaml`)*

| Type | Name | Dir | Plane | Defined in |
|---|---|---|---|---|
| 0x00 | HELLO | c→h | control | §6.2 |
| 0x01 | WELCOME | h→c | control | §6.3 |
| 0x03 | PING | any | raw | §6.5 |
| 0x04 | PONG | any | raw | §6.5 |
| 0x05 | CLOCK | any | raw | §7.1 |
| 0x06 | SUBSCRIBE | c→h | control | §6.6 |
| 0x07 | UNSUBSCRIBE | c→h | control | §6.6 |
| 0x08 | GRANT | h→c | control | §10.2 |
| 0x09 | CATALOG_REQ | c→h | control | §8.4 |
| 0x0A | CATALOG_CHUNK | h→c | raw | §8.4 |
| 0x0B | STATE | h→c | data | §9.1 |
| 0x0C | STREAM | any | data | §9.2 |
| 0x0D | INTENT | c→h | control | §9.3 |
| 0x0E | ECHO | h→c | control | §9.3 |
| 0x0F | EVENT | h→c | control | §9.4 |
| 0x10 | NACK | h→c | control | §16.1 |
| 0x11 | GOODBYE | any | control | §6.8 |
| 0x12 | PROBE | any | raw | §6.4 |
| 0x13 | PROBE_REPORT | c→h | control | §6.4 |
| 0x14 | PAIR_REQ | c→h | control | §12.2 |
| 0x15 | PAIR_GRANT | h→c | control | §12.2 |
| 0x16 | ACKMASK | any | raw | §13.3 |
| 0x17 | BEACON | h→c | raw | §13.7 |
| 0xE5 | ESTOP | any | raw | §5.5, §11.2 |

Reserved: 0x02, 0x18–0x3F spec/core; 0x40–0x7F future spec; 0x80–0xDF experimental; 0xE0–0xFF reserved except 0xE5.

## Appendix B — CBOR integer-key registry *(normative, generated view)*

Keys 1–34 as allocated in `registry.yaml` `cbor_keys` (proto_ver 1, client_kind 2, client_name 3, instance_id 4, token 5, session_id 6, boot_id 7, catalog_etag 8, cfg_gen 9, subscriptions 10, publishes 11, rate_hz 12, priority 13, granted_rate_hz 14, channel_id 15, code 16, detail 17, intent_id 18, applied 19, value 20, timestamp 21, limits 22, roles 23, deadman_ms 24, deadman_policy 25, probe_result 26, chunks 27, pin_proof 28, nonce 29, precondition 30, retry_after_ms 31, takeover 32, event_kind 33, seq_of_state 34). Ranges: 1–63 core, 64–127 reserved, 128+ experimental. A key means the same thing in every message.

## Appendix C — Catalog schema *(normative)*

The catalog's CDDL definition lives in [`schema/catalog.cddl`](schema/catalog.cddl). It is the normative encoding of §8.1; the etag (§8.3) is computed over a catalog valid against it.

## Appendix D — Initial SlopDrive channel catalog *(informative)*

Device-defined channels (0x0080+) seeded from the current firmware's `SystemState`. This is the starting map, not a commitment — the shipped hub's catalog is self-describing and authoritative.

| Id | Name | Class | Dir | Access | Notes (source fields) |
|---|---|---|---|---|---|
| 0x0080 | position | STREAM | h→c | viewer | 6-B sample `{pos_10um:u16, tgt_10um:u16, raw_10um:u16}` @ ≤240 Hz (legacy TELE samples) |
| 0x0081 | motion-input | STREAM | c→h | controller | `{target_norm_1e4:u16}` timestamped; the SlopSync-native TCode successor |
| 0x0082 | motion-status | STATE | h→c | viewer | bitfields: homed, homing, gen_running/emitting, paused, override, stream-active (legacy TELE flags) |
| 0x0083 | motion-config | STATE | h→c | viewer | window min/max, speed, accel, blend, auto_duration, stream_speed_mode, overshoot clamp (cfg_gen-coupled) |
| 0x0084 | motion-config-set | INTENT | c→h | controller | absolute setters for 0x0083's fields |
| 0x0085 | pattern-config | STATE | h→c | viewer | pattern idx, params, advanced-mode model (GEN_CFG successor) |
| 0x0086 | pattern-control | INTENT | c→h | controller | select/configure/run/stop (activates PATTERN source, §11.4) |
| 0x0087 | move | INTENT | c→h | controller | manual point move `{position_mm, speed?}` (activates MANUAL source) |
| 0x0088 | homing | INTENT | c→h | controller | home / bench home-override |
| 0x0089 | interp-status | STATE | h→c | viewer | style, start/end/cur pos, cur vel, durations (legacy INTERP) |
| 0x008A | anomalies | EVENT | h→c | viewer | kind, seq, positions, slope (legacy ANOMALY); STATE twin: 0x008B |
| 0x008B | anomaly-summary | STATE | h→c | viewer | latched per-kind counters + last-anomaly snapshot (§9.4 duality) |
| 0x008C | odometer | STATE | h→c | viewer | live/max speed, distance, strokes, energy, session time (legacy STATS) |
| 0x008D | power | STATE | h→c | viewer | bus mV, current mA, peak mA, die °C×10 (INA228) |
| 0x008E | wifi-status | STATE | h→c | viewer | rssi, channel, reconnects, ip (legacy STATUS subset) |
| 0x008F | transport-mode | STATE | h→c | viewer | active legacy-ingest mode (WS/SER/BT/DONGLE/OSSM) |
| 0x0090 | transport-mode-set | INTENT | c→h | admin | legacy-ingest mode select |

Plus the spec-core channels 0x0001–0x0007 (§ registry `core_channels`). All STATE layouts above fit 242 bytes by inspection; conformance tooling re-checks mechanically (§9.1).

## Appendix E — Worked traces *(informative)*

Annotated end-to-end session traces live in [`examples/session-traces.md`](examples/session-traces.md): **E1** cold connect (browser, dynamic catalog); **E2** reconnect mid-motion (etag skip, reconcile, no silent control resume); **E3** controller takeover (two remotes, one machine); **E4** ESTOP over a lossy relay (repeat-until-latch, fast path); **E5** constrained C5 client joins (static profile, etag mismatch degraded mode). Per §17.3 these are executable narratives: every step cites the normative rule it exercises, and a step with no rule to cite is a spec bug.

## Appendix F — Golden vector index *(normative)*

The vector manifest and generation plan live in [`vectors/manifest.yaml`](vectors/manifest.yaml). Byte-exact vector files are generated by `slopsync-core` tooling (injected clock/RNG per §17.2) and land beside the manifest. The manifest is normative as to *what* is covered; the generated bytes are normative once tagged.

## Appendix G — Limits and defaults *(normative, generated view of `registry.yaml` `limits`)*

| Limit | Value | Where used |
|---|---|---|
| Frame header | 8 B | §5.1 |
| Minimum transport payload (STATE-fit floor) | 242 B | §9.1, §13.1 |
| Catalog chunk payload | 192 B | §8.4 |
| Bundle max samples / max span | 32 / 20 ms | §5.4 |
| Seq width / newer-window | u16 / 32768 | §7.3 |
| Frag reassembly timeout / concurrent | 5 s / 2 per session | §5.6 |
| Idempotency ring depth | 32 | §9.3 |
| Event queue depth per subscriber | 16 | §9.4 |
| Never-shed stall → eviction | 2 s | §10.4 |
| Catalog chunk gap re-request (SHOULD) | 500 ms | §8.4 |
| PING interval (holding control / idle) | 200 ms / 1 s | §6.5 |
| Deadman default / clamp | 600 ms / 250–5000 ms | §11.3 |
| Intent ingress default | 50 /s | §9.3, §10.5 |
| Pairing window / PIN digits / token | 120 s / 4 / 16 B | §12.2 |
| instance_id / etag size | 8 B / 8 B | §6.1, §8.3 |
| ESTOP repeat interval / max | 50 ms / 20 | §11.2 |
| Clock resync interval | 10 s | §7.1 |
| Probe size / max duration | 8192 B / 1.5 s | §6.4 |
| Catalog max entries / max subs per session | 256 / 64 | §8, §6.6 |
| Conformance min concurrent clients | 4 | §6.3, §17.1 |
| Default client limits (WS/ESP-NOW/BLE/serial) | 8/4/1/1 | §6.3 |
| WS subprotocol / mDNS service | `slopsync.v1` / `_slopsync._tcp` | §13.2, §13.7 |

## Appendix H — Design rationale and rejected alternatives *(informative)*

- **Why not PicoMQTT (on-device MQTT broker):** broker ignores retained messages and wills, QoS 0 only — the device-shadow primitive would be rebuilt app-side anyway; transports limited to Arduino `Client` (TCP-shaped); LGPLv3. The glue equalled this spec's hard parts with none of its fit.
- **Why not zenoh-pico:** runs on ESP32 but peer-unicast nodes do not route (no hub role on-device); no ESP-NOW or browser-server transport; custom-transport API unresolved upstream.
- **Why not MQTT-SN:** the gateway/broker side has no MCU implementation; Paho's gateway is a Linux program that itself needs an upstream broker.
- **Why not Micro XRCE-DDS:** the Agent (hub role) is Linux/Windows-only. Its transport abstraction was adopted (§13.1); the stack was not.
- **Why not ThingSet wholesale:** best conceptual match, but the node library is Zephyr-bound, ESP-NOW is unlisted, and subscription-rate negotiation doesn't exist. Its self-describing data model was adopted in spirit (§8).
- **Why not esp-matter:** ~1.5 MB flash + ~195 KB RAM before application logic; cluster model aimed at smart-home semantics; commissioning UX wrong for this product.
- **Why not SSE for telemetry:** §1.1.
- **Why not deltas on STATE:** loss-corruption; §9.1.
- **Why not per-sample ACKs on motion input:** ACK storm at 333 Hz; observable applied position is the meaningful confirmation; §9.3.
- **Why hybrid CBOR + packed structs:** exactly-one-encoding CBOR gives byte-exact vectors and canned templates for MCU clients; packed structs give a zero-cost hot path at 240–333 Hz. Pure CBOR taxes the hot path; pure protobuf taxes every client with a codegen toolchain and varint decode. Decision confirmed by project owner 2026-07-22.
- **Why PIN-pairing baseline (not open, not TLS):** open LAN made capability grants fiction on a product where unauthorized control is a safety issue; TLS-everywhere costs ~40 KB+ RAM/session and hostile self-signed-cert UX. HMAC-PIN pairing is the cheapest mechanism that makes "controller" mean something. Confirmed 2026-07-22.
- **Why pattern-continues deadman default:** the pattern generator is hub-autonomous; a phone screen-lock must not interrupt a session, while vanished *streamers* still stop motion in ≤ 600 ms. Confirmed 2026-07-22.

## Appendix I — Design-review gap closure map *(informative, audit artifact)*

Findings from the pre-spec adversarial design review, mapped to their resolving sections:

| Finding | Resolution |
|---|---|
| G1 idempotency vs reconnect | §9.3 session-scoped ids + §6.7 reconcile-don't-retransmit + absolute-values rule |
| G2 stable client identity | §6.1 `instance_id` |
| G3 grant-reacquisition race | §6.7 + §11.4 (no silent control resume post-deadman) |
| G4 mid-session subscriptions | §6.6 SUBSCRIBE/UNSUBSCRIBE |
| G5 snapshot vs delta vs MTU | §9.1 full-snapshot + 242-B fit rule (no deltas) |
| G6 STATE ordering on unordered transports | §7.3 per-channel seq, newest-wins |
| G7 retained-value rule | §9.1 retained push; §6.3 |
| G8 event/state duality | §9.4 duality rule |
| V1 three version tokens | §4.2 scoped, not unified |
| V2 packed-struct evolution vs pinned clients | §5.4 append-only + prefix parsing; §8.5 |
| V3 catalog client-invariance | §8.6 |
| Q1 re-grant signaling | §10.2 unsolicited GRANT |
| Q2 probe delays connect | §6.4 optional, post-WELCOME |
| Q3 congestion signal per binding | §10.3 + §13.1 matrix |
| Q4 broadcast vs per-subscriber rates | §10.6 highest-grant rule |
| Q5 never-shed overflow | §10.4 bounded queues + slow-consumer eviction |
| Q6 shed semantics per class | §10.4 decimate/conflate/drop-oldest |
| S1 ESTOP clear authorization | §11.2 clearing rules + `CLEAR_REFUSED` |
| S2 ESTOP over lossy links | §11.2 repeat-until-latched; §14.2 fast path |
| S3 preemption honesty | §11.2 per-hop scope + §13.1 delay column |
| S4 deadman scope | §11.3 source-bound, initiator vs autonomous |
| S5 hold-vs-stop taxonomy | §11.1 four levels |
| S6 same-source contention | §11.4 exclusive ownership + TAKEOVER |
| S7 grants vs open LAN | §12.2 pairing baseline (decision) |
| S8 legacy edges bypass deadman | §15.1 synthetic sessions |
| T1 relay ACK semantics | §14.2 hop-by-hop |
| T2 STREAM degradation matrix | §13.1 + §9.2 weakest-binding rule |
| T3 catalog transfer repair | §8.4 selective repair + timeout + fallback |
| T4 constrained clients and CBOR | §8.5 static profile + §5.3 canned templates |
| T5 serial framing + ESTOP scan | §13.5 COBS + scanner rule |
| T6 SSE | §1.1 non-goal |
| T7 WS binding details | §13.2 subprotocol + 1-frame-1-message |
| T8 clock through relays | §7.4 + §14.3 a/b/c rule |
| T9 sim binding teeth | §13.6 fault injection + deterministic mode |
| T10 pairing ceremonies per transport | §12.2–12.3 |
| T11 admission control | §6.3 BUSY + retry_after |
| X1 which classes ECHO | §9.3 (streams never ACKed) |
| X2 config write races | §9.3 `precondition` CAS |
| X3 liveness definition | §6.5 any-frame liveness |
| X4 vectors vs nondeterminism | §17.2 injected clock/RNG |
| X5 STREAM terminology collision | §2.1 stream-/datagram-oriented wording |

---

*End of SPEC.md (slopsync/1, v1-draft).*
