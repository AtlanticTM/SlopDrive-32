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
**Addendum — a refusal floor is a LOAD SHEDDER, and its number is not
a sizing number (fw 2.1.92 -> 2.1.94, A/B'd live 2026-07-29):** the page
floor reads `maxblock < 12288` while the largest allocation that path can
make is 1360 B, so the arithmetic invites "correcting" it downward. Doing
so is what the A/B measured. Control (old floor) and candidate (floor sized
to the real allocation) were driven by the same harness — three concurrent
browsers, repeated — and BOTH bottomed near 250 B free. The control
survived: refusing stopped the bleeding, and it wedged. The candidate kept
serving through the same hammer and PANICked. The floor's value is not
"how much this request needs", it is "how much concurrent work the hub will
still admit", and shedding is the only backpressure a single httpTask has.
Lowering one is a concurrency-limiting question, never a threshold tweak.
What IS safe to change is WHAT the floor gates: a 304 revalidation sends no
body and allocates nothing, so gating it merely refuses the browsers that
already hold the bundle — the cheapest population to serve. Measured under
live pressure: 55 fresh loads 503'd while all 55 revalidations answered 304,
device never panicked.
**Resolution — shed DURING the work, not only before it (fw 2.1.95):** the
two failures live on different timescales, which is why one gate could not
serve both. Fragmentation is a latched fact visible at ENTRY; exhaustion is
a transient that arrives WHILE several bodies are in flight, after entry has
already said yes — three concurrent first-loads pass a check at ~36 KB free
and reach ~250 B afterward, so no entry threshold can predict it. Splitting
them fixes both: entry asks only "can an internal allocation happen at all"
(`maxblock >= 4096`, the true ceiling under
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`) plus a cheap free floor, and the
SEND LOOP re-checks free every chunk and abandons the body when it collapses.
Truncation is a page the browser retries; the alternative was a reboot.
Live A/B/C: the harness that panicked the entry-gate-only build ran twice as
hard against 2.1.95 with no reboot, the abort fired at free=11,572 and again
at maxblock=2,548, and a fresh load returned 200 at maxblock 10,740 — the
exact value that used to latch a permanent 503. Do NOT turn the abort into a
wait: httpTask is the task under pressure, so blocking there starves the one
thing that has to finish draining.

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

## T21 — Free heap is not contiguous heap, and a high-water mark only knows the paths it has walked

Two measurement traps, one family: the number that is easy to read is not the
number that decides anything.

**Fragmentation, not exhaustion.** `handleRoot` needs ONE contiguous ~12 KB
block to stream the bundle. An allocator can hold 23 KB of total free space
split into pieces whose largest is 11 KB — and then zero pages serve, forever,
while every "free heap" reading looks survivable. Measured live on fw 2.1.88:
`free=23408 maxblock=11252` against a 12288 floor, with the observed `maxblock`
CEILING at 12276 — twelve bytes under. 5/5 page loads returned 503 and it read
as a hard failure rather than a flaky one. **Watch `maxblock`; `free` is the
comforting lie.** The 10 s beacon prints both for this reason.

The corollary bit us in the fix, too: shrinking an oversized task stack from
16 KB to 8 KB returned **11,196 B**, not the 8,192 B of arithmetic — a big
allocation costs its own size *plus* the hole it leaves. Oversized allocations
are a fragmentation source, not just a size problem.

**A high-water mark is only as good as the workload since boot.**
`uxTaskGetStackHighWaterMark` reports the deepest point actually reached, which
on an idle machine means "nothing interesting ran yet." The Sampler task showed
1,152 B of 16,384 used — a tempting 15 KB — measured on an un-homed bench boot
with the motor unplugged, i.e. with its entire reason for existing (streaming
through the planner) never executed. Worse, `httpTask`'s deepest path is OTA,
whose peak is **unobservable by construction**: the flash is followed by a reboot
that resets the mark. Sizing either from that census would panic mid-session
weeks later, and the canary names the task but not the day you caused it.

**Fix:** fw 2.1.90 — `dumpTaskStacks()` re-scans every 30 s and reports ONLY
tasks that have gone DEEPER than last reported, so a bench session that
exercises motion names the stacks it actually grew. Trim only after a
representative workload has produced no new regressions, and NEVER trim a stack
whose comment records a previous canary blowout (T1) — the comment knows the
worst case, the census only knows what it has seen.
Bit us: 2026-07-29, chasing a recurring 503 that four earlier mitigations had
aimed at the wrong number.

## T22 — A sticky offset is measured from a DIFFERENT box depending on who scrolls

**Rule:** when fixed chrome reserves its height as a container's padding, the
sticky bars inside that container must NOT restate the reserve as their own
`top` — unless the page, not the container, is the scrollport. The correct
offset is not a property of the layout; it is a property of *which element
scrolls*, and this codebase has one of each at the 960px breakpoint.
**Mechanism:** a sticky element's offset is resolved against the nearest
scrollport **inset by that scroll container's padding** (CSS Position §6.3).
Mobile: the page scrolls, the scrollport is the viewport, its padding is zero,
so `top: <chrome>` parks the bar correctly below the chrome. Desktop: `.app`
is the scroll container (height-capped flex column, `overflow: hidden` — which
still establishes a scrollport), so the constraint rectangle already starts at
`.app`'s CONTENT box, below the padding that reserved the chrome. The same
`top: <chrome>` then insets a second time and the bar lands at exactly twice
the chrome height. Both modes read as correct in code review; only one is.
**Bit us:** the Tauri desktop shell shipped its bar at
`top: var(--linkbar-h)` while `.app` separately reserved
`--shell-chrome-top`, two offsets measured from different origins. The bar
landed over the LinkBar's lower half, drew BEHIND it (z-index 18 vs 20), and
left dead space above and below — the exact symptom the operator reported.
The first fix then reintroduced the doubling on desktop via the sticky path.
**Fix:** one reserve (`.app` padding-top), chrome pinned at `top: 0`, and the
sticky offset restated ONLY in the mode where the page is the scrollport.
`webui/test/shell-chrome-geometry.test.mjs` asserts flush-stacking in both
modes with no device present (`npm run check:shell`).
**Companion:** exactly one bar may absorb `env(safe-area-inset-top)`. The
topmost one owns it via `--chrome-inset-top`; two bars padding for the same
notch is the same double-gap bug wearing a phone.

## T23 — A synchronous read inside an effect is a SUBSCRIPTION; the async callback next to it is not

**Rule:** in a reactive effect that installs a timer/observer and keeps state
across ticks, every reactive value the effect body touches SYNCHRONOUSLY must
be read through `untrack()`. Seeding a local from reactive state is a
subscription, and re-running the effect re-runs its initializers.
**Mechanism:** Svelte 5 records dependencies during the effect's synchronous
execution. Reads from a `setInterval`/`ResizeObserver`/`requestAnimationFrame`
callback happen outside that window and register nothing. So the two halves of
the same function behave oppositely: `tick()` called once at the bottom of the
effect subscribes to everything it touches, while the identical `tick()` fired
by the interval subscribes to nothing. When the subscribed value updates at
telemetry rate, the effect tears down and re-runs tens of times a second —
re-executing `let data = []` and every buffer fill above it. The timer keeps
running and the drawing keeps happening, so the feature looks ALIVE. Only the
accumulated history is gone.
**Bit us:** LinkBar's activity heatmap. Two synchronous reads leaked —
`lastPushes = machine.stats.statePushes` in the body, and a first-paint
`tick()` reaching `machine.samples` through `sampleFrac()`. At ~25 Hz the
14-column history was refilled with zeros before it could ever fill, so every
column but the newest painted empty. Read as "the grid does not scroll left",
and the scroll was never the broken part. Two confident diagnoses (reduced
motion, then genuinely-flat idle data) were both wrong; the operator's
screenshot — all gray but the rightmost column — was the actual evidence,
because "exactly one live column" is the signature of a per-tick wipe.
**Fix:** `untrack()` on both reads. The guard counts columns sitting at the
v=0 baseline alpha and fails above 2 (`flagship-render-smoke`), because the
bug's signature is 13-of-14 empty and any threshold on "distinct values" would
have passed the broken version.

## T24 — A control's own box is not the box you can see

**Rule:** never derive geometry from an interactive element's rect without
checking what the UA and the design system did to that rect first. Measure the
rendered box; do not reason about it from the stylesheet.
**Mechanism:** two independent ways the box lies. (1) The UA gives `button` a
default `padding: 1px 6px`. Under `box-sizing: border-box` on a small fixed
control that padding is subtracted from the INSIDE: an 18px button keeps a
4px-wide content box. A grid item wider than its own track cannot be centered
in it — the overflow has nowhere symmetric to go, so alignment resolves to
start and the item is pinned to the content edge, overflowing one side only.
`place-items: center` is then present, correct-looking, and doing nothing.
(2) A styled `input[type=range]` is `height: 2px` — its box is the hairline
TRACK. The thumb is `::-webkit-slider-thumb`, a pseudo-element that overflows
the box entirely, so the input's rect excludes the very part the operator
grabs.
**Bit us:** the field info button drew its glyph 3.5px right of center — a
whole quarter of the control — while the CSS said `place-items: center` and the
padding was never mentioned. Two diagnoses were argued from the stylesheet
first (glyph ink balance, then a half-pixel raster offset); both were wrong,
and the second was worth 0.18px at that size, i.e. invisible. One
`getBoundingClientRect()` comparison found it immediately. Then the same
feature's echo outline, cropped to a slider's rect, produced a 10px box around
a "2px control" and would have left the handle outside the outline meant to
enclose it.
**Fix:** `padding: 0` on the icon button; the echo expands by half
`--slider-thumb-h`, which style.css now defines ONCE and the thumb rules read,
so the outline and the thing it encloses cannot drift. Guards assert a
non-zero box BEFORE trusting an offset — a `display: none` element reports an
all-zero rect, which had made the centering assertion pass while measuring
nothing.

## T25 — An unregistered custom property animates as a discrete swap

**Rule:** a custom property driving an animation or transition must be
declared with `@property` (a real `syntax`, not `*`). Without it the value is
an untyped token and interpolation is impossible.
**Mechanism:** CSS custom properties are substituted as raw token streams.
With no registered `syntax` the engine cannot know `0%` and `150%` are
lengths, so it falls back to discrete interpolation: the value flips at the
keyframe boundary instead of sweeping. Registration gives it a type, an
initial value, and `inherits: false` — after which it interpolates like any
other animatable length.
**Bit us:** the intent echo's wavefront radius. The failure mode is the
dangerous kind — the animation still plays, the timing is right, and the shape
jumps from nothing to fully-expanded, which at 500ms reads as "a slightly
janky pulse" rather than as a bug with a name.
**Fix:** `@property --pr { syntax: '<percentage>'; inherits: false;
initial-value: 0% }` in style.css. The guard samples the property MID-FLIGHT
and requires a real intermediate radius (measured 157.495% of a 165% sweep);
asserting only that the animation is running would have passed the broken
version.

## T26 — Canceling a blocked task from outside only works if it is blocked where you think

**Rule:** before canceling another task's work from a watcher task, PROVE
where the target is blocked. If the mechanism is "wake the blocked syscall",
and the target is not in that syscall, the cancel does nothing — and the
attempt is not free.
**Mechanism:** `shutdown(fd, SHUT_RDWR)` wakes a task parked in `read()` on
that fd. It does nothing for a task spinning in a poll loop, and it can make
things WORSE: a socket left in an error state can turn a
`while (available())` loop that used to yield into one that returns
immediately forever, so the task stops yielding at all and starves the idle
task it shares a core with. The cancel then CAUSES the watchdog reboot it was
written to prevent.
**Bit us:** 2026-07-31, fw 2.2.3. A Core-0 beacon let `commsTask` cancel any
`httpTask` step blocked >800 ms, aimed at half-open HTTP requests. Measured
result, from the device's own log:
`Core-0 step http:ui.update blocked >800ms - canceled client fd=57` followed
by `http:ui.update blocked 10015ms` — the cancel did not unblock it — then the
same fd re-reaped every ~800 ms forever, then `reset_reason TASK_WDT`. It also
canceled every legitimate page load and every OTA upload, so the device could
no longer be flashed over the network and needed a COM11 serial rescue. Three
regressions from one unverified assumption.
**Fix:** the reap is disabled (`kCore0ReapMs = 0`, main.cpp, with the log
excerpt inline). The general lesson is the reusable part:
* **A deadline cannot separate slow from stuck when they overlap.** Measured
  on this device: legitimate full page serve **9 s** (302 KB bundle), stuck
  half-open client **10 s**, OTA upload seconds. Any threshold that catches
  the stall also kills the page and the flash. This is exactly why Apache's
  `mod_reqtimeout` gates on a byte RATE (`MinRate=500`) rather than a
  deadline — progress is separable, elapsed time is not.
* **A one-connection-at-a-time server has no fix at this layer.** The
  Arduino sync `WebServer` serves a single client and every long operation
  owns the whole plane. The answer is an event-driven server, not tuning.
* **Co-deployed changes prove nothing individually.** The 16-socket test that
  showed "zero reboots" could not distinguish the reap from the watchdog
  raise that shipped with it. Claiming the reap worked was unfounded; it was
  actively harmful. Change one thing, or attribute nothing.

## T27 — Instrumentation that is expensive enough to change what it measures

**Rule:** a diagnostic on a hot path is judged by its CALL RATE, not by its
correctness. Before adding a probe, multiply its cost by how often the
enclosing function actually runs. If the product is a meaningful fraction of
the budget of the task it sits on, the probe will manufacture a different
failure and hide the one you are hunting.
**Mechanism:** `heap_caps_check_integrity_all()` walks every block header in
every region — milliseconds. That is nothing once per session and fatal once
per frame. A probe that starves its own task produces a watchdog reboot, and a
watchdog reboot looks like a finding. You then "discover" a bug you created.
**Bit us, twice in one session (2026-07-31, hunting THE QUEUE item 0):**
1. Probes placed in `SlopSyncAsyncWsPort::loop()`, which runs every 5 ms. The
   hub task saturated, the device went intermittently unreachable, OTA could
   not get through, and recovery needed a COM11 serial rescue (fw 2.3.0).
2. A probe at `onEvent()` ENTRY — which fires for `WS_EVT_DATA`, i.e. once per
   FRAME. ~56 000 whole-heap scans in a single test run. It watchdogged the
   AsyncTCP task before the corruption could ever be observed, and turned a
   reproducible PANIC into a TASK_WDT, i.e. it destroyed the evidence (2.3.1).
**Fix:** probes live on per-SESSION paths only (connect / refuse / disconnect)
and `loop()`'s is rate-limited to 1 Hz. Both are in
`src/comms/SlopSyncAsyncWsTransport.cpp` behind `SLOPSYNC_HEAP_BISECT`.

## T28 — Things that do NOT find a heap-corruption culprit (do not re-try these)

**Rule:** this list is negative knowledge, bought at the cost of a full session
and two serial rescues. Before reaching for any of it as a "fix" for the
corruption in LEDGER THE QUEUE item 0, read why it already failed.

**1. Raising the task watchdog does not fix corruption.** It was raised 5 -> 12 s
for a real and separate reason (T26: the sync WebServer's own 5 s ceiling). It
buys time for a stall; it has nothing to do with a bad write, and a corrupted
free list will abort regardless of how patient the watchdog is.

**2. Canceling a blocked task from outside is not a fix, it is a second bug.**
Full mechanism in T26. It did not cancel the block, it killed legitimate page
loads and OTA uploads, and it CAUSED the watchdog reboot it was written to
prevent.

**3. Refusing connections at capacity does not fix corruption — and the refusal
already exists.** `SlopSyncAsyncWsPort::onEvent`'s `WS_EVT_CONNECT` already
closes a client when no slot is free (`kSlots` = `kHubMaxSessions + 1` = 5).
Admission control bounds how OFTEN the bug is reachable; it does not remove the
bad write. Worth doing for RFC-055 reasons, never as the corruption fix.

**4. `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` is NOT the cause. Do not disable it
again. RE-TESTED 2026-07-31 ON 12 TRIALS AND IT STILL IS NOT.** Six runs per
arm of `isolate.py many 12`, load verified on every single run (12/12 sessions
accepted, 39 000-126 000 frames), verdict by `boot_seq`:

| arm | option | runs | reboots | `heap_min` at crash |
|---|---|---:|---:|---|
| A | **ON** (fw 2.3.18) | 6 | 6 | 19 455 - 27 083 |
| B | **OFF** (fw 2.3.17) | 6 | 4 | 3 219 - 7 259 |

Arm A is the load-bearing row: it reboots with **19-27 KB still free**, which
rules out exhaustion and leaves corruption. Arm B reboots too, and one of its
crashes was `tlsf_check` faulting inside a heap-integrity probe while walking a
damaged free list — corruption, present in both arms. Turning the option off
also drops `int_free` 35 467 -> 27 531 and pushes `heap_min` into the 3-7 KB
band, so the OFF arm is a starved machine as well as a corrupt one.

**METHOD WARNING FROM THIS A/B, worth more than the result:** the first attempt
at arm B reported 7 runs, 0 reboots — a clean pass that was entirely an
artifact of the 9 KB of `.bss` a diagnostic table was holding. Removing the
instrument changed arm B from 0/7 to 4/6. Two lessons: suppress the harness
output and you cannot tell a survived run from a run that never applied load
(check the "12/12 sessions accepted" line every time), and a diagnostic heavy
enough to move `int_free` has already changed the experiment (T27, memory
dimension).

The original reasoning, still valid: It was a well-formed suspect — the crash lands in `remove_free_block`
reached via `wifi_calloc`/`heap_caps_calloc_prefer`, exactly the allocation path
it redirects into PSRAM, and the crash signature changed the day it was enabled
(`heap_min` 316 B genuine exhaustion -> 28-34 KB with plenty free). It was
TESTED: built with it OFF (fw 2.2.8) and the SAME crash reproduced — same task,
same `EXCCAUSE 29`, same frames. Turning it off also costs real headroom
(int_free 41 967 -> 34 039, int_largest 28 660 -> 18 420).

**5. Heap INTEGRITY probes are too coarse to localize this.** Placed on the
per-session WS paths (fw 2.3.2) they never fired, while the reproduction still
panicked. The window between the corrupting write and the allocator tripping
over it is SHORTER than the gap between session-granularity probes — WiFi
allocates continuously in between and always finds the damage first. Making
them finer is barred by T27. Integrity walks answer "is the heap damaged NOW";
they can never answer "who damaged it".

**6. ~~Heap TRACING is wired but produced ZERO records.~~ SOLVED 2026-07-31 —
the cause was a FOURTH sdkconfig option, and the failure was silent by
construction.** `include/system/HeapTrace.h` + `src/system/HeapTrace.cpp`,
`GET /api/heaptrace`. Everything previously checked really was fine: the
generated sdkconfig carries `CONFIG_HEAP_TRACING=y`,
`CONFIG_HEAP_TRACING_STANDALONE=y`, `CONFIG_HEAP_TRACING_STACK_DEPTH=8`;
`heap_trace_init_standalone()` and `heap_trace_start(HEAP_TRACE_ALL)` both
return `ESP_OK`; the endpoint reports `active:true`; and buffer placement was
never it (tried in PSRAM, then in internal RAM — 0 records in both).

**Those three options build the tracing LIBRARY. A separate one connects it to
the ALLOCATOR:**

    CONFIG_HEAP_USE_HOOKS=y

`heap_caps_base.c` records through `CALL_HOOK(esp_heap_trace_alloc_hook, ...)`,
and `CALL_HOOK` is defined as `{}` unless `CONFIG_HEAP_USE_HOOKS` is set. Ours
was `# CONFIG_HEAP_USE_HOOKS is not set`, so every recording site in the
allocator compiled to nothing while every status the tracer could report stayed
green.

**The check that settles this class of question in one command** — ask the
BINARY what it contains, not the config what it intends:

    xtensa-esp32s3-elf-nm firmware.elf | grep heap_trace

It listed `heap_trace_init_standalone`, `heap_trace_start`,
`heap_trace_get_count`, `heap_trace_get` — the API — and **no
`esp_heap_trace_alloc_hook` whatsoever**. A tool whose recorder is absent from
the link cannot record, and no amount of re-reading Kconfig would have said so.
Generalize it: when a subsystem reports healthy and produces nothing, stop
auditing configuration and go looking for the symbol.

**What has NOT been tried and is the strongest remaining move:** a hardware
WATCHPOINT. The ledger deferred JTAG because "a leak that takes hours does not
yield to a breakpoint" — that premise is dead, the corruption now reproduces in
seconds on demand. The S3 has built-in USB-JTAG (`debug_tool = esp-builtin`, no
rebuild needed) and `esp_cpu_set_watchpoint()` can arm one without a debugger
attached. A watchpoint names the writing INSTRUCTION, which is the cause; every
item above is a detector.

## T29 — A callback that can destroy its own caller's object

**Rule:** before a loop calls user code, ask whether that call can free the
object the loop is iterating on. If it can, the liveness check must live
OUTSIDE the object — you cannot ask a freed object whether it is freed. This
applies to every dispatch loop in an async/callback library, ours or vendored.

**Mechanism:** AsyncTCP's teardown is SYNCHRONOUS IN THE CALLER'S CONTEXT.
`AsyncClient::close()` -> `_close()` -> `_tcp_close()` and then, still on the
caller's stack, `_discard_cb(...)`. ESPAsyncWebServer's `_discard_cb` is
`[](void *r, AsyncClient *c) { ((AsyncWebSocketClient *)r)->_onDisconnect();
delete c; }`, and `_onDisconnect()` reaches `_handleDisconnect()` ->
`_clients.erase()` -> `~AsyncWebSocketClient()`. So one `close()` from inside a
callback destroys BOTH the `AsyncWebSocketClient` and the `AsyncClient` before
control returns. AsyncTCP 3.5.0's release notes make this explicit and call it a
potential breaking change ("`abort()` executes... directly within the context of
the caller task/thread, like it was already the case for `close()`").

`AsyncClient::_recv()` iterates a pbuf CHAIN and invokes `_recv_cb` once per
buffer, then touches `this` again — `_ack_pcb`, `_pcb`, `_tcp_recved(&_pcb,...)`
— with no liveness check between iterations.

**Bit us, twice, in the same reproduction (2026-07-31, `isolate.py many 12`,
comprehensive heap poisoning, LEDGER THE QUEUE item 0):**

1. fw 2.3.5, task `async_tcp`, `EXCCAUSE 28`, `vaddr 0xfefeff3a`:
   `_recv` -> `_tcp_recved(&_pcb,...)` -> `tcp_recved` (lwip tcp.c:985). The
   loop read `_pcb` out of an `AsyncClient` that callback #1 had already freed.
2. fw 2.3.6, task `async_tcp`, `EXCCAUSE 28`, `vaddr 0xfefeff4e`:
   `_recv` -> `_recv_cb` -> `_onData` -> `_handleEvent` (AsyncWebSocket.cpp:924).
   Same loop, next buffer, stale `_recv_cb_arg` — this time the freed object was
   the `AsyncWebSocketClient` and the poison was read as its `_server`.

**Read the address, it names the bug.** `0xfefefefe` is ESP-IDF's
comprehensive-poisoning FREE-FILL pattern. A fault at `0xfefefefe + small` is a
pointer READ OUT OF FREED MEMORY and then member-accessed; the offset is the
member. `0xfefeff3a` = poison + `0x3C`, `0xfefeff4e` = poison + `0x50`. This is
the single fastest read in the whole hunt and it needs no debugger — but it only
works because poisoning is on, which is why the diagnostic build earns its cost.

**Fix:** both are LOCAL PATCHes in `lib/asynctcp/` (see its `VENDORED.md`):
* `~AsyncClient()` purges the async event queue UNCONDITIONALLY. Upstream purged
  only via `_close()`, which a client with a null `_pcb` never reaches — and null
  `_pcb` is exactly what `tcp_error()` leaves behind.
* A file-scope `_dispatching_client` that `~AsyncClient` clears and `_recv`
  re-checks after every callback. Out-of-band ON PURPOSE: a flag stored in the
  object would itself be freed memory. `_sent` and `_poll` return immediately
  after their callbacks and need no guard.

A third LOCAL PATCH in `lib/espasyncwebserver/` (`queueLen(uint32_t id)`) closes
the mirror image of this on OUR side: `AsyncWebSocket::client(id)` releases
`_ws_clients_lock` before returning its pointer, so
`client(id)->queueLen()` from the hub task dereferences whatever the AsyncTCP
task has since erased. The transport had three of those, under a header comment
calling "never hold an `AsyncWebSocketClient*`" the most important rule in the
file.

**NOT THE WHOLE STORY, and that is the point of writing it down.** These three
took the reproduction from reboot-every-run to reboot-roughly-one-run-in-four,
and the surviving failure reverted to the ORIGINAL signature — `remove_free_block`
<- `wifi_malloc`, `EXCCAUSE 29`, WiFi as victim, heap not short. A rate change is
evidence a real bug was removed; it is NOT evidence the last one was. Do not
close LEDGER THE QUEUE item 0 on the strength of a quieter reproduction.

## T30 — A diagnostic is code, and it lies in its own characteristic ways

**Rule:** an instrument gets the same scrutiny as the thing it measures, and it
must be judged on THREE axes before any result from it is believed: does it
actually run, does it cost enough to change the outcome, and can it produce the
finding it just produced by accident? Every one of the following cost real time
during the corruption hunt (2026-07-31, LEDGER THE QUEUE item 0).

**1. "Reports healthy" is not "is running". Ask the BINARY.** Heap tracing had
three correct sdkconfig options, returned `ESP_OK` from init and start, and
answered `active:true` — while the allocator never called it, because the
recorder is gated behind a FOURTH option (T28 item 6). One command settled what
a session of config-reading could not:
`xtensa-esp32s3-elf-nm firmware.elf | grep heap_trace`. Symbols present, hook
absent. Generalize: when a subsystem reports healthy and produces nothing, stop
auditing configuration and go looking for the symbol, then for the CALL to it
(`objdump -d --disassemble=heap_caps_free` showed the `callx8` once it was real).

**2. A detector whose cost moves the measurement has already changed it.**
This is T27 in the memory dimension. A 9 KB `.bss` table for the double-free
detector pushed `int_min_free` to **88 bytes** and turned one A/B arm from
4-reboots-in-6 into 0-reboots-in-7. The clean-looking arm was the artifact.
Removing the instrument restored the result. Budget a diagnostic's RAM against
`heap_min` under load, not against total free.

**3. Hardware watchpoints are aligned DOWN, so a naive arm watches the
neighbor.** `esp_cpu_set_watchpoint` demands natural alignment, so arming a
64-byte window on an unaligned block starts it BEFORE the block and covers the
previous heap block's header — which TLSF rewrites on every coalesce. That is
14 false hits in one run, every one landing in `block_absorb` and looking
exactly like a find. Either hand the watchpoint aligned storage or refuse to arm.

**4. Allocator hooks fire AFTER the fact, so pointer-stamp schemes race.**
`heap_caps_free` calls `esp_heap_trace_free_hook` AFTER `multi_heap_free`. In
that window another task can allocate the same block, so the stamp lands on live
memory and that block's next honest free reads as a double free. The
false positive is indistinguishable from the real thing by hit alone — it is
told apart only by the two stacks belonging to unrelated owners. Full mechanism
and the retracted conclusion it produced: LEDGER THE QUEUE item 0.

**5. A diagnostic must not be able to brick the recovery path.** The
double-free detector called `abort()` on detection. The OTA upload path tripped
it, so the device rebooted mid-flash and could no longer be updated over the
network — six attempts, including four timed into the first seconds after boot.
Recovery took a COM11 serial rescue. **A detector reports; it does not
adjudicate.** Aborting also asserts a precision the tool did not have (see 4).

**6. Suppress the harness output and you cannot tell "survived" from "never
ran".** An A/B arm reported six clean runs while the load line was piped to
`Out-Null`. With `int_free` near the WS accept floor, refused sessions look
exactly like a passing test. Assert the load landed — for `isolate.py` that is
the `12/12 sessions accepted` line and a frame count — on EVERY run, not once.

**7. `custom_sdkconfig` cannot express "off" as `=n`.** pioarduino substitutes
the literal text into sdkconfig and Kconfig rejects `=n` for a bool; the build
dies in `idf_build_process` with a CMake error that names neither the option nor
the line. Comment the line out instead — the framework default is what you get.
Related: editing that block wipes and reinstalls framework packages, which has
twice left `managed_components/espressif__cjson/cJSON/` without its sources and
once needed a second invocation to reconfigure. A failed first build after a
`custom_sdkconfig` edit is not necessarily a real failure; run it again before
diagnosing.
