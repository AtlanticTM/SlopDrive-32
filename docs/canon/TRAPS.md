# TRAPS — hard-won mechanism lessons

The single home ([CANON](CANON.md) C-1/C-12) for field bugs and platform traps that must
never be re-learned. Code comments point here; they do not retell these
stories. Each entry: the rule, then the mechanism — because knowing *why*
is what stops the same bug wearing a new coat.

## T1 — Object reset via `*this = T{}` is a stack bomb
**Rule:** reset large objects with in-place destroy + placement-new
(`obj.~T(); new (&obj) T();`), never `*this = T{}` / `obj = T{}`.
**Mechanism:** the right-hand `T{}` is a full temporary constructed ON THE
CURRENT STACK before assignment. A multi-KB object on an 8 KB FreeRTOS task
stack overflows it instantly. Host-side tests never catch this class —
desktop threads get megabyte stacks. Enforced by canon_lint `this-assign`.
Bit us: a ~9 KB session temporary panicking the hub task on every client
connect (July 2026).

## T2 — Big statics starve the internal heap
**Rule:** services with large state live in PSRAM via placement-new from
`main.cpp`, never as ordinary statics/BSS.
**Mechanism:** BSS eats internal SRAM at link time; the WiFi stack and page
serving allocate from the same internal heap at runtime. A ~100 KB static
left ~13 KB free heap and killed the network stack. Watch the boot heap
beacon (`[sys] heap free/min/maxblock psram`).

## T3 — Session teardown must be ONE funnel
**Rule:** every way a session can end (graceful bye, rude disconnect,
eviction, slot reuse) runs the same teardown routine, which runs the full
resource-loss policy. No unmonitored path to motion.
**Mechanism:** ownership released only by a watchdog that requires an
occupied slot means any teardown that clears the slot first leaks the
ownership forever — every later client is silently rejected until reboot.
Invisible whenever deploys reboot the device between test runs.
**Mandatory check for any session-lifecycle change:** two full client
sessions back-to-back WITHOUT a reboot between them.

## T4 — No function-local statics inside critical sections
**Rule:** never declare a function-local `static` (of class type) inside
`portENTER_CRITICAL` / any no-abort context; hoist to file scope.
**Mechanism:** first execution of a function-local static registers its
destructor via `__cxa_atexit` and takes an init-guard lock — both can
allocate/abort, and inside a critical section that aborts the core.
A path that has "never run live" (ours: token minting) hides this until the
first real use.

## T5 — Async library callbacks run on the library's task
**Rule:** transport/network callbacks (AsyncTCP et al.) never mutate hub or
session state directly — they enqueue, and the owning task applies
(attach/detach/RX deferred to the hub task).
**Mechanism:** the callback executes on the async library's own FreeRTOS
task, concurrently with your update loop on another task. A callback that
nulls a pointer mid-`update()` is a use-after-free with no data race visible
in single-task reasoning. The hub is single-task BY DESIGN; keep it true.

## T6 — Log sinks must never block
**Rule:** any log sink is non-blocking by contract: drop-and-count when the
output is full. Never add a sink that can wait.
**Mechanism:** USB-CDC serial with no host attached blocks ~100 ms per line
in the TX-full path — on whatever task drains the log ring (httpTask). One
chatty subsystem then freezes HTTP serving with zero CPU load visible.

## T7 — LED freeze is a diagnostic, not a bug
**Rule:** never "fix" static LEDs by moving the SlopGlow pump or removing a
heartbeat pulse. The engine only animates while every registered core
heartbeat pulses.
**Mechanism:** it's a liveness gate. The pump runs on httpTask; heartbeats
come from motorTask (Core 1) and commsTask (Core 0). Frozen LEDs + live
cores ⇒ httpTask is blocked (see T6). This distinction has solved a field
incident; preserve it.

