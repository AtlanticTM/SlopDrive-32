# WebUI Legacy-Plane Diagnosis (pre-slopsync-js refactor)

> Channel ids herein are historical (pre-C4 renumber); current map:
> [CHANNEL-MAP.md](slopsync/CHANNEL-MAP.md).

> **DEPRECATED SYSTEM, dated record (2026-07-28).** Every file this
> diagnosis traces — `link.js`, the old `cmd.js`/`shadow.js`/`range.js`,
> `UiSocket.{h,cpp}` — is deleted. The WebUI was rebuilt catalog-driven;
> current architecture is [webui-architecture.md](webui-architecture.md).
> This document stays as the diagnosis that justified the rebuild and a
> record of exactly what was broken and why — read it for history, not
> for how the WebUI works today.

**Date:** 2026-07-24. **fw at time of test:** 2.1.45 (live device 192.168.1.229,
confirmed via `/api/capabilities`). **Scope:** read-only static trace of
`webui/src/core/{link,cmd,shadow,range,telebuf,wire}.js`, `webui/src/features/rail.js`,
`src/ui/UiSocket.{h,cpp}`, `src/ui/WebUI.cpp`, `src/main.cpp`, cross-checked
against live device state (`/api/capabilities`, `/api/status`, `/api/slopmotion`,
`/api/log`). No writes to firmware or device state were made.

Live device at test time: `homed: false`, `measured_stroke_mm: 0`, WS-side
`sync.dropped: 67` (all segment bundles dropped at the HOMED gate — expected,
matches known behavior, not a bug). Log tail showed no `[STALL]` lines in the
sampled window; heap free 59–76 KB, stable. This diagnosis is therefore
**static-code-driven**, corroborated but not load-tested live (no drag-to-echo
round trip was performed — POSTs were out of scope).

---

## Suspect-by-suspect verdict

### 1. `link.js` staleness gates → CONFIRMED (mechanism real, attribution slightly off)

The `>150ms stale / >1s suspended` gate is real and does exactly what the
roadmap describes, but it does not live in `link.js` — it lives in
**`webui/src/core/telebuf.js:57-60`**:
```
var STALE_MS = 150;
var SUSPEND_MS = 1000;
var STALE_FB_MS = 750;
var SUSPEND_FB_MS = 5000;
```
evaluated every `sampleAt()` call (`telebuf.js:374-381`). `link.js` only feeds
it (`feedWireSamples` → `telebuf.js:228-236`). `suspended` sets
`window.__CMD_SUSPENDED = true` in `main.js:1078-1081`, and `cmd.js:170`
checks that flag as the **first line of `send()`** — before coalescing,
before the fallback-mode check, before anything:
```js
export function send(op, payload) {
  if (typeof window !== 'undefined' && window.__CMD_SUSPENDED) return -1;
  ...
```
So yes — a stalled telemetry/link feed alone silently kills **every** control,
including the window, with zero error surfaced to the send site (caller sees
`id === -1` and nothing else happens).

One mitigating detail the roadmap brief didn't have: suspension is gated on
**link-liveness** (`telebuf.js:133-143`, fed by 0x02 STATUS ~500ms heartbeats
and HELLO), not raw motion telemetry — so an idle-but-connected machine
(no 0x01 frames because nothing is moving) does *not* falsely suspend. Only
the visual `.stale` chip (150ms) is tied to motion-telemetry age. This was a
prior fix (see `telebuf.js:126-131` comment) and is working correctly.

**New finding not in the original suspect list:** on every fresh connect/
reconnect, `_lastSampleTs` starts at effectively "infinitely old" and the
activity gate (§5 below) deliberately withholds the first 0x01 telemetry
frame for ~2s (client must "earn" the stream with a clock ping). That means
**every page load or reconnect guarantees a ~2s `.stale` visual** before the
first telemetry frame ever lands — by design, not a bug, but it is a
concrete, code-provable contributor to "slow to reflect device state."

### 2. `cmd.js` retry ladder → PLAUSIBLE (real, but a minor contributor)

Confirmed exactly as described: `RESEND_MS = 300`, `MAX_ATTEMPTS = 3`
(`cmd.js:68-69`), scheduled per in-flight entry (`_scheduleRetry`/`_retry`,
`cmd.js:214-257`). A lost echo is retried at +300ms and +600ms before
`onFault` fires at ~900ms — so a genuinely dropped ECHO reads to the user as
up to ~900ms of "stuck pending" before any fault UI appears. This **does**
mask lost echoes for that window, but it does not amplify load in a
meaningful way for the window control specifically: `_coalesce()`
(`cmd.js:148-160`) guarantees only one in-flight command per key
(`'window'` coalesces on `OP_SET_WINDOW`), so a rapid drag doesn't stack
retries — each new `pushWindow()` debounce tick cancels the prior in-flight
timer. Contributes latency-to-fault-visibility, not the root breakage.

