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
    Preferences prefs;
    if (!prefs.begin("strokeengine", false)) {   // false = read-WRITE
        applog("saveConfig: failed to open NVS for write!");
        return;
    }
    prefs.putFloat("range_min", mapper.getMinMm());
    prefs.putFloat("range_max", mapper.getMaxMm());
    prefs.putUShort("max_speed", (uint16_t)state.config.max_speed_mm_s);
    prefs.putUShort("accel", (uint16_t)state.config.acceleration_mm_s2);
    prefs.putUShort("lookahead", motor.getLookaheadMs());
    prefs.putUShort("overshoot", (uint16_t)motor.getMaxOvershootMm());
    prefs.putBool("auto_dur", state.auto_duration);
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
    applog("Config saved to NVS");
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
        uint16_t acc = prefs.getUShort("accel", (uint16_t)state.config.acceleration_mm_s2);
        uint16_t look = prefs.getUShort("lookahead", 20);
        uint16_t over = prefs.getUShort("overshoot", 8);
        state.auto_duration = prefs.getBool("auto_dur", true);
        state.default_range_min = prefs.getFloat("def_rmin", 0.0f);
        state.default_range_max = prefs.getFloat("def_rmax", PHYSICAL_MAX_TRAVEL_MM);
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
          state.buf_tick_hz = (bt >= 75) ? 100 : (bt >= 35) ? 50 : 20; }
        { uint16_t gt = prefs.getUShort("gen_tick", state.gen_rate_tick_hz);
          state.gen_rate_tick_hz = (gt >= 75) ? 100 : (gt >= 35) ? 50 : 20; }

        // Validate startup defaults; fall back to full travel if nonsensical.
        if (state.default_range_min < 0.0f || state.default_range_min > PHYSICAL_MAX_TRAVEL_MM ||
            state.default_range_max <= 0.0f || state.default_range_max > PHYSICAL_MAX_TRAVEL_MM ||
            state.default_range_min >= state.default_range_max) {
            state.default_range_min = 0.0f;
            state.default_range_max = PHYSICAL_MAX_TRAVEL_MM;
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

        // Validate: reject 0 or out-of-range values
        if (rmin < 0.0f || rmin > PHYSICAL_MAX_TRAVEL_MM) rmin = state.config.min_position_mm;
        if (rmax <= 0.0f || rmax > PHYSICAL_MAX_TRAVEL_MM) rmax = state.config.max_position_mm;
        if (rmin >= rmax) { rmin = state.config.min_position_mm; rmax = state.config.max_position_mm; }
        if (spd == 0 || spd > (uint16_t)MAX_SPEED_MM_S) spd = (uint16_t)state.config.max_speed_mm_s;
        if (acc == 0 || acc > 5000) acc = (uint16_t)state.config.acceleration_mm_s2;
        if (state.driver.run_current_ma < 250 || state.driver.run_current_ma > 3000)
            state.driver.run_current_ma = TMC_RUN_CURRENT_MA;
        if (state.driver.toff < 1 || state.driver.toff > 15) state.driver.toff = TMC_TOFF;

        mapper.setRange(rmin, rmax);
        state.config.max_speed_mm_s = (float)spd;
        state.config.acceleration_mm_s2 = (float)acc;
        state.config.run_current_ma = state.driver.run_current_ma;

        // Apply persisted inertia/predictive-smoothing settings to the motor.
        if (look > 200) look = 20;
        if (over > 50) over = 8;
        motor.setLookaheadMs(look);
        motor.setMaxOvershootMm((float)over);

        applogf("Config: range=[%.1f, %.1f] speed=%.0f accel=%.0f run=%umA look=%u over=%u",
                rmin, rmax, (float)spd, (float)acc, state.driver.run_current_ma, look, over);
    } else {
        prefs.end();
        applog("No saved config, using defaults");
    }
}