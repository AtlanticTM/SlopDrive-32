# Proposal — Move SlopSync to the ESP32-C5, dedicate the S3 to motion

Status: **DRAFT — not ruled on.** Two feasibility gates unresolved (§4).
Raised: 2026-08-01. Supersedes nothing. Requires a CANON §3 ruling before
implementation.

## 1. Problem

Every motion defect chased during the 2026-07-31/08-01 reversal investigation
traced to the same root: **interrupt latency on an MCU that is also running
WiFi, BLE, a WebSocket hub and a 1 kHz motion sampler.**

* The MCPWM pulse backend was unusable under funscript playback. It programs one
  command at a time and cannot start the next until its completion IRQ; every
  late ISR is dead air on the step pin. Uniform patterns were clean, scripted
  playback stuttered badly — the difference is WS traffic on Core 0.
* The RMT backend survives that latency only because it pre-renders ~24 entries
  of pulses into the peripheral. The same buffer is why DIR can be written while
  committed pulses are still going out, which is the reversal step loss.
* Task placement had to be tuned three times (Core 1 → unpinned → Core 0)
  purely to keep FAS's `StepperTask` from colliding with either the sampler or
  the network stack.

None of these are motion bugs. They are contention bugs wearing motion costumes.

## 2. Proposal

Split the system across the two MCUs already on the board:

| | ESP32-S3 | ESP32-C5 |
|---|---|---|
| role | motion only | everything network-facing |
| owns | FAS pulse generation, SlopMotion/Ruckig, homing, INA228, RS485 servo bus | SlopSync hub, catalog, trust ledger, WiFi 6 / 5 GHz, UDP discovery |
| radios | none in the motion path | all of them |
| retains | OTA + HTTP diagnostics (idle cost only) | — |

Link: **UART, 2 Mbaud target.** The C5 is the SlopSync *hub*; the S3 is a
SlopSync *device node* publishing motion channels.

### 2.1 The link is not a new protocol

`slopsync::ITransport` already abstracts transports, with two implementations
(`SlopSyncAsyncWsTransport`, `SlopSyncBleTransport`). The internal link is a
**third implementation, `SlopSyncUartTransport`** — not a bespoke framing
protocol invented alongside the good one we already wrote.

This is the Prime Rule (through SlopSync, never around it) applied to our own
inter-MCU link, and it lands in the planned repo split as a transport in the lib.

## 3. Sacrifices accepted

* **BLE is dropped, not ported.** Operator ruling 2026-08-01: BLE is used almost
  exclusively for discovery, is not viable for streaming, and its bandwidth
  ceiling constrains everything else. It is also the single largest memory line
  item (§4.1). Discovery moves to UDP (already implemented) plus whatever the
  C5's WiFi 6 stack offers.
* HTTP diagnostics stay on the S3 and stay idle-cost-only.

## 4. Feasibility gates — BOTH unresolved

### 4.1 Measured cost on the S3 today (fw 2.3.31 boot trace)

```
ss:ws         +9,248 B
ss:ble       +64,080 B     <- dropped per §3
ss:udp          +144 B
ss:hubtask   +17,784 B
ss:sign       +9,504 B
ss:ctor/misc    +468 B
                           hub service in PSRAM: 240,976 B
                           catalog encodes to 24,591 B of 32,768 B scratch
steady state: heap free=36,591  min=27,051  maxblock=18,420
```

Internal total ≈ **101 KB**, plus **241 KB** of PSRAM for the hub service
object. Offloading returns the S3 to roughly 137 KB free internal — near 4x
today's headroom — and very likely dissolves LEDGER "THE QUEUE" item 0, since
that investigation targets a subsystem that would no longer be on the chip.

### 4.2 GATE A — does it fit in the C5's SRAM?

ESP32-C5 has ~384 KB SRAM and **no PSRAM on `board = esp32-c5-zero`** (to be
confirmed against the physical module). A WiFi 6 + LWIP stack is not modest.

Rough budget, BLE already removed:

```
hub service object          240,976 B   (includes the 32 KB catalog scratch)
hub task stack               17,784 B
ws port                       9,248 B
sign task stack               9,504 B
udp discovery                   144 B
                            ---------
app subtotal                ~278 KB
WiFi 6 + LWIP                 60-100 KB  (estimate, unmeasured)
                            ---------
                            ~338-378 KB against ~384 KB
```

**This does not comfortably fit.** It is marginal at best and negative at worst.
Relocation alone is insufficient — the hub must genuinely shrink.

### 4.3 GATE B — is the shrink achievable?

Yes, plausibly, because the library is **templated on capacity** rather than
hardcoded. Identified levers, all real parameters in the current source:

| lever | current | note |
|---|---|---|
| `kCatalogScratchBytes` (hub.hpp:708) | 32,768 B | catalog actually encodes to 24,591 B; the slack is pure waste, and a streaming encoder removes the buffer entirely |
| `kHubMaxSessions` (hub.hpp:51) | 4 | **conformance floor, SPEC §6.3/§17.1 — cannot reduce without breaking conformance** |
| `limits::max_subscriptions_per_session` | 64 | generated registry constant; 44 channels exist total |
| `limits::paired_devices_max` | 8 | trust ledger sizing |
| `SlopSyncAsyncWsPort` per-slot buffers | 82,764 B total | largest reducible item — see §4.4 |

### 4.4 Measured `sizeof` breakdown (compile-time probe, 2026-08-01)

```
slopdrive::SlopSyncHubService   240,976 B   (matches the boot trace exactly)
  slopsync::Hub                  92,968 B     of which 32,768 B is catalog scratch
  SlopSyncAsyncWsPort            82,764 B   <- second-largest single object
  SlopSyncBlePort                16,560 B     (object only; the NimBLE stack
                                               costs a further ~64 KB at init)
  PatternPresetStore              1,732 B
  PacingRing                      1,552 B
  remainder (delegate, crypto,   ~45,400 B
   uiTokens, udpDiscovery, ...)
```

Two findings that change the shrink strategy:

1. **The WS transport is nearly as expensive as the hub itself** (82,764 B).
   Five slots of per-session buffering. This is the largest reducible item and
   it was not on the radar before measuring.
2. **BLE's object cost is small (16,560 B); its real cost is the NimBLE stack**
   (~64 KB at init). Dropping it saves ~80 KB total, but the saving is mostly
   stack, not struct.

Revised budget with BLE removed:

```
hub object                      92,968 B
ws port                         82,764 B
misc members                   ~48,700 B
                               ---------
app objects                    ~224 KB
task stacks (hub/ws/sign)       ~37 KB
                               ---------
                               ~261 KB
WiFi 6 + LWIP                   60-100 KB   (estimate, still unmeasured)
                               ---------
                               ~321-361 KB against ~384 KB
```

**Marginal, but no longer implausible.** It fits only with the §4.3 levers
applied, and only if the WiFi 6 stack lands at the low end of the estimate.

## 4.5 GATE A RESOLVED — measured on hardware 2026-08-01

C5-Zero, `esp32-c5-zero`, live on the 5 GHz band (ch 44, 11ax=1, RSSI -58):

```
chip=ESP32-C5  cores=1  flash=4,194,304  psram=0
heap_total = 272,384 B          <- NOT 384 KB; that is the app-usable pool

S0 boot          free=241,352  largest=212,980
S1 +led          free=239,780  (-1,572)
S2 +uart @2Mbaud free=237,484  (-2,296)
S3 +wifi init    free=183,536  (-53,948)   <- WiFi is essentially the whole cost
S4 +wifi conn    free=181,948  (-1,592)
S5 +httpd+ws(5)  free=173,332  (-8,616)
live             free=172,756  min=167,644  largest=147,444
```

**Verdict: relocating the hub is dead. The bridge is comfortable.**

* `SlopSyncHubService` is 240,976 B as ONE object. Free heap is ~173 KB and the
  largest contiguous block is ~147 KB. It does not fit, and no plausible
  shrink closes a 94 KB gap against the contiguous-block constraint.
* A **bridge** — WiFi + UART + HTTP/OTA + a 5-slot WebSocket server — costs
  ~68 KB total and leaves **~147 KB contiguous free**. That is not marginal.

An OTA-only HTTP server costs **1,320 B** (Arduino `WebServer`) or **8,616 B**
including a 5-slot WebSocket (`esp_http_server`). The "microscopic HTTP server"
requirement was never the problem.

### Revised architecture — bridge, not relocation

The C5 terminates WiFi and WebSocket and relays **whole SlopSync frames** to
the S3 over UART. The **hub stays on the S3**, where PSRAM is. This still
achieves the actual objective — WiFi, BLE and AsyncTCP leave the motion MCU, so
Core 0 goes quiet and ~101 KB of internal RAM returns — without requiring the
hub to shrink at all.

Framing is SPEC §13.5 (COBS + `0x00` delimiter) using the canonical
`wire/serial_cobs.hpp`; the slot id rides inside the COBS envelope so one UART
multiplexes all five sessions. Nothing reimplements the codec.

### Bench results (C5 bridge, no S3 attached)

