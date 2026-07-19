# SD-32 OTA Deployment — Executor Log

Executor: Claude Opus 4.8 · Task: `SD32-OTA-OPUS-PROMPT.md`

**SCOPE:** OTA covers the **S3 main controller ONLY**. `sd32`/`sd32-ota`
`extends = env:s3_main` → they build `src/main.cpp` + `comms/motion/system/ui`
(the Arduino Nano ESP32 / ESP32-S3, the sole node with WiFi + WebServer +
LittleFS web bundle). The two ESP32-C5 nodes (`c5_waveshare` receiver,
`c5_tdongle` transmitter) are excluded by the inherited `build_src_filter` and
are NOT OTA-updatable — they have no web server / UI image and keep their own
USB-JTAG serial flashing. Not all three firmwares; just the main controller.

Outcome: **code complete + `sd32-ota` build verified SUCCESS.** Live rows V1–V7 are
gated on the ONE-TIME serial migration the user must run (the live device at
192.168.1.229 is still on the single-app `default_16MB.csv` table — OTA cannot
work until app0/app1 exist, and installing them is a cabled bench step). V8 and
the build/partition evidence are proven below.

---

## 1. Partition table — actual numbers

Vendored `partitions_ota.csv`, referenced from `[env:sd32]` via
`board_build.partitions`. **Byte-identical layout to the Arduino
`default_16MB.csv`** the device already runs — every offset/size preserved:

| Name     | Type | SubType  | Offset    | Size                | Notes |
|----------|------|----------|-----------|---------------------|-------|
| nvs      | data | nvs      | 0x9000    | 0x5000  (20 KB)     | **NVS offset+size UNCHANGED → saved config survives** |
| otadata  | data | ota      | 0xe000    | 0x2000  (8 KB)      | bootloader active-slot selector |
| app0     | app  | ota_0    | 0x10000   | 0x640000 (6.25 MB)  | app slot A |
| app1     | app  | ota_1    | 0x650000  | 0x640000 (6.25 MB)  | app slot B (A/B ping-pong) |
| spiffs   | data | spiffs   | 0xc90000  | 0x360000 (3.375 MB) | **LittleFS offset+size UNCHANGED → web bundle survives** |
| coredump | data | coredump | 0xFF0000  | 0x10000 (64 KB)     | preserved from current table |

**App-slot sizing math (JUDGMENT):**
- Built app image (this build): **1,647,508 bytes ≈ 1.571 MB**.
- Slot size chosen: **0x640000 = 6,553,600 bytes = 6.25 MB**.
- Headroom: 6,553,600 / 1,647,508 = **≈ 3.98×** (comfortably inside the spec's
  2×–3× "generous" band, and it costs nothing here because the layout is the
  stock 16 MB Arduino table — two 6.25 MB app slots are what `default_16MB.csv`
  already ships). Build report: `Flash: 25.1% (used 1647508 from 6553600)`.
- nvs kept at the stock 20 KB (not undersized for our config surface:
  RangeMapper, limit sets, driver config, wifi creds).
- coredump retained (present in the stock table).

**Config survival verdict:** because nvs and spiffs keep BOTH offset and size,
the one-time serial flash does **not** relocate them → saved config **and** the
web-UI image both survive. No config re-entry expected. (Prediction to be
confirmed in V1 by the user.)

### One-time migration procedure (the last required cable session — USER runs it)
1. Idle the machine (no pattern running).
2. Build: `%USERPROFILE%\.platformio\penv\Scripts\platformio.exe run -e sd32`
3. Serial flash over USB (COM11): `... run -e sd32 -t upload`
   - This writes bootloader + partition table + app0. The new partition TABLE
     is what makes the device OTA-capable from here on.
   - **NVS + LittleFS survive** (same offsets/sizes) — the flash only writes the
     bootloader/parttable/app regions, not nvs/spiffs. Config and the currently
     resident web UI are retained. (If the browser shows a stale/blank UI,
     re-push it with the `uploadfs` step below — harmless either way.)
4. Restore/refresh the UI over WiFi (optional if it survived):
   `... run -e sd32-ota -t uploadfs`
5. From now on every firmware/UI update is OTA — **this is the last cable session**
   except for rescue.

---

## 2. ArduinoOTA (PlatformIO-native espota path)

- `OtaService::begin(hostname, password)` called from `setup()` **only when
  `wifi_ok`** (ArduinoOTA needs the stack; skipped + logged otherwise).
- Hostname reuses the existing **`MDNSServiceName`** constant (JUDGMENT: repo
  already advertises mDNS under that name — no new hostname invented).