## T8 — Never stream to a wedged WebSocket client under a shared lock
**Rule:** don't send telemetry to backgrounded/unresponsive WS clients from
a path holding a mutex the HTTP task needs; sends must be bounded or
deferred.
**Mechanism:** a TCP send to a client that stopped reading fills the socket
buffer and blocks; if the sender holds the server mutex, every HTTP request
queues behind one dead browser tab.

## T9 — C++ default arguments bind to the STATIC type
**Rule:** forwarding proxies/wrappers pass explicit sentinels through to the
base; they never restate the base's default arguments.
**Mechanism:** default args are substituted at the CALL SITE from the
declared (static) type of the expression, not the dynamic type — a proxy
that redeclares a default silently overrides a subclass's different default.

## T10 — PIO's native test runner misreports doctest
**Rule:** trust the process exit code (or run `.pio/build/native/program.exe`
directly), never the runner's parsed summary ("0 test cases", phantom
CTRL_BREAK on failures are cosmetic lies).

## T11 — Wire-visible strings are protocol bytes
**Rule:** catalog descriptions, channel labels, and any string that ships in
an encoded artifact are wire content: editing one changes encodings/etags
and invalidates binary fixtures. Respelling/rewording them is a protocol
change ([CANON](CANON.md) C-11 flags, frozen artifacts never change).

## T12 — Registry/spec drift is caught by tools, not eyes
**Rule:** after any `registry.yaml` change: regenerate, run `--check`, run
`tools/catalog_lint.py`. A catalog that "did not encode (scratch N B)" is
usually NOT a sizing problem — it's entries out of ascending-id order.

## T13 — Fan-out senders must re-check the transport, not just the session
**Rule:** any hub path that iterates slots and SENDS must test
`slot.transport != nullptr` on the slot it writes to. Occupied, ready, and
subscribed do NOT imply attached. Skip the slot; never route the miss through
the congestion/stall tracker.
**Mechanism:** RFC-042 parks a session when its transport dies — slot,
session_id, grants and subscriptions are all RETAINED while
`detachTransport()` nulls `slot.transport`. `update()`'s walk skips null
slots, so every per-slot pump is safe by construction; a FAN-OUT sender
reaches the parked slot anyway and hands null to an `ITransport&` parameter.
Binding a null reference costs nothing until the virtual call, which loads a
vtable from address 0 — LoadProhibited, EXCVADDR 0. It fires only when one
client sits parked while ANOTHER triggers the broadcast, so single-client
tests and clean-disconnect tests never see it, and the first client of the
next test run is the one that dies. Tracking the failed send is its own bug:
a parked session has no link to be congested on, and aging it toward eviction
punishes it for a failure that never happened.
Bit us: `broadcastSafetyNow()` — the ONE fan-out sender missing the check its
siblings (`pumpEventDrain`, `submitSignature`) already had — panicking the
device on `override_on`/e-stop whenever a previous probe session was still
parked (fw 2.1.81, three field reboots).

## T14 — An unchecked radio-config return code ships a payload that was never on the air
**Rule:** every NimBLE advertising-data setter (`setFlags`/`setName`/
`setShortName`/`setCompleteServices`/`setManufacturerData`, and
`NimBLEAdvertising::setAdvertisementData`/`setScanResponseData` themselves)
returns `bool`. Check it and log a WARN on failure — never chain calls and
discard the result.
**Mechanism:** `NimBLEAdvertisementData` builds one legacy payload of
`BLE_HS_ADV_MAX_SZ` (31) bytes; each `set*()` call appends an AD record
(length + type + data) via `addData()`, which silently returns `false` and
adds nothing once the running total would exceed 31 — it does not truncate,
does not replace an earlier record, and raises no exception. Every record
added BEFORE the one that overflows still lands on the radio; only the
overflowing one (and everything after it in the same payload) goes missing.
A scan that shows Flags + Service UUID + Name but no Manufacturer-Specific
Data is not a filtering artifact of the scanning OS — it is proof the MSD
`set*()` call returned `false` and nobody checked. The fix is never "shrink
something until it fits by luck"; it is to budget the payload on purpose
(§13.4) and split records that don't all fit one payload across the
advertisement and the scan response, which have independent 31-byte budgets.
Bit us: the `ble_adv_flags` byte (RFC-046) was never on the air in any build
from Phase E's landing through fw 2.1.83 — service UUID(18) + Flags(3) +
`setName()`'s default **complete** name(6, not the intended shortened
name — a second, compounding bug: `setName(x)` defaults `isComplete=true`,
so it advertised AD type 0x09 "SD32" rather than the intended 0x08) already
totaled 27 of the 31 bytes, leaving no room for the 5-byte MSD record;
`advData.setManufacturerData(...)`'s `false` return was silently discarded.
Found by an operator phone scan with nRF Connect (which shows raw AD
structures, not an OS-filtered summary) — the earlier host-side `bleak` scan
had already shown empty `manufacturer_data` but that was wrongly attributed
to Windows/WinRT company-id filtering until the phone scan ruled it out.

