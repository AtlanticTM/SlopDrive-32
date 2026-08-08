#pragma once

// config_api.h â€” board pins, geometry/motion-limit macros, and the persisted
// DeviceConfig struct for the AIMServo build (DRIVER_AIM_SERVO).
//
// Constraints:
// - FIRMWARE_VERSION is bumped by hand every deploy; it is the single source
//   of truth for "which build is running" (/api/capabilities fw_version +
//   boot log).
// - AIM_STEPS_PER_MM is a RUNTIME value once DRIVER_AIM_SERVO is defined (the
//   drive's e-gear steps/rev register 0x0B is reprogrammable over Modbus) â€”
//   go through aimStepsPerMm()/aimGeometryInit(), never treat the _DEFAULT
//   macros as live.
// - DEFAULT_MAX_RAIL_MM only seeds state.config.max_rail_mm; the runtime
//   ceiling is that user setting, and homing's MEASURED stroke wins once it
//   has run.
// - NORMAL_*/EXPERT_* speed/accel/jerk ceilings are UI-only guardrails; the
//   firmware itself always accepts anything up to the hard MAX_* ceiling.
// - DEFAULT_INPUT_MAX_JERK_MM_S3 (planner jerk, mm-domain) and
//   AIM_MODBUS_JERK_MM_S3 (the Modbus executor's own target-tracker glide
//   limit) sit on two different pipeline stages â€” never conflate or equalize
//   them.
// - TCODE_MAGNITUDE_MAX / TCODE_MAGNITUDE_MAX_DIGITS are dead: the TCode
//   parser is retired (SlopSync is the only wire protocol). Kept only because
//   docs / the Intiface device-config JSON still reference the historical
//   [0,999] range â€” do not wire either back into a parser.
// - SLOPSYNC_WS_SUBPROTOCOL has exactly one definition, shared by the
//   transport's bind and its log line, so the two cannot drift apart.

#include <Arduino.h>

// ---- Secrets ----------------------------------------------------------------
// Kept OUT of git. Real values live in include/secrets.h (git-ignored); if
// that file is missing (e.g. a fresh clone before secrets.example.h is
// copied), we fall back to harmless placeholders so the project still
// compiles.
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
// SECRET_OTA_PASSWORD. Fall back to an empty string â€” OtaService treats an
// empty password as "HTTP OTA hard-refused + ArduinoOTA unauthenticated" and
// logs a loud warning, so a stale secrets.h fails safe rather than silently
// exposing an open flash endpoint. Copy the new line from secrets.example.h.
#if !defined(SECRET_OTA_PASSWORD)
  #define SECRET_OTA_PASSWORD    ""
#endif

// ---- Firmware version -------------------------------------------------------
// Bumped by hand on each firmware change so an OTA can be verified as landed
// (surfaced via /api/capabilities â†’ "fw_version" and the boot log). This is
// the single source of truth for "which build is actually running."
#define FIRMWARE_VERSION        "2.4.99"

// ---- WiFi Configuration (values come from secrets.h) ------------------------
#define WIFI_SSID      SECRET_WIFI_SSID
#define WIFI_PASSWORD  SECRET_WIFI_PASSWORD

// Per-credential-set connect timeout at boot. setupWiFi() tries the primary
// (secrets.h) creds for this long, then the NVS-stored secondary creds for the
// same window, before falling back to the serial rescue path (SlopSync needs
// WiFi; there is no other control plane). 10s is enough for a normal WPA2
// associate + DHCP without stalling boot for a network that isn't there.
// Boot-only blocking â€” never hit on the real-time path.
#define WIFI_CONNECT_TIMEOUT_MS   10000

// ---- Boot-time / reconnect strongest-AP selection ---------------------------
// Our deployment is a multi-AP network sharing ONE SSID. The ESP32 default
// fast-scan latches onto the first-heard AP (often the weakest) and never
// roams. The rig is stationary during use, so a full scan + strongest-BSSID
// pin at every WiFi bring-up (cold boot AND every reconnect-from-disconnected
// cycle) is the complete fix. See WifiLink::_connectBest().
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
// task (each cycle blocks ~scan + connect-wait).
#define WIFI_RECONNECT_INTERVAL_MS  5000

