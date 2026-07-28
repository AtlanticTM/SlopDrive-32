// ConfigStore — persist/restore runtime settings (NVS "strokeengine" namespace)
// Constraints:
//   save() / saveWifiCreds() / clearWifiCreds() all defer while
//   state.ota_active is set: a concurrent NVS write during the OTA flash
//   write window can reset the chip mid-flash.
//   Every put*() call's return value is checked; a write that did not
//   persist is counted as a failure, never logged as a success.
//   load() recomputes cfg_crc (see nvsConfigChecksum()) and warns on
//   mismatch; per-field validation still clamps any out-of-range value
//   regardless of the checksum result.

#include "ConfigStore.h"

#include <Preferences.h>
#include "sloplog/sloplog.h"
#include "config_api.h"
#include "MotorDriver.h"
#include "range_mapper.h"

// ---- Config checksum -- FNV-1a over the raw stored values, canonical key order --
// save() writes the hash of everything it stored under "cfg_crc"; load()
// recomputes from the raw stored values and warns on mismatch (per-field
// validation still clamps any out-of-range value regardless). Reading the
// keys back keeps save/load byte-identical without threading every value
// through two functions.

static uint32_t nvsConfigChecksum(Preferences& prefs) {
    uint32_t crc = 2166136261u;                       // FNV-1a offset basis
    auto mixU32 = [&crc](uint32_t v) {
        for (int i = 0; i < 4; i++) { crc ^= (uint8_t)(v >> (8 * i)); crc *= 16777619u; }
    };
    auto mixF = [&mixU32](float f) {
        uint32_t bits; memcpy(&bits, &f, sizeof(bits)); mixU32(bits);
    };
    mixF(prefs.getFloat("range_min", 0.0f));
    mixF(prefs.getFloat("range_max", 0.0f));
    mixF(prefs.getFloat("rail_mm", 0.0f));
    mixU32(prefs.getUShort("max_speed", 0));
    mixU32(prefs.getUInt("accel32", 0));
    mixU32(prefs.getUChar("blend_mode", 0));
    mixU32(prefs.getUShort("user_spd", 0));
    mixU32(prefs.getUInt("user_acc", 0));
    mixU32(prefs.getUShort("inp_spd", 0));
    mixU32(prefs.getUInt("inp_acc", 0));
    // "inp_jrk" (fw 2.1.47) is mixed ONLY WHEN PRESENT. Unconditionally adding a
    // key to this hash would change the computed value for every blob written by
    // an older firmware, and load() would cry CORRUPTION at every upgraded device
    // exactly once — a false alarm that teaches the operator to ignore a real
    // one. Absent key → hash is bit-identical to the pre-2.1.47 hash; present key
    // → fully covered. (A bit-flip that lands inp_jrk on exactly 0 escapes the
    // hash, but 0 is out of range and the loader falls back to default anyway.)
    // Any future key added here should follow the same present-only idiom.
    { uint32_t jrk = prefs.getUInt("inp_jrk", 0); if (jrk) mixU32(jrk); }
    mixU32(prefs.getBool("auto_dur", false) ? 1u : 0u);
    mixF(prefs.getFloat("def_rmin", 0.0f));
    mixF(prefs.getFloat("def_rmax", 0.0f));
    mixU32(prefs.getBool("expert", false) ? 1u : 0u);
    mixU32(prefs.getUChar("input_mode", 0));
    mixU32(prefs.getUChar("buf_easing", 0));
    mixU32(prefs.getUChar("buf_depth", 0));
    mixU32(prefs.getUShort("buf_tick", 0));
    mixU32(prefs.getUShort("gen_tick", 0));
    mixF(prefs.getFloat("stroke_mm", 0.0f));
    mixU32(prefs.getUShort("tmc_run", 0));
    mixU32(prefs.getUChar("tmc_hold", 0));
    mixU32(prefs.getUChar("tmc_sc", 0));
    mixU32(prefs.getUInt("tmc_tpwm", 0));
    mixU32(prefs.getUChar("tmc_toff", 0));
    mixU32(prefs.getUChar("tmc_tbl", 0));
    mixU32((uint32_t)(int32_t)prefs.getChar("tmc_hs", 0));
    mixU32((uint32_t)(int32_t)prefs.getChar("tmc_he", 0));
    return crc;
}

// ---- ConfigStore::save -- persist all runtime settings to NVS ---------------

