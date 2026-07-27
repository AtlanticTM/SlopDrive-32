# WebSocket transport baseline — links2004/arduinoWebSockets, fw 2.1.56

**Status:** measured live on 192.168.1.229, 2026‑07‑26, fw **2.1.56** (the
pre‑migration build). Everything below is observed, not reasoned — every number
has a JSON summary behind it, produced by `tools/slopsoak.py`.

**Headline:** the reported dropouts **reproduce**, deterministically, in one
specific scenario — and the device's own log names the whole causal chain. But
they do **not** reproduce in steady state: a quiet three‑client soak is clean.
Read §7 before treating this as "links2004 is broken all the time", because that
is not what the data says.

---

## 1. Why this document exists

The firmware's SlopSync WS transport (`SlopSyncWsTransport`) sits on
links2004/arduinoWebSockets, whose `sendBIN` is a **synchronous busy‑wait**: it
blocks the calling task until the socket accepts the bytes or it gives up. The
plan is to replace it with ESP32Async's `AsyncWebSocket`.

A transport swap that is *assumed* better is a regression waiting to happen. So
before touching it: build a harness, capture the old numbers, and write down what
"better" would have to mean. This file is the old numbers.

> SlopSync only earns its place if it is rock solid. A client that bugs out and
> drops is worse than no client — so the bar is not "the new transport looks
> fine", it is "the new transport passes the scenario that provably kills the
> old one, with the same harness, on the same device."

---

## 2. The harness

`tools/slopsoak.py` — a stability/soak harness, tracked in git precisely because
it is the evidence‑generating tool for this migration.

```bash
set PYTHONIOENCODING=utf-8         # or unicode output dies on cp1252

python tools/slopsoak.py --label links2004-baseline            # full suite
python tools/slopsoak.py --scenarios wedge-chatty              # just the killer
python tools/slopsoak.py --scenarios soak --soak-duration 300  # steady state
python tools/slopsoak.py --list
```

It emits a human table **and** `slopsoak-<label>-<timestamp>.json`, so two runs
diff directly. It imports `tools/slopsync_probe.py` for the wire layer (frame
header, the §5.3 deterministic CBOR profile, message builders, registry
constants) rather than duplicating it — one wire implementation, no drift.

### Scenarios

| Scenario | What it does | What it is actually asking |
|---|---|---|
| `b2b` | 3 consecutive full sessions, **no reboot between**, torn down clean / RST / abandoned | CLAUDE.md §8's mandatory pattern — does source ownership leak across teardown? |
| `rst` | 10× full session then `SO_LINGER 0` → TCP **RST** | does the §11.4 rude‑death path release everything? |
| `churn` | 30 rapid connect/handshake/disconnect cycles | slot leaks, handshake latency under pressure |
| `wedge-silent` | a client completes the handshake, subscribes at high rate, then **stops reading its socket** and also stops talking | the firmware's mute‑then‑evict sweep should reclaim it |
| `wedge-chatty` | same, but the wedged client **keeps sending PINGs** | `pushRx()` clears the send‑stall mute on any inbound byte, so this re‑arms the blocking write over and over |
| `stream` | motion‑input (0x0084) at the granted rate while watchers watch | ingress under load |
| `soak` | N concurrent steady‑state sessions, long duration | the plain "does it just stay up" test |

Throughout, three background monitors run for the whole harness lifetime:

- **HTTP** — `GET /api/status` every 2 s. This is the operator‑visible symptom
  ("the WebUI says unreachable"), measured directly rather than inferred. It
  also watches `uptime_ms` and flags **device reboots**.
- **Device log** — `GET /api/log`, de‑duplicated, parsing the `[sys] heap
  free=/min=/maxblock=` beacon and harvesting the firmware's own W/E lines.
  Lines are attributed to scenarios by the *device's* clock, not by when we
  happened to poll.
- **WebUI link (`:81`)** — a passive observer of `ws://ip:81/ws/ui`, the
  **separate** legacy links2004 socket the browser actually uses. Without this,
  a run could look spotless on `:82` while the thing the user complains about
  was dropping.