// ---- Device Geometry -- AIMServo build (DRIVER_AIM_SERVO) -------------------
// 57AIM30 closed-loop servo drive on a CAPSTAN DRUM (custom v0.0 controller).
// The Dyneema line wraps the drum, so linear travel = drum circumference per
// drum revolution â€” no belt teeth, no backlash.
//
// Drive train math (motor -> drum through a 2:1 reduction):
//   MOTOR_STEPS_PER_REV: 2048  (drive e-gear reg 0x0B, at the motor shaft)
//   REDUCTION:           2.0   (drum turns once per two motor revs â€” motor->drum)
//   STEPS_PER_REV:       2048 Ã— 2 = 4096 steps per DRUM revolution
//   DRUM_DIAMETER:       25.0 mm  â†’ circumference = Ï€ Ã— 25 = 78.5398 mm/drum-rev
//   STEPS_PER_MM:        4096 / 78.5398 = ~52.152 steps/mm
//   MAX_TRAVEL:          rail-length agnostic â€” there is NO fixed geometry
//                        ceiling. The user's configured max rail length
//                        (DEFAULT_MAX_RAIL_MM, runtime-set) bounds the homing
//                        search sweep, and sensorless homing MEASURES the real
//                        usable stroke between the two hard stops.
//   HOMING_BACKOFF:      10.0 mm â€” pull out 10mm after the stall so the carriage
//                        isn't parked against the hard stop.
//
// Keep AIM_STEPS_PER_MM a FLOAT â€” 20.372 truncated to an int would slowly drift
// the carriage off by mm over a long stroke.
//
// RUNTIME GEOMETRY: the AIM drive's steps/rev is an electronic-gear register
// (0x0B) reprogrammable over RS485 Modbus, so steps/mm is a RUNTIME value â€”
// seeded from NVS at boot (default below), recalculated live when the drive is
// reprogrammed from the Configure pane. Every consumer goes through
// aimStepsPerMm(); a steps/rev change forces a re-home (the step<->mm mapping
// of the current position reference is void), no reboot required.
// Use ONLY values that divide AIM_ENC_COUNTS_PER_REV (32768) exactly â€” 1024,
// 2048, 4096, 8192. A non-integer counts-per-pulse (800 -> 40.96) forces the
// drive's e-gear to carry a fractional remainder across pulses.
#define AIM_MOTOR_STEPS_PER_REV_DEFAULT 2048                              // @ motor shaft (drive reg 0x0B)
#define AIM_REDUCTION             2.0f                                    // 2:1 motor -> drum
#define AIM_DRUM_DIAMETER_MM      25.0f
#define AIM_MM_PER_REV            (3.14159265f * AIM_DRUM_DIAMETER_MM)    // 78.5398 mm/drum-rev
#define AIM_STEPS_PER_MM_DEFAULT  ((AIM_MOTOR_STEPS_PER_REV_DEFAULT * AIM_REDUCTION) / AIM_MM_PER_REV)  // ~20.372
#define AIM_HOMING_BACKOFF_MM     10.0f
// Front-end safety margin subtracted from the measured stroke. The stall
// positions are FAS *commanded* counters captured after the confirm window
// (4 polls @150Hz) + INA228 averaging lag â€” the carriage is already parked
// against the wall while ~1-3mm of phantom steps keep counting, at BOTH ends.
// Backoff only accounts for the rear (home) side; this margin keeps full
// extension off the FRONT hard stop instead of commanding into it.
#define AIM_HOMING_FRONT_MARGIN_MM 5.0f

// ---- Encoder cross-check (FAS commanded vs drive-reported position) ---------
// The AIM encoder is 15-bit absolute, 32768 counts per MOTOR rev (fixed silicon
// resolution â€” independent of the e-gear steps/rev, so this ratio never changes
// when the drive is reprogrammed). counts/mm â‰ˆ 834.4 on the 25mm drum @ 2:1.
#define AIM_ENC_COUNTS_PER_REV    32768.0f
#define AIM_ENC_COUNTS_PER_MM     ((AIM_ENC_COUNTS_PER_REV * AIM_REDUCTION) / AIM_MM_PER_REV)
// Standstill deviation beyond this (for 3 consecutive steady samples) raises
// the lost-steps warning. ~1.5mm â‰ˆ 78 steps @ 52.15 steps/mm â€” far above noise,
// far below anything that could hurt. Report-only: it never gates motion.
#define AIM_ENC_DEV_WARN_MM       1.5f
// Excursion (mm of FAS travel) needed before the validator trusts a measured
// encoder direction + scale and starts scoring deviation.
#define AIM_ENC_SIGN_DETECT_MM    8.0f

#if defined(DRIVER_AIM_SERVO)
// Runtime geometry accessors â€” defined in src/system/MotionGeometry.cpp.
// aimGeometryInit() loads the persisted motor steps/rev from NVS (namespace
// "servocfg") in setup(); aimSetMotorStepsPerRev() recomputes steps/mm live
// (32-bit float store is atomic on Xtensa â€” Core 1 sees old or new, never torn).
void     aimGeometryInit();
void     aimSetMotorStepsPerRev(uint16_t steps_per_rev, bool persist);
uint16_t aimMotorStepsPerRev();          // per MOTOR rev (mirrors drive reg 0x0B)
int32_t  aimStepsPerRev();               // per DRUM rev (motor Ã— reduction)
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
// steps/mm â€” same crawl, now invariant). forceStopAndNewPosition() kills the
// pulse train the instant we detect the current spike.
#define AIM_HOMING_SPEED_MM_S     29.5f
#define AIM_HOMING_SPEED_STEPS_S  ((int32_t)(AIM_HOMING_SPEED_MM_S * AIM_STEPS_PER_MM))

