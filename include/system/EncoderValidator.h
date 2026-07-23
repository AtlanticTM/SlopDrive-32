#pragma once

// ============================================================================
// EncoderValidator — cross-checks FAS commanded position against the drive's
// absolute encoder (regs 0x16/0x17 over RS485 Modbus)
// ============================================================================
//
// The step/dir link to the AIM drive is open-loop from the ESP32's point of
// view: FastAccelStepper counts the pulses it EMITTED, the drive's 15-bit
// encoder (32768 counts/motor-rev) reports where the shaft ACTUALLY is. This
// module compares the two so lost steps, capstan rope slip, or a shifted home
// reference get surfaced instead of silently corrupting every position until
// the next re-home.
//
// Reference model: the encoder's zero is wherever the shaft sat at drive
// power-on, so after each homing we latch a reference pair (enc0 counts,
// fas0 mm) — at the FIRST STANDSTILL, never on the homed edge itself, because
// an encoder sample can be ~270ms stale and a mid-backoff read would bake
// several mm of skew into the reference as a permanent phantom offset.
// Direction between the two frames depends on wiring + DIR polarity, so the
// sign and effective counts/mm are MEASURED between two standstill anchors
// ≥8mm apart (and checked against theory — a scale mismatch means the
// geometry model is wrong, which is a louder finding than any drift).
//
// Verdicts are STANDSTILL-ONLY: an encoder sample is read over a ~20ms Modbus
// transaction while FAS keeps stepping, so mid-motion comparison carries up to
// ~15mm of pure timing skew at speed. The live deviation is still exposed for
// the UI (labelled as noisy-while-moving); the lost-steps warning only scores
// samples taken while both FAS and the drive report the machine still.
//
// REPORT-ONLY by doctrine: this module never touches the arbiter or the
// driver. A false positive that kills a session is worse than a warning the
// operator evaluates — wire it to a gate later only with real-world stats. :3
//
// Runs entirely on Core 0 httpTask (same task as ServoModbus::update() and the
// HTTP handlers — see servo-configure architecture), so no locking is needed.

#include "config_api.h"

#if defined(FEATURE_RS485_MODBUS) && defined(DRIVER_AIM_SERVO)

#include <Arduino.h>

class ServoModbus;
class MotorDriver;

struct EncoderValidation {
    // 0 = idle (not homed / no encoder), 1 = measuring sign, 2 = tracking
    uint8_t  state          = 0;
    bool     warn           = false;  // lost-steps warning latched (until re-home)
    int8_t   sign           = 0;      // measured encoder direction vs FAS (+1/-1)
    float    dev_mm         = 0.0f;   // latest deviation — noisy while moving
    float    dev_steady_mm  = 0.0f;   // latest deviation measured at standstill
    float    max_steady_mm  = 0.0f;   // worst |standstill deviation| since reference
    float    cpmm_meas      = 0.0f;   // measured counts/mm at sign latch (0 = not yet)
    int32_t  enc_counts     = 0;      // raw encoder counts (drive frame)
    uint32_t sample_age_ms  = 0;      // age of the encoder sample behind dev_mm
    bool     have_dev       = false;  // dev_mm/dev_steady_mm carry real data
};

class EncoderValidator {
public:
    EncoderValidator(ServoModbus& bus, MotorDriver& motor);

    // ---- Lifecycle -----------------------------------------------------------
    void init() {}
    /// Call from httpTask right after servoModbus.update(). Cheap: does work
    /// only when a NEW encoder sample has committed (~3.7 Hz).
    void update();
    void emergencyStop() {}   // report-only module — nothing to stop

    /// Snapshot for /api/servo. Same-task as update(), no locking needed.
    const EncoderValidation& get() const { return _v; }

private:
    ServoModbus& _bus;
    MotorDriver& _motor;

    EncoderValidation _v;

    // Reference pair latched on homed-rising-edge
    int32_t  _enc0 = 0;
    float    _fas0 = 0.0f;
    uint32_t _last_stamp = 0;     // enc_stamp_ms of the last consumed sample
    float    _prev_fas_mm = 0.0f; // FAS position at the previous sample
    bool     _have_prev = false;  // _prev_fas_mm carries a real sample
    uint8_t  _steady_streak = 0;  // consecutive samples with the machine still
    uint8_t  _over_count = 0;     // consecutive steady samples over threshold

    void _reset();
};

#endif // FEATURE_RS485_MODBUS && DRIVER_AIM_SERVO
