---
name: build-and-flash
description: PlatformIO build, native test, and OTA deploy workflow for the SlopDrive-32 firmware. Use when building, testing, flashing, deploying, or verifying firmware on the live ESP32-S3.
---

# Build and flash (operational summary; `.claude/rules/build-test-deploy.md` is the law)

The target is firmware on a LIVE machine, no simulator. Running a change
means build -> OTA deploy -> verify version on-device. "Deployed" means
version-verified via the device API, never "upload completed" (CANON C-8).

## Build

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e sd32-ota
# web UI filesystem image: same command -t buildfs
```

## Native tests (host, no hardware)

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" test -e native
```

## Deploy (OTA over HTTP; espota is broken on this host, do not use it)

- Bump FIRMWARE_VERSION in include/system/config_api.h (plain semver patch)
  before EVERY deploy; the version delta is the proof the flash landed.
- Token comes from git-ignored include/secrets.h (SECRET_OTA_PASSWORD);
  POST the image to /api/ota (app) or /api/ota/fs (LittleFS) with
  X-OTA-Token. Device reboots ~10-15 s.
- The scripted path (preferred when present, local-only, not in clones):
  `.claude/skills/run-slopdrive-32/deploy.ps1` builds, pre-flights
  (reachable + not moving), flashes, and prints old -> new version.
- Device unreachable? STOP and ask the operator. Never guess IPs, never
  port-scan, never fall back to serial unasked.

## Before declaring done

`python tools/canon_lint.py` must report zero findings for substantive work
(governance.md §5). Touched-suite tests + one live smoke is the release floor.

## Tooling upkeep

Regenerate the compile database whenever platformio.ini changes (Serena and
clangd both eat it):

```
python tools/gen_clangd_db.py
```

Never use `pio run -t compiledb` here: it emits framework TUs only and zero
entries for `src/`, which leaves clangd parsing our headers as C. See
`.claude/rules/serena.md`.