void ConfigStore::save(SystemState& state, RangeMapper& mapper, MotorDriver& motor) {
    // OTA flash-write guard (.clinerules §2 / OTA §4): a concurrent NVS write
    // would touch the flash cache during the OTA write window and can reset
    // the chip mid-flash. Defer the save — the gated state stops motion
    // anyway, so nothing config-worthy is changing, and a successful OTA
    // reboots into freshly-loaded config regardless.
    if (state.ota_active.load()) {
        SLOGW("cfg", "saveConfig: deferred - OTA update in flight");
        return;
    }
    Preferences prefs;
    if (!prefs.begin("strokeengine", false)) {   // false = read-WRITE
        SLOGE("cfg", "saveConfig: failed to open NVS for write!");
        return;
    }
    // Every put*() returns bytes written — 0 on failure (full partition,
    // brownout mid-write). Count failures instead of discarding them, so a
    // save that DIDN'T persist is never logged as a success.
    uint32_t fails = 0;
    auto ck = [&fails](size_t written) { if (written == 0) fails++; };

    ck(prefs.putFloat("range_min", mapper.getMinMm()));
    ck(prefs.putFloat("range_max", mapper.getMaxMm()));
    // Max rail length (mm) — rail-length-agnostic ceiling. Persisted so the
    // homing sweep bound + pre-homing scale survive a reboot.
    ck(prefs.putFloat("rail_mm", state.config.max_rail_mm));
    // Speed stored as UShort (max 65535) — max speed is 10000 mm/s, fits fine.
    // Accel stored as UInt (uint32_t) — expert mode allows 100000 mm/s² which
    // overflows a uint16_t (max 65535); the legacy "accel" key (UShort) would
    // silently truncate anything above that. "accel32" uses putUInt so the
    // full 100000 survives the round-trip.
    ck(prefs.putUShort("max_speed", (uint16_t)state.config.max_speed_mm_s));
    ck(prefs.putUInt("accel32", (uint32_t)state.config.acceleration_mm_s2));
    // Continuous-blend policy: 1=let-it-land, 2=allow-reversal, 3=hybrid.
    ck(prefs.putUChar("blend_mode", motor.getBlendMode()));

    // Dual limit sets (v0.4 / D4 Phase 3) — persist per-source ceilings
    ck(prefs.putUShort("user_spd", (uint16_t)state.config.user_max_speed_mm_s));
    ck(prefs.putUInt("user_acc", (uint32_t)state.config.user_max_accel_mm_s2));
    ck(prefs.putUShort("inp_spd", (uint16_t)state.config.input_max_speed_mm_s));
    ck(prefs.putUInt("inp_acc", (uint32_t)state.config.input_max_accel_mm_s2));
    // Jerk is UInt like accel — the ceiling is 5e7 mm/s³, needing the full 32
    // bits; a UShort would truncate it into nonsense.
    ck(prefs.putUInt("inp_jrk", (uint32_t)state.config.input_max_jerk_mm_s3));

    ck(prefs.putBool("auto_dur", state.auto_duration));
    ck(prefs.putFloat("def_rmin", state.default_range_min));
    ck(prefs.putFloat("def_rmax", state.default_range_max));
    ck(prefs.putBool("expert", state.expert_mode));
    ck(prefs.putBool("pat_bgrun", state.pattern_background_run));

    // Input mode + buffered interpolation + generator tick rate
    ck(prefs.putUChar("input_mode", (uint8_t)state.getInputMode()));
    ck(prefs.putUChar("buf_easing", state.buf_easing));
    ck(prefs.putUChar("buf_depth", state.buf_depth));
    ck(prefs.putUShort("buf_tick", state.buf_tick_hz));
    ck(prefs.putUShort("gen_tick", state.gen_rate_tick_hz));

    // Measured stroke from sensorless homing — persists across reboot so the
    // rail scale is correct before the first homing cycle runs. Rounded to the
    // nearest 1mm because the safety zone makes sub-mm precision meaningless.
    ck(prefs.putFloat("stroke_mm", motor.getMeasuredStrokeMm()));

    // Driver tunables (from the Motor tab). NVS keys keep their legacy "tmc_"
    // prefix so saved settings survive across this rename — they're persistence
    // identifiers, not driver references.
    ck(prefs.putUShort("tmc_run", state.driver.run_current_ma));
    ck(prefs.putUChar("tmc_hold", state.driver.hold_current_pct));
    ck(prefs.putUChar("tmc_sc", state.driver.stealthchop));
    ck(prefs.putUInt("tmc_tpwm", state.driver.tpwm_thrs));
    ck(prefs.putUChar("tmc_toff", state.driver.toff));
    ck(prefs.putUChar("tmc_tbl", state.driver.tbl));
    ck(prefs.putChar("tmc_hs", state.driver.hstart));
    ck(prefs.putChar("tmc_he", state.driver.hend));

    // ---- SlopMotion live tuning (M5c) ---------------------------------------
    // Real settings (operator ruling 2026-07-27), written via SlopSync 0x0105.
    // NVS keys are capped at 15 chars, hence the abbreviations. They are NOT
    // mixed into cfg_crc: that checksum covers the motion-safety envelope, and
    // widening it would silently invalidate every existing device's stored
    // hash on the first boot after this change. A corrupt tuning value is
    // clamped on load like every other field; a corrupt WINDOW is what the
    // checksum is there to catch.
    ck(prefs.putFloat("sm_jmax",   state.sm_tune_jmax_ovr));
    ck(prefs.putFloat("sm_vmax",   state.sm_tune_vmax_ovr));
    ck(prefs.putFloat("sm_amax",   state.sm_tune_amax_ovr));
    ck(prefs.putUChar("sm_cent",   state.sm_tune_centering ? 1 : 0));
    ck(prefs.putFloat("sm_cgain",  state.sm_tune_centering_gain));
    ck(prefs.putUChar("sm_cff",    state.sm_tune_chase_ff ? 1 : 0));
    ck(prefs.putUChar("sm_caff",   state.sm_tune_chase_aff ? 1 : 0));
    ck(prefs.putFloat("sm_cgn",    state.sm_tune_chase_gain));
    ck(prefs.putFloat("sm_clk",    state.sm_tune_chase_look));
    ck(prefs.putUInt ("sm_dens",   state.sm_tune_dense_us));
    ck(prefs.putUChar("sm_aim",    state.sm_tune_aim_extrap ? 1 : 0));
    ck(prefs.putFloat("sm_hk",     state.sm_tune_handoff_k));
    ck(prefs.putUChar("sm_curve",  state.sm_tune_curve_policy));
    ck(prefs.putUChar("sm_ipol",   state.sm_tune_infeas_policy));
    ck(prefs.putFloat("sm_imarg",  state.sm_tune_infeas_margin));
    ck(prefs.putFloat("sm_sbud",   state.sm_tune_smooth_budget));
    ck(prefs.putFloat("sm_abud",   state.sm_tune_amp_budget));
    ck(prefs.putUChar("sm_bstep",  state.sm_tune_blend_steps));
    ck(prefs.putUChar("sm_rstep",  state.sm_tune_reshape_steps));
    ck(prefs.putUInt ("sm_settle", state.sm_tune_settle_grace_us));


    // Corruption defense: hash what actually landed in NVS (read back raw) and
    // store it. load() recomputes + compares. See nvsConfigChecksum().
    uint32_t crc = nvsConfigChecksum(prefs);
    ck(prefs.putUInt("cfg_crc", crc));
    state.config.checksum = crc;

    prefs.end();
    if (fails > 0) {
        SLOGE("cfg", "Config save: %lu NVS write(s) FAILED — settings may NOT persist across reboot! "
              "(partition full or flash error)", (unsigned long)fails);
    } else {
        SLOGI("cfg", "Config saved to NVS: range=[%.1f,%.1f] speed=%.0f accel=%.0f blend=%u crc=%08lX",
              mapper.getMinMm(), mapper.getMaxMm(),
                state.config.max_speed_mm_s, state.config.acceleration_mm_s2,
                motor.getBlendMode(), (unsigned long)crc);
    }
}

