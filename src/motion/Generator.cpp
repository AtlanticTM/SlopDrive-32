#include "Generator.h"
#include "Kinematics.h"
#include "range_mapper.h"
#include "MotorDriver.h"
#include "AppLog.h"
#include "config_api.h"

#include <Arduino.h>

// In serial-control mode the USB Serial port is dedicated to Intiface TCode,
// so status/debug must go to the web log (applog), NOT Serial.
// These macros match the ones in main.cpp so logging behaves identically.
#if SERIAL_CONTROL_MODE
  #define APPLOG(s)      applog(s)
  #define APPLOGF(...)   applogf(__VA_ARGS__)
#else
  #define APPLOG(s)      Serial.println(s)
  #define APPLOGF(...)   Serial.printf(__VA_ARGS__)
#endif

// ============================================================================
// Generator — constructor / destructor
// ============================================================================

Generator::Generator(SystemState& state, RangeMapper& mapper, MotorDriver& motor)
    : _state(state), _mapper(mapper), _motor(motor)
{}

Generator::~Generator() {
    if (_task) vTaskDelete(_task);
}

// ============================================================================
// Lifecycle
// ============================================================================

void Generator::init() {
    xTaskCreatePinnedToCore(taskFunction, "Generator", 4096, this, 2, &_task, 1);
}

void Generator::emergencyStop() {
    _state.gen.running = false;
}

bool Generator::isRunning() const { return _state.gen.running; }
bool Generator::isActive()  const { return _state.gen_active; }

// ============================================================================
// Static trampoline → member function
// ============================================================================

void Generator::taskFunction(void* param) {
    static_cast<Generator*>(param)->run();
}

// ============================================================================
// Task loop — functionally identical to the original generatorTask() in
// main.cpp.  No behaviour changes; pure structural refactor.
// ============================================================================

void Generator::run() {
    uint32_t last_diag_ms = 0;
    while (true) {
        // Local generation rate is user-selectable (20/50/100 Hz). Higher =
        // smoother motion / more step planning; lower = lighter CPU. Re-read
        // each loop so live changes from /api/gen take effect immediately.
        uint16_t ghz = _state.gen_rate_tick_hz;
        if (ghz < 5) ghz = 5;
        if (ghz > 200) ghz = 200;
        const TickType_t period = pdMS_TO_TICKS(1000 / ghz);

        bool intiface_recent = (_state.last_intiface_ms != 0) &&
                               (millis() - _state.last_intiface_ms < 250);
        bool user_has_control = _state.paused || _state.manual_override;
        bool emit = _state.gen.running && _state.homed &&
                    (user_has_control || !intiface_recent);
        _state.gen_active = emit;

        // Diagnostics: while the generator is running, log once per second so the
        // Log tab reflects what it's doing — either why it's idle, or a heartbeat
        // with the live target while it's actually driving the motor.
        if (_state.gen.running && (millis() - last_diag_ms > 1000)) {
            last_diag_ms = millis();
            if (!_state.homed)
                APPLOG("Generator: waiting - motor not homed");
            else if (intiface_recent && !user_has_control)
                APPLOG("Generator: yielding to active Intiface (use Pause/Override to keep control)");
            else
            APPLOGF("Generator: running target=%.1fmm phase=%.2f window=[%.0f,%.0f]",
                    _mapper.getMinMm() + _state.gen.offset * (_mapper.getMaxMm() - _mapper.getMinMm())
                        + (kinematics::ease(kinematics::carrier(_state.gen.wave, _state.gen_phase),
                                            _state.gen.ease) - 0.5f)
                          * _state.gen.depth * (_mapper.getMaxMm() - _mapper.getMinMm()),
                        _state.gen_phase, _mapper.getMinMm(), _mapper.getMaxMm());
        }

        if (emit) {
            uint32_t now_us = micros();
            float dt = (_state.gen_last_us != 0)
                           ? (float)(int32_t)(now_us - _state.gen_last_us) / 1e6f
                           : 0.0f;
            _state.gen_last_us = now_us;
            if (dt < 0.0f || dt > 0.5f) dt = 0.0f;   // guard against wrap/stall

            float rate  = _state.gen.rate_hz;
            float depth = _state.gen.depth;

            // Advance the modulator clock and apply FM (rate) or AM (depth).
            if (_state.gen.mod != 0) {
                _state.gen_mod_clock += dt * _state.gen.mod_rate;
                if (_state.gen_mod_clock > 1.0f)
                    _state.gen_mod_clock -= floorf(_state.gen_mod_clock);
                float m = kinematics::modShape(_state.gen.mod_wave, _state.gen_mod_clock);
                if (_state.gen.mod == 1) {        // rate FM: swing rate by +/- mod_amp Hz
                    rate = fmaxf(0.05f, rate + (m - 0.5f) * 2.0f * _state.gen.mod_amp);
                } else {                     // depth AM: reduce depth, scaled by mod_amp (Hz) vs rate
                    float a = constrain(_state.gen.mod_amp / fmaxf(_state.gen.rate_hz, 0.1f), 0.0f, 1.0f);
                    depth = constrain(depth * (1.0f - 0.5f * m * a), 0.02f, 1.0f);
                }
            }

            // Advance carrier phase and map into the working window.
            _state.gen_phase += dt * rate;
            if (_state.gen_phase > 1.0f) _state.gen_phase -= floorf(_state.gen_phase);
            float c = kinematics::ease(kinematics::carrier(_state.gen.wave, _state.gen_phase),
                                       _state.gen.ease);

            float lo = _mapper.getMinMm(), hi = _mapper.getMaxMm();
            float span = hi - lo;
            float center = lo + _state.gen.offset * span;
            float pos = center + (c - 0.5f) * depth * span;
            pos = constrain(pos, lo, hi);

            // streamTo() re-targets without force-stopping; at high Hz the motor
            // glides smoothly through the waveform. speed 0 => configured max.
            _motor.streamTo(pos, 0.0f);
        } else {
            _state.gen_last_us = 0;   // reset dt so resume doesn't take a giant step
        }

        vTaskDelay(period);
    }
}