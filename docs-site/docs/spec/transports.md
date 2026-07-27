---
title: Transports and relays
description: >-
  SlopSync clauses 13-14: the binding contract and its matrix, the WebSocket,
  ESP-NOW, BLE, serial and in-process bindings, discovery, and the relay role.
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

# Transport bindings and the relay role

## 13. Transport Bindings *(normative)* {#s13}

### 13.1 The binding contract {#s13-1}

A binding implements four operations — `open`, `close`, `write(frame)`, `read → frame` — and declares its properties. SlopSync above the binding line is transport-blind. The matrix every implementation codes against:

| Binding | `max_frame` (header-incl.) | Payload MTU | Ordered | Reliable | Congestion signal | ESTOP preempt point | Worst-case added ESTOP delay* |
|---|---|---|---|---|---|---|---|
| WebSocket | 512 | 504 | yes | yes (TCP) | egress queue watermark | front of egress queue | in-flight TCP bytes |
| ESP-NOW | **250** | **242** | **no** | **no** | ACK-bitmask loss % | front of radio queue | one airtime slot (~1 ms) |
| BLE GATT | 244 (ATT_MTU 247 − 3) | 236 | notifications: yes | no (notify) / yes (write-rsp) | notify queue depth | front of notify queue | one connection interval |
| Serial (COBS) | 512 | 504 | yes | yes† | TX buffer watermark | byte-level injection | one frame length |
| In-process | configurable (default 250) | configurable | configurable | configurable | simulated | simulated | simulated |

