#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include <esp_timer.h>

#include <LittleFS.h>


#include "config_api.h"
#if defined(DRIVER_TMC2160)
#include "TMC2160StepperDriver.h"
#endif
#include "range_mapper.h"
#include "AppLog.h"
#include "Kinematics.h"
#include "SystemState.h"
#include "ConfigStore.h"
#include "Generator.h"
#include "Interpolator.h"

// ---- Comms (Step 8: split from ButtplugServer) -------------------------------
#include "TCodeParser.h"
#include "SerialTransport.h"
#include "WebSocketTransport.h"
#include "BleTransport.h"
#include "TransportManager.h"

// ---- WebUI (Step 9: all HTTP handlers) ---------------------------------------
#include "WebUI.h"


// In serial-control mode the USB Serial port is dedicated to Intiface TCode, so
// status/debug must go to the web log (applog), NOT Serial. APPLOG()/APPLOGF()
// route accordingly so the same source works in both modes.
#if SERIAL_CONTROL_MODE
  #define APPLOG(s)      applog(s)
  #define APPLOGF(...)   applogf(__VA_ARGS__)
#else
  #define APPLOG(s)      Serial.println(s)
  #define APPLOGF(...)   Serial.printf(__VA_ARGS__)
#endif


// ============================================================================
// Driver selection — compile-time via build flag (platformio.ini)
// ============================================================================
#if defined(DRIVER_TMC2160)
  TMC2160StepperDriver motor;
#else
  #error "No motor driver selected! Define DRIVER_TMC2160 (or future driver flags) in platformio.ini build_flags."
#endif

// ============================================================================
// Global Instances
// ============================================================================

RangeMapper mapper;

// Centralised, thread-safe runtime state (replaces ~30 file-scope globals).
static SystemState g_state;

Generator generator(g_state, mapper, motor);
Interpolator interpolator(g_state, mapper, motor);

// ---- Comms modules (Step 8) -------------------------------------------------
TCodeParser tcodeParser;
SerialTransport    serialTransport(tcodeParser);
WebSocketTransport wsTransport(tcodeParser);
BleTransport       bleTransport(tcodeParser);
TransportManager   transportMgr(g_state, tcodeParser, serialTransport, wsTransport, bleTransport);

// ---- WebUI (Step 9: all HTTP handlers + routes live here) -------------------
WebUI webui(g_state, motor, mapper, generator, interpolator,
            transportMgr, serialTransport, wsTransport, bleTransport);


// ============================================================================
// Control gating blend constant (unchanged).
// ============================================================================
static const uint32_t RESUME_BLEND_MS = 800;




// ============================================================================
// TCode callbacks (glue between parser and motor)
// ============================================================================

