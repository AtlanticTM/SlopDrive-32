#include "ConfigStore.h"

#include <Preferences.h>
#include "AppLog.h"
#include "config_api.h"
#include "MotorDriver.h"
#include "range_mapper.h"

// ============================================================================
// ConfigStore::save — persist all runtime settings to NVS
// ============================================================================

void ConfigStore::save(SystemState& state, RangeMapper& mapper, MotorDriver& motor) {
    // Inflate the NVS storage with the current running state — every stroke
    // window limit, every current setting, every tune knob the user has
    // lubed up gets packed tight and sealed away. Next boot, it all leaks
    // back out exactly as it was, like a well-trained bladder holding it in. :3
    Preferences prefs;
    if (!prefs.begin("strokeengine", false)) {   // false = read-WRITE
        applog("saveConfig: failed to open NVS for write!");
        return;
    }
    prefs.putFloat("range_min", mapper.getMinMm());
    prefs.putFloat("range_max", mapper.getMaxMm());
    // Speed stored as UShort (max 65535) — max speed is 10000 mm/s, fits fine.
    // Accel stored as UInt (uint32_t) — expert mode allows 100000 mm/s² which
    // overflows a uint16_t (max 65535). Old "accel key" was UShort and silently
    // truncated anything above 65535 — that was the save bug. New key "accel32"
    // uses putUInt so the full 100000 survives the round-trip. yippie! :3
    prefs.putUShort("max_speed", (uint16_t)state.config.max_speed_mm_s);
    prefs.putUInt("accel32", (uint32_t)state.config.acceleration_mm_s2);
    // Continuous-blend policy (1=let-it-land, 2=allow-reversal, 3=hybrid).
    // Replaces the old lookahead/overshoot NVS keys — those tunables retired
    // when the predictive extrapolator got thrown out. :3
    prefs.putUChar("blend_mode", motor.getBlendMode());

    // Dual limit sets (v0.4 / D4 Phase 3) — persist per-source ceilings
    prefs.putUShort("user_spd", (uint16_t)state.config.user_max_speed_mm_s);
    prefs.putUInt("user_acc", (uint32_t)state.config.user_max_accel_mm_s2);
    prefs.putUShort("inp_spd", (uint16_t)state.config.input_max_speed_mm_s);
    prefs.putUInt("inp_acc", (uint32_t)state.config.input_max_accel_mm_s2);

    prefs.putBool("auto_dur", state.auto_duration);
    // Intiface compat — whether we decode magnitudes against the legacy /999
    // ceiling (Intiface's mangled scale) or the spec-correct digit count (MFP).
    // Persisted so the operator's chosen app stays satisfied across reboots. :3
    prefs.putBool("if_compat", state.intiface_compat);
    prefs.putFloat("def_rmin", state.default_range_min);
    prefs.putFloat("def_rmax", state.default_range_max);
    prefs.putBool("expert", state.expert_mode);
    prefs.putUChar("transport", (uint8_t)state.getTransport());

    // Input mode + buffered interpolation + generator tick rate
    prefs.putUChar("input_mode", (uint8_t)state.getInputMode());
    prefs.putUChar("buf_easing", state.buf_easing);
    prefs.putUChar("buf_depth", state.buf_depth);
    prefs.putUShort("buf_tick", state.buf_tick_hz);
    prefs.putUShort("gen_tick", state.gen_rate_tick_hz);

    // TMC driver tunables (from the Motor tab)
    prefs.putUShort("tmc_run", state.driver.run_current_ma);
    prefs.putUChar("tmc_hold", state.driver.hold_current_pct);
    prefs.putUChar("tmc_sc", state.driver.stealthchop);
    prefs.putUInt("tmc_tpwm", state.driver.tpwm_thrs);
    prefs.putUChar("tmc_toff", state.driver.toff);
    prefs.putUChar("tmc_tbl", state.driver.tbl);
    prefs.putChar("tmc_hs", state.driver.hstart);
    prefs.putChar("tmc_he", state.driver.hend);

    prefs.end();
    applogf("Config saved to NVS: range=[%.1f,%.1f] speed=%.0f accel=%.0f blend=%u",
            mapper.getMinMm(), mapper.getMaxMm(),
            state.config.max_speed_mm_s, state.config.acceleration_mm_s2,
            motor.getBlendMode());
}

// ============================================================================
// ConfigStore::load — load persisted settings (or factory defaults)
// ============================================================================