// ---- ConfigStore::load -- load persisted settings (or factory defaults) -----

void ConfigStore::load(SystemState& state, RangeMapper& mapper, MotorDriver& motor) {
    // Always start with defaults
    state.config = getDefaultConfig();
    state.driver = DriverConfig();   // default-constructed = config.h defaults
    // Seed the rail-length ceiling everywhere from the default BEFORE clamping
    // any ranges, so the no-NVS path is still rail-length aware.
    motor.setMaxRailMm(state.config.max_rail_mm);
    mapper.setMaxRailMm(state.config.max_rail_mm);
    mapper.setRange(state.config.min_position_mm, state.config.max_position_mm);

    Preferences prefs;

    // Try to load saved values (read-only mode)
    if (prefs.begin("strokeengine", true)) {
        // Corruption check FIRST — recompute the FNV-1a over the raw stored
        // values and compare with the hash save() stored. A mismatch means at
        // least one value changed outside a save() (bit-flip / partial write).
        // We warn rather than reject: the per-field validation below clamps
        // anything out of range, and an in-range corrupt value is at least now
        // VISIBLE instead of silently trusted. stored==0 → pre-checksum save,
        // nothing to verify yet.
        uint32_t stored_crc = prefs.getUInt("cfg_crc", 0);
        if (stored_crc != 0) {
            uint32_t crc = nvsConfigChecksum(prefs);
            state.config.checksum = crc;
            if (crc != stored_crc) {
                SLOGW("cfg", "Config NVS checksum MISMATCH (stored=%08lX computed=%08lX) — "
                      "possible corruption; per-field validation will clamp bad values",
                        (unsigned long)stored_crc, (unsigned long)crc);
            }
        }
        // Max rail length FIRST — every range/default validation below clamps
        // against it. Sanity-bound 10..2000mm so a corrupt NVS write can't set a
        // nonsensical ceiling. Pushed to the motor + mapper once resolved.
        float rail = prefs.getFloat("rail_mm", state.config.max_rail_mm);
        if (rail < 10.0f || rail > 2000.0f) rail = DEFAULT_MAX_RAIL_MM;
        state.config.max_rail_mm = rail;
        motor.setMaxRailMm(rail);
        mapper.setMaxRailMm(rail);

        float rmin = prefs.getFloat("range_min", state.config.min_position_mm);
        float rmax = prefs.getFloat("range_max", state.config.max_position_mm);
        uint16_t spd = prefs.getUShort("max_speed", (uint16_t)state.config.max_speed_mm_s);
        // Read accel from the new uint32_t key "accel32" first. If it's absent
        // (device was previously saved with the old uint16_t "accel" key), fall
        // back to the old key so existing saves aren't silently reset to default.
        // Once the user saves again, "accel32" gets written and the old key is
        // ignored forever.
        uint32_t acc32_saved = prefs.getUInt("accel32", 0);
        uint32_t acc;
        if (acc32_saved > 0) {
            acc = acc32_saved;
        } else {
            // Legacy fallback: old uint16_t key — max it could hold was 65535.
            // If the old key is also absent, getUShort returns the default.
            acc = (uint32_t)prefs.getUShort("accel", (uint16_t)state.config.acceleration_mm_s2);
        }
        uint8_t blend = prefs.getUChar("blend_mode", 1);  // 1=let-it-land default

        // Dual limit sets (v0.4 / D4 Phase 3) — load with legacy migration.
        // If the new keys exist, use them. The INPUT set migrates from the legacy
        // max_speed/accel (streams should inherit the old full-speed default).
        // The USER set falls back to its GENTLE factory default (50/200) — NOT
        // the legacy values — so a fresh device (or one that only ever saved the
        // legacy keys) boots gentle for manual moves + window-entry glides.
        // INPUT jerk (fw 2.1.47) has NO legacy predecessor to migrate from — a
        // blob written before this key existed reads 0 and keeps the factory
        // default seeded by getDefaultConfig() above, never a zero ceiling
        // (jmax == 0 would wedge the planner solid). Same >0 idiom as the rest,
        // then hard-clamped to the firmware ceiling below.
        float usr_spd = state.config.user_max_speed_mm_s;   // 50 (gentle default)
        float usr_acc = state.config.user_max_accel_mm_s2;  // 200 (gentle default)
        float inp_spd = spd, inp_acc = (float)acc;
        float inp_jrk = state.config.input_max_jerk_mm_s3;  // 2e6 factory default
        uint16_t usr_spd_saved = prefs.getUShort("user_spd", 0);
        uint32_t usr_acc_saved = prefs.getUInt("user_acc", 0);
        uint16_t inp_spd_saved = prefs.getUShort("inp_spd", 0);
        uint32_t inp_acc_saved = prefs.getUInt("inp_acc", 0);
        uint32_t inp_jrk_saved = prefs.getUInt("inp_jrk", 0);
        if (usr_spd_saved > 0) usr_spd = (float)usr_spd_saved;
        if (usr_acc_saved > 0) usr_acc = (float)usr_acc_saved;
        if (inp_spd_saved > 0) inp_spd = (float)inp_spd_saved;
        if (inp_acc_saved > 0) inp_acc = (float)inp_acc_saved;
        if (inp_jrk_saved > 0) inp_jrk = (float)inp_jrk_saved;
        // Per-field validation: a corrupt in-range-looking jerk is still bounded.
        if (inp_jrk < 1000.0f)        inp_jrk = DEFAULT_INPUT_MAX_JERK_MM_S3;
        if (inp_jrk > MAX_JERK_MM_S3) inp_jrk = MAX_JERK_MM_S3;
        state.config.user_max_speed_mm_s   = usr_spd;
        state.config.user_max_accel_mm_s2  = usr_acc;
        state.config.input_max_speed_mm_s  = inp_spd;
        state.config.input_max_accel_mm_s2 = inp_acc;

        // ---- SlopMotion live tuning (M5c) -----------------------------------
        // Every getter passes the CURRENT value as its default, so a device
        // whose NVS predates this block keeps the compiled-in defaults instead
        // of being zeroed. Bounds mirror the 0x0105 clamps exactly — a value
        // that got out of range by any route is corrected on the way in.
        auto clf = [](float v, float lo, float hi) {
            return !(v > lo) ? lo : (v > hi ? hi : v);
        };
        auto clu = [](uint32_t v, uint32_t lo, uint32_t hi) {
            return v < lo ? lo : (v > hi ? hi : v);
        };
        state.sm_tune_jmax_ovr    = clf(prefs.getFloat("sm_jmax",  state.sm_tune_jmax_ovr), 0.0f, 2000000.0f);
        state.sm_tune_vmax_ovr    = clf(prefs.getFloat("sm_vmax",  state.sm_tune_vmax_ovr), 0.0f, 20.0f);
        state.sm_tune_amax_ovr    = clf(prefs.getFloat("sm_amax",  state.sm_tune_amax_ovr), 0.0f, 500.0f);
        state.sm_tune_centering    = prefs.getUChar("sm_cent",  state.sm_tune_centering ? 1 : 0) != 0;
        state.sm_tune_centering_gain = clf(prefs.getFloat("sm_cgain", state.sm_tune_centering_gain), 0.0f, 1.0f);
        state.sm_tune_chase_ff    = prefs.getUChar("sm_cff",   state.sm_tune_chase_ff ? 1 : 0) != 0;
        state.sm_tune_chase_aff   = prefs.getUChar("sm_caff",  state.sm_tune_chase_aff ? 1 : 0) != 0;
        state.sm_tune_chase_gain  = clf(prefs.getFloat("sm_cgn", state.sm_tune_chase_gain), 0.0f, 1.5f);
        state.sm_tune_chase_look  = clf(prefs.getFloat("sm_clk", state.sm_tune_chase_look), 0.0f, 8.0f);
        state.sm_tune_dense_us    = clu(prefs.getUInt("sm_dens", state.sm_tune_dense_us), 10000u, 500000u);
        state.sm_tune_aim_extrap  = prefs.getUChar("sm_aim",   state.sm_tune_aim_extrap ? 1 : 0) != 0;
        state.sm_tune_handoff_k   = clf(prefs.getFloat("sm_hk", state.sm_tune_handoff_k), 0.0f, 8.0f);
        state.sm_tune_curve_policy  = (uint8_t)clu(prefs.getUChar("sm_curve", state.sm_tune_curve_policy), 0, 2);
        state.sm_tune_infeas_policy = (uint8_t)clu(prefs.getUChar("sm_ipol",  state.sm_tune_infeas_policy), 0, 4);
        state.sm_tune_infeas_margin = clf(prefs.getFloat("sm_imarg", state.sm_tune_infeas_margin), 0.5f, 1.0f);
        state.sm_tune_smooth_budget = clf(prefs.getFloat("sm_sbud",  state.sm_tune_smooth_budget), 0.0f, 1.0f);
        state.sm_tune_amp_budget    = clf(prefs.getFloat("sm_abud",  state.sm_tune_amp_budget), 0.0f, 1.0f);
        state.sm_tune_blend_steps   = (uint8_t)clu(prefs.getUChar("sm_bstep", state.sm_tune_blend_steps), 1, 10);
        state.sm_tune_reshape_steps = (uint8_t)clu(prefs.getUChar("sm_rstep", state.sm_tune_reshape_steps), 0, 8);
        state.sm_tune_settle_grace_us = clu(prefs.getUInt("sm_settle", state.sm_tune_settle_grace_us), 0u, 200000u);
        state.config.input_max_jerk_mm_s3  = inp_jrk;

        state.auto_duration = prefs.getBool("auto_dur", true);

        state.default_range_min = prefs.getFloat("def_rmin", 0.0f);
        state.default_range_max = prefs.getFloat("def_rmax", state.config.max_rail_mm);
        state.expert_mode = prefs.getBool("expert", false);
        state.pattern_background_run = prefs.getBool("pat_bgrun", false);

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
        // Gen tick rungs: 20/50/100/250/500 Hz. A saved value of 200 snaps to
        // 250 on load — close enough, and the operator can re-save.
        { uint16_t gt = prefs.getUShort("gen_tick", state.gen_rate_tick_hz);
          state.gen_rate_tick_hz = (gt >= 375) ? 500 : (gt >= 175) ? 250 : (gt >= 75) ? 100 : (gt >= 35) ? 50 : 20; }

        // Validate startup defaults; fall back to full rail if nonsensical.
        // Clamps against the configured max rail length (rail-length agnostic).
        if (state.default_range_min < 0.0f || state.default_range_min > state.config.max_rail_mm ||
            state.default_range_max <= 0.0f || state.default_range_max > state.config.max_rail_mm ||
            state.default_range_min >= state.default_range_max) {
            state.default_range_min = 0.0f;
            state.default_range_max = state.config.max_rail_mm;
        }

        // Driver tunables (legacy "tmc_" NVS keys — persistence identifiers)
        state.driver.run_current_ma   = prefs.getUShort("tmc_run", state.driver.run_current_ma);
        state.driver.hold_current_pct = prefs.getUChar("tmc_hold", state.driver.hold_current_pct);
        state.driver.stealthchop      = prefs.getUChar("tmc_sc", state.driver.stealthchop);
        state.driver.tpwm_thrs        = prefs.getUInt("tmc_tpwm", state.driver.tpwm_thrs);
        state.driver.toff             = prefs.getUChar("tmc_toff", state.driver.toff);
        state.driver.tbl              = prefs.getUChar("tmc_tbl", state.driver.tbl);
        state.driver.hstart           = prefs.getChar("tmc_hs", state.driver.hstart);
        state.driver.hend             = prefs.getChar("tmc_he", state.driver.hend);

        // Restore previously-measured stroke from NVS so the rail scale is
        // correct at boot BEFORE the first homing cycle. The homing task
        // overwrites this with a fresh measurement when it completes.
        float saved_stroke = prefs.getFloat("stroke_mm", 0.0f);
        if (saved_stroke > 0.0f) motor.setMeasuredStrokeMm(saved_stroke);

        prefs.end();

        // Validate: reject 0 or out-of-range values against the configured max
        // rail length (rail-length agnostic) — a saved range_max within the
        // user's rail survives the round-trip untouched.
        if (rmin < 0.0f || rmin > state.config.max_rail_mm) rmin = state.config.min_position_mm;
        if (rmax <= 0.0f || rmax > state.config.max_rail_mm) rmax = state.config.max_rail_mm;
        if (rmin >= rmax) { rmin = state.config.min_position_mm; rmax = state.config.max_rail_mm; }
        // Speed: floor at 1, ceiling at MAX_SPEED_MM_S (10000). Anything outside
        // that window is a corrupt NVS write — reset to the running default.
        if (spd == 0 || spd > (uint16_t)MAX_SPEED_MM_S) spd = (uint16_t)state.config.max_speed_mm_s;
        // Accel: floor at 10 (zero/garbage = bricked planner), ceiling at
        // MAX_ACCEL_MM_S2 (100000) — expert mode can write up to that and the
        // servo drive can take it.
        if (acc < 10 || acc > (uint32_t)MAX_ACCEL_MM_S2) acc = (uint32_t)state.config.acceleration_mm_s2;


        if (state.driver.run_current_ma < 250 || state.driver.run_current_ma > 3000)
            state.driver.run_current_ma = DRIVER_DEFAULT_RUN_CURRENT_MA;
        if (state.driver.toff < 1 || state.driver.toff > 15) state.driver.toff = DRIVER_DEFAULT_TOFF;

        mapper.setRange(rmin, rmax);
        // Write the resolved range back into state.config too: SystemState.h
        // documents config as cross-core-read state, and config_api.h's
        // mapToPosition/mapFromPosition/getUsableRange helpers consume these
        // fields directly. Skipping this leaves state.config diverged from
        // the mapper's actual range.
        state.config.min_position_mm = rmin;
        state.config.max_position_mm = rmax;
        state.config.max_speed_mm_s = (float)spd;
        state.config.acceleration_mm_s2 = (float)acc;
        state.config.run_current_ma = state.driver.run_current_ma;

        // Apply persisted continuous-blend policy to the motor. Clamp 1..3 so a
        // corrupt/legacy NVS value can't put us in an undefined mode.
        if (blend < 1 || blend > 3) blend = 1;
        motor.setBlendMode(blend);

        SLOGI("cfg", "Config: range=[%.1f, %.1f] speed=%.0f accel=%.0f run=%umA blend=%u",
              rmin, rmax, (float)spd, (float)acc, state.driver.run_current_ma, blend);

    } else {
        prefs.end();
        SLOGI("cfg", "No saved config, using defaults");
    }
}