static void buttplugLinearCmd(float position, uint32_t duration_ms) {

    if (!g_state.homed) return;
    if (g_state.paused || g_state.manual_override) { g_state.resume_start_ms = millis(); return; }

    g_state.last_intiface_ms = millis();

    uint32_t now = millis();
    if (g_state.last_cmd_ms != 0) {
        uint32_t gap = now - g_state.last_cmd_ms;
        if (gap > 0 && gap < 1000) {
            if (g_state.measured_interval_ms <= 0.0f) g_state.measured_interval_ms = (float)gap;
            else g_state.measured_interval_ms = 0.7f * g_state.measured_interval_ms + 0.3f * (float)gap;
        }
    }
    g_state.last_cmd_ms = now;

    if (g_state.auto_duration && g_state.measured_interval_ms > 1.0f) {
        duration_ms = (uint32_t)(g_state.measured_interval_ms + 0.5f);
    }

    float target_mm = mapper.intensityToPosition(position);

    // BUFFERED mode
    if (g_state.getInputMode() == InputMode::BUFFERED) {
        if (g_state.resume_start_ms != 0) {
            uint32_t elapsed = millis() - g_state.resume_start_ms;
            if (elapsed >= RESUME_BLEND_MS) {
                g_state.resume_start_ms = 0;
            } else {
                float blend = (float)elapsed / (float)RESUME_BLEND_MS;
                float cur = motor.getPosition();
                target_mm = cur + blend * (target_mm - cur);
            }
        }
        interpolator.pushSample(target_mm);
        return;
    }

    // EXTRAPOLATE mode: slow resume blend
    if (g_state.resume_start_ms != 0) {
        uint32_t elapsed = millis() - g_state.resume_start_ms;
        if (elapsed >= RESUME_BLEND_MS) {
            g_state.resume_start_ms = 0;
        } else {
            float blend = (float)elapsed / (float)RESUME_BLEND_MS;
            float cur = motor.getPosition();
            target_mm = cur + blend * (target_mm - cur);
        }
    }

    float speed_mm_s = 0.0f;
    if (duration_ms > 0) {
        float current_mm = motor.getPosition();
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

static void buttplugStop() {
    if (motor.isHomed()) {
        motor.hardStop();
    }
}


// ============================================================================
// FreeRTOS Tasks
// ============================================================================

void motorTask(void* parameter) {
    while (true) {
        if (g_state.homing_in_progress) {
            motor.runHomingStep();

            if (!motor.isHoming()) {
                g_state.homing_in_progress = false;
                g_state.homed = motor.isHomed();
                if (g_state.homed) {
                    APPLOG("System is now homed and ready");
                }
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

/// Comms task: services all active transports and reports inbound rate.
/// Renamed from buttplugTask (Step 8).
void commsTask(void* parameter) {
    uint32_t last_report_ms = 0;
    uint32_t last_frame_count = 0;
    while (true) {
        // Always service the WS server + (if enabled) WSDM client.
        wsTransport.run();

        // Poll USB Serial when SER is the active transport.
        if (g_state.getTransport() == TransportMode::SER) {
            serialTransport.poll();
        }

        // Once-per-second rate diagnostic: raw inbound WebSocket + Serial frame
        // rate from the parser (counted before any motor work).
        uint32_t now = millis();
        if (now - last_report_ms >= 1000) {
            uint32_t frames = tcodeParser.rxFrameCount;
            uint32_t per_sec = frames - last_frame_count;
            last_frame_count = frames;
            last_report_ms = now;
            g_state.measured_hz = (uint16_t)per_sec;
            if (wsTransport.isConnected() || serialTransport.isActive()) {
                APPLOGF("[RATE] rx=%u frames/s", per_sec);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

/// Thin HTTP server task — delegates entirely to WebUI::update().
/// (Step 9: all handlers + route registration live in ui/WebUI.cpp)
void httpTask(void* parameter) {
    WebUI* webui = static_cast<WebUI*>(parameter);
    while (true) {
        webui->update();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


// ============================================================================
// Setup & Loop
// ============================================================================

void setup() {
    Serial.begin(SERIAL_CONTROL_BAUD);
    applogBegin();
    APPLOG("=== SlopDrive-32 StrokeEngine v2.0 ===");
#if SERIAL_CONTROL_MODE
    applog("Serial control mode ON: USB Serial is dedicated to Intiface TCode.");
    applog("Add a 'Serial' device in Intiface pointing at this COM port.");
#endif

    if (LittleFS.begin(true)) {
        APPLOG("LittleFS mounted");
    } else {
        APPLOG("LittleFS mount FAILED - upload filesystem image (pio run -t uploadfs)");
    }

    ConfigStore::load(g_state, mapper, motor);

    motor.init();
    motor.applyDriverConfig(g_state.driver);

    // ---- Comms: WiFi + parser callbacks ----
    transportMgr.setupWiFi();

    // ---- WebUI: register all HTTP routes and start the server ----
    webui.init();

    // Register TCode parser callbacks.
    tcodeParser.onLinearRampTo(buttplugLinearCmd);
    tcodeParser.onLinearStop(buttplugStop);

    // Start the WS TCode server (always up — MultiFunPlayer can connect).
    wsTransport.begin();

    // Bring up the saved transport (WS / SER / BT).
    transportMgr.applyTransport(g_state.getTransport());

    // Create FreeRTOS tasks
    xTaskCreatePinnedToCore(motorTask, "Motor", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(commsTask, "Comms", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(httpTask, "HTTP", 4096, &webui, 1, NULL, 0);

    generator.init();
    interpolator.init();

    APPLOG("System ready - push shaft into endstop to home, or use the web UI");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(100));
}