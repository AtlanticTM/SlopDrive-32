# LEDGER — current volatile truth

The single home (CANON C-2) for project/device state. Entries are stamped
`[verified YYYY-MM-DD — method]` or marked `[UNVERIFIED]` (hearsay — confirm
before building on it). Superseded entries are edited in place; git history
is the archive. Read this before substantive work; update it in the same
commit as any change that alters it (C-3).

## Device & branch

- **OVERNIGHT BENCH AUTHORIZATION (operator, 2026-07-28, pre-sleep):** motor
  UNPLUGGED — flash and test freely; fake-home via homeoverride for
  full-path testing; machine functionally identical except no Modbus or
  current-sense values (expected, not failures); COM11 serial authorized as
  rescue if something breaks. Deploy + live verification of the RFC batch
  is therefore IN SCOPE tonight (supersedes the no-deploy default for this
  session only).
  MORNING-REPORT PROTOCOL (operator, 2026-07-28, second sleep): if a fatal
  error stops pipeline work, SD32-OVERNIGHT-REPORT.md gets a
  "NOT DONE — HIT AN ERROR" section stamped at the VERY END: what stopped,
  the evidence, the best-effort suggested fix, and what is blocked behind
  it. Operator reads it on waking, stamps a ruling before work, checks in
  again from work. Absence of that section = nothing fatal happened.

- Work happens on `feat/cpp20-slopsync`; the device runs this branch's
  firmware; `main` is behind until stabilization + merge. [verified
  2026-07-27 — git branch state]
- Source-tree firmware version: see `FIRMWARE_VERSION` in
  `include/system/config_api.h` (its one home). [C-1 pointer]
- Deployed firmware on the device: **2.2.2** — the `custom_sdkconfig` memory
  batch (ACTIVE TASK 1 items 2-5) plus the task-watchdog threshold fix. What
  each contains has its one home in ACTIVE TASK 1; do not restate it here.
  Flashed over HTTP `/api/ota` (espota's PBKDF2/MD5 auth still fails on this
  host — TRAPS/OTA topology). [verified 2026-07-31 — `2.2.1 -> 2.2.2` on
  `/api/capabilities`, then the 16-socket abuse test completing with zero
  reboots]
- LIVE CONFIG TRUTH, which outranks any compiled default: the device's STORED
  `sm_tune_infeas_policy` is **3** (prio-amplitude), so `InfeasiblePolicy::Blend`
  is selectable but NOT in force. Stored values beat compiled defaults by
  design. [verified 2026-07-30 — `/api/slopmotion` echo]
- Catalog etag the device serves: **94dc68dcb53577f0**, byte-identical to the
  native pin — that identity is what proves the six-option policy select is
  really on hardware. [verified 2026-07-30 — `slopsync_probe --no-motion` 44/0]
- Version-by-version deploy history lives in git and in
  `## Landed history (compacted)`. It is not restated here (C-1).

## ACTIVE TASK 1 — MAKE THE MACHINE UNCRASHABLE FROM MEMORY (2026-07-31, operator-ordered)

**Operator's acceptance bar, verbatim in intent:** once this lands, an
out-of-memory crash is a **DEFECT to be fixed, not a flaky system to be lived
with**. That is the definition of done, and it is a stronger claim than "we fixed
a leak" — it says the machine must survive a leak without panicking.

**THE ARITHMETIC THAT EXPLAINS EVERY "RANDOM" PANIC.** Measured 2026-07-31 on
fw 2.1.99 at idle, against the shipped Arduino sdkconfig:

    CONFIG_LWIP_TCP_SND_BUF_DEFAULT  5744
    CONFIG_LWIP_TCP_WND_DEFAULT      5760   -> ~11.5 KB INTERNAL per TCP conn
    CONFIG_LWIP_MAX_ACTIVE_TCP         16
    CONFIG_LWIP_MAX_SOCKETS            16
    measured int_free              36 312

**The heap backs about THREE connections; lwIP is configured to accept SIXTEEN.**
Nothing in our code decides whether a given boot dies — the client population
does. A browser opens 2-6 sockets to one host; add a WS client, mDNS, a socket
that has not timed out yet, an OTA, and the line is crossed. Same firmware, same
workload, different outcome. That IS the unpredictability, and it is a
configuration fact, not a mystery.

**Ordered by leverage. Items 2-5 are ONE deliberate `custom_sdkconfig` batch**
(they ride the hybrid IDF rebuild and change the build lineage), with an
`/api/tasks` reading immediately before and after so the effect is attributable.

0. **LANDED fw 2.2.0 — the two lines that make the NEXT crash self-explanatory.**
   * `oomhook` (`include/system/OomHook.h`, `src/system/OomHook.cpp`) installs
     the IDF failed-alloc hook. ISR-SAFETY IS THE DESIGN: the hook can fire in
     ANY context, so it writes plain statics and drops ONE crashring crumb
     (`oom`) and nothing else — it must not log, because SlopLog is explicitly
     not ISR-safe and logging from an ISR-context allocation failure would trade
     a diagnosable panic for an undiagnosable one. Draining and logging happens
     on a TASK, at the existing 10 ms heap-poll site in `main.cpp`, as
     `ALLOC FAILED #n: N B caps=0x.. task=.. fn=.. (free_int=.. largest_int=..)`.
     Also exposed on `/api/tasks` as `oom{}`, via `peek()` rather than `take()`
     so a readout can never swallow the log line the failure was going to write.
     **Live-verified 2.2.0: `oom:{"count":0}` — the hook is installed and has
     never fired. Keeping that 0 true IS this task.**
   * The WS shed line now carries `free_int` / `largest_int` and was raised
     DEBUG -> WARN. Rationale: shedding means the CLIENT stopped draining, which
     is the exact window the operator's 1-10 minute crashes cluster in. Falling
     heap while shedding indicts that path; flat heap exonerates it. One glance,
     no theory.
   **THE TWO FAILURE MODES ARE PROBABLY DIFFERENT.** The `mdns` core dump
   describes a 4.7 HOUR life; the operator's testing crashes ran 1-10 MINUTES at
   random intervals with the machine actively moving and no client attaching.
   Random intervals point at a JITTER-TRIGGERED failure (laptop-on-WiFi to
   device-on-WiFi), not a fixed-rate leak — which makes the operator's original
   network-jitter instinct correct: jitter is not the bug, it is the trigger that
   exposes a machine with no margin. Do not assume one fix addresses both.
1. **~~`heap_caps_register_failed_alloc_callback()`~~ — DONE, see item 0.** Plain API,
   works on stock precompiled libs, no rebuild, ~15 lines. Fires BEFORE the
   abort with the requested size and caps; log task name + size + caps + free +
   largest. Converts every future OOM from an anonymous `PANIC` into
   "mdns wanted 40 B internal, 316 free, largest 148". Highest signal per line in
   the whole plan — it would have turned the operator's ten crashes into ten
   diagnostic lines.
2. **`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`** (currently NOT set). **8.1 MB of
   PSRAM is completely untouched** while WiFi and lwIP contend for 36 KB of
   internal. Biggest single lever. Costs some throughput; irrelevant at
   telemetry frame sizes.
