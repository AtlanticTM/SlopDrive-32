---
paths:
  - "src/**"
  - "include/**"
  - "platformio.ini"
---

# Memory discipline

## Pools, never per-event malloc

- Steady-state comms allocate nothing: PacingRing is a fixed
  std::array<PacingEntry, 64> (include/comms/SlopSyncHubService.h:91-143);
  UART RX ring is FrameBuffer[64] per slot sized for burst tolerance, cost
  landing in PSRAM (include/comms/SlopSyncUartTransport.h:143-165);
  cross-core rings in SystemState are fixed-capacity SPSC with a
  never-blocks-never-allocates producer contract.
- Big scratch lives in members, not stack locals: TX scratch as members
  because ~1 KB of stack deep inside hub.update() is not free (T1 class,
  SlopSyncUartTransport.h:202-206); ledger/preset blobs as members of the
  PSRAM-resident service (SlopSyncHubService.h:526-542).

## PSRAM vs internal RAM

- SlopSyncHubService is placement-new'd into PSRAM from main.cpp. Never move
  it back to BSS (DOCTRINE §9, TRAPS T2: a ~100 KB static once left ~13 KB
  free heap and killed the network stack). Measured: service 240,976 B in
  PSRAM; internal-RAM cost ~101 KB (docs/c5-comms-offload.md §4.1).
- CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP redirects WiFi/lwIP bulk to PSRAM;
  exonerated as a corruption cause by A/B but load-bearing for headroom
  (int_free 41,967 vs 34,039). CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
  is the guard against internal-only allocations aborting; that abort was
  the literal 2.1.99 core dump (platformio.ini:354-389).
- PSRAM is unreachable while flash cache is disabled (OTA/flash writes):
  nothing ISR-context may touch the PSRAM-resident service
  (SlopSyncUartTransport.h:47-52).

## Budgeting rules proven by blood

- Budget any diagnostic's RAM against heap_min under load, never idle free:
  a 9 KB .bss diagnostic table flipped an A/B arm from 4-reboots-in-6 to
  0-in-7 and separately pushed int_min_free to 88 bytes (T28 item 4, T30
  item 2). A detector reports, never adjudicates: a detector's own abort()
  once bricked the OTA recovery path (T30 item 5).
- Oversized allocations cost their size plus the hole they leave: shrinking
  a 16 KB stack to 8 KB returned 11,196 B (T21). But never trim a stack
  whose comment records a canary blowout (T1).
- Heap-corruption hunt state: dev-board issue sd-wzu, parked by operator
  ruling at fw 2.3.20. All first-party suspects eliminated; signature
  survives. Read that issue before touching teardown paths.

## "32D conformance floor": does not exist

"32D" in this workspace means the ESP32-WROOM-32D, a PARKED motion-MCU port
idea (no PSRAM; would force a hub-placement rework). The protocol's real
floor is min_transport_payload = 242 B (SPEC §13.1). Never cite a "32D RAM
budget"; there is none.

## T2 -- big statics starve the internal heap

**Rule:** services with large state live in PSRAM via placement-new from
`main.cpp`, never as ordinary statics or BSS.
**Mechanism:** BSS eats internal SRAM at link time, and the WiFi stack plus
page serving allocate from the same internal heap at runtime. A ~100 KB static
left ~13 KB free heap and killed the network stack. Watch the boot heap beacon
(`[sys] heap free/min/maxblock psram`).

## T21 -- free heap is not contiguous heap, and a high-water mark only knows the paths it has walked

Two measurement traps, one family: the number that is easy to read is not the
number that decides anything.

**Fragmentation, not exhaustion.** `handleRoot` needs ONE contiguous ~12 KB
block to stream the bundle. An allocator can hold 23 KB of total free space
split into pieces whose largest is 11 KB, and then zero pages serve, forever,
while every free-heap reading looks survivable. Measured live on fw 2.1.88:
`free=23408 maxblock=11252` against a 12288 floor, with the observed `maxblock`
CEILING at 12276, twelve bytes under. 5 of 5 page loads returned 503 and it
read as a hard failure rather than a flaky one. **Watch `maxblock`; `free` is
the comforting lie.** The 10 s beacon prints both for this reason.