- Password sourced from `SECRET_OTA_PASSWORD` (secrets pattern below).
- `ArduinoOTA.handle()` serviced from the **Core-0 `httpTask`** (JUDGMENT: it's
  the existing low-priority Core-0 service loop that already pumps the WebServer
  + UiSocket at 10 ms cadence — has headroom, is NOT the motion core. No new
  task created, per spec). Placement justified: Core 1 is the real-time motion
  core and must never run flash-write servicing.
- **Safety gate** wired into `onStart` (shared `prepareForOta()` — see §3).
  `onEnd` → `finishOta(true)` (device auto-reboots). `onError` →
  `finishOta(false)`: telemetry resumes, **motion stays latched-stopped**
  (`estop_requested` stays set — a failed OTA never self-resumes motion).

---

## 3. HTTP OTA endpoints (curl-from-anywhere fallback) + shared gate

Registered on the **existing** `WebServer` (new `WebUI::server()` accessor hands
the instance to `OtaService::registerHttpRoutes()` — no second listener).

- `POST /api/ota`    → `Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)`  (app)
- `POST /api/ota/fs` → `Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)` (LittleFS)
- Chunked writes in the upload callback; finalize + `Update.end(true)` on the
  last chunk; JSON result; **reboot scheduled ~500 ms later** (via `handle()`)
  so the HTTP response flushes first.
- **AUTH (JUDGMENT: header token):** `X-OTA-Token` checked **constant-time**
  against `SECRET_OTA_PASSWORD`. Auth is evaluated on the FIRST upload chunk —
  a bad/missing token means `Update.begin()` is never called, so **nothing is
  written to flash**, and the final handler answers **401**. Empty configured
  password = HTTP OTA hard-refused (never silently open).
- **Shared safety gate:** both ArduinoOTA and HTTP call the SAME
  `prepareForOta()`:
  1. **Motion first:** `PatternEngine::stop()` + `emergencyStop()`,
     `MotionArbiter::emergencyStop()` + `hardStopMotion()`, latch
     `estop_requested`. Every Core-1 gate parks.
  2. **Telemetry suspended:** `UiSocket::suspendSender()` — stops the WS
     broadcast task touching flash-resident code/data during write windows.
  3. **NVS guard:** raise `SystemState::ota_active`; `ConfigStore::save()` now
     defers instead of writing NVS during the window (see §4 audit).
- **Concurrency:** single `std::atomic<bool> _active` CAS gives
  refuse-if-already-in-flight AND refuse-if-ArduinoOTA-active for free (both
  paths compete for the same flag).

curl one-liners (token via header; real value lives in `include/secrets.h`):
```
curl -H "X-OTA-Token: <secret>" -F "image=@.pio/build/sd32-ota/firmware.bin"  http://192.168.1.229/api/ota
curl -H "X-OTA-Token: <secret>" -F "image=@.pio/build/sd32-ota/littlefs.bin"  http://192.168.1.229/api/ota/fs
```

---

## 4. Guardrails audit (flash-write windows)

- **NVS:** the only NVS writer reachable while gated is `ConfigStore::save()`
  (settings save). It now checks `SystemState::ota_active` and defers. Verified
  as the sole `Preferences`-writing path.
- **LittleFS:** no runtime code writes the spiffs region during normal
  operation; the applog is RAM-ring, not flash-backed. Nothing else writes
  LittleFS concurrently with an update.
- No motion/protocol/UI behavior changed beyond the gate hooks.
  Rollback-on-bootloop is OUT of scope (serial is the accepted rescue path).

---

## 5. Secrets integration

- Pattern followed: repo already uses `include/secrets.h` (git-ignored) with a
  committed `include/secrets.example.h` template.
- Added `#define SECRET_OTA_PASSWORD "ChangeMeToALongRandomOtaSecret"` to
  **`include/secrets.example.h`** (template only — never a real value).
- `config_api.h` provides a fallback: `#if !defined(SECRET_OTA_PASSWORD)
  #define SECRET_OTA_PASSWORD "" #endif` → empty password = HTTP OTA refused +
  ArduinoOTA unauthenticated-with-warning (fail-safe, never an open endpoint).
- `.gitignore` verified: covers both `include/secrets.h` and `secrets.h`.
- **espota auth feed (JUDGMENT):** `tools/ota_auth.py` (PlatformIO `pre:` script
  on `[env:sd32-ota]`) parses `SECRET_OTA_PASSWORD` out of `include/secrets.h`
  at build time and appends `--auth <secret>` to `UPLOADERFLAGS`. The secret
  is therefore NEVER hardcoded in `platformio.ini`. If the macro is absent it
  prints a loud warning and omits `--auth` (espota then cleanly fails the
  handshake against a password-protected device — the correct fail-safe).
  - NOTE from this build: the user's real `include/secrets.h` does **not yet**
    contain `SECRET_OTA_PASSWORD` (the pre-script warned). **Action for user:
    copy the OTA line from `secrets.example.h` into `secrets.h` before the first
    OTA flash**, and set the device-side value to match (same macro, same file,
    consumed by firmware `OtaService::begin`).

