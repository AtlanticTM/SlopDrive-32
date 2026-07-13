// ============================================================================
// StatusLeds — heartbeat breathing, amber activity pulse, RGB machine state
// ============================================================================
// Runs entirely on Core 0 (called from httpTask's loop). Everything here is
// non-blocking and cheap — a handful of float ops and GPIO writes per call.
// The heartbeat uses hardware LEDC PWM so the breath is silky even if the
// update cadence jitters. :3

#include <Arduino.h>
#include <math.h>

#include "StatusLeds.h"
#include "config_api.h"
#include "SystemState.h"

// ---- Amber activity pulse ---------------------------------------------------
// WebUI handlers stamp this from Core 0; update() reads it. 32-bit aligned
// store/load is hardware-atomic on the S3 — no mutex needed. :3
static volatile uint32_t s_last_activity_ms = 0;
static constexpr uint32_t ACTIVITY_PULSE_MS = 120;   // how long the amber blinks per command

// ---- RGB helpers (discrete ACTIVE-LOW LEDs) ----------------------------------
static inline void rgbWrite(bool r, bool g, bool b) {
#if LED_ACTIVE_LOW
    digitalWrite(PIN_LED_R, r ? LOW : HIGH);
    digitalWrite(PIN_LED_G, g ? LOW : HIGH);
    digitalWrite(PIN_LED_B, b ? LOW : HIGH);
#else
    digitalWrite(PIN_LED_R, r ? HIGH : LOW);
    digitalWrite(PIN_LED_G, g ? HIGH : LOW);
    digitalWrite(PIN_LED_B, b ? HIGH : LOW);
#endif
}

static inline void amberWrite(bool on) {
#if LED_ACTIVE_LOW
    digitalWrite(PIN_LED_ORANGE, on ? LOW : HIGH);
#else
    digitalWrite(PIN_LED_ORANGE, on ? HIGH : LOW);
#endif
}

void statusLedsInit() {
    // Amber + RGB are plain GPIO (discrete LEDs). Start everything OFF.
    pinMode(PIN_LED_ORANGE, OUTPUT);
    pinMode(PIN_LED_R, OUTPUT);
    pinMode(PIN_LED_G, OUTPUT);   // strapping pin — only safe now, post-boot
    pinMode(PIN_LED_B, OUTPUT);
    amberWrite(false);
    rgbWrite(false, false, false);

    // Heartbeat breathes on hardware LEDC PWM — 5kHz, 8-bit duty. The
    // arduino-esp32 v3.x API attaches straight to the pin, no channel juggling.
    ledcAttach(PIN_HB_LED, 5000, 8);
    ledcWrite(PIN_HB_LED, 0);
}

void statusLedsActivity() {
    s_last_activity_ms = millis();
}

void statusLedsUpdate(const SystemState& state) {
    uint32_t now = millis();

    // ---- Heartbeat: smooth breathing ----------------------------------------
    // Raised-cosine breath, ~3s period. The gamma curve (^2.2) makes the LED's
    // perceived brightness ramp linearly to the eye instead of hanging bright.
    // A crashed scheduler freezes the breath — instantly visible. :3
    {
        const float period_ms = 3000.0f;
        float phase = (float)(now % (uint32_t)period_ms) / period_ms;   // 0..1
        float breath = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * phase);  // 0..1..0
        float duty = powf(breath, 2.2f) * 255.0f;                        // gamma
#if HB_LED_ACTIVE_HIGH
        ledcWrite(PIN_HB_LED, (uint32_t)duty);
#else
        ledcWrite(PIN_HB_LED, 255u - (uint32_t)duty);
#endif
    }

    // ---- Amber: WebUI command activity pulse --------------------------------
    amberWrite(now - s_last_activity_ms < ACTIVITY_PULSE_MS);

    // ---- RGB: machine state at a glance --------------------------------------
    //   BLUE blink  — homing in progress
    //   RED solid   — not homed (position untrusted)
    //   MAGENTA     — paused or manual override (host input gated)
    //   CYAN        — homed & motion actively streaming (TCode or generator)
    //   GREEN       — homed & idle, ready to play :3
    if (state.homing_in_progress) {
        bool blink = (now / 250) & 1;      // 2Hz blue blink
        rgbWrite(false, false, blink);
    } else if (!state.homed) {
        rgbWrite(true, false, false);      // red — unhomed
    } else if (state.paused || state.manual_override) {
        rgbWrite(true, false, true);       // magenta — operator gating host input
    } else {
        bool streaming = state.gen_active ||
                         (state.last_intiface_ms != 0 &&
                          (now - state.last_intiface_ms) < 500);
        if (streaming) rgbWrite(false, true, true);   // cyan — actively pounding
        else           rgbWrite(false, true, false);  // green — homed & idle
    }
}