## T15 — Fixed-priority status displays can mask a lower-priority, time-critical state
**Rule:** when a single "highest active state wins" display picks ONE thing
to show, rank by TIME-SENSITIVITY (what is gone if missed right now), not by
severity. A persistent, rediscoverable condition may correctly rank BELOW a
narrow window a human must catch immediately.
**Mechanism:** SlopGlow's `GlowState` shows exactly one state, the
highest-priority ACTIVE one. `Fault` originally outranked `Pairing`, and
`Fault` fires whenever the machine is simply unhomed
(`SlopGlowBoard.cpp`: `!state.homed && !state.homing_in_progress`) — the
ordinary state of a fresh boot, not a hardware failure. RFC-027's
push-to-pair window (opened by triple power-cycling a factory-fresh,
therefore UNHOMED, device) landed on exactly the device that was ALSO
showing Fault, and Fault won: red breathing instead of the pairing
invitation, hiding a 120 s, gone-if-missed ceremony behind a condition that
is still true — and still in `/api/log` — the next time anyone looks.
**Fix:** `GlowState` reordered so Pairing outranks Warning/Fault (still
below Ota, an active flash, and Estop, which are never allowed to be
masked). See `lib/slopglow/include/slopglow/slopglow_core.hpp`'s
`GlowState` ordering comment.
Bit us: the RFC-027 pairing ceremony reading as a plain Fault LED on the one
class of device (factory-fresh, unhomed) most likely to be running it.

## T16 — A stall watchdog sized for "stuck" cannot tell it apart from "big"
**Rule:** before arming a one-size stall/timeout watchdog on a shared queue,
classify the traffic crossing it. A healthy bulk transfer that legitimately
takes many ticks to drain looks IDENTICAL, from the queue's own point of
view, to a client that stopped reading and will never come back.
**Mechanism:** `SlopSyncAsyncWsTransport`'s control-stall timer
(`kCtrlStallMs` = 2000 ms) exists to catch a client stranded waiting on a
reply that will never come — correct for a genuinely wedged peer. Before
BLOB_CHUNK (0x1B) got its own backpressure class, it was classified as
ordinary control traffic: a 129-chunk catalog transfer pumping into a
32-deep send queue filled it well inside 2 seconds, and the SAME timer built
for a wedged client tore the session down mid-transfer — on a perfectly
healthy connection whose only sin was draining a big, honest payload no
faster than the network allowed. See `include/comms/
SlopSyncAsyncWsTransport.h`'s backpressure-classification comment.
**Fix:** BLOB_CHUNK got its own class, paced against the registry's OWN
advertised sender budget (`limits::blob_chunks_in_flight`) via the same
queue-depth check the data class already used, and made to hold-and-retry —
never arms the control stall timer, never NACKs or tears down on its own
(`hub_impl.hpp`'s `pumpBlobTransfer()` already retries the same un-sent
chunk next tick).
Bit us: a growing catalog BLOB transfer (57 chunks, later 129 as the catalog
grew) closing every session that tried to fetch it — misdiagnosed at first
as heap exhaustion (it happened alongside a real heap-pressure bug, T2)
before the actual mechanism — traffic classification, not memory — was
found.