3. **`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 0 -> 32768.** This may BE the panic:
   the reserve exists so allocations that MUST be internal (mutexes, DMA
   descriptors, ISR-safe work) cannot be starved by bulk ones, and it is
   currently zero. The thing that died was `lock_init_generic` creating a
   MUTEX — small, must-be-internal — failing because bulk consumers had taken
   everything. With a reserve, the same leak degrades to a refused connection
   instead of an abort.
4. **`LWIP_MAX_SOCKETS` / `LWIP_MAX_ACTIVE_TCP` 16 -> 6.** Operator ruling:
   "7 connections is an insane person problem." Refusing a 7th is a correct,
   boring outcome; accepting it and panicking mid-stroke is not.
5. **`LWIP_TCP_SND_BUF_DEFAULT` / `LWIP_TCP_WND_DEFAULT` 5744/5760 -> 2880.**
   Halves per-connection internal cost.
6. **Application-level: BOUND THE WS TX QUEUE AND DROP, NEVER ENQUEUE, FOR A
   CLIENT THAT IS NOT DRAINING.** Telemetry is regenerated continuously, so a
   dropped frame costs nothing and a frame queued for a backgrounded tab costs
   everything. This is the wedged-WS-client trap with a reservation behind it.

### ITEMS 2-5 LANDED (fw 2.2.1), AND ITEM 6 WAS ALREADY BUILT (2026-07-31)

One `custom_sdkconfig` batch on `env:s3_main` (NOT on `sd32`/`sd32-ota`, so the
serial-rescue and OTA images share one network stack — a rescue image whose
lwIP differs from the image it rescues is a trap). Idle A/B, same machine,
before -> after:

| | 2.2.0 | 2.2.1 | delta |
|---|---:|---:|---:|
| `int_free` | 36 140 | 41 279 | **+5 139** |
| `int_largest` | 22 516 | 28 660 | **+6 144 (+27%)** |
| `psram_free` | 8 118 524 | 8 103 780 | **-14 744** |

That PSRAM row is the proof `TRY_ALLOCATE_WIFI_LWIP` is real: 14.7 KB of
allocation MOVED off internal. `int_largest` matters more than free-total here
because the 2.1.99 core dump died on a small MUTEX, i.e. a contiguity failure.
`int_min_free` is NOT comparable across this boundary (fresh boot vs hours of
uptime) — do not quote it as a win.

Two corrections to the plan as written, both from reading the code rather than
reasoning about it:
* **`LWIP_MAX_SOCKETS` was never the browser knob.** AsyncTCP allocates raw
  lwIP PCBs (`tcp_new_ip_type`/`tcp_write` via `tcpip_api_call`), not BSD
  sockets, so `LWIP_MAX_ACTIVE_TCP` is what bounds a page load; the socket cap
  only ever governed mDNS and `SlopSyncUdpDiscovery`.
* **The cap is 8, not the planned 6** (operator-ruled). One browser tab opens
  up to 6 parallel connections per host plus a WebSocket = 7 from a single
  tab, and TIME_WAIT draws from the same pool. 6 would refuse assets
  mid-page-load and read as flaky WiFi.
* PSRAM on this board is **OCTAL** (`memory_type qio_opi` ->
  `CONFIG_SPIRAM_MODE_OCT`), operator-confirmed. The generic
  `esp32s3/sdkconfig` showing QUAD is a different variant's file.

**Item 6 needs no work — it was built by RFC-050.**
`SlopSyncAsyncWsTransport::write()` already sheds STATE/STREAM above
`kDataQueueHighWater` (3/4 depth), gates BLOB_CHUNK on the registry's
advertised in-flight budget, refuses without tearing the session down, and
reaps idle clients. The residual is DEPTH (32 buffers/client), not policy.

**Cost of this path, recorded so the next agent is not surprised:**
`custom_sdkconfig` stops consuming precompiled libs and builds the IDF from
source, materializing ~440 MB of `managed_components/` into the project root
(now gitignored; `dependencies.lock` deliberately left trackable because it
pins the versions an IDF-from-source build resolves). It also starts
TYPE-CHECKING Espressif's own defaults: `esp-modbus` fails its own
`_Static_assert` because the shipped config pairs `TIMEOUT_MS_RESPOND=10000`
with `MAX_API_BLOCKING_TIME_MS=6000`. Three separate build failures came from
the path, none from the values.

### THE SECOND FAILURE MODE, FOUND AND FIXED (fw 2.2.2) — NOT A MEMORY BUG

The ledger predicted two different failure modes. There are.

**Reproduced deterministically:** five HALF-OPEN HTTP connections (socket
opens, partial request, never completes) -> `/api/tasks` unreachable ->
`reset_reason TASK_WDT`, every round. **`heap_min` at the crash: 40 619 B.**
The crash that opened this task died at **316 B**. This is not memory.
Control: EIGHT *complete* keep-alive requests at the same socket count survive
untouched, so the trigger is incomplete requests, not connection count and not
the new cap.

**Mechanism — a threshold armed against itself.** The Arduino sync `WebServer`
serving `/api/*` on `httpTask` (Core 0) handles ONE client at a time and
declares `HTTP_MAX_DATA_WAIT` / `HTTP_MAX_SEND_WAIT` / `HTTP_MAX_POST_WAIT` =
5000 ms each. `CONFIG_ESP_TASK_WDT_TIMEOUT_S` was **5**, with
`CHECK_IDLE_TASK_CPU0=y` and `PANIC=y`. The watchdog was set EXACTLY at the
supervised code's own documented worst case, so it fires on correct-but-slow
rather than on hung. Raised to **12 s**.
`HTTP_MAX_DATA_WAIT` has no `#ifndef` guard, so a `-D` build flag cannot
override it — the sdkconfig lever was the only one reachable.

**Verified:** the identical 16-socket / 3-round test that rebooted in 100% of
rounds now completes with ZERO reboots (uptime 336 697 ms across the run,
`boot_seq` unchanged, `prev reset` = SW/the OTA itself), heap drift -224 B,
`int_largest` 28 660 constant, `oom` 0.

**THIS BOUNDS THE DAMAGE, IT IS NOT THE CURE.** A half-open client still
monopolizes the single-client sync server for up to 5 s, and `/api/tasks` is
genuinely unreachable while it does. The cure is getting `/api/*` off a
one-client-at-a-time server (`docs/http-plane-retirement.md`), which is an
architecture change and deliberately NOT bundled with a memory measurement.
**Operator trade, stated:** recovery from a genuine hang now takes 12 s
instead of 5. Motion safety does not depend on this watchdog (RFC-051
critical-stall parks own that), but it is a real change.

**STILL OWED:** the WS leg of the abuse harness never ran — every
`ClientWebSocket` connect threw, which is a HARNESS bug, not a device result.
"Backgrounded tab that stops draining" is therefore UNTESTED, and it is the
case the operator's original mid-stream crashes point at hardest. The harness
also drives no motion, so "machine actively moving" is untested too.

**PROOF OF DONE — the machine must survive ABUSE, not just a clean run:**
sustained stream + deliberately open more sockets than the cap + background a
client tab mid-stream + kill clients mid-frame + **back-to-back sessions with no
reboot between** (the standing regression for the ownership-teardown leak), all
while `/api/tasks` is polled. Pass = no panic, and the failed-alloc callback
either never fires or fires and is handled visibly. A leak may still EXIST after
this and that is acceptable; a leak that PANICS is not.

## ACTIVE TASK 2 — observability before the next motion change (2026-07-31, operator-agreed)

**Trigger.** Operator reported ~10 panics in an hour under live streaming, plus
random panics at idle. `/api/crash` on the surviving record: `reset_reason PANIC`,
**`heap_min 316`**, `max_block_last 148`, and both recovered crumbs are
`ws-attach` 286 ms apart. That is heap EXHAUSTION (316 bytes free internal), not a
logic fault, and it points at the WS attach/teardown path — the same class as the
"ownership teardown leak" already on record. NOT yet attributed: the crumb record
describes a 4.7 h life, which does not match "10 crashes in an hour", so the fast
failures may have a different signature that the single-deep ring has since
overwritten.

**Two stale beliefs corrected in the feasibility pass, both by checking rather
than reasoning:**
1. `CrashRing.h` says a real backtrace "needs a core-dump flash partition …
   that upgrade is a serial-reflash bench item". WRONG NOW: `partitions_ota.csv`
   already carries `coredump, data, coredump, 0xFF0000, 0x10000`, and the shipped
   Arduino sdkconfig already has `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`,
   `..._DATA_FORMAT_ELF=y`, `..._MAX_TASKS_NUM=64`. Full ELF core dumps are being
   WRITTEN today and have simply never been read.
2. An earlier agent's claim that profiling/heap-tracing needs an ESP-IDF rebuild
   was treated as prohibitive. Operator pushed back; research proved them right.
   pioarduino's `custom_sdkconfig` (present in the pinned platform —
   `builder/frameworks/arduino.py` `call_compile_libs()`) rebuilds the IDF libs
   from source with a custom config WHILE KEEPING `framework = arduino`. It is a
   platformio.ini block plus one long build, not a port to `framework = espidf`.
   Basic JTAG (`debug_tool = esp-builtin`) needs no rebuild at all.

**Ordering, and why.** JTAG halts the CPU, which drops WiFi and the motion loop;
a leak that takes hours does not yield to a breakpoint. Cheap non-halting
visibility first, and the IDF rebuild LAST so it is not confounding the crash it
is meant to measure.

1. **`/api/coredump`** — serve the coredump partition over HTTP
   (`esp_core_dump_image_get` + `esp_partition_read`). Gives faulting task, PC,
   registers and all 64 task stacks, with no cable and no serial. Fix the stale
   `CrashRing.h` comment in the same commit.
2. **Live heap + task telemetry** on `/api/status` — `heap_caps_get_info` free /
   largest-block / PSRAM, plus per-task `uxTaskGetStackHighWaterMark` and CPU
   share. Free today: `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` and
   `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y` are already set. This is the
   BASELINE PROFILE, and it also settles whether the 2026-07-30
   `physicalBandExcess` change (+~1.1 KB stack: `Trajectory<1>` 616 B +
   `InputParameter<1>` 480 B, zero heap) matters on the 16 KB Sampler task.
3. **Archive the ELF per deployed version.** A core dump is unreadable without
   its exact matching ELF and `.pio/build/sd32-ota/firmware.elf` is overwritten
   every build. Without this, steps 1 and 4 silently produce garbage later.
4. **`custom_sdkconfig`: `CONFIG_HEAP_TRACING_STANDALONE` (+ apptrace).**
   Per-allocation attribution with call stacks — the real answer to "what is
   using what memory where". Last, because it wipes and reinstalls the framework
   packages, so the resulting firmware is a different IDF build lineage than the
   one that has been crashing.

Then: baseline profile -> fix -> keep the fixed profile as the regression
reference the operator compares future reports against.

### Steps 1-3 LANDED (fw 2.1.98 / 2.1.99), and the first real post-mortem

**`/api/coredump` (2.1.98).** A complete 42 084 B ELF core dump was ALREADY in
flash and had never been read. Summary form needs no host tooling; `?raw=1`
streams the partition for `espcoredump.py`. The stale `CrashRing.h` claim is
corrected in place.

**THE PANIC IS HEAP EXHAUSTION, CONFIRMED TWICE, INDEPENDENTLY.** Faulting task
`mdns`, `exc_cause 0` (an abort, not a memory fault), and the top six frames are
in IRAM/ROM so they symbolize correctly even against a mismatched ELF:
`panic_abort <- esp_system_abort <- abort <- lock_init_generic (locks.c:77) <-
lock_acquire_generic <- _lock_acquire_recursive`. locks.c:77 is newlib creating a
lazy mutex and calling abort() when the allocation FAILS. That is the same event
`crashring` recorded as `heap_min 316`, reached by a different route.
**`mdns` is the VICTIM, not the culprit** — the faulting task is only whichever
one next needed memory. The leaker is still unidentified.
The lower ten frames symbolized incoherently (`uart_tcgetattr`, `bt_bb_gain_set`,
`_fread_r` in one stack) — that is the ELF mismatch showing exactly as predicted,
and is why step 3 existed.

**Step 3 done:** `artifacts/elf/firmware-<version>.elf` archived from 2.1.98 on.

**`/api/tasks` (2.1.99) — BASELINE PROFILE, machine IDLE, no client connected,
not homed, no stream.** Reports raw runtime counters, not percentages: this
endpoint has no memory of when it was last polled, so the caller diffs two
samples and gets a real interval instead of a since-boot average.

    heap    int_free 36 312   int_min_free 25 648   int_largest 22 516
            psram_free 8 118 524 (essentially untouched)
    CPU over 45 s (2 cores = 200 % available)
            SlopSyncHub 6.39 %   HTTP 4.99 %   Motor 3.89 %
            Sampler 2.59 %       wifi 2.33 %   esp_timer 1.97 %
    stack_free_min (bytes, lowest first)
            ipc0 80   IDLE0 244   ipc1 264   IDLE1 348   mdns 1 652
            ... Sampler 15 232 of 16 384   SlopSyncHub 13 792 of 16 384

**THREE FINDINGS FROM THE BASELINE:**
1. **No leak at idle.** Internal free drifted +304 B over 45 s and the largest
   block did not move at all. So the leak is triggered by ACTIVITY. Reproduce
   with streaming, not by waiting.
   **RETRACTED — "and the ws-attach crumbs point at the connect path".** That
   inference was a SELECTION ARTIFACT and is withdrawn. `crashring::crumb()` has
   exactly seven call sites, ALL of them WebSocket lifecycle or HTTP request
   (`ws-refuse`/`ws-attach`/`ws-detach`/`ws-idlereap`,
   `http-root`/`http-503`/`http-abort`). There is not one crumb in the motion
   path, the sampler, the servo bus or the SlopSync data plane — so during an
   active stream NOTHING crumbs, and the last crumb is necessarily from whenever
   a client last attached, however long before. "Last crumb = ws-attach" carries
   no information about where the crash was.
   **OPERATOR OBSERVATION, which outranks it:** the crashes happened MID-STREAM,
   machine actively moving, with no client attaching. That points at the
   STREAMING DATA path — per-frame allocation in the async WS TX queue, the hub's
   channel encoding, the telemetry fan-out — not the connect path. Consistent
   with `SlopSyncHub` already burning 6.39 % of a core with ZERO clients.
   **CONSEQUENCE: the crash ring is instrumented only where the crashes are not.**
   Crumbs (or better, the failed-alloc callback of ACTIVE TASK 1 item 1) are
   needed on the streaming path before the next hunt, or the next post-mortem
   will be just as blind.
2. **`SlopSyncHub` burns 6.39 % of a core with ZERO clients connected**, and
   `Motor` 3.89 % while stationary. Together with HTTP that is ~24 % of one core
   at rest. This is the operator's "code that cycles way more times than
   necessary" and it now has a number.
3. **`ipc0` has 80 BYTES of stack headroom** (IDLE0 244, ipc1 264). These are
   IDF-owned tasks with fixed small stacks, but 80 B is thin enough that any
   deepening of an `esp_ipc_call` callback lands on it. Watch, do not yet touch.

**NOT YET CLEARED — the 2026-07-30 `physicalBandExcess` stack delta.** Sampler
shows 15 232 B free of 16 384, i.e. ~1.1 KB used — but this sample was taken with
NO motion plan running, so the waveform path never executed. The reading proves
nothing until it is retaken under a live stream. Do that before the leak hunt.

**BACK POCKET (operator, 2026-07-31; cannot flash until home).** The ESP32-C5
coprocessor on the custom PCB is functionally obsolete since SlopSync but is
still fitted, still a separate CPU, and still UART-connected to the S3. Proposal:
turn it into a dedicated logging offload that ships all telemetry to a server on
the LAN over its WiFi 6 / 5 GHz radio, so a crash stops taking its own log with
it. Notes for whoever picks this up: SlopLog's `ISink` is already the right seam
and the one hard constraint is that the sink must never block (the USB-CDC
blocking-sink trap); a DMA-backed UART TX ring with drop-on-full satisfies that
at up to 5 Mbaud. SPI with the S3 as master is the higher-headroom option and
leaves the UART free, but is not needed first.

## ⏭ THE QUEUE (operator-ordered 2026-07-31) — SUPERSEDES "NEXT STEPS" BELOW

Ordering ruled by the operator: **machine stability/diagnostics -> gigagauntlet
(short + long) -> webui -> nice to have.** Entries that were chasing the same
goal from different sections have been MERGED here; where that happened the old
entry is named so nobody re-opens it as separate work.

### TIER 1 — MACHINE STABILITY / DIAGNOSTICS

0. **HEAP CORRUPTION ON THE HUB PLANE — REPRODUCIBLE, ISOLATED, UNFIXED
   (2026-07-31, fw 2.2.6). This outranks everything below it and is the first
   thing that has ever explained the operator's real-world crashes.**

   **REPRODUCTION (deterministic on fw 2.2.9): 12 concurrent WS sessions on
   `:82` blasting well-formed frames. No motion stream needed. No malformed
   frames needed.** Harness: scratch `isolate.py many 12`.

   **Isolation runs, all on `:82`, one variable each:**

   | run | poisoning | motion stream | result |
   |---|---|---|---|
   | lying length field (declares 65 535 B, sends 4) x40 | light | off | survived |
   | attach/RST churn x40 | light | off | survived |
   | storm, 1 session, 103 565 frames in 10 s | light | off | survived |
   | 12 concurrent sessions, 166 936 frames | light | off | survived |
   | 12 concurrent sessions RST together, 3 rounds | light | off | survived |
   | 12 concurrent sessions, 44 769 frames | light | **ON** | **REBOOT** |
   | **12 concurrent sessions, 40 715 frames** | **COMPREHENSIVE** | **off** | **REBOOT** |

   **THE STREAM IS NOT REQUIRED — that was a wrong conclusion from the first
   pass and it is corrected here.** The last row is the same load as row 4,
   which "survived" under LIGHT poisoning. The difference is only the detector:
   comprehensive poisoning turns SILENT corruption into an immediate abort. So
   the rows marked "survived" under light poisoning were most likely corrupting
   the heap and getting away with it, which is the worse outcome. Treat
   "survived under light poisoning" as "not detected", never as "clean".

   **METHOD WARNING, learned the expensive way:** the first harness compared
   `uptime_ms` before/after. That gives FALSE "survived" verdicts — a device
   that reboots at the START of a 10 s test climbs back past the pre-test
   reading before the test ends, so `after < before` is false. Two verdicts
   were wrong before this was caught. **Use `boot_seq` from `/api/crash`; it
   only ever increments and cannot lie.**

   **It is CORRUPTION, not exhaustion. Everything ACTIVE TASK 1 did is
   irrelevant to it.** Symbolized from `/api/coredump` (clean backtrace,
   `bt_corrupted: false`), faulting task `tiT`:

       panic_abort <- __assert_func
                   <- multi_heap_free   (multi_heap_poisoning.c:279)
                   <- free <- mem_free  (lwip/core/mem.c:236)
                   <- do_memp_free_pool (lwip/core/memp.c:409)
                   <- sys_check_timeouts(lwip/core/timeouts.c:401)
                   <- tcpip_thread

   `heap_min` at that panic was **28 911 B** — there was plenty of memory. The
   heap POISONING checker caught a bad canary while lwIP freed a timeout pool
   entry, i.e. a double-free or an overrun happened EARLIER and lwIP's timer
   thread is merely the first to touch the damage. The panic is correct
   behavior; without poisoning this corrupts silently, which is worse. All 15
   recovered crumbs were `ws-attach`/`ws-detach`.

   **THE ALLOCATOR'S OWN FREE LIST IS THE THING BEING DAMAGED.** Reproduced
   three times with an identical, uncorrupted backtrace, `EXCCAUSE 29`
   (LoadProhibited) in `remove_free_block` — TLSF following a next/prev
   pointer into invalid memory while servicing a malloc:

       remove_free_block (tlsf_control_functions.h:373)
       block_locate_free / tlsf_malloc (tlsf.c:444)
       multi_heap_malloc <- heap_caps_malloc_prefer
       wifi_malloc (esp_adapter.c:74) <- esf_buf_alloc_dynamic

   `heap_min` was 29 299 / 30 071 / 28 911 B across those runs — never short.
   **WiFi is the VICTIM, not the culprit**: it is simply the most frequent
   allocator on the device, so it is whoever trips over damage someone else
   wrote. An overrun past a block boundary into TLSF metadata fits all three.
   Prime suspect remains the AsyncTCP/AsyncWebSocket buffer path, because WS
   concurrency is the reproduction and field bug #5 was already a lifetime
   defect in that exact code.

   **A THIRD, DISTINCT signature appeared in the same session** and must not
   be conflated: task `ipc0`, `heap_min` 145 171 B, dying in
   `btdm_intr_alloc -> esp_intr_alloc -> heap_caps_malloc` with
   `_xt_context_save`/`_frxt_int_enter` on the stack — an interrupt saving
   context onto a task whose measured headroom is **84 B**. ACTIVE TASK 2's
   baseline explicitly predicted this ("80 B is thin enough that any deepening
   of an `esp_ipc_call` callback lands on it. Watch, do not yet touch"). It has
   now landed. Whether it is independent or a downstream effect of the
   corruption above is NOT established.

   **Next steps, in order, none taken yet:**
   * `CONFIG_HEAP_POISONING_COMPREHENSIVE` — catches the write at the moment
     of corruption instead of at the victim's next free. One line in the
     existing `custom_sdkconfig` block.
   * `CONFIG_HEAP_TRACING_STANDALONE` (was ACTIVE TASK 2 step 4, now unblocked)
     for allocation attribution.
   * Suspect ordering: AsyncTCP/AsyncWebSocket buffer lifetime under
     concurrent TX while the hub task is also publishing telemetry and draining
     the pacing ring. Field bug #5 was already an AsyncTCP-task lifetime defect
     in this exact area.
   * Reproduction is cheap and deterministic: force-home, start
     `slopsync_probe --stream`, then 12 concurrent WS sessions blasting
     well-formed frames. Harness: scratch `gauntlet.py` / `isolate.py`, to be
     promoted into the Tier-2 gigagauntlet.

1. **WINDOW-EXIT BRAKING RUNAWAY — ranked first because it is the only SAFETY
   item on this list.** Measured 625 mm of travel on a 500 mm rail: the
   carriage reaches the physical end stop. Fix is measured and NOT applied
   (runaways 11/54 -> 0/54, worst excursion 289x smaller). Full chain and the
   proposed one-line rule are in `## Pending rulings`; this needs an operator
   stamp, not more measurement. Everything else here is about uptime; this one
   is about the rail hitting the end of the world while someone is on it.

2. **HTTP PLANE -> EVENT-DRIVEN (`:80` async).** *MERGES three entries that
   were the same goal:* `docs/http-plane-retirement.md`'s remaining scope,
   "Extract `MachineCommand` from `WebUI.cpp`" (Deferred/planned), and the
   2026-07-31 async migration. Scope: 27 routes, 76 sync-API call sites.
   **Justification is measured, not architectural taste** (TRAPS T26):
   legitimate full page serve **9 s**, wedged half-open client **10 s**, OTA
   upload seconds — all on a server that handles ONE client at a time. No
   deadline separates slow from stuck at a 10x gap, so this cannot be tuned;
   it has to stop blocking. Do it ALONE, after a heap measurement
   (AsyncWebServer allocates per queued message), OTA last, serial-rescue
   window open. Sub-items that land with it:
   * **RFC-055 admission control** (SlopSync `spec/RFC-QUEUE.md`) — needs ONE
     connection-accounting point, which this creates.
   * The dead Core-0 reap (`kCore0ReapMs = 0`) gets DELETED here, not left as
     scaffolding.
   * `CONFIG_ESP_TASK_WDT_TIMEOUT_S=12` gets re-examined: it was raised
     because a 5 s watchdog sat exactly on the sync server's 5 s ceiling. With
     no blocking serve slot, the original 5 s may be correct again.

3. **OTA HARDENING — two independent items, both small, both do-first.**
   * **Size the erase.** `Update.begin(UPDATE_SIZE_UNKNOWN, command)`
     (`src/system/OtaService.cpp:181`) erases the WHOLE OTA partition every
     flash — the documented cause of cache-disabled stalls and watchdog trips
     (arduino-esp32 #3775). `WebServer::clientContentLength()` already has the
     real size. Bounding the erase shrinks the hazard for every OTA regardless
     of anything else on this list.
   * **OTA GATES ON MOTION (operator ruling 2026-07-31).** Currently INVERTED:
     `prepareForOta()` refuses only on the concurrent-OTA CAS, then *stops
     motion and proceeds* — hard e-stop mid-stroke, reboot, and the machine
     comes back UNHOMED. The ruling is refuse-the-OTA, never interrupt the
     session. **Design constraint, non-negotiable:** gate on evidence that
     EXPIRES (measured speed / live plan), never a latched flag — a stuck
     latch makes the device permanently unflashable.

4. **PAGE-SERVE THROUGHPUT — FIXED (fw 2.2.5), and the cause was our own
   change.** ACTIVE TASK 1 item 5 halved `LWIP_TCP_SND_BUF_DEFAULT`/`WND`
   5744/5760 -> 2880 on 2026-07-31. Measured A/B, same machine, same 115 970 B
   bundle:

   | | 2880 | stock 5744/5760 |
   |---|---:|---:|
   | cold page load | **8.99 s** | **0.98 s** |
   | throughput | 12.9 KB/s | **119 KB/s** |
   | `int_free` | 41 107 | 41 103 |
   | `int_largest` | 28 660 | 28 660 |

   **~9x throughput for ZERO measurable heap.** Item 5 was a pure loss: it
   bounded a worst case that never materialized while charging the cost on
   every single page load. The general rule, now in platformio.ini next to the
   values: these are per-connection CEILINGS filled on demand, not
   reservations — to bound per-connection RAM, bound the CONNECTION COUNT
   (`LWIP_MAX_ACTIVE_TCP`), which costs nothing until the cap is reached.
   Remaining, and NOT a defect: TTFB 0.058-0.088 s and revalidation **304 in
   63 ms**, so warm loads were always fine; only cold load was affected.
   **Correction, recorded because it went the wrong way:** an earlier draft of
   this entry claimed the bundle was 302 423 B and that "115 KB" comments were
   2.6x stale. WRONG — 302 423 is the DECOMPRESSED size reported by
   `Invoke-WebRequest`; the wire size is 115 970 B gzipped and the existing
   comments were right all along.

5. **`canon_lint.py` CANNOT SEE UNTRACKED FILES.** `tracked_files()` shells to
   `git ls-files`, so new work is invisible to the gate that is supposed to
   police it — it reported "clean (0 findings)" with a British spelling sitting
   in `sim/slopsim/src/machine/MotionCore.h:253`. Every "gate PASS" on a branch
   with new files is a partial truth until this is fixed.

6. **ACTIVE TASK 2 step 4 — heap tracing, NOW UNBLOCKED.** It was ranked last
   because it needed an IDF-from-source rebuild. `custom_sdkconfig` is live and
   working as of 2026-07-31, so `CONFIG_HEAP_TRACING_STANDALONE` is now a
   one-line addition to an existing block, not a project.

7. **ONE DOCTRINE FOR WEDGED PEERS.** *MERGES:* the landed WS idle-RX reap
   (fw 2.1.88), `IdleGuardWebServer::dropIdleCapture()` (silent-socket half
   only), the never-implemented BLE idle reap (was in Deferred/planned), and
   the dead Core-0 reap. Four mechanisms, one goal, no shared rule. Decide the
   rule once — what counts as wedged, who reaps, on which task — then apply it
   per transport. TRAPS T26 is the evidence for why ad-hoc reaping is
   dangerous.

8. **Square-pulse forensics** — operator narrowed it to MANUAL tape driving
   (patterns run clean), pointing at the move-INTENT/arbiter path. Reproduction
   plan already written in `## Pending rulings`.

9. **Clocked-logging + legacy-log audit** (operator-approved 2026-07-27, NOT
   STARTED) — diagnostics hygiene; every periodic log earns its keep, demotes,
   or dies. Rider: strip MotionArbiter's sediment in the same pass.

### TIER 2 — GIGAGAUNTLET (replaces the ad-hoc PROOF OF DONE list)

The 2026-07-31 abuse harness was a scratch script and it FOUND REAL BUGS, so it
gets promoted to a tracked, two-mode gate. Both modes assert the operator's
priority order: **(1) hub alive AND STILL HOMED, (2) existing sessions survive,
(3) new clients are refused legibly.** Losing home is a FAILURE, not a recovery.

* **SHORT MODE — everything that breaks fast, no waiting.** Socket flood past
  the cap; half-open/slowloris on `:80`; **packet storm on `:82` faster than
  the hub can drain** (never yet run — the plane the priorities are actually
  about); malformed/truncated/oversized frames; more sessions than `kSlots`;
  mid-frame kills with LINGER 0; rapid connect/disconnect churn; back-to-back
  sessions with NO reboot between (the standing ownership-teardown
  regression); OTA refused while motion is active; `/api/tasks` polled
  throughout. Runs against a force-homed machine with the motor bus verified
  <10 V.
* **LONG MODE — the leak hunt, the only one that needs patience.** Sustained
  stream for hours with `/api/tasks` sampled on an interval and DIFFED (the
  endpoint reports raw counters precisely so a caller can do this), heap
  watermark + largest-block tracked, `oom` count asserted 0, homed asserted
  true throughout.
* **Harness debt, recorded so it is not rediscovered:** the WS leg of the
  2026-07-31 script never connected (wrong URL/subprotocol — it is
  `ws://<ip>:82/` with subprotocol `slopsync.v1`), so "backgrounded tab that
  stops draining" is STILL UNTESTED. `tools/slopsync_probe.py` in the sibling
  repo is importable (`__main__`-guarded) and already has real framing,
  `--bench-home`, and `--stream` — build on it instead of hand-rolling frames.

### TIER 3 — WEBUI

10. Authoring-legibility campaign Phases 2-6 (plan:
    `~/.claude/plans/pure-crafting-thacker.md`; Phases -1/0/1a/1b DONE).
11. SSManager v0 then v1 (schema landed, Rust half not started).
12. UI punch list — the 4 still-open items in `## OPERATOR UI FEEDBACK QUEUE`
    plus the rapid-fire list in `## Deferred / planned`.
13. PLAN STRIP — scale the segment to travel (ruled 2026-07-30, NOT STARTED).
14. `RailWidget.svelte:146` comment lies (both RFC-041 roles ARE tagged) and
    the lo/hi/span derivation wants the shared helper — already specced.

### TIER 4 — NICE TO HAVE

15. The 2026-07-31 truth-scrub's remaining confirmed comment/doc fixes (27
    findings, receipts in that session's report) — mechanical, none
    load-bearing.
16. SlopMotion: `positionAt()`/`velocityAt()` each re-run `sampleRaw()`, so the
    1 kHz path computes the same p/v/a TWICE per tick, in `double` on a
    single-precision FPU. A combined `sample(now, p, v)` halves it with zero
    numerical change. Benefit UNMEASURED — measure before touching motion.
17. `ServoModbus` calls `_port.flush()` after every TX (5 sites), blocking the
    bus task until the last bit clears. With the XY-G485's hardware
    auto-direction nothing needs that completion signal. Bench change.
18. PSRAM-resident page bundle — **downgraded from earlier framing.** The
    per-request malloc it would have saved is ALREADY gone (static
    `sPageSendBuf`), and staleness is a non-issue (every fw/fs OTA reboots).
    Remaining value is only "off the flash bus" + a contiguous buffer for
    async chunking. Fold into item 2 if it helps there; not worth doing alone.
19. Four probably-superseded probe scripts in `webui/test/` — never triaged.
20. SlopLog + SlopGlow uplift — NARROWED: both already inject their clock and
    are hardware-free, so only "tighter conformance tests" remains of the
    original three-part description.
21. Sim: fray-d Advanced pattern has no motion effect in `sim/slopsim`.
22. RFC candidate — batched telemetry sub-samples per STATE push.
23. TCode pass-through channel; native Intiface SlopSync support; C5-node
    SlopSync transports; merge to `main`.

**Deleted as redundant during this pass:** the resolved Phase-C2 fixture-regen
entry in `## Pending operator rulings` (resolved 2026-07-28, git has it), and
the standalone "INCIDENT: 4-CLIENT PANIC" entry's action items, which are now
covered by items 2 and 4 above.

## Async-tune bench findings (2026-07-30 session)

- **RULED — default `infeasible_policy` is now `Blend`, one slider, at 0.5.**
  The other four spend ONE axis to exhaustion before touching the other, which
  is why an infeasible segment arrives as a straight line: `alpha` is driven to
  1 (the chord) rather than to what it needed. Blend walks a RAY through the
  (amplitude-loss, shape-loss) plane at an angle the slider sets and returns the
  SMALLEST legal radius, so degradation is proportional and continuous.
  Swept 10 recordings x 6 perturbations (window tight/wide/offset, halved speed,
  weakened accel) = 60 cases, 42 of which exercise the policy, scored as
  scale-free per-case regret on shape_corr + reach_ratio.
  **THE OPTIMUM MOVES WITH THE EXCHANGE RATE** — shape weighted 10x reach -> 0.875
  (regret 0.236); weighted equally or reach-heavy -> 0.5 (0.239 / 0.197 / 0.110).
  0.5 is the equal-weight optimum and wins 3 of 5 weightings, so it is the
  neutral default. NOT ambiguous: the low end is wrong — blend <= 0.25 scores
  ~0.72 regret against ~0.24 at the top, because a small shape concession keeps
  the plan FEASIBLE while lost reach is visible AND often still falls through to
  the Ruckig guard. Blend at its best beats Stretch (0.226 vs 0.248) without
  overrunning the deadline to do it.
  [verified 2026-07-30 — 60-case sweep, native slopmotion 31/31]
- **Fixed: a SECOND hand-written policy-name chain** in `/api/slopmotion` had
  already drifted — it predated `blend` and reported the live default as "?".
  Deleted; both readouts now go through the one `policyName()` switch, so the
  compiler names the next policy added instead of a readout lying about it.
- **SUPERSEDED — default `infeasible_policy` was briefly Stretch** (was Reshape).
  Measured GoogleCat, 50-150 mm window, follow->c1, 1000/50000: stretch
  0.761 rms / 14 anomalies; prio-smooth 0.747 / 50; prio-amplitude 0.765 / 40;
  reshape 0.928 / 29; scale 1.647 / 31. Fidelity across the top three is a
  0.02 mm tie, the anomaly count is not, and Stretch also sidesteps the soften
  overshoot (that fires only on Reshape). `testConfig()` in the native suite now
  PINS Reshape, because its measured numbers were all taken under it and a
  fixture must not inherit a product default it is not testing.
  [verified 2026-07-30 — native slopmotion 31/31]
- **FIXED (2.1.97) — the SAME reachability bug, one layer deeper: three CLAMPS,
  not just the name tables.** 2.1.96 widened four name/option tables and shipped;
  the operator selected blend in the UI and watched it snap back to prio-smooth.
  Cause: `setU(14, 0, 4, ...)` in `SlopSyncHubService.cpp` clamped the 0x0105
  write to 4, and 4 IS prio-smooth — the symptom named the bug exactly. Two more
  would have bitten next: `ConfigStore.cpp`'s NVS load clamped 0..4 (so it would
  have reverted on the following boot even if the write had landed) and
  `SystemState.h`'s stored default was still 2/Reshape while the catalog select
  now advertised 5/Blend.
  ROOT CAUSE, and why patching three literals was not the fix: the enum's
  cardinality was restated as a bare `4` in every clamp. It now has ONE home,
  `slopmotion::kInfeasiblePolicyMax`, and all three bounds plus the sim's twin
  derive from it. `WebUI.cpp` also gained a `static_assert` tying
  `kInfeasPolicyNames` to that constant — PROVEN to fail the build by deleting an
  entry and watching `pio run -e sd32-ota` refuse, so the next policy addition is
  a compile error instead of a silent clamp.
  LESSON: grepping the enum's NAME found four sites; the bug lived in the three
  that only mention its RANGE. Search for both.
- **FIXED (2.1.96) — `InfeasiblePolicy::Blend` was UNREACHABLE FROM THE DEVICE.** The
  engine has had it since 0.9.0; all four device-side tables were still five
  wide — `main.cpp`'s per-tick boot map (no `case 5`), `WebUI.cpp`'s string map
  and `kInfeasPolicyNames`, and the `SlopSyncCatalog.h` select's option list.
  The boot map runs EVERY TICK, so whatever it writes IS the policy: an operator
  selecting blend fell through `default:` and silently got whatever the engine
  default happened to be. This is the fw 2.1.49 bug recurring, in the exact
  place that carries a comment warning about it — the map must be widened in the
  same commit as any InfeasiblePolicy addition. Catalog select default also
  moved 2 -> 5 to match the engine, which moved the device etag
  B6 9E B0 62 49 EB E7 3A -> 94 DC 68 DC B5 35 77 F0 (deliberate; pin moved with
  its rationale in `test_slopsync_devicecatalog`). Caught by reading the device
  before flashing, NOT by a test — the whole tuning session would have measured
  prio-amplitude while believing it measured blend.
- **`/api/slopmotion` now echoes `blend`, `overshoot_guard` and
  `overshoot_chord_slack`.** All three ship non-zero as of 2.1.96 and none had a
  readout; a load-bearing default no surface shows is the ground-truth gap the
  doctrine forbids. They are read from a fresh `slopmotion::Config` because none
  has a stored setting or a POST field — with the constraint stated in-place that
  adding a setter means moving them to the `sm_eff_*` back-channel in the same
  commit.
- **RULED — the throbbing is a DISCONTINUITY the polynomial arcs across, and it
  is fixed.** Mechanism, from the 1 ms trace at OvershootTestThrobbing t=61.85 s:
  the machine is at 15.25 mm doing **+800 mm/s** when a segment says "be at 0 mm
  in 291 ms". A fixed duration plus fixed endpoints uniquely determines the
  Hermite curve and it must SPEND those 291 ms, so it climbs to 58.9 mm before
  turning — **43.6 mm of excursion on a 15.25 mm move**. Jerk-limited braking
  from 800 mm/s costs ~15 mm on this machine, so ~28 mm of that is invented by
  the polynomial, not forced by momentum. NOT "small moves" as such and NOT
  entry velocity alone: it is entry velocity meeting a duration far longer than
  the move needs. Three fixes shipped, each measured separately:
  1. **The Blend ray stopped in the INTERIOR of the box.** Losses were
     `s*blend` and `s*(1-blend)`, so at blend 0.5 the search exhausted itself at
     alpha 0.5 / f 0.0 with half of both budgets unspent, and everything past
     that fell to the Ruckig guard — the flattest, latest answer available.
     `k = 1/max(blend, 1-blend)` rescales the ray to reach the box edge.
     OvershootTestThrobbing, window 0-500: **fallbacks 82 of 221 -> 0**, mean
     per-segment excursion 6.23 -> 3.39 mm, flattening 40.4 % -> 36.3 %. THIS is
     the operator's "some strokes go suddenly linear" and it was a ray bug.
  2. **The guard's allowance was the wrong number.** `v0^2/(2*amax)` ignores the
     jerk ceiling — 6.4 mm on paper against ~15 mm in fact — so the knob was
     ~2.3x too strict, rejected near-physical plans into the flat fallback, and
     scored non-monotone in its own value. Replaced by `physicalBandExcess()`,
     the MEASURED excursion of the time-optimal plan (one Ruckig solve per
     waveform commit). Monotone now, and asserted so in the native suite.
  3. **`overshoot_chord_slack` 0.25 makes the guard selective.** An absolute
     bound fires on strokes that overshoot 2 % of their own travel, and each such
     rejection buys a straight line; throbbing is a RATIO. Measured against
     slack 0 (C1, window 150-350): InterpTest1 flattening **35.7 % -> 5.1 %** and
     22 fallbacks -> 0, GoogleCat 23.8 -> 20.8 % and 13 -> 2, SYN-truncated
     30.8 -> 23.8 % and 9 -> 0, at a cost of ~0.5 mm on worst-case excursion.
  **Defaults now `overshoot_guard = 1.0`, `overshoot_chord_slack = 0.25`** — the
  operator's standing request, granted once the knob was worth granting.
  THE COST, ON THE CURVE FAMILY THAT SHIPS (C1; see the fixture-lens correction
  below): 3 of 12 recordings byte-identical to guard-off, 5 within 1.4 points of
  flattening, and 3 paying 5-15 points for a 33-69 % cut in worst-case excursion
  — overshoot-mini **3.67x -> 0.77x its own chord** (45.5 -> 13.9 mm, flat
  21.3 -> 36.6 %), synth-sine 8.48 -> 3.51 mm (35.3 -> 40.4 %),
  OvershootTestThrobbing 20.28 -> 13.69 mm (33.5 -> 39.2 %) with sender rms
  IMPROVING 9.40 -> 8.27. Flattening is the price because bounding excursion
  means abandoning the polynomial on those segments and Ruckig cruises at vmax
  near saturation. One edit reverts it.
  [verified 2026-07-30 — 12-recording shelf sweep at windows 150-350 and 0-500,
  under both `curve=c1` and `client_curve=c1` (identical, as they must be),
  native slopmotion 32/32 (56 doctest cases), `pio run -e sd32-ota` SUCCESS
  (RAM 24.9 % / 81,588 B, Flash 28.6 % / 1,875,812 B), canon_lint 0 findings]
- **RULED — C2 IS NOT THE ANSWER FOR A DISCONTINUITY; IT IS MEASURABLY WORSE.**
  The operator's question, and the intuition is good: if the arc is the cubic
  running out of freedom, give it more. It goes the other way. A quintic must
  additionally match the machine's ENTRY ACCELERATION, and in exactly the
  discontinuity case that acceleration points the wrong way — at the captured
  t=61.85 s handoff the carriage was still accelerating through +800 mm/s when
  the reversal landed, so the quintic is contractually obliged to travel further
  out before it may turn. The cubic ignores `a` and turns immediately.
  Measured (window 150-350, guard off): OvershootTestThrobbing per-segment
  excursion mean 1.85 -> 3.23 mm, **max 20.28 -> 80.30 mm, ratio 37.7 -> 184.9**,
  sender rms 9.40 -> 13.83; GoogleCat max 1.27 -> 7.43 mm, rms 13.82 -> 17.91.
  An extra degree of freedom is only free when it is not also an extra
  CONSTRAINT, and C2's two extra coefficients are spent on boundary conditions,
  not on shape. (Jerk is not the escape either: at 800 mm/s the ramp from 0 to
  amax takes 25 ms and costs 14.8 mm of the ~15 mm stopping distance against
  6.4 mm for the textbook v^2/2a, so jerk IS the dominant limiter — but raising
  it only lowers the physical FLOOR, never the ~28 mm the polynomial invents.)
- **BENCH HAZARD — 10 of the 12 shelf recordings predate the `curve_family`
  column, so under `FollowClient` they replay as QUINTIC, which is not what the
  machine ships.** Only `OvershootTestThrobbing` and `overshoot-mini` carry it
  (both family 1). MFP declares c1_cubic, so **any shelf sweep meant to describe
  shipping behavior must pass `client_curve=c1`** — otherwise it measures a
  family no client sends. This is not a replay bug: an old recording replaying as
  itself is the documented and correct behavior of the appended column. It is a
  reading hazard, and it silently inflated the first pass of the guard's measured
  flattening cost in this same session (reported as 3-15 points on five takes;
  under C1 it is 3 recordings unchanged, 5 within 1.4 points, 3 paying).
- **New bench metric: PER-SEGMENT excursion** (`seg_over_mean_mm`,
  `seg_over_max_mm`, `seg_over_ratio_max`). Take-level `pos_max` vs `cmd_max` is
  blind to this defect class — it read 0.05 mm on the take whose worst segment
  excursion was 38 mm, because longer strokes in the same take reach further.
  `ratio` is excursion / that segment's own chord, which is the scale-free form
  of the complaint: > 1 means the carriage traveled further past the target than
  the move was ever asked to cover.
- **REJECTED, measured, not skipped — two plausible fixes that lost.**
  (a) Planning the guarded fallback TIME-OPTIMALLY instead of stretched to the
  deadline: per-segment excursion came back unchanged across 12 recordings
  (largest move 0.66 -> 0.72 mm, the wrong way) while sender rms rose on 8 of 12.
  Arriving early and holding costs the sender's timing and buys no excursion.
  (b) A second smoothness sweep at full amplitude (`findAlpha(1.0, 1.0)`) after
  the Blend ray fails: it finds more legal shapes and they are worse ones —
  InterpTest1 excursion 0.23 -> 3.42 mm max, rms 2.08 -> 4.20. Both are recorded
  in-place at the code they would have touched.
- **SUPERSEDED — `overshoot_guard` pending entry** (twice: by the hold, then by
  the ruling above, which granted it). Operator asked for 1.0; held
  back, because evidence produced later in the same session argued both ways.
  FOR: it is the only thing that touches handoff overshoot (synth-sine 2.18 ->
  0.22 mm past target; policy, curve family, handoff bound and safety filter all
  measured inert). AGAINST: on that same take it costs reach (1.000 -> 0.958)
  and RAISES flattening (flat_frac 35.3% -> 44.1%), trading an invented
  excursion for the straight-line defect under investigation; and it fails
  test_main.cpp:1486 and :2168, which assert a centering sag the guard removes —
  adopting it means re-measuring those, not relaxing them.
- **New SHAPE metrics on the bench** (`reach_ratio`, `shape_corr`, `flat_frac`),
  because rms conflates offset, lag, amplitude and shape into one number and
  therefore scores a FLATTENED stroke the same as a slightly late one. `flat` is
  the share of moving time the plan held a constant velocity, which is a
  straight line in position — the one that finds a waveform-fallback stroke.
- **Fixture hygiene: `SYN-baseline` was a duplicate of `GoogleCat`** (identical
  raw wire values, arrival and due times to the microsecond; only the DECODED
  columns differed, and replay decodes from the raw ones). It replayed as
  GoogleCat, so any "two recordings agree" conclusion drawn from the pair was
  one recording counted twice. Moved to `retired/`. The other SYN-* fixtures are
  genuinely distinct. **The frozen run `linear-moves-` is mislabeled**: it says
  `recording=SYN-baseline` but rendered GoogleCat's commands.

- **FIXED — the replay bench planned at the LIVE sim's normalized ceilings, not
  the ones its own window implies, so every infeasible-policy comparison ever
  run on it was contaminated.** The engine plans in window fractions, and the
  machine derives `vmax = input_speed / span` on every window change
  (`deriveEngineLimits`). The bench let the window move independently and kept
  the inherited limits — so a 100 mm bench window still planned at the live
  500 mm machine's **2.0/s when the real machine would have had 10.0/s**. At a
  fifth of the true velocity authority the planner is infeasible almost
  constantly, which is exactly when the infeasible policy engages, so the bench
  was comparing five different ways of FAILING at content the machine would
  have rendered cleanly. Rule moved to `MotionCore.h deriveLimits()` (one home,
  both callers); bench `vmax`/`amax`/`jmax` now mean 0 = derive, non-zero =
  override, mirroring the firmware's own `jovr`/sm-set semantics.
  Measured on GoogleCat, window 50-150, curve follow+c1: whole-take sender_rms
  fell **11.3 mm -> 1.02 mm** (stretch) and **18.1 -> 1.14** (reshape), and the
  five policies became IDENTICAL in 9 of 12 sections — because at correct
  limits the content is feasible and the policy never fires.
  [verified 2026-07-30 — 12-section `/api/replay.bin` sweep before and after,
  native slopmotion 31/31]
- **Policy ranking, at correct limits** (GoogleCat, 50-150 window, follow+c1):
  stretch 1.016 / prio-smooth 1.017 / prio-amplitude 1.052 / reshape 1.142 /
  scale 1.780 mean sender_rms mm. Anomaly counts separate them more than
  fidelity does: stretch 13, reshape 28, scale 30, prio-amplitude 43,
  prio-smooth 56. **The default is Reshape.** The spread over it is ~0.13 mm on
  a 100 mm window, so this is NOT yet a ruling — it is one recording.
- **FIXED — the `sender_max` 33.95 mm was the BENCH'S OWN reference line, not
  the machine.** `SenderCurve::note` advanced the sender frame to each
  segment's commanded endpoint, so a segment SUPERSEDED in flight teleported
  the reference: MFP's seek/resume path emits zero-offset re-anchor segments
  (GoogleCat has two, 9 ms apart at t=2.87 s), and `raw` stepped 143.67 ->
  110.00 mm in one sample while the carriage was mid-stroke and tracking its
  command correctly. The frame now continues from where the previous curve
  actually REACHED (`evalCurve` at the commit instant); a span that ran to
  completion evaluates to its endpoint, so the normal chained case is
  unchanged. That event: 33.95 -> 0.92 mm. [verified 2026-07-30 — 1 ms trace
  across the knot + wire-log offsets, native slopmotion 31/31]
- **PENDING RULING — `infeasible_soften` buys back shape by picking the softest
  feasible jerk, and nothing in that search bounds OVERSHOOT PAST THE
  SEGMENT'S OWN ENDPOINT.** `ruckigWorstRatio` scores velocity vs vmax,
  acceleration vs amax, and position vs the stroke WINDOW (+-0.02) — a plan
  that sails past its target and returns is inside all three and scores legal.
  Measured on GoogleCat t=58.30 s, commanded 0.23 over 250 ms: the adopted plan
  peaks at **75.93 mm against a 73.00 mm target (+2.93 mm, 12.7 % of the
  stroke)** at 2.544/s where a clean profile needs 1.380/s — a spurious
  reversal at the top of the stroke the script never asked for.
  FIRES ON THE SHIPPING DEFAULTS: policy Reshape (default) + c1 cubic (what MFP
  declares, RFC-030) + soften (default true). Disabling any ONE removes it —
  soften off 12.16 -> 10.77 whole-take max, c2 -> 0.56 on that second, any
  other policy -> 2.25. Soften off is NOT a free win: it restores the
  flat-topped velocity-saturated straight line soften exists to fix (measured
  here as a dead-flat 0.967/s for 200 ms of a 250 ms span).
  Same defect class as the quintic-path backswing that `overshoot_guard`
  prototypes, on the Ruckig path, which that knob does not cover.
- **NOTE — the +-0.02 window grace still exists on the RUCKIG path**
  (`ruckigWorstRatio`). The 2026-07-30 root-cause removal took it out of
  `quinticWorstRatio` only.
- **Policy ranking, corrected metric + correct derived limits** (GoogleCat,
  50-150 window, follow+c1, whole take): prio-smooth 0.747 / stretch 0.761 /
  prio-amplitude 0.765 / reshape 0.928 / scale 1.647 mean sender_rms mm.
  Anomaly counts: stretch 14, reshape 29, scale 31, prio-amplitude 40,
  prio-smooth 50. **Stretch is the only one that is good on both axes.**
  Still one recording — not a ruling.
- **FIXED — slopsim never asked the hub for the RFC-030 curve family, so every
  MFP segment stream rendered as a quintic.** The firmware has been correct
  since 2.1.75 (`SlopSyncHubService.cpp` calls `Hub::publishCurveFamily()` and
  stamps each pacing entry); slopsim's `onStreamBundle` discarded `session_id`
  and left `WaveformCommand::client_curve_family` at 0, which resolves
  `FollowClient` to quintic. MFP declares `c1_cubic` (1) and its
  `{target, duration, end_vel}` payload IS a cubic Hermite, so the bench was
  rendering a different curve than the machine — the one defect a tuning tool
  must not have. Now stamped from the hub exactly as the firmware does, plus a
  `curve_family` column on the recording CSV (appended, so shelf fixtures made
  before it still load and still replay as themselves).
  Measured on GoogleCat, window 100-400: `follow` + declared c1 is byte-identical
  to forced c1 (sender_rms 54.426 mm) and distinct from forced c2 (56.084 mm) —
  and the cubic is the MORE accurate of the two, which is what the operator
  predicted from the hardware. [verified 2026-07-30 — `/api/replay.bin` A/B
  across `client_curve` and `curve`, native slopmotion 31/31]
- The TUI's `follow(->c2)` readout was retired with it: `follow` now resolves
  per command, so no fixed outcome can be named. Plan kind is the ground truth.

- **RULED — MFP `Samples` and `Segments` carry DIFFERENT SIGNALS, not two
  encodings of one signal.** Read from the plugin source
  (`../SlopSync/clients/mfp/SlopSync.cs`): Segments walks `_segKeyframes` (the
  AUTHORED SCRIPT + value transform) on the script clock; Samples reads
  `Axis::Value` (MFP's FINAL OUTPUT — script plus motion providers, smart
  limits, sync) on a wall clock with velocity as a finite difference.
  `StreamMode` is an enum: they are mutually exclusive by construction, and
  "both at once" is not merely unimplemented, it is not meaningful — it would
  be two contradictory position streams.
  * The red "Axis output diverges from script" box is a CORRECT DIAGNOSTIC, not
    a defect. `CheckDivergence` compares `Axis::Value` against the keyframe
    interpolation; a motion provider moving the axis off-script is exactly what
    it is built to report. The plugin comment is explicit: "warn, never switch
    modes."
  * CONSEQUENCE: a timed move to a position IS a segment. Idle centering ships
    as ONE 0x2101 `{target, duration_ms, end_vel=0}`, which the engine renders
    as a jerk-limited curve arriving at rest. No dual-stream arbitration, no
    protocol change, no machine change. `duration_ms` is u16 → 65.5 s ceiling.
  * TWO KNOWN GOTCHAS, both plugin-side: the divergence probe still fires while
    an off-script segment owns the reference (it compares against keyframes, not
    against emissions) and must be paused; and an out-of-band segment must
    SUPERSEDE pending buffered segments rather than queue among them — the
    pacing ring pops by DUE TIME, so with preview buffering the stale script
    segments would fire after the centering move and undo it. Implement
    centering-as-segment BEFORE preview; the reverse order introduces that bug.
  * Two earlier diagnoses in this session were WRONG and are retracted: there is
    no lost TCode duration on the sample channel (Samples is a clocked position
    stream and needs none), and the "lunge to center" was an artifact of the
    synthetic durationless points the bench sent, not machine behavior.

- **PENDING — idle centering.** "Homing" was the wrong word: the sensorless
  cycle that establishes the zero reference is a different thing. Idle centering
  = park at a rest position after the stream goes quiet, a natural extension of
  the engine's existing SETTLE. Probably machine-side (works for every client
  rather than only the one that implements it). 🚩 It is a PATH TO MOTION THAT
  NOBODY COMMANDED — DOCTRINE §11.3 ("no unmonitored path to motion") applies.
  Needs an explicit opt-in, a live-session gate, and a bounded speed before it
  is built.

- **PENDING — motion-planning architecture.** The five InfeasiblePolicy values
  each win somewhere and lose somewhere because ONE mechanism is being asked to
  solve THREE different problems. Researched direction, in the order the work
  should happen:
  1. **Safety filter** (braking-distance invariant, `v <= sqrt(2*a*d)` enforced
     continuously). The inequality ALREADY EXISTS as `applyEndVelGuard` — it is
     just applied once at plan time to the commanded end velocity instead of
     continuously to the state. Making it continuous lets the ±0.02 legality
     grace AND the arbiter's `entering` accel collapse be DELETED rather than
     tuned. Jerk-exact variant is free: `lib/ruckig/src/ruckig/brake.cpp` is
     already compiled in.
  2. **One reference governor replacing five policies.** A scalar lambda in
     [0,1] — the largest that stays admissible — plus a DIRECTION. The five
     policies are one governor with different search directions: lerping the
     target toward current position is spending AMPLITUDE (prio-amplitude),
     lerping the end handle toward the chord is spending SMOOTHNESS
     (prio-smooth). One scalar + a blend weight gives every intermediate that
     does not exist today, monotone, no cliffs, and it is curve-family agnostic
     (C1 and C2 both unchanged).
  3. **Bad-move bridge** on `T_optimal(from ACTUAL p,v,a) / T_commanded` —
     Reshape already computes that ratio, so the detector is free. Above the
     threshold a command is a DISCONTINUITY, not a stroke, and belongs on a
     jerk-limited point-to-point bridge. Threshold must sit well above 1.0:
     measured, 13.4 % of normal GoogleCat segments already exceed 1.0x demand
     and the max is 2.09x. Demand ratio computed from the PREVIOUS TARGET is the
     wrong discriminator — it ignores current velocity, which is what makes a
     mid-stroke discontinuity violent, and a synthetic 30 s seek scored only
     1.06x (a seek in TIME is not a jump in POSITION).
  4. **Preview / feedforward** (ZPETC, Tomizuka) — DEMOTED BY MEASUREMENT.
     Synthesized 250/750/2000 ms sender lead against an identical due schedule:
     the RFC-008 guard wakes up (`handoff_bounded` 0 -> 10 -> 14 -> 14,
     saturating at 750 ms) but the rendered motion moves 39.29 -> 39.26 mm RMS.
     0.03 mm. GoogleCat sends nearly all-zero handoff velocities, so the guard
     has nothing to bound. Cheap and correct; not a priority for this content.
  5. **MPC** — the general form, with the jerk weight as the smooth-vs-accurate
     dial. Affordable (planning is ~4 Hz, not 1 kHz) but explicitly NOT where to
     start.
  RULED OUT with reasons: TOPP/TOPP-RA and CNC look-ahead feedrate scheduling
  both RETIME FREELY, and the schedule here is external (the next 0x0085
  preempts on the sender's clock); CNC additionally needs lookahead depth that
  measured 0.3 % available. Ruckig Pro waypoints optimizes duration, is the paid
  tier, and Community has no position limits at all.

- **SIM FIDELITY — `SimStepper` is a bang-bang follower and its velocity trace
  is not machine behavior.** Measured: 90.6 % of all ticks apply exactly
  +/-amax*dt, and the acceleration SIGN FLIPS on 68.2 % of ticks; scaling
  `input_accel` 50000 -> 20000 scales the jitter exactly 50 -> 20 mm/s. There is
  no cruise state — the follower only ever commands +/-amax, so tracking a
  setpoint moving slower than vmax chatters every tick. Real FastAccelStepper
  plans a ramp with a cruise phase. CONSEQUENCES: the analyzer's velocity fuzz is
  an artifact, and the `peak accel` readout is meaningless (it is always exactly
  amax by construction). Position excursions over hundreds of ms — i.e. every
  window finding above — are unaffected. FIXED IN THIS SESSION: the velocity
  teleport (a hard clamp let the model shed 950 mm/s in 1 ms, 250 000 mm/s^2
  against a 50 000 ceiling, which MASKED the window runaway by acting as a free
  emergency brake) and a double-spend of the accel budget per step. The missing
  cruise state is NOT fixed.

## Pending rulings

- **WINDOW-EXIT BRAKING RUNAWAY — three-part chain, measured on the async-tune
  bench against the `GoogleCat` funscript recording (2026-07-30). AWAITING AN
  OPERATOR RULING; no firmware was changed.** A carriage that overshoots the
  stroke window by more than 0.5 mm loses ~250x of its braking authority at the
  instant it needs it, and the excursion compounds. Worst measured: **625 mm
  absolute travel on a 500 mm rail** — i.e. into the physical end stop.
  The chain, each link individually defensible:
  1. `slopmotion` `quinticWorstRatio()` grants a **±0.02 normalized window
     grace** (`lib/slopmotion/include/slopmotion/slopmotion.hpp` ~2173) so a plan
     may legally bulge past the rail; the comment's premise is "the sampler
     clamp flattens tiny bulges". The clamp flattens POSITION. It does not
     flatten VELOCITY — measured 69 samples with the setpoint pinned at the top
     rail while the plan still drove outward, worst +217 mm/s.
  2. `MotionArbiter::submitStreamSample` (`src/motion/MotionArbiter.cpp:133`)
     reduces the ACCEL ceiling to the gentle user set whenever the carriage is
     outside the window, keyed on POSITION ALONE. Written for "parked outside,
     glide it in" (correct at v≈0); also fires for "just overshot, must stop
     now", where it removes the authority required. 50 000 -> 200 mm/s2.
  3. The two thresholds disagree by **12x**: the engine may plan 6 mm past the
     rail on a 300 mm window; the arbiter calls 0.5 mm "outside".
  Traced frame by frame: carriage 0.4 mm from a clean stop, crosses 0.5 mm,
  authority collapses in one tick, 16 mm out -> cascade -> 225 mm out for 6.2 s.
  PROPOSED FIX (measured, not applied): keep the gentle SPEED ceiling, never
  reduce the ACCEL ceiling below the input set. Across 54 configs
  (3 windows x 3 policies x 2 curve families x 3 input speeds): runaways
  **11/54 -> 0/54**, worst excursion **306.78 mm -> 1.06 mm (289x)**. The bench
  carries it as the `gentle_accel_outside` lab switch so it stays measurable.
  POLICY EXPOSURE: `prio-smooth` 10/18 configs runaway (worst 306.78 mm),
  `prio-amplitude` 1/18 (100.47 mm), `reshape` **0/18** (worst 0.44 mm) — a
  policy that surrenders reach never drives the rail at speed.

- **RFC-008 handoff guard is inert against MFP** [verified 2026-07-30 —
  GoogleCat, 390 segments]. The guard needs a successor in the pacing ring, but
  MFP sends each segment ~114 ms before its due time while segments are ~252 ms
  apart, so the successor has never arrived when the current one fires:
  lookahead available on **0.3 %** of segments, `handoff_bounded` = 0 in every
  run. The guard written for exactly this client's pathology cannot engage.

- **Four knobs live only in `slopmotion::Config`** — no wire channel, no HTTP
  field, no CLI flag: `infeasible_soften`, `infeasible_soften_floor`,
  `infeasible_soften_steps`, `handoff_chord_factor`. The bench tags them
  `lab` and reaches them anyway; whether any deserves a wire surface is
  PENDING measurement on a real funscript recording. [2026-07-30]
- **STATE-shed floor after the coalescing wire-up — PENDING OPERATOR STAMP**
  (2026-07-28, fw 2.1.85). Not a fully healthy floor: normal-priority STATE
  is not shed until congestion level 2 (correct per the normative table),
  and the 1 s sustained-congestion hysteresis means a ~2.7 s packed burst
  can mostly complete before the signal engages. The next lever
  (burst-aware escalation or a shorter sustain window) risks shed-flapping
  on the hot path, so it is parked for a ruling, not chased.
- **`Reset reason: PANIC (unexpected)` on the post-OTA boot into fw 2.1.86**
  — this repo's documented pattern for an `ESP.restart()`-driven OTA reboot
  is `SW`, so something on 2.1.85 crashed rather than restarting cleanly.
  No serial/backtrace access was available (bench-only per doctrine); the
  device came back healthy on 2.1.86 with no further anomalies. Flagged for
  the operator to watch on the next bench session. [2026-07-28]
- **Pairing-window bit0 (`pairing_window_open`) has never been observed on
  air.** No plain HTTP route opens/closes the pairing window — the only
  paths are the 3-quick-power-cycle boot gesture (needs real reboots) or a
  signed INTENT frame over an authenticated session (which would write real
  pairing state to NVS), so this has been deliberately skipped, not missed.
- **RFC-042 STALE park + reattach over a hard-dropped BLE link with a WS
  client also attached** (the T13 regression scenario, BLE side) has never
  been run live — only the WS-side scenario is live-verified.
- **Square-pulse forensics — still open.** Operator narrowed the trigger to
  MANUAL tape driving (patterns run clean), pointing at the move-INTENT/
  arbiter path rather than the pattern engine. Reproduction plan for the
  next bench window: re-fake-home, one armed wire probe, tap-to-move
  harness drives, correlate outlier values field-by-field. [2026-07-29]

## Landed history (compacted)

- **SlopSim async tune (2026-07-30)** — the analyzer can re-run a saved wire
  recording through a fresh `slopmotion::Engine` under operator-moved settings
  at ~35 000x realtime (10 921 samples of 1 kHz motion in 0.3 ms). New:
  `machine/MotionCore.h` (the live/replay shared seam — PacingRing, SimStepper,
  `decodeWireSample`, `SenderCurve`, `applyArbiter`), `machine/MotionReplay.*`,
  `machine/RecordingStore.*`, `slopsim replay`, `rec.save`/`rec.new`/`rec.list`,
  and `/api/{recordings,replay.bin,rec/save,run/save,run.bin}`. Usage:
  `sim/slopsim/README.md` "Async tune". [verified 2026-07-30 — clean build, CLI
  and HTTP replay agree to the digit (follow_rms 14.840 both), clip/run
  save+recall round-trip, forged run-file forward/backward compatibility]
  - **OPERATOR RULING — the sim's `HttpFacade` is DEBUG SCAFFOLDING**, outside
    the conformant hub surface entirely (a SlopSync-compatible hub needs no
    HTTP), so bench endpoints writing recording FILES do not widen "100 % of
    control goes through SlopSync". The rule that binds: **no HTTP handler may
    write MACHINE state.**
  - A saved **run** freezes its samples instead of re-deriving them from its
    settings — a recomputed baseline is not a baseline. Settings ride as
    `# key=value` lines; a key a run predates reports ABSENT, never backfilled.
  - **The shelf is `%LOCALAPPDATA%/slopsim/`, never the working directory**
    (never next to the exe either — that is `build/`). Field-found: CWD was
    `C:\Windows\System32`, unwritable, and the save endpoint answered
    `{"saved":0}` with HTTP 200. A write that wrote nothing is an ERROR
    everywhere it can be observed; the resolved path + its writability are
    announced at boot, in `rec.list`, and in the analyzer panel.
- **M5c** — SlopSync is the only input/output plane; links2004 stack, `:81`
  telemetry socket, `:55555` TCode server all removed from the build. HTTP
  fallback *polling* remains by design. Story:
  `docs/http-plane-retirement.md`. [verified 2026-07-27 — truth-scrub audit,
  platformio.ini + src grep]
- **HTTP control routes → device-defined SlopSync INTENT channels** — the
  servo pane, `POST /api/slopmotion` and the clear-fault routes are retired in
  `src/ui/WebUI.cpp`; the pairing ceremony (PAIR_REQ/PAIR_GRANT + operator PIN
  pane) is landed in `webui/src`. Replacements: `machine-admin` 0x30F0
  (clear_fault/save_config/servo_scan), `modes-set` 0x3030 (machine modes),
  `sm-set` 0x3120 (SlopMotion tuning). [verified 2026-07-28 — code read,
  `include/comms/SlopSyncCatalog.h` addMachineAdmin()/addModesSet() +
  `ch::` constants]
- **RFC-030 `curve_family`** (registry key 45) landed with fw 2.1.75 glue.
  [verified 2026-07-27 — registry + MOTION-TODO cross-check]
- **SlopSync channel 0x0002 session-roster is reserved, NOT implemented** —
  the registry note that claimed otherwise was corrected. [verified
  2026-07-27 — repo-wide grep, no implementation exists]
- **2026-07-27 truth-scrub fix pass** — `lib/slopsync` client idle-PING
  interval bug (never switched off the idle interval; SPEC §6.5) [verified
  2026-07-28 — native suite 31/31 exit 0]; `webui` session.js EVENT fan-out
  (log/anomaly frames double-dispatched as sessionEvent); `platformio.ini`
  env:esp32-c5-dev1 upload baud set to the documented-broken C5 rate; ~50
  lying comments/stale docs across all areas; docs-site generator unbroken +
  regenerated.
- **Wire-visible British spellings RESPELLED** (pre-release operator ruling —
  "that's a stain that never comes out if deferred"): all catalog strings, the
  schema field respelled to `centering`, the `waveform_centered` token family
  in one pass; device catalog etag changes on next deploy; frozen mini-catalog
  untouched (contains none). [verified 2026-07-27 — canon_lint C-11 0
  findings + native suite + sd32-ota build]
- **Dead code DELETED** (tests green before and after): `MotionInterpolator`
  (moved to `examples/slopmotion_traces/`); `MotionProfile.h` + its test
  suite; `WebUI::handleApiMove/Home/Stop/Pause/Halt/Override/ClearFault`;
  `SystemState` legacy anomaly ring; `UiProtocol.h` dead frame macros;
  `src/s3_main/main.cpp` stub; `Kinematics::planTrapezoid()` + `PlanResult`;
  `ServoMotionExecutor::adoptProfile()`; intiface websocket block. [verified
  2026-07-27 — sd32-ota SUCCESS + native suite]
- **CLAUDE.md split** — preferences only; all rules live in
  `docs/canon/DOCTRINE.md` (engineering) + `CANON.md` (governance).
- **Operator ruling 2026-07-27 (standing): full autonomy** — escalate only
  HMM-grade judgment calls; agent decisions are recorded as veto-able.
- **Phase B** — RFC-043/045/046/047 → Landed (v1.0); RFC-044 → Accepted
  (posture landed, channel deferred). Registry: BLE identity UUIDs
  (534C4F50-5359-4E43-…), UDP discovery port 21328/"SLOP", frames 0x1E/0x1F,
  WELCOME keys 46 `ws_port` / 47 `ipv4`, status field on core channels. SPEC
  §13.1 profiles, §13.8 UDP probe, §6.3 migration, §11.3 loss-policy removal
  reconciled across 8 sections, §9.6 onramp; new docs-site `discovery.md`.
  [verified 2026-07-27 — all generators --check green + native 31/31 +
  sd32-ota SUCCESS + canon_lint 0]
- **Phase C1** — RFC-048, the UI/rendering constitution: new normative
  companion `docs/slopsync/RENDERING.md`; SPEC §19 + §6.1/§6.3/§13.8; registry
  gains eleven frozen vocabulary sections (`ui_categories` 14, `ui_ranks` 6,
  `value_aspects`/`value_scopes`/`value_provenance` 6/3/3, `unit_ids` 23,
  `action_tags` 13, `ui_archetypes` 15 w/ machine-checkable `fallback:`,
  `ui_regions` 5, `renderer_classes` 3, `widget_patterns` 13 w/ 3 `required`).
  Resolves the Phase B veto: DISCOVER_REPLY's `hub_id` becomes
  `hub_instance_id` (u64, `identity_keys` 5, random-once NVS-persisted; reply
  payload 72→76 B). Riding along: RFC-045's `on_disconnect` promoted to the
  registered `field_roles` entry `source.background_run`. [verified
  2026-07-27 — `gen_registry_header.py`/`gen_docs_tables.py` (14 files)/
  `gen_spec_pages.py` (20 files) --check, native 31/31, sd32-ota SUCCESS,
  canon_lint 0]
- **Phase C2** — the device catalog evolution: 0xCDSS renumber + new
  vocabulary fields, one etag bump; clients (probe/MFP/webui-js/sim),
  devicecatalog goldens, fixture re-capture. [landed — see the sim-fidelity
  milestone entry below]
- **Spec fresh-eyes panel (operator-ordered, 2026-07-27)** — 15 vacuum readers
  + convergence: `docs/slopsync/reviews/spec-panel-2026-07-27.md`. 8
  consistently-hated themes, 9 consistently-liked (core doctrines validated
  cold: shedding table 13/15, honesty clauses 12/15, closed motion surface
  12/15, readiness gate 11/15, ground-truth echo 10/15, RFC-045 redesign
  9/15). Hate #2 (`source.background_run` unshipped) required no new spec work
  — it was exactly Phase D, independently validated.
- **Phase C3** — RFC-049 (omnibus of 7 small normative fixes, spec/registry
  side; hub behavior deferred to Phase D per sub-item) + the RFC-044
  correction, both appended to `RFC-QUEUE.md`; RFC-050 appended DRAFT only.
  Numbers allocated: CBOR key `48 requested_curve_family`; NACK `0x0504
  INVALID_NAMESPACE` (transfer band, next free after `BLOB_REFUSED` 0x0503 —
  **judgment call, flagged for veto**: the `0x00xx` protocol band was the
  alternative); registry `limits` gained `segment_handoff_k` (1.5, first
  non-integer limit), `pairing_gesture_boot_count` (3),
  `pairing_gesture_max_uptime_ms` (10000). SPEC touched: §9.6, §7.2/§12.6,
  §8.4 + §18-8/9, §14.3, §12.3, §18-20, Appendix B (key 48) and G (3 limit
  rows). Frozen `conformance/mini_catalog.hpp` + fixture untouched. [verified
  2026-07-27 — `gen_registry_header.py`, `gen_docs_tables.py` (14 files, 108
  dictionary terms), `gen_spec_pages.py` (20 files), `gen_channel_map.py` all
  --check green + native 31/31 exit 0 + `pio run -e sd32-ota` SUCCESS (RAM
  27.4% / 89,920 B, flash 26.1% / 1,709,528 B) + canon_lint 0]
- **Phase D — RFC-042 session staleness, in full.** `HubSessionState::STALE`:
  silence (deadman/idle-reap) and out-of-band transport loss
  (`Hub::detachTransport()`) mark a session STALE via shared `Hub::markStale()`
  instead of tearing it down — slot, `session_id`, grants, intent ring,
  readiness all RETAINED, ownership released unconditionally, nothing
  latched. `Hub::reviveIfStale()`/`Hub::handleReattach()` implement
  resumption; `Hub::findEvictableStale()` implements slot-pressure reclaim
  (best-effort GOODBYE `SLOT_RECLAIMED`). New registry: NACK `0x010D
  SLOT_RECLAIMED`, `session_event_kinds` 4/5. RFC-046's general
  cross-BINDING-TYPE migration is NOT implemented — this hub has one WS
  binding and falls back to duplicate-identity eviction per §6.3's own MAY
  clause.
- **Phase D — RFC-045 hub behavior.** `Hub::releaseSessionSources()` runs no
  Stop-vs-Continue policy dispatch: every release is
  `onSourceOwnership(source, 0, reason)` and nothing else.
  `HubDelegate::sourcePolicy()`/`onDeadmanStop()` remain declared (frozen
  delegate interface) but are dead from the hub's side. **Judgment call,
  flagged for veto:** SPEC §11.3's pre-Phase-D text claimed the
  hub-autonomous `background_run=false` case still latches STOP with
  `cause=deadman`; no public Hub API lets a delegate latch that honestly, so
  the SPEC text was fixed to match the implemented reality instead.
- **Phase D — `source.background_run` shipped** on `pattern-state` (0x1200,
  settingKey 7; paired 0x3200 key 7; bit 6 of `enabled_mask`, unconditionally
  1). NVS-persisted (`pat_bgrun`). `SlopDriveHubDelegate::onSourceOwnership()`
  stops `PatternEngine` on release iff the flag is false. SPEC §18-21 reworded
  from "specified, not shipped" to shipped.
- **Phase D — RFC-049(b)** — `requested_curve_family` (CBOR key 48) echoed
  verbatim in `granted_publishes`/WELCOME/GRANT alongside the effective
  `curve_family` (45). **RFC-049(c) first half** — the firmware's
  hardcoded `1.5f` now reads `slopsync::limits::segment_handoff_k`.
- **OPEN — RFC-049(c) second half (sparse-segment scheduling-depth backstop)
  EVALUATED AND NOT LANDED.** A `commitWaveform()` variant bounding a
  lookahead-less handoff against its own chord was implemented, then reverted
  after it measurably shrank `test_slopmotion`'s "Mixed feasible/infeasible
  chain settles centered and STAYS there" regression bench's characterized
  defect (-23.6 mm -> -9.4 mm) via an unverified interaction with the
  centering/reshape control loop. Recorded in `slopmotion.hpp`'s
  `commitWaveform()` comment; left open. [2026-07-28]
- **Phase D tests** — new `test/native/test_slopsync_staleness/` (5 cases);
  rewritten expectations across `test_slopsync_safety`, `_m3b`, `_m4b`,
  `_m4c`, `_streamingress`, `_devicecatalog`. [verified 2026-07-28 — native
  suite all environments PASSED (exit 0 per TRAPS T10), `pio run -e sd32-ota`
  SUCCESS (RAM 27.4% / 89,920 B, flash 26.1% / 1,712,352 B), canon_lint 0,
  catalog_lint OK (32 entries), all four generators --check green, webui wire
  test ALL PASS]
- **Phase E — BLE GATT `ITransport` + UDP discovery responder + advertising
  (2026-07-28).** BUILD/HOST-VERIFIED ONLY at landing — live status in the
  DEPLOY + LIVE-VERIFY entries below (C-8).
  - NimBLE returns: `h2zero/NimBLE-Arduino@^2.3.0` (resolved 2.5.0);
    `-DBLE_ENABLED` restored to `env:s3_main` (propagates to `sd32`/`sd32-ota`
    by inheritance).
  - `src/comms/SlopSyncBleTransport.{h,cpp}` — the NUS-shaped GATT
    `ITransport` (RFC-043), UUIDs
    `534C4F50-5359-4E43-8000-0000000000{01,02,03}` transcribed from
    registry.yaml `ble_identity` (the codegen does not emit that section as
    C++ constants — documented fallback, not a spec gap). Mirrors
    `SlopSyncAsyncWsTransport`'s SPSC-ring + deferred-attach/detach pattern
    for TRAPS T5. 2 concurrent connections (`SlopSyncBlePort::kSlots`). A
    failed `notify()` IS the §13.1 notify-queue-depth signal. MTU 20 B
    pre-negotiation, up to 247 (250 requested at init). Advertising: service
    UUID + shortened name "SD32" in the primary payload, full "SlopDrive-32"
    in the scan response, one MSD flags byte (company id `0xFFFF`) carrying
    `ble_adv_flags` bit0 `pairing_window_open`/bit1 `ws_available`.
  - `src/comms/SlopSyncUdpDiscovery.{h,cpp}` — the UDP responder (RFC-046
    item 5), port 21328, built on raw lwIP sockets NOT `AsyncUDP`: this
    project's LDF does not resolve the core-bundled `AsyncUDP.h` (confirmed
    live — build failed with LDF's own "no local provider" message). Polls
    non-blocking `recvfrom()` from the hub task's 5 ms tick — no foreign-task
    callback at all, so TRAPS T5's defer discipline does not apply here.
    `DiscoveryRateLimiter`: fixed 8-slot ring, 1 reply/source IP/s, no heap.
  - `include/comms/SlopSyncDiscoveryWire.h` — pure byte encode/decode for
    DISCOVER_PROBE/DISCOVER_REPLY, zero Arduino/NimBLE/socket dependency,
    host-testable (`test/native/test_slopsync_discovery`, 8 cases) — a
    documented fallback for the identity/port numbers a socket binds to and
    an advertising payload builds from, not spec-gap numbers.
  - `hub_instance_id` (RFC-048) — generated once via `esp_random()` x2,
    persisted in NVS, fed to `slopsync::Hub::setHubInstanceId()` and the UDP
    responder's snapshot.
  - slopsync-core additive changes (per the CANON-frozen "extend, never
    reshape" rule): `Hub::setHubInstanceId()`/`hubInstanceId()`,
    `Hub::setEndpoint()`, `Hub::pairingWindowOpen()`, `Hub::catalogEtag()`;
    `WelcomeMsg` gained `ws_port`(46)/`ipv4`(47)/`hub_instance_id`(identity
    key 5). WELCOME's `limits.max_frame` changed from a hardcoded 512 for
    every transport to `min(transport.properties().mtu, kFrameBufferCapacity)`
    — a no-op for WS, honest for BLE, and a fix for a pre-existing §13.1
    violation.
  - Known limitation: no generic control-frame fragmentation over a small
    BLE ATT MTU — matches SPEC §18 item 22's own "no reference
    implementation" admission.
  - [verified 2026-07-28 — native suite 31/31 (exit 0 per TRAPS T10); `pio
    run -e sd32-ota` SUCCESS, **RAM 27.4%→29.4% (89,784 B→96,436 B, +6,652 B),
    flash 26.1%→28.6% (1,710,489 B→1,876,816 B, +166,327 B)**; canon_lint 0;
    catalog_lint OK (32 entries, unchanged); all five generators --check
    green — registry.yaml/SPEC.md not touched this phase]
  - **Main-loop review (2026-07-28): all three flagged judgment calls
    ACCEPTED** (raw-lwIP responder over AsyncUDP; `max_frame = min(mtu, 512)`;
    no BLE control-frame fragmentation).

- **DEPLOY + LIVE-VERIFY (2026-07-28 overnight bench, motor unplugged) — fw
  2.1.77 → 2.1.78, ten-item checklist, all closed.** Build RAM 29.4% /
  96,436 B, flash 28.6% / 1,876,816 B; firmware via `/api/ota`, web UI via
  `/api/ota/fs`. Pre-existing drift caught by the deploy (not a regression):
  the previously-"live" 2.1.77 was a stale build still advertising
  `has_dongle`; post-deploy `/api/capabilities` shows no `has_dongle`,
  `has_ble`/`slopsync_ble` true, and a new `udp_discovery_port` field.
  [verified 2026-07-28 — `/api/capabilities` diffed before/after]
  - **(a) MFP LiveWireTest ×2 back-to-back, no reboot — FIXED + VERIFIED**
    (client-side C# only, fw unchanged at 2.1.81). Cause: auth enforcement
    (fw 2.1.59) needs a `/uitoken` mint in HELLO for `control` tier;
    `clients/mfp-slopsync/LiveWireTest.cs` never minted one, so the
    motion-grant rate read `NaN`. Fix: `MintUiTokenAsync()` (`GET /uitoken`,
    `Convert.FromHexString`, 3× retry on 429), token passed at both
    `HelloAsync` call sites. `/uitoken` is self-serve for LAN clients by
    design — no pairing, PIN, or physical gate; its only defenses are rate
    limiting, a 60 s single-use TTL, and the deliberate absence of CORS
    headers (documented limit, not a gap). Both runs ALL HARD CRITERIA PASS,
    identical `boot_id=0xFA5951E1`, `granted motion-input rate == 50 Hz
    (granted=50.00)`. [verified 2026-07-28 — two live runs vs fw 2.1.81,
    `dotnet build -c Release` 0/0]
  - **(b) Fake-home — PASS.** `/api/machine/homeoverride` is a 410 tombstone;
    fake-home is in-band via `tools/slopsync_probe.py --bench-home` (INTENT
    0x3101 op 2 `force_home`), a deliberate ROUND TRIP;
    `--bench-home-no-revert` added to leave the override ON for a session;
    left `homed=true home_override=true measured_stroke_mm=250`. [verified
    2026-07-28 — `/api/status` before/after]
  - **(c) Full probe pass — PASS, 56/0/2** (opt-in skips: `estop_assert`,
    `bench_home`). Confirms the C4 renumber is what the live catalog serves,
    catalog etag **`9275f578ada7d314`**, WELCOME `ws_port`(46)=82 /
    `ipv4`(47)=192.168.1.229 / `hub_instance_id`(identity_keys 5)=
    **`0x28F1295A0510B8E1`**. Two FALSE FAILURES were in the probe, not the
    firmware, and fixed in `tools/slopsync_probe.py`: `FIELD_ROLES` was a
    stale 16-entry hand-copy of registry.yaml's 32-entry `field_roles` map;
    `pattern_mask`'s homed/estop cross-check tested the whole `enabled_mask`
    byte, but `source.background_run` (bit 6) is unconditionally 1, so every
    unhomed session was a guaranteed false CONTRADICTS (bit 6 now masked
    off). [verified 2026-07-28 — probe run before (2 FAIL) and after (0
    FAIL)]
  - **(d) STALE/reattach live (RFC-042), the `attachTransport()` STALE-slot
    clobber — FIXED + VERIFIED, fw 2.1.78 → 2.1.80.** Root cause
    (`lib/slopsync/include/slopsync/hub/hub_impl.hpp`): `attachTransport()`
    picked a slot by `slot.transport == nullptr` ALONE. A STALE parked slot
    has a null transport by definition, so a brand-new, unrelated
    `instance_id` was handed the parked slot before `handleHello()`'s
    reattach-by-identity logic ever ran. **Governance ruling (main loop,
    2026-07-28):** the CANON freeze on `hub.hpp`/`client.hpp` covers PUBLIC
    API SHAPE (extend, never reshape) and does not shield an internal
    `hub_impl.hpp` bug that defeats a stamped RFC — fix authorized, no
    signature changed, frozen artifacts untouched. Fix: prefer a genuinely
    free slot, else fall back to `findEvictableStale()` — the SAME
    oldest-parked policy `handleHello()`'s slot-pressure branch already used
    (RFC-042 item 5) — severing the victim's transport first. Tests:
    STALE-05/06/07. Native suite 31/31 exit 0; `pio run -e sd32-ota` SUCCESS
    (RAM 29.4% / 96,436 B, flash 28.7% / 1,877,624 B). Live: 2× kill+reattach
    kept the ORIGINAL `session_id`, an interleaved third run confirmed the
    interloper got its own `session_id` while the parked session survived,
    and 20 rapid connect/hard-kill cycles ran 20/20 with uptime monotonic and
    heap flat at ~13.6 KB free. [verified 2026-07-28 — live repro + code
    read, native 31/31, live re-verification]
  - **(e) `background_run` 0/1 disconnect behavior — PASS, both directions.**
    With key 7 = 1 the pattern still reads `running=true` from a second
    client after the first disconnects; with 0 it reads `running=false`.
    [verified 2026-07-28 — scripted 2-client wire test]
  - **(f) UDP discovery — PASS** (unicast, broadcast, rate limit). Unicast to
    192.168.1.229:21328 returned a 76-byte DISCOVER_REPLY matching the WS
    side. Broadcast failed only from a wildcard-bound socket on this
    multi-homed Windows host — test-host artifact, not a device gap. 3
    probes in ~0.1 s got exactly 1 reply (1/source/s limit). [verified
    2026-07-28 — raw-socket scripts]
  - **(g) `hub_instance_id` stability — PASS.** `0x28F1295A0510B8E1` across
    the OTA reboot, a deliberate re-flash reboot, and the UDP reply.
    [verified 2026-07-28]
  - **(h) `requested_curve_family` (CBOR key 48) — PASS.** A HELLO publish
    wish with `curve_family`(45)=3 (`step`) came back as `{45: 1, 48: 3}`:
    key 48 verbatim-echoes the wish, effective key 45 shows `curve_policy`
    downgraded it to c1_cubic — RFC-049(b) working as designed. [verified
    2026-07-28 — hand-built HELLO wish + WELCOME decode]
  - **(i) Heap beacon — DIAGNOSED + MITIGATED (HEAP RELIEF pass, fw 2.1.80 →
    2.1.81).** The BLOB `ConnectionResetError` had TWO mechanisms: (1)
    `SlopSyncAsyncWsTransport::write()` classified `BLOB_CHUNK` (0x1B) as
    ordinary control, so a catalog transfer outrunning the client's drain
    rate armed the control-stall-then-close timer (catalog is 24,581 B / 129
    chunks); (2) genuine heap-exhaustion PANIC — `AsyncWebSocketClient`
    allocates a frame copy BEFORE checking the 32-deep queue, so up to ~10 KB
    could sit on a heap with only ~15 KB free and `maxblock` as low as
    7,668 B (TRAPS T2). ELF inventory named the largest non-mandatory
    internal-RAM reservation: `AppLog.cpp`'s `/api/log` `WebRingSink` (≈17.8
    KB BSS, `httpTask`-only). **Fixes:** `BLOB_CHUNK` got its own
    backpressure class, gated on `limits::blob_chunks_in_flight`=4 (RFC-050)
    — a pure HOLD, never the stall timer; and `WebRingSink` moved to PSRAM
    (`heap_caps_malloc(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)` + placement new).
    **Measured:** RAM 29.4% / 96,436 B → **24.1% / 78,948 B (−17,488 B)**,
    flash 28.7% / 1,877,776 B → 28.5% / 1,869,436 B; boot heap free
    **15,320 B → 32,840 B (2.1x)**, `maxblock` **7,668 B → 22,516 B (2.9x)**;
    steady state free ~31,900 B / min ~15,280 B, vs the old baseline's min
    touching 84-528 B. The 129/129-chunk catalog BLOB now completes every
    time; probe full run 47/0/4. [verified 2026-07-28 — live BLOB repro
    before/after ×2 each, heap beacon before/after OTA, RAM/flash
    before/after, native 31/31 exit 0, probe 47/0/4, canon_lint 0]
    - **Residual (heap):** under a much heavier combined load (full catalog
      BLOB + a 29-channel subscribe-everything batch + a bench `force_home`
      INTENT inside ~2.7 s), fw 2.1.81's low-water mark touched **60 bytes
      free** (steady ~31 KB, `min=60` a sharp transient, no crash). Relieved
      to **164-216 B** by the STATE-coalescing congestion wire-up (Morning
      ruling item 1, fw 2.1.85) and now owned by ACTIVE TASK 1.
    - **The one PANIC reproduced on the fixed 2.1.81 was NOT a heap event** —
      root-caused and fixed as the parked-slot safety broadcast, below.
    - **Hardware/tooling trap (TRAPS-worthy, no TRAPS entry found for it):**
      opening `COM11` via `pyserial` for read-only monitoring RESETS the
      device (`Reset reason: USB`) via the ESP32-S3's native USB auto-reset
      circuit — confirmed live twice. Serial monitoring is not passive on
      this hardware.
  - **(j) BLE advertising — PASS (partial), real radio, real scan.**
    `BleakScanner.discover()` found address `20:6E:F1:31:74:6D`, name
    **"SlopDrive-32"**, service UUID
    `534c4f50-5359-4e43-8000-000000000001`. `manufacturer_data` came back
    empty `{}`. **CLOSED 2026-07-28 (later session):** phone + nRF Connect
    proved a real firmware gap, not OS filtering — the 31-byte advertisement
    had no room for the MSD record and `addData()` failed silently
    unchecked; MSD moved to the scan response (fw 2.1.84, see the BLE
    ADVERTISING MSD FIX entry below, TRAPS T14). [verified 2026-07-28 — live
    `bleak` capture]
  - **Also from this session:** a bad manual `curl` POST to `/api/ota/fs`
    with no multipart body wedged `WebUI::update()` for 209 s — test-harness
    caused, not a firmware regression, never recurred. The reset-reason gap
    it exposed is CLOSED: boot now logs `esp_reset_reason()` by enum name,
    WARN-level for anything but POWERON/SW so it lands in the protected
    Warn+ log sub-ring instead of the Trace/Debug/Info ring the 10 s heap
    beacon recycles inside a minute. [verified 2026-07-28 — `Reset reason:
    SW` in `/api/log` after the 2.1.79 → 2.1.80 reboot]
- **PARKED-SLOT SAFETY BROADCAST — spontaneous reboots FIXED (2026-07-28
  overnight, fw 2.1.81 → 2.1.82).** The three unexplained mid-probe reboots
  above were ONE bug (TRAPS T13), not the heap: `Hub::broadcastSafetyNow()`
  fanned out to a parked slot whose `detachTransport()` had already nulled
  `slot.transport`, loading a vtable from address 0
  (`Guru Meditation Error ... LoadProhibited`, confirmed 2× via serial dump +
  `addr2line`). Fix (`hub_impl.hpp`, same internal-only authorization
  precedent as (d) above): `broadcastSafetyNow()` skips null-transport slots
  — a deliberate SKIP, not a tracked failure, since tracking would age a
  parked session toward eviction for a send never attempted;
  `sendFrameToTracked()`/`sendNackTracked()` also refuse a null transport.
  Regression test STALE-08 (host `SIGSEGV` with the guards removed, PASS
  with them). [verified 2026-07-28 — serial register dump + addr2line, host
  regression test both polarities, `pio run -e sd32-ota` SUCCESS (RAM 24.1% /
  78,948 B, flash 28.5% / 1,869,456 B), deployed via `POST /api/ota` +
  `X-OTA-Token`, FIVE consecutive probe runs `--estop --bench-home
  --bench-home-no-revert` at 50/0/3 each, zero `ConnectionResetError`, zero
  reboots, uptime rising 64,877 → 84,620 ms]
- **Sim fidelity (SlopDeck milestone 1) — LANDED (2026-07-28 overnight).**
  `sim/slopsim` gained `--profile device|alien|minimal` (default `device`).
  `device` = literally `slopdrive::buildSlopDriveCatalog()`, **44 channels
  (12 spec-core + 32 device-range), 24,581 B, etag `9275f578ada7d314` —
  matching the LIVE device's etag**, independent proof of byte-identical
  fidelity; write plane restored for `move`/`home`/`config_set`/
  `pattern_cmd`. `alien` = the prior `SlopSimCatalog.h` benchrig catalog, 21
  channels / 4,272 B, now opt-in. `minimal` = literal device-catalog subset,
  spec-core + `motion`/`move`/`home`, 15 channels / 2,442 B.
  `webui/test/fixtures/slopsim-catalog.{bin,etag}` re-captured; the sim's
  3× `[FAIL]` + write-plane `FATAL` are fixed. [verified 2026-07-28 —
  `slopsync-wire.test.mjs` and `slopsync-sim.mjs` ALL PASS against a fresh
  `device` sim, 3-profile connect/HELLO/catalog/write smoke ALL PASS, `npm
  run check` ALL PASS, canon_lint 0]
- **Sim fidelity 19-channel follow-on (2026-07-28) — LANDED, morning ruling
  item 3.** ONE-WAY PARITY ruling executed: the machine is truth, the sim
  conforms, the firmware is never edited to close a sim gap. The 19
  advertised-but-inert entries (machine-modes, SlopMotion tuning, fray-d
  pattern-advanced + 6 modifier lanes, preset roster/store/cmd,
  machine-admin) now have real, firmware-mirrored behavior; their writes
  previously NACKed `UNKNOWN_CHANNEL`. No catalog change — fixture re-capture
  at etag **`b69eb06249ebe73a`**, byte-identical to the committed fixture and
  matching the live device's post-wire-strings etag. Parity is by
  construction where possible: the sim compiles the firmware's own
  `advpat::Settings`/`BaseControl`/`Modifier` and `PatternPresetStore.h`
  directly. [verified 2026-07-28 — sim rebuild clean, fixture etag match,
  `slopsync-sim.mjs` and `slopsync-wire.test.mjs` ALL PASS, `npm run check`
  ALL PASS, canon_lint 0]
  - **Firmware quirks mirrored deliberately, not defects** (recorded so a
    future client author does not "fix" either side): preset save captures
    only speed/accel + the 6 modifier blocks, never `master` or the depth
    pair; the six modifier-lane channel ids are NOT in `advpat::BaseId`
    order on the wire, while the writer/preset key arithmetic both use
    `BaseId` order; machine-modes keys 1/2 are permanent gaps on both sides.
  - **Sim limitation, still open:** `SimPattern` does not consume `_ap` via
    `advpat::Settings::planStroke()`, so `ap_mode` and the 6 modifier lanes
    are wire-only in `sim/slopsim` with no motion effect — porting the
    firmware's per-half-stroke scheduling loop was judged materially larger
    than this pass.

- **Morning ruling batch (operator, 2026-07-28, on the overnight stamp
  list).** Sequencing ruled: Phase G close-out → (2) wire strings → (3) sim
  parity + SlopBench → (1) coalescing → (5) comment pass; deploys serialize.
  - **Morning ruling item 1 — STATE-coalescing congestion wire-up. Ruling:
    COALESCE, not backpressure. LANDED (fw 2.1.84 → 2.1.85).** The
    coalescing engine (`RetainedStore` + per-subscriber pacing +
    `shedDecision()`) was already correct and normatively tested; the gap
    was that `Hub::setCongestionLevel()` was never called from
    `src/comms/SlopSyncAsyncWsTransport.{h,cpp}`, so `congestionLevel` sat
    at 0 forever on hardware and shedding never engaged. Wired:
    `pollCongestionLevel()` classifies 0/1/2 from `queueLen()` watermark
    hysteresis (SPEC §10.3's 50%/1 s, 20%/5 s) plus the control-stall timer
    for severe. Bench ×3: heap low-water **60 B → 164–216 B** (~3x), no
    crash, 45/0/6 every run. [verified 2026-07-28 — commit `3323e44`, native
    31/31 exit 0, sd32-ota SUCCESS (flash +840 B), canon_lint 0, 3x live
    repro on fw 2.1.85]
  - **Item 2 — punctuation pass:** approved, DONE (fw 2.1.82 → 2.1.83). See
    the WIRE-STRING PUNCTUATION EVOLUTION entry below.
  - **Item 3 — sim division of labor: `sim/slopsim` is the 1:1 DEVICE TWIN,
    PARITY IS ONE-WAY** — the machine is the truth, the firmware is NEVER
    edited to close a sim gap. The "be anything" role moved to
    **SlopBench**, LANDED same date: `.bench` config-file catalog builder
    with zero hardcoded channel knowledge, generic INTENT-clamp/echo/
    STATE-mirror write plane, 3 example configs. [verified 2026-07-28 —
    commit `317b19d`, build exit 0, `smoke_test.py` 12/12 PASS across all 3
    configs, canon_lint 0]
  - **Item 5 — comment standardization DONE.** DOCTRINE §4's comment law
    landed, then a codebase-wide pass (~130 files) plus a follow-up: 855
    section banners across 105 files repadded to column 80; ~23 stale
    `CLAUDE.md §N` pointers repointed. Two law amendments: banner width is
    column 80 (was 76), and RFC-nnn/T-nn inside a banner NAME are POINTERS,
    not the numbering C-12 forbids. TRAPS gained T15/T16/T17.
    `lib/SharedProtocol/SharedProtocol.h` DELETED (C-9: zero includes
    tree-wide, never wired via lib_deps). [verified 2026-07-28 — native
    31/31 exit 0; sd32-ota SUCCESS RAM 78,948 B byte-identical, flash
    1,870,292 B vs a fresh-rebuild baseline of 1,870,276 B (+16 B, the one
    deliberate log-string edit); all trees/tests green; canon_lint 0]
- **SlopSync repo split — EXECUTED and PUSHED (2026-07-28).** SlopSync is its
  own first-class repo (spec suite + registry/codegen, `lib/slopsync`,
  `clients/js` + `clients/mfp`, `hub/slopbench`, verification tools,
  `test/native/test_slopsync_*` minus devicecatalog/discovery, `docs-site`);
  SlopDrive-32 stays the machine repo and consumes it via a pin. Ruled shape:
  two plain side-by-side repos + a VS Code multi-root workspace, explicitly
  NO submodules and NO subtree merges. Licensing: MIT for SlopSync code, CC-BY
  4.0 for spec documents, a NOTICE reserving the SlopSync name for conformant
  implementations; SlopDrive-32's own license unchanged. Mechanics that bind:
  `platformio.ini` uses `symlink://../SlopSync/lib/slopsync`;
  `tools/canon_lint.py` carries the PIN RULE (FAIL if `../SlopSync` missing
  or HEAD != pin); `sim/slopsim/CMakeLists.txt` includes the sibling. Pin
  file: `slopsync.pin` at repo root — its one home. [verified 2026-07-28 —
  SlopSync main pushed (PRIVATE), HEAD == pin, canon_lint 0, catalog_lint OK
  (32 entries), native 31/31 exit 0, sd32-ota SUCCESS RAM 24.1% / 78,948 B
  flash 28.5% / 1,870,292 B, c5_waveshare SUCCESS, all webui tests ALL PASS].
  Both repos have origins and SlopDrive-32 tracks
  `origin/feat/cpp20-slopsync` [verified 2026-07-31 — `git status -sb`].
- **Phase G riders — STANDING DOCS RULES (operator, 2026-07-28); all five
  confirmed executed.** Riders 1-3 (banner hex ids gone, historical docs
  header, CHANNEL-MAP Old-column retirement note) are one-shot and done.
  Riders 4-5 still bind future docs work: **Rider 4** register/channel
  reference pages follow ASD-STE100 Simplified Technical English (no em/en
  dashes, ≤20/25 words per sentence, imperative steps, a banned-word list) —
  scope is register/channel reference pages ONLY, wire-visible catalog
  strings are NOT rewritten by this rider. **Rider 5** the gold-standard bar:
  house voice (de-AI'd), legible standalone mermaid diagrams, clear per-page
  topic scope, the LINK RULE (a named reference with a home gets linked),
  aesthetic matching `webui/src/style.css`, and `DEMO-CANDIDATE:` markers for
  spots agents mark but never build.
- **Phase G LANDED (2026-07-28): docs gold-standard pass + channel-grid page
  + close-out gauntlet.** 1 initial pass + 9 correction sub-sweeps (81
  findings, 77 fixed, 4 left open) plus an independent close-out
  verification round. **35** `DEMO-CANDIDATE:` markers exist across docs
  (implementation parked — see WEBUI PHASE KICKOFF below). Truth fixes: the
  plain-language pages now describe RFC-045's landed park-not-evict behavior
  instead of the pre-RFC-045 deadman-forces-a-stop model; the session-roster
  overclaim is gone; the BLE overclaim downgrade is committed. A tree-wide
  markdown link verifier found 38 broken links (36 were RFC-heading-retitle
  fallout), second run 0 broken. [verified 2026-07-28 — link checker 0
  broken; `mkdocs build --strict` 0 warnings; all six generators --check
  green; canon_lint 0; catalog_lint OK (32 entries); native 31/31 exit 0;
  `pio run -e sd32-ota` SUCCESS **RAM 24.1% / 78,948 B, flash 28.5% /
  1,869,456 B** — byte-identical to the PARKED-SLOT SAFETY BROADCAST build,
  proving the catalog banner-comment strip moved zero bytes]
- **Phase C4 LANDED (2026-07-28; execution spec was `tools/gen_channel_grid.py`'s
  ALLOC dict, stamped 2026-07-27 via the channel-grid visual):** 22 device
  channels renumbered onto the **family-nibble sub-slot convention** — slot =
  [family][member], member 0 = family master; the MIRROR RULE (twin channels
  share domain+family+member digits across class bands); family F =
  admin/meta in every band. Per-channel moves are in
  `docs/slopsync/CHANNEL-MAP.md` (its one home). `pattern-state` (`0x1200`)
  did NOT move — Phase D's `background_run` field rides along untouched.
  `gen_channel_grid.py` now PARSES the live catalog instead of an embedded
  dict, gained `--check`. The banner-hex-id and historical-doc-header
  follow-through this deferred to Phase G is DONE (riders 1-2 above).
  [verified 2026-07-28 — native 31/31 exit 0 incl. `test_slopsync_staleness`;
  `sd32-ota` SUCCESS RAM 27.4% / 89,920 B, flash 26.1% / 1,712,336 B;
  canon_lint 0; catalog_lint OK (32 entries); all generators --check green;
  webui wire test + MFP `WireSelfTest` ALL PASS]
- **RFC-050 — LANDED v1.0 spec/registry side only (2026-07-28); implementation
  deferred and still unimplemented.** New frame type `0x20 BLOB_DONE` (dir
  any, plane raw): `blob_keys` identity fields + `status:u8` (0
  verified-complete, 1 hash-mismatch, 2 aborted), sent by the RECEIVER,
  idempotent like CATALOG_READY — chosen over the draft's recommendation
  because it generalizes to the client→hub direction (a STORE import). New
  `limits.blob_chunks_in_flight` = 4. SPEC §8.4 gained the normative
  backpressure decision table (congested with budget→send, at budget→hold,
  recovered→resume, sustained >5 s→abort with one NACK BUSY +
  `retry_after_ms`); §18 item 24 records spec-landed / implementation-
  deferred. The reference hub gates `BLOB_CHUNK` on the budget but emits no
  `BLOB_DONE` and sends no BUSY NACK on sustained congestion.
- **RFC-048 — LANDED 2026-07-27 (Phase C1), superseding its own narrower
  original scope:** the well-known channel-name vocabulary shipped as
  RENDERING.md §2's STANDARD tier + capability interfaces. Two questions are
  formally PARKED (not forgotten), preserved verbatim in RENDERING.md §2.2:
  multi-axis and actuator types / vibrator support.
- **OSSM-Sauce opcodes — ruled N/A (2026-07-27):** zero OSSM protocol surface
  remains in-tree and no emulation shim will ever be built. Opcode
  compatibility, if wanted, is a third-party client-side adapter.

- **SPEC §18 status reconcile** — all 24 known-limitations items re-checked
  against landed state plus a direct code read; 5 were stale and reworded, 19
  left accurate. Item 8 (blob `INVALID_NAMESPACE`) reworded from
  "Implementation: Phase D" to "open, no phase currently owns it" —
  `Hub::resolveBlobBytes` still answers `CHUNK_UNAVAILABLE` for an
  unregistered `blob.ns`. Item 22 (BLE GATT / UDP discovery) marked shipped +
  live-verified. Item 23 catalog side landed in Phase C2. Item 24's
  hold-not-drop half shipped in fw 2.1.81. Still open (SPEC §18's own
  tracker, not restated elsewhere): no small-MTU control-frame
  fragmentation; cross-transport migration never exercised live across two
  bindings; no reference client builds pages from the full
  rank/aspect/scope/provenance derivation chain; no NACK `BUSY` and no
  reference `BLOB_DONE` emission on sustained blob congestion. [verified
  2026-07-28 — direct reads of `hub_impl.hpp`, `blob_req.hpp`,
  `SlopSyncAsyncWsTransport.cpp`, `SlopSyncCatalog.h`; commit `ea072aa`]
- **Wire-string punctuation evolution** — fw 2.1.82 → 2.1.83. Em/en dashes,
  double-hyphens, and banned prose words purged from every wire-emitted
  `.desc` in `SlopSyncCatalog.h` and from `registry.yaml` desc/note strings —
  prose only, no keys/numbers/names/refs/status touched. Frozen conformance
  artifacts were already clean and stayed untouched (C-11/T11 precedent).
  Catalog etag `9275f578ada7d314` → `b69eb06249ebe73a`; build byte-identical
  to pre-pass. [verified 2026-07-28 — native 31/31 exit 0; canon_lint 0;
  catalog_lint OK (32 entries); `mkdocs build --strict` 0 warnings; OTA'd,
  `/api/capabilities` `fw_version 2.1.83`; probe 50/0/3; BLOB 129 chunks /
  24,585 B etag-verified; commit `274abc3`]
- **BLE advertising MSD fix** — fw 2.1.83 → 2.1.84, closes item (j), TRAPS
  T14. Root cause: the primary advertisement packed Flags+UUID+name+MSD = 32
  bytes, one over `BLE_HS_ADV_MAX_SZ` (31); `addData()` silently returns
  `false` and drops only the overflowing record (the MSD, added last) while
  every setter's return value went unchecked. Fix: advertisement =
  Flags+UUID+`setShortName()` = 27 B; scan response = complete name + MSD =
  19 B — the flags byte now rides the scan response (an ACTIVE scan reads
  it, both `bleak` and nRF Connect do this by default). Every advertising
  return code now checked and logs on failure. [verified 2026-07-28 — live
  active `bleak==3.0.2` scan: `manufacturer_data {65535: b'\x02'}` = company
  `0xFFFF`, payload `0x02` = bit1 set / bit0 clear, matching bench state;
  `/api/capabilities` `fw_version 2.1.84`; probe 55/0/2 then 45/0/6; native
  31/31; canon_lint 0]
## BRITISH-SPELLING TOTAL SWEEP (2026-07-28)

canon_lint.py's `BRITISH_SPELLING_EXEMPT_SECTIONS` keys this exact heading
text to exempt the illustrative bad-spelling strings quoted below from its
own camelCase/subword check — do not rename or remove this heading without
updating that table in the same commit (C-1/C-9).

- **Total sweep** — operator ruling: "every single instance,
  now and for good." Both linters were already codespell-backed and full-tree
  clean going in; the gap was that codespell's word regex treats a whole
  camelCase/PascalCase or combined snake_case token as ONE word. Closed
  permanently by adding `run_camelcase_check()` to `tools/canon_lint.py` and
  `tools/slopsync_lint.py`: splits every identifier/filename on case,
  underscore, digit boundaries and checks every ≥4-char subword against
  codespell's own dictionary. Result: SlopDrive-32 0 hits; SlopSync 1 hit —
  `tools/slopsync_probe.py:235` `"waveform_centred"` → `"waveform_centered"`
  (display text only), which also fixed a drift from this repo's
  authoritative `kSmAnomalyNames[7]`. Bucket B (wire/NVS/storage keys,
  flag-do-not-change) was EMPTY. `slopsync.pin` `aa670db3...` →
  `6317b74e...`. Residual mechanism gap (accepted, out of scope by the
  operator's chosen mechanism): "fibre"/"vapour"/"colonise" are not in
  codespell's builtin dictionary at all; none are present in either tree.
  [verified 2026-07-28 — planted-and-reverted `int colourMode` fired the
  new check in both repos, clean after revert; SlopSync native 16/16 +
  slopbench 43/43 + smoke 12/12 + JS/MFP ALL PASS; SlopDrive-32 all native
  suites PASS, `pio run -e sd32-ota` RAM 24.1% / 78,948 B flash 28.5% /
  1,870,292 B, canon_lint 0 incl. pin check]

## Landed history (compacted, continued)

- **First live BLE GATT session** — fw 2.1.85, NO firmware change: the Phase
  E transport, framing, and MTU behavior worked as deployed, first try.
  SlopSync `1993f95` (pushed) adds `--ble [ADDR]` to
  `tools/slopsync_probe.py` (bleak 3.0.2 bridged via one background
  asyncio-loop thread; oversized frames raise `BleFrameTooLarge` per SPEC
  §13.4's no-fragmentation rule). Client-side bug fixed in the same commit:
  GATT writes now carry a fixed 5 s timeout (were reusing the recv-poll's
  decayed 0.5 s and spuriously timing out on GOODBYE). [verified 2026-07-28
  — live against `20:6E:F1:31:74:6D`: BLE `--listen-only`/`--no-motion` both
  44/0/6, matching the WS baseline; MTU negotiated 250 (payload 247); full
  129-chunk / 24,585 B catalog BLOB pulled over GATT and etag-verified; STATE
  cadence 20.8–22.5 Hz against the 20 Hz grant]
- **RFC-051 landed: critical-stall parks the session instead of evicting it**
  — fw 2.1.85 → 2.1.86, SlopSync `c724b25`. A vanished client's link looks
  CONGESTED before it looks GONE, so §10.4's never-shed stall clock (2 s)
  always outraced RFC-042's own transport-loss park and destroyed a session
  a reconnect would otherwise have resumed. `Hub::detachTransport`'s park
  body is factored into `Hub::parkAndDetach()`, called from the stall-timeout
  branch instead of `evictSlot`; `SESSION_EVICTED` narrows to admin evict
  only. Real bug fixed in the same commit: `Hub::pumpSlot`'s frame-read loop
  assumed nothing inside `dispatchFrame()` could null `slot.transport`
  mid-loop — `parkAndDetach()` breaks that, caught by a new native test
  crashing with `SIGSEGV`, now re-checked every iteration. [verified
  2026-07-28 — new + existing native tests PASS incl. a critical-stall
  reattach case; both linters clean; `pio run -e sd32-ota` RAM 24.1% flash
  28.5%; deployed, device confirmed `2.1.85 -> 2.1.86`; live kill-test showed
  the new park behavior with no `session ... left` line; probe `--listen-only`
  44/0/6 unchanged]
- **VERIFICATION POSTURE RULING (operator, 2026-07-28) — bare minimum until
  current task + UI complete.** Pre-release iteration regime, operator-
  stamped: the verification floor is compile + lint (canon_lint /
  slopsync_lint) + the native suite covering the changed area + one live
  smoke of the actual change after deploy. Dropped until this ruling is
  lifted: fuzz runs, full-gauntlet sweeps on every touch, sim-parity
  re-runs when the sim was not touched, multi-round doc verification
  passes. SlopSync CI still gates every push. Rationale: nobody is using
  this yet, minor changes are frequent, ceremony per minor change is waste.
  Lift at UI completion / first release — "UI complete" is scoped in the
  WEBUI PHASE KICKOFF entry below.
- **BLE dual-central slot test** — fw 2.1.86, `SlopSyncBlePort::kSlots=2`
  live-verified: the operator phone (nRF Connect) held slot 0 while the host
  probe (`--ble`, direct address) claimed slot 1 and ran a full
  `--listen-only` session to a clean GOODBYE — 44/0/6, phone connection
  uninterrupted throughout. Third-slot refusal remains untestable on this
  bench (no third radio). [verified 2026-07-28 — live, both radios]
- **BLE MSD on air confirmed** — operator observed the MSD company `0xFFFF`,
  payload `0x02` (bit1 `ws_available` set, bit0 clear) in nRF Connect's
  parsed AD view against live fw 2.1.85 — the same instrument that found the
  record missing pre-fix (T14). [verified 2026-07-28 — operator phone, nRF
  Connect]
- **Recurring bench facts from this batch (not regressions):** every
  firmware OTA reboots the device and clears the volatile `home_override`;
  restore with `slopsync_probe.py --bench-home --bench-home-no-revert`.
  Native test runs on this host need the winlibs `mingw64/bin` ahead of
  git-bash's own `/mingw64/bin` on `PATH`, or `pio test -e native` silently
  crashes (`STATUS_ENTRYPOINT_NOT_FOUND`, TRAPS T10). Bench end-state after
  this batch: fw **2.1.86**, `homed=true home_override=true` (fake-homed,
  left ON), `estopped=false`, `measured_stroke_mm=250`.

- **WEBUI PHASE KICKOFF (operator + main loop, 2026-07-28) — scope rulings +
  plan.** The phase is COMPLETION + ALIGNMENT of the existing catalog-driven
  Svelte 5 client, NOT a rebuild; the rail/hero identity stays locked.
  Rulings stamped: **"UI complete" (the VERIFICATION POSTURE lift milestone)
  = embedded UI + hosted build config + Tauri 2 shell** — the operator took
  the shell-inclusive scope over the main loop's embedded+hosted
  recommendation; the bare-minimum verification floor holds for the whole
  ride. Telemetry redesign PARKED; the 35 `DEMO-CANDIDATE:` markers stay a
  separate parked pass. **Actual-is-actual:** the firmware's reported
  position IS "actual" for UI-verification purposes; client-side
  hardware-health inference is forbidden. Phase centerpiece finding:
  **RFC-048 vocabulary consumption gap** — the device catalog EMITS the
  rendering vocabulary and neither `clients/js` nor the webui model consumes
  any of it; `roles.js`/`heroes.js` predate RFC-048 and hand-guess the chain
  RENDERING.md later made normative. [verified 2026-07-28 — grep both trees
  + SlopSyncCatalog.h read]
  - **WEBUI PHASE KICKOFF plan** (order agreed, each step live-smoked per
    DOCTRINE §3): 1 truth pass (DONE, below) · 2 Ruling-6: minimal-catalog
    window controls + pattern gen · 3 Tier-0 RFC-048 alignment (`clients/js`
    decodes the vocabulary) · 4 founding Tier-1 completion (SlopMotion
    tuning + fray-d Advanced generator panels) · 5 protocol-surface catch-up
    (RFC-042 staleness/resume UX, curve-family downgrade visibility, BLE/
    discovery in link surfaces, knock-and-approve) · 6 widget interface
    extraction (deliberately LAST, contract from real widgets) · 7 shell
    tail. **RE-ORDERED (operator, 2026-07-28): the shell jumped from step 7
    to the FRONT as a feasibility spike.** Steps 2-6 STILL OPEN; live
    sequencing is in the NEXT STEPS section.
- **STEP 1 — TRUTH PASS DONE (2026-07-28).** RFC-032 tap-to-move end-to-end
  live-verified 15/15 via `webui/test/tap-to-move-live.mjs` (Playwright page
  served FROM the device + an independent wire watcher): two taps landed
  exactly on the wire and matched the UI cursor + numeral 3-way. **An
  fs-only flash DOES reboot the device** (`/api/ota/fs` answers
  `reboot_ms:500`; a stale doc claiming otherwise was fixed).
  `WEBUI-HANDOFF-RFC-BATCH.md` DELETED (SlopSync repo) per its own
  instruction, all items grep-verified absorbed/superseded/shipped/verified
  here. [verified 2026-07-28 — harness ALL PASS ×2 runs, probe 48/0/4,
  canon_lint 0]
- **SHELL FEASIBILITY SPIKE (2026-07-28) — COMPLETE, FULL LADDER
  LIVE-VERIFIED ON THE OPERATOR'S PHONE.** Result: discovery → BLE GATT
  session (watch tier) → WS upgrade → control tier, with machine-log
  evidence of the §6.3 same-identity handover. Toolchain installed: rustup,
  Temurin JDK 17, Android cmdline-tools + platform-36 + NDK.
  - **M0 desktop shell — LIVE-VERIFIED.** `webui/src-tauri/` (Tauri 2.11)
    wraps the existing Vite project; a SHELL branch keyed on
    `TAURI_ENV_PLATFORM` wires native-origin fetch, so embedded-bundle
    purity is provable by build diff (zero Tauri/blec matches in the device
    bundle).
  - **M1 BLE client path.** `tauri-plugin-blec` 0.12 as a WebSocket duck
    through `createSession({WebSocketImpl})`, a seam `session.js` already
    had. BLE sessions land at watch tier by design (no HTTP sideband → no
    `/uitoken`); control arrives with the WS upgrade.
  - **M2 WS upgrade.** SlopSync `77c275d` decodes WELCOME's `ws_port`/
    `ipv4`/`hub_instance_id` (additive); upgrade = drop BLE, reconnect WS to
    the advertised endpoint with the SAME `instance_id` — a clean handover
    on today's firmware via the duplicate-identity rule.
  - **M3 Android — DEBUG APK, live on the phone.** Two blec landmines burned
    down (commit `4fa3a9e`): Android notify/event closures ran on the binder
    thread via JNI and used `blocking_send().expect()` — a receiver dropped
    in a disconnect race turned that panic into a nounwind process abort
    (T5 in Android clothes: foreign-task callbacks must fail soft);
    write-WITH-response is structurally broken on real Android hardware (one
    lost ATT-ack wedged the one-op-in-flight GATT queue forever) — c2h
    writes now go withoutResponse; `subscribe_channel`'s capacity-1
    `try_send().expect()` also panicked on the first burst — fixed by
    vendoring the crate (warn-and-drop everywhere, capacity 1 → 64). Ruling
    from the same round: **no baked-in host** — the shell cold-starts at
    the discovery surface on both targets.
  - **Desktop release builds** (first 2026-07-29, rebuilt 2026-07-30):
    `slopdeck.exe` 16.17 MB, NSIS 3.82 MB, MSI 5.62 MB. Fixed on the way in:
    `tauri.conf.json` shipped an 800x600 window against a 960 px rail
    breakpoint (booted the phone layout) — now 1440x900.
  - **Chrome stacking FIXED (2026-07-29), TRAPS T22** — ShellBar and LinkBar
    were offset from two different origins and z-index-collided; ShellBar is
    now topmost chrome, exactly one bar pads for the notch. Guard:
    `webui/test/shell-chrome-geometry.test.mjs`, deliberately kept OUT of
    `npm run check` (must not spawn a browser during a firmware build).
  - 🚩 **STANDING TRAP, burned live twice: `npm run tauri build` REPLACES
    `webui/dist/` with the SHELL bundle** (`TAURI_ENV_PLATFORM` survives
    tree-shaking). The firmware path is safe (`build_webui.py` runs its own
    `npm run build`), but anything trusting `dist/` as-is is not. Rule:
    after any `tauri build`, run `npm run build` to put the device bundle
    back.
  - **Spike-scope shortcuts still owed at flesh-out:** `http://**`
    capabilities scope; blec upstream issue + vendored-patch retirement;
    release-build cleartext-traffic flag must be flipped before any release
    APK; M1/M2 never live-verified on the DESKTOP shell.
- **FLAGSHIP UI PASS (operator-directed, 2026-07-28) — desktop-shell UX,
  first slice.** Left nav rail ≥960 px (MACHINE = catalog categories,
  CONSOLE = Pairing/SlopSync/Log/Display), collapsible mini rail; phones keep
  the tab strip. **RULING — reserved (wire value 0) ops are NOT rendered**
  (RFC-034: value 0 of an `action.*` select just keeps the array
  index-aligned, it is never an operation — these are buttons, not an
  index-addressed listbox); real ops a session lacks access for stay GRAYED,
  never hidden. Safety dock redesign: e-stop is an OG-language hazard chip
  pinned OUTSIDE the scrolling op row. Terse-instruments mode hides
  `.explain`; **settings pages never hide their descriptions — that split is
  the ruling.** [verified 2026-07-28 — device-knowledge + settings-model
  suites + Vite build green, canon_lint 0, render smoke passed]
- **UX MATURITY PASS (operator-directed, 2026-07-28) — "this feels amateur,
  not mature".** **RULING — one owner per edge:** shell transport chrome
  moves to the top under the LinkBar, the safety dock alone owns the bottom.
  **Desktop = OG fixed-viewport architecture:** at ≥960 px the page never
  scrolls, the pane is the only scroll region. Rail rebuilt 1:1 to
  `main:webui/src/features/rail.js`. **Marker jitter/lag mechanisms:**
  constant jitter = linear interpolation over a ~25 Hz single-sample STATE
  feed → cubic Hermite in `telebuf.sampleAt()`; intermittent lag =
  render-clock slew + a 50 ms extrapolation ceiling losing to >100 ms hub-tick
  gaps → EXTRAPOLATE_MS 80, SLEW_MS_PER_FRAME 4. [verified 2026-07-28 —
  telebuf sim + Hermite assertions PASS, suites + build green, canon_lint 0,
  render smoke 25/25]
- **OG VISUAL LANGUAGE PASS (operator-directed, 2026-07-28) — "apply the
  visual language everywhere".** Root cause of the amateur look: `Field.svelte`
  — the generic renderer every settings control goes through — had NO styles
  at all. The OG control language now lives as style.css defaults + `og-*`
  utilities (verbatim port from `webui-prerefactor`). **Rail ticks were a
  real bug, not styling:** the ruler SVG used an abstract 100×100 viewBox
  with `preserveAspectRatio="none"`, stretching 1px ticks to ~7px — fixed to
  pixel-true viewBox. **RULING — transport row returns to the top (OG
  layout)**; e-stop stays in the fixed bottom dock below 960 px, shows in the
  TransportBar at ≥960 px (both in the DOM, CSS decides). **JITTER ROOT
  CAUSE FOUND AND FIXED — arrival-time stamping.** STATE frames arrive in TCP
  clumps (71 of 393 arrivals with identical timestamps) and `telebuf.push()`
  dropped every duplicate-stamped sample — ~18% of all motion discarded.
  Fix: arrival time treated as a HINT, stamps reconstructed future-anchored
  and evenly spaced by an EMA period. **LIVE CONFIRMED — operator, bench,
  2026-07-28: "the jitter is gone."** [verified 2026-07-28 — canon_lint
  clean, checks + build green, fs deployed to fw 2.1.86, render smoke 27/27]
- **PIXEL FIDELITY PASS (operator-directed, 2026-07-28) — "like someone
  explained it over the phone".** **METHOD CHANGE, standing for all future UI
  fidelity work:** the OG is extracted from `main`, served under Vite, and
  screenshotted as PIXEL ground truth
  (`webui/test/og-reference-shots.mjs`); disputed details are settled by
  zoomed crops, never memory or prose. Landed: true OG two-line transport
  buttons from a registry-vocabulary table; zero-padded fixed-width hero
  numerals; PatternWidget tiles = OG `.pat-grid`. [verified 2026-07-28 —
  canon_lint clean, build green, fs deployed fw 2.1.86, render smoke 27/27,
  side-by-side against the OG reference reviewed]
- **AESTHETIC AUDIT + DENSITY PASS (operator-directed, 2026-07-29)** — "the
  sliders look bad, the page looks flat". Root cause was density: Field
  rendered a bounds caption row and every catalog description inline. RULING
  (amends terse-mode presentation, veto-able): settings descriptions collapse
  behind a per-field ⓘ toggle; `.field-reason`/`.field-error` stay ALWAYS
  visible.
- **CSS DRIFT AUDIT (operator: "honestly diff the css", 2026-07-29)** —
  computed-style + rule-text diff, both pages live. **ROOT CAUSE of the
  residual "off" feel: the missing root scale.** OG sets `html { font-size:
  calc(var(--s) * 16px) }` (17.92 px); the rebuild never set it and pinned
  body to 15 px, so every rem-based size rendered ~11% smaller uniformly.
  Fixed. Slider thumb rule text is byte-identical to OG — **"no sliding
  looking element" is a new design ask, not drift, awaiting operator call.**
- **INCIDENT: HEAP-STARVED HTTP → PANIC REBOOT UNDER SESSION LOAD (2026-07-29)
  — DIAGNOSTIC ONLY; fixed by the FW 2.1.87 entry below.** Several concurrent
  WS sessions plus repeated 270 KB page serves left WS STATE delivery perfect
  while HTTP crawled to 5-8 s loads and sys heap logged `min=60` bytes
  against ~32 KB post-init headroom; ended in a PANIC — the second
  unexplained PANIC on 2.1.86 — no backtrace existed (no crash ring yet).
  Session hygiene lesson (standing): probe fleets against the live hub are
  LOAD — arm ONE probe at a time, never leave harness pages half-open.
- **FW 2.1.87 — crash ring + heap-pressure guards** (operator-stamped,
  2026-07-29), closing the two INCIDENT work items. Crash ring
  (`include/system/CrashRing.h`/`.cpp`): RTC_NOINIT last-words ring (boot
  seq, heap min/last, maxblock last, 12 alloc/lock-free breadcrumb
  checkpoints), recovered on next boot and served at `GET /api/crash` — NOT
  a backtrace (needs a core-dump partition, does not OTA; serial-reflash
  bench item, still queued). Heap floors: new WS sessions refused below
  free<14336 OR maxblock<6144 (checked pre-slot-claim; existing sessions
  never touched); page serve answers 503 below maxblock<12288. First-boot
  proof: `/api/crash` on 2.1.87 showed two ws-refuse crumbs firing during
  the fs-flash heap fragmentation. **Pressure snapshot under ~42 min of live
  churn** (`webui/test/evidence/pressure-snapshot-20260729-014239`): heap
  min touched **40 bytes**, guards held (ws-refuse, 503 at maxblock 7668, no
  panic), heap snapped back to ~30 K free / 15 K maxblock once clients
  detached — churn fragments transiently, does not leak. Verdict at the
  time: 3-4 concurrent sessions is the honest ceiling of the ~32 KB
  post-init internal headroom, with moving slopsync's ~110 KB internal-heap
  footprint to PSRAM floated as the structural relief — **superseded by FW
  2.1.88 below: the actual pressure was ghost sessions, not footprint
  size**, so the PSRAM move is not currently a tracked open item. Same
  deploy: webui LimitsWidget reuses Field (second hand-rolled slider
  deleted). [verified 2026-07-29 — deploy 2.1.86 -> 2.1.87 + fs, render
  smoke ALL PASS, canon_lint clean]
- **Pairing proven end-to-end + MFP settle fix (2026-07-29).**
  Knock-and-approve always worked — every layer (hub lib, trust NVS,
  catalog, transports, JS client, PairingPane) was already implemented and
  lib-tested (M4B-12..26); only PIN-mode
  (`SlopSyncHubService::openPairing`/`closePairing`) has **no caller yet**
  (comment-marked as a planned caller — still true, confirmed live in
  `src/comms/SlopSyncHubService.cpp`). First full round trip recorded
  (`webui/test/pairing-roundtrip.mjs`, both modes): push-to-pair -> first
  knock grants `configure` on a fresh ledger -> token reconnect via the
  trust ledger -> second joiner's knock parks (pending STATE + knocked
  EVENT) -> operator approves over session-admin -> PAIR_GRANT -> token
  reconnect at the approved tier. ALL PASS. Sim gap closed: slopsim's
  `validateToken` now consults its own PairingManager first, then floats
  bare sessions at `control` (never `configure`). MFP settle fix shipped
  (SlopSync `540325f`, installed into MultiFunPlayer 1.34.5): segment wish
  5->20 Hz sustained / burst 25->50 Hz — dense passages starved the
  emitter's token bucket, eroding the 120 ms lookahead and firing the hub's
  settle brake mid-stroke. [verified 2026-07-29 — build + WireSelfTest +
  LiveWireTest --segments ×2 back-to-back]
  - **Still open:** live-device PairingPane two-tab check (bench nicety,
    not blocking); real-content MFP playback check (operator's own call).
- **FW 2.1.88 — WS idle-RX reap: ghost sessions were the pressure
  (2026-07-29).** Operator correction of the 2.1.87 pressure-snapshot
  verdict: a silently dead peer (locked phone, killed tab, no FIN) never
  goes stale at the transport level — it passes `cleanupClients()`/
  `hasClient()` forever, holding its slot and heap, and the T19 accept
  floor fires BEFORE HELLO processing, so ghost-held heap refused the very
  connect whose slot-pressure path is the only other evictor (a deadlock).
  Fix: WS idle-RX reap, `kWsIdleReapMs=20000` (ten missed ~2 s
  proof-of-life PINGs) force-closes silent clients; the close lands as
  RFC-042's transport-closed staleness trigger, so the session parks for
  reattach as designed. `ws-idlereap` crumb added to the crash ring.
  [verified 2026-07-29 — deploy 2.1.87 -> 2.1.88, Playwright session forced
  offline reaped at 20001 ms with a clean deferred detach, `/api/log`]

## Known residuals (bench-measured, July 2026)

- SlopMotion v3 chase trails a dense source stream ~35 ms
  (estimator-smoothing lag; regression estimator is the upgrade path if it
  matters on hardware). [bench 2026-07 — traces harness]
- Chase acceleration spikes to ceiling at stream reversals (aim overshoot at
  turn points; cosmetic in position — verify feel on hardware). [bench
  2026-07]

## Pending operator rulings

- **CLAUDE.md is gitignored** — the covenant is not in version control; one
  clean checkout loses it. Track it (or an agreed public variant)?
- ~~tools/slopsync_probe.py untracked~~ — RESOLVED by the repo split: the
  probe lives tracked in the SlopSync repo (`tools/slopsync_probe.py` there);
  this repo's copy concern is moot. [verified 2026-07-29 — C-10 scrub]

## Active plan — SlopDeck (gold-standard client & widget system)

- Design ratified 2026-07-27: `docs/slopdeck/DESIGN.md` — three tiers, the
  Prime Rule (plugins go through SlopSync, never around it), founding Tier-1
  set, sim catalog profiles (`device` default / `alien` / `minimal`),
  sequencing (sim fidelity first).
- §8 RULED 2026-07-27: embedded UI = thin client, Tier 0+1, on hubs with the
  capability to serve it; hosted plain-http instance is the universal /
  UI-less-hub (WROOM) path; Tauri shell is the premium delivery (discovery,
  Tier 2 plugins, non-WS transports). PWA is NOT a viable delivery (https
  requirement blocks ws:// to LAN — recorded in DESIGN.md §8 so nobody
  promises it later).
- Transport rulings CALIBRATED 2026-07-27 (second pass): hardware hub profile
  = BLE GATT MUST (floor: infrastructure-free control, discovery,
  provisioning) + WS SHOULD/preferred; ESP-NOW = supported-not-developed peer
  binding; sim/hosted hubs exempt (RFC-043 recut). Client onramp ladder: TCode
  passthrough → native segments → native samples (RFC-044 Draft — **operator
  clarification on passthrough + streaming clients still PENDING** before it
  firms up). Intake doctrine: SlopSync is the only way in and out of THIS
  machine; other firmwares are never forced.
- SlopDeck delivery accord + Svelte-5 framework ruling recorded in
  `docs/slopdeck/DESIGN.md` §8–9 (one kernel two faces; framework-neutral
  plugin ABI insurance).
- **`OssmBleService` REMOVED 2026-07-27** (the XToys ruling made the compat
  argument moot): service files deleted; TransportManager/main.cpp/WebUI
  unwired; `TransportMode` 4 tombstoned (never reuse); a stored NVS mode 4
  migrates via the existing >BT clamp. [verified 2026-07-27 — sd32-ota SUCCESS
  + native suite 31/31 post-surgery]
- **RULING 2026-07-27: the transport switch dies** — "there's nothing to
  select, the only option is SlopSync." WS_OP_MODE, `TransportMode`,
  `applyTransport`, the NVS "transport" key and the selector UI were all
  vestiges of the pre-SlopSync multi-protocol era. SCOPE RULED kill-it-all:
  SerialTransport, BleTransport (NUS), DongleTransport, TransportManager (WiFi
  duties extracted to `src/system/WifiLink.{h,cpp}`), TCodeParser +
  TCodeAxisState glue, the selector, and the NimBLE dependency (zero consumers
  until the BLE GATT ITransport lands). Intiface TCode users wait for
  RFC-044/native — accepted consequence. **SURGERY LANDED 2026-07-27**;
  WS_OP_MODE 0x06 and `/api/mode` gone, `applogSerialDedicated` gone. RAM
  30.0% → 27.4% (89,920 B), flash 28.9% → 26.0% (1,705,208 B, −~190 KB).
  [verified 2026-07-27 — sd32-ota SUCCESS + native 31/31 + canon_lint 0]
  - Residual pass: `commanded_raw_mm` writer restored on the SlopSync drain
    path (raw_10um telemetry regression), `intiface_compat` deleted (zero
    consumers), `-DBLE_ENABLED` removed (stops reserving BT controller memory
    for a stack not in the build; the `#if` machinery stays for the BLE GATT
    transport's return), `has_dongle` capability advert dropped. RAM/flash
    unchanged at 27.4% / 26.0% (89,920 B / 1,705,208 B). [verified 2026-07-27
    — sd32-ota SUCCESS + native 31/31 + canon_lint 0]
- RULING 2026-07-27 (expanded same day): **deadman-as-safety retired
  wholesale** (RFC-045 recut): liveness stays as bookkeeping (STALE/reattach/
  slot reclaim); source-loss forced-stop REMOVED for all classes (streaming
  settles by physics); autonomous sources (PatternEngine, generators) get
  explicit `on_disconnect: stop | continue` (default stop); 0x0005 explicit
  stops unchanged. Implemented — see the Phase D entries above.
- RFC-044 (TCode passthrough) DEPRIORITIZED by operator: "a later feature,
  parsed machine-side" — a channel alongside segments and samples, not
  near-term work.
- C5 co-processor plan: DEAD AS PLANNED (operator 2026-07-27) — it existed to
  stream TCode v3 at 333 Hz, which SlopMotion + TCode v4 obsoleted. The
  concept may return; `c5_waveshare`/`c5_tdongle` envs + sources stay for now.
  ESP-NOW posture: criminally easy to enable, supported, not developed.

## Deferred / planned (homes: docs/REFACTOR-ROADMAP.md, docs/MOTION-TODO.md)

- TCode pass-through channel (post-MFP; parser cross-task race was the
  blocker).
- Native Intiface SlopSync support (replaces the deleted :55555 bridge).
- Telemetry redesign (parked by the WEBUI PHASE KICKOFF ruling above);
  C5-node SlopSync transports; merge to `main`. Tauri 2 shell moved INTO
  the webui phase by the same ruling — no longer deferred.
- **BLE transport-level idle-RX reap — not implemented.** BLE has no
  equivalent to the WS idle-RX reap above; a wedged-but-connected central
  holds its GATT slot + heap indefinitely (link-layer supervision timeout
  only reaps dead radios, T19 class). Shape when wanted: mirror the WS
  sweep in `SlopSyncBlePort::loop()` (per-slot last-RX stamp, sweep vs a
  `kBleIdleReapMs`, force-disconnect -> RFC-042 park); T3 back-to-back
  live verify mandatory. [recorded 2026-07-29 — C-10 scrub]
- **Webui rapid-fire punch list, queued behind the authoring-legibility
  campaign:** manual slider entry box, ⓘ centering, intent/pending glow
  redesign, power-bar max ticks + click-reset + hover-read + per-category
  reset-all, session ms -> h:m:s (click for ms), telemetry-rate trace
  (position vs plan-strip), reset-to-default buttons, label casing.
  `_webui.handleCommand` WS_OP bridge collapse — future milestone. YAML
  codegen sugar — only if tables prove insufficient. Session-gate/closeout
  system (C-13 proposal + ledger diet + tiered canon loading) — designed
  2026-07-29, implement after campaign Phase 0.
- **Four probably-superseded probe scripts** in `webui/test/`
  (`gap-probe`, `tap-probe`, `jitter-measure`,
  `render-vs-samplerate-probe`) — left alone during the 2026-07-29 tree
  cleanup because they are hand-written source, not output; never
  triaged.
- **Extract `MachineCommand` from `WebUI.cpp`** (operator-approved
  2026-07-27, NOT STARTED) — `handleCommand` + the `apply*` mutation family
  + post-clamp echo building move to an HTTP-free class the SlopSync
  delegate calls directly; what remains of `WebUI` becomes honestly the
  HTTP plane. Follow-up riding on it: migrate the extracted core to TYPED
  intent handlers, dissolving the WS_OP/JSON shim op-by-op until
  `UiProtocol.h` is nothing.
- **Clocked-logging + legacy-log audit** (operator 2026-07-27, NOT STARTED)
  — inventory every periodic/cadence log (heap beacon, `[sys]` lines, rate
  reports) and MotionArbiter's logging specifically; verdict per line: earns
  its keep / demote to SLOGD / delete. Rider: strip MotionArbiter's sediment
  in the same pass (the retired OSSM_STREAM source slot's wire label row,
  interpolator-era comment vocabulary).
- **SlopLog + SlopGlow uplift pass** (NOT STARTED) — bring the two elder
  modules up to the slopmotion/slopsync core standard (injected clock,
  purer hardware-free core, tighter conformance tests).
- **RFC CANDIDATE — batched telemetry sub-samples per STATE push** (the OG
  0x01 design, generalized). Client-side smoothing is now at the
  interpolation-order ceiling of a 25 Hz single-sample feed; if the Hermite
  + arrival-stamp fixes do not reach OG smoothness, this is the real cure.
  NOT implemented, queued for operator stamp as an RFC. [2026-07-28]
- **Sim: fray-d Advanced pattern has no motion effect** — `SimPattern` does
  not consume `_ap` via `advpat::Settings::planStroke()`, so the 6 modifier
  lanes and `ap_mode` are wire-only in `sim/slopsim`. Porting the firmware
  `PatternEngine`'s per-half-stroke scheduling loop is the work. [flagged
  2026-07-28 — 19-channel follow-on]

## Authoring-legibility campaign + C-10 scrub (2026-07-29)

- **Campaign (operator-approved plan):** SlopSync authoring legibility —
  the catalog reads like the UI it renders. Phases: scrub (done, below) ->
  etag pin (landed: `test_slopsync_devicecatalog` pins
  `B6 9E B0 62 49 EB E7 3A`; flips ONCE, at the Phase-6 sweep) ->
  RENDERING.md §3 fix + AUTHORING.md + RFC-052 (SlopSync repo) -> reference
  client implements the §1 derivation chain -> `slopsync::author` tables ->
  byte-identical catalog port -> derived cold encoders + hot-path layout
  guards -> operator sweep + deploy. **Phases beyond the scrub are STILL
  OPEN.**
- **Ceilings ruling (operator-derived safe values), NOT YET APPLIED:**
  **1000 mm/s speed, 60000 mm/s² accel, across the board.** Lands at the
  Phase-6 sweep (moves catalog `.max` annotations, one planned etag bump).
  Live TODO pointer: `src/motion/AIMServoDriver.cpp` `setAcceleration()`.
  May be tuned up later.
- **RENDERING.md §3 flag ruling:** ledger + shipped catalog win — Phase C2
  already wired `ui_categories` onto entries; RENDERING.md §3's stale
  "until a later catalog-evolution RFC" note and `clients/js/frames.js`'s
  5-entry `SETTING_CATEGORY_NAME` are the stale halves. Fix lands in the
  SlopSync repo (campaign Phase 0).
- **Dead-code ruling — DONE per C-9:** `moveTo`/`streamTo` interface +
  impls, `handleApiHomeOverride`, ramp fields, seq counters,
  `PIN_NEOPIXEL` alias, `esp32-c5-waveshare` board json, `FanoutOutput`.
  Proofs in the deletion commit messages.
- **C-10 scrub record:** 17 agents, 59 raw -> 55 deduped -> 39
  adversarially verified findings, 0 refuted, +16 lows (sibling SlopSync
  repo covered only via seed findings, not swept). Fixed as part of the
  same pass: MotionArbiter's always-dispatch comment cited a stream-stall
  watchdog that is disabled in D4; WifiLink's "drops to serial TCode
  control" log described a deleted fallback; README.md still described
  the pre-retirement transport zoo. [verified 2026-07-29 — truth-scrub
  wf_ac7c5f00-855, findings applied this commit]

## Tree cleanup (operator: "if it doesn't spark joy get rid of it", 2026-07-29)

Repo went **13.7 GB -> 618 MB**; nothing with content was deleted outright,
only moved. Archive home: `../SlopDrive-32-archive/2026-07-29-cleanup/`
(48 MB, outside the repo root) — dated soak runs, simrun captures, the
three `SD32-*.md` dated reports, root `build/`/`flashpack/`/`tools/`
scratch, obsolete `intiface/`, retired webui evidence, pre-strip README.
LEDGER entries citing `SD32-OVERNIGHT-REPORT.md`/soak JSONs by name still
resolve there.

- 13.1 GB was regenerable build output (`webui/src-tauri/target` 12 GB,
  `gen/android/app/build` 1.1 GB, `gen/schemas`, `webui/dist`); 118 MB was
  three vendored `.git` histories (`GIT_SHALLOW` never took on the
  FetchContent clones). `sim/slopsim/build` kept `slopsim.exe` +
  `compile_commands.json` in place (`~/bin/slopsim.cmd`/`SlopCLI.cmd`
  launch that exact path; clangd has no other compile db for `sim/`).
- `intiface/` retired (described the deleted NUS TCode BLE service).
  README stripped to a stub (license/attribution kept substantive —
  CERN-OHL-S v2 conveyance obligations are not tidyable prose).
  `reference/` created for the AIM datasheet/PCB/`.md` reference files;
  the four firmware pointers in `ServoModbus.{h,cpp}` repointed.
- **Two load-bearing `.gitignore` defects fixed:** `tools/*` was excluding
  `tools/ota_auth.py` (named `pre:` in every `-ota` env — **a fresh clone
  could not build any OTA target**) and `tools/catalog_lint.py`
  (DOCTRINE/TRAPS-binding). Both whitelisted now.
- Stale claims corrected in CHANNEL-MAP.md, slopdeck/DESIGN.md,
  REFACTOR-ROADMAP.md, ws-transport-baseline.md.
- Kept deliberately (load-bearing in use despite historical origin):
  `docs/ws-transport-baseline.md`, `docs/webui-legacy-diagnosis.md`,
  `docs/http-plane-retirement.md` (~15 inbound pointers incl. five
  firmware comments).

**PsychicHttp retired + GLM branch deleted (operator ruling 2026-07-29):**
"psychichttp is dead, and the glm branch, both can go." The sync
`WebServer` + `IdleGuardWebServer` is now the only HTTP backend, permanently
(not transitional — REFACTOR-ROADMAP §4 CLOSED). Removed:
`[env:sd32-psychic]`/`[env:sd32-psychic-ota]`, the `USE_PSYCHIC_HTTP` B-side
of `OtaService.{h,cpp}`, `src/ui/SlopHttpServer.cpp` entirely (C-9: its
whole body sat inside the never-defined `#if USE_PSYCHIC_HTTP` guard, so it
compiled to nothing in every shipping env). `SlopHttpServer` KEPT as a name
(57 call sites across 11 files; collapsing it is a wide rename for zero
functional gain). Branch `T2WebuiGLM` deleted (1 unmerged commit `b2db0fb`;
recoverable via reflog: `git branch T2WebuiGLM b2db0fb`). [verified
2026-07-29 — `pio run -e sd32-ota` SUCCESS (RAM 24.2%, flash 28.5%) and
`-e sd32-async-ota` SUCCESS, not deployed (no shipping env ever defined
`USE_PSYCHIC_HTTP`)]

## Agent tooling set up (2026-07-29) — clangd/LSP, playwright, ponytail scope

Operator installed frontend-design, claude-md-management, playwright,
typescript-lsp, clangd-lsp, plus ponytail. Facts worth keeping because
they are invisible in a fresh clone:

- **clangd** is PlatformIO's Espressif fork (`tool-clangd-esp`, v21.1.3, no
  LLVM install); its bin dir + `toolchain-xtensa-esp-elf/bin` added to the
  user PATH via the registry API (`setx` truncates at 1024 chars, PATH is
  1386). `.clangd` is gitignored in both repos (absolute host paths) — its
  trap comments are the only record of three mechanisms: the ESP fork
  defaults to a riscv32 triple, fallback names an uninstalled `clang`
  driver, and clangd infers a command for db-absent files from the nearest
  firmware TU (dragging IDF includes into a host parse); fixed via
  `Compiler:` + a deliberately-empty `CompilationDatabase:`. SlopSync
  needed its own `.clangd` (host g++, `gnu++2b`) — had none.
- `typescript-lsp` needs TypeScript 5.x; `npm i -g typescript` installs
  7.x (no `lib/tsserver.js`, rejected outright) — pinned to 5.9.3.
- Playwright MCP defaults to the real Chrome channel (absent on this
  host); repo `.mcp.json` pins `--browser chromium`.
- **claude-md-management DISABLED** — its improver rewrites CLAUDE.md
  against generic templates (would inline rules that belong in
  CANON/DOCTRINE, a C-1 break), and CLAUDE.md is gitignored so a bad
  rewrite is not `git checkout`-recoverable.
- frontend-design KEPT, scoped to precedent-free surfaces (DOCTRINE §3
  binds the house look). Ponytail scope is an operator ruling, binding
  home DOCTRINE §4, not restated here (C-1).

## OPERATOR UI FEEDBACK QUEUE (2026-07-30) -- 3 of 7 CLOSED, 4 still open

Phase 1b's verdict was **lamps approved, steppers unchallenged, no veto on
`DRAG_TICKS_MIN`**, plus this punch list. Closed items are RIPPED per this
list's own discipline -- the durable record is the commit, and the mechanism
lessons are TRAPS T23/T24/T25.

**CLOSED (2026-07-30, `eb1d900` / `6d8bab9` / `a83673b` / `e41daac`, deployed
and live-verified):** item 1 the off-center info glyph, item 2 dropdowns
printing their value twice, item 6 the activity grid "not scrolling left".
Two of the three had a RECORDED DIAGNOSIS IN THIS LEDGER THAT WAS WRONG, which
is the part worth carrying forward. Item 1 was blamed on the glyph's ink
balance -- worth 0.18px at an 11px render, invisible -- when the cause was
un-reset UA button padding shrinking the content box below the glyph's own
width, putting it 3.5px off, a quarter of the control. Item 6 was written up as
"probably NOT broken, likely reduced-motion or genuinely flat data" when the
history buffer was in fact being refilled with zeros ~25x a second by a leaked
`$effect` dependency. Both were found by MEASURING the live DOM after arguing
from the stylesheet twice, and the operator's screenshot -- every column gray
but the rightmost -- was the evidence that broke item 6 open.

STILL OPEN:

3. **`raw_10um` is a meaningless label** (renders "Raw 10um" on the Motion
   tab, rank=diagnostic). `_10um` describes the WIRE unit; the field ships
   `unit: "mm"`, `unitId: mm`, `scale: 100`, so the operator never sees 10 um
   at all. Siblings `pos_10um`/`tgt_10um` have the same wart but are
   hero-claimed by role and rarely drawn loose. Suggested: `demand` /
   `position` / `target`, whose meanings are already spelled out in the descs
   and in `value_provenance` (demand/planned/actual).
   [FLAG] **THIS IS A WIRE CHANGE, NOT A RENAME -- TRAPS T11.** Catalog field
   names are protocol bytes: they move the etag and invalidate the pinned
   fixture. The campaign budget allows exactly ONE etag bump, at Phase 6. Fold
   it in there or not at all. Safe on the client side -- `roles.js` binds by
   ROLE and the device-knowledge gate forbids name knowledge in `webui/src` --
   but the C++ STATE packer and any vector fixtures move with it.
4. **Cards are too wide to track left-to-right.** At 1440px a label sits ~1200
   px from its value. The card body should flow into columns rather than one
   full-bleed row per field: `repeat(auto-fit, minmax(~320px, 1fr))` on the
   field container is the cheap version. Watch two things -- sliders need
   enough width to stay draggable (see `DRAG_TICKS_MIN`, same concern from the
   other end), and the OG reference is the arbiter of the final cadence, so
   shoot it against `og-ref` rather than inventing a grid.
   **Largest remaining UI item: it reshapes the surface every other settings
   change lands on.** UNBLOCKED (2026-07-30 ruling, shedding entry below).
   Sequencing note: Phase 6's sweep is the operator editing annotations ON this
   surface, so landing item 4 first means sweeping once instead of twice.
5. **Position telemetry jitters in the Tauri shell but not on the
   device-served page.** Same source, same bundle build, so the difference is
   environmental -- do NOT start by editing the smoothing. Bisect in this
   order:
   (a) **access tier** -- the device page mints control via same-origin
   `/uitoken`; the shell mints through `tauri-plugin-http`. If the shell lands
   at watch, subscription rate drops and interpolation starves. Read
   `machine.link.roles` in the shell window first, it is one glance.
   (b) **subscribed sample rate** -- compare the actual arrival cadence, not
   the requested one.
   (c) **rAF cadence** -- WebView2 is not Chrome; if the render clock assumes
   ~60 Hz vsync and WebView2 delivers something else, the interpolation the
   jitter fix relies on breaks even with a perfect stream. TRAPS T18 is the
   background (arrival-time stamping destroys the timeline).
   Existing instruments answer all three without new code:
   `webui/test/position-jitter-probe.mjs`, `render-vs-samplerate-probe.mjs`,
   `jitter-measure.mjs`, `streamed-outlier-probe.mjs`.
   **Now one operator click away:** a current desktop shell exists (see the
   shell entry), so step (a) is a glance rather than a build.
7. **Phone layout: DEFERRED by operator ruling** until the desktop layout is
   settled. Do not spend passes there; it will churn again.

## OPERATOR UI RULINGS LANDED (2026-07-30) -- beyond the punch list

Taken in the same session, all deployed, all veto-able:

- **The info glyph is a LETTER, not a drawn icon.** Martian Mono 400 at 10px;
  the operator chose it against a rendered 1x comparison of five candidates
  (the OG `i-info` sprite and a Chakra Petch cut were the runners-up).
- **A description reaches the reader exactly ONE way, and `terse` is the
  switch.** Verbose prints it inline under the control; terse moves it to a
  hover tip on the info button. Never both. The info button renders ONLY under
  terse -- in verbose it was an affordance for revealing text already on
  screen. Click-to-pin is kept on every pointer type AGAINST the operator's
  instinct that it should be touch-only: touch has no hover, the keyboard uses
  `:focus-visible`, and on a pointer device the click pins a multi-line tip so
  it can be read without holding the pointer still. One media query reverses
  it. ThemePicker's help text and its terse comment both claimed settings
  pages always keep their explanations; both were corrected, not left to rot.
- **Slider values are typeable.** A slider was the one writable numeric with
  no way to enter an exact number, so its head chip is now an input (commits
  on `change`, never per keystroke). Readouts deliberately did NOT become
  typeable -- no `setting_key` means effective truth, not a control.
  `commitNumber()` is the single clamp path for both typing and the stepper's
  nudges, so the two cannot clamp differently.
- **The intent echo pulses out from the handle it was thrown from.** Pending
  sends an annulus wavefront out from the slider handle every 500ms
  (`OVERDUE_MS`, so the second wave and the amber escalation land together);
  confirm expands one filled reality-blue disc until the outline is lit, then
  fades uniformly over 900ms (`SETTLE_MS`, so the animation cannot outlive the
  state it draws). `fault` still does not pulse. The echo is cropped to the
  CONTROL's band, not the whole field, and expands past the 2px track to
  enclose the thumb (T24).
  [FLAG] This draws an OUTLINE where style.css's state block says inset-only.
  The rule's stated reason is that a state change must not shift layout, and
  the echo lives on an absolutely-positioned pseudo-element that shifts
  nothing -- so the constraint is honored, not bent. The rule now says so and
  points at Field.svelte.
- **NOT GATE-VERIFIED ON HARDWARE: the echo crop (`e41daac`).** Its three
  assertions are written and will run on a device that is not shedding page
  loads. Everything before it passed `flagship-render-smoke` at 43 assertions.

## INCIDENT: 4-CLIENT PANIC + PAGE-SERVE SHEDDING (2026-07-30) — DEFERRED, NOT BLOCKING

**OPERATOR RULING (2026-07-30, second pass): this is an EDGE CASE and it
blocks nothing. Deferred to the pre-merge stability campaign.** The evidence
that reclassified it: the operator held F5 down across three windows for
several minutes while streaming motion into the machine and watching telemetry
on three more clients, and saw no shed and no panic. That is a harder load than
the sequential-`curl` measurement below and it passed, so whatever the `curl`
run caught is narrower than "roughly every third load." **Browser-based
verification is therefore NOT blocked** — the previously recorded consequence
for UI work is struck, and punch-list item 4 and the plan strip are open.
Reasoning of record, operator's: chasing a fault into code that the campaign
may replace is wasted work, and mega-stability testing lands as its own pass
before the merge to `main`.
One correction to that reasoning, recorded because it changes where the work
would start if it were resumed: **campaign Phases 2–5 do not touch this code.**
The suspect paths are `handleRoot` and the WS attach path in `WebServer` /
AsyncTCP; the campaign reshapes the catalog and its encoders. The deferral
stands on the edge-case finding, not on pending replacement.

Everything below is the captured measurement, kept as the starting point for
the stability campaign. Raw data is in
`tools/diag/crash-20260730-4clients/` (gitignored -- crash.json, log.txt,
log-preDeploy.txt, status.json, caps.json, truncation-sample.txt). This entry
holds only what changes a DECISION; the numbers live in those files.

**The panic.** Operator connected 4 clients while idly prodding and the device
PANICked. `/api/crash` for the previous boot: `reset_reason PANIC`,
`boot_seq 10`, `heap_min 524`, `max_block_last 2548`, 13 crumbs. The crumb
trail is all `http-root` / `ws-attach` / `ws-detach`, and the final two crumbs
are `ws-attach@344210` then `ws-attach@446733`.

**The decision this changes: it died on the SESSION-ATTACH path, not the page
serve.** 2.1.95's load-shedding covers `handleRoot` only -- entry gates on
`maxblock >= 4096` plus `free >= 12288`, and it re-checks free inside the send
loop. The attach path has no equivalent gate, and this is the same signature as
the 2.1.92 candidate that panicked (`heap_min=260`, `max_block_last=124`). Any
future work on the accepted-503 posture should start here, not on the serve.

**The serve is ALSO worse than this ledger currently claims, and that claim is
corrected here.** The PAGE-SERVE FLOOR entry above says the covered window is
"first load, during pressure" and that a clean boot reads 41,928 / 31,732.
Measured 2026-07-30 on a FRESHLY BOOTED, CLIENT-FREE device, after boot-heap
init settled: sequential `curl` loads of `/` go full, full, then TRUNCATE
(115,970 / 115,970 / 6,800 bytes). Roughly every third load sheds. Earlier in
the same session, loads 1-3 went full and 4-5 truncated at 25,840.

**A browser cannot complete a navigation at all right now.** `curl` gets two
full loads back to back; Chromium fails every time, because a browser opens
MULTIPLE CONCURRENT connections where curl makes one sequential request, and
the concurrency is what drives the transient dip. The ledger's accepted "a 503
from two tabs opened at once is ACCEPTED" is now being hit by a single tab.

**Why sampling will never show you this:** the abandon fires between beacons.
`[355.112 W ui] handleRoot: body abandoned mid-send (free=9468 maxblock=7668)`
and one second later `[356.163 I sys] heap free=35648 maxblock=24564`. The
10-second `[sys] heap` beacon reads healthy through the whole event. That is
T19's addendum demonstrated live -- fragmentation latches and is visible at
entry, exhaustion is a transient that arrives DURING the transfer.

**The consequence-for-UI-work paragraph that stood here is STRUCK** by the
ruling at the head of this section. It claimed browser verification was blocked
and that punch-list item 4 and the plan strip were blocked with it; the
operator's three-window F5 pass under stream load disproved it. Kept as a
pointer only so nobody re-derives the blocked claim from the measurements
below. Confirm with one `flagship-render-smoke` run before a visual pass.

Also observed, no action owed: `min=136` and `min=200` in the heap beacon on
boots during this session, i.e. the floor is being touched routinely, and
`http:ui.update blocked ~1000ms` warnings cluster around the sheds.

Not an incident: the operator latched the e-stop deliberately while prodding
("no biggie"), and `flagship-render-smoke` was verified NOT to fire it -- the
assertion only reads `!button.disabled`.

## PLAN STRIP -- SCALE THE SEGMENT TO TRAVEL (operator-ruled 2026-07-30, NOT STARTED)

**Ruling: the lane represents FULL TRAVEL, and `window.min`/`window.max` map
the planned segment into it. The operator classed this a visual mod, NOT a
spec-driven change** -- and that holds, because every fact it needs is already
role-tagged on the wire. Verified before accepting it: `window.min`/
`window.max`, plus `geometry.max_travel` (`SlopSyncCatalog.h:543`) and
`geometry.measured_travel` (`:589`).

Why it currently fills the lane: `plan.start_norm`/`end_norm` are u16,
`unit: "norm"`, scale 10000, and declare NO min/max, so PlanStrip's `pct()`
takes its already-normalized branch. Normalized against the stroke window, a
stroke spanning that window is always 0->1 -- the whole lane, every time.

[FLAG] **Do NOT copy the mapping into PlanStrip.** `RailWidget.svelte:150-170`
already derives exactly this fact -- `lo` from the window field's own `min`
annotation, `hi` preferring `geometry.measured_travel` over the configured
ceiling because it is ground truth from this session's own home, then `span`
and a `pct()`. Duplicating it is one-fact-two-homes (C-1), and the precedence
is subtle enough to drift: the naive `max.max` fallback draws the rail 4x too
long on this device (window.max caps at 2000mm, the rail is ~500mm). Extract it
into a shared helper both widgets consume.

**C-12 violation to fix in the same pass:** `RailWidget.svelte:145` still says
`geometry.*` is untagged -- "RFC-041 is filed, not landed" -- and calls its
fallback permanent behavior. Both roles ARE tagged now, so the comment lies and
the fallback it documents is dead on this hub.

UNBLOCKED (2026-07-30 ruling, shedding entry above); still NOT STARTED. The
standing condition is not the device, it is the discipline: this is canvas
drawing work, and shipping visual work blind is what produced two wrong
diagnoses on 2026-07-30. Verify against the rendered thing.

## SSManager RULED (operator, 2026-07-30) — one door over the whole tool surface

**Operator ruling: build it, name is SSManager.** The rules it must obey are
DOCTRINE §10 (binding form); this entry holds scope, the measured case, and
what is still veto-able.

**The measured case.** The surface is **~85 distinct entry points across 5
languages and 5 build systems**: 10 Python tools over both repos, 27 `.mjs`
probes in `webui/test/`, 16 SlopSync native suites, 7 fuzz harnesses, 11
PlatformIO environments, 8 npm scripts, slopsim, slopbench, mkdocs,
`smoke.ps1`, and the C# plugin build. The operator's complaint was not the
count — it was that nothing says which results are still true.

**What it is, in one line:** a developer clones a repo, runs SSManager, and it
reports what toolchains exist, what is green, and what went stale since the
code moved — with no flags to look up.

**The staleness mechanic is the product, not a feature.** Results are stored
against a hash of each entry's declared inputs; when inputs move the tile goes
STALE (gray), never failed. This is CANON C-4 mechanized: today a human or an
agent has to remember that a green run from two commits ago is hearsay.

**Design constraints, all operator-confirmed:** funnel the surface, never
reduce it — no tool gets deleted or dumbed down; a funnel covering 80% is
WORSE than none, because it adds a second place to look; every tool stays
runnable standalone; adding a tool is a manifest entry and never a code change.
All four are DOCTRINE §10 now.

**Stack: reuse the Tauri shell** (Rust + the existing Svelte/vite chain).
Cross-platform portable binary is already solved there, and it makes the
catalog inspector free — SSManager IS a SlopSync client, rendering through the
same `roles.js`/`Field.svelte` path a real client uses, so the inspector cannot
drift from what users see.

**Honest limit, state it in the UI:** SSManager orchestrates host toolchains,
it cannot contain them (PlatformIO alone is hundreds of MB). The first-run
capability probe turns that limit into the best onboarding feature — "34 of 85
gates runnable; PlatformIO is missing and unlocks firmware build + OTA."

**SEPARATE BINARY from the SlopDeck operator shell — operator-stamped
2026-07-30**, sharing stack and components. Bundling a test runner into the app
that drives a moving machine was scope nobody asked for.

**THE UI IS THIN (operator ruling).** The view renders manifest data and
results; it holds no logic worth testing. Everything real — discovery,
spawning, hashing, staleness, capability probing — lives behind it in Rust.
The test for this: SSManager's behavior must be exercisable with the UI closed.
A frontend that grows its own state machine is the same failure as a console
that grows tool-specific branches.

**ONBOARDING IS THE POINT, and it is DEFERRED (operator: "not a concern for
now, but some logic to build into it").** The target flow is: clone the
SlopSync repo, run SSManager, and be walked into developing a client or hub
firmware. v0 does NOT build the guided flow. What v0 owes is a schema that can
carry it later without a migration — entries need ordering, prerequisites, and
a statement of what each one unlocks, because those are exactly what a guide
reads. Adding those fields costs nothing now and is a schema break later.

**v0 scope (the clone-to-productive path only):** manifest schema, process
runner, input-hash staleness, results grid, capability probe. ~15 manifest
entries, NOT all 85 — the rest arrive by accretion under DOCTRINE §10.
**Acceptance criterion, testable:** registering a new tool touches zero lines
of SSManager code.
v1 adds the catalog inspector and sim launcher (mostly wiring existing
components); v2 moves `smoke.ps1`, OTA, and the log viewer behind buttons.

## HUB IMPLEMENTATION COST — MEASURED (2026-07-30). SDK proposal AWAITING RULING.

Measured, not estimated, so the next pass does not re-derive it. SlopDrive's
hub side is **7,619 lines** on top of the 18,315-line library, and **~3,200 of
them (42%) are code every ESP32 hub author rewrites identically**: the WS/BLE/UDP
transports (1,803), crypto (345), the `IClock`/`IRandom` binding (52), and
~1,000 lines of plumbing inside `SlopSyncHubService.cpp` (task loop, log
bridge, signing shuttle, `cfg_gen` pump, trust-ledger NVS, pairing window, OTA
park/revive). The coupling is thin BY MEASUREMENT, not by impression: the three
transports touch SlopDrive at 27 log-macro calls and 2 crash crumbs, and
`SlopSyncCrypto.cpp` reads exactly one machine field (`state.ota_active`).

Why this is a location problem and not a design problem: the lib already
forbids itself from naming a platform source (SPEC §17.2), `ITransport` lives
at `transport/transport.hpp`, and the lib already ships one implementation of
it (`inprocess_binding.hpp`). An `esp32` port follows precedent.

**The authoring campaign does not cover any of this** — it reshapes the
machine-specific half (Phase 3 halves the catalog, Phase 4 the encoders) and
never touches the platform half.

Client side, same measurement: the JS client (3,969 lines) IS a library, so a
new JS client costs nearly nothing; `clients/mfp/SlopSync.cs` is **3,861
hand-written lines** re-implementing the wire because no C# library exists.

**PROPOSED, no ruling yet:** (i) move the ESP32 port into the lib behind a
build flag — pure relocation, etag-neutral; (ii) a C# emitter in
`gen_registry_header` (it already emits C++ and JS, so a third target follows
the pattern and kills the transcription-drift class TRAPS T20 records);
(iii) do NOT extract the `SlopSyncHubService.cpp` plumbing yet — its seams
would be invented rather than found, and the parked WROOM-32D port is the
forcing function that shapes them honestly.

**Spec-only fresh-eyes review, TRIAGED 2026-07-30 — recorded so it is not
re-raised.** An outside model reviewed SPEC.md with no access to the
implementation and recommended ~15 items. Nearly all were already built, and
several are implemented more carefully than recommended: the ESTOP magic scan
(`estop_frame.hpp`, which resumes at `i+1` not `i+4` because `0xE5` runs can
overlap), deadman (11 files + `test_slopsync_safety`), the readiness gate
(`test_slopsync_readygate`), the `arg > remaining` bounds idiom (already a
constraint comment at `cbor_reader.hpp:174`), mutation testing (7 fuzz
harnesses + corpus), `static_profile.hpp`, deferred HUB_SIG, bundle limits,
and the honest-limitations duty (the spec's numbered HONESTY CLAUSES + §18,
normative in that they MUST NOT be denied). Three were rejected on merit:
**ISR-level ESTOP scanning** (a scan loop plus CRC-32 over an
attacker-controlled buffer, in an ISR, is a self-inflicted DoS on the motion
loop — the ring-plus-high-priority-task shape is correct), **tinycbor**
(a regression against a canonical encoder with fuzz harnesses and a pinned
etag), and generic sample-sizing advice already covered by normative limits.
**Operator ruling on the e-stop cluster: null and void — a physical e-stop is
the integrator's and hardware manufacturer's responsibility; the webui exposes
it and that is the extent of our duty.** Confirmed compatible with the spec,
which already says exactly this normatively (H2: preemption is per-hop, "not
magic end-to-end latency").
Two live items came out of it: a YAML catalog frontend (do NOT fork Phase 2/3
for it — the byte-identical port is the safety net, and YAML becomes a
mechanical frontend over the table type afterward; `registry.yaml` →
`gen_registry_header` is the working precedent) and pre-encoding the catalog
blob at build time, which buys ~nothing here (`ss:ctor` is 240 B) but is a real
lever for the no-PSRAM WROOM-32D port — recorded against that parked item.

## FREEZE STATE RULED (operator, 2026-07-30): NOTHING IS PINNED YET

**Ruling, verbatim intent: everything is up in the air until the protocol has
been put through its paces. Then one commit gets pinned and locked, and after
that only documentation, clients, and tools move.** The lock event is the
**release pin**: one commit sha, not a tag — a sha is stricter because a tag
can be moved.
**Two different things are called "pin"; do not confuse them.** `slopsync.pin`
is the BUILD pin — which SlopSync commit SlopDrive compiles against — and it is
bumped routinely, gated by canon_lint. The RELEASE pin is the one-time freeze
this ruling is about. Same mechanism, unrelated events.

**Measured 2026-07-30: the SlopSync repo has ZERO git tags**, while SPEC.md's
header read "Document version: v1.0 (public)". The spec's whole freeze
discipline keys off "the v1.0 tag forward" in at least eight normative places
(SPEC §5.7 never-renumber, §16 fixture freeze, §19; RENDERING.md §14 and its
status line; RFC-QUEUE lines 29-30 and 751). So the freeze was **written but
not armed**, on a PUBLIC repo, while RFC-QUEUE explicitly plans a registry
regeneration *at* the pin. A stranger reading it would reasonably build against
numbers we still intend to move. **Corrected in the same session:** the header
now reads `v1.0-draft (public, not yet pinned)` and a FREEZE STATE notice sits
above §0 saying the clauses bind nothing yet and that consumers must pin by
commit sha. That notice is deleted at the pin, and its deletion is the
announcement.

**TWO ETAGS, ONE COMPATIBILITY SURFACE — do not conflate them, it makes the
plan look far more constrained than it is:**
- `B6 9E B0 62 49 EB E7 3A` — SlopDrive's DEVICE catalog etag. Per-hub;
  clients re-fetch on mismatch by design. Changing it is NEVER a protocol
  break. The campaign's "exactly ONE etag bump at Phase 6" is self-imposed
  test-fixture and cache-churn discipline, not a compatibility rule.
- `F4 A2 8F BB 58 CE D1 6A` (775 bytes) — the CONFORMANCE mini-catalog fixture
  in SPEC §16. Frozen at the pin; changing it after is a genuine protocol
  break.
Consequence: Phase 6's bump, the ceilings ruling, and punch item 3's
`raw_10um` rename are free forever. RFC-052(d)'s entry key 17 is safe on either
side of the pin — SPEC §4.3 requires decoders to ignore unknown CBOR map keys
at any nesting level, so additive keys are forward-compatible by construction.

**Accumulating LOCK-DAY checklist** (nothing here is owed before then; it is
recorded so the pin is not spent half-ready): the planned registry
regeneration/renumber (RFC-QUEUE 29-30) MUST land before the pin, not after;
CHANNEL-MAP Old-column retirement; the `(was 0x...)` comment sweep; RFC-052(b)'s
deferred half (the §5.4 reorder/insert-before-tail lint, which needs the golden
shape the pin records); re-freeze of the conformance fixture pins; delete the
FREEZE STATE notice.

## SESSION CLOSEOUT 2026-07-30b — introspection, SSManager, freeze state

No code shipped, no deploy, no firmware change. This was a decisions session:
four operator rulings, three of which corrected something the repo was
asserting falsely. Commits: SlopDrive `20ad5ad`, `b049f46`; SlopSync `7a430bd`,
`37b6366`. Both repos clean and ahead of their remotes by exactly those; no
divergence, nothing behind.

**SSManager schema v1 LANDED (SlopSync `7a430bd`)** — `ssmanager/SCHEMA.md`
plus a real `ssmanager.toml` with 8 tool entries and 6 toolchains. Data only,
no console code: the schema shape is the expensive thing to get wrong, so it is
argued before it is built. Two of its rules exist ONLY because the probes were
run instead of trusted, which is the same lesson as the previous session:
- **`probe` is a candidate LIST.** `pio --version` fails on this host while
  PlatformIO is fully installed at `~/.platformio/penv/Scripts/`, and mkdocs
  lives in `docs-site/.venv`. A single-command probe tells a developer to
  reinstall tools they already have — exactly the failure SSManager exists to
  prevent. A toolchain id appearing as `argv[0]` is substituted with its
  resolved candidate; that is the ONLY substitution, and a template language
  would be a 🚩.
- **Candidate paths resolve from the REPO ROOT, never the shell cwd.** Caught
  by a probe reporting `docs-site/.venv/Scripts/mkdocs.exe` missing while run
  from the sibling checkout. The binary was there the whole time.
Verified: the three entries whose toolchains exist here were run from the
manifest's own argv, all exit 0 (slopsync_lint, `gen_registry_header --check`,
JS wire suite ALL PASS). The two fuzz entries need clang, which is genuinely
absent — UNRUN, not passing.
**Still open, worth settling before the Rust:** whether `error_exit` earns its
place (only slopsync_lint uses it today) and whether `group` stays free text or
becomes a fixed set.

**Note for whoever builds SSManager: ponytail does NOT govern it.** DOCTRINE §4
bars minimalism mode from the SlopSync repo, which is where SSManager lives.
The schema is deliberately complete — reserved onboarding fields, explicit
`error_exit` — rather than trimmed to today's need.

**Measurement corrected, mine:** SlopSync is ALREADY its own repo with its own
remote. An earlier statement this session that the split happens "around v1.0"
was wrong; the split executed 2026-07-28.

## ⏭ NEXT STEPS — SUPERSEDED 2026-07-31 by "## ⏭ THE QUEUE" near the top of this file

**Do not work from this section.** It is kept only because the campaign-phase
detail below (Phases -1/0/1a/1b receipts, RFC-052 disposition, the etag pins)
is still the one home for those facts. The ORDER it states is dead: the
operator re-ranked everything on 2026-07-31 as stability/diagnostics ->
gigagauntlet -> webui -> nice-to-have. Read THE QUEUE for what to do next; read
this for what the campaign already did.

## (historical ordering) NEXT STEPS (restamped 2026-07-30b after the introspection + SSManager + freeze-state session)

**SUPERSEDED AT THE HEAD (operator, 2026-07-31): ACTIVE TASK 1 (memory/crash)
AND ACTIVE TASK 2 (logging/observability) COME FIRST — everything below is
queued behind them.** Verbatim intent: "the logging and crash fixing is the
most important task right now, that's first up". Reason it outranks: the
campaign's own verification runs against a machine that panics mid-stream, so
every gate it claims to pass is a claim about an unstable substrate.

**ORDER OF OPERATIONS (operator-ruled 2026-07-30, supersedes the bare item
order below, itself now queued behind the ACTIVE TASKs):** SSManager v0 →
campaign Phase 2, registering its gates as the
first real manifest content → UI punch item 4 (card columns) → Phase 3 →
SSManager v1 → Phases 4–6.
**Immediate next action:** ACTIVE TASK 1 items 2–5 (the one `custom_sdkconfig`
batch) and item 6 (bounded WS TX queue), per the 2026-07-31 ruling above.
**Immediate next action ONCE THE ACTIVE TASKs CLEAR:** SSManager v0's Rust half — manifest parse,
capability probe with candidate resolution, process spawn, input-hash
staleness. The schema it consumes is landed and verified (closeout above);
settle `error_exit` and `group` first, both one-line decisions.
**Nothing is frozen and nothing is owed to a tag** — see the FREEZE STATE
ruling. Work freely; the lock-day checklist is what the release pin costs when
it eventually happens. The reasoning: building SSManager after the campaign
means the campaign's own verification runs through the scattered surface that
prompted it, and building all of SSManager first stalls the campaign — so v0
is deliberately small and the campaign is its first customer. Every phase
writes its gate list down either way; under DOCTRINE §10 it goes in the
manifest instead of a ledger paragraph, at no extra cost.

The answer to "what's next on the ledger":

1. **Campaign: SlopSync authoring legibility** — plan APPROVED and
   RE-VALIDATED post-scrub (2026-07-29, operator + main loop):
   `~/.claude/plans/pure-crafting-thacker.md`. Status: Phase −1 (scrub) DONE,
   applied, committed. Etag pin DONE (`B6 9E B0 62 49 EB E7 3A`,
   test_slopsync_devicecatalog). Next up, in order:
   - **Phase 0 DONE (2026-07-29, SlopSync `1a930e6`, pin bumped):**
     RENDERING.md §3 corrected per the recorded ruling; the SAME stale
     vocabulary found and fixed in SPEC.md §8.1 key table + §8.8 Categories
     (still said setting_categories, 0–127/128–255) and the registry
     tombstone's four-value-for-five miscount; spec/AUTHORING.md written
     (reverse-chain hub-author quickstart, pointers only); **RFC-052
     authored, status PROPOSED at the time — ALL FOUR PARTS NOW STAMPED,
     see the RFC-052 entry below** (a: author tables, b: released marker,
     c: JS vocab codegen, d: entry key 17
     `group_descs`) [veto-able; written exactly to the approved plan's
     scope]. **(d) RULED IN (operator, 2026-07-29, SlopSync `f93353f`):**
     card descs a necessity — context factors out of field desc budgets;
     first-in-catalog-order duplicate rule added at the ruling. (a)–(c)
     still awaiting stamp. Bycatch: SlopSync's docs-site generator had been failing
     since RFC-051 landed (missing SOURCE_LINKS anchor) — fixed, 4 stale
     generated pages caught up. Gates: slopsync_lint 0,
     gen_registry_header --check clean, etag pin test green post-bump.
   - **RFC-052 (a)(b)(c) STAMPED (operator, 2026-07-29)** — RFC now reads
     ACCEPTED on all four parts. (a) ruled in STAGED: home is the SlopSync
     lib, `field_spec`/`channel_table`/`catalog_feed` in Phase 2 and
     `packer`/`layout_guard` deferred to Phase 4 so the guard API is designed
     against real encoder call sites. (b) ruled in REDUCED to the
     compile-time half (`released` marker + mandatory static_assert pins);
     the §5.4 reorder/insert-before-tail lint needs a recorded golden shape
     no static_assert can see and would bind zero layouts pre-tag, so it is
     deferred to the v1 tag. (c) ruled in as written — no open choice
     remained, the committed-artifact-plus-`--check` posture being settled
     precedent. Rejected alternative for (a), recorded: build the layer in
     SlopDrive and promote later; Phase 6's `examples/author_minimal_hub/`
     is already a second consumer, and relocating headers afterward means
     rewriting includes across the whole ported catalog.
   - **Phase 1a DONE (2026-07-29, SlopSync `42c7299`, pin bumped to it).**
     JS vocabulary codegen (= RFC-052(c)) + entry-rank/keys-19-23 decode +
     settings.js rank/category law. The C++ header regenerated
     BYTE-IDENTICAL, so the firmware had zero exposure. **Seven drifts
     found by diffing every hand table against the registry — receipts in
     the commit message, mechanism in TRAPS T20.** The load-bearing
     one: `SETTING_CATEGORY_NAME` was a 5-entry 0-based array standing in
     for 14-entry 1-based `ui_categories`, so every settings tab in every
     JS client had been mislabeled since RFC-047 Phase C2 shipped
     (category 2 `motion` drew as "Limits", 5 `library` as "Category 5").
     `SETTING_CATEGORY*` is REMOVED, not aliased. Also landed: the
     enabled-mask ordering trap — a `rank=hidden` field still CONSUMES its
     mask bit, so the skip must happen after the counter increments (the
     reference catalog ships exactly that shape at `settingKey` 4).
     Gates: slopsync_lint 0, canon_lint 0, gen --check clean on both
     artifacts, clients/js wire suite ALL PASS with the etag pin
     `B6 9E B0 62 49 EB E7 3A` still green, webui settings-model suite ALL
     PASS, `npm run build` clean, device-knowledge gate PASS, slopsim wire
     smoke ALL PASS, and a render check against slopsim confirming the rail
     now reads Tuning/Motion/Control/Library.
     **LIVE-VERIFIED and DEPLOYED (2026-07-29, same session).** `uploadfs`
     landed on the device; fw stayed 2.1.88 (page-only, no bump owed) and
     the served bundle's UI build stamp moved `7190240` -> `2388ea2`, which
     IS the proof-of-landing for an fs-only deploy (vite stamps
     `__UI_BUILD__` from `git rev-parse --short HEAD`; a version bump would
     have needed a firmware flash to mean anything). Live gates:
     `flagship-render-smoke.mjs` ALL PASS (27 assertions), the Phase 1a
     render check ALL PASS against the device at control tier, `smoke.ps1
     -ExpectFw 2.1.88` PASS with no `[STALL]` lines.
   - **RFC-052(d) landing** (SlopSync repo, additive; land with or before
     Phase 2): catalog.cddl entry key 17 `group_descs` + SPEC §8.1/§8.8
     text + C++ codec encode/decode + clients/js decode + vectors;
     duplicate rule = first-in-catalog-order (in the RFC). Reference
     catalog ADOPTION (preset-meta card descs and friends) rides the
     Phase 6 sweep inside the one etag bump.
   - **Phase 1b DONE (2026-07-29), DEPLOYED AND LIVE-VERIFIED — the one
     thing still owed is the OPERATOR EYEBALL the plan reserved.**
     `resolveWidget()`'s hand-guess is replaced by §8.2's table:
     `resolveArchetype()` walks the rows in order and returns a
     `UI_ARCHETYPE` code, `resolveWidget()` projects that onto this
     renderer's controls, and every field now carries `archetype` beside
     `widget`. Rows 2-5 and 16-17 are absent BY DESIGN and the code says
     so — `stop` is safety-op identity, `axis`/`pad2d`/`list` are claimed
     by heroes from roles before a field reaches the generic path.
     **THE ROW 9/10 PIN, veto-able and measured before it was chosen:
     `DRAG_TICKS_MIN = 20` distinct positions**, exported from
     `webui/src/model/settings.js`. Two corrections to the plan's proposal,
     both forced by the live catalog:
     (i) a bare `(max−min)/step` cannot see the fields that most need a
     stepper, because `blend_steps` (u8 1..10) and `reshape_steps` (u8 0..8)
     declare NO step — an unannotated integer is quantized by its own type
     at `1/scale` of a display unit, and only a float with no step is
     genuinely continuous;
     (ii) `step` rides the wire as an f32, so an exactly-20-tick range
     computes 19.9999997 and a bare `>= 20` would have flipped all three
     0..1-by-0.05 budget sliders. The comparison carries float slack.
     Measured effect: **8 of 184 fields change, 5 of them on a rendered
     tab** — `chase_lookahead`/`blend_steps`/`reshape_steps` slider→stepper
     (Tuning), `plan-strip.flags` and `motion.flags` readout→indicator
     (Tuning/Motion). The plan's feared "half the sliders flip" needs
     T≈32, which was measured and rejected. Nothing derived to `chart`:
     this catalog ships no `aspect: rate` field, so that row is live but
     unexercised and falls back to the plain numeral (§14d, §8.4's own
     glance degradation).
     Two renderer findings the screenshot pass caught, both fixed:
     **the design system strips native number spinners on purpose**
     (`style.css`: "nudge/trim buttons cover the increment use-case"), so
     `<input type=number>` alone does NOT satisfy §8.4's "increments in
     step-sized ticks" — the stepper draws its own −/+ nudges; and a
     read-only bitfield had been rendering as the numeral `0.00`, which
     the indicator lamps replace with named per-bit state.
     🚩 SPEC GAP, recorded not resolved: §8.2 has NO row for a WRITABLE
     named-bit bitfield8. It derives to `toggle` here (a set of booleans,
     composed the way §8.4 composes pad2d out of sliders) and the code
     names the line to change if a row ever lands.
     Gates: settings-model suite ALL PASS (12 new assertions pinning the
     rule, including the f32-dust case), canon_lint 0, device-knowledge
     gate PASS, `npm run build` clean, `flagship-render-smoke` ALL PASS
     (27) against the device, `smoke.ps1 -ExpectFw 2.1.91` PASS with no
     `[STALL]`. Deployed `-Target fs`; fw stays 2.1.91 (page-only, no bump
     owed), UI build stamp `1d117e2` -> `5624b6a`.
   - Then Phases 2–6 per plan. Phase 2 is (a)'s first three headers only,
     per the staging ruling. Phase 6 carries the ceilings ruling
     (1000 mm/s / 60k mm/s²) + the ONE etag bump + deploy.
2. **MEMORY PRESSURE — root-caused, margin banked, remaining decisions open
   (2026-07-29). Mechanism lessons are TRAPS T21; measured receipts are in
   commits `f6e8159`, `1d117e2`, `a467bee`. This entry keeps only what is still
   a DECISION.**
   Where the ~226 KB of init spend goes, live-attributed at fw 2.1.90 (heap
   starts near 258 KB; `bootheap`/`dumpTaskStacks` in `src/main.cpp` +
   `include/system/BootHeap.h` re-measure this on any boot):
   | consumer | bytes | share |
   |---|---:|---:|
   | ss:ble (NimBLE) | 64,076 | 28.3% |
   | wifi | 56,412 | 24.9% |
   | tasks (our stacks) | 45,592 | 20.2% |
   | ss:ws (AsyncTCP) | 18,056 | 8.0% |
   | ss:hubtask / ss:sign | 27,288 | 12.1% |
   | webui | 4,348 | 1.9% |
   | everything else | 10,472 | 4.6% |
   Three findings that settle old arguments: the **webui is 1.9%**, so
   "drop the web UI to save RAM" is dead; **`ss:ctor` is 240 B**, so the
   catalog costs no internal RAM and the PSRAM placement works completely;
   and **compiler flags are the wrong lever entirely** — `.bss` is 55 KB, the
   spend is runtime, and an optimizer may not shrink a declared buffer.
   **BANKED (fw 2.1.91, deployed + verified):** AsyncTCP stack 16K->8K and
   `Comms` 6144->4096, both sized from the census; plus the webui
   hidden-tab close-on-blur fix that removes the session-churn fragmentation
   driver. Result: free 31,336 -> 42,064 (+34%), **maxblock 14,836 -> 31,732
   (+114%)**. maxblock is what the 503 gates on, and it moved from 2.5 KB
   above the floor to 19.4 KB above it. Operator ruling that scoped this:
   *"this is not a computer with a ballooning memory load... we need a few
   more inches, and that's enough tolerance"* — a 503 from two tabs opened
   at once is ACCEPTED, so no further work is owed on the 503 itself.
   **STILL OPEN, all three needing an operator call, none blocking:**
   - **Deferred BLE start** — the only reachable way to reclaim BLE's 64 KB
     on this toolchain, and the ONLY way to touch its 39,424 B controller
     share, which cannot go to PSRAM at all (the controller's link-layer ISR
     must be reachable while the flash cache is disabled — TRAPS T21's
     sibling constraint, detail in `1d117e2`). Costs: it only helps while BLE
     is OFF, RFC-043 makes BLE GATT a MUST for the hardware-hub profile, and
     `SlopSyncBleTransport` has no teardown path today, so it is net-new work.
   - **ESP32 Arduino Lib Builder rebuild** — the precompiled-lib wall blocks
     FOUR wins: NimBLE `MEM_ALLOC_MODE_EXTERNAL` (~24.7 KB of host pools, at
     ~zero performance cost), dropping BLE central/observer roles a hub never
     uses, `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` (attacks WiFi's 56 KB *and* the
     sub-4 KB pbuf churn), and heap tracing + apptrace. Cost is unchanged and
     large: it replaces the working pioarduino/GCC13/C++23 build model.
   - **`Sampler`'s 15 KB of apparent slack** — gated on a motion-exercising
     bench pass producing no new stack regressions (TRAPS T21). `Motor` and
     `httpTask` sit behind the same gate. Do NOT trim `ss:hubtask` or
     `ss:sign` from census data at all; both comments record why (T1).
   - Not reproduced deliberately: whether a real backgrounded phone still
     churns sessions against 2.1.91. The mechanism says it cannot.
   - **PAGE-SERVE FLOOR: half fixed, half ruled OPEN after a live A/B
     (fw 2.1.91 -> 2.1.94, 2026-07-29).** Shipped, deployed, verified:
     (i) the pressure gate now sits BELOW the ETag check, so a 304
     revalidation answers even while fresh loads are refused — the old order
     503'd exactly the browsers that already held the bundle; (ii) the serve
     no longer allocates per request. `WebServer::streamFile()` delegates to
     `NetworkClient::write(Stream&)`, which `malloc()`s 1360 B per call; that
     is replaced by a 1360 B `.bss` buffer and an explicit loop (1360 because
     it sits under the 1460 B TCP MSS — a larger chunk straddles the segment
     boundary and buys an extra pbuf). Live proof under real pressure: **55
     fresh loads 503'd while all 55 revalidations answered 304**, no panic,
     heap recovered to maxblock 20,468 afterwards.
     The floor's VALUE was open for one build and is now **CLOSED (2.1.95)**.
     History worth keeping, because it is why the fix looks the way it does:
     the `maxblock < 12288` test is ~9x the largest allocation the path can
     make, so "correct the arithmetic" was tried and A/B'd. Control and a
     candidate with the floor sized to the real allocation met the same
     three-concurrent-browser harness; both bottomed near 250 B free; the
     control WEDGED and survived, the candidate kept serving and **PANICked**
     (2.1.92, boot_seq 12, `heap_min=260`, `max_block_last=124`). The floor
     was doing load-shedding, not sizing.
     **The resolution was to split the two failures by timescale rather than
     to pick a better number.** Fragmentation is latched and visible AT ENTRY;
     exhaustion is a transient that arrives DURING the transfer, after entry
     has already said yes, so no entry threshold can see it. 2.1.95 therefore
     gates entry only on "can an internal allocation happen at all"
     (`maxblock >= 4096` — the true ceiling under
     `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`) plus a `free >= 12288` floor,
     and re-checks free INSIDE the send loop, abandoning the body below
     12288. Truncation is a page the browser retries. Live A/B/C: the harness
     that killed 2.1.92 ran twice as hard against 2.1.95 with **no reboot**,
     the abort fired at `free=11,572` and again at `maxblock=2,548`, and a
     fresh load returned **200 at maxblock 10,740** — the exact value that
     used to latch a permanent 503. Mechanism: TRAPS T19 addendum + its
     resolution note.
   - **PROPOSED (operator, 2026-07-29), not implemented: a boot watchdog in
     the page that reloads when the bundle arrived truncated.** Verdict: it
     works, but only in one specific shape, and it is a load amplifier unless
     built carefully — so it is written down rather than dropped in.
     Why it works at all: HTML parsing is streaming, so a *tiny* inline script
     in `<head>` is received in the first packet and executes long before the
     document ends. It arms a timer; `main.js` sets the flag on mount; if the
     rest of the bundle never lands, the timer fires and reloads. A watchdog
     placed anywhere else — bundled with the app, or after the 114 KB inline
     script — is cut off by the very truncation it is meant to catch and never
     runs. This is a `vite-plugin-singlefile` bundle, so "top of head, before
     everything" has to survive the inliner; verify it does before trusting it.
     Two things that must be designed in, not bolted on:
     (i) **backoff and jitter are mandatory.** Truncation happens because the
     hub is over-loaded; a fleet of tabs each retrying on a fixed 5 s timer
     feeds exactly the load that caused it. Exponential with jitter and a
     retry cap, and the give-up state must SAY it gave up rather than sit
     blank.
     (ii) it is a mitigation for a residual, not a fix. 2.1.95 sheds before it
     truncates, and 2.1.94's 304 path means a returning tab transfers no body
     at all — so the window this covers is "first load, during pressure".
     **That gating assumption is now TESTED and it HOLDS (2026-07-30).** The
     fear was that a truncated gzip body with a `Content-Length` promising more
     would be discarded whole, so the head script would never run and the whole
     idea would be worthless. Chrome does NOT discard it. Measured against a
     real shed response: navigation commits with no error, the partial document
     is parsed and live (108,630 chars of HTML in the DOM), `<head>` is fully
     parsed with 6 children, and `document.title` is applied. Only the 114 KB
     inline bundle dies -- an inline script executes when the parser reaches
     its `</script>`, which never arrives. `readyState` then stays `"loading"`
     forever, which is exactly the hung half-drawn page this would rescue.
     So a tiny top-of-`<head>` script WILL execute; the remaining requirement
     is unchanged, that `vite-plugin-singlefile` keeps it ahead of the bundle.
     Backoff/jitter/give-up-state are still mandatory (above).
   - **The accepted 503 is NOT transient — it holds until reboot.** Measured
     2026-07-29 during the Phase 1b screenshot pass, which stacked ~25 page
     loads and a dozen WS sessions in a few minutes. Per session the cost is
     ~1-2 KB of free heap and a little maxblock, and it mostly comes back;
     but once maxblock crosses under `handleRoot`'s 12,288 B floor it PINS
     — observed flat at 10,740 B for 100+ s with every client gone and
     `free` still fluctuating around 25 KB, so nothing was leaking, the big
     block was simply gone. A clean boot reads 41,928 / 31,732 (the recorded
     2.1.91 figures, reconfirmed). This does not change the ruling that a
     503 from two tabs is ACCEPTED, but it means the recovery story is
     "reboot", not "wait" — worth knowing before anyone reads a stuck 503 as
     a new bug. Crossing the floor is what pins it, NOT depth of dip: a
     later boot under the same probe load bottomed at 240 B free and still
     coalesced maxblock back to 29,684 B every time, serving `/` in 0.69 s
     afterwards. And the heartbeat LED says nothing either way — a frozen
     LED means httpTask is BLOCKED, whereas the 503 is httpTask bailing out
     fast, so a pinned device heartbeats normally. `maxblock` in the
     `[sys] heap` beacon is the only readout for this. Agent-side lesson, no
     firmware owed: run browser probes one at a time, and note that
     `flagship-render-smoke.mjs` leaks its chromium when an assertion throws
     mid-run.

3. **PARKED (operator: "future task once all of this is rock solid"):
   stripped-down ESP32-WROOM-32D variant, no webui.** For the widest user
   base; operator believes it is achievable and nothing measured contradicts
   that. The ambush to know BEFORE starting: **WROOM-32D has no PSRAM.** This
   build places the 240,976 B hub service in PSRAM and hard-disables SlopSync
   when it is absent, and `SPIRAM_MALLOC_ALWAYSINTERNAL=4096` silently
   offloads every >=4 KB allocation there. So the port is NOT "strip the
   webui" — that is 4,352 B, 1.9% (item 2) — it is a catalog/session-capacity
   reshape to fit ~240 KB of hub into internal SRAM alongside BLE's 64 KB and
   WiFi's 56 KB. Build on RFC-043's hardware-hub profile and the STAMPED
   `minimal` sim profile. One freebie the S3 does not get: the original ESP32
   HAS Classic BT, so `esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT)` reclaims
   real memory there.
4. **DATAGRAM-SAFETY + PROVISIONING TODO (2026-07-29 chat; smart order;
   each item carries the context its implementer needs. Discipline,
   operator-directed: once an item is implemented AND verified, RIP it
   from this ledger — the durable record is the RFC + the commit; this
   list is working memory, not archive).**
   Milestone context (keep until the v1 tag): first live BLE client held
   a full-control GATT session AND performed the §6.3 BLE→WS mid-session
   migration, operator-verified 2026-07-28 — SPEC limitation 22 updated;
   BLE binding is field-real now.
   - **T1 — RFC-053 hub implementation. RIDES THE PHASE 6 SWEEP:** the
     opt-out setting is a catalog field addition = etag movement, and the
     campaign plan allows exactly ONE bump, at Phase 6 — fold it in
     there, do not spend a second bump. Work: (i) ESTOP dispatch in
     `src/comms/SlopSyncUdpDiscovery.cpp` — E5-magic prefix match
     (disjoint from the SLOP probe magic by construction) + CRC-32 over
     first 8 bytes + dispatch into the SAME single e-stop function the
     WS/BLE paths use (§11.2's by-construction rule); (ii) NVS-persisted
     bool, DEFAULT ON (opt-out, operator-amended ruling), catalog-exposed
     (`configure` tier; home = safety or network channel, decide at
     implementation and 🚩 flag if contentious); (iii) build flag for
     hard removal; (iv) per-source rate limit mirroring discovery
     replies; (v) DISCOVER_REPLY flags bit1 `datagram_estop` = live
     setting value (RFC-053 2b, APPROVED, trivial). Verify: broadcast +
     unicast scream from a LAN peer latches; toggle-off refuses; limiter
     holds under spray; T3 back-to-back-sessions posture unaffected.
   - **T2 — RFC-054 ruling (operator), then implement:** pick option
     a/b/c in the RFC. Binding constraints for whoever implements:
     credential rides a UNICAST-ONLY surface (never STATE, never
     broadcast ECHO — RFC-009.5 logic), `configure` tier + open pairing
     window + BLE bonding SHOULD; SPEC §13.2 already names this admin
     channel as BLE's purpose. ESP-NOW half stays dormant until T3.
   - **T3 — ESP-NOW binding (§13.3, zero implementation today).
     SEQUENCED BEHIND PSRAM OFFLOAD** (live pressure snapshot bottomed
     internal heap at 40 B; ESP-NOW adds few-KB buffers). Mechanism
     context: it rides the already-up WiFi MAC — not a third radio
     contender; peers must sit on the STA's AP channel; the real coex
     cost is BLE latency jitter under WiFi bursts — measure with motion
     streaming + a live BLE client attached before calling it good.
     Carries RFC-053's ESP-NOW ESTOP acceptance + BEACON mirror bit.
   - **T4 — e-stop fob (hardware, operator project):** BLE-only fob is
     buildable TODAY (raw-frame path is unconditional on every binding a
     hub runs); UDP leg unlocks at T1, no-network leg at T3. Intended
     semantics recorded in RFC-053 item 5: repeat-while-latched — fresh
     seq per interval while the button is physically down, so an
     authorized clear re-latches within a second; the fob never holds
     clear authority. Confirmation gap: fob surfaces UNCONFIRMED locally
     (RFC-053 open question, lean (i)).
   - **T5 — residuals, low:** ATT-MTU control-frame fragmentation
     (oversize control frame fails to send rather than splitting — SPEC
     §18-22); WS→BLE migration direction never exercised live. BLE
     idle-reap and PSRAM offload have their own ledger entries — pointed
     at, not restated (C-1).
5. **Parked webui rapid-fire list** — see the kickoff entry above; queued
   BEHIND the campaign (several items become trivial on the new surface).
   Two items joined it this session, both in the SHELL FEASIBILITY SPIKE
   entry: the truncated-bundle reload watchdog (item 2, test the gzip
   assumption first) and M1/M2's still-unverified BLE + WS-upgrade paths,
   which the desktop shell now makes clickable — the shell launches, so
   "scan BLE / connect / upgrade to WS" is a five-minute operator check
   rather than a build.
6. **Session-gate/closeout system** (C-13 proposal + ledger diet + tiered
   canon loading, designed in chat 2026-07-29) — implement after Phase 0;
   this closeout entry is its manual prototype.
7. **Deploy state: device runs fw 2.1.95; the filesystem is current at
   `e41daac`.** No firmware bump is owed -- every 2026-07-30 change was
   page-only, and an fs-only deploy proves itself by the served bundle's
   `__UI_BUILD__` stamp (vite stamps it from `git rev-parse --short HEAD`), not
   by a version. **Redeploy `-Target fs` after any commit rewrite**: squashing
   moves HEAD and leaves the device stamped with a commit that no longer
   exists. The three rs485 FAILs in every smoke run are the operator's motor
   being unplugged -- expected, not a regression.
8. **UI punch list: 3 of 7 closed, 4 open** -- see the OPERATOR UI FEEDBACK
   QUEUE section, which now carries only what is still open. Item 4 (card
   columns) is the big one and is now UNBLOCKED; land it before Phase 6 so the
   annotation sweep happens once. Item 3 rides Phase 6's single etag bump. Item
   5 is one operator click away because a current desktop shell exists.
9. **The plan strip is ruled and specced but NOT STARTED** -- own section
   above. It needs the `RailWidget` travel-extent derivation extracted into a
   shared helper first (C-1), and it fixes a C-12 lying comment on the way.
   Unblocked with item 4.
10. **Session shape, for whoever restamps this next.** The lesson of the day
   was the same one three times: **a diagnosis argued from the source is a
   hypothesis, and the DOM/hardware is the only witness.** Three separate bugs
   this session had a confident, plausible, written-down cause that measurement
   overturned in one read -- the info glyph (ink balance vs UA padding, off by
   a factor of 20), the activity grid (flat data vs a leaked `$effect`
   dependency), and the page-serve floor from the previous session (correct
   arithmetic, wrong conclusion, panicked the device). Two of those wrong
   diagnoses were in THIS FILE, written with full confidence. The habit that
   caught all three: measure the rendered thing, and write the guard around
   the SIGNATURE of the bug rather than around "does the feature run" -- every
   naive assertion drafted this session would have passed the broken version,
   and one of them (glyph centering) was silently passing on an all-zero rect
   from a `display: none` element until it was forced to prove the box existed.