// ---- Secondary WiFi credentials -- serial-settable NVS fallback -------------
// A second SSID/password pair, tried by setupWiFi() after the compile-time
// primary (secrets.h) fails. Written over USB serial via the `WIFI <ssid>
// <pass>` command so a rig on an unknown network can be recovered without a
// reflash. Uses the same "strokeengine" namespace with putString/getString.
// Keys stay ≤15 chars for NVS.

void ConfigStore::saveWifiCreds(const SystemState& state, const char* ssid, const char* pass) {
    // Same OTA flash-contention guard as save() — a `WIFI ...` serial command
    // landing mid-OTA must not write NVS during the flash write window.
    if (state.ota_active.load()) {
        SLOGW("cfg", "saveWifiCreds: REFUSED — OTA update in flight, retry after it completes");
        return;
    }
    Preferences prefs;
    if (!prefs.begin("strokeengine", false)) {   // false = read-WRITE
        SLOGE("cfg", "saveWifiCreds: failed to open NVS for write!");
        return;
    }
    size_t w1 = prefs.putString("wifi_ssid2", ssid ? ssid : "");
    size_t w2 = prefs.putString("wifi_pass2", pass ? pass : "");
    prefs.end();
    if ((ssid && ssid[0] && w1 == 0) || (pass && pass[0] && w2 == 0)) {
        SLOGE("cfg", "saveWifiCreds: NVS write FAILED — creds NOT stored!");
    } else {
        SLOGI("cfg", "Secondary WiFi creds saved to NVS: SSID='%s'", ssid ? ssid : "");
    }
}

