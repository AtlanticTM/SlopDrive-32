/**
 * EncoderValidator — FAS commanded position vs AIM drive encoder. :3
 *
 * See EncoderValidator.h for the model. Short version: latch a reference pair
 * at homed-rising-edge, measure the encoder's sign (and effective counts/mm)
 * from the first real excursion, then score deviation on every new encoder
 * sample — but only pass verdicts on samples taken at standstill, because a
 * Modbus read of a moving axis is time-skewed by design.
 */

#include "EncoderValidator.h"

#if defined(FEATURE_RS485_MODBUS) && defined(DRIVER_AIM_SERVO)

#include "ServoModbus.h"
#include "MotorDriver.h"
#include "AppLog.h"

// Standstill gates: FAS moved < this between samples AND the drive reports
// (near) zero rpm. Samples are ~270ms apart, so 0.05mm ≈ < 0.2 mm/s.
static constexpr float STEADY_FAS_MM  = 0.05f;
static constexpr float STEADY_RPM     = 3.0f;
// Consecutive steady samples over threshold before the warning latches (~1s
// of sustained disagreement — a single glitched frame can't trip it).
static constexpr uint8_t WARN_SAMPLES = 3;
static constexpr uint32_t WARN_LOG_INTERVAL_MS = 30000;

EncoderValidator::EncoderValidator(ServoModbus& bus, MotorDriver& motor)
    : _bus(bus)
    , _motor(motor)
{
}

void EncoderValidator::_reset() {
    _v = EncoderValidation{};
    _over_count = 0;
    _last_stamp = 0;
    _have_prev = false;
    _steady_streak = 0;
}

void EncoderValidator::update() {
    ServoTelemetry t = _bus.getTelemetry();

    // Not homed (or no encoder on the wire): no reference frame exists. Drop
    // everything — the next homed edge latches a fresh reference, which also
    // covers steps/mm reprogramming (that path force-unhomes the machine). :3
    if (!_motor.isHomed() || !t.enc_valid) {
        if (_v.state != 0) {
            APPLOG("EncoderValidator: reference dropped (unhomed) — will re-latch on next home");
            _reset();
        }
        return;
    }

    // Only do work when a NEW encoder sample has committed.
    if (t.enc_stamp_ms == _last_stamp) return;
    _last_stamp = t.enc_stamp_ms;

    float fas_mm = _motor.getPosition();
    _v.enc_counts    = t.enc_counts;
    _v.sample_age_ms = millis() - t.enc_stamp_ms;

    // ---- Standstill detection ------------------------------------------------
    // An encoder sample is read over a ~20ms Modbus transaction up to ~270ms
    // before we consume it, while FAS keeps stepping — so ONLY samples taken
    // with the machine still are skew-free. Everything that anchors the model
    // (reference latch, sign/scale measurement, verdicts) demands a streak of
    // ≥2 consecutive still samples (~540ms), which also defeats the pattern-
    // turnaround alias where one sample can catch fas equal + rpm near zero. :3
    bool steady = _have_prev &&
                  (fabsf(fas_mm - _prev_fas_mm) < STEADY_FAS_MM) &&
                  (fabsf(t.speed_rpm) < STEADY_RPM);
    _steady_streak = steady ? (uint8_t)(_steady_streak < 255 ? _steady_streak + 1 : 255) : 0;
    _prev_fas_mm = fas_mm;
    _have_prev   = true;
    bool anchored = _steady_streak >= 2;

    // ---- State 0 → 1: latch the reference pair at standstill -----------------
    // Latching on the homed edge itself froze a mid-backoff (stale-by-~270ms)
    // encoder read against a fresh FAS read — several mm of skew baked into
    // the reference as a permanent phantom offset. Wait for genuine rest. :3
    if (_v.state == 0) {
        if (!anchored) return;
        _enc0 = t.enc_counts;
        _fas0 = fas_mm;
        _v.state = 1;
        APPLOGF("EncoderValidator: reference latched at standstill (enc=%ld, fas=%.2fmm) — measuring direction",
                (long)_enc0, _fas0);
        return;
    }

    // ---- State 1: measure encoder sign + effective counts/mm -----------------
    // Scored only between two standstill anchors ≥8mm apart, so the measured
    // counts/mm is clean — it latches at the first pause after real travel,
    // not mid-stroke where skew pollutes the ratio.
    if (_v.state == 1) {
        float   d_fas = fas_mm - _fas0;
        int32_t d_enc = t.enc_counts - _enc0;
        if (anchored && fabsf(d_fas) >= AIM_ENC_SIGN_DETECT_MM) {
            _v.sign      = ((d_enc >= 0) == (d_fas >= 0)) ? 1 : -1;
            _v.cpmm_meas = fabsf((float)d_enc / d_fas);
            _v.state     = 2;
            float ratio = _v.cpmm_meas / AIM_ENC_COUNTS_PER_MM;
            APPLOGF("EncoderValidator: sign=%+d, measured %.1f counts/mm (theory %.1f, ratio %.3f) — tracking",
                    (int)_v.sign, _v.cpmm_meas, (float)AIM_ENC_COUNTS_PER_MM, ratio);
            // Scale disagreement means the geometry model (drum/reduction/
            // 32768-counts-per-rev) is wrong — louder finding than drift.
            // Both anchors are standstill-clean now, so a real mismatch reads
            // as a crisp ratio; the band stays generous vs the 2× errors this
            // hunts (an e-gear/geometry mismatch is never a subtle 10%).
            if (ratio < 0.75f || ratio > 1.25f) {
                APPLOGF("EncoderValidator: WARNING — encoder scale %.1f counts/mm is %.0f%% of theory. "
                        "Deviation numbers are suspect until geometry is reconciled.",
                        _v.cpmm_meas, ratio * 100.0f);
            }
        }
        return;
    }

    // ---- State 2: track deviation --------------------------------------------
    float enc_mm = (float)(t.enc_counts - _enc0) * (float)_v.sign / AIM_ENC_COUNTS_PER_MM;
    float dev    = enc_mm - (fas_mm - _fas0);
    _v.dev_mm   = dev;
    _v.have_dev = true;

    if (!anchored) return;   // verdicts only from skew-free standstill samples

    _v.dev_steady_mm = dev;
    if (fabsf(dev) > _v.max_steady_mm) _v.max_steady_mm = fabsf(dev);

    if (fabsf(dev) > AIM_ENC_DEV_WARN_MM) {
        if (_over_count < 255) _over_count++;
        if (_over_count >= WARN_SAMPLES) {
            _v.warn = true;
            uint32_t now = millis();
            if (now - _last_warn_log_ms > WARN_LOG_INTERVAL_MS) {
                _last_warn_log_ms = now;
                APPLOGF("EncoderValidator: LOST-STEPS WARNING — encoder disagrees with FAS by %+.2fmm "
                        "at standstill (threshold %.1fmm). Position reference is suspect: re-home.",
                        dev, (float)AIM_ENC_DEV_WARN_MM);
            }
        }
    } else {
        _over_count = 0;
        // warn stays latched until re-home — a drift that wandered back is
        // still a drift the operator should know happened. :3
    }
}

#endif // FEATURE_RS485_MODBUS && DRIVER_AIM_SERVO
