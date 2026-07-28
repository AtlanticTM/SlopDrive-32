---
title: CLI guide
description: >-
  SlopScope, the motion-pipeline oscilloscope, and slopsync_probe.py, the reference verifier: what each one proves, how to run it, and how to read what it prints.
register: STE
---

# CLI guide

Two command-line tools ship with SlopSync. They answer different questions.

| Tool | Answers |
|---|---|
| [SlopScope](#slopscope) | Did the machine do what the app asked, and if not, where did the difference come from? |
| [The probe](#the-probe) | Does this hub answer a full scripted session correctly? |
| [The generators](#the-generators) | Are the published numbers still the registry's numbers? |

Both tools are Python. Both need the `websocket-client` package. Both run
against [the simulator](local-testing.md#the-simulator) exactly as they run
against hardware.

## SlopScope

`tools/slopscope.py` is an oscilloscope for the motion pipeline.

### The question it answers

A machine feels wrong. The app is a good app, the script is a good script, and
the motion is still not what the author intended. Nothing in a log answers
this, because every layer reports itself as healthy.

SlopScope graphs the raw commanded input as it arrives over SlopSync, scaled
into the [stroke window](../reference/dictionary.md#stroke-window), against
what the planner actually did with it. Quintic shaping, chase lag, guard
fallbacks and handoff bounding stop being feelings and become lines.

### Three lines

Every graph carries the same three position series, all in millimetres.

| Series | What it is |
|---|---|
| [Asked](../reference/dictionary.md#asked) | The demand as it landed, before planning |
| [Planned](../reference/dictionary.md#planned) | Where the motion core is driving to right now |
| [Achieved](../reference/dictionary.md#achieved) | Where the carriage actually is |

Read the two gaps separately. They mean different things.

**Planned minus achieved is tracking.** The plan is the machine's own promise.
A gap here is the actuator falling behind its own plan.

**Asked minus planned is shaping.** The planner does not chase a demand
instantly, and it should not. In [waveform](../reference/dictionary.md#waveform)
mode this gap is simply the travel still to come inside the commanded
duration, so a large number is normal. In
[chase](../reference/dictionary.md#chase) mode the same gap is lag, and it is
worth reading.

A third layer sits underneath: the plan envelope, drawn from the planner's own
current segment, plus the velocity plane and the engine's
[anomaly](../reference/dictionary.md#motion-anomaly) events marked on the time
axis.

### It cannot move the machine

SlopScope connects at the [watch](../reference/dictionary.md#watch) tier. It
subscribes and nothing else. It sends no
[intent](../reference/dictionary.md#intent), publishes no
[stream](../reference/dictionary.md#stream), and carries no `publishes` wish in
its HELLO. There is no intent builder in the file at all.

This is structural, not a flag you can forget. Drive the machine with something
else and watch it here.

### record

`record` captures a trace, and can render it in the same run.

```bash
python tools/slopscope.py record --ip 127.0.0.1 --port 82 \
    --seconds 26 --out run.jsonl --render run.html
```

| Flag | What it does |
|---|---|
| `--ip`, `--port` | Where the hub is. Port 82 is the SlopSync plane |
| `--http-port` | Where to read `/api/capabilities` for the firmware version. Optional |
| `--seconds` | Capture length. `0` runs until ctrl-c |
| `--out` | Trace file. Defaults to `slopscope-<stamp>.jsonl` |
| `--render` | Also write the HTML graph here |
| `--theme`, `--palette` | Passed to the renderer. See [color](#color) |
| `--window MIN:MAX` | Escape hatch for a hub whose catalog declares no window role. Recorded as an operator override, never as machine truth |

It prints how it found every channel, which is worth reading once:

```text
[slopscope] connected ws://127.0.0.1:82/ (subprotocol slopsync.v1)
[slopscope] WELCOME session=1200999484 roles=1 (watch tier is all this tool needs)
[slopscope] catalog 10617 B, 26 entries, etag VERIFIED
  resolve window      field_role window.min/window.max -> channel 0x0081 'machine-config' fields 'window_min'/'window_max'
  resolve achieved    field_role telemetry.position -> 0x0080 'motion' field 'pos_10um'
  resolve velocity    field_role telemetry.velocity -> same channel, field 'speed'
  resolve planned     field name match on 0x0080 layout -> 'tgt_10um'
  resolve asked       field name match on 0x0080 layout -> 'raw_10um'
[slopscope] grants: safety=on-change, motion=60.0Hz, machine-config=on-change,
            plan-strip=45.0Hz, slopmotion-diag=1.0Hz, motion-anomaly=on-change
[slopscope] captured 26.0s: 1305 motion, 724 plan, 27 diag, 3 anomaly -> run.jsonl
```

Nothing there is a hardcoded channel number. Positions and the window come from
[field roles](../reference/registry/catalog-vocabulary.md#field-roles); the rest
comes from the catalog's own names and layouts. A hub that numbers its channels
differently still graphs.

### live

`live` prints a terminal dashboard while it captures. Add `--out` to record at
the same time.

```bash
python tools/slopscope.py live --ip 127.0.0.1 --port 82 --seconds 18
```

```text
SlopScope live  ·  127.0.0.1:82  ·  fw slopsim-0.2.0  ·  watch tier, subscribe-only
------------------------------------------------------------------------------
window   0.0 .. 500.0 mm  (span 500.0)   limits input.accel=8000 input.jerk=2e+06
         input.speed=550 user.accel=200 user.speed=50

rates    motion  52.7Hz  plan   0.0Hz  diag   1.0Hz  anom   0.0Hz

position asked   150.00   planned   150.00   achieved   150.00   vel     0.00

divergence  asked-vs-achieved   mean  32.32 mm  p95 194.02  max 200.00
            asked-vs-planned    mean  31.00 mm  p95 193.40  max 200.00

planner  plans 0        failures 0      anomalies 3       plan_us last/max/avg 0/0/0.00
anomaly  settle=1  endvel_clamped=1  waveform_fallback=1
ingress  bundles=12  samples=12  enqueued=12  dropped=0  seg_bundles=12

recent anomaly EVENTs (3 total)
   t=   2.02s  endvel_clamped     target=0.70     detail=1.10
   t=   2.02s  waveform_fallback  target=0.70     detail=1.17
   t=   2.92s  settle             target=0.70     detail=1.10
```

Use `live` while you change something on the machine. Use `record` when you
want to compare two runs, or keep the evidence.

### render

`render` turns a trace into one self-contained HTML file.

```bash
python tools/slopscope.py render run.jsonl --out run.html --theme light --palette cvd
```

The page inlines its own data, its own SVG and its own script. It fetches
nothing. It opens on a bench laptop with no internet, months later, and it
zooms. `render` needs no network and no `websocket-client`: it is a pure
function of the trace file, so anyone you send a trace to can render it.

The page carries a toolbar: each series on or off, the plan envelope, the
anomaly marks, palette, theme and reset-zoom. Under the graph are three
collapsed tables — trace metadata, the anomaly log, and a series summary. Every
value on the graph is reachable in those tables without seeing a single color.

### The trace format

A trace is JSONL. One header record, then one record per frame, then a footer
that repeats the header with the final window, limits and record counts.

The header is the reason the format is worth explaining. It carries:

- the hub identity: session id, boot id, `cfg_gen`, `deadman_ms`;
- the catalog [etag](../reference/dictionary.md#etag), the catalog size, and
  whether the etag was verified against the bytes actually received;
- the firmware version and whether the target was simulated;
- the [stroke window](../reference/dictionary.md#stroke-window) and the whole
  [limit set](../reference/dictionary.md#limit-set);
- the CLOCK offset and round-trip time;
- the full layout of every recorded channel — every field's name, type, unit,
  scale, role, description, bit names and option labels;
- a `resolution` block saying **how** each thing was found.

```json
{"rec":"header","tool":"slopscope","trace_format":1,
 "hub":{"catalog_etag":"0458eec408a43692","catalog_etag_verified":true,"deadman_ms":600},
 "window":{"min_mm":0.0,"max_mm":500.0,"span_mm":500.0},
 "limits":{"limit.input.speed":550.0,"limit.input.accel":8000.0,"limit.user.speed":50.0},
 "series":{"planned":{"channel":128,"field":"tgt_10um","unit":"mm"}}}
```

Two properties follow from that, and both matter.

**A trace stays interpretable.** Open one in a year, against a firmware that
has since changed its layout, and every number still has a name, a unit and a
scale attached to it. You never need the tool version that recorded it, and you
never need to guess which field was which.

**`render` is offline and total.** The graph is a pure function of the trace.
Re-rendering an old trace with a newer SlopScope gives the same picture, so a
trace attached to a bug report is evidence rather than an anecdote.

Frame records are short on purpose: `r` names the kind (`m` motion, `p` plan
strip, `c` machine config, `d` diagnostics, `a` anomaly, `s` safety), `t` is
seconds since capture start, `th` is the hub-clock estimate, and `v` is the
decoded sample with catalog field names as keys.

### A worked example: segments against chase {#worked-example}

Here is one capture read end to end. Both runs are against the simulator, on
one unrestarted instance, with the probe as the driver.

```bash
# terminal 1 — the machine
slopsim machine --homed --headless --duration 150

# terminal 2 — the scope
python tools/slopscope.py record --ip 127.0.0.1 --port 82 --seconds 26 --out seg.jsonl

# terminal 3 — the driver: timed segments, one per second
python tools/slopsync_probe.py --ip 127.0.0.1 --port 82 --segments 20
```

Then the same again with `--stream 20`, which sends bare points at 50 Hz
instead.

The machine has a 500 mm stroke window and a 550 mm/s input speed ceiling. Each
capture holds about 1200 motion samples in the mode of interest.

| Measured over the capture | Segments (waveform) | Points (chase) |
|---|---|---|
| Mean \|planned − achieved\| | **7.2 mm** | **31.1 mm** |
| Median \|planned − achieved\| | 1.3 mm | 37.9 mm |
| 95th percentile | 10.0 mm | 48.9 mm |
| Mean speed | 173 mm/s | 399 mm/s |
| Samples at the speed ceiling | 0% | **61%** |
| Anomalies counted | 3 | 669 |
| Samples dropped on ingress | 0 | 0 |

Now read it.

**The segment run tracks its plan.** Half the samples are inside 1.3 mm on a
500 mm stroke. Each funscript action arrives as one timed segment, becomes one
[quintic](../reference/dictionary.md#quintic) over exactly the commanded
duration, and the machine follows it. Peak speed stayed at 517 mm/s, under the
550 ceiling, so the demand fitted inside the machine's envelope. The three
anomalies are the first handoff at the start of the run.

**The chase run does not, and the trace says why.** The driver streams
`0.5 + 0.35·sin(2π·0.8·t)`, which on a 500 mm window peaks at **880 mm/s** of
source velocity. The machine's input ceiling is 550 mm/s. The demand is
physically impossible on this machine, so speed pins at the ceiling for 61% of
the capture and the position falls behind. 566 of the anomalies are
`endvel_clamped`: the engine refusing an end velocity that the window could not
absorb.

That is the whole skill. **The gap alone means nothing; the gap beside the
speed line means everything.** A gap with speed headroom left is a tracking or
tuning question. A gap with speed pinned at the ceiling is a demand the machine
was never able to serve, and the fix belongs in the app, in the script, or in
the limits — not in the planner.

### Color {#color}

The colors are the product's own, so a graph reads like the WebUI and like
these docs.

| Series | Meaning |
|---|---|
| Purple | Intent: commanded, not yet confirmed. **Asked** |
| Deep blue | Reality's side, accepted: the hub's own target. **Planned** |
| Blue | Reality: measured. **Achieved** |
| Amber, red | Safety only. Never a data series |

Planned is drawn as a deeper step of the reality blue rather than a fourth hue.
The planner's target has already passed arbitration, clamping and the window,
so it belongs to the reality family; the lightness step says "accepted, not yet
executed".

!!! warning "Two of these lines are not distinguishable by color"

    Measured, not assumed: the reality and intent pair is **ΔE 1.1** under
    deuteranopia, against 11.4 under normal vision.

    So color is never the only encoding. Every series also carries its own
    dash pattern, a legend key drawn in that pattern, a direct end-label on the
    line, and a named readout under the crosshair. The series-summary table
    gives every value in text.

    If you cannot separate the lines, press **palette** in the toolbar. The
    `cvd` palette re-steps the same semantic families to a set that passes.
    `--palette cvd` makes it the default for a rendered file.

### Honest limits

**The simulator's actuator is an ideal follower.** It runs the real motion
engine, the real hub and the real catalog, so *asked* and *planned* are exactly
what the device would produce. *Achieved* is optimistic: there is no step
quantisation, no current limit, no encoder lag and no mechanical compliance.
Trust the simulator for protocol, planning and shaping questions. Confirm
tracking numbers on hardware.

**`plan_us_*` is always zero on a host build.** Plan time is measured where it
runs, on the device. A host number would be a different CPU answering a
question nobody asked.

**A trace is one workload.** Every number in the worked example above is a
property of that machine, that limit set and that driver. Capture your own.

## The probe

`tools/slopsync_probe.py` is the reference verifier. It runs a scripted session
against a live hub and prints a narrated pass-or-fail transcript.

It hand-rolls its own encoder against the registry instead of importing the
library, so a hub that passes the probe has agreed with an independent
implementation. It is also a complete, readable v1.0 client, which is why the
[Quickstart](quickstart.md) is built from it.

### What it verifies

The session walks the whole protocol in order: connect and subprotocol, HELLO
and WELCOME with every required key, the
[ready gate](../reference/dictionary.md#ready-gate), catalog transfer over
BLOB and its etag, subscriptions and grants, retained STATE, each telemetry
channel's layout, an intent and its post-clamp ECHO, the CLOCK exchange, stream
and segment ingress with counters, safety ops, the anomaly feed, and GOODBYE.

```bash
python tools/slopsync_probe.py --ip 127.0.0.1 --port 82 --segments 20
```

```text
  ✓  intent                 PASS
  -  stream                 SKIP
  ✓  segment_clock          PASS
  ✓  segment_send           PASS
  ✓  segment_counters       PASS
  ✓  motion_anomaly         PASS
  ✓  goodbye                PASS

  46 passed, 0 failed, 3 skipped
```

A failure names its stage. The stage names are the protocol's own steps, so the
failing stage tells you which clause to read.

| Flag | What it does |
|---|---|
| `--stream <seconds>` | Stream bare samples on the motion-input channel at 50 Hz |
| `--segments <seconds>` | Stream timed segments instead: one bundle per second |
| `--pair` | Run the pairing and trust scenario instead of the motion session |
| `--estop` | Assert a client-side emergency stop, then clear it |
| `--bench-home` | Exercise the bench homing operations |
| `--no-motion`, `--listen-only` | Skip every step that commands motion |

Two of these change the machine's state and say so. `--estop` really latches
the machine and leaves it unhomed. `--bench-home` asserts a stroke window
nothing measured, and is for motorless rigs. Neither is a default.

Motion steps are dropped by the hub's homed gate on an unhomed machine. That is
correct behavior, and the wire path is still fully exercised, so an unhomed
run is the safe way to prove a hub.

### Run it twice

!!! danger "Two runs, back to back, with nothing restarted between them"

    Run the probe. Then run it **again**, against the same hub, without
    rebooting, reflashing or restarting anything.

    A departed session's [source ownership](../reference/dictionary.md#source-ownership)
    once leaked forever, silently refusing every later client as a conflict.
    The bug hid for months, because every deployment rebooted the device
    between test runs and the reboot cleared the leak.

    One run cannot find that class of bug. Two can.

The full reasoning, and the other three patterns worth keeping, are on
[Local testing](local-testing.md#the-pattern-that-is-mandatory).

## The generators

Every number this site publishes is generated from the registry or the
specification. Both generators take `--check`, which exits non-zero when a
generated file no longer matches its source.

```bash
python tools/gen_registry_header.py --check          # the C++ constants
cd docs-site
python tools/gen_docs_tables.py --check              # the registry tables + Dictionary
python tools/gen_spec_pages.py --check               # the Specification tier
```

Run the plain form to regenerate, then commit the source and the output
together. Never hand-edit a generated file: the banner at the top of each one
says so, and `--check` enforces it.

## Where to go next

- [Local testing](local-testing.md) — the simulator, the fuzz harnesses, and
  the regression patterns.
- [Quickstart](quickstart.md) — the shortest correct session, built from the
  probe.
- [The Dictionary](../reference/dictionary.md) — every term used here.
