// Motor Controller Header - SlopDrive-32
#ifndef MOTOR_H
#define MOTOR_H

#include <Arduino.h>
#include "config.h"

// Forward declarations
class TMC2160Stepper;
class FastAccelStepper;

struct DriverConfig {
    uint16_t microsteps = MICROSTEPS;
    uint16_t run_current_ma = TMC_RUN_CURRENT_MA;
    uint8_t  hold_current_pct = TMC_HOLD_CURRENT_PCT;
    int8_t   stallguard_dma = TMC_STALLGUARD_DMA;
    uint8_t  toff = TMC_TOFF;
    uint8_t  tbl = TMC_TBL;
    uint8_t  stealthchop = TMC_STEALTHCHOP;   // 1=StealthChop, 0=SpreadCycle
    uint32_t tpwm_thrs = TMC_TPWM_THRS;
    int8_t   hstart = TMC_HSTART;
    int8_t   hend = TMC_HEND;
};

class MotorController {
public:
    MotorController();
    
    void begin();
    void enable();
    void disable();
    
    // Homing
    bool home(int32_t home_speed_steps_s = 4000);
    void runHomingStep();
    bool isHomed() const { return _homed; }
    bool isHoming() const { return _homing; }

    // Push-to-home: when NOT homed and NOT actively homing, the user can simply
    // push the shaft into the endstop to establish home without the web UI.
    // Call this periodically; if it sees the endstop active it zeroes position
    // and marks the motor homed. Returns true if it just homed this call.
    bool checkPushToHome();


    // Position monitor - call frequently to stop motor when target reached
    void runMotorStep();

    // Movement
    void moveTo(float pos_mm);
    // Smooth streaming move for Intiface/TCode: re-targets on the fly WITHOUT
    // force-stopping. speed_mm_s<=0 uses the configured max speed.
    void streamTo(float pos_mm, float speed_mm_s);

    // --- Predictive extrapolation (input "inertia") ---
    // Problem: Intiface sends discrete position samples ~every 20-50ms. Moving
    // exactly TO each sample means the motor is always "catching up" and briefly
    // settling at each point, which feels choppy and can't keep up with fast
    // motion. Instead, streamExtrapolated() measures the velocity implied by the
    // incoming samples and commands the motor to a point slightly AHEAD - it
    // keeps coasting past the last sample, in the same direction, for a short
    // lookahead window. The next sample corrects any error, so motion stays
    // continuous and fluid. Overshoot is bounded and a stall timeout prevents
    // runaway if the stream stops.
    void streamExtrapolated(float pos_mm, float speed_mm_s);
    // Call every motor tick: enforces the stall fallback (settle on the true
    // last sample if no new command arrived recently).
    void updateExtrapolation();
    // Tuning (all settable live from the web UI):
    void setLookaheadMs(uint16_t ms) { _lookahead_ms = ms; }
    void setMaxOvershootMm(float mm) { _max_overshoot_mm = constrain(mm, 0.0f, 50.0f); }
    uint16_t getLookaheadMs() const { return _lookahead_ms; }
    float getMaxOvershootMm() const { return _max_overshoot_mm; }

    void stop();           // Decelerate stop
    void hardStop();       // Immediate stop
    
    // Speed & Acceleration
    void setMaxSpeed(float speed_mm_s);
    void setAcceleration(float accel_mm_s2);
    float getMaxSpeed() const { return _max_speed_mm_s; }
    
    // Status
    bool isMoving();
    float getPosition() const;
    float getTargetPosition() const;
    
    // Driver config
    void applyDriverConfig(const DriverConfig& cfg);
    
    // Diagnostics
    uint16_t getCurrentmA();
    uint8_t getMicrosteps();

    // Unit conversion (static for external use)
    static int32_t mmToSteps(float mm);
    static float stepsToMm(int32_t steps);

private:
    TMC2160Stepper* _tmc = nullptr;
    FastAccelStepper* _stepper = nullptr;
    
    bool _homed = false;
    bool _homing = false;
    bool _enabled = false;
    bool _moving_to_target = false;
    int32_t _target_steps = 0;
    
    float _max_speed_mm_s = MAX_SPEED_MM_S;
    float _accel_mm_s2 = DEFAULT_ACCEL_MM_S2;
    float _current_position_mm = 0.0f;

    // Predictive extrapolation state
    uint16_t _lookahead_ms = 20;        // how far ahead to project (ms)
    float    _max_overshoot_mm = 8.0f;  // cap on how far past a sample we coast
    float    _last_sample_mm = 0.0f;    // last TRUE sample target (unprojected)
    float    _stream_velocity = 0.0f;   // implied mm/s from the input stream
    uint32_t _last_sample_ms = 0;       // arrival time of last sample
    bool     _have_last_sample = false; // seeded after first sample
    bool     _coasting = false;         // currently projected past a sample
    static const uint16_t STREAM_STALL_MS = 80;  // no sample => settle on truth
};

#endif // MOTOR_H