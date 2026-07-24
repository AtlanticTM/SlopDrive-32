#pragma once

#include <Arduino.h>

// =============================================================================
// Secrets (WiFi password, network addresses) — kept OUT of git like a good
// pup keeps its leash on. No accidental exposure here. :3
// =============================================================================
// Real values live in include/secrets.h (git-ignored). If that file is missing
// (e.g. a fresh clone before you copy secrets.example.h -> secrets.h), we fall
// back to harmless placeholders so the project still compiles.
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #warning "include/secrets.h not found - copy secrets.example.h to secrets.h and edit it. Using placeholder WiFi values."
  #define SECRET_WIFI_SSID       "CHANGE_ME_SSID"
  #define SECRET_WIFI_PASSWORD   "CHANGE_ME_PASSWORD"
  #define SECRET_INTIFACE_HOST   "192.168.1.100"
  #define SECRET_INTIFACE_PORT   54817
#endif

// If secrets.h predates the OTA feature (older clone), it won't define
// SECRET_OTA_PASSWORD. Fall back to an empty string — OtaService treats an
// empty password as "HTTP OTA hard-refused + ArduinoOTA unauthenticated" and
// logs a loud warning, so a stale secrets.h fails safe rather than silently
// exposing an open flash endpoint. Copy the new line from secrets.example.h. :3
#if !defined(SECRET_OTA_PASSWORD)
  #define SECRET_OTA_PASSWORD    ""
#endif

// ---- Firmware version --------------------------------------------------------
// Bumped by hand on each firmware change so an OTA can be verified as landed
// (surfaced via /api/capabilities → "fw_version" and the boot log). This is the
// single source of truth for "which build is actually running." :3
#define FIRMWARE_VERSION        "2.1.39"

// =============================================================================
// WiFi Configuration (values come from secrets.h)
// =============================================================================
#define WIFI_SSID      SECRET_WIFI_SSID
#define WIFI_PASSWORD  SECRET_WIFI_PASSWORD

// Per-credential-set connect timeout at boot. setupWiFi() tries the primary
// (secrets.h) creds for this long, then the NVS-stored secondary creds for the
// same window, before dropping to serial TCode control. 10s is enough for a
// normal WPA2 associate + DHCP without stalling boot for a network that isn't
// there. Boot-only blocking — never hit on the real-time path. :3
#define WIFI_CONNECT_TIMEOUT_MS   10000

// ---- Boot-time / reconnect strongest-AP selection --------------------------
// Our deployment is a multi-AP network sharing ONE SSID. The ESP32 default
// fast-scan latches onto the first-heard AP (often the weakest) and never
// roams. The rig is stationary during use, so a full scan + strongest-BSSID
// pin at every WiFi bring-up (cold boot AND every reconnect-from-disconnected
// cycle) is the complete fix. See TransportManager::_connectBest(). :3
#define WIFI_SCAN_PIN_ENABLED       1
// Consecutive pinned-connect failures tolerated before a bring-up cycle falls
// back to an unpinned WiFi.begin() (lets the core associate with ANY live AP so
// a dead/deauthing pinned BSSID can't strand the device). The next reconnect
// cycle re-scans and re-pins the current strongest AP.
#define WIFI_PIN_MAX_ATTEMPTS       3
// APs for our SSID weaker than this (dBm) are still connectable but are omitted
// from the per-candidate applog dump to keep the boot log readable. The chosen
// AP is ALWAYS logged regardless of this threshold.
#define WIFI_MIN_RSSI_LOG_DBM       (-90)
// While the link is down, minimum spacing between supervised reconnect cycles.
// Keeps a WiFi outage from turning into a continuous scan storm on the comms
// task (each cycle blocks ~scan + connect-wait). :3
#define WIFI_RECONNECT_INTERVAL_MS  5000

