# SD-32 — OTA Update Path (firmware + web UI over WiFi)
## Executor: Claude Opus 4.8 · full codebase in context · device live at 192.168.1.229 (serial access available for the one-time migration)
## Goal: after this task, routine firmware flashes are `pio run -e sd32-ota -t upload` and UI updates are
## `-t uploadfs`, both over the device's IP (works via Tailscale). Serial remains the bench/rescue path only.
## You have latitude: where this spec says JUDGMENT, decide from the actual codebase/setup and log the reasoning.

═══════════════════════════════════════════════════════════════
1. PARTITION TABLE (one-time serial migration)
═══════════════════════════════════════════════════════════════
- Inspect the current partition scheme and the actual sizes: built app (.pio/build/*/firmware.bin)
  and the web bundle / LittleFS usage. Design a 16MB table with: nvs (keep current size unless
  undersized), otadata, ota_0 + ota_1 (JUDGMENT: size = current app size with generous headroom,
  typically 2×–3× — quote the numbers), coredump if currently present, and LittleFS taking the
  remainder. Preserve NVS OFFSET AND SIZE if at all possible so saved config survives the
  migration; if the current table makes that impossible, say so and note that config re-entry is
  needed once.
- Add partitions_ota.csv to the repo, reference via board_build.partitions.
- Write the exact one-time migration procedure into the log: build, serial flash (which erases what
  — be explicit about whether NVS/LittleFS survive), then uploadfs to restore the UI. This is the
  last required cable session.

═══════════════════════════════════════════════════════════════
2. ARDUINOOTA (the PlatformIO-native path)
═══════════════════════════════════════════════════════════════
- Initialize ArduinoOTA after WiFi is up: hostname "sd32" (JUDGMENT: reuse existing mDNS/hostname
  constant if one exists), password from the secrets file — the repo has one; find how secrets are
  currently included (grep for the pattern) and add OTA_PASSWORD alongside the existing entries,
  updating the example/template file if one exists, never committing a real value.
- ArduinoOTA.handle() serviced from an appropriate existing low-priority loop/task (JUDGMENT:
  pick the task with headroom; do not create a new task if an existing service loop fits; must not
  run on the motion-critical core/path — justify placement).
- SAFETY GATE (non-negotiable): onStart must (1) refuse or hard-stop all motion first — stop the
  pattern engine, stop/disable the motor via the existing stop semantics, detach/quiesce the
  stream sources; (2) suspend the telemetry/WS broadcast tasks (flash writes stall flash-cache
  access; heavy tasks touching flash-resident code/data during write windows cause resets); 
  (3) applog "OTA start". onEnd/onError: applog outcome; onError must leave the machine in the
  stopped state (a failed OTA never resumes motion by itself). Device reboots on success
  (ArduinoOTA default).
- platformio.ini: split into [env:sd32] (serial, unchanged behavior) and [env:sd32-ota]
  (upload_protocol=espota, upload_port=192.168.1.229, auth flag wired to the secrets password —
  JUDGMENT on the cleanest way to feed it: upload_flags with --auth sourced so the secret isn't
  hardcoded in the ini; extra_scripts reading the secrets file is acceptable).

═══════════════════════════════════════════════════════════════
3. HTTP OTA ENDPOINTS (the curl-from-anywhere fallback)
═══════════════════════════════════════════════════════════════
- On the existing ESPAsyncWebServer: POST /api/ota (app image → Update.begin(UPDATE_SIZE_UNKNOWN)
  … U_FLASH) and POST /api/ota/fs (LittleFS image → U_SPIFFS), using the async upload handler
  pattern (chunked writes in the file-upload callback, finalize + verify on final chunk, JSON
  result, then scheduled reboot ~500ms later so the response flushes).
- AUTH REQUIRED: same secret (JUDGMENT: header token e.g. X-OTA-Token checked constant-time, or
  HTTP basic — pick one, document the curl one-liner for both endpoints in the log). Reject
  unauthenticated with 401 before consuming the body if the framework allows.
- Same safety gate as §2 (share the implementation — one prepareForOta() used by both paths).
- Refuse concurrent updates (single in-flight flag) and refuse if an ArduinoOTA session is active.

═══════════════════════════════════════════════════════════════
4. GUARDRAILS
═══════════════════════════════════════════════════════════════
- No changes to motion, protocol, or UI behavior beyond the safety gate hooks. rollback-on-bootloop
  is explicitly OUT of scope (serial is the accepted rescue path).
- Flash-write windows: audit that nothing in the gated state still writes NVS/LittleFS
  concurrently (settings save, applog-to-flash if any) — quiesce or defer during update.
- Keep the secrets pattern consistent with the repo's existing convention; template updated;
  .gitignore already covering it (verify).

═══════════════════════════════════════════════════════════════
5. LIVE VERIFICATION (quote evidence per row)
═══════════════════════════════════════════════════════════════
V1 Migration: one-time serial flash performed per §1 procedure; device boots; note whether config
   survived as predicted.
V2 `pio run -e sd32-ota -t upload` from the network (not USB): succeeds, device reboots into the
   new image (bump a version string and quote it from /api/capabilities or boot log before/after).
V3 `-t uploadfs`: UI bundle updates over WiFi; browser refresh shows a marker change; firmware
   untouched (same version string).
V4 HTTP path: curl both endpoints with auth → success + reboot; without auth → 401, nothing
   written.
V5 Safety gate: start a pattern, fire an OTA → motion stops BEFORE flash writes begin (telemetry
   quoted), machine comes back in stopped/un-resumed state.
V6 Failure handling: abort an upload mid-transfer → device stays on the old image, still
   reachable, motion still stopped until user action.
V7 Second OTA cycle end-to-end (prove ota_0↔ota_1 ping-pong works, not just the first hop).

═══════════════════════════════════════════════════════════════
6. EXECUTOR LOG
═══════════════════════════════════════════════════════════════
Partition math with actual numbers / every JUDGMENT with reasoning / secrets integration details
(pattern followed, template updated) / the exact routine-use commands (pio + both curl one-liners)
written as a short README block I can keep / V1–V7 evidence quoted.

═══════════════════════════════════════════════════════════════
7. AGENT DOCUMENTATION — .clinerules SECTION (required deliverable)
═══════════════════════════════════════════════════════════════
Add a section to the repo's .clinerules (JUDGMENT: match the file's existing structure/location —
if the repo uses .clinerules/ directory convention or a single file, follow what exists; create
the conventional form if absent) titled "Firmware & Web UI Deployment (OTA)" that teaches every
future agent the deployment contract. It must cover, in the file's existing voice/format:

- OTA is the DEFAULT deployment path. Do not ask the user to plug in USB, do not hunt COM ports,
  do not touch serial envs unless OTA is confirmed unavailable.
- Firmware: `pio run -e sd32-ota -t upload` · Web UI only (after vite build): 
  `pio run -e sd32-ota -t uploadfs` — include the actual final commands and the curl fallbacks
  for /api/ota and /api/ota/fs with the auth mechanism (reference the secrets file by path for
  where the token lives; never inline the value).
- The device's usual address is 192.168.1.229. If unreachable or an upload fails: do NOT guess
  other IPs, do NOT port-scan the network, do NOT fall back to serial on your own. STOP and ask
  the user: what IP is the device currently on, and what state is the machine in (powered?
  on WiFi? mid-crash? cable-connected?). Resume only with their answer.
- Before flashing: confirm the machine is idle (no pattern running) — the firmware's OTA gate
  will stop motion itself, but agents should not fire an update mid-session without the user
  knowing. After flashing: verify the new build actually landed (curl /api/capabilities or the
  version string; quote it) before declaring success — a completed upload is not a verified
  deployment.
- UI-only changes never require a firmware upload (uploadfs + browser refresh is the whole
  cycle); firmware changes reboot the device (~10-15s offline is normal, warn the user if
  they're mid-session).
- Serial/USB remains the rescue path ONLY (bootlooped device, WiFi-breaking change, partition
  work) and is a bench operation the user performs — describe when to recommend it, not how to
  seize it.

Verification row V8: the .clinerules section exists, follows repo conventions, and contains the
exact working commands from your V2/V3/V4 runs (commands proven live, then documented — never
documented untested).
