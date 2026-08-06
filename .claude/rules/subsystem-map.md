---
paths:
  - "**"
---

# Where things already are

Orientation, so no agent spends calls rediscovering the shape of the tree.
`repo-layout.md` is the normative twin: where a NEW file goes. This one is
descriptive: where the existing things live. Structural on purpose, so it rots
slowly. For anything finer, ask codebase-memory (`get_architecture`,
`search_graph`) rather than grepping for `^class` (see `navigation.md`).

## The tree

| Path | What lives there |
|---|---|
| `src/motion` + `include/motion` | Drivers (Modbus, AIM), arbiter, executor, pattern engine, proxy |
| `src/comms` + `include/comms` | SlopSync hub service, catalog, transports (UART/WS/BLE), ServoModbus |
| `src/system` + `include/system` | Config store, OTA, logging, app state, encoder validator |
| `src/ui` + `include/ui` | `WebUI.cpp`, the HTTP/API surface |
| `lib/sloplog`, `lib/slopglow`, `lib/slopmotion` | FIRST-PARTY libraries |
| `lib/ruckig`, `lib/asynctcp`, `lib/espasyncwebserver`, `lib/lcd_st7735`, `lib/StrokeEnginePatterns` | VENDORED. Do not restyle or respell (C-11 carve-out) |
| `lib/slopsync` | Symlink to the sibling SlopSync repo, pinned by `slopsync.pin`. READ-ONLY from here |
| `webui/` | Svelte frontend, built into the LittleFS image by `build_webui.py` |
| `sim/slopsim/` | Host simulator. Separate CMake build, appears NOWHERE in `platformio.ini` |
| `src/quad_probe`, `src/c5_probe`, `src/c5_tdongle`, `src/c5_waveshare` | Standalone bench sketches, each excluded from the firmware envs by `build_src_filter` and built only by its own env |
| `test/native/` | Host doctest suites (`pio test -e native`) |
| `tools/` | Instruments. Mostly gitignored; the tracked ones are named in `.gitignore` |

## Tasks and cores (`src/main.cpp`)

Core 1 is the realtime side, core 0 is network. Getting this wrong is how a
blocking call in the wrong place freezes the UI or drops motion.

| Task | Core | Priority | Stack | Role |
|---|---|---|---|---|
| `ServoBus` | 1 | 5 | 4096 | Modbus servo bus, highest priority, created conditionally |
| `Sampler` | 1 | 4 | 16384 | Stream sampling. Largest stack; its deep path is motion, so an idle-boot high-water reading means nothing |
| `Motor` | 1 | 3 | 4096 | Motion stepping |
| `Comms` | 0 | 2 | 4096 | SlopSync hub update |
| `HTTP` | 0 | 1 | 8192 | Web server and API. Lowest priority, so it starves first under load |

A blocked `HTTP` task is the classic symptom: the LEDs freeze because the
status path runs through it. See `logging-leds.md` and `memory-budget.md`.

## One fact, one home (C-1)

Stop grepping for these. They live in exactly one place.

| Fact | Home |
|---|---|
| Firmware version | `FIRMWARE_VERSION` in `include/system/config_api.h` |
| Wire numbers, CBOR keys, NACK codes, channels | sibling `SlopSync/spec/registry/registry.yaml` |
| Protocol behavior | sibling `SlopSync/spec/SPEC.md` |
| This machine's channel allocation | `docs/slopsync/CHANNEL-MAP.md` |
| Build envs, flags, `custom_sdkconfig` | `platformio.ini` |
| Which `tools/` scripts survive a clone | `.gitignore` negations |
| Versions, deployment state, milestones, open bugs, rulings | the dev board (`bd`) |

## `docs/canon/` is GONE

`LEDGER.md`, `DOCTRINE.md`, `TRAPS.md` and `CANON.md` no longer exist. Do not
go looking; the directory itself was removed 2026-08-04.

- Volatile state (versions, deployment, milestones, open bugs) -> the dev
  board. `bd ready`, `bd list --label area:motion`.
- Engineering doctrine -> this `.claude/rules/` directory.
- Trap lessons -> still `T<n>`, now headings inside the rules files. All 30
  numbers have homes; `grep -rn "## T12" .claude/rules/` finds any of them.

## First moves that are never wasted

1. `bd ready` and `bd list --label area:<subsystem>` for current state. In a
   SUBAGENT run `bd prime` first; SessionStart hooks do not fire for you.
2. `search_graph(query="...")` in codebase-memory for "where is the thing that
   does X", especially when you do not know the symbol name.
3. `get_symbols_overview` (serena) for a file's shape. Never grep `^class`.
4. `python tools/canon_lint.py` before declaring work done. **Baseline is ZERO
   findings** (2026-08-06: the standing `slopsync.pin` mismatch was resolved by
   bumping to the sibling's HEAD, a docs-only commit touching no normative
   surface). A finding is a defect, never furniture -- fix it or nothing else
   proceeds.