// =============================================================================
// Device Geometry — AIMServo build (DRIVER_AIM_SERVO)
// =============================================================================
// 57AIM30 closed-loop servo drive on a CAPSTAN DRUM (custom v0.0 controller).
// The Dyneema line wraps the drum, so linear travel = drum circumference per
// drum revolution — no belt teeth, just a slick wrap that pulls the carriage
// in balls-deep and lets it slide back out with zero backlash. :3
//
// Drive train math (motor -> drum through a 2:1 reduction):
//   MOTOR_STEPS_PER_REV: 800   (drive DIP switch, at the motor shaft)
//   REDUCTION:           2.0   (drum turns once per two motor revs — motor->drum)
//   STEPS_PER_REV:       800 × 2 = 1600 steps per DRUM revolution
//   DRUM_DIAMETER:       25.0 mm  → circumference = π × 25 = 78.5398 mm/drum-rev
//   STEPS_PER_MM:        1600 / 78.5398 = ~20.372 steps/mm
//   MAX_TRAVEL:          rail-length agnostic — there is NO fixed geometry
//                        ceiling. The user's configured max rail length
//                        (DEFAULT_MAX_RAIL_MM, runtime-set) bounds the homing
//                        search sweep, and sensorless homing MEASURES the real
//                        usable stroke between the two hard stops. :3
//   HOMING_BACKOFF:      10.0 mm — pull out 10mm after the stall so the carriage
//                        isn't grinding balls-deep against the hard stop. :3
//
// Keep AIM_STEPS_PER_MM a FLOAT — 20.372 truncated to an int would slowly drift
// the carriage off by mm over a long stroke. Single-precision float feeds the
// FPU and keeps every thrust landing exactly where it's told. :3
//
// RUNTIME GEOMETRY: the AIM drive's steps/rev is an electronic-gear register
// (0x0B) reprogrammable over RS485 Modbus, so steps/mm is a RUNTIME value —
// seeded from NVS at boot (default below), recalculated live when the drive is
// reprogrammed from the Configure pane. Every consumer goes through
// aimStepsPerMm(); a steps/rev change forces a re-home (the step<->mm mapping
// of the current position reference is void), no reboot required.
#define AIM_MOTOR_STEPS_PER_REV_DEFAULT 800                               // @ motor shaft (drive reg 0x0B factory/OSSM standard)
#define AIM_REDUCTION             2.0f                                    // 2:1 motor -> drum
#define AIM_DRUM_DIAMETER_MM      25.0f
#define AIM_MM_PER_REV            (3.14159265f * AIM_DRUM_DIAMETER_MM)    // 78.5398 mm/drum-rev
#define AIM_STEPS_PER_MM_DEFAULT  ((AIM_MOTOR_STEPS_PER_REV_DEFAULT * AIM_REDUCTION) / AIM_MM_PER_REV)  // ~20.372
#define AIM_HOMING_BACKOFF_MM     10.0f
// Front-end safety margin subtracted from the measured stroke. The stall
// positions are FAS *commanded* counters captured after the confirm window
// (4 polls @150Hz) + INA228 averaging lag — the carriage is already parked
// against the wall while ~1-3mm of phantom steps keep counting, at BOTH ends.
// Backoff only accounts for the rear (home) side; this margin keeps full
// extension off the FRONT hard stop instead of commanding into it. :3
#define AIM_HOMING_FRONT_MARGIN_MM 5.0f

// ---- Encoder cross-check (FAS commanded vs drive-reported position) ---------
// The AIM encoder is 15-bit absolute, 32768 counts per MOTOR rev (fixed silicon
// resolution — independent of the e-gear steps/rev, so this ratio never changes
// when the drive is reprogrammed). counts/mm ≈ 834.4 on the 25mm drum @ 2:1.
#define AIM_ENC_COUNTS_PER_REV    32768.0f
#define AIM_ENC_COUNTS_PER_MM     ((AIM_ENC_COUNTS_PER_REV * AIM_REDUCTION) / AIM_MM_PER_REV)
// Standstill deviation beyond this (for 3 consecutive steady samples) raises
// the lost-steps warning. ~1.5mm ≈ 61 steps @ 40.7 steps/mm — far above noise,
// far below anything that could hurt. Report-only: it never gates motion. :3
#define AIM_ENC_DEV_WARN_MM       1.5f
// Excursion (mm of FAS travel) needed before the validator trusts a measured
// encoder direction + scale and starts scoring deviation.
#define AIM_ENC_SIGN_DETECT_MM    8.0f

#if defined(DRIVER_AIM_SERVO)
// Runtime geometry accessors — defined in src/system/MotionGeometry.cpp.
// aimGeometryInit() loads the persisted motor steps/rev from NVS (namespace
// "servocfg") in setup(); aimSetMotorStepsPerRev() recomputes steps/mm live
// (32-bit float store is atomic on Xtensa — Core 1 sees old or new, never torn).
void     aimGeometryInit();
void     aimSetMotorStepsPerRev(uint16_t steps_per_rev, bool persist);
uint16_t aimMotorStepsPerRev();          // per MOTOR rev (mirrors drive reg 0x0B)
int32_t  aimStepsPerRev();               // per DRUM rev (motor × reduction)
float    aimStepsPerMm();
#define AIM_MOTOR_STEPS_PER_REV   (aimMotorStepsPerRev())
#define AIM_STEPS_PER_REV         (aimStepsPerRev())
#define AIM_STEPS_PER_MM          (aimStepsPerMm())
#else
#define AIM_MOTOR_STEPS_PER_REV   AIM_MOTOR_STEPS_PER_REV_DEFAULT
#define AIM_STEPS_PER_REV         ((int32_t)(AIM_MOTOR_STEPS_PER_REV_DEFAULT * AIM_REDUCTION))
#define AIM_STEPS_PER_MM          AIM_STEPS_PER_MM_DEFAULT
#endif

// Homing sweep speed: fixed in mm/s and converted through the LIVE steps/mm so
// a reprogrammed steps/rev can never turn the gentle homing crawl into a
// freight-train slam (600 steps/s was ~29.5 mm/s at the default 20.372
// steps/mm — same crawl, now invariant). forceStopAndNewPosition() kills the
// pulse train the instant we detect the current spike. :3
#define AIM_HOMING_SPEED_MM_S     29.5f
#define AIM_HOMING_SPEED_STEPS_S  ((int32_t)(AIM_HOMING_SPEED_MM_S * AIM_STEPS_PER_MM))

