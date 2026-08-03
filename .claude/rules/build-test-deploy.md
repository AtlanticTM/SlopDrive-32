---
paths:
  - "platformio.ini"
  - "test/**"
  - "tools/**"
  - "src/**"
  - "include/**"
---

# Building, testing, and deployment

Operational commands live in the `build-and-flash` skill. This file is the
LAW: scope, gates, and the traps that make a green result a lie.

## Building and testing

- pio: `%USERPROFILE%\.platformio\penv\Scripts\platformio.exe`, host
  Windows 11.
- Native tests: `pio test -e native` with WinLibs MinGW-w64 on PATH
  (`.../BrechtSanders.WinLibs.POSIX.UCRT_.../mingw64/bin`).
- **Cross-compile proof each milestone:** xtensa `-fsyntax-only` on library
  translation units AND `pio run -e sd32-ota` green. Both, not either.
- `python tools/canon_lint.py` gates every substantive change; zero findings
  is the bar.

## Deployment (OTA)

- **Scope:** OTA is the S3 main controller ONLY (`sd32`/`sd32-ota` extend
  `s3_main`). The C5 nodes have no web server or UI image and flash over
  USB-JTAG serial only.
- **OTA is the default path.** No USB hunting, no serial fallback unless OTA
  is confirmed unavailable.
- **Firmware and web UI are separate images.** `-t upload` ships
  `firmware.bin` only; `-t uploadfs` ships the web bundle and does not reboot.
  Both changed means BOTH commands, `upload` then `uploadfs`. **Never report a
  combined change deployed after running one.**
- Secret: `SECRET_OTA_PASSWORD` in git-ignored `include/secrets.h` (template
  `include/secrets.example.h`). Never inline it anywhere durable.
- **If unreachable:** do NOT guess IPs, port-scan, or seize serial. Stop and
  ask the operator for the device's IP and machine state.
- **Before flashing:** machine idle, operator aware. **After flashing:**
  verify `fw_version` via `/api/capabilities` against `FIRMWARE_VERSION` in
  `config_api.h`. Upload completed is not deployed (C-8). After `uploadfs`:
  hard refresh, verify the change visibly.
- **Serial and USB are rescue-only** (bootloop, WiFi-breaking change,
  partition work). That is a bench act the operator performs.

## T10 -- PIO's native test runner misreports doctest

**Rule:** trust the process exit code, or run `.pio/build/native/program.exe`
directly. Never trust the runner's parsed summary. "0 test cases" and a
phantom CTRL_BREAK on failures are cosmetic lies.

## T12 -- registry and spec drift is caught by tools, not eyes

**Rule:** after any `registry.yaml` change, regenerate, run `--check`, and run
`tools/catalog_lint.py`. A catalog that "did not encode (scratch N B)" is
usually NOT a sizing problem, it is entries out of ascending-id order.

## T20 -- a hand-copied vocabulary drifts silently, and a RETIRED one lies

**Rule:** a vocabulary with more than one consumer language gets a generator
and a staleness gate, or it gets one consumer. Never a generator on one side
and a comment saying "transcribed from" on the other. When a vocabulary is
retired, DELETE its identifiers rather than aliasing them onto the successor;
a working alias is how this survived undetected for months.
**Mechanism:** when two languages consume one registry and only one generates
its constants, the hand-written side has no failure mode that looks like
failure. A wrong wire NUMBER crashes or NACKs; a wrong wire NAME renders. The
copy compiles, the session goes live, the page draws, and the only symptom is
a label nobody cross-checks. Drift accumulates one skipped registry addition
at a time, and every skip is individually invisible.

Retiring a vocabulary converts that lag into an active lie. `setting_categories`
was tombstoned and succeeded by `ui_categories` on the same wire key, with a
different base (1, not 0) and 14 entries instead of 5. The generated C++ side
followed; the JS side kept the old 5-entry 0-based array. From the moment the
firmware emitted the new vocabulary, EVERY settings tab in EVERY JS client was
mislabeled, and entries 5 through 14 resolved to `undefined`. Nothing errored.
The UI looked finished.

**The census matters more than the one bug:** diffing all 24 hand tables
against the registry found SEVEN drifted -- missing frame types, three missing
NACK codes surfacing as raw numbers, missing CBOR keys, missing session-event
kinds, and `LIMITS` carrying 16 of 68 entries. Not one had been noticed.
**Fix:** `tools/gen_registry_header.py` emits the JS vocabulary alongside the
C++ header, committed because browsers import it directly, with `--check`
covering both artifacts. What legitimately stays hand-written is named and
justified in the file banner, because "this one is fine to transcribe" is the
belief that produced all seven. The `/drift` skill detects this class.

## `custom_sdkconfig` cannot express "off" as `=n`

pioarduino substitutes the literal text into sdkconfig and Kconfig rejects
`=n` for a bool. The build dies in `idf_build_process` with a CMake error
naming neither the option nor the line. Comment the line out instead; the
framework default is what you get. Related: editing that block wipes and
reinstalls framework packages, which has twice left
`managed_components/espressif__cjson/cJSON/` without its sources and once
needed a second invocation to reconfigure. A failed first build after a
`custom_sdkconfig` edit is not necessarily a real failure. Run it again before
diagnosing.
