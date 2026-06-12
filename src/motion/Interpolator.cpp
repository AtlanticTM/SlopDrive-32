#include "Interpolator.h"
#include "Kinematics.h"
#include "range_mapper.h"
#include "MotorDriver.h"
#include "AppLog.h"
#include "config_api.h"

#include <Arduino.h>
#include <esp_timer.h>

// ============================================================================
// Interpolator — constructor / destructor
// ============================================================================

Interpolator::Interpolator(SystemState& state, RangeMapper& mapper, MotorDriver& motor)
    : _state(state), _mapper(mapper), _motor(motor)
{}

Interpolator::~Interpolator() {
    if (_task) vTaskDelete(_task);
}

// ============================================================================
// Lifecycle
// ============================================================================

void Interpolator::init() {
    xTaskCreatePinnedToCore(taskFunction, "Interp", 4096, this, 2, &_task, 1);
}

void Interpolator::emergencyStop() {
    _state.buf_active = false;
    _play_clock_ms = 0;
    _last_real_us = 0;
    portENTER_CRITICAL(&_state.buf_mux);
    _state.buf_count = 0;
    _state.buf_head = 0;
    portEXIT_CRITICAL(&_state.buf_mux);
}

void Interpolator::pushSample(float pos_mm) {
    portENTER_CRITICAL(&_state.buf_mux);
    _state.buf[_state.buf_head] = { pos_mm, (uint32_t)(esp_timer_get_time() / 1000ULL) };
    _state.buf_head = (_state.buf_head + 1) % SystemState::BUF_CAP;
    if (_state.buf_count < SystemState::BUF_CAP) _state.buf_count++;
    portEXIT_CRITICAL(&_state.buf_mux);
}

bool Interpolator::isActive() const { return _state.buf_active; }

// ============================================================================
// Static trampoline → member function
// ============================================================================

void Interpolator::taskFunction(void* param) {
    static_cast<Interpolator*>(param)->run();
}

// ============================================================================
// Task loop — functionally identical to the original interpolatorTask() in
// main.cpp.  No behaviour changes; pure structural refactor.
// ============================================================================
//
// TIME-DELAYED JITTER BUFFER.
//
// Why the old "advance a cursor by the assumed tick period" approach jittered:
//   1. vTaskDelay() is NOT isochronous. With WiFi+BLE+web-server running, this
//      task is preempted; a "20ms" tick can land anywhere from 18 to 45ms. The
//      old code advanced the cursor by a FIXED 1000/hz ms regardless, so late
//      ticks under-advanced and the carriage stuttered.
//   2. It used each sample's raw BLE arrival delta (b.t_ms - a.t_ms) as the
//      segment duration. BLE delivers in bursts (connection-interval batching),
//      so those deltas are themselves jittery — feeding transport jitter
//      straight into motor velocity.
//   3. `depth` was only a look-behind COUNT, giving no real time cushion, so any
//      BLE gap bigger than the tiny backlog drained the ring → freeze → refill.
//
// The fix is the standard media jitter-buffer model: timestamp every sample on
// arrival, then PLAY BACK on a clock that runs a fixed delay (PLAY_DELAY_MS)
// behind real wall-clock time. Each tick we:
//   * read the REAL elapsed time (esp_timer, immune to tick jitter),
//   * compute play_time = now - PLAY_DELAY_MS,
//   * find the two buffered samples that bracket play_time,
//   * interpolate by the TRUE time fraction between them.
// Because we always render "the past", bursty/late arrivals are already in the
// buffer by the time we need them, and uneven tick spacing is corrected by
// using real time instead of an assumed step. Result: smooth motion even over
// jittery BLE.