// ---- Sensorless homing tunables (INA228 current-stall detection) ----
// The new PCB has NO endstop switch — we feel our way to the hard stop by
// watching motor current on the INA228. Free travel draws low single-digit
// amps; when the carriage buries itself against the stop the current climbs
// fast toward the drive's limit. We call it a stall when current sits above the
// free-run baseline by STALL_MARGIN_A for STALL_CONSEC consecutive polls. :3
// Tune these empirically on the real machine — start gentle, tighten later.
#define AIM_HOME_STALL_MARGIN_A     3.0f   // amps above free-run baseline = stall
#define AIM_HOME_STALL_CONSEC       4      // consecutive over-threshold samples
#define AIM_HOME_POLL_HZ            150    // INA228 poll rate during homing (Hz)
#define AIM_HOME_BASELINE_SAMPLES   20     // samples averaged for the free-run baseline

// ---- Modbus direct-drive backend tunables (Phase 3 — see plan.md) ----------
// Streamed-setpoint executor cadence: how often StreamedSetpointExecutor
// samples the active TrapezoidProfile and streams one 0x7B setpoint while
// genuinely moving. 10ms matches the OSSM-RS reference cadence and fits
// comfortably inside the bus budget at 115200 (plan.md "Bus budget"). :3
#define AIM_SP_PERIOD_MS            10
// Idle/frozen keep-alive cadence — slower than the motion cadence since
// nothing's actually changing; also doubles as a passive bus-liveness probe
// (a keep-alive echo failing is just as valid a health signal as a motion
// setpoint failing). :3
#define AIM_SP_KEEPALIVE_MS         250
// Bus-health watchdog thresholds (consecutive missed 0x7B echoes, see
// ServoModbus::getBusHealth().sp_fail_streak): FREEZE holds position and
// latches a soft fault (recoverable by re-home); ESTOP additionally cuts
// drive output. Both are one-shot logged — this is read on the 1ms
// motorTask tick and must never spam the ring. :3
#define AIM_SP_FAIL_FREEZE          3      // ~30ms of silence -> freeze in place
#define AIM_SP_FAIL_ESTOP           15     // ~150ms of silence -> output off
// Wire-mapping sign: wire_counts = wire_offset + AIM_MODBUS_WIRE_SIGN * cmd_counts.
// The drive's own absolute encoder frame (regs 0x16/0x17) has UNKNOWN sign
// relative to the arbiter's home=0/front=negative convention until confirmed
// on the bench — this is a PLACEHOLDER default. First bench step: force-home,
// jog a small positive mm move, watch which way the encoder count actually
// moves, and flip this if it's backwards. :3
// Jerk ceiling (mm/s^3) for the Modbus executor's jerk-limited target tracker
// — OSSM-RS parity (their Ruckig streams at MAX_JERK = 100000 mm/s^3). The
// tracker glides toward every target under vmax/amax/jmax; this is what
// killed the "clocked waypoint" texture of raw trapezoid streaming. :3
#define AIM_MODBUS_JERK_MM_S3       100000.0f
// BENCH-DETERMINED (fw 2.1.27, first live jog): +1 ran the carriage the wrong
// way — a positive-depth command must move the same physical direction as the
// FAS build's negative-step convention, and on this wiring that is encoder
// NEGATIVE. Flipped to -1 and verified by the operator. :3
#define AIM_MODBUS_WIRE_SIGN        (-1)
// Standstill max-output written to drive reg 0x18 whenever Modbus-mode
// applies its expected register set. BENCH-LEARNED ENCODING (fw 2.1.24): the
// RAW register packs PWM*10 + alarm-mode digit — factory value reads 600
// (= PWM 60, alarm 0), and OSSM-RS's "12-60" range describes the DECODED PWM
// field, not the raw register. Writing a bare 20 here (= PWM 2) made the
// standstill hold MEGA weak — the operator felt the shaft go limp. 600 =
// factory full hold. Lower for squish-safety as PWM*10 (e.g. 200 = PWM 20). :3
#define AIM_MODBUS_STANDSTILL_MAX   600


// =============================================================================
// DEFAULT_MAX_RAIL_MM — rail-length-agnostic default ceiling
// =============================================================================
// This firmware is agnostic to the physical length of the rail — you can run
// it on a machine of ANY stroke. There is NO hardcoded geometry ceiling. The
// "max rail length" is a RUNTIME user setting (state.config.max_rail_mm,
// editable in the WebUI and persisted to NVS) whose only jobs are:
//   1. Bound the sensorless homing search sweep so homing can't hunt forever
//      when a wall is never felt (electrical/mechanical fault).
//   2. Serve as the position ceiling BEFORE homing has measured the real
//      stroke. Once homing feels out both hard stops, the MEASURED stroke is
//      the source of truth and governs the usable range (measurement wins).
// This macro is only the factory default that seeds that setting — 500mm is a
// sane, generous rail length. The AIM drive-train constants above (steps/mm,
// drum geometry) remain purely for the motion math. :3
#define DEFAULT_MAX_RAIL_MM  500.0f

