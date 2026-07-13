#include "Generator.h"
#include "Kinematics.h"
#include "range_mapper.h"
#include "MotorDriver.h"
#include "AppLog.h"
#include "config_api.h"

#include <Arduino.h>

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
//
// The generator config is now snapshotted under gen_mux at the top of each
// tick so that live /api/gen writes from Core 0 never produce a torn (half-
// new, half-old) parameter set for a single computation frame. We don't do
// half-and-half here — every stroke gets the full treatment. :3
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

        // ---- Consistent snapshot of the generator config (Core 1 read side) --
        GeneratorConfig cfg;
        portENTER_CRITICAL(&_state.gen_mux);
        cfg = _state.gen;
        portEXIT_CRITICAL(&_state.gen_mux);

        bool intiface_recent = (_state.last_intiface_ms != 0) &&
                               (millis() - _state.last_intiface_ms < 250);
        bool user_has_control = _state.paused || _state.manual_override;
        bool emit = cfg.running && _state.homed &&
                    (user_has_control || !intiface_recent);
        _state.gen_active = emit;

        // Diagnostics: while the generator is running, log once per second so the
        // Log tab reflects what it's doing — either why it's idle, or a heartbeat
        // with the live target while it's actually driving the motor.
        if (cfg.running && (millis() - last_diag_ms > 1000)) {
            last_diag_ms = millis();
            if (!_state.homed)
                APPLOG("Generator: waiting - motor not homed (gotta know where home is before we play~ :3)");
            else if (intiface_recent && !user_has_control)
                APPLOG("Generator: yielding to active Intiface (such a polite sub — use Pause/Override if you wanna top)");
            else
            APPLOGF("Generator: running target=%.1fmm phase=%.2f window=[%.0f,%.0f]",
                    _mapper.getMinMm() + cfg.offset * (_mapper.getMaxMm() - _mapper.getMinMm())
                        + (kinematics::ease(kinematics::carrier(cfg.wave, _state.gen_phase),
                                            cfg.ease) - 0.5f)
                          * cfg.depth * (_mapper.getMaxMm() - _mapper.getMinMm()),
                        _state.gen_phase, _mapper.getMinMm(), _mapper.getMaxMm());
        }

        if (emit) {
            uint32_t now_us = micros();
            float dt = (_state.gen_last_us != 0)
                           ? (float)(int32_t)(now_us - _state.gen_last_us) / 1e6f
                           : 0.0f;
            _state.gen_last_us = now_us;
            if (dt < 0.0f || dt > 0.5f) dt = 0.0f;   // guard against wrap/stall

            float rate  = cfg.rate_hz;
            float depth = cfg.depth;

            // Advance the modulator clock and apply FM (rate) or AM (depth).
            //
            // The FM path literally stretches and contracts the rhythm—each
            // stroke rides an irregular pulse that slams deep at the peak then
            // pulls back shallow when the modulator bottoms out.  AM is the
            // greedy one: it throttles the stroke depth in real time, making
            // the shaft inflate and deflate inside, going from a teasing rim
            // to a full stomach-bulging stretch and back.  The mod clock wraps
            // each cycle so the hole never gets a predictable beat to clench on
            // — it just gets rhythmically fisted open wider owo
            if (cfg.mod != 0) {
                _state.gen_mod_clock += dt * cfg.mod_rate;
                if (_state.gen_mod_clock > 1.0f)
                    _state.gen_mod_clock -= floorf(_state.gen_mod_clock);
                float m = kinematics::modShape(cfg.mod_wave, _state.gen_mod_clock);
                if (cfg.mod == 1) {        // rate FM: swing rate by +/- mod_amp Hz
                    rate = fmaxf(0.05f, rate + (m - 0.5f) * 2.0f * cfg.mod_amp);
                } else {                     // depth AM: reduce depth, scaled by mod_amp (Hz) vs rate
                    float a = constrain(cfg.mod_amp / fmaxf(cfg.rate_hz, 0.1f), 0.0f, 1.0f);
                    depth = constrain(depth * (1.0f - 0.5f * m * a), 0.02f, 1.0f);
                }
            }

            // Advance carrier phase — this is the heartbeat. Each tick nudges
            // the wave forward a tiny bit; when it tips past 1.0 we wrap it
            // back so the shaft cycles: thrust in, stretch the walls to the
            // point they almost tear, then slowly pull back until the tip is
            // barely kissing the rim before the next ram.  The wrap is the
            // moment the stroke bottoms out and starts the withdrawal — that
            // delicious instant where maximum fullness flips to emptiness. :3
            _state.gen_phase += dt * rate;
            if (_state.gen_phase > 1.0f) _state.gen_phase -= floorf(_state.gen_phase);
            float c = kinematics::ease(kinematics::carrier(cfg.wave, _state.gen_phase),
                                       cfg.ease);

            float lo = _mapper.getMinMm(), hi = _mapper.getMaxMm();
            float span = hi - lo;
            float center = lo + cfg.offset * span;
            float pos = center + (c - 0.5f) * depth * span;
            pos = constrain(pos, lo, hi);

            // streamTo() re-targets without force-stopping; at high Hz the motor
            // glides through the waveform like a well-oiled piston. speed 0 =>
            // configured max — let the good boy go full throttle. :3
            //
            // Safe-approach soft start: right after the generator engages (or
            // pause/override releases) the waveform's target can be far from
            // the parked carriage. safeSpeedCap() ramps the speed ceiling up
            // from SAFE_APPROACH_SPEED_MM_S over SAFE_RESUME_RAMP_MS so the
            // first stroke glides in instead of lunging. Once the ramp expires
            // it returns the configured max — we pass 0.0f then so streamTo
            // keeps its normal "use configured max" fast path. :3
            float cap = _state.safeSpeedCap(_state.config.max_speed_mm_s, millis());
            _motor.streamTo(pos, (cap < _state.config.max_speed_mm_s) ? cap : 0.0f);

            // Publish what we just demanded so the UI's target trace tracks the
            // generator too — same "told vs took" overlay as the TCode path. :3
            _state.commanded_target_mm = pos;
            // The generator computes its position straight from the waveform —
            // there's no separate "raw vs planned" stage like the TCode path, so
            // the raw trace just mirrors the demand. Keeps all three lines valid
            // on the graph whether we're driven by Intiface or self-pleasuring. :3
            _state.commanded_raw_mm = pos;


        } else {
            _state.gen_last_us = 0;   // reset dt so resume doesn't take a giant step
        }

        vTaskDelay(period);
    }
}