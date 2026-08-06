---
paths:
  - "**"
---

# SlopLog and SlopGlow (NON-NEGOTIABLE usage)

Self-contained ecosystem modules: hardware-free core plus thin Arduino glue,
vendorable to C5 nodes, header-only via explicit `-I lib/<name>/include`.

## Logging goes through SlopLog. Only.

- `SLOGT/D/I/W/E/F("tag", fmt, ...)` from any task, either core. Bounded
  format plus spinlock slot copy; never blocks, never allocates, NOT ISR-safe.
- Throttle with `SLOGx_EVERY_MS`. Compile floor `SLOPLOG_COMPILE_LEVEL`.
- ONE drain point: `applogDrain()` in httpTask. Sinks implement
  `sloplog::ISink`, registered in `applogBegin()`. `src/system/AppLog.cpp` is
  ONLY the sink and bridge.
- **No `Serial.print` debug output. No new log macros.** WebUI JS is exempt.
  Enforced by canon_lint `serial-print`.
- **Boot lifecycle:** `applogBegin()` immediate-drains during single-task
  `setup()`; main.cpp disables that before task creation; the first `/api/log`
  serve demotes serial to Warn and above.

## LEDs go through SlopGlow. Only.

- Callers speak semantics: `slopglowEngine().raise/clear/set(GlowState::X)`.
  Board wiring lives in `src/system/SlopGlowBoard.cpp`.
- **Never `digitalWrite` or `ledcWrite` an LED anywhere else.**
- **The LED liveness gate is a safety feature.** Animation advances only while
  every registered heartbeat pulses (motorTask on Core 1, commsTask on Core 0;
  the pump runs on httpTask). Never defeat it. Frozen LEDs are a diagnostic.

## T6 -- log sinks must never block

**Rule:** any log sink is non-blocking by contract: drop-and-count when the
output is full. Never add a sink that can wait.
**Mechanism:** USB-CDC serial with no host attached blocks ~100 ms per line in
the TX-full path, on whatever task drains the log ring (httpTask). One chatty
subsystem then freezes HTTP serving with zero CPU load visible.

## T7 -- LED freeze is a diagnostic, not a bug

**Rule:** never "fix" static LEDs by moving the SlopGlow pump or removing a
heartbeat pulse.
**Mechanism:** it is a liveness gate. The pump runs on httpTask; heartbeats
come from motorTask (Core 1) and commsTask (Core 0). Frozen LEDs plus live
cores means httpTask is blocked, see T6. This distinction has solved a field
incident; preserve it.

## T15 -- fixed-priority status displays can mask a lower-priority, time-critical state

**Rule:** when a single highest-active-state-wins display picks ONE thing to
show, rank by TIME-SENSITIVITY (what is gone if missed right now), not by
severity. A persistent, rediscoverable condition may correctly rank BELOW a
narrow window a human must catch immediately.
**Mechanism:** `GlowState` shows exactly one state, the highest-priority
active one. `Fault` originally outranked `Pairing`, and `Fault` fires whenever
the machine is simply unhomed (`SlopGlowBoard.cpp`:
`!state.homed && !state.homing_in_progress`), the ordinary state of a fresh
boot rather than a hardware failure. RFC-027's push-to-pair window, opened by
triple power-cycling a factory-fresh and therefore UNHOMED device, landed on
exactly the device that was ALSO showing Fault, and Fault won: red breathing
instead of the pairing invitation, hiding a 120 s gone-if-missed ceremony
behind a condition still true the next time anyone looks.
**Fix:** `GlowState` reordered so Pairing outranks Warning and Fault, still
below Ota, an active flash, and Estop, which are never allowed to be masked.
See the `GlowState` ordering comment in
`lib/slopglow/include/slopglow/slopglow_core.hpp`.

## Diagnostics are a DUMP, not a stream (operator ruling 2026-08-06)

Depth beats liveness. The failure that matters is "the ring recycled before
anyone read it", not "the ring is not fast enough", so the S3 records into a
megabytes-deep PSRAM archive and it is read ONCE, after something goes wrong,
over HTTP. `/api/diag` is the whole archive; `/api/diag/<tag>` filters to one
SLOGx tag, so a new subsystem gets a route by logging under a new tag and the
route table never changes.

- **Three rings, three jobs, and they do not merge.** `/api/log` is the small
  severity-partitioned DISPLAY (60 lines, a Debug flood can never bury an
  error). `/api/diag` is the flat ARCHIVE. `/api/crash` is the RTC_NOINIT ring
  that survives a panic. Every property of the display ring is a consequence of
  being small; scaling it instead of adding a second one would have meant a
  multi-MB memcpy under a spinlock and a multi-MB `String`.
- **The archive dies with its boot.** PSRAM is re-allocated at `applogBegin()`.
  Post-panic forensics stays crashring. Never cite `/api/diag` as a
  post-reboot instrument.
- **The writer NEVER stops for a reader; there is NO freeze.** Slots carry a
  monotonic seq; `DiagRead` detects being lapped and truncates WITH A FOOTER
  NOTICE instead of being protected. A read can be a remote pager over the C5
  bridge taking minutes, and any gate held that long has a remote owner -- the
  latched-gate class (sd-emy) that has bitten this project twice. Never
  reintroduce a freeze, a lock, or any reader-owned state on this ring.
- **The footer's `next=<seq>` is the resume cursor** and it is the LAST line of
  every dump, on purpose: machine consumers parse it there and pass it back as
  `?from=`. This is what lets a pager (the C5 relay, a browser fetch loop) pull
  the archive in bounded requests instead of one held-open stream, and it makes
  incremental tailing free. Verified live 2026-08-06: full dump -> `next=64`,
  `?from=64` eleven seconds later returned exactly the 2 new lines.
- **Blocking during a full local dump is ACCEPTED, not a defect.** It owns
  httpTask until it finishes. This is the instrument reached for when the
  machine is already unwell, so it carries no heap floor and no mid-body abort:
  it must not be the first thing memory pressure switches off.
- **This is why the C5 does NOT need a SlopSync CBOR decoder.** The S3 owns the
  archive and its format; the C5 forwards bytes and stays a bridge. Later, the
  handful of diagnostics worth watching LIVE become ordinary catalog elements
  read through SlopSync. Shipping the whole archive over SlopSync would clog
  the plane that carries motion, which is the thing the dump exists to avoid.

## T17 -- a flag reused across unrelated concerns can silence a sink for good

**Rule:** gate a sink's existence at RUNTIME, not compile-time, and never let
a flag whose stated job is something else (transport selection, a feature
toggle) also decide whether a diagnostic sink is registered at all.
**Mechanism:** the serial log sink was once wrapped in
`#if !SERIAL_CONTROL_MODE` inside `applogBegin()`, but that macro's actual job
is picking the factory-default transport, not gating diagnostics. At the
macro's normal value the `#if` never registered the serial sink at all: not
throttled, not floored, ABSENT, for the entire life of the build, regardless
of anything happening at runtime. Every `SLOG*` call still went out over the
web ring, so nothing looked broken from the firmware's own side; only a human
watching USB serial would notice, by which point the boot banner and any early
crash trace were gone.
**Fix:** the `#if` is gone. Serial-sink visibility is two independent RUNTIME
floors composed in `applySerialFloor()`: muted while serial is the live
dedicated transport, demoted to Warn and above once `/api/log` has been served
at least once. A sink's existence is never compile-time-conditional on a flag
that means something else.
