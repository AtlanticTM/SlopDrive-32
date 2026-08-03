---
paths:
  - "lib/sloplog/**"
  - "lib/slopglow/**"
  - "src/system/AppLog.cpp"
  - "src/system/SlopGlowBoard.cpp"
  - "src/**"
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