### Failure vs expected — the harness is strict on purpose

- A deliberate RST, a deliberate GOODBYE, or the firmware evicting a client we
  *deliberately wedged*: **expected**, never counted as a dropout.
- A disconnect of a client that was reading and sending keepalives:
  **unexpected**. That is a dropout.
- **A socket that stays open while the session dies counts as a dropout.** The
  hub can tear a session down while TCP stays perfectly connected, and a client
  that only watches for a close event sits there showing stale data forever. To
  a user that *is* a dropout.
- **STATE `seq` gaps are not loss.** The hub bumps a channel's retained seq on
  every publish and paces sends at the granted rate, so a ~60 Hz channel granted
  at 20 Hz legitimately steps +3. Only seq *regression* or a repeat is a wire
  defect; stalls are measured as arrival gaps instead.

---

## 3. Runs

Four runs, ~35 minutes of device time. The suite run is the headline; the rest
exist because the first pass raised questions the first pass could not answer.

| Run | Label | Scope | Wall | Verdict |
|---|---|---|---|---|
| A | `links2004-baseline` | full suite, 12‑min soak | 17.4 min | FAIL |
| B | `links2004-baseline-b` | wedge ×2 + 7‑min soak, **with `:81` observer** | 9.2 min | FAIL |
| C | `soak-only-attribution` | soak only, 5 min | 5.1 min | 0 dropouts; leak test false‑positived (§8) |
| D | `soak-final` | soak only, 5 min | 5.1 min | PASS |

**Why 17 minutes for run A:** long enough that a slow leak or a periodic reaper
would show — the 12‑minute soak alone delivered **18,503 frames per client**
(13,729 of them on `motion (0x0080)`) — and short enough to re‑run after every
firmware change without it becoming a chore. The A/B is only useful if the
"after" run is cheap enough that nobody skips it.

### Run A summary

```
scenario      result   dur(s)   http  httpErr  logWarns
b2b           PASS       15.5      8        0        2
rst           PASS       31.5     15        0        9
churn         PASS       16.4      8        0       29
wedge-silent  PASS       62.5     30        0        1
wedge-chatty  FAIL       62.4     28        0        4
stream        PASS      120.8     58        0        0
soak          FAIL      722.5    345        1        2

http  499 polls, 1 failure, p50 57.9 ms, p95 116.9 ms, max 3596.4 ms
```

---

## 4. What passed, and how well

**`b2b` — 3/3.** Every one of three consecutive sessions (clean / RST /
abandoned teardown, no reboot between) was granted `motion-input (0x0084)` at
50 Hz. No stranded source ownership. The bug CLAUDE.md §8 records as "the third
field bug for the ages" stays fixed.

**`rst` — 10/10.** Ten RST teardowns; every following session established and
took the source.

**`churn` — 30/30.** Handshake latency p50 **111 ms**, p95 164 ms, max 195 ms.
Zero failures.

**`stream` — clean.** 5,890 bundles at **49.1 Hz** against a 50 Hz grant; the
device counted 5,886. Four unaccounted, zero NACKs — consistent with in‑flight
frames at teardown, not loss. Watchers held 18.9 Hz with a worst gap of 341 ms
and no disconnects. (The machine was unhomed at that point, so all 5,886 samples
were correctly dropped at the HOMED gate: full ingress path, zero motion.)

**`wedge-silent` — PASS, twice.** Healthy watchers held **19.2–19.3 Hz** against
a 19.1–19.2 Hz baseline, worst gap 83–173 ms. Device free heap never dipped
below **48,888 B**. The victim recovered on resuming reads.

That last result is the control, and it matters: the *silent* wedge is harmless
because the victim never sends anything, so the transport's mute latches and the
hub stops trying. One failed `sendBIN`, then silence.

---

## 5. THE REPRODUCTION — `wedge-chatty`

A client that **stops reading but keeps talking**. Reproduced identically on two
independent runs.

