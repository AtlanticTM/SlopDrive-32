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
// 120 BPM cardiac throb (ruling 2026-08-06): sin^2 attack, cos^2 release,
// dark rest between beats. Shape is perceptual; the driver's gamma8 maps it.
constexpr uint32_t kHeartPeriodMs  = 500;   // 120 BPM
constexpr uint32_t kHeartAttackMs  = 70;
constexpr uint32_t kHeartReleaseMs = 180;   // rest = the remaining 250 ms

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
    // Bus-unreachable outranks it as DEGRADED: an unpowered drive BLINKS
    // amber, a machine that merely needs homing SITS solid.
#if defined(SD32_MODBUS_TOOLS)
    const bool busDown = !state.servo_bus_ready;
#else
    const bool busDown = false;   // no RS485 in this build; nothing to report
#endif
    s_engine.set(System::Motion,
                 state.homing_in_progress                ? Status::Working
                 : busDown                               ? Status::Degraded
                 : !state.homed                          ? Status::Latched
                 : (state.paused || state.manual_override) ? Status::Latched
                 : streaming                             ? Status::Working
                                                         : Status::Nominal);

    s_engine.update(now);

    // Yellow heartbeat: 120 BPM throb. Phase advances only while the liveness
    // gate is happy; a dead core (or a stalled httpTask, which stops this
    // very call) freezes the beat mid-frame, same contract as before.
    if (!s_heartSeeded) {
        s_heartSeeded = true;
        s_lastHeartMs = now;
    }
    uint32_t dt = now - s_lastHeartMs;
    s_lastHeartMs = now;
    if (!s_engine.frozen()) {
        s_heartPhaseMs = (s_heartPhaseMs + dt) % kHeartPeriodMs;
        float v = 0.0f;
        if (s_heartPhaseMs < kHeartAttackMs) {
            // Systole: sin^2 rise, zero slope leaving dark, sharp swell.
            float x = sinf(1.5707963f * float(s_heartPhaseMs) / float(kHeartAttackMs));
            v = x * x;
        } else if (s_heartPhaseMs < kHeartAttackMs + kHeartReleaseMs) {
            // Diastole: cos^2 decay, slower than the rise; the "throb".
            float x = cosf(1.5707963f * float(s_heartPhaseMs - kHeartAttackMs) /
                           float(kHeartReleaseMs));
            v = x * x;
        }
        // else: dark rest until the next beat.
        uint8_t lum = uint8_t(v * 255.0f + 0.5f);
        s_heartLamp.set(0, {lum, lum, lum});
        s_heartLamp.show();
    }
}
