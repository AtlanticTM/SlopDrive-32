# SlopBench — the "be anything" SlopSync test hub

A dumb, config-file-driven conformant SlopSync hub. Point it at a `.bench`
file and it serves exactly the catalog that file describes, with a generic
INTENT-clamp/echo/STATE-mirror write plane and a configurable fake delay
between accepting a write and reflecting it in STATE.

## Division of labor (docs/canon/LEDGER.md, "Morning ruling batch" item 3)

- **`sim/slopsim`** is the 1:1 device twin: it embeds the real device catalog
  (`--profile device`) and reproduces SlopDrive-32's actual behavior,
  including the real `slopmotion::Engine`. Parity is one-way — the firmware
  is never edited to close a sim gap.
- **SlopBench** is the opposite role: no fixed catalog, no motion engine, no
  device semantics. It builds whatever catalog a config file describes and
  applies one generic write-plane rule to all of it. Use SlopBench to test a
  client/widget against catalog shapes SlopDrive-32 will never produce
  (extra archetypes, alien channel domains, deliberately slow echoes) without
  waiting on a firmware change.

Both tools speak the same `slopsync.v1` WebSocket protocol and can be probed
with the same `tools/slopsync_probe.py`.

## What it does

1. **Config-file catalog.** A `.bench` file declares the hub's name/identity
   and every channel: id, name, class, category, and fields (name, type,
   unit, min/max, and for INTENT/EVENT the wire key). SlopBench builds and
   serves exactly that catalog, plus the spec-core channels (0x0003-0x000E)
   every conformant hub carries.
2. **Write plane.** An INTENT channel that declares `writes <state-id>`
   mirrors its fields into that STATE channel BY NAME: an intent field and a
   state field with the same name are linked automatically. Writes are
   clamped to the field's configured min/max, echoed (the post-clamp applied
   value, per SPEC §9.3 — always immediate, never delayed), and reflected
   into the mirror STATE channel. None of this is hardcoded per channel; it
   is driven entirely by the parsed config.
3. **Fake STATE-echo delay.** A channel's mirrored fields land in STATE after
   `echo_delay_ms` (per-intent-channel, falling back to the hub-wide
   default) — 0 means instant. This exists to catch clients that assume a
   write is visible in STATE the instant the ECHO arrives; SlopSync's ECHO
   is a receipt, not a promise about when STATE catches up.
4. **Auto-animation.** A STATE field can `animate` on a sine or a triangle
   ramp, so a client sees live telemetry with nobody writing to it.
5. **TUI.** A plain ANSI redraw (no curses dependency): a live channel/field
   table, connected sessions, a scrolling recent-writes log, and the tail of
   the hub's own log. `q` quits — that is the whole keymap.

