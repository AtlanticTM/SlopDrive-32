// SlopDrive-32 — main.cpp
//
// Thin composition root. All logic lives in the modules under
// system/, motion/, comms/, and ui/. This file only:
//   1. Declares module instances.
//   2. Wires them together in setup().
//   3. Creates FreeRTOS tasks with correct core pinning.
//   4. Idles in loop().
//
// Core assignment (.clinerules §2):
//   Core 1 (real-time):  motorTask, Generator task
//   Core 0 (system):     commsTask, httpTask

#include <Arduino.h>
#include <LittleFS.h>

#include "config_api.h"
#include "AppLog.h"
#include "SystemState.h"
#include "ConfigStore.h"

#include "range_mapper.h"
#include "Kinematics.h"

#if defined(DRIVER_TMC2160)
#include "TMC2160StepperDriver.h"
#endif

#include "Generator.h"

#include "TCodeParser.h"
#include "SerialTransport.h"
#include "WebSocketTransport.h"
#include "BleTransport.h"
#include "TransportManager.h"

#include "WebUI.h"


// ============================================================================
// Module instances — one big orgy of objects :3
// Driver selected at compile time via build flag.
// ============================================================================

#if defined(DRIVER_TMC2160)
  TMC2160StepperDriver motor;
#else
  #error "No motor driver selected. Define DRIVER_TMC2160 (or a future driver flag) in platformio.ini build_flags."
#endif

static SystemState        g_state;
static RangeMapper        mapper;
static Generator          generator(g_state, mapper, motor);

static TCodeParser        tcodeParser;
static SerialTransport    serialTransport(tcodeParser);
static WebSocketTransport wsTransport(tcodeParser);
static BleTransport       bleTransport(tcodeParser);
static TransportManager   transportMgr(g_state, tcodeParser,
                                        serialTransport, wsTransport, bleTransport);

static WebUI webui(g_state, motor, mapper, generator,
                   transportMgr, serialTransport, wsTransport, bleTransport);


// ============================================================================
// Glue callbacks — the good boys sitting between TCode and the motor :3
// ============================================================================

// How long we ease back in after being edged (paused), in ms.
static const uint32_t RESUME_BLEND_MS = 800;