| Metric | Run A | Run B |
|---|---|---|
| Healthy watchers, baseline | 19.16 Hz | 19.3 Hz |
| Healthy watchers, **during wedge** | **9.48 Hz** | **9.5 Hz** |
| Healthy watchers, **after wedge cleared** | **0.0 Hz** | **0.0 Hz** |
| Healthy watchers, **after bad client fully closed** | **0.0 Hz** | **0.0 Hz** |
| Healthy sessions killed | **2 of 2** | **2 of 2** |
| Device free heap, min | — | **8,776 B** (largest free block **2,036 B**) |
| `/api/status` latency, max | **3,596 ms** | 86 ms |
| The wedged client itself | survived, 756 frames after resume | survived, 752 frames |

Raw per‑watcher frame counters (run B), which are a deliberately dumber witness
than the gap analysis — they agree:

```
delta_baseline    [312, 312]     12 s window
delta_wedged      [319, 319]     25 s window   <- half the rate
delta_recovery    [  0,   0]     15 s window   <- dead
delta_after_close [  0,   0]     10 s window   <- still dead
```

### The device's own log, unedited

```
[805.659 I sys]      heap free=45740 min=208 maxblock=31732
[815.659 I sys]      heap free=8776  min=208 maxblock=2036
[818.891 W slopsync] client#2 send stalled — muted (kept connected)
[823.985 W sys]      http:ui.update blocked 2219ms
[824.841 I slopsync] session d698ab93 left
[824.843 I slopsync] session b48f6196 left
```

That is the entire failure, in six lines:

1. **One client stops draining.** Its un‑ACKed TX data piles up in lwIP and eats
   internal heap: 45,740 → **8,776 B** free, largest free block 31,732 →
   **2,036 B**. The heap is gone in ten seconds.
2. **The blocking send bites.** `sendBIN` busy‑waits on a socket with a zero
   receive window. `SlopSyncWsTransport` mutes the client after the first
   failure — but `pushRx()` clears the mute on *any* inbound byte, and this
   client is still PINGing, so the mute is cleared and the blocking write is
   re‑armed on the next pump. The SlopSync service task is stuck in that loop.
3. **httpTask blocks for 2,219 ms.** That is the WebUI going unreachable, in the
   firmware's own words. In run A the same window produced a **3,596 ms**
   `/api/status` response.
4. **The healthy sessions die.** With the service task stalled, their inbound
   PINGs are not serviced in time and the §6.5 deadman reaps them — sockets
   still open, sessions gone.
5. **It never recovers.** Not when the bad client resumes reading, not when the
   bad client is fully closed. The two healthy watchers received exactly zero
   frames for the remaining 25 seconds. The only client that survived was the
   one that caused the problem.

**The last point is the one to keep.** This is not "telemetry gets choppy while
a bad client misbehaves". A misbehaving client permanently kills every *other*
client's session and cannot be recovered from without reconnecting them. That is
precisely the shape of the operator's complaint.

### Why `chatty` and not `silent`

The difference is one line in `SlopSyncWsTransport::pushRx()`:

```cpp
// Inbound traffic proves the client is draining: clear any send-stall mute
_muted = false;
```

Inbound traffic proves the client is *sending*. It proves nothing about whether
it is *draining*. A client that talks and never listens therefore gets its
stream re‑armed forever, and each re‑arm is another blocking write on the one
task that serves everybody.

The async transport removes the blocking write, which should make this whole
class disappear. **It does not by itself fix the heap growth** — an undrained
client's TX backlog still has to live somewhere, and 8.7 KB free with a 2 KB
largest block is a device that cannot serve a web page. Whatever queue the new
transport uses needs a hard per‑client cap with a drop‑and‑disconnect policy.

---

## 6. Steady state: clean

| Run | Clients | Duration | Unexpected disconnects | HTTP failures | `:81` drops | Heap verdict |
|---|---|---|---|---|---|---|
| D | 3 | 300 s | **0** | 0 / 147 | 0 | no leak |