| test | result |
|---|---|
| WS handshake + frame relay | works, sizes 8..512 B |
| oversize frame (700 B) | refused, no crash |
| heap over full gauntlet | 172,608 -> 172,340, **delta -268 B, no leak** |
| 60x connect/disconnect churn | 0 failures, slots return to 0 |
| slot exhaustion (7 clients) | 5 admitted, 2 refused AND closed |
| UART backpressure | `write()` refuses instead of blocking (correct) |

Two bugs found and fixed by the gauntlet: WS slots were never released on
disconnect (`close_fn` was missing, so the fifth disconnect wedged the bridge
permanently), and over-capacity sockets were accepted then left silent.

### C5 link performance (WiFi 6, 5 GHz ch 44, RSSI -58)

WS echo round-trip, measured host <-> C5:

| payload | median | p95 | p99 | max |
|---|---|---|---|---|
| 8 B | 2.47 ms | 3.15 | 4.22 | 4.85 |
| 64 B | 2.43 ms | 3.01 | 3.78 | 4.66 |
| 242 B | 2.51 ms | 3.07 | 4.14 | 4.67 |
| 504 B | 2.63 ms | 3.45 | 4.33 | 4.48 |

Sustained 32 B echo: **471 round trips/s (2.12 ms each)**.

**TCP_NODELAY is mandatory and was worth 19x.** Before setting it the link ran
**46.9 ms median, 21 rt/s** — and flat across 8..504 B payloads, which is the
tell: payload size not mattering means a fixed stall, not bandwidth. Nagle was
holding each small frame until the peer's delayed-ACK timer (40 ms) expired.
Set via `httpd_config_t::open_fn`; there is no built-in for it in
`esp_http_server`. Do not remove it.

Also applied: `WiFi.setSleep(false)` (modem sleep parks the radio between
beacons and adds tens of ms of jitter), explicit `WIFI_PROTOCOL_11AX` so the
driver cannot negotiate down to 11n, and max TX power.

**Open risk:** `min_free` dips to ~90 KB during rapid churn — a ~76 KB
transient on a 272 KB heap. It recovers fully, but it is the tightest moment
observed and wants understanding before this carries real traffic.

## 4.6 LINK PROVEN END TO END — 2026-08-01

Wiring (operator, from the PCB — **this is the authority**, not `main`'s
`DongleTransport` values):

```
S3 TX  D1 / GPIO 43  ->  C5 RX  IO12
C5 TX  IO11          ->  S3 RX  D0 / GPIO 44
```

`main`'s 43/44 for the S3 were correct; the C5 side was NOT 7/8 — those are
`src/c5_waveshare`'s `PIN_RELAY_TX/RX` for the ESP-NOW relay node, a different
board and a different link. Using them measured zero raw bytes in both
directions across both S3 pin orders (fw 2.3.36 / 2.3.37).

### Measured

| test | result |
|---|---|
| S3 -> C5 heartbeat | 6 sent, 6 received |
| 300 x 64 B | 100.0% bytes, 100.0% frames, 0 decode errors |
| 300 x 242 B | 100.0% bytes, 100.0% frames, 0 decode errors |
| 500 x 504 B | 100.0% bytes, 100.0% frames, 0 decode errors |
| 45 s saturation, 242 B | **195.4 KB/s (97.7% of line rate)**, 9,031,711 / 9,032,905 B = 99.99%, 31 decode errors (0.084%) |
| 30 s @ 100 fps, 64 B (realistic) | **100.00% bytes, 100.00% frames, 0 errors, 0 drops** |

Heaps flat throughout: S3 121,451 free / C5 153,644 free.

### Two fixes the bring-up required

1. **RX buffer must outlast the DRAIN INTERVAL, not the frame.** `pumpRx()`
   runs on the hub's 5 ms tick and 2 Mbaud delivers ~1,000 B/ms, so ~5,000
   bytes land between drains. The initial 4,096 B buffer was smaller than one
   service interval and overflowed by construction (measured 9,513 of 20,100 B
   with 28 decode errors). Now 16,384 B.
2. **Bulk-read, never byte-at-a-time.** `HardwareSerial::read()` takes the UART
   mutex per call; ~5,000 of those per tick could not keep up with the wire.
   Symptom was size-dependent and misleading: 64 B frames passed at 100% while
   242/504 B sat at 68-74%. `readBytes()` into a 512 B chunk fixed it outright.

The 0.084% error rate appears only at full wire saturation, which is ~10x the
real workload. At the expected motion-stream cadence the link is lossless.

## 5. Phasing

**Phase 0 — measure.** DONE for the S3 side (§4.4). Still open: confirm C5-Zero
PSRAM presence, and measure the actual WiFi 6 + LWIP footprint on a C5 with a
bare sketch. Those two numbers close Gate A.