// Called by TCodeParser on every valid L0 linear command.
// This is where the dirty talk becomes actual thrusting. :3
static void buttplugLinearCmd(float position, uint32_t duration_ms) {

    if (!g_state.homed) return;
    if (g_state.paused || g_state.manual_override) {
        g_state.resume_start_ms = millis();
        return;
    }

    g_state.last_intiface_ms = millis();

    // Measure inter-command cadence — always track the real arrival gap so the
    // Hz display and the auto-duration fallback stay calibrated. We do this
    // regardless of whether the command carried an explicit I-duration. :3
    uint32_t now = millis();
    if (g_state.last_cmd_ms != 0) {
        uint32_t gap = now - g_state.last_cmd_ms;
        if (gap > 0 && gap < 1000) {
            if (g_state.measured_interval_ms <= 0.0f)
                g_state.measured_interval_ms = (float)gap;
            else
                g_state.measured_interval_ms =
                    0.7f * g_state.measured_interval_ms + 0.3f * (float)gap;
        }
    }
    g_state.last_cmd_ms = now;

    // Auto Duration: ONLY substitute the measured gap when the TCode command
    // carried NO explicit I-duration (duration_ms == 0). If the host (MFP,
    // Intiface, etc.) sent an explicit interval we MUST honour it — overriding
    // it with our own measurement was the "jitter half a mm" bug: the measured
    // gap was often 10–30ms, which made the planner's distance-clamp fire and
    // reduce every stroke to a sub-millimetre twitch. Trust the host. :3
    if (duration_ms == 0 && g_state.auto_duration && g_state.measured_interval_ms > 1.0f)
        duration_ms = (uint32_t)(g_state.measured_interval_ms + 0.5f);

    float target_mm = mapper.intensityToPosition(position);

    // Stash the raw mapped demand for the UI motion graph (stage 1 of 3:
    // raw → planned → actual). The graph reads this from g_state. :3
    g_state.commanded_raw_mm = target_mm;

    // ---- VELOCITY FEED-FORWARD (lookahead, currently OFF) -------------------
    // Kept as a live knob: LOOKAHEAD_FRAC = 0.0 matches OSSM exactly (lag
    // killed by correct cruise speed, not prediction). Nudge toward 0.3–0.5
    // if residual trail remains; leave at 0 if it overshoots. :3
    static float    s_last_raw_mm   = 0.0f;
    static uint32_t s_last_raw_ms   = 0;
    static bool     s_have_last_raw = false;
    {
        uint32_t t_now = millis();
        if (s_have_last_raw) {
            float dt_ms = (float)(t_now - s_last_raw_ms);
            if (dt_ms > 0.5f && dt_ms < 1000.0f) {
                float vel_mm_per_ms = (target_mm - s_last_raw_mm) / dt_ms;

                const float LOOKAHEAD_FRAC = 0.0f;

                float lead_ms = (g_state.measured_interval_ms > 1.0f)
                                    ? g_state.measured_interval_ms
                                    : dt_ms;
                float predicted = target_mm + vel_mm_per_ms * (lead_ms * LOOKAHEAD_FRAC);

                // Safety leash: never lead more than half the stroke window. :3
                float window   = mapper.getMaxMm() - mapper.getMinMm();
                float max_lead = window * 0.5f;
                predicted = constrain(predicted,
                                      target_mm - max_lead,
                                      target_mm + max_lead);
                target_mm = predicted;
            }
        }
        s_last_raw_mm   = g_state.commanded_raw_mm;
        s_last_raw_ms   = t_now;
        s_have_last_raw = true;
    }

    // ---- Slow resume blend after being edged (paused) -----------------------
    if (g_state.resume_start_ms != 0) {
        uint32_t elapsed = millis() - g_state.resume_start_ms;
        if (elapsed >= RESUME_BLEND_MS) {
            g_state.resume_start_ms = 0;
        } else {
            float blend = (float)elapsed / (float)RESUME_BLEND_MS;
            float cur   = motor.getPosition();
            target_mm   = cur + blend * (target_mm - cur);
        }
    }

    // ---- THE ONE TRUE PATH: trapezoidal planner -----------------------------
    //
    // Chain targets, not positions. Each segment goes from "where I told it to
    // be last time" to "where I'm telling it to go now", at the cruise speed
    // dist/T. The motor's actual position is irrelevant to the planning math —
    // FAS handles the real-world physics. We keep the command chain geometrically
    // consistent so the speed math is always sane. :3
    //
    // On first command (no previous target), fall back to actual position so
    // we don't plan a phantom hop from 0mm. After that, pure target chaining.
    static float s_last_target_mm = -1.0f;  // -1 = "not yet initialized"
    float plan_from_mm = (s_last_target_mm < 0.0f)
                             ? motor.getPosition()
                             : s_last_target_mm;

    auto plan = kinematics::planTrapezoid(
        plan_from_mm, target_mm,
        (float)duration_ms * 0.001f,
        g_state.config.max_speed_mm_s,
        g_state.config.acceleration_mm_s2,
        mapper.getMinMm(), mapper.getMaxMm());

    // Remember this command's clamped target for the next interval's planning.
    // Use the clamped target so the chain stays inside the stroke window —
    // no phantom debt from a target that got clipped. :3
    s_last_target_mm = plan.clamped_target_mm;

    motor.setAcceleration(plan.accel_mm_s2);
    motor.streamTo(plan.clamped_target_mm, plan.speed_mm_s);

    // Stash the planned target for the UI motion graph (stage 2 of 3). :3
    g_state.commanded_target_mm = plan.clamped_target_mm;
}

// Called by TCodeParser on DSTOP — pull out cleanly. :3
static void buttplugStop() {
    if (motor.isHomed()) motor.hardStop();
}


// ============================================================================
// FreeRTOS Tasks
// ============================================================================

