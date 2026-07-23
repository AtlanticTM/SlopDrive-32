/**
 * MotionGeometry — runtime steps/rev → steps/mm for the AIM servo build. :3
 *
 * The AIM drive's steps/rev is an electronic-gear register (0x0B) that the
 * Configure pane can reprogram over RS485 Modbus. The firmware's step<->mm
 * math has to follow it EXACTLY or every commanded millimetre is a lie, so
 * the value lives here as runtime state:
 *
 *   - Seeded from NVS (namespace "servocfg", key "mspr") in setup(), default
 *     AIM_MOTOR_STEPS_PER_REV_DEFAULT (800 — the OSSM/factory standard).
 *   - aimSetMotorStepsPerRev() recomputes steps/mm live and persists, called
 *     by WebUI's servo-program path the moment reg 0x0B is written. The
 *     caller is responsible for forcing a re-home — the position reference's
 *     step<->mm mapping is void after the change.
 *
 * Thread-safety: readers (Core 1 motion path) hit a single aligned volatile
 * float / uint16 — 32-bit stores are atomic on Xtensa, so a mid-change read
 * sees old or new, never a torn value. Changes are additionally gated on
 * machine-idle by the caller.
 */

#include "config_api.h"

#if defined(DRIVER_AIM_SERVO)

#include <Preferences.h>
#include "sloplog/sloplog.h"

static volatile float    s_steps_per_mm  = AIM_STEPS_PER_MM_DEFAULT;
static volatile uint16_t s_motor_spr     = AIM_MOTOR_STEPS_PER_REV_DEFAULT;

static const char* NVS_NS  = "servocfg";
static const char* NVS_KEY = "mspr";

// Sanity bounds for a motor-shaft steps/rev. The drive register (0x0B)
// accepts 1–65535; anything outside this band on a capstan rig is either a
// typo or an experiment that deserves a hard stop here rather than a 2000
// mm/s carriage. 50 → 1.27 steps/mm, 32767 → 834 steps/mm.
static constexpr uint16_t SPR_MIN = 50;
static constexpr uint16_t SPR_MAX = 32767;

static float computeStepsPerMm(uint16_t motor_spr) {
    return ((float)motor_spr * AIM_REDUCTION) / AIM_MM_PER_REV;
}

void aimGeometryInit() {
    Preferences prefs;
    uint16_t spr = AIM_MOTOR_STEPS_PER_REV_DEFAULT;
    if (prefs.begin(NVS_NS, /*readOnly=*/true)) {
        spr = prefs.getUShort(NVS_KEY, AIM_MOTOR_STEPS_PER_REV_DEFAULT);
        prefs.end();
    }
    if (spr < SPR_MIN || spr > SPR_MAX) spr = AIM_MOTOR_STEPS_PER_REV_DEFAULT;
    s_motor_spr    = spr;
    s_steps_per_mm = computeStepsPerMm(spr);
    SLOGI("geom", "MotionGeometry: steps/rev=%u (motor) -> %.3f steps/mm%s",
          (unsigned)spr, s_steps_per_mm,
          (spr == AIM_MOTOR_STEPS_PER_REV_DEFAULT) ? " (default)" : " (NVS)");
}

void aimSetMotorStepsPerRev(uint16_t steps_per_rev, bool persist) {
    if (steps_per_rev < SPR_MIN) steps_per_rev = SPR_MIN;
    if (steps_per_rev > SPR_MAX) steps_per_rev = SPR_MAX;
    uint16_t old_spr = s_motor_spr;
    float    old_spm = s_steps_per_mm;
    s_motor_spr    = steps_per_rev;
    s_steps_per_mm = computeStepsPerMm(steps_per_rev);
    if (persist) {
        Preferences prefs;
        if (prefs.begin(NVS_NS, /*readOnly=*/false)) {
            prefs.putUShort(NVS_KEY, steps_per_rev);
            prefs.end();
        }
    }
    SLOGW("geom", "MotionGeometry: steps/rev %u -> %u, steps/mm %.3f -> %.3f%s — RE-HOME REQUIRED",
          (unsigned)old_spr, (unsigned)steps_per_rev, old_spm, s_steps_per_mm,
          persist ? " (persisted)" : "");
}

uint16_t aimMotorStepsPerRev() { return s_motor_spr; }
int32_t  aimStepsPerRev()      { return (int32_t)((float)s_motor_spr * AIM_REDUCTION); }
float    aimStepsPerMm()       { return s_steps_per_mm; }

#endif // defined(DRIVER_AIM_SERVO)
