# slopsim — the SlopDrive terminal simulator

Roadmap §6 "SlopSim", grown a face. One standalone desktop binary, two modes:

* **machine mode (LIVE)** — a virtual SlopDrive. Embeds the **real**
  `slopsync::Hub`, the **real** `slopmotion::Engine` (quintic/chase/settle over
  vendored Ruckig), and the **real** device catalog
  (`include/comms/SlopSyncCatalog.h`) behind a real WebSocket server speaking
  `slopsync.v1`. Real clients — `tools/slopsync_probe.py`, `webui/test/*.mjs`,
  slopsync-js, the MFP plugin — connect as if it were hardware.
* **client mode (NEXT)** — the tabbed SlopSync cockpit (motion / telemetry /
  config / settings / logs) that doubles as the WebUI-refactor spec vehicle.
  Works against the sim or the live device.

Doctrine: **100% of control goes through SlopSync.** HTTP exists only to serve
the page and two read-only JSON views (`/api/capabilities`, `/api/slopmotion`).
No legacy UiSocket plane, ever.

## Build (Windows, WinLibs GCC + CMake + Ninja)

```bash
export PATH="/c/Users/Atlan/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin:$PATH"
cmake -S sim/slopsim -B sim/slopsim/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc
cmake --build sim/slopsim/build
```

Deps are FetchContent-pinned (FTXUI v6.1.9 MIT, IXWebSocket v11.4.6 BSD-3,
cpp-httplib v0.20.1 MIT). The exe is `-static` linked — no DLL hunting, copy it
anywhere. Linux should build with the same CMake (untested yet).

## Run

```bash
SlopCLI                       # bare launch = machine TUI (shims in ~/bin)
slopsim machine --homed --headless --duration 60   # scripted / CI
# flags: --port 82 (WS) --http 80 --homed --headless --duration S
#        --webui <dist/index.html> --no-mdns
#        --policy <scale|stretch>                     (engine knobs at launch)
#        --jerk <mm/s3>     INPUT-set jerk ceiling (default 2000000; /span -> jmax)
#        --jmax <units/s3>  NORMALIZED jerk override; wins over --jerk when > 0
#        --speedmode <pegged|matched>   stream speed feed (device default: pegged)
#        --no-timer-boost   don't raise the Windows timer resolution (A/B only)
```

**The loop runs at 1 ms and it is not free.** The sim thread paces itself with a
deadline loop that coarse-sleeps then yield-spins the last millisecond, so it
sits at roughly one core while running. That is deliberate: no Windows sleep
primitive can land a 1 ms deadline (measured on this host — `sleep_for(1ms)`
15.7 ms, `Sleep(1)` with `timeBeginPeriod(1)` 1.9 ms, high-resolution waitable
timer 1.5 ms, yield-spin **1.000 ms**), and the 1 ms grid is what makes stream
commits land where the device lands them. `--no-timer-boost` only skips
`timeBeginPeriod(1)`; it does not disable the spin.

**Command palette** (`/`): type to filter, `Tab`/`Enter` completes, `Enter` on a
complete command runs it. Commands: `home unhome estop clear pause resume
override`, `window <min> <max>`, `speed.user/input <mm/s>`,
`accel.user/input <mm/s2>`, `jerk.input <mm/s3>` (all clamped exactly like
0x0101 config-set — jerk is key 7, ceiling 50 000 000 — applied
value echoed in the toast), `motion` + `motion.policy <scale|stretch>` /
`motion.margin <0.5-1.0>` / `motion.aimff <on|off>` / `motion.jmax <units/s3>` /
`motion.speedmode <pegged|matched>` (SlopMotion + arbiter knobs —
the palette twin of the device's `POST /api/slopmotion`; bare `motion`, or `m`,
toggles the **engine config panel** — the applied `slopmotion::Config` grouped
into policy / limits / chase / centering, read from the engine itself so it shows
the same numbers as `GET /api/slopmotion`), `machine.stroke <mm|0>` (measured-stroke ceiling;
0 = fall back to the max rail), `pattern <0|1|2|off>` + `pattern.speed/depth/
stroke/sensation`, `graph freeze zoom export [file.csv]`,
`segments [on|off|auto]` + `segments.reset` / `segments.export [file.csv]`
(the inbound wire recorder — see below), `kick`, `congestion`, `quit`.