// Core 1 — real-time: homing state machine + per-tick motor maintenance.
// This is the dom core — it keeps the shaft disciplined and on time. :3
//
// The handler core (Core 0) sets flags (homing_in_progress, estop_requested)
// to issue orders. We detect those flags here and call ALL motor hardware
// operations from Core 1, where the FastAccelStepper engine lives. No cross-
// core racing on the stepper object — the handler asks, the dom core does. :3
//
// Homing is fully self-contained: motor.home() spawns its own FreeRTOS task
// that runs the sweep, polls the endstop, does the backoff, and sets the homed
// flag when done. motorTask just kicks it off once and watches isHoming() go
// false. No polling — the homing task IS the loop. :3
static void motorTask(void* /*param*/) {
    bool homing_started = false;
    while (true) {
        // ---- E-stop: the red button got slapped. Cut power NOW. :3 ----------
        if (g_state.estop_requested) {
            motor.stop();
            homing_started = false;
            g_state.estop_requested = false;
            APPLOG("E-Stop handled — shaft is soft, waiting for orders~ :3");
        }
        // ---- Homing in progress: spawn the homing task ONCE, then watch
        //      isHoming() go false when the task finishes. :3 -----------------
        else if (g_state.homing_in_progress) {
            if (!motor.isHoming() && !homing_started) {
                motor.home();
                homing_started = true;
            }
            if (!motor.isHoming() && homing_started) {
                g_state.homing_in_progress = false;
                g_state.homed = motor.isHomed();
                homing_started = false;
                if (g_state.homed)
                    APPLOG("System is now homed and ready to pound :3");
                else
                    APPLOG("Homing failed — endstop not found. Check wiring.");
            }
        } else {
            // ---- Push-to-home: shove the carriage into the endstop manually. :3
            homing_started = false;
            if (!g_state.homed) {
                if (motor.checkPushToHome()) {
                    g_state.homed = true;
                    APPLOG("System homed via push-to-home and ready :3");
                }
            }
        }
        motor.update();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Core 0 — system: services all active transports and reports inbound rate.
// This is the handler core — takes all the dirty talk (TCode) and passes
// orders to the dom core without breaking a sweat. :3
static void commsTask(void* /*param*/) {
    uint32_t last_report_ms   = 0;
    uint32_t last_frame_count = 0;
    while (true) {
        // In SER mode skip WebSocket networking — the RX FIFO needs every
        // microsecond we can give it. :3
        if (g_state.getTransport() == TransportMode::SER) {
            serialTransport.poll();
        } else {
            wsTransport.run();
        }

        // Once-per-second rate diagnostic — feeds the Hz chip in the UI. :3
        uint32_t now = millis();
        if (now - last_report_ms >= 1000) {
            uint32_t frames  = tcodeParser.rxFrameCount;
            uint32_t per_sec = frames - last_frame_count;
            last_frame_count = frames;
            last_report_ms   = now;
            g_state.measured_hz = (uint16_t)per_sec;
            if (wsTransport.isConnected() || serialTransport.isActive())
                APPLOGF("[RATE] rx=%u frames/s", per_sec);
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// Core 0 — system: HTTP server (delegates entirely to WebUI::update()). :3
static void httpTask(void* param) {
    WebUI* ui = static_cast<WebUI*>(param);
    while (true) {
        ui->update();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


// ============================================================================
// setup() — ordered wiring only
// ============================================================================

void setup() {
    Serial.begin(SERIAL_CONTROL_BAUD);
    applogBegin();
    APPLOG("=== SlopDrive-32 v2.0 ===");
#if SERIAL_CONTROL_MODE
    applog("Serial control mode ON: USB Serial is dedicated to Intiface TCode.");
    applog("Add a 'Serial' device in Intiface pointing at this COM port.");
#endif

    // Mount filesystem (web assets served from LittleFS).
    if (LittleFS.begin(true))
        APPLOG("LittleFS mounted");
    else
        APPLOG("LittleFS mount FAILED - upload filesystem image (pio run -t uploadfs)");

    // Load persisted settings (or factory defaults on first boot).
    ConfigStore::load(g_state, mapper, motor);

    // Motor hardware init + apply saved driver config.
    motor.init();
    motor.applyDriverConfig(g_state.driver);

    // WiFi + mDNS (prerequisite for WS transport and the web UI).
    transportMgr.setupWiFi();

    // Register all HTTP routes and start the web server.
    webui.init();

    // Wire TCode parser callbacks to the motion layer.
    tcodeParser.onLinearRampTo(buttplugLinearCmd);
    tcodeParser.onLinearStop(buttplugStop);

    // Start the WS TCode server (always up — MultiFunPlayer can connect). :3
    wsTransport.begin();

    // Bring up the saved transport (WS / SER / BT).
    transportMgr.applyTransport(g_state.getTransport());

    // ---- Create FreeRTOS tasks with correct core pinning (.clinerules §2) ----
    //   Core 1 (real-time):  motorTask, Generator
    //   Core 0 (system):     commsTask, httpTask
    xTaskCreatePinnedToCore(motorTask, "Motor", 4096, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(commsTask, "Comms", 4096, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(httpTask,  "HTTP",  4096, &webui,  1, nullptr, 0);

    generator.init();    // creates its own task on Core 1, priority 2

#if HOMING_DISABLED
    // >>>> HOMING DISABLED: force homed flags for bench/remote testing only.
    // Re-enable homing before running on real hardware. <<<<
    g_state.homed = true;
    motor.forceHomeState(true);
    APPLOG("!!! HOMING DISABLED — bench-test build only. Remove -DHOMING_DISABLED for real hardware.");
#endif

    APPLOG("System ready — push that thick shaft all the way in to home, or use the web UI :3");
}


// ============================================================================
// loop() — idle; all work is done in FreeRTOS tasks
// ============================================================================

void loop() {
    vTaskDelay(pdMS_TO_TICKS(100));
}