Cut for leanness (judgment calls, not gaps found by accident):
- STREAM/EVENT/STORE channels can be *declared* (to exercise the catalog
  encoder against every archetype) but are not functionally wired: a
  declared STREAM never ingests, a declared STORE always answers BLOB_REQ
  with `CHUNK_UNAVAILABLE` (legal, honest behavior for an unserved
  namespace — see `HubDelegate::readBlob()`'s default), and a declared EVENT
  never fires on its own.
- Every session is granted `configure` regardless of token
  (`BenchHub::validateToken`). SlopBench is a write-plane/catalog test
  double, not an auth conformance harness — that is `slopsim`'s (and the
  real firmware's) job.
- No per-field bit labels, no RFC-009 `group`/`desc`/`role`/`step`
  annotations beyond `default`, no priority knob (every device-range channel
  authors as `normal`). The config format can grow these later without
  breaking existing files (every new keyword is additive).

## Config format reference

Deliberately **not YAML**: a hand-rolled ~250-line recursive-descent-free
parser (`src/config/ConfigParser.cpp`) covers everything below with no new
external dependency for a dev-only tool, and no indentation sensitivity to
get subtly wrong. If the format ever needs real YAML's expressiveness
(anchors, multi-line strings, etc.), that is a deliberate follow-up, not
something to grow ad hoc here.

Line-oriented; `#` starts a comment; blank lines are ignored. A **block
keyword** (`hub`, `state`, `intent`, `event`, `stream`, `store`) starts a new
block; every following line belongs to it until the next block keyword.
Indentation is cosmetic only.

```
hub
  name <string>                # hub identity (WELCOME's `identity` map)
  product <string>              # default: SlopBench
  fw_version <string>           # default: 0.1.0
  echo_delay_ms <uint>          # hub-wide default fake STATE-echo delay

state <id> <name>               # id: 0x1000 or decimal; must be >= 0x0080
  category <name>                # a registry ui_categories name, or any
                                  # other word (assigned a vendor id + label)
  rate_hz <float>                 # maxRateHz; 0 = on-change only
  access <watch|control|configure>
  field <name> <type> <unit> [min max] [default=V]
  animate <field> sine <hz> <amplitude> <center>
  animate <field> ramp <hz> <min> <max>

intent <id> <name>
  writes <state-id>              # the STATE channel this one mirrors into
  echo_delay_ms <uint>            # overrides the hub-wide default
  rate_hz <float>
  access <watch|control|configure>
  field <name> <type> <unit> [min max] key=<N> [default=V]

event <id> <name>                # declared only -- see "cut for leanness"
  field <name> <type> <unit> key=<N>

stream <id> <name>               # declared only
  rate_hz <float>

store <id> <name>                # declared only
  kind <string>
  capacity <uint>
  per_item_max <uint>
  name_max <uint>
```

STATE/STREAM field **types**: `u8 i8 u16 i16 u32 i32 f32 bitfield8 str16
str32 str64` (`slopsync::PackedFieldType`). INTENT/EVENT field **types**:
`uint int f32 bool tstr bstr` (`slopsync::CborFieldType`) — these REQUIRE
`key=N` (the CBOR sub-map key), unique and strictly ascending per channel.
`unit` is a bare token; use `-` for none. `min`/`max` are two bare numbers
right after `unit`, both or neither.

A STATE field's name matching an INTENT field's name (on a channel that
declares `writes <this-state-id>`) is the whole mirror link — no other
wiring is needed. That link is also recorded in the served catalog itself
(the STATE field's `setting_key`, the entry's `setting_channel` — RFC-009),
so a generic client can discover it without reading this file.

## TUI keys

`q` (or `Q`) quits. Nothing else. The screen redraws every tick (~20 Hz)
showing: hub identity + catalog size + session count, every channel's live
field values, connected sessions (peer address), a 12-line recent-writes
log, and the tail of the hub's own diagnostic log.

## Example invocations

```
slopbench sim/slopbench/configs/tiny-axis.bench
slopbench sim/slopbench/configs/alien.bench --port 7500
slopbench sim/slopbench/configs/kitchen-sink.bench --headless --duration 30
```

Flags: `--port <n>` (default 7000), `--headless` (no TUI, log lines echo to
stdout instead — for scripted/CI runs), `--duration <seconds>` (auto-exit,
headless or not).

## Example configs

- **`configs/tiny-axis.bench`** — the potato-client floor: one STATE, one
  mirrored field, one animated field, zero echo delay.
- **`configs/alien.bench`** — every device-range channel sits in a reserved
  *domain* nibble (`docs/slopsync/CHANNEL-MAP.md`'s 0xCDSS grid: 3=auxiliary,
  4=playback, 5=automation, 8-F=parked for multi-axis) that SlopDrive-32
  itself never allocates, one of each class (STATE/INTENT/EVENT/STORE/
  STREAM), and an explicit non-zero echo delay.
- **`configs/kitchen-sink.bench`** — every field type, both animation kinds,
  a hub-wide default echo delay AND a per-channel override, a vendor-defined
  category, and one declared-only channel of STREAM/EVENT/STORE each.

## Build (Windows, WinLibs GCC + CMake + Ninja)

Same toolchain as `sim/slopsim`:

```bash
export PATH="/c/Users/Atlan/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin:$PATH"
cmake -S sim/slopbench -B sim/slopbench/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc
cmake --build sim/slopbench/build
```

The only external dependency is IXWebSocket (FetchContent-pinned, same
version as `sim/slopsim`); the exe is `-static` linked.

## Verify

```
python sim/slopbench/tools/smoke_test.py
```

Drives all three example configs end to end against the built binary:
HELLO/WELCOME, CATALOG_READY, a full BLOB_REQ catalog fetch (checked against
the channels the config declares), one clamped INTENT write, and a
measurement of the mirrored STATE field's echo delay. It imports
`tools/slopsync_probe.py` as a module for the wire layer rather than
reimplementing it.