The corollary bit us in the fix too: shrinking an oversized task stack from
16 KB to 8 KB returned **11,196 B**, not the 8,192 B of arithmetic. A big
allocation costs its own size PLUS the hole it leaves. Oversized allocations
are a fragmentation source, not just a size problem.

**A high-water mark is only as good as the workload since boot.**
`uxTaskGetStackHighWaterMark` reports the deepest point actually reached,
which on an idle machine means nothing interesting ran yet. The Sampler task
showed 1,152 B of 16,384 used, a tempting 15 KB, measured on an un-homed bench
boot with the motor unplugged, that is, with its entire reason for existing
never executed. Worse, `httpTask`'s deepest path is OTA, whose peak is
UNOBSERVABLE BY CONSTRUCTION: the flash is followed by a reboot that resets
the mark. Sizing either from that census would panic mid-session weeks later,
and the canary names the task but not the day you caused it.
**Fix:** `dumpTaskStacks()` re-scans every 30 s and reports ONLY tasks that
have gone DEEPER than last reported, so a bench session that exercises motion
names the stacks it actually grew. Trim only after a representative workload
has produced no new regressions, and NEVER trim a stack whose comment records
a previous canary blowout (T1).

## T19 -- split-plane starvation: the pre-allocated plane stays healthy while the allocating plane dies

**Rule:** under memory pressure a hub must REFUSE NEW LOAD (close new sessions
early, 503 heavyweight serves) and must never let "one plane looks fine" pass
for health. The plane that allocates per-request starves first while the plane
running on pre-allocated buffers keeps humming. A system with no persisted
last words cannot be debugged after it dies: keep a crash ring.
**Mechanism:** N concurrent WS sessions plus repeated ~270 KB LittleFS page
serves ground internal heap to a 60-BYTE low-water mark. SlopSync STATE
delivery stayed PERFECT throughout, 25 Hz and zero gaps, because its buffers
pre-exist, while httpTask's per-request allocations crawled page loads to
5 to 8 s. The episode ended in a PANIC reboot with nothing persisted. The
healthy WS plane actively MISLED diagnosis.
**Fix (fw 2.1.87):** WS accept refuses below free 14336 or maxblock 6144,
before slot claim, existing sessions untouched; page serve answers 503 below
maxblock 12288; `CrashRing` (RTC_NOINIT) persists boot seq, heap watermarks
and breadcrumb checkpoints across panic reboots, surfaced in AppLog and
`GET /api/crash`.

**Addendum, the floor deadlocks against ghosts (fw 2.1.88).** The accept floor
fires BEFORE HELLO processing, but a silently dead peer (locked phone, killed
tab, no FIN) never reads as stale at the transport level: it holds its slot
and heap forever, and the ghost-held heap refuses the very connect whose HELLO
slot-pressure path is the only other evictor. A refuse-new-load floor MUST be
paired with a transport-level idle-RX reap (`kWsIdleReapMs`, ten missed
proof-of-life PINGs) or the floor becomes the deadlock.