// ---- Sensorless homing tunables (INA228 current-stall detection) ------------
// The new PCB has NO endstop switch â€” we feel our way to the hard stop by
// watching motor current on the INA228. Free travel draws low single-digit
// amps; when the carriage buries itself against the stop the current climbs
// fast toward the drive's limit. We call it a stall when current sits above the
// free-run baseline by STALL_MARGIN_A for STALL_CONSEC consecutive polls.
// Tune these empirically on the real machine â€” start gentle, tighten later.
#define AIM_HOME_STALL_MARGIN_A     3.0f   // amps above free-run baseline = stall
#define AIM_HOME_STALL_CONSEC       4      // consecutive over-threshold samples
#define AIM_HOME_POLL_HZ            150    // INA228 poll rate during homing (Hz)
#define AIM_HOME_BASELINE_SAMPLES   20     // samples averaged for the free-run baseline
// A stall debounced this far into the search sweep is far more likely "ran out
// of search distance" (a sustained current glitch â€” foldback, an alarming
// drive on an open phase, an unplugged motor reading garbage off a separate
// I2C device) than "found a wall". A REAL wall is always found well inside the
// configured rail length; only a fault rides the sweep out to its bound. Below
// this fraction of the full 1.2x-rail search sweep, a debounced stall is
// trusted; at/above it, homing REJECTS the stall and fails safe exactly like
// "no stall found at all" (see _sweepToStall()/`_homingTask()`'s FAILED path).
#define AIM_HOME_STALL_PLAUSIBLE_FRAC 0.90f

// ---- Modbus direct-drive backend tunables (Phase 3) -------------------------
// Every value here is measured; rationale in docs/reversal-drift.md.
// 0x7B cadence, moving / idle. Lag is FLAT 43Hz..320Hz, so 100Hz costs nothing
// and leaves the SHARED wire room for the telemetry rotation.
#define AIM_SP_PERIOD_MS            10
#define AIM_SP_KEEPALIVE_MS         250
// DEAD-LINK detector, not a freshness target. Sized well above the slowest
// legitimate encoder rate so a busy bus never reads as a dead one (T16).
#define AIM_ENC_STALE_FREEZE_MS     2000
#define AIM_ENC_STALE_ESTOP_MS      5000
// Homing wall detector: encoder-behind-setpoint, mm, and its debounce.
#define AIM_MODBUS_HOME_STALL_MM    4.0f
#define AIM_MODBUS_HOME_STALL_CONSEC 3
// Detector arming: shaft travel that counts as "started moving", and the
// ceiling on waiting for it. Unarmed, start-of-move lag reads as a wall.
#define AIM_MODBUS_HOME_MOVED_MM    1.0f
#define AIM_MODBUS_HOME_GRACE_MS    600
// Drive built-in home (reg 0x19 = 1): how long to wait for the shaft to start,
// and how long it must sit still before the cycle counts as finished.
// Arrival is |0x0C/0x0D| (steps-to-go) under this, an ossm-rs positive signal.
// It replaces waiting out a stillness window, which cost 5-10s per home.
#define AIM_DRIVE_HOME_ARRIVE_CNT   15
// Homing runs at reduced torque/speed (0x18 / 0x02) for a gentle approach.
#define AIM_DRIVE_HOME_TORQUE       89
#define AIM_DRIVE_HOME_SPEED_RPM    80
#define AIM_DRIVE_HOME_START_MS     3000
// MEASURED: the drive's cycle DWELLS mid-run (approach, pause, reverse off the
// stop). At 700ms this latched during that pause, so the executor was seeded
// early and pinned the carriage before the drive could back off. Two runs of
// the same command settled at 1121ms and 8021ms; only the second was real.
#define AIM_DRIVE_HOME_STILL_MS     2500
#define AIM_DRIVE_HOME_TIMEOUT_MS   60000
// Homing sweep accel, mm/s^2. Gentle on purpose: the sweep must reach steady
// state fast enough to be quick, slow enough that ramp lag is not a wall.
#define AIM_MODBUS_HOME_ACCEL_MM_S2 500.0f
// Arm-sequence register set. Speed and accel are deliberately WIDE OPEN: the
// stream is already jerk-limited, and clamping the drive would flatten the
// script instead of smoothing it (operator ruling, sd-s37).
#define AIM_MODBUS_ARM_SPEED_RPM    1000
#define AIM_MODBUS_ARM_ACCEL        50000
#define AIM_MODBUS_ARM_SPEED_P      3000
#define AIM_MODBUS_ARM_POS_P        3000
// Wire-mapping sign: wire_counts = wire_offset + AIM_MODBUS_WIRE_SIGN * cmd_counts.
// The drive's own absolute encoder frame (regs 0x16/0x17) has UNKNOWN sign
// relative to the arbiter's home=0/front=negative convention until confirmed
// on the bench â€” this is a PLACEHOLDER default. First bench step: force-home,
// jog a small positive mm move, watch which way the encoder count actually
// moves, and flip this if it's backwards.
// Jerk ceiling (mm/s^3) for the Modbus executor's jerk-limited target tracker
// â€” OSSM-RS parity (their Ruckig streams at MAX_JERK = 100000 mm/s^3). The
// tracker glides toward every target under vmax/amax/jmax; this is what
// killed the "clocked waypoint" texture of raw trapezoid streaming.
#define AIM_MODBUS_JERK_MM_S3       100000.0f
// BENCH-DETERMINED (fw 2.1.27, first live jog): +1 ran the carriage the wrong
// way â€” a positive-depth command must move the same physical direction as the
// FAS build's negative-step convention, and on this wiring that is encoder
// NEGATIVE. Flipped to -1 and verified by the operator.
#define AIM_MODBUS_WIRE_SIGN        (-1)
// Standstill max-output written to drive reg 0x18 whenever Modbus-mode
// applies its expected register set. BENCH-LEARNED ENCODING (fw 2.1.24): the
// RAW register packs PWM*10 + alarm-mode digit â€” factory value reads 600
// (= PWM 60, alarm 0), and OSSM-RS's "12-60" range describes the DECODED PWM
// field, not the raw register. Writing a bare 20 here (= PWM 2) made the
// standstill hold too weak to feel reliable. 600 =
// factory full hold. Lower for squish-safety as PWM*10 (e.g. 200 = PWM 20).
#define AIM_MODBUS_STANDSTILL_MAX   600


