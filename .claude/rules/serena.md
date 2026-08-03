---
paths:
  - "platformio.ini"
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

After any change here, verify against a real project symbol rather than
assuming: `get_symbols_overview` on a header under `include/comms/` must
report kind `Namespace` for `slopdrive`. Kind `Variable` means the C-parse
regression is back.