**Phase 1 — shrink in place.** Apply the §4.3 levers on the S3, where the whole
system still works and regressions are immediately visible. Re-measure. If the
hub cannot be brought inside the C5 budget here, the proposal stops and the
board respin (§7) becomes the path.

**Phase 2 — `SlopSyncUartTransport`.** Implement against `ITransport`, exercised
S3-to-host first so the transport is proven before the topology changes.

**Phase 3 — split.** C5 becomes hub, S3 becomes node. Both OTA paths, version
skew policy, and cross-MCU diagnostics defined before flashing.

**Phase 4 — re-measure motion.** The whole point. Quantify reversal drift and
script-playback smoothness on a quiet S3.

## 6. Risks

* **Two firmwares.** OTA for both, version skew, cross-MCU debugging. Manageable
  but it is permanent added surface.
* **The C5 is the weaker chip.** Single RISC-V core against the S3's dual
  Xtensa. It inherits the hub's worst-case work with less to do it with.
* **Phase 1 may fail.** If the hub will not shrink to fit, Phases 2-4 are dead
  and the effort is not wasted only because the shrink itself benefits the S3.

## 7. Explicitly out of scope

* **Motion MCU change (WROOM-32D).** The classic ESP32 is FastAccelStepper's
  best-supported target — mature MCPWM/PCNT, years of field use — versus the
  S3's fresh IDF5 port that produced a 5x clock bug and a missing `repeat_entry`
  in one session. This is a real future option and this proposal makes it
  *possible*, but changing the motion MCU and the topology together would make
  any regression un-attributable. Decide after Phase 4.
* **Board respin / ESP32-P4.** Same reasoning, longer horizon.

## 8. Open questions

1. ~~C5-Zero PSRAM~~ — ANSWERED: none (`psram=0`). Drove the shift to a bridge.
2. ~~WiFi 6 + LWIP footprint~~ — ANSWERED: 53,948 B, at the low end of estimate.
3. ~~Does dropping BLE require a SPEC change?~~ — RAISED AS **SlopSync
   RFC-056** ("Modular conformance: a hub is a set of duties, not a chip",
   `spec/RFC-QUEUE.md`), which demotes BLE GATT to SHOULD, states that a hub
   may be split across processors so long as the composite satisfies the
   golden vectors, and adds the client duty to offer WiFi credential entry.
   Until that RFC lands, the BLE removal here is a known conformance gap.
   OPERATOR RULING 2026-08-01:
   yes, and the SPEC changes. RFC-043's "BLE GATT is MUST" (`SPEC.md:1167-1172`)
   is amended: BLE is dropped, UDP discovery becomes the discovery path. The
   spec is pre-release and unratified, and the author's position is that a
   hardware controller is an ESP32 in every observed case and a remote is on
   WiFi regardless — so the BLE MUST buys nothing it cannot get from UDP + WiFi.
   Needs writing into SPEC.md + a CANON §3 amendment entry; until then this doc
   is the only record of the ruling.
4. Which side owns the trust ledger and NVS after the split?
5. ~~BLOCKER — physical wiring~~ — ANSWERED from the `main` branch, where the
   removed `DongleTransport` used this exact link (`main:config_api.h:537-538`):

   ```
   S3 Serial2 TX = GPIO 43  ->  C5 GPIO 8 (Serial1 RX)
   S3 Serial2 RX = GPIO 44  <-  C5 GPIO 7 (Serial1 TX)
   ```

   Both GPIOs and `Serial2` itself are unused on `feat/cpp20-slopsync`, and the
   S3's `Serial1` stays the Modbus servo bus. The old link ran 460800; 2 Mbaud
   is the target over a few cm of trace. NOTE: GPIO 43/44 are the Nano ESP32's
   D0/D1 header pins — free only because the board's USB is native CDC, not a
   UART0 bridge. Do not reassign them.
6. Flash: the bridge is 1,065,023 B in a 1,310,720 B app partition (81%). OTA
   needs TWO app slots. 4 MB is workable but the partition table needs
   designing, not defaulting.
7. The 92 KB `min_free` churn transient (§4.5).

## 9. Hardening pass — 2026-08-01/02

The bridge came up working and then dropped WebSocket sessions for a full
evening. Six defects; five fixed. Status and rulings live in
the dev board (`bd`, epic sd-6kz and its children); the
mechanisms are here.

### 9.1 The one that mattered — unsolicited PONG reset the connection