// =============================================================================
// Motor Driver Pins (ESP32-S3)
// =============================================================================
// --- AIMServo build (DRIVER_AIM_SERVO) — CUSTOM v0.0 CONTROLLER (Nano ESP32) ---
// New board routes the servo drive through an SN74AHCT125 buffer -> opto inputs.
// PUL → GPIO 5 (D2), DIR → GPIO 6 (D3). No endstop on this board — homing is
// sensorless via the INA228 current sensor (see below). The old GPIO12 endstop
// is kept only for the legacy HOMING_USE_ENDSTOP fallback. :3
//
// The AHCT125's output-enable is tied LOW (always on), so ANY boot glitch on
// PUL/DIR squirts straight through to the motor's opto inputs — pull both LOW
// as early as possible in setup(). No premature twitching before we're ready. :3
#define AIM_PIN_STEP            5    // PUL — pulse train to the servo drive (D2)
#define AIM_PIN_DIR             6    // DIR — direction signal (D3) [was GPIO4 on old PCB]
#define AIM_PIN_ENDSTOP         12   // Legacy endstop (only used if HOMING_USE_ENDSTOP)

// =============================================================================
// I2C bus (INA228 current sensor @ 0x40, AS5600 encoder @ 0x36 — deferred)
// =============================================================================
// Nano ESP32 does NOT default I2C to these pins — call Wire.begin(SDA, SCL)
// explicitly. INA228 lives behind an ISO1640 isolator but is transparent to
// software. The bus is where the machine feels itself out — every current
// reading is the drive telling us how hard it's straining. :3
#define PIN_I2C_SDA             8    // D5
#define PIN_I2C_SCL             9    // D6

// INA228 high-side current/voltage monitor on the 36V bus. Register map differs
// from the INA226 — use an INA228-specific library. 5mΩ shunt, ADCRANGE=0. :3
#define INA228_I2C_ADDR         0x40
#define INA228_SHUNT_OHMS       0.005f   // 5 mΩ, 2W
#define INA228_MAX_CURRENT_A    32.768f  // ADCRANGE=0 full scale (163.84mV / 5mΩ)

// =============================================================================
// RS485 / Modbus to motor — DEFERRED (do not implement this pass)
// =============================================================================
// XY-G485 auto-direction module, 19200 8N1. Gives access to the motor's
// internal 15-bit encoder, temps, fault codes. Wired but not yet driven. :3
#define AIM_PIN_485_TX          17   // D8 (deferred)
#define AIM_PIN_485_RX          18   // D9 (deferred)

// =============================================================================
// Status LEDs — Arduino Nano ESP32 (NOT a NeoPixel!)
// =============================================================================
// The Nano ESP32's onboard "RGB" is three DISCRETE LEDs on separate pins, plus
// a standalone orange user LED. All of them are ACTIVE-LOW (drive the pin LOW to
// light it). There is NO addressable NeoPixel on this board — the old
// Adafruit_NeoPixel status path must drive these discrete pins instead. :3
//
//   Orange user LED : GPIO48 (was the old NeoPixel data pin — now just a dumb LED)
//   RGB Red         : GPIO46
//   RGB Green       : GPIO0   ⚠ strapping pin — drive ONLY after boot settles
//   RGB Blue        : GPIO45
//
// Heartbeat LED (yellow-green on the PCB, ACTIVE-HIGH): GPIO21 (D10). Blinks to
// prove the S3 is alive during bring-up before the displays are wired. :3
#define PIN_LED_ORANGE          48
#define PIN_LED_R               46
#define PIN_LED_G               0    // strapping pin — init after boot
#define PIN_LED_B               45
#define LED_ACTIVE_LOW          1    // onboard LEDs sink current — LOW = lit
#define PIN_HB_LED              21   // heartbeat, ACTIVE-HIGH (PCB yellow-green LED, D10)
#define HB_LED_ACTIVE_HIGH      1

// Legacy alias — some status code still references PIN_NEOPIXEL_PIN. Point it at
// the orange LED so it compiles; the status module should migrate to the RGB
// pins above. There is no real NeoPixel to drive. :3
#define PIN_NEOPIXEL_PIN        48
#define NEOPIXEL_COUNT          1


// =============================================================================
// Motor Defaults
// =============================================================================
// Maximum motor speed in mm/s.
// Normal UI cap: 5000 mm/s. Expert mode UI cap: 10000 mm/s.
// This firmware ceiling is set to 10000 so expert mode values aren't rejected
// by the ConfigStore validator. The WebUI enforces the normal/expert split. :3
// The 57AIM servo drive at 800 steps/rev × 10 steps/mm can push this — it's
// a closed-loop servo, not a stepper, so it won't skip steps. Strap in. :3
#define MAX_SPEED_MM_S              10000.0f
#define DEFAULT_MAX_SPEED_MM_S      550.0f   // factory default on fresh boot