bool ConfigStore::loadWifiCreds(char* ssid, size_t ssidLen, char* pass, size_t passLen) {
    if (!ssid || ssidLen == 0 || !pass || passLen == 0) return false;
    ssid[0] = '\0';
    pass[0] = '\0';
    Preferences prefs;
    if (!prefs.begin("strokeengine", true)) {     // true = read-only
        prefs.end();
        return false;
    }
    String s = prefs.getString("wifi_ssid2", "");
    String p = prefs.getString("wifi_pass2", "");
    prefs.end();
    if (s.length() == 0) return false;            // no secondary creds stored
    strlcpy(ssid, s.c_str(), ssidLen);
    strlcpy(pass, p.c_str(), passLen);
    return true;
}

void ConfigStore::clearWifiCreds(const SystemState& state) {
    // Same OTA flash-contention guard as save()/saveWifiCreds().
    if (state.ota_active.load()) {
        SLOGW("cfg", "clearWifiCreds: REFUSED — OTA update in flight, retry after it completes");
        return;
    }
    Preferences prefs;
    if (!prefs.begin("strokeengine", false)) {
        SLOGE("cfg", "clearWifiCreds: failed to open NVS for write!");
        return;
    }
    prefs.remove("wifi_ssid2");
    prefs.remove("wifi_pass2");
    prefs.end();
    SLOGI("cfg", "Secondary WiFi creds cleared from NVS");
}