## T17 — A flag reused across unrelated concerns can silence a sink for good
**Rule:** gate a sink's existence at runtime, not compile-time, and never let
a flag whose stated job is something else (transport selection, a feature
toggle, ...) also decide whether a diagnostic sink is registered at all.
**Mechanism:** the serial log sink was once wrapped in `#if
!SERIAL_CONTROL_MODE` inside `applogBegin()` — but `SERIAL_CONTROL_MODE`'s
actual job is picking the factory-default transport (serial vs WiFi), not
gating diagnostics. At the macro's normal value (1), that `#if` never
registered the serial sink at all: not throttled, not floored, ABSENT, for
the entire life of the build, regardless of anything happening at runtime.
Every `SLOG*` call still went out fine over the web ring, so nothing looked
broken from the firmware's own side; only a human watching USB serial and
expecting log lines would notice, by which point the boot banner and any
early crash trace were already gone.
**Fix:** the `#if` is gone. Serial-sink visibility is now two independent
RUNTIME floors composed in `applySerialFloor()` (`src/system/AppLog.cpp`):
muted while serial is the live dedicated TCode transport, demoted to Warn+
once `/api/log` has been served at least once (a human is watching the web
log by then). A sink's existence is never compile-time-conditional on a
flag that means something else.
Bit us: USB serial going permanently quiet with no runtime symptom to chase,
traced back to a transport-selection macro moonlighting as a logging gate.

## T18 — Arrival-time stamping destroys a stream's timeline
**Rule:** never stamp streamed samples with local receive time and then
interpolate/derive against those stamps. Reconstruct the SOURCE's timeline
(re-space by the known/estimated production cadence, future-anchored), or
carry source timestamps on the wire; treat arrival time as a hint only.
**Mechanism:** the network batches — TCP clumps several STATE frames into
one segment, so decode-time `Date.now()` gives them IDENTICAL stamps
followed by a gap. Measured on-device: 71 of 393 motion samples in 12 s
carried a duplicate stamp (p95 arrival gap 90 ms against a ~30 ms true
period). Downstream, a "not newer than the last" guard silently DISCARDED
every duplicate (~18% of all motion), and the interpolator played the
missing span in one frame (snap) then starved to the next clump (freeze):
107 snap frames + 17 multi-frame freezes in 719 rendered. No interpolation
upgrade can survive garbage timestamps — a Hermite pass shipped first and
changed nothing visible, which is itself the diagnostic: when smoothing
math does not help, question the time base, not the curve.
**Corollary:** any cadence ESTIMATOR feeding the reconstruction must learn
only from plausible streaming gaps — idle/shed/dwell gaps are mode
switches, and one 600 ms gap taught the estimator a garbage period whose
first post-resume spans rendered as a one-tick wrong position.
**Fix:** `webui/src/ui/hero/telebuf.js` `push()` — timestamps
reconstructed (max(arrival + lead, prev + EMA period), capped, monotonic,
never dropping a sample); EMA gated to gaps < min(4×period, 200 ms);
>500 ms gap resyncs the schedule. Burst-replay + dwell/resume regression
tests in `webui/test/telebuf-sim.mjs`.
Bit us: the rail marker "jitter" that survived a whole interpolation
rewrite, then the "random position for one tick on first movement" residual
— both the same trap wearing two coats.