// ---- DEFAULT_MAX_RAIL_MM -- rail-length-agnostic default ceiling ------------
// This firmware is agnostic to the physical length of the rail â€” you can run
// it on a machine of ANY stroke. There is NO hardcoded geometry ceiling. The
// "max rail length" is a RUNTIME user setting (state.config.max_rail_mm,
// editable in the WebUI and persisted to NVS) whose only jobs are:
//   1. Bound the sensorless homing search sweep so homing can't hunt forever
//      when a wall is never felt (electrical/mechanical fault).
//   2. Serve as the position ceiling BEFORE homing has measured the real
//      stroke. Once homing feels out both hard stops, the MEASURED stroke is
//      the source of truth and governs the usable range (measurement wins).
// This macro is only the factory default that seeds that setting â€” 500mm is a
// sane, generous rail length. The AIM drive-train constants above (steps/mm,
// drum geometry) remain purely for the motion math.
#define DEFAULT_MAX_RAIL_MM  500.0f

// ---- Motor Driver Pins (ESP32-S3) -------------------------------------------
// ---- AIMServo build (DRIVER_AIM_SERVO) -- Nano ESP32 v0.0 controller --------
// New board routes the servo drive through an SN74AHCT125 buffer -> opto inputs.
// PUL â†’ GPIO 5 (D2), DIR â†’ GPIO 6 (D3). No endstop on this board â€” homing is
// sensorless via the INA228 current sensor (see below). The old GPIO12 endstop
// is kept only for the legacy HOMING_USE_ENDSTOP fallback.
//
// The AHCT125's output-enable is tied LOW (always on), so ANY boot glitch on
// PUL/DIR squirts straight through to the motor's opto inputs â€” pull both LOW
// as early as possible in setup(), before any premature motion.
#define AIM_PIN_STEP            5    // PUL â€” pulse train to the servo drive (D2)
#define AIM_PIN_DIR             6    // DIR â€” direction signal (D3) [was GPIO4 on old PCB]
#define AIM_PIN_ENDSTOP         12   // Legacy endstop (only used if HOMING_USE_ENDSTOP)

// Quiet pause FAS inserts between the DIR toggle and the first step of a
// reversal (FAS dir_change_delay_us). On the MCPWM backend this is the ONLY
// pause at a reversal â€” FAS's own pre-toggle quiet is zero there â€” so it is
// the whole window the DIR edge has to sit in, together with the PCNT step-edge
// retime in AIMServoDriver::init(). Do not set to 0; 200 covers the drive's 5us
// setup spec with margin. Do not set 1..199 either: ESP32's MIN_DIR_DELAY_US is
// 200 and FAS clamps up to it silently. Only 200..4095 are real values.
#define AIM_DIR_CHANGE_DELAY_US 200