// USER limit-set factory defaults — deliberately gentle. The USER set caps the
// operator's own hand (manual moves) AND the "glide into the window" entry move
// when a machine-driven source starts from outside the stroke window. A soft
// 50 mm/s / 200 mm/s² means the carriage eases into the window instead of
// lunging to the edge at the input ceiling. Raise them in Settings once you've
// felt the machine out. Distinct from DEFAULT_MAX_SPEED/ACCEL which seed the
// INPUT set (streams/patterns). :3
#define DEFAULT_USER_MAX_SPEED_MM_S  50.0f    // gentle user/manual speed default
#define DEFAULT_USER_ACCEL_MM_S2     200.0f   // gentle user/manual accel default

// Split ceilings the WebUI's expert-mode toggle switches between. Advertised
// via /api/capabilities so the UI derives its slider `max` attrs from the API
// instead of hardcoding 3000/8000 literals (the "half-updated slider" bug in
// plan.md §5.10.1). Firmware itself always accepts anything up to the hard
// MAX_SPEED_MM_S/MAX_ACCEL_MM_S2 ceiling above — these are UI-only guardrails. :3
//
// Normal mode is confirmed safe up to 1000 mm/s for regular use. Expert mode
// unlocks the full hardware ceiling for those who know what they're asking for.
// The machine advertises these — the UI only ever asks, never assumes. :3
#define NORMAL_MAX_SPEED_MM_S       1000.0f
#define EXPERT_MAX_SPEED_MM_S       MAX_SPEED_MM_S      // 10000

// Default acceleration mm/s^2
// Normal UI cap: 50000 mm/s². Expert mode UI cap: 100000 mm/s².
// The 57AIM servo drive can hit these — it's a closed-loop servo that will
// absolutely fist the carriage into the endstop if you let it. yippie! :3
// Firmware ceiling is 100000 — the WebUI enforces the normal/expert split.
// NOTE: accel is stored as uint32_t in NVS (not uint16_t) — values above
// 65535 would silently overflow a uint16_t and corrupt the saved setting. :3
#define DEFAULT_ACCEL_MM_S2     8000.0f
#define MAX_ACCEL_MM_S2         100000.0f

// Split accel ceilings — same expert-mode split as speed above.
// Normal mode confirmed safe to 20000 mm/s²; expert unlocks the full ceiling.
#define NORMAL_MAX_ACCEL_MM_S2      20000.0f
#define EXPERT_MAX_ACCEL_MM_S2      MAX_ACCEL_MM_S2     // 100000

// =============================================================================
// Safe-approach soft start — no more scary full-speed lunges. :3
// =============================================================================
// Whenever motion (re)engages after a discontinuity — a brand-new stream
// connection, un-pausing, turning manual override OFF, the generator starting,
// or a target that jumps from outside the stroke window into it — the first
// move can be arbitrarily far from the carriage's current position. Dispatching
// that at the configured max speed produces a sudden, violent lunge.
//
// Instead, for the first SAFE_RESUME_RAMP_MS after (re)engagement the speed
// ceiling ramps linearly from SAFE_APPROACH_SPEED_MM_S up to the configured
// max. The carriage glides to where the stream wants it, THEN opens the
// throttle. Ease in first — nobody likes being slammed into from cold. :3
#define SAFE_APPROACH_SPEED_MM_S    100.0f   // initial speed cap on re-engage (mm/s)
#define SAFE_RESUME_RAMP_MS         1200u    // ramp duration back to full speed (ms)


// =============================================================================
// Driver Tunable Defaults
// =============================================================================
// Seed DriverConfig / the Motor-tab driver settings, persisted to NVS. These are
// legacy stepper-chopper params retained for the shared driver-config plumbing;
// the closed-loop 57AIM servo configures its gains over Modbus and ignores most
// of them, but the struct is still populated + persisted + echoed to the UI. :3
#define DRIVER_DEFAULT_RUN_CURRENT_MA   2000     // mA (default run current)
#define DRIVER_DEFAULT_STALLGUARD_DMA   -64
#define DRIVER_DEFAULT_TOFF             3        // off-time regulation
#define DRIVER_DEFAULT_HOLD_CURRENT_PCT 50       // % of run current while idle
#define DRIVER_DEFAULT_TBL              2        // blank time code (1 = 24 clocks, typical)
#define DRIVER_DEFAULT_STEALTHCHOP      0        // 0 = SpreadCycle (more torque), 1 = quiet
#define DRIVER_DEFAULT_TPWM_THRS        0        // 0 = never auto-switch stealth<->spread
#define DRIVER_DEFAULT_HSTART           5        // chopper hysteresis start
#define DRIVER_DEFAULT_HEND             1        // chopper hysteresis end