\* added by the binding, beyond queue-front admission — see [§11.2](safety.md#s11-2)'s honesty clause H2. † USB CDC; raw UART is reliable in practice, CRC-carrying frames (ESTOP) self-protect, and the STATE/STREAM classes tolerate loss by design.

The ESP-NOW line is the **normative floor**: `min_transport_payload` = 242 comes directly from it, every mandatory control message and every STATE payload MUST fit it ([§9.1](channels.md#s9-1)), and anything relying on more is a per-binding luxury. A hub MAY advertise a smaller `max_frame` than its binding permits; it MUST NOT advertise a larger one.

### 13.2 WebSocket {#s13-2}

Subprotocol **`slopsync.v1`** in the upgrade handshake — this is version negotiation for free, and it lets a legacy protocol coexist on a different path or subprotocol during migration. One SlopSync frame = one WS **binary** message; no batching at the WS layer, since bundles already amortize. Text messages on a `slopsync.v1` socket are a protocol error (close 1002). The server is the hub. RECOMMENDED endpoint: `/slopsync` on the primary HTTP port.

### 13.3 ESP-NOW {#s13-3}

Datagram binding: 250-byte payload − 8-byte header = 242. Unicast per peer where peers are few; broadcast segments follow [§10.6](qos.md#s10-6).

Reliability layer: every data frame carries its header seq; receivers emit a batched **ACKMASK** frame (`0x16`, raw, channel 0) every 10 ms — payload `base_seq:u16, mask:u32` — acknowledging seqs `base..base+31`. Senders use the resulting loss rate as the [§10.3](qos.md#s10-3) congestion signal. There is **no retransmission of STATE or STREAM** (those classes do not need it); control-plane frames use stop-and-wait retransmit (3×, 100 ms) keyed on the ACK mask. Discovery and pairing broadcast: [§13.7](#s13-7).

### 13.4 BLE GATT {#s13-4}

A NUS-shaped service (one write characteristic c→h, one notify characteristic h→c) carrying SlopSync frames as characteristic values, each ≤ ATT_MTU − 3.

Clients SHOULD negotiate MTU ≥ 250 and enable data-length extension **before catalog transfer**; below that the binding declares its real MTU and the 242-byte STATE-fit rule still governs catalog *design*, while control frames fragment per [§5.6](wire-format.md#s5-6) and data frames are sized to the declared MTU at grant time by bundling less. A client stuck at the legacy 23-byte MTU cannot carry a full STATE frame at all and pays a long one-time catalog transfer (visibly SYNCING) or ships the [§8.5](catalog.md#s8-5) static profile. Static-profile clients are the expected BLE norm.

### 13.5 Serial {#s13-5}

Byte pipe → **COBS** framing, delimiter `0x00`: encode each SlopSync frame with COBS and append `0x00`.

ESTOP scanning: the [§5.5](wire-format.md#s5-5) magic is matched on the **decoded** stream; additionally, because COBS never produces `0x00` inside a frame and re-synchronizes at every delimiter, a receiver in an unsynced or corrupt state MUST still run the four-`0xE5` scanner on **raw** bytes between delimiters. `0xE5` survives COBS encoding unchanged when no zero bytes occur in the window, and the CRC validates any candidate either way.

### 13.6 In-process (the conformance binding) {#s13-6}

The in-process binding connects hub and client roles inside one process (desktop simulator, unit tests). It is a **first-class conformance instrument** and therefore MUST support: configurable MTU (down to 242 and below), injected loss/reorder/duplication rates, injected latency and jitter, and a **deterministic mode** (seeded fault schedule plus injected clock) in which a run is bit-reproducible. The behavioral tests of [§17.3](conformance.md#s17-3) run against it; an implementation without fault injection cannot claim conformance testing.

### 13.7 Discovery {#s13-7}

- **mDNS/DNS-SD (WS clients):** service `_slopsync._tcp`; TXT records `v=1`, `name=<hub name>`, `etag=<hex>`, `pairing=<open|closed>`. Browsers cannot mDNS-browse; a hub-served web UI connects to its own origin, so mDNS serves native applications and simulators. A manually-entered address MUST always work — discovery is a convenience, never a requirement.
- **BLE:** advertise the service UUID with the hub name; a pairing flag in advertising data while a window is open.
- **ESP-NOW:** the hub or its relay broadcasts a **BEACON** frame (`0x17`, raw, channel 0; payload: `boot_id`, catalog etag, pairing-open flag) every 500 ms **only while a pairing window is open**. New peers respond to beacons, then run PAIR_REQ over unicast. Outside the window, peers must already know the segment from a previous pairing.

**Discovery is an untrusted input.** A client that auto-connects to a discovered service is one malicious hub away from parsing hostile bytes; [§5.8-5](wire-format.md#s5-8) and [§12.5](security.md#s12-5) are what bound the consequences.

## 14. Relay Role *(normative)* {#s14}

### 14.1 Forwarding {#s14-1}

A relay bridges the hub's reachable transports to segments it cannot reach. Rules:

- A relay forwards **frames, not sessions**: it does not parse control-plane CBOR, does not hold grants, and is invisible to the session layer except as specified here. Clients behind a relay hold ordinary sessions with the hub.
- **Priority-aware buffering:** a relay MUST maintain at least two queues per direction — critical (the never-shed set plus the ESTOP fast path) and everything else — and MUST apply [§10.4](qos.md#s10-4)-style shedding when its downstream is slower than its upstream, **including the segment exception**: it decimates `samples`-kind streams and conflates STATE by replacing queued frames for the same channel with newer ones, but it MUST NOT decimate a `segments`-kind stream. A relay that blindly FIFOs is non-conformant: it converts congestion into latency, which for motion data is the worst outcome ([§9.2](channels.md#s9-2)).
- A relay MAY further decimate below granted rates when its segment demands it; the hub's congestion machinery observes the resulting loss and re-grants honestly ([§10.3](qos.md#s10-3)), so the system converges without the relay speaking the grant protocol.

### 14.2 ACK aggregation and the ESTOP fast path {#s14-2}

- **Reliability is hop-by-hop.** The relay acknowledges what it receives from its segment and takes responsibility for upstream delivery, and vice versa. There are no end-to-end transport acknowledgements across a relay.
  **HONESTY CLAUSE (H10), stated plainly:** the hub knowing a frame reached the relay does **not** mean the client got it. This is safe because no protocol correctness depends on transport delivery — STATE re-pushes, STREAM tolerates loss, and the only end-to-end confirmations that exist are protocol-level: **INTENT ⇒ ECHO** and **ESTOP ⇒ observed latch**.
- **ESTOP fast path:** on matching the four-`0xE5` magic with a raw scanner — no deframing, no queueing — a relay MUST transmit the frame onward on **all** attached segments ahead of every queued frame, then resume normal operation. CRC validation MAY be deferred to endpoints when the relay's budget is tight: forwarding a corrupt candidate costs 12 bytes; dropping a real one costs much more.

### 14.3 Timestamp correction and limits {#s14-3}

A relay that buffers — adds more than 1 ms of asymmetric delay — MUST satisfy **exactly one** of:

**(a) correct** — stamp arrival and, on transmit, rewrite STREAM `t_base` by its holding time;
**(b) be CLOCK-transparent** — forward CLOCK frames with strict priority, under 1 ms of added delay;
**(c) drop CLOCK frames entirely** ([§7.1](time.md#s7-1)), degrading its clients to WELCOME-bootstrap accuracy.

Silent uncorrected buffering of CLOCK is non-conformant. Note that (a) matters doubly for `segments`-kind streams, where `t_base` is a **schedule**, not an observation ([§5.4](wire-format.md#s5-4)): an uncorrected relay does not merely blur a graph, it moves commands in time.

**Relays MUST NOT chain.** One relay hop maximum in v1; multi-hop is a v2 problem nobody currently has.