// ---- Step-pulse backend + PCNT retime â€” the DIR ladder's two switches -------
// One token each, so a ladder rung is a single edit and a rebuild. The rungs
// and what each one proves live on the dev board (dir-ladder issues).
//   AIM_FAS_BACKEND      0 = MCPWM_PCNT, 1 = RMT. Matches FasDriver's own
//                        numbering; AIMServoDriver logs which one FAS actually
//                        handed back, and that log is the ground truth.
//   AIM_MCPWM_PCNT_RETIME 1 = count FALLING step edges so the DIR toggle lands
//                        in the reversal dwell instead of inside the last
//                        pulse. 0 = library behavior (rising edges). Ignored
//                        entirely on the RMT backend.
#define AIM_FAS_BACKEND         0
#define AIM_MCPWM_PCNT_RETIME   1

// ---- FAS pipeline depth â€” how stale a stream sample is when it lands --------
// streamSamplerTask produces a micro-target every ~1 ms. FAS consumes on its
// own cadence, and both library defaults are sized for discrete moves, not for
// a stream:
//   task_rate       4 ms  -> the 1 kHz sample stream is decimated to 250 Hz
//   forward plan   20 ms  -> the queue is committed 20 ms ahead of the target
// So the machine executes a plan built from a sample up to 20 ms old, refilled
// 250 times a second. Match the consumer to the producer instead.
//
// CONSTRAINT: plan-ahead must stay comfortably ABOVE the task rate or the queue
// runs dry mid-move and the motor stops dead â€” FastAccelStepperEngine.h states
// this outright. Keep the 4:1 ratio the library ships if these get swept.
//
// NOT FREE ON MCPWM: a shorter task rate runs `addQueueEntry`'s
// fasDisableInterrupts() window more often, and that is the exact thing that
// delays the PCNT ISR (motion-control.md, MCPWM traps) -- sweep these and the
// DIR ladder separately or neither result means anything.
#define AIM_FAS_TASK_RATE_MS    1
#define AIM_FAS_PLAN_AHEAD_MS   4

// ---- I2C bus -- INA228 current sensor @ 0x40, AS5600 encoder (deferred) -----
// Nano ESP32 does NOT default I2C to these pins â€” call Wire.begin(SDA, SCL)
// explicitly. INA228 lives behind an ISO1640 isolator but is transparent to
// software.
#define PIN_I2C_SDA             8    // D5
#define PIN_I2C_SCL             9    // D6

// INA228 high-side current/voltage monitor on the 36V bus. Register map differs
// from the INA226 â€” use an INA228-specific library. 5mÎ© shunt, ADCRANGE=0.
#define INA228_I2C_ADDR         0x40
#define INA228_SHUNT_OHMS       0.005f   // 5 mÎ©, 2W
#define INA228_MAX_CURRENT_A    32.768f  // ADCRANGE=0 full scale (163.84mV / 5mÎ©)

// ---- RS485 / Modbus to motor -- LIVE ----------------------------------------
// XY-G485 on Serial1, 19200 8N1. Never reassign: RS485 stays on the S3.
#define AIM_PIN_485_TX          17   // D8
#define AIM_PIN_485_RX          18   // D9

// ---- Status LEDs ------------------------------------------------------------
// Discrete LEDs, not a NeoPixel. ACTIVE-LOW: drive the pin LOW to light it.
// GPIO48 is NOT an LED here. It is S3_DISP_SCK, owned by the RP2350 SPI clock
// (sd-dxy); the module's own D13 LED on that pin now reads as bus activity.
#define PIN_LED_R               46
#define PIN_LED_G               0    // strapping pin â€” init after boot
#define PIN_LED_B               45
#define LED_ACTIVE_LOW          1    // onboard LEDs sink current â€” LOW = lit
#define PIN_HB_LED              21   // heartbeat, ACTIVE-HIGH (PCB yellow-green LED, D10)
#define HB_LED_ACTIVE_HIGH      1

// ---- Motor Defaults ---------------------------------------------------------
// Maximum motor speed in mm/s.
// Normal UI cap: 5000 mm/s. Expert mode UI cap: 10000 mm/s.
// This firmware ceiling is set to 10000 so expert mode values aren't rejected
// by the ConfigStore validator. The WebUI enforces the normal/expert split.
// The 57AIM servo drive at 800 steps/rev Ã— 10 steps/mm can push this â€” it's
// a closed-loop servo, not a stepper, so it won't skip steps.
#define MAX_SPEED_MM_S              10000.0f
#define DEFAULT_MAX_SPEED_MM_S      950.0f   // factory default on fresh boot (operator's hardware)

