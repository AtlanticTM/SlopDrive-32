---
paths:
  - "src/comms/**"
  - "include/comms/**"
  - "src/c5_probe/**"
---

# Transport and SlopSync boundary constraints

SPEC §13/§14 and the headers own the protocol story; this file is this
machine's half of the boundary plus the transport traps.

## SlopSync (NON-NEGOTIABLE)

The ecosystem sync protocol is developed in its OWN repo, SlopSync (sibling
checkout `../SlopSync`, pinned at this repo's root by `slopsync.pin`). This
machine repo CONSUMES it, never edits it. The spec, registry, codegen, library
invariants, frozen-artifact list, layering and tests are that repo's doctrine;
restating them here would violate C-1.

- **Consumption mechanics.** `platformio.ini` pulls `lib/slopsync` via
  `symlink://../SlopSync/lib/slopsync`, plus an explicit
  `-I../SlopSync/lib/slopsync/include` for `env:native`. `tools/gen_channel_map.py`
  and `tools/gen_channel_grid.py` read the sibling's `registry.yaml` to render
  this device's `docs/slopsync/CHANNEL-MAP.md`. `tools/catalog_lint.py` reads
  the sibling's generated `registry_constants.hpp`. `tools/canon_lint.py`'s pin
  rule FAILS if the sibling's HEAD does not match `slopsync.pin`, and
  cross-checks the sibling's frozen conformance artifacts against the same
  hashes SlopSync's own lint pins. Belt and suspenders across the repo
  boundary.
- **Spec-gap ritual, cross-repo order.** Need a number or rule the spec lacks:
  fix it in the SlopSync repo FIRST (registry.yaml or SPEC.md, regenerated,
  committed there), bump `slopsync.pin` to the new sha, THEN code against the
  constant here. Never a code-local magic number for anything wire-visible,
  and never a spec change made from this repo. Enforced by
  `.claude/hooks/vendor-lock.sh`.
- **Frozen (C-6).** The conformance artifacts and the `hub.hpp`/`client.hpp`
  public API freeze are SlopSync's own frozen list, enforced by its lint. This
  repo's half is the sha256 cross-check in `tools/canon_lint.py`.
- **Firmware shape.** `SlopSyncHubService` (composition root, own Core-0 task,
  single-task hub, T5) plus the transport ports plus `SlopSyncCatalog.h`. The
  service lives in PSRAM via placement-new from main.cpp (T2); never move it
  back to BSS. Session teardown funnels through one path (T3), and
  **back-to-back sessions without a reboot is mandatory verification for any
  session-lifecycle change.**
- **Auth.** `validateToken` resolves `/uitoken`, then the trust ledger, then
  `watch`. Tokenless clients can watch and e-stop (stop and estop are
  role-EXEMPT) but cannot command motion. While `/uitoken` is enabled, LAN HTTP
  equals control; a lockdown posture buys a chokepoint, not LAN secrecy.
- **SlopSync is the ONLY input/output plane (operator ruling 2026-07-26).**
  Motion input, telemetry, anomaly events and settings ride SlopSync channels.
  HTTP remains for fallback polling and bootstrap only.
- **Transport doctrine (operator rulings 2026-07-27, calibrated).** SlopSync is
  the only protocol and is transport-agnostic (SPEC §13, RFC-043 profiles). For
  hardware hubs: BLE GATT is the conformance floor (infrastructure-free
  control, discovery, future WiFi provisioning); WebSocket is the preferred
  high-throughput path, expected on ESP32-class silicon; clients auto-upgrade
  BLE to WS. ESP-NOW is the ESP32-peer binding: supported and deliberately
  trivial to enable, NOT actively developed or tested. UI-serving is a hub
  capability, never a requirement.
- **Intake doctrine (operator ruling 2026-07-27).** On THIS machine the only
  way in and out is SlopSync. Other firmwares are never forced: SlopSync
  competes via the CLIENT ONRAMP (RFC-044). TCode passthrough is the easy rung
  (clients feed the TCode they already generate through a SlopSync session),
  then native segments, then native samples. First-party client support in
  MFP, Intiface and similar is maintained and encouraged. TCode integration is
  a CLIENT-SIDE adapter, never a hub-side stream.
- **Clients.** The MFP plugin and the verifier
  (`tools/slopsync_probe.py --ip <ip> --port 82`) both live in the SlopSync
  repo. `LiveWireTest` refuses to run homed; run it TWICE back to back, which
  is the T3 check.



## UART bridge profile (S3 <-> C5, SlopSyncUartTransport)

- Serial2 only, TX GPIO43 / RX GPIO44, 2 Mbaud. Serial1 is the Modbus servo
  bus and must never be touched from here (SlopSyncUartTransport.h:16-17,86).
- Buffers sized BEFORE begin() (HardwareSerial refuses to resize running):
  TX 4096, RX 16384. The RX buffer must outlast the 5 ms DRAIN INTERVAL, not
  the frame: 2 Mbaud lands ~5,000 B between hub ticks; 4096 overflowed by
  construction (SlopSyncUartTransport.h:100-111, c5-comms-offload.md §4.6).
- Bulk reads only: readBytes() into a 512 B chunk. Per-byte read() takes the
  UART mutex per call and measurably cannot keep up
  (src/comms/SlopSyncUartTransport.cpp:149-157).
- RX ring depth 64 per slot, sized for ~1 s of hub stall, not steady rate.
  16 (copied from BLE) measured drops live (SlopSyncUartTransport.h:143-165).
- pumpRx() stays in TASK context, never an ISR: the service is PSRAM-resident
  and PSRAM faults during flash-cache-disabled windows
  (SlopSyncUartTransport.h:47-52). SPSC + deferred attach/detach discipline
  kept identical to the other ports even though producer==consumer here
  (TRAPS T5; SlopSyncUartTransport.h:28-46).
- write() checks availableForWrite() and refuses instead of blocking; false
  is flow control (§13.1), not an error. No per-session teardown over shared-
  wire backpressure (SlopSyncUartTransport.h:54-63).

## Framing robustness -- verified 2026-08-03

- **The delimiter sits BETWEEN frames, so resync costs at most one frame.**
  The wire is `COBS(body) + 0x00` per frame, and the `0x00` is appended by the
  transport, not the codec (`serial_cobs.hpp` header note). A COBS-encoded
  region never contains a literal `0x00`, so after garbage, a brownout, or a
  mid-plug the receiver discards bytes only until the next delimiter and the
  following frame decodes normally. `_rxOverflow` implements this: it
  suppresses accumulation until the next delimiter, so an oversized run
  resyncs rather than poisoning the stream.
- **There is NO general CRC, and that is the binding's declared design.**
  Ordinary frames carry none; the serial binding declares `reliable=true`
  because the link is a wired point-to-point trace. The ONE exception is the
  ESTOP frame: 12 bytes, `E5 E5 E5 E5 | cause origin seq:u16 | crc32`, IEEE,
  **over the first 8 PRE-COBS bytes** (SPEC §5.5). Verified: the CRC covers
  the frame payload, never the encoded bytes, so a receiver validates after
  deframing OR directly on a raw scan when the window happens to be
  zero-free. Adding a CRC to ordinary frames is a WIRE CHANGE and rides an
  RFC in SlopSync; it is not a local hardening decision.

## RX ring sizing -- the robustness knob, because there is no flow control

The link runs without RTS/CTS by design, so the RX ring is the only thing
absorbing a burst. It is sized against the DRAIN INTERVAL, never the frame:
`pumpRx()` runs on the hub's 5 ms tick, and 2 Mbaud delivers ~5,000 B between
drains. `kUartLinkRxBufferBytes = 16384` gives ~80 ms of absorption at 2 Mbaud
(~65 ms at 8/N/1 framing overhead), which is roughly 16 drain intervals of
slack. The original 4,096 overflowed by construction. TX is 4,096.

**Buffers must be sized BEFORE `begin()`.** `setRxBufferSize`/`setTxBufferSize`
are silent no-ops once the port is running (`HardwareSerial.cpp:667-691`). The
symptom is a size cliff, not an error: small frames pass while anything over
the ~128 B hardware FIFO is refused forever.

## Physical-layer footguns

- **Common ground before signal.** On jumpered runs, connect GND first and
  remove it last. Two boards on separate supplies with TX/RX joined and no
  shared return will inject the return current through the signal pins.
- **Both ends are non-inverted, idle-HIGH raw TTL.** No RS-232 transceiver, no
  inverter, no level shifter in the path (both are 3V3). An inverting adapter
  or a `Serial.begin()` invert flag on either end yields a link that looks
  wired correctly and never frames.

## COBS + CRC

- Wire: COBS([slot_id][slopsync frame]) + 0x00 delimiter (SPEC §13.5).
  Delimiter-finding is the transport's job; the codec is the sibling lib's
  wire/serial_cobs.hpp, used identically by both ends. Never reimplement
  COBS here.
- Ordinary frames carry no CRC; the binding declares reliable=true (wired
  point-to-point). The ESTOP frame is the one exception: 12 bytes,
  E5 E5 E5 E5 | cause origin seq:u16 | crc32 (IEEE, first 8 bytes).

## ESTOP raw-scan duty

- A receiver in unsynced/corrupt state MUST scan raw bytes for the 4x0xE5
  magic before and regardless of COBS decode (SPEC §13.5). COBS passes a
  zero-free 0xE5 run through unchanged; that property is load-bearing.
  Implemented live in src/c5_probe/main.cpp pumpFromS3(): scan first, fan
  out under the slot lock, no session required.
- On CRC failure resume scanning at i+1, not i+4 (overlapping candidates).

## Relay semantics (SPEC §14), for anything relay-shaped

- Frames, not sessions; a relay holds no grants and parses no control CBOR.
- Priority-aware dual queues; may decimate samples-kind streams and conflate
  STATE, MUST NOT decimate segments-kind streams (a segment is a schedule).
  Blind FIFO is non-conformant (§14.1).
- Reliability is hop-by-hop (Honesty Clause H10). ESTOP fast path: forward on
  all attached segments ahead of every queued frame, at most one frame-time
  of added latency (§14.2). CLOCK frames: correct, be transparent (<1 ms), or
  drop; exactly one (§14.3). One relay hop maximum in v1.
- The C5 bridge is NOT a §14 relay: it terminates the WS session and forwards
  raw frames over UART; hub/session state lives on the S3. It implements the
  ESTOP raw-scan duty; it does not implement §14.1 shedding
  (docs/c5-comms-offload.md, "bridge, not relocation").

## T3 -- session teardown must be ONE funnel

**Rule:** every way a session can end (graceful bye, rude disconnect,
eviction, slot reuse) runs the same teardown routine, which runs the full
resource-loss policy. No unmonitored path to motion.
**Mechanism:** ownership released only by a watchdog that requires an occupied
slot means any teardown that clears the slot first leaks the ownership
forever, and every later client is silently rejected until reboot. Invisible
whenever deploys reboot the device between test runs.
**Mandatory check for any session-lifecycle change:** two full client sessions
back to back WITHOUT a reboot between them.

## T5 -- async library callbacks run on the library's task

**Rule:** transport and network callbacks (AsyncTCP and friends) never mutate
hub or session state directly. They enqueue, and the owning task applies:
attach, detach and RX are deferred to the hub task.
**Mechanism:** the callback executes on the async library's own FreeRTOS task,
concurrently with your update loop on another task. A callback that nulls a
pointer mid-`update()` is a use-after-free with no data race visible in
single-task reasoning. The hub is single-task BY DESIGN; keep it true.

## T8 -- never stream to a wedged WebSocket client under a shared lock

**Rule:** do not send telemetry to backgrounded or unresponsive WS clients
from a path holding a mutex the HTTP task needs. Sends must be bounded or
deferred.
**Mechanism:** a TCP send to a client that stopped reading fills the socket
buffer and blocks. If the sender holds the server mutex, every HTTP request
queues behind one dead browser tab.

## T11 -- wire-visible strings are protocol bytes

**Rule:** catalog descriptions, channel labels, and any string that ships in
an encoded artifact are wire content. Editing one changes encodings and etags
and invalidates binary fixtures. Respelling or rewording them is a protocol
change: C-11 flags it, and frozen artifacts never change.

## T13 -- fan-out senders must re-check the transport, not just the session

**Rule:** any hub path that iterates slots and SENDS must test
`slot.transport != nullptr` on the slot it writes to. Occupied, ready and
subscribed do NOT imply attached. Skip the slot, and never route the miss
through the congestion or stall tracker.
**Mechanism:** RFC-042 parks a session when its transport dies. Slot,
session_id, grants and subscriptions are all RETAINED while
`detachTransport()` nulls `slot.transport`. `update()`'s walk skips null
slots, so every per-slot pump is safe by construction, but a FAN-OUT sender
reaches the parked slot anyway and hands null to an `ITransport&` parameter.
Binding a null reference costs nothing until the virtual call, which loads a
vtable from address 0: LoadProhibited, EXCVADDR 0. It fires only when one
client sits parked while ANOTHER triggers the broadcast, so single-client
tests and clean-disconnect tests never see it, and the first client of the
next test run is the one that dies. Tracking the failed send is its own bug: a
parked session has no link to be congested on, and aging it toward eviction
punishes it for a failure that never happened.
Bit us: `broadcastSafetyNow()`, the ONE fan-out sender missing the check its
siblings already had, panicking the device on `override_on` and e-stop
whenever a previous probe session was still parked (fw 2.1.81, three field
reboots).

## T14 -- an unchecked radio-config return code ships a payload that was never on the air

**Rule:** every NimBLE advertising-data setter returns `bool`. Check it and log
a WARN on failure; never chain calls and discard the result.
**Mechanism:** `NimBLEAdvertisementData` builds one legacy payload of 31 bytes.
Each `set*()` call appends an AD record via `addData()`, which silently returns
`false` and adds nothing once the running total would exceed 31. It does not
truncate, does not replace an earlier record, and raises no exception. Every
record added BEFORE the overflowing one still lands on the radio; only the
overflowing one and everything after it in the same payload goes missing. A
scan showing Flags plus Service UUID plus Name but no Manufacturer-Specific
Data is not a filtering artifact of the scanning OS, it is proof the MSD
`set*()` returned `false` and nobody checked. The fix is never "shrink
something until it fits by luck", it is to budget the payload on purpose and
split records across the advertisement and the scan response, which have
independent 31-byte budgets.
Bit us: the `ble_adv_flags` byte was never on the air in any build through
fw 2.1.83. Service UUID (18) plus Flags (3) plus `setName()`'s default
COMPLETE name (6, a second compounding bug, since `setName(x)` defaults
`isComplete=true`) already totaled 27 of 31 bytes. Found by an operator phone
scan with nRF Connect, which shows raw AD structures rather than an
OS-filtered summary.

## T16 -- a stall watchdog sized for "stuck" cannot tell it apart from "big"

**Rule:** before arming a one-size stall or timeout watchdog on a shared queue,
classify the traffic crossing it. A healthy bulk transfer that legitimately
takes many ticks to drain looks IDENTICAL, from the queue's own point of view,
to a client that stopped reading and will never come back.
**Mechanism:** the control-stall timer (`kCtrlStallMs` = 2000 ms) exists to
catch a client stranded waiting on a reply that will never come, which is
correct for a genuinely wedged peer. Before BLOB_CHUNK got its own
backpressure class it was classified as ordinary control traffic: a 129-chunk
catalog transfer pumping into a 32-deep send queue filled it well inside 2
seconds, and the SAME timer built for a wedged client tore the session down
mid-transfer, on a perfectly healthy connection whose only sin was draining a
big honest payload no faster than the network allowed.
**Fix:** BLOB_CHUNK got its own class, paced against the registry's OWN
advertised sender budget (`limits::blob_chunks_in_flight`) via the same
queue-depth check the data class already used, and made to hold and retry. It
never arms the control stall timer and never NACKs or tears down on its own.
Bit us: a growing catalog BLOB transfer closing every session that tried to
fetch it, misdiagnosed at first as heap exhaustion because it happened
alongside a real heap-pressure bug (T2).

## T26 -- canceling a blocked task from outside only works if it is blocked where you think

**Rule:** before canceling another task's work from a watcher task, PROVE
where the target is blocked. If the mechanism is "wake the blocked syscall"
and the target is not in that syscall, the cancel does nothing, and the
attempt is not free.
**Mechanism:** `shutdown(fd, SHUT_RDWR)` wakes a task parked in `read()` on
that fd. It does nothing for a task spinning in a poll loop, and it can make
things WORSE: a socket left in an error state can turn a `while (available())`
loop that used to yield into one that returns immediately forever, so the task
stops yielding at all and starves the idle task it shares a core with. The
cancel then CAUSES the watchdog reboot it was written to prevent.
**Bit us (fw 2.2.3):** a Core-0 beacon let `commsTask` cancel any `httpTask`
step blocked over 800 ms. The device's own log:
`blocked >800ms - canceled client fd=57` followed by `blocked 10015ms`, so the
cancel did not unblock it, then the same fd re-reaped every ~800 ms forever,
then `reset_reason TASK_WDT`. It also canceled every legitimate page load and
every OTA upload, so the device could no longer be flashed over the network
and needed a serial rescue. Three regressions from one unverified assumption.
**Fix:** the reap is disabled (`kCore0ReapMs = 0`). The reusable lessons:

- **A deadline cannot separate slow from stuck when they overlap.** Measured
  on this device: legitimate full page serve 9 s for a 302 KB bundle, stuck
  half-open client 10 s, OTA upload seconds. Any threshold that catches the
  stall also kills the page and the flash. This is why Apache's
  `mod_reqtimeout` gates on a byte RATE rather than a deadline: progress is
  separable, elapsed time is not.
- **A one-connection-at-a-time server has no fix at this layer.** The Arduino
  sync `WebServer` serves a single client and every long operation owns the
  whole plane. The answer is an event-driven server, not tuning.
- **Co-deployed changes prove nothing individually.** The 16-socket test that
  showed zero reboots could not distinguish the reap from the watchdog raise
  that shipped with it. Change one thing, or attribute nothing.