## T19 — Split-plane starvation: the pre-allocated plane stays healthy while the allocating plane dies
**Rule:** under memory pressure a hub must REFUSE NEW LOAD (close new
sessions early, 503 heavyweight serves) and must never let "one plane looks
fine" pass for health — the plane that allocates per-request starves first
while the plane running on pre-allocated buffers keeps humming. And a
system with no persisted last words cannot be debugged after it dies:
keep a crash ring.
**Mechanism:** N concurrent WS sessions plus repeated ~270 KB LittleFS page
serves ground internal heap to a 60-BYTE low-water mark (post-init headroom
is only ~32 KB). SlopSync STATE delivery stayed PERFECT throughout (25 Hz,
zero gaps — its buffers pre-exist), while httpTask's per-request
allocations crawled page loads to 5-8 s; the episode ended in a PANIC
reboot with no serial attached and nothing persisted — the reset reason was
the entire post-mortem. The healthy WS plane actively MISLED diagnosis
("the link is fine, so the device is fine").
**Fix:** fw 2.1.87 — WS accept refuses below free<14336 or maxblock<6144
(before slot claim; existing sessions untouched), page serve answers 503
below maxblock<12288, and `CrashRing` (RTC_NOINIT, `include/system/
CrashRing.h`) persists boot seq, heap watermarks, and breadcrumb
checkpoints across panic reboots, surfaced in AppLog and GET /api/crash.
A true backtrace still needs a core-dump partition — partition tables do
not OTA (queued bench reflash).
Bit us: the 2026-07-29 wedge-then-PANIC (second unexplained PANIC on
2.1.86), diagnosed only from heap beacons and the arrival pattern because
nothing else survived the reboot.
**Addendum — the floor deadlocks against ghosts (fw 2.1.88):** the accept
floor fires BEFORE HELLO processing, but a silently dead peer (locked
phone, killed tab — no FIN) never reads as stale at the transport level:
it holds its slot and heap forever, and the ghost-held heap refuses the
very connect whose HELLO slot-pressure path is the only other evictor.
A refuse-new-load floor MUST be paired with a transport-level idle-RX
reap (WS: kWsIdleReapMs, ten missed proof-of-life PINGs) or the floor
becomes the deadlock. Ledger's FW 2.1.88 entry has the live proof.

## T20 — A hand-copied vocabulary drifts silently, and a RETIRED one lies

**Mechanism:** when two languages consume one registry and only one of them
generates its constants, the hand-written side has no failure mode that looks
like failure. A wrong wire NUMBER crashes or NACKs; a wrong wire NAME renders.
The copy compiles, the session goes LIVE, the page draws, and the only symptom
is a label nobody cross-checks against the source of truth. Drift accumulates
one skipped registry addition at a time, and every skip is individually
invisible.

Retiring a vocabulary converts that lag into an active lie. `setting_categories`
was tombstoned in registry.yaml and succeeded by `ui_categories` on the same
wire key (10), with a different base (1, not 0) and 14 entries instead of 5.
The generated C++ side followed. `clients/js/frames.js` kept the old 5-entry
0-based array, so from the moment the firmware emitted the new vocabulary
(RFC-047 Phase C2), EVERY settings tab in EVERY JS client was mislabeled:
category 2 `motion` drew as "Limits", 4 `limits` as "Diagnostics", and 5..14
resolved to `undefined` and fell back to "Category 5". Nothing errored. The
UI looked finished.

The census matters more than the one bug: diffing all 24 hand tables against
the registry found SEVEN drifted — missing frame types, three missing NACK
codes surfacing as raw numbers, missing CBOR keys, missing session-event
kinds, and `LIMITS` carrying 16 of 68 entries. Not one had been noticed.

**Fix:** RFC-052(c), SlopSync `42c7299` — `tools/gen_registry_header.py` emits
`clients/js/generated/registry_vocab.js` alongside the C++ header, committed
for the same reason (browsers import `clients/js` directly, so there is no
build step to generate it on demand), with `--check` covering both artifacts.
The hand tables are gone; what legitimately stays hand-written is named and
justified in the file banner, because "this one is fine to transcribe" is the
belief that produced all seven.

**The rule:** a vocabulary with more than one consumer language gets a
generator and a staleness gate, or it gets one consumer. Never a generator on
one side and a comment saying "transcribed from" on the other. And when a
vocabulary is retired, DELETE its identifiers rather than aliasing them onto
the successor — a working alias is how this survived from Phase C2 to now.
Bit us: 2026-07-29, found by the authoring-legibility campaign's Phase 1a
while looking for something else entirely.