// USER limit-set factory defaults â€” deliberately gentle. The USER set caps the
// operator's own hand (manual moves) AND the "glide into the window" entry move
// when a machine-driven source starts from outside the stroke window. A soft
// 50 mm/s / 200 mm/sÂ² means the carriage eases into the window instead of
// lunging to the edge at the input ceiling. Raise them in Settings once you've
// felt the machine out. Distinct from DEFAULT_MAX_SPEED/ACCEL which seed the
// INPUT set (streams/patterns).
#define DEFAULT_USER_MAX_SPEED_MM_S  50.0f    // gentle HAND-DRIVEN default â€” deliberately NOT the
                                             // master/input pair below. Manual jogging stays slow.
#define DEFAULT_USER_ACCEL_MM_S2     200.0f   // gentle hand-driven default (see above)

// Split ceilings the WebUI's expert-mode toggle switches between. Advertised
// via /api/capabilities so the UI derives its slider `max` attrs from the API
// instead of hardcoding 3000/8000 literals (the "half-updated slider" bug: a
// UI with a stale hardcoded max silently clamps input below the real ceiling).
// Firmware itself always accepts anything up to the hard
// MAX_SPEED_MM_S/MAX_ACCEL_MM_S2 ceiling above â€” these are UI-only guardrails.
//
// Normal mode is confirmed safe up to 1000 mm/s for regular use. Expert mode
// unlocks the full hardware ceiling for those who know what they're asking for.
// The machine advertises these â€” the UI only ever asks, never assumes.
#define NORMAL_MAX_SPEED_MM_S       1000.0f
#define EXPERT_MAX_SPEED_MM_S       MAX_SPEED_MM_S      // 10000

// Default acceleration mm/s^2
// Normal UI cap: 50000 mm/sÂ². Expert mode UI cap: 100000 mm/sÂ².
// The 57AIM servo drive can hit these â€” it's a closed-loop servo that will
// drive the carriage straight into the endstop if commanded to.
// Firmware ceiling is 100000 â€” the WebUI enforces the normal/expert split.
// NOTE: accel is stored as uint32_t in NVS (not uint16_t) â€” values above
// 65535 would silently overflow a uint16_t and corrupt the saved setting.
#define DEFAULT_ACCEL_MM_S2     50000.0f
#define MAX_ACCEL_MM_S2         100000.0f

// Split accel ceilings â€” same expert-mode split as speed above.
// Normal mode confirmed safe to 20000 mm/sÂ²; expert unlocks the full ceiling.
#define NORMAL_MAX_ACCEL_MM_S2      20000.0f
#define EXPERT_MAX_ACCEL_MM_S2      MAX_ACCEL_MM_S2     // 100000

// ---- INPUT-set jerk ceiling (mm/s^3) -- third member of the limit family ----
// The planner (SlopMotion, Â§7.6) is jerk-limited, and until fw 2.1.47 its jmax
// was a BARE NORMALIZED CONSTANT (500 units/s^3) with no mm-domain source. That
// made the PHYSICAL jerk ceiling `500 * window_span` â€” i.e. it SHRANK as the
// operator narrowed the stroke window, which is exactly backwards, and it
// silently bound fast segments long before speed or accel did. Measured on the
// bench: a 200 mm window fed 400 ms funscript segments reached only 49% of the
// commanded stroke depth because JERK (not velocity) was the binding limit;
// raising the effective jerk took the same script to 98%. So jerk gets promoted
// to a first-class, mm-domain, persisted, wire-exposed limit that lives beside
// speed and accel, and main.cpp divides it by the window span exactly like the
// other two. Narrow the window now and the physical ceiling stays put.
//
// WHY THE DEFAULT IS THIS HIGH â€” jerk here is a MECHANICAL PROTECTION limit,
// not a smoothing crutch. The classic reason to hold jerk low is to hide the
// stair-stepping of a clocked position output; this codebase does not have that
// problem, because the Motion Doctrine engineered it out â€” ONE COMMAND â†’ ONE
// PLAN, the trajectory is C2 by construction (quintic Hermite / Ruckig), and
// FastAccelStepper's ISR is the sample rate. Nothing here needs jerk to smooth
// it. What jerk still buys us is protecting the belt, the carriage, and the
// mounting from the shock of a genuinely instantaneous accel change. Set it at
// the mechanical pain threshold and let velocity be the limit that actually
// binds, which is the honest physical one.
//
// Calibration, at the NORMAL 1000 mm/s speed ceiling: the quintic is
// velocity-bound (rather than jerk-bound) for segments longer than ~120 ms at
// 2e6 mm/s^3, and only for segments longer than ~240 ms at 5e5. Funscript
// cadence lives well under 240 ms, which is why 5e5-class ceilings ate stroke
// depth. 2e6 is the "fast script still reaches full depth" default; the normal
// UI cap (1e7) covers aggressive setups, expert unlocks 5e7 for rigid rails.
//
// NOT the same thing as AIM_MODBUS_JERK_MM_S3 (100000, ~line 198): that one is
// the Modbus servo executor's OWN target-tracker glide limit, downstream of the
// planner and specific to that driver's incremental-delta wire protocol. Two
// different limits on two different stages of the pipeline â€” do not conflate
// them, and do not "fix" a mismatch by making them equal.
#define DEFAULT_INPUT_MAX_JERK_MM_S3  2000000.0f   // factory default (2e6)
#define MAX_JERK_MM_S3               50000000.0f   // hard firmware ceiling (5e7)