void ConfigStore::load(SystemState& state, RangeMapper& mapper, MotorDriver& motor) {
    // Always start with defaults
    state.config = getDefaultConfig();
    state.driver = DriverConfig();   // default-constructed = config.h defaults
    mapper.setRange(state.config.min_position_mm, state.config.max_position_mm);

    Preferences prefs;

    // Try to load saved values (read-only mode)
    if (prefs.begin("strokeengine", true)) {
        float rmin = prefs.getFloat("range_min", state.config.min_position_mm);
        float rmax = prefs.getFloat("range_max", state.config.max_position_mm);
        uint16_t spd = prefs.getUShort("max_speed", (uint16_t)state.config.max_speed_mm_s);
        // Read accel from the new uint32_t key "accel32" first. If it's absent
        // (device was previously saved with the old uint16_t "accel" key), fall
        // back to the old key so existing saves aren't silently reset to default.
        // Once the user saves again, "accel32" gets written and the old key is
        // ignored forever. Smooth migration, no data loss. yippie! :3
        uint32_t acc32_saved = prefs.getUInt("accel32", 0);
        uint32_t acc;
        if (acc32_saved > 0) {
            acc = acc32_saved;
        } else {
            // Legacy fallback: old uint16_t key — max it could hold was 65535.
            // If the old key is also absent, getUShort returns the default. :3
            acc = (uint32_t)prefs.getUShort("accel", (uint16_t)state.config.acceleration_mm_s2);
        }
        uint8_t blend = prefs.getUChar("blend_mode", 1);  // 1=let-it-land default

        // Dual limit sets (v0.4 / D4 Phase 3) — load with legacy migration.
        // If the new keys exist, use them. Otherwise seed both sets from the
        // legacy max_speed/accel values already loaded above (spd, acc).
        float usr_spd = spd, usr_acc = (float)acc, inp_spd = spd, inp_acc = (float)acc;
        uint16_t usr_spd_saved = prefs.getUShort("user_spd", 0);
        uint32_t usr_acc_saved = prefs.getUInt("user_acc", 0);
        uint16_t inp_spd_saved = prefs.getUShort("inp_spd", 0);
        uint32_t inp_acc_saved = prefs.getUInt("inp_acc", 0);
        if (usr_spd_saved > 0) usr_spd = (float)usr_spd_saved;
        if (usr_acc_saved > 0) usr_acc = (float)usr_acc_saved;
        if (inp_spd_saved > 0) inp_spd = (float)inp_spd_saved;
        if (inp_acc_saved > 0) inp_acc = (float)inp_acc_saved;
        state.config.user_max_speed_mm_s   = usr_spd;
        state.config.user_max_accel_mm_s2  = usr_acc;
        state.config.input_max_speed_mm_s  = inp_spd;
        state.config.input_max_accel_mm_s2 = inp_acc;

        state.auto_duration = prefs.getBool("auto_dur", true);
        // Intiface compat — default false (spec-correct/MFP decode) when the key
        // was never written. main.cpp pushes this into TCodeParser::intifaceCompat
        // right after load() so the parser is in lockstep from the first frame. :3
        state.intiface_compat = prefs.getBool("if_compat", false);

        state.default_range_min = prefs.getFloat("def_rmin", 0.0f);
        state.default_range_max = prefs.getFloat("def_rmax", MACHINE_MAX_TRAVEL_MM);
        state.expert_mode = prefs.getBool("expert", false);
        {
            TransportMode t = (TransportMode)prefs.getUChar("transport", (uint8_t)DEFAULT_TRANSPORT_MODE);
            if ((uint8_t)t > (uint8_t)TransportMode::BT) t = DEFAULT_TRANSPORT_MODE;
            state.setTransport(t);
        }

        // Input mode + buffered interpolation + generator tick rate
        {
            InputMode im = (InputMode)prefs.getUChar("input_mode", (uint8_t)InputMode::BUFFERED);
            if ((uint8_t)im > (uint8_t)InputMode::BUFFERED) im = InputMode::EXTRAPOLATE;
            state.setInputMode(im);
        }
        state.buf_easing = constrain((int)prefs.getUChar("buf_easing", state.buf_easing), 0, 4);
        state.buf_depth  = constrain((int)prefs.getUChar("buf_depth", state.buf_depth), 1, 5);
        { uint16_t bt = prefs.getUShort("buf_tick", state.buf_tick_hz);
          state.buf_tick_hz = (bt >= 150) ? 200 : (bt >= 75) ? 100 : (bt >= 35) ? 50 : 20; }
        // Gen tick rungs updated: 20/50/100/250/500 Hz. Old saved value of 200
        // snaps to 250 on load — close enough, and the operator can re-save. :3
        { uint16_t gt = prefs.getUShort("gen_tick", state.gen_rate_tick_hz);
          state.gen_rate_tick_hz = (gt >= 375) ? 500 : (gt >= 175) ? 250 : (gt >= 75) ? 100 : (gt >= 35) ? 50 : 20; }

        // Validate startup defaults; fall back to full travel if nonsensical.
        // Uses MACHINE_MAX_TRAVEL_MM so the 57AIM build clamps to 260mm, not 240mm. :3
        if (state.default_range_min < 0.0f || state.default_range_min > MACHINE_MAX_TRAVEL_MM ||
            state.default_range_max <= 0.0f || state.default_range_max > MACHINE_MAX_TRAVEL_MM ||
            state.default_range_min >= state.default_range_max) {
            state.default_range_min = 0.0f;
            state.default_range_max = MACHINE_MAX_TRAVEL_MM;
        }

        // TMC tunables
        state.driver.run_current_ma   = prefs.getUShort("tmc_run", state.driver.run_current_ma);
        state.driver.hold_current_pct = prefs.getUChar("tmc_hold", state.driver.hold_current_pct);
        state.driver.stealthchop      = prefs.getUChar("tmc_sc", state.driver.stealthchop);
        state.driver.tpwm_thrs        = prefs.getUInt("tmc_tpwm", state.driver.tpwm_thrs);
        state.driver.toff             = prefs.getUChar("tmc_toff", state.driver.toff);
        state.driver.tbl              = prefs.getUChar("tmc_tbl", state.driver.tbl);
        state.driver.hstart           = prefs.getChar("tmc_hs", state.driver.hstart);
        state.driver.hend             = prefs.getChar("tmc_he", state.driver.hend);
        prefs.end();

        // Validate: reject 0 or out-of-range values. MACHINE_MAX_TRAVEL_MM is
        // driver-aware (260mm for 57AIM, 240mm for TMC) so a saved 250mm range_max
        // doesn't get clamped back to 240 on the 57AIM build. :3
        if (rmin < 0.0f || rmin > MACHINE_MAX_TRAVEL_MM) rmin = state.config.min_position_mm;
        if (rmax <= 0.0f || rmax > MACHINE_MAX_TRAVEL_MM) rmax = state.config.max_position_mm;
        if (rmin >= rmax) { rmin = state.config.min_position_mm; rmax = state.config.max_position_mm; }
        // Speed: floor at 1, ceiling at MAX_SPEED_MM_S (10000). Anything outside
        // that window is a corrupt NVS write — reset to the running default. :3
        if (spd == 0 || spd > (uint16_t)MAX_SPEED_MM_S) spd = (uint16_t)state.config.max_speed_mm_s;
        // Accel: floor at 10 (zero/garbage = bricked planner), ceiling at
        // MAX_ACCEL_MM_S2 (100000). Expert mode can write up to 100000 mm/s²
        // and the servo drive can take it — we just need to not reject it on
        // load. The old ceiling of 30000 was silently resetting any expert-mode
        // accel save back to the 8000 default. That was the bug. Fixed. :3
        if (acc < 10 || acc > (uint32_t)MAX_ACCEL_MM_S2) acc = (uint32_t)state.config.acceleration_mm_s2;


        if (state.driver.run_current_ma < 250 || state.driver.run_current_ma > 3000)
            state.driver.run_current_ma = TMC_RUN_CURRENT_MA;
        if (state.driver.toff < 1 || state.driver.toff > 15) state.driver.toff = TMC_TOFF;

        mapper.setRange(rmin, rmax);
        state.config.max_speed_mm_s = (float)spd;
        state.config.acceleration_mm_s2 = (float)acc;
        state.config.run_current_ma = state.driver.run_current_ma;

        // Apply persisted continuous-blend policy to the motor. Clamp 1..3 so a
        // corrupt/legacy NVS value can't put us in an undefined mode. :3
        if (blend < 1 || blend > 3) blend = 1;
        motor.setBlendMode(blend);

        applogf("Config: range=[%.1f, %.1f] speed=%.0f accel=%.0f run=%umA blend=%u",
                rmin, rmax, (float)spd, (float)acc, state.driver.run_current_ma, blend);

    } else {
        prefs.end();
        applog("No saved config, using defaults");
    }
}