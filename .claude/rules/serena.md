---
paths:
  - "**"
---

# Serena / clangd navigation upkeep

Regenerate the compile database whenever platformio.ini changes (envs, build
flags, lib_deps):

```
python tools/gen_clangd_db.py          # defaults to env sd32-ota
```

## Do NOT use `pio run -t compiledb`

For this ESP-IDF-framework project that target emits ONLY framework and
managed-component translation units. It holds zero first-party entries in
every env, because `src/` is compiled by PlatformIO's SCons layer, which the
target does not walk. Verified 2026-08-03 by forcing a clean regen against
sd32-ota and grepping the result for project paths: zero hits. It also
reports "up to date" and does nothing unless the old file is deleted first.

`gen_clangd_db.py` reads `pio run -t idedata` instead, which reports the
exact driver, flags, defines and include roots the real build used, and
writes one entry per first-party TU. Headers are not TUs; clangd infers their
command from the nearest entry, so `src/` coverage is what fixes `include/`.

Known tradeoff: the generated db covers first-party sources only. Reading
ESP-IDF component sources falls back to the base `.clangd` flags. Merge the
compiledb output in if IDF browsing ever matters more than load time.

## Verify against real symbols, never by assuming

Two checks, both required. They catch different failures.

1. Parse health: `get_symbols_overview` on `include/comms/SlopSyncCatalog.h`
   must report kind `Namespace` for `slopdrive`. Kind `Variable` means the
   C-parse regression is back. Name that file specifically, not "a header
   under include/comms/": several of them predate the namespace and correctly
   report only classes, which reads as a failure when it is not.

2. Index health: `find_referencing_symbols` on `ServoModbus/emergencyStop`
   must include `ModbusServoDriver.cpp`. Intra-file hits with nothing
   cross-file means the background index is off or stale, and check 1 passes
   anyway, so the namespace check alone will not catch it.

An empty reference result never proves a symbol is unused. See
`navigation.md`: a C-9 deletion needs both Serena and Grep to agree.

## Two traps that make correct output read as wrong

**Line numbers are 0-BASED.** Every `body_location` and reference line is one
less than the editor, Grep, and `file:line` links. Add 1 before citing.
Measured 2026-08-04: Serena reported the `_bus.emergencyStop()` call site at
147, the file has it at 148.

**A cold index answers `{}`.** Serena spawns clangd at session start and the
background index needs roughly a minute on this project before cross-file
references resolve; the same query returned `{}` at 23 seconds and the correct
single hit a few minutes later. Never read an early-session empty as "no
callers": re-run it, and if it stays empty, check `Index.Background` per
`navigation.md`.