// Split jerk ceilings â€” same expert-mode split as speed/accel above, advertised
// via /api/capabilities so the UI derives its slider max from the API.
#define NORMAL_MAX_JERK_MM_S3        10000000.0f   // 1e7
#define EXPERT_MAX_JERK_MM_S3        MAX_JERK_MM_S3 // 5e7

// ---- Safe-approach soft start -----------------------------------------------
// Whenever motion (re)engages after a discontinuity â€” a brand-new stream
// connection, un-pausing, turning manual override OFF, the generator starting,
// or a target that jumps from outside the stroke window into it â€” the first
// move can be arbitrarily far from the carriage's current position. Dispatching
// that at the configured max speed produces a sudden, violent lunge.
//
// Instead, for the first SAFE_RESUME_RAMP_MS after (re)engagement the speed
// ceiling ramps linearly from SAFE_APPROACH_SPEED_MM_S up to the configured
// max. The carriage glides to where the stream wants it, THEN opens the
// throttle.
#define SAFE_APPROACH_SPEED_MM_S    100.0f   // initial speed cap on re-engage (mm/s)
#define SAFE_RESUME_RAMP_MS         1200u    // ramp duration back to full speed (ms)


// ---- Driver Tunable Defaults ------------------------------------------------
// Seed DriverConfig / the Motor-tab driver settings, persisted to NVS. These are
// legacy stepper-chopper params retained for the shared driver-config plumbing;
// the closed-loop 57AIM servo configures its gains over Modbus and ignores most
// of them, but the struct is still populated + persisted + echoed to the UI.
#define DRIVER_DEFAULT_RUN_CURRENT_MA   2000     // mA (default run current)
#define DRIVER_DEFAULT_STALLGUARD_DMA   -64
#define DRIVER_DEFAULT_TOFF             3        // off-time regulation
#define DRIVER_DEFAULT_HOLD_CURRENT_PCT 50       // % of run current while idle
#define DRIVER_DEFAULT_TBL              2        // blank time code (1 = 24 clocks, typical)
#define DRIVER_DEFAULT_STEALTHCHOP      0        // 0 = SpreadCycle (more torque), 1 = quiet
#define DRIVER_DEFAULT_TPWM_THRS        0        // 0 = never auto-switch stealth<->spread
#define DRIVER_DEFAULT_HSTART           5        // chopper hysteresis start
#define DRIVER_DEFAULT_HEND             1        // chopper hysteresis end


// ---- Serial Control Mode ----------------------------------------------------
// SlopSync (WiFi) is the only control plane (transport.md). USB Serial
// is boot-log + the rescue/OTA-recovery path only. SERIAL_CONTROL_MODE just
// gates the boot banner and the /api/status â†’ serial_mode diagnostic field.
#define SERIAL_CONTROL_MODE     1            // 1 = boot banner says serial-rescue, 0 = WiFi-only banner
#define SERIAL_CONTROL_BAUD     115200       // USB serial baud (boot log + rescue path)

// ---- Intiface / Buttplug -- REMOVED (M5c, fw 2.1.65) ------------------------
// BUTTPLUG_WEBSOCKET_PORT (55555), INTIFACE_HOST/PORT/ENABLED/IDENTIFIER/ADDRESS
// all lived here. Their one consumer, WebSocketTransport, is deleted: SlopSync
// is now the only input and output on this device, and Intiface is planned to
// gain native SlopSync support rather than the device continuing to speak
// Intiface's protocol.
//
// SECRET_INTIFACE_HOST/PORT survive in secrets.h and secrets.example.h only so
// an existing (git-ignored) secrets.h keeps compiling. Nothing reads them.