// =============================================================================
// Serial Control Mode
// =============================================================================
// This flag no longer gates runtime input — the active transport (WS/SER/BT
// selected in the web UI and persisted to NVS) is the single source of truth.
// SERIAL_CONTROL_MODE is now used ONLY to:
//   1. Set the factory-default transport mode (1 → SER, 0 → WS).
//   2. Announce serial-control status via /api/status → serial_mode.
// It NO LONGER gates debug output — that is runtime now: the SlopLog serial
// sink mutes itself only while serial TCode traffic is actively flowing
// (applogSerialDedicated ← serialTransport.isActive()), so the Intiface
// stream stays clean exactly when it exists and logs flow every other
// moment. (The old compile-time exclusion silenced ALL serial logging — a
// diagnosed field incident. Don't bring it back.)
#define SERIAL_CONTROL_MODE     1            // 1 = USB Serial is default transport, 0 = WiFi
#define SERIAL_CONTROL_BAUD     115200       // must match Intiface's serial port

// =============================================================================
// Buttplug WebSocket Port
// =============================================================================
// Port the ESP32 runs its OWN WebSocket server on (for MultiFunPlayer, which
// connects TO the device and streams raw TCode).
#define BUTTPLUG_WEBSOCKET_PORT 55555

// =============================================================================
// Intiface WSDM (Websocket Device Manager) Client
// =============================================================================
// When Intiface's "Device WebSocket Server" toggle is ON, Intiface LISTENS for
// devices to connect to IT. The ESP32 connects out to this host:port as a
// WebSocket CLIENT, sends the identification handshake, then exchanges TCode.
//
// Set INTIFACE_HOST to the IP of the PC running Intiface, and INTIFACE_PORT to
// the port shown in Intiface's log ("Listening on: 0.0.0.0:<port>"). The port
// can change between Intiface launches - update it or set via the web UI.
#define INTIFACE_HOST          SECRET_INTIFACE_HOST  // <-- from secrets.h
#define INTIFACE_PORT          SECRET_INTIFACE_PORT  // <-- from secrets.h
#define INTIFACE_ENABLED       true             // enable WSDM client connection

// Identification handshake sent as the FIRST message after connecting.
// identifier: must match the protocol selected in Intiface's websocket device
//             dropdown. For TCode v0.3 stroker this is "tcode-v03".
// address:    arbitrary unique string to identify this device across sessions.
#define INTIFACE_IDENTIFIER    "tcode-v03"
#define INTIFACE_ADDRESS       "slopdrive32-0001"

// TCode magnitude scaling — DEPRECATED, NO LONGER USED IN THE DECODE PATH. :3
//
// Old assumption (WRONG): Intiface emits the magnitude against a fixed 0–999
// scale unpadded ("L086" = 86/999), so we divided by this constant. That broke
// the moment a sender used more (or fewer) digits — a 5-digit value like 50000
// got divided by 999 = 50.0, clamped to 1.0, and every fast high-precision
// stroke slammed into the wall.
//
// Correct TCode v0.3 (what we do now, in TCodeParser): the magnitude is an
// IMPLICIT DECIMAL FRACTION of arbitrary length — strip the axis+channel, then
// treat the entire remaining digit string as if prefixed with "0." So:
//   L0500    → 0.500       (leading zeros are decimal placeholders!)
//   L0086    → 0.086
//   V0010000 → 0.010000
// The divisor is 10^(digit count), never a fixed magic number, which is
// immune to Intiface's occasional extra-leading-zero padding glitch. Kept here
// only because docs / the Intiface device-config JSON still reference the
// historical [0,999] range. Don't wire it back into the parser. :3
#define TCODE_MAGNITUDE_MAX    999.0f


// Max fractional digits we KEEP when decoding a TCode v0.3 magnitude. The spec
// puts no ceiling on how many digits a sender can cram after the channel
// (L0500 = 0.5, L050000 = 0.5 too — just more precision), so we truncate
// anything past this. 6 digits = ~1 part in a million, comfortably finer than a
// float's ~7 significant figures and WAY finer than the stepper can physically
// resolve. Capping here also keeps mag_value safely inside uint32 no matter how
// long a greedy app makes the value. Take six, spit the rest — that's all this
// good boy can swallow without overflowing. :3
#define TCODE_MAGNITUDE_MAX_DIGITS  6



// =============================================================================
// Transport Mode — three ways to get dirty talk into this machine. :3
// =============================================================================
// The machine can receive TCode over three transports. Exactly ONE is active
// at a time; the user picks it in the web UI (Settings) and we remember it
// in NVS (we're loyal like that). The status chip shows WS / SER / BT.
//   WS  = WebSocket. The ESP32 runs a TCode WebSocket server (MultiFunPlayer
//         connects in) AND can connect out to Intiface's WSDM server. Needs WiFi.
//         The social butterfly of transports — always making new connections.
//   SER = USB Serial. Intiface's "serialport" comm manager streams TCode over
//         USB. Lowest latency, no WiFi needed. The direct, no-nonsense top.
//   BT  = Bluetooth LE. The ESP32 advertises a Nordic-UART-style BLE service;
//         a BLE host (phone app / Intiface BLE) writes TCode to the RX
//         characteristic. Wireless, personal, intimate. No WiFi needed.
enum class TransportMode : uint8_t {
    WS      = 0,
    SER     = 1,
    BT      = 2,
    OSSM_BLE = 4,
    // DONGLE: T-Dongle C5 connected via hardware UART (Serial2).
    // The dongle receives TCode over USB from MFP and relays it to the S3
    // over a physical UART wire — TX/RX on pins defined below. This lets
    // MFP talk to the dongle's USB port while the S3 stays on WiFi for the
    // web UI. The dongle is basically a wireless-capable USB-to-UART bridge
    // that also shows a pretty display. yippie! :3
    DONGLE = 3,
};

