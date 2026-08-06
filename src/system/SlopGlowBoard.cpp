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

HeartbeatSource* s_hbMotor = nullptr;
HeartbeatSource* s_hbComms = nullptr;

}  // namespace

void slopglowInit() {
    s_rgb.begin();        // GPIO0 strapping pin: this runs post-boot by contract
    s_heartLamp.begin();

    // Boot rainbow holds until these report in: Motion (motor bound,
    // main.cpp), Session + Link (hub init). A hub that never comes up leaves
    // the rainbow running, which is the honest "never finished booting".
    uint8_t need = uint8_t((1u << uint8_t(System::Motion)) |
                           (1u << uint8_t(System::Session)));
#if defined(UART_LINK_ENABLED)
    need |= uint8_t(1u << uint8_t(System::Link));
#endif
    s_engine.requireReady(need);

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

void slopglowUpdate(const SystemState& state) {
    uint32_t now = millis();

    // ---- SystemState -> (system, status). set() is idempotent; the arbiter
    // shows the most time-sensitive pair (Status rank, Safety wins ties).
    s_engine.set(System::Safety,
                 state.estop_latched ? Status::Urgent : Status::Nominal);
    s_engine.set(System::Flash,
                 state.ota_active.load(std::memory_order_relaxed) ? Status::Urgent
                                                                  : Status::Nominal);
    bool streaming = state.gen_active ||
                     (state.last_intiface_ms != 0 && (now - state.last_intiface_ms) < 500);
    // Unhomed is LATCHED (waiting on the operator to home), not a fault: the
    // T15 lesson, now expressed in the grammar instead of the enum order.
    s_engine.set(System::Motion,
                 state.homing_in_progress                ? Status::Working
                 : !state.homed                          ? Status::Latched
                 : (state.paused || state.manual_override) ? Status::Latched
                 : streaming                             ? Status::Working
                                                         : Status::Nominal);

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
}
