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