Run D detail: each client held **19.0 Hz** on `motion (0x0080)` against a 20 Hz
grant; `:81` observer saw **28,942 frames at 95.2 Hz**, worst gap 840 ms, zero
drops; `/api/status` p50 47.9 ms, p95 105.8 ms.

Heap over a 10.8‑minute reboot‑free window: median **56,660 → 56,648 B** against
**11,908 B** of p10–p90 noise. Flat. **No leak.**

---

## 7. Honest read — what did NOT reproduce

**A quiet soak does not drop.** With three well‑behaved clients at 20 Hz, over
5, 7 and 12 minute runs, the SlopSync plane on links2004 is solid: 19.0 Hz
sustained, no NACKs, no unexpected disconnects, flat heap. If the operator's
dropouts happen during ordinary use with well‑behaved clients, **this harness
has not reproduced that**, and the wedge result is not evidence that it will.

Every "dropout" the soak scenario reported in runs A and B turned out to be a
**device reboot**, not a transport failure. The correlation is exact, not
approximate:

| Run | Client disconnects | When | Reboot detected |
|---|---|---|---|
| A | 3 (one per client) | 18:41:13 | 18:41:16 |
| B | 6 (two per client) | 18:58:07, 19:03:12 | 18:58:11, 19:03:16 |

Three clients × the number of reboots, every time, all three at the same second,
each ~4 s before the HTTP poller noticed — which is exactly the expected order
(a client's 5 s silence watchdog fires before a 2 s poll interval plus timeout
resolves). Once reboots are excluded, steady state is clean: run D, 300 s, three
clients, **zero**.

**So what does the wedge result actually prove?** That there exists a client
behaviour — stop reading, keep talking — that reliably and permanently kills
every other SlopSync session and blocks the HTTP task for seconds. Whether that
behaviour occurs in the field is a separate question. Plausible causes: a
browser tab backgrounded by the OS, a Wi‑Fi client whose radio sleeps, a paused
MFP process, a laptop suspending. All of those produce exactly this pattern —
TCP stays up, the receive window closes, the client's keepalive timer keeps
firing. That is a *hypothesis*, and this document is not entitled to call it the
cause of the operator's dropouts.

**What would settle it:** the next time the WebUI actually goes unreachable in
real use, pull `/api/log` before rebooting and look for `send stalled — muted`
and `http:ui.update blocked`. Those two lines together confirm this mechanism in
the field. Their absence points somewhere else entirely.

---

## 8. Confounders, and one harness bug worth recording

**Four device reboots during the session** (≈18:33, 18:41, 18:58, 19:03), none
preceded by a heap warning or a stall warning. Three of the four
landed during the *gentlest* scenario (a quiet soak); none landed during the
wedge, which is the only scenario that drives heap toward the floor. If load
caused reboots, the wedge would be the one rebooting.

Another workstream was actively editing `SlopSyncWsTransport.cpp` and
`platformio.ini` and flashing this device throughout. An OTA flash reboots the
device and leaves `fw_version` unchanged if the version was not bumped — which
matches what was observed. **The harness cannot distinguish an OTA reboot from a
crash**, and that ambiguity is itself a result: `slopsoak` now detects reboots
via `uptime_ms` and flags every scenario that spans one, so the numbers are
marked untrustworthy instead of silently corrupted. A dedicated re‑run on an
undisturbed device would remove this footnote entirely.

**Three measurement bugs were found and fixed in the harness before these
numbers were taken.** Recorded because each one produced a *confident wrong
answer*, and the same traps apply to whoever runs the "after" comparison:

1. **The keepalive only fired on a read timeout.** §6.5 liveness is
   one‑directional — frames arriving *from* the hub prove nothing about us — so
   while telemetry was flowing the harness never pinged at all and the hub
   correctly reaped every session ~3 s after connect. This produced a
   spectacular false positive ("the whole hub dies after 3 seconds") in the
   first wedge runs. Keepalive is now unconditional, on a timer derived from the
   `deadman_ms` the hub declares in WELCOME.