### 3. `shadow.js:236-284` + `settingsAuthoritative` → CONFIRMED (guard works; found a second, real, currently-inert defect alongside it)

The described resync-clobber guard is real and correctly implemented:
- `processConfig()` (`shadow.js:236-284`) rejects non-finite/degenerate window
  bounds (`_wValid`, line 246-247) and — critically — **will not apply a
  device-authored window while the window shadow is `pending/overdue1/overdue2`**
  (`_wPending` check, `shadow.js:249-250`), so a resync cannot stomp an
  in-flight drag.
- `_applyConfigField()` (`shadow.js:294-295`) has the same pending-guard for
  every other key.
- `settingsAuthoritative` (`range.js:60-67`, set true only after
  `loadSettings()`'s HTTP `/api/settings` pull completes — `settings.js:365`)
  additionally blocks a racing WS config-push from painting stale/default
  bounds before the authoritative HTTP load resolves.

This part is solid and battle-tested (comments reference a real prior "wrong
window on boot" incident it fixed). **However**, tracing the 'window' shadow
render path turned up a genuine, currently-inert bug:

- `range.js:89-93` sends `cmd.send(OP_SET_WINDOW, { range_min, range_max, no_persist })`
  — field names `range_min`/`range_max`.
- `shadow.js`'s `'window'` key render function (`shadow.js:60-62`) reads
  `sh.desired.min` / `sh.desired.max` — different field names. Until an ECHO
  arrives and `processEcho()` normalizes `sh.desired` (`shadow.js:216-222`,
  *only* triggered by ECHO), `sh.desired.min/.max` are `undefined`, so this
  render function writes empty-string `data-d-min`/`data-d-max` attributes to
  `#spineRailHost` for the whole pending window.
- Grepping the entire `webui/src` tree for `data-d-min`/`data-d-max` shows
  **these attributes are never read anywhere** — `rail.js` (the actual, current
  window-drag control) reads `winMin`/`winMax` module state from `range.js`
  directly (`rail.js:16`, used throughout `positionBand`/`positionTape`), never
  the DOM data attributes shadow.js writes. This is confirmed dead code on
  both ends (shadow.js's comment even says "the band in rail.js reads winMin/
  winMax directly from range.js" — the data-attribute plumbing it goes on to
  write was superseded when rail.js became the primary window editor and was
  never removed). **Currently harmless** (nothing consumes it) but it means
  the shadow layer's supposed "intent overlay" for the window control does
  nothing today — the real-time visual feedback during a drag comes entirely
  from rail.js's own local state, independent of shadow.js's pending/overdue
  machinery.

### 4. 100ms HTTP-fallback poll hammering the sync WebServer → CONFIRMED

`main.js` starts `setInterval(pollStatus, 100)` in three places
(`main.js:1043`, `:1181`, `:1229` — degrade, boot-without-WS, and
post-crash-emergency paths). `pollStatus()` (`main.js:474-...`) hits
`GET /api/status`, served by the **synchronous** `WebServer` on port 80
(`WebUI.cpp`, confirmed 28 `_httpServer->on(...)` registrations by grep). That
server is serviced by exactly one call per `httpTask` loop iteration
(`main.cpp:570`, `TIME_STEP(ui->update(), "http:ui.update")` →
`WebUI.cpp:138` `_httpServer->handleClient()`), sharing the same Core-0,
priority-1 task with OTA handling, ServoModbus (FAS-backend builds skip this),
`applogDrain()`, and the heap beacon. `WebServer::handleClient()` is
inherently one-request-at-a-time — there's no concurrency here to hide a slow
request behind. **Crucially, this poll is exactly the mode active when WS is
degraded, which is also when `cmd.js`'s HTTP-fallback command routing
(`cmd.js:285-326`, e.g. `_FALLBACK_ROUTES[OP_SET_WINDOW]` → `/api/settings`)
is live** — so a stroke-window drag under a degraded link competes directly
with a 10 Hz status poll for the same single-threaded server, and any other
slow handler on that server (e.g. the documented ~0.5-1s LittleFS bundle
stream at page load, `WebUI.cpp:247-248` comment) queues behind whichever
request got there first. This is a real amplifier, specifically activated in
the degraded/fallback state — which is also the state most likely to
coincide with "the UI feels laggy."

### 5. Firmware `UiSocket.cpp` senderTask + links2004 block defect → EXONERATED as an active bug (mitigations are landed); PLAUSIBLE residual UX cost by design

The specific defect named in roadmap §4 (`WS send blocks HTTP mutex`) has
already been fixed in this codebase, extensively and recently:
- `_ws.loop()` was moved off `httpTask` entirely onto the dedicated
  `senderTask` (Core 0, prio 3, 22ms tick) — confirmed by
  `UiSocket.cpp:890-901` and the `main.cpp:571` comment
  (`"uiSocket.update() (_ws.loop()) was MOVED to UiSocket::senderTask"`).
  `include/ui/UiSocket.h:213-225`'s doc comment is now **stale** — it still
  describes `httpTask` calling `update()`, which is no longer true; harmless
  (comment-only), but worth fixing during the refactor so nobody re-learns
  the wrong mental model.
- Every outbound frame type (not just telemetry) routes through
  `_sendGuarded()` (`UiSocket.cpp:115-126`), which mutes (never disconnects)
  a client whose `sendBIN` fails, and `_reapDeadClients()`
  (`UiSocket.cpp:702-715`) kills genuinely dead half-open sockets after 4s so
  they can't starve `_ws.loop()` for everyone else.
- A recursive mutex (`_wsMutex`) serializes all `_ws` access between
  `senderTask` and the WS event callback that fires synchronously inside it.

This machinery is real and working as documented. The one **remaining,
deliberate** cost: the activity gate (`UiSocket.h:43-54`,
`CLIENT_ACTIVE_WINDOW_MS = 10000`) plus the "don't stamp activity on
connect" policy (`UiSocket.cpp:251-259`) means a client is muted from all
telemetry/status/interp/anomaly frames until it sends its first inbound
frame — normally its clock ping, sent every 2s (`link.js:409-411`,
`CLOCK_SYNC_INTERVAL_MS = 2000`). So **every fresh connection has an
intentional ~2s window with zero telemetry**, compounding finding #1's
"guaranteed stale flash on load" — this is where "slow to reflect device
state" is most concretely rooted in current code, not a regression, a
tradeoff made to fix a worse prior bug (the reboot/reconnect thundering-herd
stall).

### 6. The stroke-window path end-to-end: drag → cmd → firmware → echo → band render

Traced concretely:

1. **Drag input** — `rail.js:576-601` (`pointermove` on the band/handles)
   updates `winMin`/`winMax` **module-level state in `range.js`** directly
   (`setWinMin`/`setWinMax`) and calls `layoutWindow()` (repaints band/tape/
   hazards from that same local state, `rail.js:439-444`) **synchronously,
   every pointermove event, unconditionally** — this path is NOT gated by
   `__CMD_SUSPENDED`, shadow state, or connection state in any way. The band
   under your finger always moves.
2. **Network send** — `rail.js` also calls `pushWindow()`
   (`range.js:85-95`) on every pointermove, which debounces 60ms then calls
   `cmd.send(OP_SET_WINDOW, { range_min, range_max, no_persist: true })`.
3. **`cmd.send()`** (`cmd.js:168`) — **first check is `__CMD_SUSPENDED`**
   (`cmd.js:170`). If the link has been stale >1s (or >5s in HTTP fallback),
   this returns `-1` and **nothing is sent, silently** — no console warning,
   no toast, no fault event (the fault path only fires from retry
   exhaustion or a send that was actually attempted).
4. If not suspended: WS path sends a `0x10 CMD` binary frame
   (`wire.js` `buildCmd`), or (WS down but not yet "suspended," i.e. in the
   `degraded`-but-fresh window, or if `_sendBinary` is null) HTTP fallback
   POSTs `/api/settings` with the same field names.
5. **Firmware** — `UiSocket.cpp:295-336` parses the `0x10` frame inline
   inside `_handleEvent` (which itself runs inside `senderTask`, not
   `httpTask` — see #5), dispatches to `WebUI::handleCommand()`
   (`WebUI.cpp:1791-1800`) which for `WS_OP_SET_WINDOW` calls
   `applySettings()` (`WebUI.cpp:526-...`). This mutates `_mapper` directly
   and synchronously (`_mapper.setRange(rmin, rmax)`, `WebUI.cpp:610`) —
   cheap, no blocking I/O unless `no_persist` is false (window drags always
   set `no_persist: true`, so `ConfigStore::save()` — a flash write — is
   correctly skipped, `WebUI.cpp:619`). Op-code values match on both ends
   (`0x01` in `wire.js:20` and `UiProtocol.h:81`) — **the one drift found
   here is cosmetic**: `UiProtocol.h:81`'s comment says `{min, max,
   no_persist?}` but the real (and firmware-parsed) field names are
   `range_min`/`range_max` — comment-only, not a functional bug.
6. **Echo** — `sendEcho()` (`UiSocket.cpp:164-194`) sends `0x11 ECHO` with the
   post-clamp `range_min`/`range_max`. `cmd.js`'s `processEcho()`
   (`cmd.js:335-364`) dispatches to `shadow.js`'s `processEcho()`
   (`shadow.js:204-234`), which normalizes the field-name mismatch noted in
   #3 (`shadow.js:216-222`) and updates `sh.reported`.
7. **Band re-render on echo** — nothing in `shadow.js`'s window-echo path
   calls back into `rail.js`. Looking at the actual live-position band
   update mechanism, `rail.js` repaints on `pointerup`
   (`rail.js:603-610`, calls `renderWindow()` → `setRailSync` callback →
   `layoutWindow()`) and whenever `setRailSync`'s registered callback fires
   (wired from `range.js:77`, invoked by `renderWindow()`). Since local drag
   state (`winMin`/`winMax`) already reflects the position the user dragged
   to, and the echo (in the non-suspended, non-clamped case) just confirms
   the same numbers, this is not a visible break in the happy path.

**The one clean, concrete, statically-provable break point:** step 3. When
`__CMD_SUSPENDED` is true, the band drag visual (step 1) and the network
send (steps 2-7) are **completely decoupled** — the band keeps moving
smoothly under the operator's finger while `cmd.send()` returns `-1` and
transmits literally nothing to the firmware, with zero surfaced feedback
that this is happening (no toast fires from `cmd.js` on this path; the
`suspended` banner from `shadow.js:543-557` fires from the DOM class
observer, so it *is* visible, but the band's continued smooth motion right
next to a "controls suspended" banner reads as "this control is buggy,"
not as coherent suspended-state UX). This matches the operator's "stroke
window control is broken" report precisely, and is triggered by exactly the
staleness mechanism named in suspect #1.

---

## Ranked most-likely causes of the three symptoms

1. **"Stroke-window control is broken" → `cmd.js:170`'s silent
   `__CMD_SUSPENDED` short-circuit, combined with `rail.js`'s drag-visual
   being fully decoupled from the send path.** The band always looks like it
   works because its rendering never checks suspension; the network send
   silently no-ops. This is the most concrete, single-line-attributable root
   cause found. Fix direction for the refactor: either gate the visual drag
   on link health too (so a suspended UI visibly refuses to drag, matching
   ground-truth doctrine — "a control that renders but drives nothing is a
   defect" per [DOCTRINE.md](canon/DOCTRINE.md) §3), or make `cmd.send()`'s suspended-drop
   observable at the call site instead of returning a bare `-1`.

2. **"Slow to reflect device state" → the ~2s guaranteed telemetry blackout
   on every fresh connect/reconnect** (activity gate, `UiSocket.h:43-54` +
   `UiSocket.cpp:251-259`, "must earn the stream"), stacked with the
   150ms/1s staleness thresholds in `telebuf.js` that fire `.stale` almost
   immediately on a cold `_lastSampleTs = 0`. Both are deliberate,
   previously-justified tradeoffs (they fixed a worse reboot-thundering-herd
   bug), not regressions — but they are a real, reproducible multi-second
   "looks dead" window on every page load, exactly matching the complaint.

3. **"Slow/laggy" (general) → the 100ms HTTP-fallback poll and any
   fallback-mode command POST sharing one single-threaded synchronous
   `WebServer`** (`WebUI.cpp:138`, 28 routes, one `handleClient()` per
   `httpTask` tick). This only bites once WS has degraded (2 failures in
   10s, `link.js:28,377-385`), but once it does, status polling and window
   drags queue behind each other and behind anything else slow on that same
   server (e.g. the ~0.5-1s bundle stream noted at `WebUI.cpp:247-248`).
   Lower-ranked than #1/#2 because it requires the WS to already be
   unhealthy to engage at all — but when it does engage, it's the clearest
   explanation for compounding general sluggishness on top of the control
   breakage in #1.

4. **Minor/contributing, not primary:** `cmd.js`'s 300ms×3 retry ladder
   (up to ~900ms before a fault surfaces — masks drops but doesn't cause
   them); the dead `data-d-min`/`data-d-max` shadow-to-rail plumbing (§3,
   currently inert, but worth deleting rather than porting during the
   refactor — it's dead weight, not a live bug); the stale `UiSocket.h`
   doc comment describing the pre-fix `httpTask`-owns-`ws.loop()` model.

5. **Exonerated:** the links2004 WS-blocks-HTTP-mutex defect itself (§4/§5)
   — already fixed and well-defended in the current code (guarded sends,
   reaping, dedicated sender task, recursive mutex). It is the *reason* the
   activity-gate/mute tradeoffs in #2 exist, not an active bug today. The
   `shadow.js:236-284` resync-clobber guard is also exonerated — it
   correctly protects in-flight drags; it was not the source of the
   reported breakage.

None of this requires patching before the migration — per the roadmap brief,
the fix is moving the browser onto the SlopSync plane (§5.2), where the
GRANT/retained-safety/NACK model replaces the ad hoc staleness-gate +
activity-gate + HTTP-fallback stack wholesale. This document exists so the
migration knows precisely which legacy behaviors it is allowed to not miss.

---

## Post-migration incident (2026-07-24, same day): the dual-plane config storm

The first slopsync-js deploy surfaced a NEW defect class the legacy plane had
merely been immune to. `renderWindow()` historically ended with an implicit
`pushWindow()` — so every ADOPTION path (shadow `processConfig`, `setTravel`,
machine-config STATE) that re-rendered the band also TRANSMITTED it. On the
legacy plane this never sustained a loop. SlopSync's config-set bumps
`cfg_gen` on every accepted set and the hub republishes machine-config
0x0081 on change → adopt → render → implicit push → bump → republish →
**a cross-plane perpetual loop at ~12 Hz**, armed forever by the first window
touch. Symptoms: config churn, legacy GET_CFG resync storm, RATE_LIMITED
NACKs, toast spam — "90% of the UI feels broken."

Fix (deployed same day, UI-only): `renderWindow()` no longer transmits;
`pushWindow()` is explicit at USER-action sites only (nudge/trim/setBound,
min/max inputs, rail drag/tap). Bridge `sendWindow` is coalesced latest-wins
(one intent in flight, ≥100 ms spacing vs the 10 Hz grant, toast only on
terminal failure). Bridge also stops adopting 0x0081's window as a stored
setting while UNHOMED (STATE carries the EFFECTIVE window = full rail when
unhomed; it stomped freshly dragged values — window adoption while unhomed
comes from ECHO only).

**Doctrine distilled: adoption must never transmit.** Any render path
reachable from a device-state adoption must be side-effect-free toward the
wire, or two ground-truth planes will happily play ping-pong through it.

Found via headless-Edge CDP probes (scratchpad browser-*.mjs): console/echo
timeline → sender wrap with stacks → network capture → zero-browser cfg_gen
drift check (which also caught the operator's stale pre-fix tab still
looping at 1 Hz — a hard refresh is part of the fix rollout).

## Layer 3 (same day, fw 2.1.46): the 5-second speculative-socket capture

With the storm dead and pollers guarded, page loads still hit 5–15 s
lotteries. Measured: TCP connect instant, ping clean, [STALL] silent, but
time-to-first-byte QUANTIZED at 4.8–5.0 s (and 9.7 s = two in a row). That
constant is the core WebServer's HTTP_MAX_DATA_WAIT (5000 ms, no override
guard): in HC_WAIT_READ the single-slot server waits up to 5 s for a
connected-but-silent client to send request bytes — serving nobody else.
Chromium's SPECULATIVE pool sockets connect and sit idle by design, so each
one deafens HTTP for 5 s. No app-level watchdog can see it (the wait loop
yields normally) and no JS can prevent it (browsers own their socket pool).

Fix: include/ui/IdleGuardWebServer.h — WebServer subclass (the needed
members are protected) whose dropIdleCapture() pump, called after
handleClient() in WebUI::update(), stops a client that has been connected
&gt;300 ms without sending a byte. Real requests send within milliseconds of
the handshake; only speculative/idle sockets are silent that long, and
dropping them is free (the browser reconnects on demand). Verified: 75 s
live-tab sampler went from 5 stalls (2× 9.7 s) to worst-case ~1 s across
two runs; page loads 0.6–0.7 s steady.

This subclass was scheduled demolition at the §4 PsychicHttp migration. That
migration was retired unflashed (2026-07-29), so the subclass is PERMANENT —
see the header comment in `include/ui/IdleGuardWebServer.h`. Riding along in
2.1.46:
SlopSyncHubService::loadPairing() opens the "slopsync" NVS namespace
read-write so first boot creates it and the scary Preferences NOT_FOUND
E-line (operator-misread as a boot blocker) never logs again.