// TCode magnitude scaling â€” DEPRECATED, NO LONGER USED IN THE DECODE PATH.
//
// Old assumption (WRONG): Intiface emits the magnitude against a fixed 0â€“999
// scale unpadded ("L086" = 86/999), so we divided by this constant. That broke
// the moment a sender used more (or fewer) digits â€” a 5-digit value like 50000
// got divided by 999 = 50.0, clamped to 1.0, and every fast high-precision
// stroke slammed into the wall.
//
// Correct TCode v0.3 decode (historical â€” the TCode parser is retired, SlopSync
// is the only wire protocol now): the magnitude is an
// IMPLICIT DECIMAL FRACTION of arbitrary length â€” strip the axis+channel, then
// treat the entire remaining digit string as if prefixed with "0." So:
//   L0500    â†’ 0.500       (leading zeros are decimal placeholders!)
//   L0086    â†’ 0.086
//   V0010000 â†’ 0.010000
// The divisor is 10^(digit count), never a fixed magic number, which is
// immune to Intiface's occasional extra-leading-zero padding glitch. Kept here
// only because docs / the Intiface device-config JSON still reference the
// historical [0,999] range. Don't wire it back into the parser.
#define TCODE_MAGNITUDE_MAX    999.0f


// Max fractional digits we KEEP when decoding a TCode v0.3 magnitude. The spec
// puts no ceiling on how many digits a sender can cram after the channel
// (L0500 = 0.5, L050000 = 0.5 too â€” just more precision), so we truncate
// anything past this. 6 digits = ~1 part in a million, comfortably finer than a
// float's ~7 significant figures and WAY finer than the stepper can physically
// resolve. Capping here also keeps mag_value safely inside uint32 no matter how
// long a greedy app makes the value.
#define TCODE_MAGNITUDE_MAX_DIGITS  6



// ---- HTTP Server Port -------------------------------------------------------

#define SLOPSYNC_WS_PORT        82           // SlopSync hub transport (binary WS)
// The negotiated WS subprotocol. ONE definition, used by both the transport's
// bind and its log line, so the two cannot drift apart. A client that does not
// offer exactly this is refused at handshake (RFC 6455 4.2.2) -- see
// lib/espasyncwebserver/VENDORED.md.
#define SLOPSYNC_WS_SUBPROTOCOL "slopsync.v1"
// NOTE: WiFi power-save (WIFI_PS_*) is never touched anywhere in this
// firmware â€” the device is permanently wall-powered via a brick, so there's
// no power budget to protect and toggling PS modes only adds WiFi radio
// latency/jitter. WifiLink::setupWiFi() calls WiFi.setSleep(false)
// once at boot and that's the end of it.

#define HTTP_SERVER_PORT        80
#define HTTP_PORT               80           // alias used in main.cpp

// MDNS service name
#define MDNSServiceName         "slopdrive32"

// ---- Device State for Configuration (persisted to EEPROM/NVS) ---------------
struct DeviceConfig {
    // Range mapping (mm) - where buttplug 0.0 and 1.0 map to physically
    float min_position_mm;     // default: 0mm (rearmost)
    float max_position_mm;     // default: DEFAULT_MAX_RAIL_MM (forwardmost)

    // Max rail length (mm) â€” user-set, rail-length-agnostic ceiling. Bounds the
    // homing search sweep and acts as the position ceiling until homing measures
    // the real stroke. Default DEFAULT_MAX_RAIL_MM (500mm).
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
    // Third member of the INPUT limit family (fw 2.1.47). Feeds SlopMotion's
    // jmax after division by the stroke-window span, exactly like the two
    // above â€” a MECHANICAL protection ceiling, never a smoothing knob.
    float input_max_jerk_mm_s3;    // default: 2000000

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
    // ConfigStore::load() â€” a mismatch is logged as possible corruption. This
    // field mirrors the last computed hash for diagnostics.
    uint32_t checksum;
};

// Default configuration values
inline DeviceConfig getDefaultConfig() {
    DeviceConfig cfg;
    cfg.min_position_mm = 0.0f;
    cfg.max_rail_mm     = DEFAULT_MAX_RAIL_MM;     // agnostic default rail ceiling (500mm)
    cfg.max_position_mm = DEFAULT_MAX_RAIL_MM;     // seeds the startup range until homing measures
    cfg.max_speed_mm_s = DEFAULT_MAX_SPEED_MM_S;  // 950 mm/s factory default
    cfg.acceleration_mm_s2 = DEFAULT_ACCEL_MM_S2;
    // Dual limit sets. USER set defaults gentle (glide-into-window + manual);
    // INPUT set seeds from the legacy full-speed default (streams/patterns).
    cfg.user_max_speed_mm_s   = DEFAULT_USER_MAX_SPEED_MM_S;   // 50 mm/s
    cfg.user_max_accel_mm_s2  = DEFAULT_USER_ACCEL_MM_S2;      // 200 mm/sÂ²
    cfg.input_max_speed_mm_s  = DEFAULT_MAX_SPEED_MM_S;
    cfg.input_max_accel_mm_s2 = DEFAULT_ACCEL_MM_S2;
    cfg.input_max_jerk_mm_s3  = DEFAULT_INPUT_MAX_JERK_MM_S3;
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