`httpd_uri_t` was registered with `handle_ws_control_frames = false`. A PONG
arriving unsolicited reached `httpd_ws_recv_frame()` as an error, `wsHandler`
did `return e`, and **a non-OK return from an esp_http_server URI handler
closes the socket.**

.NET's `ClientWebSocket` defaults `KeepAliveInterval` to 30 s and emits exactly
that frame on exactly that cadence, which is why the failure was a metronomic
30.0 s and why it hit MultiFunPlayer and nothing else on the bench. RFC 6455
§5.5.3 is unambiguous that this is legal and needs no response.

The bridge now owns control frames and answers them per §5.5:

| frame | response | clause |
|---|---|---|
| unsolicited PONG | discard | §5.5.3 — unidirectional heartbeat |
| PING | PONG carrying the payload verbatim | §5.5.2 |
| CLOSE | echo CLOSE, let `close_fn` tear down | §5.5.1 |

**The failed first attempt is the more useful lesson.** "Ignore it, return
`ESP_OK`" turned an immediate reset into a reset twelve pongs later: a frame
whose bytes are never read stays in the TCP stream and desyncs the parser for
everything after it. Draining the payload — for every frame type, control
frames included — is what actually fixed it. Tolerating an unknown frame is not
the same as consuming it.

Verified: 150 s, 29 unsolicited PONGs, 0 disconnects, PING payload returned
intact.

### 9.2 Cross-task use-after-close on the slot table

`pumpFromS3()` (Arduino loop task) validated `g_wsUsed[slot]`/`g_wsFd[slot]`
and then sent; `onSockClose()` (httpd task) could clear the slot AND
`close(sockfd)` in between, returning the fd number to the OS. With
`lru_purge_enable` and clients churning, that number is reused immediately and
the bridge writes a SlopSync frame into an unrelated socket. Guarded by a
RECURSIVE mutex held across check-and-send and across clear-and-close —
recursive because a failing async send can re-enter `onSockClose` on the calling
task. The benign face of this race was `g_rxDrops` climbing continuously (3636
observed); the malignant face was the mid-stream crash.

### 9.3 Slot teardown had no signal, so silence was used as one

The S3 side reaped a slot after 10 s of RX silence, which destroys sessions that
SPEC §11.3 / RFC-042 say to RETAIN as STALE. The bridge now sends a control
frame when a socket actually closes.

**Bridge control channel** — the envelope is `[slot][payload]`; slot `0xFF` is
reserved for bridge-to-host control and carries `[op][args]`. Chosen outside the
`0..kSlots-1` session range so the receiver's existing `slot >= kSlots` guard
cannot mistake one for a session frame. Defined identically in
`src/c5_probe/main.cpp` and `include/comms/SlopSyncUartTransport.h` — one
vocabulary, two ends.

| op | payload | meaning |
|---|---|---|
| `0x01` | `[slot]` | that slot's WebSocket closed |

This is the same channel the WS-advertisement ruling needs for the C5 to report
its IPv4 + port; that work is still owed.

### 9.4 Depth, wedge, and the wrong access point

- **RX ring 16 -> 64.** `poll()` drains the whole 8 KB Serial2 buffer (~350
  frames) before the hub consumes, so any hub hiccup over ~250 ms overflowed a
  16-deep ring. Back-pressure at a delimiter boundary is the structural fix and
  is still owed; depth only widens the window.
- **`SO_SNDTIMEO` 50 ms.** `httpd_ws_send_frame_async()` blocks, and
  `pumpFromS3()` calls it on the loop task, so one client with a full receive
  window stalled the UART drain and every other client. Never observed firing
  once the real bug was found; kept because the hazard is real.
- **WiFi scan-and-pin.** The C5 sat at **-70 dBm one meter from an AP** (ch 44)
  while the S3 held -47 in the same room — an 8-AP UniFi network sharing one
  SSID, and the default fast-scan never roams off the first AP heard. Ported
  `WifiLink::_connectBest()`; result **-44 dBm on ch 161**. The loop also had no
  reconnect supervision at all, so `setAutoReconnect(false)` plus a re-scanning
  reconnect cycle now replaces Arduino's remember-the-last-one behavior.

### 9.5 Method note — witness before theory

Four firmware theories were built and shipped against this dropout before the
cause was found. Every one was a real defect; none was the dropout. What settled
it in four minutes was a WITNESS CLIENT — a non-MFP session held open on the
same bridge, from the same host, at the same time. It survived 120 s while MFP
died twice beside it, eliminating the bridge, the network, the WiFi and the load
at once, and pointing straight at what the client was doing differently.

Corollary learned the hard way: a witness that requests a motion publish grant
CONTESTS §11.4 source ownership with the client under test. Use a watch-tier
client for passive observation.
