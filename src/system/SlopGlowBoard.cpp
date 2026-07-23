// SlopGlowBoard — SlopGlow wiring for the Nano-ESP32 controller.
// The old StatusLeds gave us: heartbeat breathe (LEDC), amber command pulse,
// and 7-color discrete RGB state. This keeps all three roles but upgrades
// them: the RGB LED is now a true-color gamma-corrected pixel driven by the
// semantic state engine, the heartbeat lamp is the same frame's luma shadow
// (so it pulses in sympathy and freezes with the engine), and the whole
// display is liveness-gated on BOTH cores — a dead motorTask visibly
// freezes the lights even while Core 0 hums along.

#include "SlopGlowBoard.h"

#include <Arduino.h>

#include "SystemState.h"
#include "config_api.h"

using namespace slopglow;

namespace {

// RGB = the status pixel (engine-driven). The yellow heartbeat LED is its
// own lamp with its own steady breathe — deliberately NOT tied to the status
// animation (user decree: heartbeat is heartbeat, status is status). It
// still freezes with the liveness gate: its phase only advances while the
// engine isn't frozen, and only while httpTask pumps us at all.
LedcRgbOutput s_rgb(PIN_LED_R, PIN_LED_G, PIN_LED_B, LED_ACTIVE_LOW == 1);
LedcMonoOutput s_heartLamp(PIN_HB_LED, HB_LED_ACTIVE_HIGH == 1);
GlowEngine s_engine(s_rgb);

uint32_t s_heartPhaseMs = 0;
uint32_t s_lastHeartMs = 0;
bool s_heartSeeded = false;
constexpr uint32_t kHeartPeriodMs = 3000;   // the familiar ~3 s breath

// Amber activity pulse (plain GPIO — it's a discrete LED and 120ms square
// pulses don't need PWM). 32-bit aligned store is atomic on the S3.
volatile uint32_t s_last_activity_ms = 0;
constexpr uint32_t kActivityPulseMs = 120;

HeartbeatSource* s_hbMotor = nullptr;
HeartbeatSource* s_hbComms = nullptr;

inline void amberWrite(bool on) {
#if LED_ACTIVE_LOW
    digitalWrite(PIN_LED_ORANGE, on ? LOW : HIGH);
#else
    digitalWrite(PIN_LED_ORANGE, on ? HIGH : LOW);
#endif
}

}  // namespace

void slopglowInit() {
    pinMode(PIN_LED_ORANGE, OUTPUT);
    amberWrite(false);
    s_rgb.begin();        // GPIO0 strapping pin: this runs post-boot by contract
    s_heartLamp.begin();

    // Preserve this board's established color language where the stock spec
    // differs: paused/override has always been magenta here.
    s_engine.setSpec(GlowState::Paused, {{255, 0, 255}, {12, 0, 12}, GlowMode::Breathe, 2600});

    // Generous staleness windows: a real freeze is forever, so detection
    // latency is cheap — but homing legitimately blocks motorTask for long
    // stretches (protected calibration cycles), and WiFi supervision can
    // stall commsTask; neither should read as a crash.
    s_hbMotor = s_engine.addHeartbeat(1500);
    s_hbComms = s_engine.addHeartbeat(500);
}

slopglow::HeartbeatSource* slopglowMotorHeartbeat() { return s_hbMotor; }
slopglow::HeartbeatSource* slopglowCommsHeartbeat() { return s_hbComms; }
slopglow::GlowEngine& slopglowEngine() { return s_engine; }

void slopglowActivity() { s_last_activity_ms = millis(); }

void slopglowUpdate(const SystemState& state) {
    uint32_t now = millis();

    // ---- SystemState -> semantic conditions. set() is idempotent; the
    // engine's priority order picks the winner (Estop > Ota > Fault > ...).
    s_engine.set(GlowState::Estop, state.estop_latched);
    s_engine.set(GlowState::Ota, state.ota_active.load(std::memory_order_relaxed));
    s_engine.set(GlowState::Calibrating, state.homing_in_progress);
    s_engine.set(GlowState::Fault, !state.homed && !state.homing_in_progress);
    s_engine.set(GlowState::Paused, state.paused || state.manual_override);

    bool streaming = state.gen_active ||
                     (state.last_intiface_ms != 0 && (now - state.last_intiface_ms) < 500);
    s_engine.set(GlowState::Active, state.homed && streaming);
    s_engine.set(GlowState::Ready, state.homed);

    s_engine.update(now);

    // Yellow heartbeat: independent triangle breathe, gamma'd via luma. Phase
    // advances only while the liveness gate is happy — a dead core (or a
    // stalled httpTask, which stops this very call) freezes the breath.
    if (!s_heartSeeded) {
        s_heartSeeded = true;
        s_lastHeartMs = now;
    }
    uint32_t dt = now - s_lastHeartMs;
    s_lastHeartMs = now;
    if (!s_engine.frozen()) {
        s_heartPhaseMs = (s_heartPhaseMs + dt) % kHeartPeriodMs;
        uint32_t ph = s_heartPhaseMs * 512u / kHeartPeriodMs;   // 0..511 triangle
        uint8_t v = uint8_t(ph < 256 ? ph : 511 - ph);
        s_heartLamp.set(0, {v, v, v});
        s_heartLamp.show();
    }

    amberWrite(now - s_last_activity_ms < kActivityPulseMs);
}