**Addendum, a refusal floor is a LOAD SHEDDER and its number is not a sizing
number (fw 2.1.92 to 2.1.94, A/B'd live).** The page floor reads
`maxblock < 12288` while the largest allocation that path can make is 1360 B,
so the arithmetic invites correcting it downward. Doing so is what the A/B
measured. Control and candidate were driven by the same harness, three
concurrent browsers, repeated, and BOTH bottomed near 250 B free. The control
survived: refusing stopped the bleeding, and it wedged. The candidate, sized
to the real allocation, kept serving through the same hammer and PANICked. The
floor's value is not how much this request needs, it is how much concurrent
work the hub will still admit, and shedding is the only backpressure a single
httpTask has. Lowering one is a concurrency-limiting question, never a
threshold tweak. What IS safe to change is WHAT the floor gates: a 304
revalidation sends no body and allocates nothing.

**Resolution, shed DURING the work and not only before it (fw 2.1.95).** The
two failures live on different timescales, which is why one gate could not
serve both. Fragmentation is a latched fact visible at ENTRY; exhaustion is a
transient that arrives WHILE several bodies are in flight, after entry already
said yes. Entry asks only whether an internal allocation can happen at all
(`maxblock >= 4096`, the true ceiling under
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`) plus a cheap free floor, and the
SEND LOOP re-checks free every chunk and abandons the body when it collapses.
Truncation is a page the browser retries; the alternative was a reboot. **Do
NOT turn the abort into a wait:** httpTask is the task under pressure, so
blocking there starves the one thing that has to finish draining.

## T27 -- instrumentation expensive enough to change what it measures

**Rule:** a diagnostic on a hot path is judged by its CALL RATE, not by its
correctness. Before adding a probe, multiply its cost by how often the
enclosing function actually runs. If the product is a meaningful fraction of
the budget of the task it sits on, the probe will manufacture a different
failure and hide the one you are hunting.
**Mechanism:** `heap_caps_check_integrity_all()` walks every block header in
every region, so milliseconds. That is nothing once per session and fatal once
per frame. A probe that starves its own task produces a watchdog reboot, and a
watchdog reboot looks like a finding. You then discover a bug you created.
**Bit us twice in one session:** probes in a `loop()` running every 5 ms
saturated the hub task, made the device intermittently unreachable, blocked
OTA and needed a serial rescue; a probe at `onEvent()` entry fired once per
FRAME, roughly 56,000 whole-heap scans in one test run, and turned a
reproducible PANIC into a TASK_WDT, destroying the evidence.
**Fix:** probes live on per-SESSION paths only (connect, refuse, disconnect)
and any `loop()` probe is rate-limited to 1 Hz.

## T28 -- things that do NOT find a heap-corruption culprit (do not re-try these)

Negative knowledge, bought at the cost of a full session and two serial
rescues. Before reaching for any of it as a fix for dev-board issue sd-wzu,
read why it already failed.

1. **Raising the task watchdog does not fix corruption.** It was raised 5 to
   12 s for a real and separate reason (T26). It buys time for a stall; a
   corrupted free list aborts regardless of how patient the watchdog is.
2. **Canceling a blocked task from outside is not a fix, it is a second bug.**
   Full mechanism in T26 (`.claude/rules/transport.md`).
3. **Refusing connections at capacity does not fix corruption, and the refusal
   already exists.** Admission control bounds how OFTEN the bug is reachable;
   it does not remove the bad write.
4. **`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` is NOT the cause. Do not disable
   it again.** Re-tested on 12 trials. Arm A (ON) rebooted 6 of 6 with
   **19 to 27 KB still free**, which rules out exhaustion and leaves
   corruption. Arm B (OFF) rebooted 4 of 6, and one crash was `tlsf_check`
   faulting inside a heap-integrity probe while walking a damaged free list.
   Corruption is present in both arms, and turning the option off also drops
   `int_free` from 35,467 to 27,531. **Method warning worth more than the
   result:** the first attempt at arm B reported 7 runs and 0 reboots, a clean
   pass that was entirely an artifact of the 9 KB of `.bss` a diagnostic table
   was holding. Removing the instrument changed arm B from 0 of 7 to 4 of 6.
5. **Heap INTEGRITY probes are too coarse to localize this.** Placed on
   per-session WS paths they never fired while the reproduction still
   panicked. The window between the corrupting write and the allocator
   tripping over it is SHORTER than the gap between session-granularity
   probes. Making them finer is barred by T27. Integrity walks answer "is the
   heap damaged NOW"; they can never answer "who damaged it".
6. **Heap TRACING needs a FOURTH sdkconfig option and fails silently without
   it.** `CONFIG_HEAP_TRACING`, `_STANDALONE` and `_STACK_DEPTH` build the
   tracing LIBRARY. `CONFIG_HEAP_USE_HOOKS=y` is what connects it to the
   ALLOCATOR: `CALL_HOOK` is defined as `{}` unless it is set, so every
   recording site compiled to nothing while every status the tracer could
   report stayed green (`ESP_OK` from init and start, `active:true` from the
   endpoint, zero records).
7. **Stopping the HUB TASK from initiating client teardown does NOT fix it.**
   The last first-party suspect. All four hub-task initiators disabled at
   once gave 4 reboots in 6 runs against 6 in 6, which at n=6 is noise
   (Fisher's exact ~0.45), and the surviving crash was byte-identical to
   baseline. The scaffolding was REVERTED rather than left behind a flag:
   disabling the idle reap is a real regression, and dead scaffolding is not
   how this codebase records a tested hypothesis.

**What has NOT been tried and is the strongest remaining move:** a hardware
WATCHPOINT. The corruption reproduces in seconds on demand, the S3 has
built-in USB-JTAG, and `esp_cpu_set_watchpoint()` can arm one without a
debugger attached. A watchpoint names the writing INSTRUCTION, which is the
cause; every item above is a detector.

## T30 -- a diagnostic is code, and it lies in its own characteristic ways

**Rule:** an instrument gets the same scrutiny as the thing it measures, and
must be judged on THREE axes before any result from it is believed: does it
actually run, does it cost enough to change the outcome, and can it produce
the finding it just produced by accident?

1. **"Reports healthy" is not "is running". Ask the BINARY.** One command
   settled what a session of config-reading could not:
   `xtensa-esp32s3-elf-nm firmware.elf | grep heap_trace`. Symbols present,
   hook absent. Generalize: when a subsystem reports healthy and produces
   nothing, stop auditing configuration and go looking for the symbol, then
   for the CALL to it.
2. **A detector whose cost moves the measurement has already changed it.**
   This is T27 in the memory dimension. A 9 KB `.bss` table pushed
   `int_min_free` to **88 bytes** and turned one A/B arm from 4-reboots-in-6
   into 0-in-7. The clean-looking arm was the artifact. Budget a diagnostic's
   RAM against `heap_min` under load, never against total free.
3. **Hardware watchpoints are aligned DOWN, so a naive arm watches the
   neighbor.** Arming a 64-byte window on an unaligned block starts it BEFORE
   the block and covers the previous heap block's header, which TLSF rewrites
   on every coalesce: 14 false hits in one run, every one landing in
   `block_absorb` and looking exactly like a find. Hand the watchpoint aligned
   storage or refuse to arm.
4. **Allocator hooks fire AFTER the fact, so pointer-stamp schemes race.**
   `heap_caps_free` calls the free hook AFTER `multi_heap_free`. In that
   window another task can allocate the same block, so the stamp lands on live
   memory and that block's next honest free reads as a double free. The false
   positive is indistinguishable from the real thing by hit alone.
5. **A diagnostic must not be able to brick the recovery path.** The
   double-free detector called `abort()` on detection. The OTA upload path
   tripped it, so the device rebooted mid-flash and could no longer be updated
   over the network. Recovery took a serial rescue. **A detector reports; it
   does not adjudicate.**
6. **Suppress the harness output and you cannot tell "survived" from "never
   ran".** An A/B arm reported six clean runs while the load line was piped to
   null. With `int_free` near the WS accept floor, refused sessions look
   exactly like a passing test. Assert the load landed on EVERY run, not once.
7. `custom_sdkconfig` cannot express "off" as `=n`. See
   `.claude/rules/build-test-deploy.md`.