`GET /api/slopmotion` carries a `sync` block with DEVICE semantics
(`bundles`/`seg_bundles`/`samples`/`enqueued`/`dropped`, the `sm_sync_*` twins)
plus a `simstats` block that is host-only and never confusable with a device
counter: `plan_rejected` (engine `commit()` refusals — the same events the
device reports as `sm_failures`, not wire loss), `ts_clamped` (far-future
wire timestamps), `substeps_discarded` (ms of motion time lost to a host stall),
`tick_ms_avg`/`tick_ms_max` (measured loop period — if this is not ~1 ms the
sim's motion output is not trustworthy), `stream_speed_mode`, and `ingress`
(the inbound wire recorder's red-flag block — see below).

## Inbound wire recorder — what the CLIENT actually sent

The trace ring answers *"did the machine render this well?"*. This answers the
other half: **"was what arrived on the wire what the sender meant to send?"**
When a client (the MFP plugin, slopsync-js, your own) produces motion that is
"not even remotely close to intended" while the sim renders faithfully, the bug
is upstream of the machine and only wire content can prove it.

Every accepted 0x0084 sample and 0x0085 segment is recorded **where it is
decoded** (`MachineSim::onStreamBundle`), RAW wire values and DECODED values
side by side, into a 4096-row newest-wins ring on the same time base as the
trace ring — so a row here lines up with a sample from `/api/trace.bin`.

* `GET /api/segments.csv` — the ring as CSV, header row, newest-last.
  `?since=<t_s>` for incremental pulls, exactly like `/api/trace.bin`.
* `GET /api/segments.json` — same rows plus the red-flag block, for scripts.
* `/api/slopmotion` → `simstats.ingress` — the red-flag block on its own.
* TUI: `s` cycles the panel **auto → pinned on → off** (`/segments [on|off|auto]`
  does the same). **Auto is the default** and shows the panel whenever anything
  was recorded in the last 10 s, so it appears when a client starts sending and
  folds away when it stops. `/segments.reset` clears ring + stats,
  `/segments.export [file.csv]` writes the same CSV to disk.

### CSV columns

| column | meaning |
|---|---|
| `t_s` | arrival time, sim seconds — **same clock as `/api/trace.bin`** |
| `ch` | `0x0084` (sample) or `0x0085` (segment) |
| `kind` | `sample` / `segment` |
| `raw_target` | **WIRE u16**, pre-scale (target × 10 000) |
| `raw_dur_ms` | **WIRE u16** duration, ms (0x0085 only; 0 on 0x0084) |
| `raw_end_vel` | **WIRE i16** (× 1000); `-32768` is the no-end-velocity sentinel |
| `target_norm` | decoded normalized target — must equal `raw_target/1e4` |
| `duration_ms` | decoded duration — must equal `raw_dur_ms` |
| `end_vel_norm` | decoded end velocity — must equal `raw_end_vel/1e3` |
| `has_end_vel` | 0 = the sentinel was sent (0 is a **real slope**, not "none") |
| `accepted` | 0 = refused at decode (`duration_ms == 0`) — still recorded |
| `ts_clamped` | 1 = the wire timestamp was >250 ms in the future and got clamped |
| `wire_t_off_us` | `t_off[i]` within the bundle |
| `wire_t_us` | `t_base + t_off[i]` — the absolute stamp the sender put on it |
| `due_us` | the local time the pacing ring will release it |
| `due_delta_ms` | `due − now`: **the sender's timestamp skew** |
| `gap_ms` | arrival Δ from the previous row on the SAME channel (`-1` = first) |

### Red-flag stats (`simstats.ingress`, and the panel's top line)

| stat | what a bad value means |
|---|---|
| `dur_floor_10ms` | segments at the MFP plugin's 10 ms floor — a violent, physically infeasible command; the sender computed a near-zero interval |
| `dur_under_50ms` | segments **below the WAVEFORM threshold**: SlopMotion routes these to CHASE instead of the quintic. A silent mode switch — the machine stops reproducing the sender's spline and starts point-chasing |
| `seg_gap` / `smp_gap` | inter-arrival min/mean/max, **per channel, at bundle granularity**. Compare `seg_gap.mean_ms` against the mean commanded `duration_ms`: they should match |
| `duty_ratio` | `Σ(commanded durations) / (wall-clock span covered)`, over 0x0085. **≈1.0** = the sender is covering script time contiguously. **≪1.0 = the sender is SKIPPING script time** — it asks for a 400 ms move and then says nothing for 2 s. **≫1.0** = it is overlapping/preempting its own segments |
| `sentinel` | 0x0085 rows that carried the no-end-velocity sentinel |
| `rejected` | `duration_ms == 0` — refused, and a plain sender bug |

Stats are cumulative since boot or `/segments.reset`; a sim left running across
idle gaps reports a duty ratio diluted by the silence, so reset before the take
you care about.

### Reading the CSV

* **Skipped script time** — `duty_ratio` well under 1.0, or per-row: `gap_ms`
  consistently larger than that row's `duration_ms`. Diff `t_s` deltas against
  the funscript's action timestamps; if the CSV has 12 segments where the script
  has 40, the sender is dropping keyframes, not the machine.
* **Units / scale error** — compare the `raw_*` and `*_norm` columns. They are
  the same number twice (÷1e4, ÷1e3, ×1). A `raw_target` of 8000 that should
  have been 0.8 of full stroke is right; one that reads 80 or 65535 is a
  scale bug in the sender, and you can see it *before* any machine clamping
  touched it. `raw_end_vel = 0` with `has_end_vel = 1` is a genuine
  zero-slope handoff; `-32768` with `has_end_vel = 0` is the sentinel — a
  sender that confuses the two shows up here immediately.
* **Timestamp skew** — `due_delta_ms` should be small and positive (a sender
  timestamps a few ms ahead). Large positive = the sender is scheduling far into
  the future; `ts_clamped = 1` = past the 250 ms clamp, i.e. it lost CLOCK sync.
  Persistently 0.000 with `wire_t_us` behind `due_us` means the stamps arrive
  already late and everything is being released immediately — the pacing is
  gone and segments effectively preempt each other.

**Graph pane** (glanceable, in-terminal): live pos (green) / engine target
(yellow) / velocity (purple) braille plot with window-band guides. `g` toggle ·
`z` fullscreen · `f` freeze (then `←/→` pan, `+/-` zoom) · `/export` writes the
whole ~4-minute 1 kHz trace ring as CSV.

**Analyzer popout** (the real instrument): `a` or `/analyze` opens
`http://127.0.0.1/graph` in the browser — a rendered canvas view of the same
1 kHz trace: per-pixel min/max envelope drawing (honest detail at any zoom),
wheel-zoom at cursor, drag pan, crosshair sample readout, stroke-window band,
live-follow or paused analysis, visible-range stats, CSV + PNG export. Data
rides `GET /api/trace.bin` (20-B header {n,max_rail,win_min,win_max,stride} +
n×stride f32 LE, stride 5 = {t,pos,tgt,vel,cmd_norm}; `?since=<t_s>` for
incremental polls at ~3 Hz — `stride` is in the header so the layout is
self-describing to a curl/python reader too). The page is embedded in the exe
(`src/net/GraphPage.h`) — no files to ship.

The **cmd** trace is the commanded move as it arrived, NORMALIZED 0..1, mapped
through the live stroke window by the page itself (`win_min + c·span`). Because
slopsim IS the hub it can record the inbound 0x0084/0x0085 target directly, so
this is genuinely upstream data and not `tgt` under another name: if the
normalization and the window mapping are right the dashed cmd line sits on top
of tgt, and any separation is a scaling/window bug made visible. It is a
staircase (held between commands) and absent before the first command of a
session. Toggle it with the `cmd` button.

**The page stays O(canvas pixels), not O(samples).** A 1 kHz feed over an hour
is 3.6 M client-side samples; every series, the visible-range stats, the
velocity autoscale and the minimap all read an incrementally-built min/max
PYRAMID (32-sample level-0 buckets, ×4 per level) rather than walking raw
samples, and redraw is demand-driven instead of every animation frame. Bucket
extremes are EXACT, so a single-sample excursion survives every zoom level —
decimation here never averages a spike away, which is the one thing this tool
must not do.

Other keys: `h u e c p o k` as before · `↑↓` select session slot · `q` quit.

**mDNS**: machine mode advertises `slopsim._slopsync._tcp` (TXT proto/fw) via
the native Windows responder (DnsServiceRegister) — discovery-capable clients
find the sim like hardware. Instance name is deliberately `slopsim`, never
`slopdrive32`, so the sim can't impersonate the real machine.

## Stream speed feed: ceiling-pegged vs velocity-matched

`SystemState::stream_speed_mode` has two branches and **the device default is
ceiling-pegged (0)**: the sampler hands FAS the full speed *ceiling* every 1 ms
and lets the micro-target position deltas shape the velocity, so the follower
always has authority to close an error. Velocity-matched (1) hands FAS the
curve's own instantaneous speed as its vmax — which structurally means it can
never recover lag, only track or fall behind.

The sim used to implement *only* velocity-matched, i.e. only the non-default
mode, which made every stream result quietly pessimistic. Both branches are
transcribed now; switch with `motion.speedmode` / `--speedmode`. Measured on
alternating full-window 400 ms segments (200 mm window, 1000 mm/s, 50 000
mm/s², default jerk):

| mode | achieved amplitude | peak velocity |
|---|---|---|
| matched (old sim-only behavior) | 196.4 mm (98 %) | 937 mm/s |
| **pegged (device default)** | **200.1 mm (100 %)** | 950 mm/s |

That missing 2 % was never the planner — it was the sim running the wrong mode.

## Verify (all green as of 2026-07-24)

```bash
# full wire pass: HELLO→WELCOME→GRANT→retained safety→move INTENT/ECHO→
# 0x0084 chase stream→deadman→0x0085 segments→HTTP counters   (16/16)
.venv/Scripts/python.exe tools/slopsync_probe.py --ip 127.0.0.1 --port 8282 --stream 4 --segments 4

# slopsync-js core (the WebUI's future wire layer), two back-to-back sessions
node webui/test/slopsync-live.mjs --ip 127.0.0.1 --port 8282
```

Run the probe **twice without restarting the sim** — the back-to-back-sessions
regression pattern (firmware field bug #3) is mandatory for any
session-lifecycle change here too.

**Probe + non-default HTTP port, a trap worth knowing:** `slopsync_probe.py`
hard-codes `http://<ip>/api/slopmotion` (port 80) for its counter steps. Run the
sim on any other HTTP port and that step doesn't just fail — on Windows the
refused loopback connect **blocks the probe for ~2 s**, which exceeds the §6.5 /
§11.3 600 ms silence window, so the *next* step (`segment_clock`) gets no reply
either and you lose two checks (12/14) for one port mismatch. Measured, not
guessed. Either run the sim's facade on :80 or point a copy of the probe at your
port; a wire-layer failure and this artifact look nothing alike once you know.

## Architecture map

```
src/common/HostPlatform.h   HostClock/HostRandom — EspClock/EspRandom analogs
                            (u32 wrapping hub clock + unwrapped u64 engine clock)
src/net/WsServerPort.*      IXWebSocket -> ITransport, firmware SlopSyncWsPort
                            semantics (5 slots, RX rings, mute/2s-evict).
                            Connection threads only MARSHAL into mutex-guarded
                            rings; ONLY the sim thread touches the hub.
src/net/HttpFacade.*        cpp-httplib, read-only JSON/CSV, own thread, never
                            touches the hub (reads a mutex-guarded stats copy);
                            /api/segments.csv|json serve the wire recorder ring
src/machine/MachineSim.*    composition root + HubDelegate transcribed from
                            SlopSyncHubService.cpp (gates/NACKs/clamps/echoes
                            identical), PacingRing copy, SimPattern generators,
                            telemetry publishers byte-mirroring publishTelemetry()
      SimStepper            FAS modeled at the MotorDriver seam: trapezoidal
                            ramp follower fed 1 kHz engine setpoints under the
                            ceilings submitStreamSample derives (speed mode,
                            soft start, window entry, envelope clamp).
                            Telemetry pos = stepper (ACTUAL), tgt = sampled
                            setpoint — same divergence as hardware.
src/common/HostPlatform.h   also owns the loop pacing: HostTimerResolution
                            (timeBeginPeriod RAII) + preciseSleepUntil
                            (hi-res waitable timer, yield-spin residual)
src/tui/MachineScreen.*     FTXUI; sim tick + UI pumped on ONE thread
                            (ftxui::Loop::RunOnce) — one-task invariant intact
cmake/PatchIXWebSocket.cmake  RFC6455 subprotocol echo upstream omits (below)
```

## The host timer, and why the sim used to lie about chase mode

The sim's loop slept 2 ms per iteration. On Windows at default timer resolution
that is **15.6 ms** — measured 15.713 ms per iteration, 50 iterations in
785.6 ms — so the "1 kHz sampler" was a ~64 Hz burst generator: every host tick
replayed ~16 substeps, and because the pacing ring was drained *before* the
substep loop, a freshly committed plan evaluated at τ=0 (its START point) for
every substep in the burst. Net: the engine setpoint held **flat for a whole
tick and then jumped**. At 50 Hz chase ~78 % of ticks carried a commit, so the
sim's chase output was a staircase — and chase fidelity is exactly what a
streaming client comes here to test.

Fixed on both axes: the loop is deadline-paced at 1 ms (see Run, above) and the
ring is drained *inside* the substep loop at each substep's own instant, so
`elapsed == 0` lasts one 1 ms sample like it does on the device.

Measured, 50 Hz sine chase (0.5 Hz, ±40 % of a 500 mm window, 1000 mm/s):

| | before | after |
|---|---|---|
| loop period | 15.71 ms | **1.000 ms** (max 1.9) |
| consecutive samples with `tgt` unchanged | 73.3 % | **0.34 %** |
| setpoint flat-run length | mean 3.75 ms, 15-17 ms whenever a commit landed | **mean 1.00 ms**, p90 1 ms |
| largest single-sample `tgt` jump | 21.13 mm | **1.00 mm** |

## Motion policy — and the EXPERT-ceiling divergence

When a commanded segment is physically impossible in its commanded duration,
something has to give. The full policy set and their semantics are owned by
`InfeasiblePolicy` in `lib/slopmotion/include/slopmotion/slopmotion.hpp` — this
section is a summary for sim users, not the source of truth; read the enum's
own doc comment for the derivation. The engine default is **Reshape**, not
Scale — the sim only overrides it if you pass `--policy`/`motion.policy`.

Five policies, in registry order:

* **Stretch** — range-first: keeps the full stroke, overruns the deadline
  (what a "go here" manual point move wants).
* **Scale** — timing-first + shape-first: keeps the deadline, shrinks the
  stroke around the current position, keeping a quintic shape.
* **Reshape** (engine default) — timing-first + machine-first: keeps the
  deadline AND the full stroke's shape as far as the machine allows, giving up
  shape fidelity before range. `motion.reshape` / `--reshape-steps 0-8` sets
  the bisection depth (0 = no bisection).
* **PrioritizeAmplitude** (`amp` on the CLI) — budgeted: spends a smoothness
  budget before touching amplitude. `--smooth-budget 0-1` caps how far a
  handle may shorten toward the chord.
* **PrioritizeSmooth** (`smooth` on the CLI) — budgeted: spends an amplitude
  budget before touching smoothness. `--amplitude-budget 0-1` caps how much
  stroke may be surrendered.

`motion.margin` is how much of the physically-achievable stroke Scale/Reshape
actually ask for (headroom against the ceilings); `motion.aimff` makes the
chase-mode predictive aim second-order so it stops overshooting the
stroke-window rails at crests. `motion.curve` / `--curve follow|c1|c2` selects
the waveform curve family (`CurvePolicy`, same header) independently of the
infeasible policy. `--settle-grace <ms>` and `--centering on|off` /
`--centering-gain 0-1` are separate engine knobs also set at launch — see
`slopsim machine --help` for their current defaults.

`--policy <scale|stretch|reshape|amp|smooth>`, `--jerk <mm/s3>` and `--jmax
<units/s3>` set these knobs at launch, before the first segment is planned.
They exist as FLAGS because comparing engine behavior is a scripted job
(headless run → drive the wire → read `/api/trace.bin`) and the palette needs
a terminal. All go through the same `MachineSim::uiSet*` seam the palette
uses.

## Jerk is a mechanical limit, not a smoothing crutch

Jerk is now a **first-class mm-domain limit of the INPUT set**, sitting next to
speed and accel: `input_jerk_mm_s3` (default 2 000 000; NORMAL ceiling
10 000 000, EXPERT 50 000 000), settable via `jerk.input`, `--jerk`, or 0x0101
key 7, and published as the 8th f32 of 0x0081. The engine's normalized ceiling
derives exactly like the other two:

```
vmax = input_speed / span      amax = input_accel / span
jmax = jerk_override > 0 ? jerk_override : input_jerk / span      # firmware `jovr`
```

**Why this mattered.** Jerk used to be a bare normalized constant (`jmax = 500`,
no physical source). Because the *normalized* number was fixed, the *mechanical*
ceiling `jmax × span` scaled with the stroke window — 250 000 mm/s³ on a 500 mm
window but only 100 000 mm/s³ on a 200 mm window, on the same physical rail.
Backwards: shortening your stroke made the machine gentler on itself for no
reason. Deriving from mm makes the ceiling a property of the *machine*, and it
makes the jerk/velocity crossover span-independent too:
`T_x = sqrt(32·vmax_mm / jerk_mm)` — 126 ms at 1000 mm/s and 2e6 mm/s³,
whatever the window.

**It is not a smoothing knob.** Event-driven planning already removed the
clocked-output artifacts that jerk limiting used to paper over: there is no
sample-rate staircase to filter, because there is no clocked output — one
command becomes one C² quintic evaluated on its own. Whatever is left for the
jerk ceiling to do is *mechanical*: bound the rate of force change through the
belt and the motor's current loop. Read it as an accel rise time — `τ = amax /
jmax`, so 50 000 mm/s² over 2e6 mm/s³ is a 25 ms ramp to full accel. Turning it
down to "smooth things out" just costs stroke.

**Measured** (window 200 mm, 1000 mm/s, 50 000 mm/s², alternating full-window
segments, Scale policy, zero drops in every run):

| T | old (jmax 500 = 100 000 mm/s³) | new (2e6 mm/s³) | binding old → new |
|---|---|---|---|
| 400 ms | 98 mm | **196 mm** | jerk → velocity |
| 250 ms | 24 mm | **118 mm** | jerk → velocity |
| 150 ms | 5 mm | **68 mm** | jerk → velocity |

On a 500 mm window velocity was already binding, so 400 ms (193 mm) and 1000 ms
(500 mm) are **unchanged** — which is the point: the promotion did not raise any
ceiling the machine was actually using, it deleted an artificial one that only
appeared on short windows.

At vmax 1000 mm/s the quintic is **velocity**-bound for every segment ≳120 ms at
2e6 mm/s³ (200 ms delivers an identical 93.6 mm at 2e6 and at 1e7). Jerk only
takes back over below that: 100 ms gives 29 mm at 2e6 vs 37 mm at 4e6+, 80 ms
gives 16 mm at 2e6 vs 25 mm at 1e7.

So when a Scale run loses depth, check which of the three ceilings is binding
before blaming the policy — a min-jerk quintic's peaks are `v = 1.875·d/T`,
`a = 5.7735·d/T²`, `j = 60·d/T³`, so the max feasible normalized distance is
`min(vmax·T/1.875, amax·T²/5.7735, jmax·T³/60)`. Jerk is the one that scales as
`T³`, which is why it always wins on short segments.

`motion.jmax` / `--jmax` remain as a **normalized override** for exactly this
kind of A/B: when > 0 it wins over the derived value; 0 clears it back to
derived. Bare `motion` shows both the mm limit and the derived ceiling, and
flags when the override is live.

**Ceiling divergence, deliberate:** the sim clamps limits at the firmware's
**EXPERT** ceilings (10000 mm/s, 100000 mm/s², `config_api.h` ~332-348) because
the operator's real machine runs ~1000 mm/s and ~50000 mm/s² and the NORMAL
accel cap (20000) would refuse that outright. A device left in **NORMAL** mode
enforces 1000/20000 — so *the sim accepting an accel is not proof the device
will*.

**Sim DEFAULT input limits are 1000 mm/s / 60000 mm/s² — NOT the device factory
pair.** The device's factory-default INPUT limits live in exactly one place:
`DEFAULT_MAX_SPEED_MM_S` / `DEFAULT_ACCEL_MM_S2` in
`include/system/config_api.h`. A device still on those factory defaults will
not reproduce a sim trace taken at the sim's own defaults above: set one side
to match the other before comparing traces. The user set is unchanged
(50 mm/s / 200 mm/s²).

## What the sim models faithfully — and what it does not

Read this before trusting a sim result. "Faithful" means transcribed from the
named firmware source and verified over the wire, not "looks about right".

**Faithful (substitutable for the device):**

* **The protocol.** The real `slopsync::Hub`, the real device catalog, real
  frames. Sessions, grants, deadman, source ownership, NACK codes, ECHO
  post-clamp values, retained safety. `tools/slopsync_probe.py` cannot tell the
  difference.
* **The motion core.** The real `slopmotion::Engine` (quintic / Ruckig chase /
  settle) over vendored Ruckig, with ceilings derived from the mm INPUT limit
  set across the stroke window exactly as `streamSamplerTask` derives them.
* **The sampler seam.** `MotionArbiter::submitStreamSample` transcribed line for
  line: window clamp → `effectiveCeilingMm()` envelope → window-entry gentleness
  (outside the window ±0.5 mm ⇒ *both* speed and accel drop to the USER set) →
  `SystemState::safeSpeedCap` soft-start ramp (100 mm/s → ceiling over 1200 ms
  from the last `resume_start_ms`) → safe-approach floor → speed feed by
  `stream_speed_mode` (**ceiling-pegged by default, like the device**).
* **The timing structure.** Two rates, like the firmware's two tasks: the hub /
  transport pump at 5 ms (`SlopSyncHubService::taskLoop`) and motion substeps at
  1 ms (`streamSamplerTask`'s `vTaskDelayUntil`). The pacing ring is drained
  *inside* the substep loop, so a commit is the current plan for exactly one
  1 ms sample — the device's granularity, not the host timer's.
* **Window legality.** `RangeMapper::setRange` (swap, clamp to `[0, rail]`,
  5 mm minimum span) behind `WebUI::applySettings`' refusal of `min >= max`,
  on both the 0x0101 intent path and the palette. Including the firmware's own
  quirk that the 5 mm expansion is not re-clamped to the rail (`600..700` on a
  500 mm rail echoes `500..505`, on device and sim alike).
* **E-stop semantics.** Latch → motion stops → the clear is REFUSED
  (`CLEAR_REFUSED`) until the motion tick that kills the pulse train has run,
  mirroring `estop_requested`'s Core-1 consumption. Clearing never re-homes.
* **Stop is not a latch.** `hardStopMotion()` is `MotorDriver::hardStop()` and
  nothing else — no plan reset, no pattern stop — so motion resumes from the
  live plan on the next sample, exactly like the machine. Paths that on hardware
  leave the sampler *unfed* (deadman, unhome, e-stop) drop the plan instead,
  which is the sim's stand-in for `streamActive` going false.

**Not modeled (do not conclude anything about these from a sim run):**

* **Step quantization, RMT jitter, driver electrical behavior, motor current,
  encoder feedback, thermal or mechanical compliance.** `SimStepper` is an ideal
  trapezoidal follower in the mm domain (real quantization is 0.0491 mm at
  `AIM_STEPS_PER_MM` = 20.372 — below anything the UI shows).
* **The sampler's stream arbitration**: `STREAM_IDLE_TIMEOUT_MS`, `interpBusy` /
  post-move hold, pattern reclaim. The sim's sampler always renders the live
  plan while the gates are open.
* **Homing** is a 3 s timer plus a gentle glide to 0 that sets the measured
  stroke — not a sensorless-cycle model (no current sensing, no crash detect).
* **Patterns** are 3 stand-in generators, NOT `PatternEngine` (FreeRTOS-tainted).
* **Limit ceilings diverge on purpose** — the sim clamps at EXPERT values; see
  the section above. Sim accepting a limit is not proof the device will.
* **Persistence, OTA, WiFi, mDNS beyond the advert, the legacy UiSocket plane,
  TCode transports, OSSM BLE.** None exist here.
* **Host-stall honesty**: if the OS starves the sim thread past 100 ms the
  catch-up burst is capped and that motion time is genuinely lost — counted in
  `simstats.substeps_discarded` and logged. Zero is the only value that means a
  clean run.

## Known gaps / residuals (rough-in honesty ledger)

* **IXWebSocket server handshake lacked the Sec-WebSocket-Protocol echo** —
  strict clients (python websocket-client, browsers) hard-fail without it.
  Patched via `cmake/PatchIXWebSocket.cmake` (idempotent, applied on fetch).
  Candidate upstream PR; alternative is the single-threaded hand-rolled server
  (which would also delete the marshaling layer).
* Chase-stream "drops" were a MEASUREMENT bug, now split: `sync.dropped` counts
  only what the device counts (gate drops + ingress ring overwrites), and
  planner refusals live in `simstats.plan_rejected`. The historical ~36/204
  figure was inflated by the host-timer staircase (below) — a 50 Hz sine now
  runs 500/500 with `plan_rejected` 0.
* `cfg_gen` still cannot be bumped machine-side (spec-gap ledger below).
* TUI verified to build/run; visual pass in Windows Terminal pending operator.
* Client mode: stub only (`slopsim client` prints a pointer here).

* mDNS is Windows-only (DnsServiceRegister); Linux needs an avahi backend.
* The host-IP pick uses the UDP-connect routing trick — gethostname enumeration
  returned APIPA 169.254.x.x from a dead adapter on the dev machine (field-hit).

## Spec-gap ledger (for the RFC batch — registry.yaml first, never code-local)

* **Machine-side config edits can't bump protocol cfg_gen** — `slopsync::Hub`
  has no bump API; cfg_gen only moves via intents. The sim's palette overrides
  republish 0x0081 (clients adopt values) but a client using cfg_gen
  preconditions won't see the generation move. The firmware has the same
  asymmetry (its `_state.cfg_gen` is a separate counter). RFC candidate:
  `Hub::bumpCfgGen()` for hub-side config mutation.

* **Device log channel**: logs ride HTTP `/api/log`; under the 100%-SlopSync
  doctrine the device needs a log EVENT channel (bounded, drop-oldest). New
  channel id + registry entry required.
* **Sim/capabilities discovery over SlopSync**: `fw_version`/feature flags still
  come from HTTP `/api/capabilities`; a hub-identity STATE channel (or WELCOME
  extension) would finish the HTTP demolition for apps without a page.
* Existing queue (docs/slopsync/RFC-QUEUE.md) RFC-001 (NACK intent correlation)
  and RFC-003 (stored-vs-effective flags) will matter for the client cockpit's
  shadow lifecycle — same batch.