// Default transport on a fresh device (before any saved selection). SER keeps
// backwards-compatible behavior with the old SERIAL_CONTROL_MODE build flag.
#ifndef DEFAULT_TRANSPORT_MODE
  #if SERIAL_CONTROL_MODE
    #define DEFAULT_TRANSPORT_MODE  TransportMode::SER
  #else
    #define DEFAULT_TRANSPORT_MODE  TransportMode::WS
  #endif
#endif

// =============================================================================
// Dongle UART Transport — Serial2 on the ESP32-S3
// =============================================================================
// The T-Dongle C5 relays TCode from MFP over a physical UART wire to the S3.
// On the custom v0.0 board the relay goes to the onboard C5-Zero over GPIO43/44
// (D1/D0), NOT the old 8/9 — those now belong to the I2C bus (INA228/AS5600).
// Leaving these on 8/9 would fight the current sensor for the same pins. :3
// If TX and RX are swapped, just swap the wires — no firmware change needed. :3
#define DONGLE_UART_TX_PIN      43   // S3 TX → C5 RX (D1/TX)  [was GPIO8 on old PCB]
#define DONGLE_UART_RX_PIN      44   // S3 RX ← C5 TX (D0/RX)  [was GPIO9 on old PCB]

// 460800 baud: each byte takes ~22µs vs 87µs at 115200. A 12-byte TCode frame
// arrives in ~260µs instead of ~1ms — cuts per-frame UART jitter by ~4×.
// The dongle firmware must match this baud or every byte will be garbage. :3
#define DONGLE_UART_BAUD        460800

// =============================================================================
// Bluetooth LE (BLE) Transport
// =============================================================================
// Advertised device name (what shows up in a BLE scanner / Intiface).
#define BLE_DEVICE_NAME        "SlopDrive-32"
// Nordic UART Service (NUS) UUIDs - the de-facto "BLE serial" profile. A host
// writes TCode bytes to the RX characteristic; we notify TCode replies on TX.
#define BLE_NUS_SERVICE_UUID   "8a846175-ea22-4411-88f5-9a8afcc20671"
#define BLE_NUS_RX_CHAR_UUID   "8a846175-ea22-4411-88f5-9a8afcc20672"  // host -> device (write)
#define BLE_NUS_TX_CHAR_UUID   "8a846175-ea22-4411-88f5-9a8afcc20673"  // device -> host (notify)


// =============================================================================
// HTTP Server Port
// =============================================================================

#define UI_WS_PORT              81           // binary WebSocket UI control plane
#define SLOPSYNC_WS_PORT        82           // SlopSync hub transport (binary WS, slopsync/1)
// NOTE: WiFi power-save (WIFI_PS_*) is never touched anywhere in this
// firmware — the device is permanently wall-powered via a brick, so there's
// no power budget to protect and toggling PS modes only adds WiFi radio
// latency/jitter. TransportManager::setupWiFi() calls WiFi.setSleep(false)
// once at boot and that's the end of it. :3

#define HTTP_SERVER_PORT        80
#define HTTP_PORT               80           // alias used in main.cpp

// MDNS service name
#define MDNSServiceName         "slopdrive32"

// =============================================================================
// Control Mode
// =============================================================================
enum class ControlMode : uint8_t {
    MANUAL = 0,     // Web UI manual control
    BUTTPLUG = 1    // Buttplug/Intiface WebSocket control
};

// =============================================================================
// Device State for Configuration (persisted to EEPROM/NVS)
// =============================================================================
struct DeviceConfig {
    // Range mapping (mm) - where buttplug 0.0 and 1.0 map to physically
    float min_position_mm;     // default: 0mm (rearmost)
    float max_position_mm;     // default: DEFAULT_MAX_RAIL_MM (forwardmost)

    // Max rail length (mm) — user-set, rail-length-agnostic ceiling. Bounds the
    // homing search sweep and acts as the position ceiling until homing measures
    // the real stroke. Default DEFAULT_MAX_RAIL_MM (500mm). :3
    float max_rail_mm;         // default: 500mm

    // Speed limit (mm/s) - legacy; migrated to input_max_speed_mm_s on save
    float max_speed_mm_s;      // default: 550

    // Acceleration (mm/s^2) - legacy; migrated to input_max_accel_mm_s2 on save
    float acceleration_mm_s2;  // default: 1500