2. **A window with zero frames scored PASS.** The gap‑ratio comparison had no
   gaps to compare because nothing arrived at all, so the worst possible outcome
   fell through every branch. "Zero frames" is now checked first and is the
   loudest failure.
3. **The heap leak test cried wolf twice.** A least‑squares fit over the whole
   run is dominated by post‑boot settling and by any reboot inside the window
   (which teleports free heap upward). Run A reported "‑1,629 B/min, leak" purely
   from those artifacts; a later run reported "‑2,596 B/min, leak" on a soak that
   *ended 3,504 B up*. The test now splits the series at reboots (detected from
   the device's own log timestamps going backwards), drops the first 60 s of each
   segment, and requires the **last third's median to sit below the first
   third's by more than the run's own p10–p90 spread**. Noise cancels in the
   medians; a real leak does not.

**One incident to disclose:** the first draft of the streaming scenario took its
"hold at the current position" target from plan‑strip's `cur` field. On an idle
machine with no active plan that reads **0.5** — the middle of the stroke window,
not where the carriage is — so the first streaming test moved a homed machine
from 150.9 mm to 223.5 mm. It is now derived from `/api/settings`
(`range_min`/`range_max`) and `/api/status` (`position`), and streaming is
refused outright if either is unreadable. Default `--stream-amp` is `0.0`.

---

## 9. Acceptance criteria for the AsyncWebSocket replacement

Run the same suite, same device, same label convention. The migration is
justified when:

| # | Criterion | links2004 baseline |
|---|---|---|
| 1 | `wedge-chatty` healthy‑watcher rate during the wedge ≥ 80 % of baseline | **49 %** (19.3 → 9.5 Hz) |
| 2 | `wedge-chatty` healthy watchers still receiving after the wedge clears | **0 frames — dead** |
| 3 | `wedge-chatty` healthy sessions killed = **0** | **2 of 2 killed** |
| 4 | Device free heap during `wedge-chatty` stays above ~30 KB, largest block above ~16 KB | **8,776 B / 2,036 B** |
| 5 | `http:ui.update blocked` never exceeds ~200 ms during the wedge | **2,219 ms**; `/api/status` max **3,596 ms** |
| 6 | Everything already passing stays passing — `b2b` 3/3, `rst` 10/10, `churn` 30/30, stream loss ≤ 0.1 %, soak 0 unexpected disconnects | all currently pass |
| 7 | `:81` WebUI link: 0 drops outside reboots | 0 drops outside reboots |

Criterion 6 is the one to watch. The async transport moves callbacks onto a
different task, and the current transport's **one‑task invariant is why it is
deliberately mutex‑free** (`include/comms/SlopSyncWsTransport.h`). A regression
in `b2b`/`rst` after the swap most likely means that invariant was broken rather
than that the new library is slow.

---

## 10. Not tested

- **Real client behaviour.** Every client here is `slopsoak.py`. The MFP plugin
  and the WebUI's own SlopSync bridge were not exercised.
- **More than 3 concurrent SlopSync sessions.** The hub floor is
  `kHubMaxSessions = 4`; the 4‑session and over‑subscription cases are untested.
- **Segment streaming (`0x0085`)**, and streaming with real motion. Amplitude was
  0.0 throughout on a shared device.
- **Long soaks.** The longest steady‑state window here is 12 minutes, and it
  contained a reboot. Nothing here can speak to a leak or a reaper with an
  hours‑long time constant.
- **Wi‑Fi‑layer failures** — roaming, RSSI collapse, AP restarts. The device held
  −52 dBm on one AP throughout.
- **The C5 relay/display nodes.** Out of scope; no web server, no SlopSync
  transport.

---

## 11. Artifacts

JSON summaries (scratchpad, not committed — regenerate with the commands in §2):

- `baseline.json` — run A, full suite
- `baseline-b.json` — run B, wedge ×2 + soak, with `:81` observer
- `soakfinal.json` — run D, clean 5‑minute soak

Re‑run the "after" side with `--label asyncws` and diff the two JSON files
scenario by scenario.