void Interpolator::run() {
    while (true) {
        uint16_t hz = _state.buf_tick_hz; if (hz < 5) hz = 5; if (hz > 200) hz = 200;
        TickType_t period = pdMS_TO_TICKS(1000 / hz);

        bool gated = _state.paused || _state.manual_override || !_state.homed;
        bool run = (_state.getInputMode() == InputMode::BUFFERED) && !gated;

        if (run) {
            uint32_t now_us = (uint32_t)esp_timer_get_time();
            uint32_t now_ms = now_us / 1000U;

            // The buffer delay (ms) the user dials in via `depth`. Each depth step
            // ~= 30ms of cushion (1->30ms .. 5->150ms). More delay = smoother ride
            // over BLE stalls at the cost of a little extra latency.
            uint8_t depth = _state.buf_depth; if (depth < 1) depth = 1; if (depth > 5) depth = 5;
            uint32_t play_delay_ms = (uint32_t)depth * 30U;

            // Snapshot the ring.
            portENTER_CRITICAL(&_state.buf_mux);
            uint8_t count = _state.buf_count;
            uint8_t head  = _state.buf_head;
            BufSample ring[SystemState::BUF_CAP];
            for (uint8_t i = 0; i < SystemState::BUF_CAP; i++) ring[i] = _state.buf[i];
            portEXIT_CRITICAL(&_state.buf_mux);

            if (count >= 2) {
                // Oldest valid sample index and the newest timestamp.
                uint8_t oldest_i = (uint8_t)((head + SystemState::BUF_CAP - count) % SystemState::BUF_CAP);
                uint8_t newest_i = (uint8_t)((head + SystemState::BUF_CAP - 1) % SystemState::BUF_CAP);
                uint32_t oldest_t = ring[oldest_i].t_ms;
                uint32_t newest_t = ring[newest_i].t_ms;

                // Advance the playback clock by REAL elapsed time (tick-jitter
                // immune). On the first primed tick, start it one buffer-delay
                // behind "now" so we render from the oldest available history.
                if (_play_clock_ms == 0 || !_state.buf_active) {
                    _play_clock_ms = (now_ms > play_delay_ms) ? (now_ms - play_delay_ms) : oldest_t;
                    if (_play_clock_ms < oldest_t) _play_clock_ms = oldest_t;
                    _last_real_us = now_us;
                    _state.buf_active = true;
                } else {
                    uint32_t elapsed_ms = (now_us - _last_real_us) / 1000U;
                    if (elapsed_ms > 0) {
                        _play_clock_ms += elapsed_ms;
                        _last_real_us = now_us;
                    }
                }

                // Clamp the playback clock into the available window:
                //  * never run past the newest sample we have (would extrapolate
                //    blindly); hold at newest if the buffer starved.
                //  * never fall so far behind that we'd render ancient history.
                uint32_t target_lag = newest_t - play_delay_ms;   // ideal play point
                if ((int32_t)(_play_clock_ms - newest_t) > 0) {
                    _play_clock_ms = newest_t;                      // starved: hold newest
                }
                // Gentle resync if we've drifted too far from the ideal lag
                // (keeps latency bounded without snapping).
                if (newest_t > play_delay_ms &&
                    (int32_t)(target_lag - _play_clock_ms) > (int32_t)(play_delay_ms + 50)) {
                    _play_clock_ms = target_lag;                    // jumped behind: catch up
                }
                if (_play_clock_ms < oldest_t) _play_clock_ms = oldest_t;

                // Find the pair of samples bracketing play_clock_ms.
                BufSample a = ring[oldest_i];
                BufSample b = ring[newest_i];
                for (uint8_t k = 0; k < count - 1; k++) {
                    uint8_t i0 = (uint8_t)((oldest_i + k) % SystemState::BUF_CAP);
                    uint8_t i1 = (uint8_t)((i0 + 1) % SystemState::BUF_CAP);
                    if ((int32_t)(_play_clock_ms - ring[i0].t_ms) >= 0 &&
                        (int32_t)(ring[i1].t_ms - _play_clock_ms) >= 0) {
                        a = ring[i0]; b = ring[i1];
                        break;
                    }
                }

                // True time fraction between the bracketing samples.
                float seg_ms = (float)(b.t_ms - a.t_ms);
                float t = (seg_ms > 0.5f)
                              ? ((float)((int32_t)(_play_clock_ms - a.t_ms)) / seg_ms)
                              : 1.0f;
                t = constrain(t, 0.0f, 1.0f);

                float e = kinematics::bufEase(_state.buf_easing, t);
                float pos = a.pos_mm + (b.pos_mm - a.pos_mm) * e;
                pos = constrain(pos, _mapper.getMinMm(), _mapper.getMaxMm());

                // Velocity to reach the next sample over its real remaining time,
                // so motion stays continuous (no per-tick speed spikes).
                float remain_ms = (float)((int32_t)(b.t_ms - _play_clock_ms));
                if (remain_ms < 1.0f) remain_ms = seg_ms > 0.5f ? seg_ms : 20.0f;
                float dist = fabsf(b.pos_mm - pos);
                float spd  = (dist / remain_ms) * 1000.0f;
                spd = constrain(spd, 0.0f, _state.config.max_speed_mm_s);
                _motor.streamTo(pos, spd);

                // Retire samples strictly older than what we still need to
                // interpolate (keep the one just before play_clock_ms as `a`).
                portENTER_CRITICAL(&_state.buf_mux);
                while (_state.buf_count > 2) {
                    uint8_t o  = (uint8_t)((_state.buf_head + SystemState::BUF_CAP - _state.buf_count) % SystemState::BUF_CAP);
                    uint8_t o1 = (uint8_t)((o + 1) % SystemState::BUF_CAP);
                    // Drop the oldest only if the playback clock has already passed
                    // its successor (so `a/b` bracketing never loses its left edge).
                    if ((int32_t)(_play_clock_ms - _state.buf[o1].t_ms) > 0) _state.buf_count--;
                    else break;
                }
                portEXIT_CRITICAL(&_state.buf_mux);
            } else {
                // Fewer than 2 samples: not enough to interpolate yet. Don't
                // reset _state.buf_active for a brief gap, but do pause the clock so it
                // doesn't run away while starved.
                _last_real_us = now_us;
            }
        } else {
            // Not in buffered mode (or gated): clear ring/clock so a later switch
            // back into buffered mode starts fresh.
            emergencyStop();
        }

        vTaskDelay(period);
    }
}