    // Dual limit sets (v0.4 / D4 Phase 3)
    // USER set: manual moves, UI controls, rail tap, nudges, window-entry glide
    float user_max_speed_mm_s;     // default: 50 (gentle)
    float user_max_accel_mm_s2;    // default: 200 (gentle)
    // INPUT set: TCode streams, PatternEngine, OSSM
    float input_max_speed_mm_s;    // default: 550
    float input_max_accel_mm_s2;   // default: 1500

    // Control mode
    uint8_t control_mode;      // 0=Manual, 1=Buttplug

    // Driver settings (live-tunable from the Motor tab)
    uint16_t microsteps;       // 1/2/4/8/16/32/64/128/256 - smoothness vs torque
    uint16_t run_current_ma;   // motor RMS current while moving (torque + heat)
    uint8_t  hold_current_pct; // % of run current when idle (holding torque/heat)
    int8_t   stallguard_dma;   // StallGuard threshold (sensorless load detect)
    uint8_t  toff;             // chopper off-time / driver enable (1-15, 0=off)
    uint8_t  tbl;              // chopper blank time code 0-3 (16/24/36/54 clocks)
    uint8_t  stealthchop;      // 1 = StealthChop (quiet), 0 = SpreadCycle (torque)
    uint32_t tpwm_thrs;        // TSTEP velocity threshold to flip stealth->spread
    int8_t   hstart;           // chopper hysteresis start (0-7)
    int8_t   hend;             // chopper hysteresis end (-3..12)

    // Manual mode settings
    float manual_depth;        // 0.0-1.0 position within trimmed range
    float manual_speed;        // 0.0-1.0 speed ratio

    // NVS corruption defense: FNV-1a hash over the raw stored config values,
    // computed by ConfigStore::save() (stored under "cfg_crc") and verified by
    // ConfigStore::load() — a mismatch is logged as possible corruption. This
    // field mirrors the last computed hash for diagnostics. :3
    uint32_t checksum;
};

// Default configuration values
inline DeviceConfig getDefaultConfig() {
    DeviceConfig cfg;
    cfg.min_position_mm = 0.0f;
    cfg.max_rail_mm     = DEFAULT_MAX_RAIL_MM;     // agnostic default rail ceiling (500mm)
    cfg.max_position_mm = DEFAULT_MAX_RAIL_MM;     // seeds the startup range until homing measures
    cfg.max_speed_mm_s = DEFAULT_MAX_SPEED_MM_S;  // 550 mm/s factory default
    cfg.acceleration_mm_s2 = DEFAULT_ACCEL_MM_S2;
    // Dual limit sets. USER set defaults gentle (glide-into-window + manual);
    // INPUT set seeds from the legacy full-speed default (streams/patterns). :3
    cfg.user_max_speed_mm_s   = DEFAULT_USER_MAX_SPEED_MM_S;   // 50 mm/s
    cfg.user_max_accel_mm_s2  = DEFAULT_USER_ACCEL_MM_S2;      // 200 mm/s²
    cfg.input_max_speed_mm_s  = DEFAULT_MAX_SPEED_MM_S;
    cfg.input_max_accel_mm_s2 = DEFAULT_ACCEL_MM_S2;
    cfg.control_mode = (uint8_t)ControlMode::BUTTPLUG;
    cfg.microsteps = 16;
    cfg.run_current_ma = DRIVER_DEFAULT_RUN_CURRENT_MA;
    cfg.hold_current_pct = DRIVER_DEFAULT_HOLD_CURRENT_PCT;
    cfg.stallguard_dma = DRIVER_DEFAULT_STALLGUARD_DMA;
    cfg.toff = DRIVER_DEFAULT_TOFF;
    cfg.tbl = DRIVER_DEFAULT_TBL;
    cfg.stealthchop = DRIVER_DEFAULT_STEALTHCHOP;
    cfg.tpwm_thrs = DRIVER_DEFAULT_TPWM_THRS;
    cfg.hstart = DRIVER_DEFAULT_HSTART;
    cfg.hend = DRIVER_DEFAULT_HEND;
    cfg.manual_depth = 0.5f;
    cfg.manual_speed = 0.3f;
    return cfg;
}

// Compute usable travel range in mm
inline float getUsableRange(const DeviceConfig& cfg) {
    return cfg.max_position_mm - cfg.min_position_mm;
}

// Map a normalized value (0.0-1.0) to physical position in mm
inline float mapToPosition(float normalized, const DeviceConfig& cfg) {
    normalized = constrain(normalized, 0.0f, 1.0f);
    return cfg.min_position_mm + normalized * getUsableRange(cfg);
}

// Map a physical position (mm) to normalized value (0.0-1.0)
inline float mapFromPosition(float pos_mm, const DeviceConfig& cfg) {
    float range = getUsableRange(cfg);
    if (range <= 0.0f) return 0.0f;
    return constrain((pos_mm - cfg.min_position_mm) / range, 0.0f, 1.0f);
}

// Map normalized speed to actual mm/s
inline float mapToSpeed(float normalized, const DeviceConfig& cfg) {
    normalized = constrain(normalized, 0.0f, 1.0f);
    return normalized * cfg.max_speed_mm_s;
}