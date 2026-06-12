// SlopDrive-32 — main.cpp
//
// Thin composition root (Step 10 of refactoring_plan.md).
// All logic lives in the modules under system/, motion/, comms/, and ui/.
// This file only:
//   1. Declares module instances.
//   2. Wires them together in setup().
//   3. Creates FreeRTOS tasks with correct core pinning.
//   4. Idles in loop().
//
// Core assignment (.clinerules §2):
//   Core 1 (real-time):  motorTask, Generator task, Interpolator task
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
#include "Interpolator.h"

#include "TCodeParser.h"
#include "SerialTransport.h"
#include "WebSocketTransport.h"
#include "BleTransport.h"
#include "TransportManager.h"

#include "WebUI.h"


// ============================================================================
// Module instances — one big orgy of objects :3
// Driver selected at compile time via build flag
// ============================================================================

#if defined(DRIVER_TMC2160)
  TMC2160StepperDriver motor;
#else
  #error "No motor driver selected. Define DRIVER_TMC2160 (or a future driver flag) in platformio.ini build_flags."
#endif

static SystemState       g_state;
static RangeMapper       mapper;

static Generator         generator(g_state, mapper, motor);
static Interpolator      interpolator(g_state, mapper, motor);

static TCodeParser       tcodeParser;
static SerialTransport   serialTransport(tcodeParser);
static WebSocketTransport wsTransport(tcodeParser);
static BleTransport      bleTransport(tcodeParser);
static TransportManager  transportMgr(g_state, tcodeParser,
                                       serialTransport, wsTransport, bleTransport);

static WebUI webui(g_state, motor, mapper, generator, interpolator,
                   transportMgr, serialTransport, wsTransport, bleTransport);


// ============================================================================
// Glue callbacks — these good boys sit between TCode and the motor, making
// sure commands go where they're supposed to and nobody gets overstimulated :3
// ============================================================================

// How long we ease back in after being edged (paused), in ms.
static const uint32_t RESUME_BLEND_MS = 800;

// Called by TCodeParser on every valid L0 linear command.
static void buttplugLinearCmd(float position, uint32_t duration_ms) {

    if (!g_state.homed) return;
    if (g_state.paused || g_state.manual_override) {
        g_state.resume_start_ms = millis();
        return;
    }

    g_state.last_intiface_ms = millis();

    // Measure inter-command cadence for Auto Duration.
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

    if (g_state.auto_duration && g_state.measured_interval_ms > 1.0f)
        duration_ms = (uint32_t)(g_state.measured_interval_ms + 0.5f);

    float target_mm = mapper.intensityToPosition(position);

    // ---- BUFFERED mode: skullfuck this sample deep into the jitter buffer,
    //      straight down its throat until it's knotted in place. Smooth entry,
    //      no gag reflex :3 ----
    if (g_state.getInputMode() == InputMode::BUFFERED) {
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
        interpolator.pushSample(target_mm);
        return;
    }

    // ---- EXTRAPOLATE mode: slow resume blend then stream ----
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

    float speed_mm_s = 0.0f;
    if (duration_ms > 0) {
        float current_mm  = motor.getPosition();
        float distance_mm = fabsf(target_mm - current_mm);
        if (distance_mm > 0.05f) {
            speed_mm_s = (distance_mm / (float)duration_ms) * 1000.0f;
            speed_mm_s = constrain(speed_mm_s, 5.0f, g_state.config.max_speed_mm_s);
        } else {
            speed_mm_s = g_state.config.max_speed_mm_s * 0.25f;
        }
    }

    motor.streamExtrapolated(target_mm, speed_mm_s);
}

// Called by TCodeParser on DSTOP.
static void buttplugStop() {
    if (motor.isHomed()) motor.hardStop();
}


// ============================================================================
// FreeRTOS Tasks
// ============================================================================

// Core 1 — real-time: homing state machine + per-tick motor maintenance.
// This is the dom core — it keeps the shaft disciplined and on time.
static void motorTask(void* /*param*/) {
    while (true) {
        if (g_state.homing_in_progress) {
            motor.runHomingStep();
            if (!motor.isHoming()) {
                g_state.homing_in_progress = false;
                g_state.homed = motor.isHomed();
                if (g_state.homed)
                    APPLOG("System is now homed and ready");
            }
        } else if (!g_state.homed) {
            if (motor.checkPushToHome()) {
                g_state.homed = true;
                APPLOG("System homed via push-to-home and ready");
            }
        }
        motor.update();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Core 0 — system: services all active transports and reports inbound rate.
// This is the handler core — it takes all the dirty talk (TCode) and passes
// orders to the dom core without breaking a sweat :3
//
// Moved to Core 0 per .clinerules §2 (networking/service on Core 0, real-time
// motion on Core 1). The original monolithic code pinned the buttplug task to
// Core 1 "for coherence with the motion pipeline"; this corrects the
// architecture at the cost of a cross-core call (motor.streamExtrapolated()
// from the TCode callback), which on the ESP32-S3 is basically a whisper
// across the core interconnect — sub-microsecond, barely a tease.
static void commsTask(void* /*param*/) {
    uint32_t last_report_ms  = 0;
    uint32_t last_frame_count = 0;
    while (true) {
        // Always service the WS server + (if enabled) WSDM client.
        wsTransport.run();

        // Poll USB Serial when SER is the active transport.
        if (g_state.getTransport() == TransportMode::SER)
            serialTransport.poll();

        // Once-per-second rate diagnostic.
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

// Core 0 — system: HTTP server (delegates entirely to WebUI::update()).
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
    APPLOG("=== SlopDrive-32 StrokeEngine v2.0 ===");
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

    // Start the WS TCode server (always up — MultiFunPlayer can connect).
    wsTransport.begin();

    // Bring up the saved transport (WS / SER / BT).
    transportMgr.applyTransport(g_state.getTransport());

    // ---- Create FreeRTOS tasks with correct core pinning (.clinerules §2) ----
    //   Core 1 (real-time):  motorTask, Generator, Interpolator
    //   Core 0 (system):     commsTask, httpTask
    xTaskCreatePinnedToCore(motorTask, "Motor",  4096, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(commsTask, "Comms",  4096, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(httpTask,  "HTTP",   4096, &webui,  1, nullptr, 0);

    generator.init();    // creates its own task on Core 1, priority 2
    interpolator.init(); // creates its own task on Core 1, priority 2

    APPLOG("System ready — push that thick shaft all the way in until it bottoms out on the endstop to home, or use the web UI you naughty pup :3");
}


// ============================================================================
// loop() — idle; all work is done in FreeRTOS tasks
// ============================================================================

void loop() {
    vTaskDelay(pdMS_TO_TICKS(100));
}