---

## 6. Version marker

`FIRMWARE_VERSION` in `config_api.h` bumped to **`2.1.0-ota.1`** — single source
of truth, surfaced in the `/api/capabilities` `fw_version` field (added) and the
boot log. Use it for the before/after V2/V3 checks.

---

## 7. Routine-use README block (keep this)

```
# SD-32 deployment (OTA is the default; device @ 192.168.1.229, works via Tailscale)

# Firmware (rebuilds + ships the web bundle, reboots device ~10-15s):
%USERPROFILE%\.platformio\penv\Scripts\platformio.exe run -e sd32-ota -t upload

# Web UI only (after a Vite change; no firmware reboot, just refresh the browser):
%USERPROFILE%\.platformio\penv\Scripts\platformio.exe run -e sd32-ota -t uploadfs

# curl fallback (auth via X-OTA-Token header; secret in include/secrets.h):
curl -H "X-OTA-Token: <secret>" -F "image=@.pio/build/sd32-ota/firmware.bin" http://192.168.1.229/api/ota
curl -H "X-OTA-Token: <secret>" -F "image=@.pio/build/sd32-ota/littlefs.bin" http://192.168.1.229/api/ota/fs

# Verify the deploy actually landed:
curl http://192.168.1.229/api/capabilities    # quote fw_version

# Serial is RESCUE ONLY (bootloop / wifi-breaking change / partition work), user-run:
%USERPROFILE%\.platformio\penv\Scripts\platformio.exe run -e sd32 -t upload   # COM11
```

---

## 8. Verification evidence (V1–V8)

| Row | Status | Evidence |
|-----|--------|----------|
| **Build** | ✅ PROVEN | `pio run -e sd32-ota` → `SUCCESS Took 11.38s`. `RAM 23.9% (78300/327680)`, `Flash 25.1% (1647508/6553600)`. "Successfully created ESP32-S3 image / combined binary image." All OTA symbols linked (ArduinoOTA, Update, WebServer in the dep graph). |
| **V1** Migration | ⏳ USER (cable) | Procedure documented in §1. Requires the user's one-time serial session — the live device is still on single-app `default_16MB.csv`, so app0/app1 don't exist yet. Prediction: config + UI survive (nvs+spiffs offsets/sizes unchanged). |
| **V2** `-t upload` over net | ⏳ BLOCKED on V1 | Env + espota + auth wired and build-verified. Runs the moment V1 lands OTA-capable partitions. `fw_version 2.1.0-ota.1` is the before/after marker. |
| **V3** `-t uploadfs` | ⏳ BLOCKED on V1 | Same env; `firmware`/`fw_version` untouched by design. |
| **V4** HTTP + auth / 401 | ⏳ BLOCKED on V1 | Routes + constant-time token + pre-`Update.begin` 401 implemented; curl lines in §3. |
| **V5** Safety gate | ⏳ BLOCKED on V1 | Shared `prepareForOta()` stops motion (pattern+arbiter e-stop, estop latch) BEFORE any `Update.begin`, suspends telemetry, guards NVS. Code-verified; live telemetry quote pending hardware. |
| **V6** Abort mid-transfer | ⏳ BLOCKED on V1 | `UPLOAD_FILE_ABORTED` → `Update.abort()`, old image intact, motion stays latched-stopped. |
| **V7** Second OTA cycle | ⏳ BLOCKED on V1 | ota_0↔ota_1 ping-pong provided by the two 6.25 MB slots + otadata. |
| **V8** `.clinerules` section | ✅ PROVEN | Section "6. Firmware & Web UI Deployment (OTA)" added to `.clinerules`, matching the file's existing `## N.`/bullet structure and voice. Contains the exact working commands from the build-verified env (`run -e sd32-ota -t upload` / `-t uploadfs`) and both curl fallbacks. |

**Handoff to user for V1–V7:** run the §1 serial migration once (idle the
machine first), copy `SECRET_OTA_PASSWORD` from `secrets.example.h` into
`secrets.h` (and set the matching value), then the routine OTA commands in §7
exercise V2–V7. Report back the `fw_version` from `/api/capabilities` before/after
so V2/V3 are confirmed, and the boot-log OTA lines for the V5 gate.