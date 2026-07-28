#include "SlopSyncHubService.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <array>
#include <cmath>
#include <cstring>

#include "AppLog.h"          // applogSerialQuiet — the RFC-017 serial handoff
#include "MotionArbiter.h"
#include "MotorDriver.h"
#include "PatternEngine.h"
#include "SlopGlowBoard.h"
#include "SystemState.h"
#include "UiProtocol.h"
#include "WebUI.h"
#include "config_api.h"
#include "sloplog/sloplog.h"
#include "slopmotion/slopmotion.hpp"
#include "slopsync/util/byte_io.hpp"
#include "slopsync/wire/messages/event.hpp"

namespace slopdrive {

// ============================================================================
// ANTI-DRIFT GUARDS for the catalog's RFC-009 annotations.
//
// SlopSyncCatalog.h is hardware-free by contract (the native test suite and
// the sim both build it against nothing but the library), so it cannot include
// config_api.h — <Arduino.h> comes with it. The factory defaults and hard
// ceilings it advertises to every client are therefore a hand-mirror of
// getDefaultConfig()/applySettings(), and a hand-mirror rots.
//
// This translation unit DOES see both, so it is where the mirror is nailed
// down: change a default in config_api.h and the FIRMWARE stops compiling
// until the catalog follows. The alternative — a wrong `default` on the wire —
// is invisible, because it only shows up as a client's "reset to factory"
// button writing a number this machine never shipped with.
static_assert(factory::window_min  == 0.0f,                        "catalog default window_min drifted from getDefaultConfig()");
static_assert(factory::window_max  == DEFAULT_MAX_RAIL_MM,         "catalog default window_max drifted from DEFAULT_MAX_RAIL_MM");
static_assert(factory::user_speed  == DEFAULT_USER_MAX_SPEED_MM_S, "catalog default user_speed drifted");
static_assert(factory::user_accel  == DEFAULT_USER_ACCEL_MM_S2,    "catalog default user_accel drifted");
static_assert(factory::input_speed == DEFAULT_MAX_SPEED_MM_S,      "catalog default input_speed drifted");
static_assert(factory::input_accel == DEFAULT_ACCEL_MM_S2,         "catalog default input_accel drifted");
static_assert(factory::input_jerk  == DEFAULT_INPUT_MAX_JERK_MM_S3,"catalog default input_jerk drifted");
static_assert(factory::max_rail    == DEFAULT_MAX_RAIL_MM,         "catalog default max_rail drifted from DEFAULT_MAX_RAIL_MM");
static_assert(ceiling::speed_max   == MAX_SPEED_MM_S,              "catalog speed ceiling drifted from MAX_SPEED_MM_S");
static_assert(ceiling::accel_max   == MAX_ACCEL_MM_S2,             "catalog accel ceiling drifted from MAX_ACCEL_MM_S2");
static_assert(ceiling::jerk_max    == MAX_JERK_MM_S3,              "catalog jerk ceiling drifted from MAX_JERK_MM_S3");
// M5b mode defaults (0x008A). Same contract: this TU sees BOTH the catalog's
// mirrored table and the real source, so drift fails the build here rather than
// shipping a catalog that advertises a factory default the machine never had.
static_assert(factory::stream_speed_mode == SystemState::SPEED_CEILING_PEGGED,
              "catalog default stream_speed_mode drifted from SystemState");
// kApBaseCount mirrors advpat::BASE_COUNT (see SlopSyncCatalog.h's comment on
// why the catalog header can't include AdvancedPattern.h directly) — this TU
// includes PatternEngine.h (and therefore AdvancedPattern.h), so it is where
// the mirror is pinned.
static_assert(slopdrive::kApBaseCount == advpat::BASE_COUNT,
              "catalog kApBaseCount drifted from advpat::BASE_COUNT");
// Same mirror rule for the RFC-021 preset store: SlopSyncCatalog.h duplicates
// PatternPresetStore's constants rather than including its header (kept
// dependency-free, same reasoning as kApBaseCount above); this TU sees both.
static_assert(slopdrive::kPresetCapacity == PatternPresetStore::kCapacity,
              "catalog kPresetCapacity drifted from PatternPresetStore::kCapacity");
static_assert(slopdrive::kPresetNameMax == PatternPresetStore::kNameMax,
              "catalog kPresetNameMax drifted from PatternPresetStore::kNameMax");
static_assert(slopdrive::kPresetPayloadBytes == PatternPresetStore::kPayloadBytes,
              "catalog kPresetPayloadBytes drifted from PatternPresetStore::kPayloadBytes");

// ============================================================================
// Small helpers
// ============================================================================

namespace {

using slopsync::IntentValue;
using slopsync::IntentValueField;
using slopsync::IntentValueMap;
using slopsync::NackCode;

// 0x0085 end_vel_norm sentinel: INT16_MIN means "no end velocity" (0 is a
// legitimate slope, so it can't double as "absent"). Named here to keep the
// magic number off the decode hot path and greppable against the catalog doc.
constexpr int16_t kSegNoEndVel = -32768;

const IntentValueField* findField(const IntentValueMap& m, uint8_t key) {
    for (uint32_t i = 0; i < m.count; ++i) {
        if (m.fields[i].key == key) return &m.fields[i];
    }
    return nullptr;
}

float fieldF32(const IntentValueField* f, float dflt) {
    if (!f) return dflt;
    switch (f->value.kind) {
        case IntentValue::Kind::F32: return f->value.f32_val;
        case IntentValue::Kind::U64: return float(f->value.u64_val);
        case IntentValue::Kind::I64: return float(f->value.i64_val);
        default: return dflt;
    }
}

bool fieldBool(const IntentValueField* f, bool dflt) {
    if (!f) return dflt;
    if (f->value.kind == IntentValue::Kind::Bool) return f->value.bool_val;
    if (f->value.kind == IntentValue::Kind::U64) return f->value.u64_val != 0;
    return dflt;
}

uint64_t fieldU64(const IntentValueField* f, uint64_t dflt) {
    if (!f) return dflt;
    if (f->value.kind == IntentValue::Kind::U64) return f->value.u64_val;
    if (f->value.kind == IntentValue::Kind::I64 && f->value.i64_val >= 0) return uint64_t(f->value.i64_val);
    return dflt;
}

uint16_t clampU16(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 65535.0f) return 65535;
    return uint16_t(v + 0.5f);
}

int16_t clampI16(float v) {
    if (v >= 32767.0f) return 32767;
    if (v <= -32768.0f) return -32768;
    return int16_t(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

}  // namespace

// ============================================================================
// SlopDriveHubDelegate
// ============================================================================

slopsync::AccessLevel SlopDriveHubDelegate::validateToken(std::span<const std::byte> instance_id,
                                                          std::span<const std::byte> token, bool hasToken) {
    // ================= ENFORCEMENT IS ON (fw 2.1.59) ========================
    // This used to `return control` unconditionally. It no longer does. The
    // chain is /uitoken -> trust ledger -> watch, and `watch` is the floor for
    // anything that cannot prove otherwise.
    //
    // WHAT `watch` ACTUALLY COSTS A CLIENT: nothing it needs to be safe. It may
    // connect, fetch the catalog, subscribe to telemetry, and — because the
    // 0x0005 safety channel marks `stop` and `estop` role-EXEMPT in its
    // option_access vector (RFC-025b) — it may STOP THIS MACHINE. Safety
    // outranks authorization, so a demoted client is a client that can watch
    // and can panic, which is the correct degraded state for a machine someone
    // may be standing next to. What it loses is the ability to COMMAND motion.
    //
    // HONEST SCOPE — read this before believing the machine is locked down:
    // while /uitoken is enabled (the default), anything on the LAN that can
    // HTTP GET can mint a control-tier credential. The endpoint's only defense
    // is the absence of CORS headers, which stops a hostile WEB PAGE and
    // nothing else. So this flip buys a real chokepoint, an audit line per
    // authorization, and a default-deny posture — NOT LAN secrecy. The
    // lockdown posture is setUiTokenEnabled(false) plus paired tokens, and this
    // function already implements it: rung 1 simply stops answering.
    // ========================================================================

    // ---- Rung 1: RFC-029 §4, the browser-borne credential ------------------
    // A /uitoken mint grants CONTROL and never configure. It is consumed here
    // (single-use), which is why this check comes FIRST: a valid token must be
    // burned even if a later branch would have granted the same tier anyway, or
    // a page could hoard one and replay it after the posture is tightened.
    //
    // DO NOT MOVE A LAZILY-INITIALIZED ANYTHING INTO consume()'s spinlock —
    // field bug #4 (fw 2.1.58) was exactly that, and it aborted the device on
    // the first HELLO that ever presented a live token. See SlopSyncUiToken.cpp.
    if (hasToken && _uiTokens.consume(token)) {
        SLOGI("slopsync", "session authorized by /uitoken (control tier)");
        return slopsync::AccessLevel::control;
    }

    // ---- Rung 2: the persisted trust ledger (§12.2) ------------------------
    // validate() is constant-time (RFC-028.3) and answers `watch` for BOTH an
    // unknown device and a recognized-but-suspended one — deliberately
    // indistinguishable, so this can never be used as an instance-id oracle.
    if (hasToken && _pairing != nullptr) {
        const auto role = _pairing->validate(instance_id, token);
        if (role > slopsync::AccessLevel::watch) {
            SLOGI("slopsync", "session authorized by paired token (tier %u)", unsigned(role));
            return role;
        }
    }

    // ---- Rung 3: default deny (to `watch`, not to a closed door) -----------
    // Logged at WARN because a client silently losing its write plane is the
    // single most confusing failure this change can produce, and the operator
    // should be able to see it in /api/log without instrumenting anything.
    SLOGW("slopsync", "session UNAUTHORIZED -> watch tier (token %s)",
          hasToken ? "present but not recognized" : "absent");
    return slopsync::AccessLevel::watch;
}

slopsync::Result<IntentValueMap, NackCode> SlopDriveHubDelegate::applyIntent(
    uint16_t channel_id, const IntentValueMap& requested, slopsync::AccessLevel role, bool& cfgChanged) {
    using Ret = slopsync::Result<IntentValueMap, NackCode>;
    (void)role;  // catalog access level already gated by the hub before we run

    // JsonDocument translation is a COLD path (intent application, not the
    // telemetry hot loop) — a stack JsonDocument here is fine per the brief.
    JsonDocument in;
    JsonDocument out;
    IntentValueMap applied{};

    switch (channel_id) {
        // ---- 0x0100 move → WS_OP_MOVE ------------------------------------
        case ch::move: {
            if (_state.estop_latched) return Ret::err(NackCode::ESTOP_ACTIVE);
            if (!_state.homed) return Ret::err(NackCode::NOT_HOMED);
            in["position"] = fieldF32(findField(requested, 1), 0.0f);
            in["bypass_limits"] = fieldBool(findField(requested, 2), false);
            if (!_webui.handleCommand(WS_OP_MOVE, in, out)) {
                // applyMove only fails on not-homed (guarded above) or an
                // unwired arbiter — the latter is a hub-side interlock.
                return Ret::err(NackCode::INTERLOCK);
            }
            // Ground Truth: echo POST-CLAMP values from the handler response.
            applied.count = 2;
            applied.fields[0] = {1, IntentValue::ofF32(out["position"] | 0.0f)};
            applied.fields[1] = {2, IntentValue::ofBool(out["bypass_limits"] | false)};
            return Ret::ok(applied);
        }

        // ---- 0x0101 config-set → WS_OP_SET_WINDOW ------------------------
        case ch::config_set: {
            const auto* f1 = findField(requested, 1);  // window_min
            const auto* f2 = findField(requested, 2);  // window_max
            const auto* f3 = findField(requested, 3);  // user_speed
            const auto* f4 = findField(requested, 4);  // user_accel
            const auto* f5 = findField(requested, 5);  // input_speed
            const auto* f6 = findField(requested, 6);  // input_accel
            const auto* f7 = findField(requested, 7);  // input_jerk (fw 2.1.47)
            const auto* f8 = findField(requested, 8);  // max_rail (fw 2.1.76, item 1)
            if (f1) in["range_min"] = fieldF32(f1, 0.0f);
            if (f2) in["range_max"] = fieldF32(f2, 0.0f);
            // applySettings gates the user/input branches on is<uint32_t>(), so
            // these MUST be integer-typed in the doc, not floats.
            if (f3) in["user_max_speed"] = uint32_t(lroundf(fieldF32(f3, 0.0f)));
            if (f4) in["user_max_accel"] = uint32_t(lroundf(fieldF32(f4, 0.0f)));
            if (f5) in["input_max_speed"] = uint32_t(lroundf(fieldF32(f5, 0.0f)));
            if (f6) in["input_max_accel"] = uint32_t(lroundf(fieldF32(f6, 0.0f)));
            if (f7) in["input_max_jerk"] = uint32_t(lroundf(fieldF32(f7, 0.0f)));
            // max_rail is a float field in applySettings (doc["max_rail"].is<float>()
            // is checked FIRST there), unlike the uint32-gated user/input pairs above.
            if (f8) in["max_rail"] = fieldF32(f8, 0.0f);
            // Bump the protocol cfg_gen (that is what cfgChanged means) but do
            // NOT hammer NVS on a potentially-streamed intent — no_persist keeps
            // flash safe; a future explicit save channel handles durability.
            // max_rail is the one exception: it is a rare, deliberate geometry
            // edit (never a streamed value), so it earns an explicit COALESCED
            // persist via _maxRailDirty below rather than waiting on a manual
            // WebUI save — see item 1's "savable" requirement.
            in["no_persist"] = true;

            // RFC-002: cfg_gen advances IFF an applied value actually CHANGED.
            // Snapshot the live config before applying and compare after —
            // an accepted-but-value-identical config-set still gets its ECHO
            // (ground truth is unaffected) but must NOT bump the generation,
            // because a bump re-arms every observer's on-change republish and
            // resync cycle. That amplification is the wire-side accomplice of
            // the 2026-07-24 dual-plane config storm.
            const auto& cfg = _state.config;
            const float p0 = cfg.min_position_mm, p1 = cfg.max_position_mm;
            const float p2 = cfg.user_max_speed_mm_s, p3 = cfg.user_max_accel_mm_s2;
            const float p4 = cfg.input_max_speed_mm_s, p5 = cfg.input_max_accel_mm_s2;
            const float p6 = cfg.input_max_jerk_mm_s3;
            const float p7 = cfg.max_rail_mm;

            if (!_webui.handleCommand(WS_OP_SET_WINDOW, in, out)) {
                return Ret::err(NackCode::INVALID_VALUE);  // e.g. min >= max
            }
            cfgChanged = (p0 != cfg.min_position_mm) || (p1 != cfg.max_position_mm) ||
                         (p2 != cfg.user_max_speed_mm_s) || (p3 != cfg.user_max_accel_mm_s2) ||
                         (p4 != cfg.input_max_speed_mm_s) || (p5 != cfg.input_max_accel_mm_s2) ||
                         (p6 != cfg.input_max_jerk_mm_s3) || (p7 != cfg.max_rail_mm);
            // RFC-011: tell the service's machine-side detector that THIS change
            // was client-driven, so it doesn't bump cfg_gen a second time for
            // the same edit (the hub already bumps on cfgChanged).
            if (cfgChanged) _cfgFromIntent = true;
            uint32_t n = 0;
            if (f1) applied.fields[n++] = {1, IntentValue::ofF32(out["range_min"] | 0.0f)};
            if (f2) applied.fields[n++] = {2, IntentValue::ofF32(out["range_max"] | 0.0f)};
            if (f3) applied.fields[n++] = {3, IntentValue::ofF32(float(out["user_max_speed"] | 0u))};
            if (f4) applied.fields[n++] = {4, IntentValue::ofF32(float(out["user_max_accel"] | 0u))};
            if (f5) applied.fields[n++] = {5, IntentValue::ofF32(float(out["input_max_speed"] | 0u))};
            if (f6) applied.fields[n++] = {6, IntentValue::ofF32(float(out["input_max_accel"] | 0u))};
            if (f7) applied.fields[n++] = {7, IntentValue::ofF32(float(out["input_max_jerk"] | 0u))};
            if (f8) applied.fields[n++] = {8, IntentValue::ofF32(out["max_rail"] | 0.0f)};
            applied.count = n;
            // Coalesced NVS persist, mirroring 0x0105's _smTuneDirty — a rare
            // geometry edit deserves durability without hammering flash on a
            // channel other keys on THIS SAME intent deliberately don't persist.
            if (f8) _maxRailDirty = true;
            return Ret::ok(applied);
        }

        // ---- 0x0102 pattern-cmd → WS_OP_GEN_CFG --------------------------
        case ch::pattern_cmd: {
            if (_state.estop_latched) return Ret::err(NackCode::ESTOP_ACTIVE);
            const auto* f1 = findField(requested, 1);  // running
            const auto* f2 = findField(requested, 2);  // pattern
            const auto* f3 = findField(requested, 3);  // speed
            const auto* f4 = findField(requested, 4);  // depth
            const auto* f5 = findField(requested, 5);  // stroke
            const auto* f6 = findField(requested, 6);  // sensation
            const auto* f7 = findField(requested, 7);  // background_run (RFC-045/048)
            uint32_t n = 0;
            if (f1 || f2 || f3 || f4 || f5 || f6) {
                if (f1) in["running"] = fieldBool(f1, false);
                if (f2) in["pattern"] = int(fieldU64(f2, 0));
                if (f3) in["speed"] = fieldF32(f3, 0.0f);
                if (f4) in["depth"] = fieldF32(f4, 0.0f);
                if (f5) in["stroke"] = fieldF32(f5, 0.0f);
                if (f6) in["sensation"] = fieldF32(f6, 0.0f);
                if (!_webui.handleCommand(WS_OP_GEN_CFG, in, out)) {
                    return Ret::err(_state.homed ? NackCode::INVALID_VALUE : NackCode::NOT_HOMED);
                }
                // applyPattern echoes running(bool), pattern/speed/depth/stroke/
                // sensation as ints — re-widen to the schema's f32.
                if (f1) applied.fields[n++] = {1, IntentValue::ofBool(out["running"] | false)};
                if (f2) applied.fields[n++] = {2, IntentValue::ofU64(uint64_t(int(out["pattern"] | 0)))};
                if (f3) applied.fields[n++] = {3, IntentValue::ofF32(float(int(out["speed"] | 0)))};
                if (f4) applied.fields[n++] = {4, IntentValue::ofF32(float(int(out["depth"] | 0)))};
                if (f5) applied.fields[n++] = {5, IntentValue::ofF32(float(int(out["stroke"] | 0)))};
                if (f6) applied.fields[n++] = {6, IntentValue::ofF32(float(int(out["sensation"] | 0)))};
            }
            // RFC-045/048: source.background_run is a STANDING POLICY, not a
            // live pattern parameter — handled independently of WS_OP_GEN_CFG
            // (so it never trips NOT_HOMED: a preference about what happens on
            // disconnect is legal to set before ever homing) and PERSISTED
            // (coalesced, like max_rail's _maxRailDirty) rather than
            // session-volatile like keys 1-6.
            if (f7) {
                bool bg = fieldBool(f7, false);
                if (bg != _state.pattern_background_run) {
                    _state.pattern_background_run = bg;
                    _patternBackgroundRunDirty = true;
                }
                applied.fields[n++] = {7, IntentValue::ofBool(bg)};
            }
            cfgChanged = false;  // session-volatile (background_run persists via its own dirty flag)
            applied.count = n;
            return Ret::ok(applied);
        }

        // ---- 0x0103 home → WS_OP_HOME / WS_OP_HOME_OVERRIDE ---------------
        case ch::home: {
            uint64_t op = fieldU64(findField(requested, 1), 0);
            switch (op) {
                case 1:  // real sensorless homing
                    if (_state.estop_latched) return Ret::err(NackCode::ESTOP_ACTIVE);
                    _webui.handleCommand(WS_OP_HOME, in, out);  // always accepts
                    applied.count = 1;
                    applied.fields[0] = {1, IntentValue::ofU64(1)};
                    return Ret::ok(applied);

                case 2: {  // force_home {stroke} — BENCH OP, RFC-025
                    // *** HAZARD, READ BEFORE TOUCHING (this is why RFC-025 put
                    // these ops under SAFETY review and not in a convenience
                    // bucket): WS_OP_HOME_OVERRIDE CLEARS THE E-STOP LATCH. It
                    // is therefore deliberately NOT gated on !estop_latched the
                    // way every other op on this channel is — gating it would
                    // make it unusable for its entire purpose (recovering a
                    // motorless bench rig, where a latch is the resting state).
                    //
                    // What it actually does: declares the machine homed WITHOUT
                    // running a homing cycle and ASSERTS a stroke length that
                    // nothing measured. On a rig with a motor attached that is
                    // a collision hazard, full stop — the arbiter will happily
                    // plan moves across a window that may not physically exist.
                    // It exists because motorless development is otherwise
                    // impossible, and it stays `control`-gated and rate-capped.
                    //
                    // The hub's ESTOP latch is dropped in lockstep by
                    // syncSafety() on the next service tick (it reconciles the
                    // hub to _state.estop_latched in BOTH directions), so the
                    // safety snapshot never lies about the latch we just
                    // cleared.
                    const auto* fs = findField(requested, 2);
                    float stroke = fieldF32(fs, 250.0f);
                    if (!(stroke >= 1.0f)) stroke = 250.0f;  // also catches NaN
                    in["on"] = true;
                    in["stroke"] = stroke;
                    _webui.handleCommand(WS_OP_HOME_OVERRIDE, in, out);
                    SLOGW("slopsync",
                          "BENCH force_home via SlopSync: homed asserted, stroke %.1f mm, e-stop latch cleared",
                          double(stroke));
                    applied.count = 2;
                    applied.fields[0] = {1, IntentValue::ofU64(2)};
                    // Ground truth: echo the stroke the handler actually adopted.
                    applied.fields[1] = {2, IntentValue::ofF32(out["measured_stroke"] | stroke)};
                    return Ret::ok(applied);
                }

                case 3:  // clear_override — back to real homing
                    in["on"] = false;
                    _webui.handleCommand(WS_OP_HOME_OVERRIDE, in, out);
                    SLOGI("slopsync", "BENCH clear_override via SlopSync: back to real homing");
                    applied.count = 1;
                    applied.fields[0] = {1, IntentValue::ofU64(3)};
                    return Ret::ok(applied);

                default:
                    return Ret::err(NackCode::UNSUPPORTED_OP);
            }
        }

        // ---- 0x0104 modes-set → MODE / STREAM_MODE / OVERSHOOT ------------
        // M5b: originally the four MODE settings the legacy :81/HTTP plane
        // owned. Every key optional; only the keys PRESENT are applied, and
        // each one echoes the value the handler actually took.
        //
        // KEY 1 (blend_mode) IS NOW A PERMANENT GAP, same treatment as key 2
        // (transport) below — fw 2.1.76, operator ruling 2026-07-27, item 2.
        // MotionArbiter has aliased every blend mode to "allow" since before
        // this channel existed, so there was no live setting left to write;
        // see the field comment on 0x008A's `blend_mode_reserved` in
        // SlopSyncCatalog.h. A client that still sends key 1 falls through
        // unhandled below, same as key 2 always has — if it is the ONLY key
        // present the call NACKs INVALID_VALUE (anyApplied stays false).
        //
        // GROUND TRUTH, and it is not decoration here: these clamp or
        // reinterpret their input somewhere downstream. So the echo is
        // re-read from the machine AFTER the handler ran, never assumed from
        // the request — otherwise a rejected switch would leave every
        // client's dropdown showing a mode the machine is not in.
        case ch::modes_set: {
            applied.count = 0;
            bool anyApplied = false;

            if (const auto* f = findField(requested, 3)) {   // stream_speed_mode
                in.clear(); out.clear();
                in["mode"] = uint8_t(fieldU64(f, 0));
                _webui.handleCommand(WS_OP_STREAM_MODE, in, out);
                applied.fields[applied.count++] = {3, IntentValue::ofU64(_state.stream_speed_mode)};
                anyApplied = true;
            }
            if (const auto* f = findField(requested, 4)) {   // overshoot_clamp
                in.clear(); out.clear();
                in["on"] = fieldU64(f, 0) != 0;
                _webui.handleCommand(WS_OP_OVERSHOOT, in, out);
                applied.fields[applied.count++] =
                    {4, IntentValue::ofU64(_state.interp_clamp_overshoot ? 1u : 0u)};
                anyApplied = true;
            }
            if (!anyApplied) return Ret::err(NackCode::INVALID_VALUE);
            return Ret::ok(applied);
        }


        // ---- 0x0105 slopmotion-set → the live tuning knobs -----------------
        // M5c: what POST /api/slopmotion used to do. Every key optional; each
        // applied value is CLAMPED HERE to the same bounds the catalog
        // advertises and then echoed, so a client always renders what the
        // machine took rather than what it asked for.
        //
        // Writes land straight in SystemState. The Core-1 sampler copies these
        // into the engine config on its own tick (see main.cpp's per-tick
        // push), which is what makes them effective within ~1 ms without this
        // task ever touching the engine.
        case ch::sm_set: {
            applied.count = 0;
            bool any = false;
            auto cl = [](float v, float lo, float hi) {
                // NaN-safe: the !(v>lo) form returns lo for NaN, where a plain
                // v<lo comparison would let it through untouched.
                return !(v > lo) ? lo : (v > hi ? hi : v);
            };
            auto setF = [&](uint8_t key, float lo, float hi, volatile float& dst) {
                if (const auto* f = findField(requested, key)) {
                    const float v = cl(fieldF32(f, dst), lo, hi);
                    dst = v;
                    applied.fields[applied.count++] = {key, IntentValue::ofF32(v)};
                    any = true;
                }
            };
            auto setU = [&](uint8_t key, uint32_t lo, uint32_t hi, auto&& store) {
                if (const auto* f = findField(requested, key)) {
                    uint64_t raw = fieldU64(f, lo);
                    if (raw < lo) raw = lo;
                    if (raw > hi) raw = hi;
                    store(uint32_t(raw));
                    applied.fields[applied.count++] = {key, IntentValue::ofU64(raw)};
                    any = true;
                }
            };
            // Times are MILLISECONDS on the wire, microseconds in the engine.
            auto setMs = [&](uint8_t key, float lo, float hi, volatile uint32_t& dst) {
                if (const auto* f = findField(requested, key)) {
                    const float ms = cl(fieldF32(f, float(dst) / 1000.0f), lo, hi);
                    dst = uint32_t(ms * 1000.0f);
                    applied.fields[applied.count++] = {key, IntentValue::ofF32(ms)};
                    any = true;
                }
            };

            setF(1, 0.0f, 2000000.0f, _state.sm_tune_jmax_ovr);
            setF(2, 0.0f, 20.0f,      _state.sm_tune_vmax_ovr);
            setF(3, 0.0f, 500.0f,     _state.sm_tune_amax_ovr);
            setU(4, 0, 1, [&](uint32_t v) { _state.sm_tune_centering = (v != 0); });
            setF(5, 0.0f, 1.0f,       _state.sm_tune_centering_gain);
            setU(6, 0, 1, [&](uint32_t v) { _state.sm_tune_chase_ff = (v != 0); });
            setU(7, 0, 1, [&](uint32_t v) { _state.sm_tune_chase_aff = (v != 0); });
            setF(8, 0.0f, 1.5f,       _state.sm_tune_chase_gain);
            setF(9, 0.0f, 8.0f,       _state.sm_tune_chase_look);
            setMs(10, 10.0f, 500.0f,  _state.sm_tune_dense_us);
            setU(11, 0, 1, [&](uint32_t v) { _state.sm_tune_aim_extrap = (v != 0); });
            setF(12, 0.0f, 8.0f,      _state.sm_tune_handoff_k);
            setU(13, 0, 2, [&](uint32_t v) { _state.sm_tune_curve_policy = uint8_t(v); });
            setU(14, 0, 4, [&](uint32_t v) { _state.sm_tune_infeas_policy = uint8_t(v); });
            setF(15, 0.5f, 1.0f,      _state.sm_tune_infeas_margin);
            setF(16, 0.0f, 1.0f,      _state.sm_tune_smooth_budget);
            setF(17, 0.0f, 1.0f,      _state.sm_tune_amp_budget);
            setU(18, 1, 10, [&](uint32_t v) { _state.sm_tune_blend_steps = uint8_t(v); });
            setU(19, 0, 8,  [&](uint32_t v) { _state.sm_tune_reshape_steps = uint8_t(v); });
            setMs(20, 0.0f, 200.0f,   _state.sm_tune_settle_grace_us);

            if (!any) return Ret::err(NackCode::INVALID_VALUE);
            // PERSIST (operator ruling 2026-07-27): tuning survives a reboot.
            // Flagged rather than written here -- NVS is flash and this runs on
            // the hub task at up to 5 Hz; the service coalesces it onto its own
            // 1 Hz housekeeping tick so a slider drag costs ONE write, not
            // thirty.
            _smTuneDirty = true;
            return Ret::ok(applied);
        }


        // ---- 0x0106 machine-admin → the non-motion device actions ----------
        // Routed through WebUI::handleCommand like every other intent, so the
        // sole-caller rule and the existing OTA/idle deferrals apply unchanged.
        case ch::machine_admin: {
            const uint64_t op = fieldU64(findField(requested, 1), 0);
            switch (op) {
                case 1:  // clear_fault
                    _webui.handleCommand(WS_OP_CLEAR_FAULT, in, out);
                    SLOGI("slopsync", "machine-admin: clear_fault");
                    break;
                case 2:  // save_config
                    _webui.handleCommand(WS_OP_SAVE, in, out);
                    SLOGI("slopsync", "machine-admin: config saved to NVS");
                    break;
                case 3: {  // servo_scan
                    // The scan is ASYNC by design — it queues Modbus reads and
                    // returns immediately. Echoing the op (not a result) is the
                    // honest answer: "accepted, started", with the outcome
                    // arriving on the servo telemetry, not in this ECHO.
                    in["scan"] = true;
                    if (!_webui.handleCommand(WS_OP_GET_CFG, in, out)) {
                        return Ret::err(NackCode::INTERLOCK);
                    }
                    SLOGI("slopsync", "machine-admin: servo scan requested");
                    break;
                }
                default:
                    return Ret::err(NackCode::UNSUPPORTED_OP);
            }
            applied.count = 1;
            applied.fields[0] = {1, IntentValue::ofU64(op)};
            return Ret::ok(applied);
        }

        // ---- 0x0005 safety-intents ----------------------------------------
        // estop (op 6) and estop_clear (op 1) are HUB-handled and never reach
        // the delegate. Everything below returns accepted-or-UNSUPPORTED_OP,
        // and the HUB latches the resulting level/mode into the 0x0003
        // snapshot (RFC-025a) — delegate acceptance is the trigger, so a
        // machine that cannot do a level just says so and nothing is latched.
        case slopsync::channels::safety_intents: {
            uint64_t op = fieldU64(findField(requested, 1), 0);
            switch (op) {
                case slopsync::safety_ops::stop:
                case slopsync::safety_ops::hold:
                    _webui.handleCommand(WS_OP_HALT, in, out);  // hard-stop, stays homed
                    break;
                case slopsync::safety_ops::pause:
                    in["paused"] = true;
                    _webui.handleCommand(WS_OP_PAUSE, in, out);
                    break;
                case slopsync::safety_ops::resume:
                    in["paused"] = false;
                    _webui.handleCommand(WS_OP_PAUSE, in, out);
                    break;
                // RFC-025c: override/bypass are SAFETY-domain state now. They
                // ride the exact same WS handlers the legacy plane uses, so
                // there is ONE writer of _state.manual_override /
                // _state.bypass_limits and no second truth to drift; the hub's
                // mode bits are then reconciled from that state by syncSafety()
                // every tick, which keeps 0x0003 honest even when the change
                // came from the legacy UI instead of from here.
                case slopsync::safety_ops::override_on:
                case slopsync::safety_ops::override_off:
                    in["on"] = (op == slopsync::safety_ops::override_on);
                    _webui.handleCommand(WS_OP_OVERRIDE, in, out);
                    break;
                case slopsync::safety_ops::bypass_on:
                case slopsync::safety_ops::bypass_off:
                    in["on"] = (op == slopsync::safety_ops::bypass_on);
                    _webui.handleCommand(WS_OP_BYPASS, in, out);
                    break;
                default:
                    return Ret::err(NackCode::UNSUPPORTED_OP);
            }
            applied.count = 1;
            applied.fields[0] = {1, IntentValue::ofU64(op)};
            return Ret::ok(applied);
        }

        // ---- 0x0107 pattern-advanced-cmd -> WS_OP_GEN_CFG (ap_* fields) -----
        // Shared writer behind 0x008E..0x0094, the flattened-and-split
        // advanced-pattern settings surface (SlopSyncCatalog.h has the full
        // budget story). SlopDriveHubDelegate has no PatternEngine reference
        // of its own — this is the SAME seam 0x0102 pattern_cmd uses just
        // above (_webui.handleCommand -> WebUI::applyPattern's existing ap_*
        // handling), not a new one. WebUI::applyPattern's `touched[]`/
        // `ap_mods` echo (added alongside this channel) is what makes a
        // ground-truth reply possible when one wire frame lands sub-fields on
        // more than one base control at once.
        case ch::pattern_advanced_cmd: {
            if (_state.estop_latched) return Ret::err(NackCode::ESTOP_ACTIVE);

            static constexpr const char* kBaseJsonKeys[kApBaseCount] = {
                "ap_max_depth", "ap_min_depth", "ap_in_speed", "ap_out_speed",
                "ap_in_accel", "ap_out_accel",
            };
            static constexpr const char* kModJsonKeys[6] = {
                "amplitude", "in_step", "in_wait", "out_step", "out_wait", "offset",
            };

            if (const auto* f = findField(requested, 1)) in["ap_mode"] = fieldBool(f, false);
            if (const auto* f = findField(requested, 2)) in["ap_speed"] = int(fieldU64(f, 0));
            // Key = 3 + advpat::BaseId: 3 max_depth, 4 min_depth, 5 in_speed,
            // 6 out_speed, 7 in_accel, 8 out_accel — exactly 0x008E's layout.
            for (uint8_t id = 0; id < kApBaseCount; ++id) {
                if (const auto* f = findField(requested, uint8_t(3 + id)))
                    in[kBaseJsonKeys[id]] = int(fieldU64(f, 0));
            }
            // Modifier cycles: keys 9..44, base = 9 + 6*id, sub-offsets
            // amplitude+0 in_step+1 in_wait+2 out_step+3 out_wait+4 offset+5 —
            // exactly 0x008F..0x0094's wire layout. Build ap_mods with only
            // the sub-keys THIS request touches per control; applyModObject
            // (WebUI.cpp) reads the rest back from the engine.
            JsonArray modsArr;
            for (uint8_t id = 0; id < kApBaseCount; ++id) {
                const uint8_t base = uint8_t(9 + 6 * id);
                bool any = false;
                JsonObject m;
                for (uint8_t sub = 0; sub < 6; ++sub) {
                    const auto* f = findField(requested, uint8_t(base + sub));
                    if (!f) continue;
                    if (!any) {
                        if (modsArr.isNull()) modsArr = in["ap_mods"].to<JsonArray>();
                        m = modsArr.add<JsonObject>();
                        m["ctrl"] = id;
                        any = true;
                    }
                    m[kModJsonKeys[sub]] = int(fieldU64(f, 0));
                }
            }

            if (!_webui.handleCommand(WS_OP_GEN_CFG, in, out)) {
                return Ret::err(NackCode::INVALID_VALUE);
            }

            applied.count = 0;
            if (findField(requested, 1))
                applied.fields[applied.count++] = {1, IntentValue::ofBool(out["ap_mode"] | false)};
            if (findField(requested, 2))
                applied.fields[applied.count++] =
                    {2, IntentValue::ofU64(uint64_t(int(out["ap_speed"] | 0)))};
            for (uint8_t id = 0; id < kApBaseCount; ++id) {
                const uint8_t key = uint8_t(3 + id);
                if (findField(requested, key))
                    applied.fields[applied.count++] =
                        {key, IntentValue::ofU64(uint64_t(int(out[kBaseJsonKeys[id]] | 0)))};
            }
            // Modifier echo: out["ap_mods"] carries the POST-CLAMP block for
            // EVERY touched control — pull out just the sub-keys this request
            // actually asked for (Ground Truth without inventing values for
            // fields it never touched).
            if (out["ap_mods"].is<JsonArray>()) {
                for (JsonObject m : out["ap_mods"].as<JsonArray>()) {
                    int ctrl = m["ctrl"] | -1;
                    if (ctrl < 0 || ctrl >= int(kApBaseCount)) continue;
                    const uint8_t base = uint8_t(9 + 6 * ctrl);
                    for (uint8_t sub = 0; sub < 6; ++sub) {
                        const uint8_t key = uint8_t(base + sub);
                        if (!findField(requested, key)) continue;
                        applied.fields[applied.count++] =
                            {key, IntentValue::ofU64(uint64_t(int(m[kModJsonKeys[sub]] | 0)))};
                    }
                }
            }

            if (applied.count == 0) return Ret::err(NackCode::INVALID_VALUE);
            cfgChanged = false;  // session-volatile, same as classic pattern-cmd
            return Ret::ok(applied);
        }

        // ---- 0x0108 pattern-presets-cmd → PatternPresetStore CRUD (RFC-021) --
        // Retires POST /api/pattern/presets. `save`/`load` are the two ops that
        // touch live advanced-pattern state; `delete`/`rename` are pure store
        // bookkeeping. The roster (0x0096) republishes on the NEXT tick once it
        // notices the store's generation moved (publishPresetRoster) — no
        // special-case publish needed here.
        case ch::pattern_presets_cmd: {
            if (_presets == nullptr) return Ret::err(NackCode::UNSUPPORTED_OP);

            const uint64_t op = fieldU64(findField(requested, 1), 0);
            const uint64_t slotRaw = fieldU64(findField(requested, 2), uint64_t(PatternPresetStore::kCapacity));
            if (slotRaw >= PatternPresetStore::kCapacity) return Ret::err(NackCode::INVALID_VALUE);
            const uint8_t slotIdx = uint8_t(slotRaw);

            const auto* nameField = findField(requested, 3);
            std::string_view name;
            if (nameField && nameField->value.kind == IntentValue::Kind::Tstr) name = nameField->value.tstr_val;

            switch (op) {
                case 1: {  // save — captures LIVE advpat state (Ground Truth: a
                           // pure READ of the engine, never the request) into
                           // `slotIdx`, named `name`. Overwrites whatever was there.
                    if (name.empty()) return Ret::err(NackCode::INVALID_VALUE);
                    if (_presetPatternEngine == nullptr) return Ret::err(NackCode::UNSUPPORTED_OP);
                    const advpat::Settings& live = _presetPatternEngine->apSettings();
                    uint8_t payload[PatternPresetStore::kPayloadBytes];
                    // Same four scalars + six modifier blocks the retired HTTP
                    // handler's `def` carried ("never depths or master speed") —
                    // base = 4 + 6*id, id in advpat::BaseId order, matching
                    // 0x008F..0x0094's wire layout exactly (see the load case
                    // below, which runs this arithmetic in reverse).
                    payload[0] = live.in_speed.value;
                    payload[1] = live.out_speed.value;
                    payload[2] = live.in_accel.value;
                    payload[3] = live.out_accel.value;
                    for (uint8_t id = 0; id < advpat::BASE_COUNT; ++id) {
                        const advpat::BaseControl* bc = live.byId(id);
                        const uint8_t base = uint8_t(4 + id * 6);
                        payload[base + 0] = bc->modifier.amplitude;
                        payload[base + 1] = bc->modifier.in_step;
                        payload[base + 2] = bc->modifier.in_wait;
                        payload[base + 3] = bc->modifier.out_step;
                        payload[base + 4] = bc->modifier.out_wait;
                        payload[base + 5] = bc->modifier.offset;
                    }
                    if (!_presets->save(slotIdx, name, payload)) return Ret::err(NackCode::INVALID_VALUE);
                    SLOGI("slopsync", "preset saved: slot %u \"%.*s\"", unsigned(slotIdx),
                          int(name.size()), name.data());
                    applied.count = 3;
                    applied.fields[0] = {1, IntentValue::ofU64(1)};
                    applied.fields[1] = {2, IntentValue::ofU64(slotIdx)};
                    applied.fields[2] = {3, IntentValue::ofTstr(name)};
                    cfgChanged = false;
                    return Ret::ok(applied);
                }

                case 2: {  // load — decode the stored payload and apply it via
                           // the SAME validated path a client's own 0x0107 write
                           // uses (clamping included); ground truth arrives via
                           // the normal 0x008E/0x008F..0x0094 STATE broadcasts,
                           // no special echo. Also engages Advanced mode — a
                           // loaded preset is a pattern being asked for, not a
                           // value being previewed.
                    if (_state.estop_latched) return Ret::err(NackCode::ESTOP_ACTIVE);
                    const uint8_t* p = _presets->payload(slotIdx);
                    if (p == nullptr) return Ret::err(NackCode::INVALID_VALUE);
                    in["ap_mode"] = true;
                    in["ap_in_speed"]  = int(p[0]);
                    in["ap_out_speed"] = int(p[1]);
                    in["ap_in_accel"]  = int(p[2]);
                    in["ap_out_accel"] = int(p[3]);
                    JsonArray mods = in["ap_mods"].to<JsonArray>();
                    for (uint8_t id = 0; id < advpat::BASE_COUNT; ++id) {
                        const uint8_t base = uint8_t(4 + id * 6);
                        JsonObject m = mods.add<JsonObject>();
                        m["ctrl"]      = id;
                        m["amplitude"] = int(p[base + 0]);
                        m["in_step"]   = int(p[base + 1]);
                        m["in_wait"]   = int(p[base + 2]);
                        m["out_step"]  = int(p[base + 3]);
                        m["out_wait"]  = int(p[base + 4]);
                        m["offset"]    = int(p[base + 5]);
                    }
                    if (!_webui.handleCommand(WS_OP_GEN_CFG, in, out)) return Ret::err(NackCode::INVALID_VALUE);
                    SLOGI("slopsync", "preset loaded: slot %u", unsigned(slotIdx));
                    applied.count = 2;
                    applied.fields[0] = {1, IntentValue::ofU64(2)};
                    applied.fields[1] = {2, IntentValue::ofU64(slotIdx)};
                    cfgChanged = false;  // session-volatile, same as 0x0107
                    return Ret::ok(applied);
                }

                case 3: {  // delete
                    if (!_presets->remove(slotIdx)) return Ret::err(NackCode::INVALID_VALUE);
                    SLOGI("slopsync", "preset deleted: slot %u", unsigned(slotIdx));
                    applied.count = 2;
                    applied.fields[0] = {1, IntentValue::ofU64(3)};
                    applied.fields[1] = {2, IntentValue::ofU64(slotIdx)};
                    cfgChanged = false;
                    return Ret::ok(applied);
                }

                case 4: {  // rename
                    if (name.empty()) return Ret::err(NackCode::INVALID_VALUE);
                    if (!_presets->rename(slotIdx, name)) return Ret::err(NackCode::INVALID_VALUE);
                    SLOGI("slopsync", "preset renamed: slot %u -> \"%.*s\"", unsigned(slotIdx),
                          int(name.size()), name.data());
                    applied.count = 3;
                    applied.fields[0] = {1, IntentValue::ofU64(4)};
                    applied.fields[1] = {2, IntentValue::ofU64(slotIdx)};
                    applied.fields[2] = {3, IntentValue::ofTstr(name)};
                    cfgChanged = false;
                    return Ret::ok(applied);
                }

                default:
                    return Ret::err(NackCode::UNSUPPORTED_OP);
            }
        }

        default:
            // A cataloged INTENT channel the delegate doesn't implement.
            return Ret::err(NackCode::UNKNOWN_CHANNEL);
    }
}

void SlopDriveHubDelegate::onEstop(uint8_t cause, uint8_t origin) {
    (void)cause;
    (void)origin;
    // §11.2: motion must STOP before protocol bookkeeping. We set the exact
    // field set WS_OP_ESTOP writes — done inline (not via handleCommand) on
    // purpose: onEstop runs deep inside the hub's per-slot iteration on the
    // safety-critical path; the inline writes avoid a JsonDocument allocation
    // and the WS_OP_ESTOP _bumpGen side effect, and are byte-identical to that
    // handler's field set. estop_requested is the RMW motorTask consumes to
    // actually kill the pulse train on Core 1.
    _state.estop_requested.store(true);
    _state.estop_latched = true;
    _state.homed = false;
    _state.homing_in_progress = false;
    _state.paused = false;
    _state.manual_override = false;
    _state.resume_start_ms = 0;
    SLOGW("slopsync", "ESTOP latched via hub (cause=%u origin=%u)", cause, origin);
}

bool SlopDriveHubDelegate::canClearEstop() {
    // §11.2 machine-domain precondition: allow the clear ONLY once motion is
    // genuinely stopped — estop_requested has been consumed (RMW'd to false) by
    // motorTask on Core 1. While it is still pending, refuse (hub NACKs
    // CLEAR_REFUSED).
    if (_state.estop_requested.load()) return false;

    // Clean drop point for the firmware latch: this is the ONLY delegate hook
    // the hub calls on the clear path, and the hub guarantees the clear WILL
    // proceed iff we return true (it has already checked its own ESTOP bit is
    // set). So dropping the firmware latch here keeps both sides in lockstep.
    // Clearing NEVER rehomes: homed stays false, so motion stays refused
    // (NOT_HOMED) until an explicit HOME intent.
    _state.estop_latched = false;
    return true;
}

std::optional<uint8_t> SlopDriveHubDelegate::sourceForChannel(uint16_t channel_id) {
    if (channel_id == ch::move) return uint8_t(MotionSource::MANUAL);    // 0
    if (channel_id == ch::pattern_cmd) return uint8_t(MotionSource::PATTERN);  // 2
    if (channel_id == ch::motion_input) return uint8_t(MotionSource::TCODE_STREAM);  // 1
    // 0x0085 motion-segment shares the stream source: a client uses one channel
    // OR the other, both ARE "the stream input" (§11.4 ownership then guarantees
    // only one drives at a time).
    if (channel_id == ch::motion_segment) return uint8_t(MotionSource::TCODE_STREAM);  // 1
    return std::nullopt;
}

slopsync::SourceLossPolicy SlopDriveHubDelegate::sourcePolicy(uint8_t source_id) {
    // VESTIGIAL as of RFC-045 (Phase D): the hub library no longer calls this
    // — releaseSessionSources() latches nothing for any source class, so the
    // Stop-vs-Continue question it used to answer has no caller any more (see
    // hub_impl.hpp's comment). Kept only because HubDelegate's interface is
    // frozen-additive; PATTERN's actual "keep going after disconnect" fate is
    // now onSourceOwnership()'s `pattern_background_run` check, below.
    if (source_id == uint8_t(MotionSource::PATTERN)) return slopsync::SourceLossPolicy::Continue;
    return slopsync::SourceLossPolicy::Stop;
}

void SlopDriveHubDelegate::onDeadmanStop(uint8_t source_id) {
    (void)source_id;
    // VESTIGIAL as of RFC-045 (Phase D): the hub library no longer calls this
    // — a deadman fire never forces a stop for any source class (§11.3). Kept
    // only because HubDelegate's interface is frozen-additive.
    JsonDocument in;
    JsonDocument out;
    _webui.handleCommand(WS_OP_HALT, in, out);
    SLOGW("slopsync", "onDeadmanStop fired (unexpected post-RFC-045) on source %u — motion halted", source_id);
}

void SlopDriveHubDelegate::onSourceOwnership(uint8_t source_id, uint32_t owner_session, uint8_t reason) {
    // RFC-045: the hub library only ever RELEASES here (latches nothing) — see
    // hub_impl.hpp's releaseSessionSources(). `owner_session == 0` is a
    // release; anything else (acquire/takeover) needs no action from THIS
    // device — the arbiter already has the intent/stream that caused it.
    if (owner_session != 0) return;
    if (source_id != uint8_t(MotionSource::PATTERN)) return;  // command-driven sources settle on their own (§9.6)
    if (_state.pattern_background_run) return;  // RFC-045/048: policy says keep going, unattended

    // false (default): stop the generator. Fires identically whether the
    // owning session went STALE (RFC-042) or was genuinely torn down —
    // `reason` (3 deadman-release / 4 session-loss-release) is not consulted,
    // matching the library's own "reason-agnostic release" doctrine.
    if (_presetPatternEngine != nullptr && _presetPatternEngine->isRunning()) {
        _presetPatternEngine->stop();
        SLOGI("slopsync", "pattern generator stopped: owning session released source %u (reason %u), "
                          "background_run is off",
             source_id, reason);
    }
}

void SlopDriveHubDelegate::onSessionJoined(uint32_t session_id) {
    SLOGI("slopsync", "session %08x joined", session_id);
}

void SlopDriveHubDelegate::onSessionLeft(uint32_t session_id) {
    SLOGI("slopsync", "session %08x left", session_id);
}

// ---- 0x0084 motion-input + 0x0085 motion-segment STREAM -> pacing ring ------
// Runs on the SlopSyncHub task, synchronously inside _hub.update() (the hub
// has already validated caps/rate/ownership/deadman per the hub.hpp contract
// — see this method's declaration comment). Decodes each sample by FIXED
// OFFSET, not the generic layout_codec: the byte layouts below MUST mirror
// SlopSyncCatalog's 0x0084 (4 B chase point) / 0x0085 (6 B timed segment)
// entry field order exactly, the same convention publishTelemetry() already
// uses on the encode side for every other packed channel in this file. Both
// channels feed the SAME pacing ring and drain to the SAME Core-1 sampler
// queue — a client uses one OR the other, never both at once (§11.4 source
// ownership on TCODE_STREAM enforces that).
void SlopDriveHubDelegate::onStreamBundle(uint16_t channel_id, uint32_t session_id,
                                          const slopsync::BundleView& bundle) {
    const bool isSegment = (channel_id == ch::motion_segment);
    if (channel_id != ch::motion_input && !isSegment) return;

    // RFC-030: the session's GRANTED (effective, post-curve_policy) family,
    // looked up once per bundle and stamped on every segment entry below.
    // Chase points (0x0084) never carry one — the family is a waveform-
    // reconstruction concept and the engine only reads it on that path.
    const uint8_t curveFamily =
        (isSegment && _hub != nullptr) ? _hub->publishCurveFamily(session_id, channel_id) : 0;

    // t_base/t_off are u32 HUB-µs — the SAME wrapping domain EspClock::nowUs()
    // reads (esp_timer_get_time() truncated to 32 bits, §7.2). now64 stays the
    // FULL 64-bit esp_timer reading so due_us in the ring never itself wraps;
    // only the WIRE timestamp we're resolving against it does.
    const int64_t now64 = esp_timer_get_time();
    const uint32_t now32 = uint32_t(now64 & 0xFFFFFFFFull);

    uint32_t ringDrops = 0;
    uint32_t farClamped = 0;
    uint32_t badDuration = 0;

    const uint8_t n = bundle.sampleCount();
    for (uint8_t i = 0; i < n; ++i) {
        const uint32_t wireT = bundle.sampleTimeUs(i);
        // Nearest-window resolve (§7.2): a plain wrap-aware signed subtract,
        // since wireT is always near "now" by construction (bundle span is
        // capped at limits::bundle_max_span_ms, far under the 32-bit wrap).
        // For 0x0085 this is the intended segment START in hub time.
        int32_t delta = int32_t(wireT - now32);
        if (delta > 250000) { delta = 250000; ++farClamped; }
        if (delta < 0) delta = 0;
        const uint64_t due = uint64_t(now64 + int64_t(delta));

        const auto sample = bundle.sample(i);
        const uint16_t rawTarget = slopsync::getU16(sample.subspan(0, 2));

        PacingEntry e{};
        e.due_us = due;
        e.target = float(rawTarget) / 10000.0f;  // catalog scale 10000 (both channels)

        if (isSegment) {
            // 6-B layout: {target_norm u16, duration_ms u16, end_vel_norm i16}.
            const uint16_t rawDurMs = slopsync::getU16(sample.subspan(2, 2));
            const int16_t  rawEndV  = int16_t(slopsync::getU16(sample.subspan(4, 2)));
            if (rawDurMs == 0) {
                // durationless points belong on 0x0084 — skip + count dropped.
                ++badDuration;
                continue;
            }
            e.has_duration = true;
            e.duration_us  = uint32_t(rawDurMs) * 1000u;
            e.curve_family = curveFamily;
            if (rawEndV == kSegNoEndVel) {
                // -32768 sentinel = "no end velocity" (0 is a legit slope, so 0
                // cannot mean absent) → the engine estimates vf/af itself.
                e.has_end_vel = false;
            } else {
                e.vel         = float(rawEndV) / 1000.0f;  // catalog scale 1000
                e.has_end_vel = true;
            }
        } else {
            // 4-B layout: {target_norm u16, vel_norm i16}. 0 velocity = no
            // handoff (the pre-segment-channel convention, preserved verbatim).
            const int16_t rawVel = int16_t(slopsync::getU16(sample.subspan(2, 2)));
            e.vel         = float(rawVel) / 1000.0f;  // catalog scale 1000
            e.has_end_vel = (e.vel != 0.0f);
        }

        if (_pacingRing.push(e)) ++ringDrops;
    }

    _state.sm_sync_bundles = _state.sm_sync_bundles + 1;
    _state.sm_sync_samples = _state.sm_sync_samples + n;
    if (isSegment) _state.sm_sync_seg_bundles = _state.sm_sync_seg_bundles + 1;
    if (ringDrops || badDuration)
        _state.sm_sync_dropped = _state.sm_sync_dropped + ringDrops + badDuration;
    if (farClamped) {
        SLOGW_EVERY_MS(2000, "slopsync", "motion-stream: %u sample(s) clamped from far-future "
                       "t_off this window (missed CLOCK resync on the client?)",
                       (unsigned)farClamped);
    }
}

// RFC-030: the grant-plane echo of the curve family. The wish arrives already
// clamped to the registered curve_families range; this machine's curve_policy
// then decides what will actually be rendered, and THAT is what the grant
// reports — never the request (ground-truth doctrine). Note the deliberate
// asymmetry with the engine: the engine re-reads curve_policy every tick, so
// an operator flipping policy mid-session changes MOTION instantly, while the
// grant echo only refreshes on the next HELLO/PUBLISH. The echo is a
// declaration receipt, not live telemetry.
uint8_t SlopDriveHubDelegate::effectiveCurveFamily(uint16_t channel_id, uint8_t requested) {
    (void)channel_id;
    switch (_state.sm_tune_curve_policy) {
        case 1:  return slopsync::curve_families::c1_cubic;    // ForceC1
        case 2:  return slopsync::curve_families::c2_quintic;  // ForceC2
        default:
            // FollowClient honors what it can RENDER. No step renderer
            // exists (SPEC §18): the engine plans a step declaration as a
            // quintic, so the echo says quintic — claiming "step honored"
            // would be the exact lie the effective-family echo exists to kill.
            return (requested == slopsync::curve_families::step)
                       ? slopsync::curve_families::c2_quintic
                       : requested;
    }
}

// RFC-021 BLOB_REQ export for the pattern-preset store (0x0095, store_id 2).
// The hub already validated the access gate (0x0095's `control` floor) and
// resolved that this isn't the hub-served trust ledger before calling here —
// see hub_impl.hpp's resolveBlobBytes. Just hand back the slot's raw bytes.
std::optional<slopsync::HubDelegate::BlobView> SlopDriveHubDelegate::readBlob(uint8_t ns, uint8_t store_id,
                                                                              uint8_t slot) {
    if (ns != slopsync::blob_ns::store || store_id != 2 || _presets == nullptr) return std::nullopt;
    const uint8_t* p = _presets->payload(slot);
    if (p == nullptr) return std::nullopt;
    BlobView v{};
    v.bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(p), PatternPresetStore::kPayloadBytes);
    v.generation = _presets->generation();
    return v;
}

// ============================================================================
// SlopSyncHubService
// ============================================================================

// Fills `c` in place and hands back a reference to it, so the catalog can be
// built INSIDE the member-initializer list — `_hub`'s constructor encodes the
// catalog immediately, so it must already be populated by the time _hub is
// constructed, and _hub is a by-reference binding we cannot defer to the
// constructor BODY. buildSlopDriveCatalog() is an out-param builder precisely
// because a ~22 KB Catalog32 must never be a return value.
static slopsync::Catalog32& initCatalog(slopsync::Catalog32& c, DeviceFeatures feat) {
    if (!buildSlopDriveCatalog(c, feat)) {
        SLOGE("slopsync", "device catalog OVERFLOWED Catalog32 capacity — channels are MISSING");
    }
    // ---- Say WHICH failure this is, not just THAT it failed -----------------
    // encodeCatalog() returns 0 for two unrelated reasons: the scratch buffer
    // being too small, and the entry list not being strictly ascending by id
    // (its first loop refuses outright, at any buffer size). The existing
    // "DID NOT ENCODE (scratch N B)" line names only the first, so an ORDERING
    // mistake reads as a sizing problem and sends you off growing a buffer that
    // was never the constraint. That cost a build-flash-observe cycle at M5b.
    //
    // The order is a real wire requirement (§8.1), not a style rule, so this
    // check is cheap and belongs here regardless — it turns "advertises
    // NOTHING" into the exact pair of channels to go and swap.
    for (uint16_t i = 1; i < c.count; ++i) {
        if (c.entries[i].id <= c.entries[i - 1].id) {
            SLOGE("slopsync",
                  "catalog entries OUT OF ORDER at index %u: 0x%04X follows 0x%04X — "
                  "entries MUST ascend by id or encodeCatalog() refuses the whole catalog",
                  unsigned(i), unsigned(c.entries[i].id), unsigned(c.entries[i - 1].id));
            break;
        }
    }
    return c;
}

SlopSyncHubService::SlopSyncHubService(SystemState& state, WebUI& webui, MotionArbiter& arbiter,
                                       MotorDriver& motor)
    : _state(state),
      _webui(webui),
      _arbiter(arbiter),
      _motor(motor),
      _catalog(),
      _delegate(state, webui, arbiter, _pacingRing, _uiTokens),
      // RFC-016: the catalog IS the capability list, so the feature probe
      // happens HERE, before the hub computes the etag. A machine with no
      // current sensor never advertises 0x0087 at all — its absence is the
      // honest answer, where a channel of permanent zeros would be a lie a
      // client cannot detect.
      _hub(initCatalog(_catalog, DeviceFeatures{motor.hasCurrentSensor(), motor.hasPowerMonitor()}),
           _clock, _rng, _delegate, _crypto),
      _port() {}

void SlopSyncHubService::init() {
    // Cache what the catalog actually decided, rather than re-asking the driver
    // every tick — and, more importantly, so publish cadence and catalog
    // content can never disagree about whether this machine has a power plane.
    _hasPowerChannel = (_catalog.find(ch::power) != nullptr);
    _hasDieTemp = _motor.hasPowerMonitor();

    // A catalog that did not fit the hub's encode scratch produces ZERO bytes,
    // an etag over nothing, and an empty catalog served to every client — the
    // machine looks healthy and advertises nothing. It cost a live probe run to
    // find once (M5a's annotations pushed the encoding past the old 8 KB
    // default); it will never cost that again.
    const size_t encoded = _hub.catalogEncodedBytes();
    if (encoded == 0) {
        SLOGE("slopsync", "CATALOG DID NOT ENCODE (scratch %u B) — this hub advertises NOTHING",
              unsigned(slopsync::Hub::catalogScratchCapacity()));
    } else {
        SLOGI("slopsync", "catalog encodes to %u B of %u B scratch", unsigned(encoded),
              unsigned(slopsync::Hub::catalogScratchCapacity()));
    }

    loadPairing();
    _uiTokens.begin();

    // RFC-027(c) push-to-pair: AFTER loadPairing() so the factory-fresh check
    // (hasConfigureToken()) sees the restored ledger, not an empty one — a
    // machine that was already claimed must never re-open a configure-granting
    // window just because it also happened to reboot three times fast.
    checkQuickBootPairingGesture();

    // Close the delegate -> ledger loop. MUST come after loadPairing() so the
    // very first HELLO of the boot is validated against the RESTORED ledger and
    // not an empty one — a paired client reconnecting during boot would
    // otherwise be told `watch` and have no way to know it was a timing
    // accident. Both objects exist by now; see SlopDriveHubDelegate::bindPairing
    // for why this is a post-construction bind rather than a ctor reference.
    _delegate.bindPairing(_hub.pairing());
    _delegate.bindHub(_hub);  // RFC-030: onStreamBundle reads granted curve families
    SLOGI("slopsync", "auth ENFORCED — /uitoken -> ledger (%u paired) -> watch",
          unsigned(_hub.pairing().entryCount()));

    // RFC-016(a): in-band hub identity on WELCOME key 37. String literals are
    // rodata, satisfying setIdentity's outlives-the-hub contract. fw_version's
    // ONLY other home is mDNS TXT — "what firmware is this machine running"
    // finally has an answer a client can get without HTTP.
    _hub.setIdentity("slopdrive-32", FIRMWARE_VERSION, "");

    // RFC-048: the hub's DURABLE cross-boot identity (WELCOME identity key 5,
    // also carried by DISCOVER_REPLY, §13.8) — generated ONCE with the
    // hardware RNG and persisted in NVS; every later boot just reads it back.
    // Blocking NVS I/O here is the CLAUDE.md §2 boot-sequence exception (this
    // runs once, before the hub task exists), same as checkQuickBootPairingGesture().
    {
        Preferences prefs;
        if (prefs.begin("slopsync", false)) {
            uint64_t id = prefs.getULong64("hubid", 0);
            if (id == 0) {
                // esp_random() x2: a single call only yields 32 bits.
                id = (uint64_t(esp_random()) << 32) | uint64_t(esp_random());
                if (id == 0) id = 1;  // astronomically unlikely, but 0 means "unset" — never persist it
                prefs.putULong64("hubid", id);
                SLOGI("slopsync", "hub_instance_id generated: %016llX", (unsigned long long)id);
            }
            prefs.end();
            _hub.setHubInstanceId(id);
        } else {
            SLOGW("slopsync", "hub_instance_id: NVS unavailable — WELCOME/DISCOVER_REPLY omit it this boot");
        }
    }

    // RFC-021 pattern-preset store (M5): load/migrate before binding, same
    // ordering reason as pairing above — the delegate must never see an
    // instant where it's bound to a store the boot-time migration hasn't run
    // against yet.
    loadPresets();
    _delegate.bindPresets(_presets);

    // ---- Wall clock (RFC-029's first_seen/last_seen) -----------------------
    // THIS DEVICE HAS NO WALL CLOCK. There is no SNTP client anywhere in the
    // firmware (checked, not assumed — no configTime(), no sntp_*), and protocol
    // time is boot-relative and wraps in ~71 minutes (§7.2), so it cannot stand
    // in. Feeding 0 is the REGISTERED honest answer: first_seen/last_seen then
    // mean "unknown" and the ledger simply omits them. Inventing a plausible
    // epoch would be worse than none — a trust ledger that dates entries wrongly
    // is a ledger an operator cannot audit. If SNTP ever lands, this one call
    // moves to wherever time is acquired and every entry starts dating itself.
    _hub.setWallClockSeconds(0);

    _port.begin(&_hub);

#if defined(BLE_ENABLED)
    // RFC-043 (Phase E): the BLE GATT ITransport + advertising. "SD32" is the
    // shortened name the legacy ≤31-byte advertising budget can afford
    // (§13.4); the full "SlopDrive-32" name rides the scan response.
    _blePort.begin(&_hub, "SlopDrive-32", "SD32");
#endif

    // RFC-046 (Phase E): the UDP discovery responder — the WS-side discovery
    // path for a LAN client without BLE (§13.8). Started after the WS port so
    // ws_port below is meaningful the instant the first probe can arrive;
    // hub_instance_id was resolved just above.
    _udpDiscovery.begin("slopdrive-32", _hub.hubInstanceId(), SLOPSYNC_WS_PORT, FIRMWARE_VERSION,
                         _hub.catalogEtag());

    // RFC-017: arm the log bridge now — from here on every SlopLog line is also
    // an in-band 0x0008 EVENT. Deliberately AFTER the boot narration and
    // immediately before the task that drains the hand-off ring exists.
    applogSyncBridgeArm();

    // 16 KB stack: the HELLO path proved capable of several KB of frame
    // buffers + WS-handshake stack on top of baseline (an 8 KB stack blew
    // its canary in the field even after the ~9 KB HubSession::reset()
    // temporary was eliminated at the source). Internal RAM is plentiful
    // post-PSRAM-relocation; this is cheap insurance on the safety plane.
    BaseType_t ok = xTaskCreatePinnedToCore(&SlopSyncHubService::taskTrampoline, "SlopSyncHub", 16384, this,
                                            2, &_task, 0);
    if (ok != pdPASS) {
        SLOGE("slopsync", "FAILED to create SlopSyncHub task");
        _task = nullptr;
    } else {
        SLOGI("slopsync", "hub service up — catalog %u channels, Core 0", unsigned(_catalog.count));
    }

    // ---- M4c: the deferred-signing worker ----------------------------------
    // INLINE SIGNING MUST STAY OFF ON THIS PART. Hub::setInlineSigning(true)
    // would move signP256() onto the hub task — 30-80 ms inside a 5 ms tick that
    // also paces STATE, runs the deadman and drains the motion-input ring, i.e.
    // one client's handshake punching a visible hole in another client's motion
    // stream. It would ALSO be a second task inside EspCrypto's mbedtls group
    // state, which the signing task is otherwise the sole user of. The default
    // is false and this device never flips it; the option exists for hosts and
    // for parts that actually have an ECC accelerator (C3/C6/H2 — not the S3).
    // Queues first, task second: the task's very first act is a keygen and it
    // must never find a null queue on the far side.
    _signReqQ = xQueueCreate(2, sizeof(SignRequest));
    _signResQ = xQueueCreate(2, sizeof(SignResult));
    if (_signReqQ == nullptr || _signResQ == nullptr) {
        SLOGE("slopsync", "sign queues alloc FAILED — hub authenticity disabled");
    } else {
        // ---- STACK SIZE: 8192 B, and here is the reasoning ------------------
        // What runs on it: mbedtls_ecp_gen_privkey once, then
        // mbedtls_ecdsa_sign_det_ext forever. The heavy structures are NOT on
        // this stack — mbedtls heap-allocates every MPI limb array and the
        // ecp_mul comb table — so the cost is call frames plus the HMAC_DRBG +
        // SHA-256 contexts that sign_det_ext keeps as locals, which puts the
        // real peak in the 2-4 KB band for P-256.
        //
        // 8 KB is therefore roughly 2x headroom, and it is deliberate rather
        // than generous: this project has had TWO stack incidents (a ~9 KB
        // whole-object-reassignment reset temporary that blew an 8 KB task on
        // every client connect -- see hub-is-single-task memory / CLAUDE.md §8
        // field bug #1 -- and a 320 KiB by-value catalog it narrowly avoided),
        // and a guessed-tight crypto stack that overflows only on the rare code path
        // where the scalar has an unusual bit pattern is exactly the kind of bug
        // this codebase should not ship. It is 8 KB of internal RAM on a part
        // where the WHOLE SlopSync service was moved to PSRAM to protect the
        // heap — cheap insurance, and the high-water mark is logged after the
        // first sign so on the first flash the number stops being an estimate.
        //
        // PRIORITY 1 = below the hub task (2), below motion, below comms. A
        // sign is allowed to take as long as it likes; nothing waits on it
        // except a client's authenticity check, which is already asynchronous by
        // protocol design (HUB_SIG arrives after WELCOME).
        BaseType_t sok = xTaskCreatePinnedToCore(&SlopSyncHubService::signTaskTrampoline, "SlopSyncSign",
                                                 8192, this, 1, &_signTask, 0);
        if (sok != pdPASS) {
            SLOGE("slopsync", "FAILED to create SlopSyncSign task — hub authenticity disabled");
            _signTask = nullptr;
        }
    }
}

void SlopSyncHubService::attachHttpRoutes(SlopHttpServer* server) {
    _uiTokens.attachRoutes(server);
}

void SlopSyncHubService::taskTrampoline(void* arg) {
    static_cast<SlopSyncHubService*>(arg)->taskLoop();
}

void SlopSyncHubService::taskLoop() {
    const TickType_t period = pdMS_TO_TICKS(5);
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, period);

        // OTA flash-write window: skip all XIP/WS/hub work. OtaService also
        // suspendTask()s us outright (belt AND braces) — this flag guard covers
        // the gap between the flag rising and the suspend landing.
        if (_state.ota_active.load(std::memory_order_relaxed)) continue;

        _port.loop();                 // service WS: accept/read/heartbeat/stall-sweep
#if defined(BLE_ENABLED)
        _blePort.loop();              // service BLE: deferred attach/detach (TRAPS T5)
#endif
        _udpDiscovery.poll();         // RFC-046: drain + answer pending DISCOVER_PROBEs
        _hub.update(_clock.nowUs());  // pump every session: frames, pacing, deadman (fires onStreamBundle)
        drainMotionStream();          // pop due 0x0084 pacing-ring entries -> Core-1 sampler queue
        syncSafety();
        publishTelemetry();
        // AFTER publishTelemetry, per hub.hpp's contract: bump the generation
        // once the change has been applied AND its STATE republished, so no
        // client can ever see a new cfg_gen advertising config it has not been
        // sent yet.
        pumpConfigGeneration();       // RFC-011: machine-side config change -> cfg_gen
        publishAnomalies();           // Core-1 anomaly ring -> 0x0089 EVENTs
        drainLogBridge();             // RFC-017: httpTask's SlopLog ring -> 0x0008 EVENTs
        pumpSigning();                // M4c: hub <-> SlopSyncSign task
    }
}

// ============================================================================
// RFC-017: the SlopLog -> 0x0008 log-channel bridge (consumer half)
// ============================================================================
//
// Producer is the SlopSyncSink in AppLog.cpp, on httpTask. This is the ONLY
// place a log line is allowed to become a frame, because slopsync::Hub is
// mutex-free and single-task by contract — publishLog() straight from the drain
// task would be a data race on the safety plane, which is the worst class of bug
// this codebase can have.
//
// BOUNDED PER TICK for the same reason publishAnomalies() is: one burst of log
// lines must not turn a 5 ms tick into a dozen CBOR encodes and fan-outs on the
// task that also pumps the WS port, the deadman and the motion-input drain. The
// ring's own drop counter covers the overflow, and 8/tick is 1600 lines/s of
// headroom against a logger whose whole design point is that it never floods.
//
// NO RECURSION IS POSSIBLE, which is the caveat hub.hpp warns about: anything
// this path logs goes into the SlopLog ring and comes back around on a later
// tick as data. It can never re-enter publishLog() from inside its own transport
// writes.
void SlopSyncHubService::drainLogBridge() {
    // RFC-017's serial handoff: rebind from "first /api/log HTTP serve" to
    // "first log-channel GRANT". Checked before the drain so the very first
    // in-band line already goes out with serial demoted.
    if (!_logGrantSeen) {
        for (size_t i = 0; i < slopsync::kHubMaxSessions + 1; ++i) {
            const slopsync::HubSession* s = _hub.sessionBySlot(i);
            if (s == nullptr || !s->occupied()) continue;
            if (s->subs.find(slopsync::channels::log) == nullptr) continue;
            _logGrantSeen = true;
            // Serial demotes to Warn+ exactly as it does for the legacy HTTP
            // handshake. BOTH triggers stay live during the transition: the
            // WebUI still polls /api/log and would otherwise lose its own
            // handoff the day this lands. When /api/log retires, its call goes
            // and this one is the whole mechanism.
            applogSerialQuiet();
            SLOGI("slopsync", "log channel granted in-band — serial demoted to Warn+");
            break;
        }
    }

    SystemState::SlopLogRec rec;
    for (int i = 0; i < 8 && _state.slopLogPop(rec); ++i) {
        // sloplog::Level and the registry's `log_levels` are the same numbers
        // 0..5 by construction (static_asserted in AppLog.cpp), so this is a
        // cast and not a translation table.
        if (_hub.publishLog(rec.level, rec.tag, rec.msg)) ++_logPublished;
    }
}

// ============================================================================
// M4c: the deferred-signing shuttle (RFC-029 item 1)
// ============================================================================
//
// Runs on the hub task. Both Hub calls live HERE and nowhere else; the actual
// ECDSA happens on "SlopSyncSign" (see signTaskLoop). Results first so a
// signature that finished during the last 5 ms reaches its client on this tick.
void SlopSyncHubService::pumpSigning() {
    if (_signReqQ == nullptr || _signResQ == nullptr) return;

    SignResult res;
    while (xQueueReceive(_signResQ, &res, 0) == pdTRUE) {
        // submitSignature() re-checks the session id itself — a slow signer's
        // answer arriving after its session died and a new client took the slot
        // is dropped there, never delivered into the wrong session.
        std::array<std::byte, slopsync::kTrustSigMaxBytes> sig{};
        for (size_t i = 0; i < res.len && i < sig.size(); ++i) sig[i] = std::byte(res.sig[i]);
        if (_hub.submitSignature(res.session_id, std::span<const std::byte>(sig.data(), res.len)))
            ++_signsDone;
    }

    // One job per tick: a burst of connecting clients queues up rather than
    // filling the request queue with work the signer cannot start for seconds.
    if (uxQueueSpacesAvailable(_signReqQ) == 0) return;
    slopsync::Hub::SignJob job;
    if (!_hub.takePendingSignJob(job)) return;
    SignRequest req{};
    req.session_id = job.session_id;
    for (size_t i = 0; i < sizeof(req.message); ++i) req.message[i] = uint8_t(job.message[i]);
    if (xQueueSend(_signReqQ, &req, 0) != pdTRUE) {
        // Job already taken out of the hub (takePendingSignJob is at-most-once
        // by design), so a failed send costs this session its signature. The
        // client reads that as "unverified" and handles it; the spaces check
        // above makes it effectively unreachable.
        SLOGW("slopsync", "sign job dropped — request queue full (session %08x)", req.session_id);
    }
}

void SlopSyncHubService::signTaskTrampoline(void* arg) {
    static_cast<SlopSyncHubService*>(arg)->signTaskLoop();
}

// Low-priority (1), Core 0, 8 KB stack. NOTHING here touches slopsync::Hub.
void SlopSyncHubService::signTaskLoop() {
    // Keygen runs HERE, not in init(): it is 40-100 ms of blocking ECP and this
    // task owns a stack sized for exactly that work, where setup()'s Arduino
    // loop-task stack is shared with everything that ran before it. Boot is not
    // delayed at all; the hub simply has no public key for the first ~100 ms,
    // which only matters to a PAIR_GRANT that cannot arrive that fast anyway.
    _crypto.begin(_state);
    SLOGI("slopsync", "sign task ready (keygen %u us, stack free %u B)", unsigned(_crypto.keygenUs()),
          unsigned(uxTaskGetStackHighWaterMark(nullptr)));

    SignRequest req;
    for (;;) {
        if (xQueueReceive(_signReqQ, &req, portMAX_DELAY) != pdTRUE) continue;
        // OTA: a flash-write window is no time to be burning 80 ms of CPU with
        // the cache disabled around us. Drop the job; the client stays
        // unverified, which is a correct answer during a firmware update.
        if (_state.ota_active.load(std::memory_order_relaxed)) continue;

        SignResult res{};
        res.session_id = req.session_id;
        std::array<std::byte, slopsync::kTrustSigMaxBytes> sig{};
        std::array<std::byte, slopsync::kHubSigMaterialBytes> msg{};
        for (size_t i = 0; i < msg.size(); ++i) msg[i] = std::byte(req.message[i]);

        const size_t n = _crypto.signP256(std::span<const std::byte>(msg), std::span<std::byte>(sig));
        if (n == 0) continue;  // no keypair / sign failed — "cannot sign" is conformant
        res.len = uint8_t(n);
        for (size_t i = 0; i < n; ++i) res.sig[i] = uint8_t(sig[i]);
        xQueueSend(_signResQ, &res, pdMS_TO_TICKS(10));

        SLOGI_EVERY_MS(30000, "slopsync", "P-256 sign %u us, sign-task stack free %u B",
                       unsigned(_crypto.lastSignUs()), unsigned(uxTaskGetStackHighWaterMark(nullptr)));
    }
}

// ============================================================================
// RFC-011: the MACHINE-SIDE half of the one cfg_gen rule
// ============================================================================
//
// The client-driven half lives in the delegate (applyIntent reports cfgChanged
// and the hub bumps). This is the other half M3b deliberately left unwired: a
// config change that did NOT come from an intent — a physical control, boot
// adoption, an internal recalculation, the legacy HTTP/WS settings plane — must
// still advance `cfg_gen`, or a client's `precondition` CAS silently succeeds
// against config that already moved.
//
// WHY A VALUE COMPARE AND NOT _state.cfg_gen: the firmware's own generation
// counter is bumped by ~20 call sites including pattern edits, homing and mode
// changes, none of which are protocol CONFIG. Keying off it would bump on
// session-volatile changes, which is exactly the amplification RFC-002 removed
// from the other half. So the detector watches the fields 0x0081 actually
// publishes — the same set the delegate snapshots — which IS the definition of
// "an applied configuration value changed".
//
// Runs on the hub task, so no ring is needed: the crossing is a poll of state
// the writers already publish, not a call into the Hub from another task.
void SlopSyncHubService::pumpConfigGeneration() {
    const auto& cfg = _state.config;
    CfgSnapshot now{cfg.min_position_mm,      cfg.max_position_mm,
                    cfg.user_max_speed_mm_s,  cfg.user_max_accel_mm_s2,
                    cfg.input_max_speed_mm_s, cfg.input_max_accel_mm_s2,
                    cfg.max_rail_mm,          cfg.input_max_jerk_mm_s3,
                    true};

    // Whether or not the config moved, consume the delegate's flag: it is a
    // per-change token and must not survive to be spent on a later, genuinely
    // machine-side change.
    const bool fromIntent = _delegate.takeCfgIntentFlag();

    if (!_cfgSnap.valid) {  // first tick: adopt, never bump
        _cfgSnap = now;
        return;
    }
    const bool changed =
        now.window_min != _cfgSnap.window_min || now.window_max != _cfgSnap.window_max ||
        now.user_speed != _cfgSnap.user_speed || now.user_accel != _cfgSnap.user_accel ||
        now.input_speed != _cfgSnap.input_speed || now.input_accel != _cfgSnap.input_accel ||
        now.max_rail != _cfgSnap.max_rail || now.input_jerk != _cfgSnap.input_jerk;
    if (!changed) return;
    _cfgSnap = now;

    if (fromIntent) return;  // the hub already bumped for this edit

    _hub.bumpConfigGeneration();
    SLOGI("slopsync", "machine-side config change — cfg_gen -> %u", unsigned(_hub.cfgGen()));
}

void SlopSyncHubService::syncSafety() {
    // Keep the hub's ESTOP latch in lockstep with the firmware's estop_latched
    // in BOTH directions (Ground Truth — a SlopSync viewer must see exactly
    // what the device is in).
    bool fw = _state.estop_latched;
    bool hubLatched = _hub.estopLatched();
    if (fw && !hubLatched) {
        // Firmware estopped (any transport) while the hub isn't: latch it. This
        // calls delegate.onEstop() first (idempotent re-set) then publishes the
        // critical-priority safety snapshot. cause 0 = user, origin 0.
        _hub.latchEstop(0, 0, ++_estopSeq);
    } else if (!fw && hubLatched) {
        // Firmware left the e-stopped state (e.g. a HOME cycle cleared it, or a
        // bench force_home did) while the hub still holds the latch: clear it.
        // clearEstop() consults canClearEstop() and is a no-op if motion isn't
        // actually stopped yet.
        _hub.clearEstop();
    }

    // RFC-025c: the same both-directions reconciliation for the 0x0003 `modes`
    // byte. manual_override and bypass_limits have FOUR writers between them
    // (the legacy WS ops, the HTTP settings plane, the SlopSync ops via
    // applyIntent, and onEstop's forced override drop), so the honest source of
    // truth is SystemState itself — read it every tick and let the hub publish
    // only on a real change. Without this the snapshot would be right only for
    // changes that happened to arrive over SlopSync, which is exactly the kind
    // of "the UI lies about machine state" defect the Ground Truth doctrine
    // calls a safety defect on this product.
    _hub.setSafetyModes(_state.manual_override, bool(_state.bypass_limits));
}

void SlopSyncHubService::drainMotionStream() {
    PacingEntry entry;
    const uint64_t now64 = uint64_t(esp_timer_get_time());

    while (_pacingRing.popDue(now64, entry)) {
        // ---- Gates: standard motion-command early-outs ----------------------
        if (!_state.homed) {
            _state.sm_sync_dropped = _state.sm_sync_dropped + 1;
            continue;
        }
        if (_state.paused || _state.manual_override) {
            _state.resume_start_ms = millis();
            _state.sm_sync_dropped = _state.sm_sync_dropped + 1;
            continue;
        }

        // ---- Sampler gating stamps -------------------------------------------
        // Without these the Core-1 sampler never drives.
        const uint32_t now = millis();

        // New-stream soft start — stamp resume so safeSpeedCap eases the
        // first move, evaluated against the OLD last_intiface_ms.
        if (_state.last_intiface_ms == 0 || (now - _state.last_intiface_ms) > 2000) {
            _state.resume_start_ms = now;
        }

        // Cadence measurement (EMA), 0.7/0.3 filter + <1000 ms gap window,
        // feeding the UI rate readout.
        if (_state.last_cmd_ms != 0) {
            const uint32_t gap = now - _state.last_cmd_ms;
            if (gap > 0 && gap < 1000) {
                if (_state.measured_interval_ms <= 0.0f)
                    _state.measured_interval_ms = float(gap);
                else
                    _state.measured_interval_ms =
                        0.7f * _state.measured_interval_ms + 0.3f * float(gap);
            }
        }
        _state.last_cmd_ms      = now;
        _state.last_intiface_ms = now;  // marks the stream active for the sampler

        // Yield-on-MOTION stamp: only a sample that actually moves the
        // commanded target counts as "the stream is driving".
        if (_syncPrevTarget < 0.0f || fabsf(entry.target - _syncPrevTarget) > 0.003f) {
            _state.last_intiface_move_ms = now;
            _syncPrevTarget = entry.target;
        }

        // Pre-planning demand telemetry (0x0080 raw_10um): the window-mapped
        // mm this entry's normalized target represents, one stage upstream of
        // commanded_target_mm (still-normalized Command.target, planned by
        // the engine below). Same window bounds pumpConfigGeneration snapshots
        // (_state.config.min/max_position_mm) — mirrors MachineSim::normToMm.
        _state.commanded_raw_mm = _state.config.min_position_mm +
            entry.target * (_state.config.max_position_mm - _state.config.min_position_mm);

        // Both stream channels land here; the ingress decode already resolved
        // has_end_vel (0x0084: vel≠0; 0x0085: sentinel) and has_duration/
        // duration_us (0x0085 waveform segments only), so the Command is a
        // straight copy — the drain is channel-agnostic by construction.
        slopmotion::Command cmd;
        cmd.target       = entry.target;
        cmd.end_vel      = entry.vel;
        cmd.has_end_vel  = entry.has_end_vel;
        cmd.duration_us  = entry.duration_us;
        cmd.has_duration = entry.has_duration;
        cmd.client_curve_family = entry.curve_family;  // RFC-030: FollowClient's input

        // ---- RFC-008 one-segment LOOKAHEAD --------------------------------
        // The whole hub-side handoff sanity guard reduces, here, to answering
        // ONE question: "how fast does the segment AFTER this one move, on
        // average?" The engine does the bounding (slopmotion::
        // boundHandoffVelocity — it also owns the chord of the segment being
        // planned, measured from the machine's real position); ingress owns
        // only the fact that a successor exists and what its chord is.
        //
        // WHY HERE AND NOT EARLIER: this is the LAST possible moment before the
        // command crosses to Core 1, so it is the moment with the MOST
        // information. Bounding at push time (onStreamBundle) would see less of
        // the future, not more.
        //
        // WHY HERE AND NOT LATER: the Core-1 sampler queue is not a lookahead —
        // the ring releases entries only as they come DUE, so the queue holds
        // 0-1 commands. The schedule-ahead knowledge lives in the ring and
        // nowhere else, and it dies the moment an entry is popped.
        //
        // Both segments must be real timed segments: a durationless 0x0084
        // chase point has no chord (no duration to divide by) and a mixed
        // stream cannot happen anyway (§11.4 source ownership gives one client
        // one motion source). No successor -> has_next_chord stays false and
        // the engine plans exactly as it did before the guard existed. That
        // TAIL CASE is a deliberate accept-unchanged: guessing a chord we do
        // not have would trim well-behaved senders, and the segment is DUE, so
        // deferring it to wait for its successor would trade a shape problem
        // for a deadline problem. The legality scan + Ruckig guard remain the
        // backstop they have always been, so nothing is less safe.
        if (entry.has_duration && entry.has_end_vel) {
            const PacingEntry* next = _pacingRing.peekOldest();
            if (next && next->has_duration && next->duration_us > 0) {
                const float next_dur_s = float(next->duration_us) * 1e-6f;
                cmd.next_chord     = fabsf(next->target - entry.target) / next_dur_s;
                cmd.has_next_chord = true;
            }
        }

        if (_motionStreamQueue && xQueueSend(_motionStreamQueue, &cmd, 0) == pdTRUE) {
            _state.sm_sync_enqueued = _state.sm_sync_enqueued + 1;
        } else {
            _state.sm_sync_dropped = _state.sm_sync_dropped + 1;
        }
    }
}

void SlopSyncHubService::publishTelemetry() {
    uint32_t now = millis();

    // ---- 0x0080 motion — ≥16 ms (≤~60 Hz) --------------------------------
    // 9 B as of M5a — "raw_10um" appended. MUST stay byte-for-byte in step
    // with the 0x0080 layout in SlopSyncCatalog.h.
    if (now - _lastMotionMs >= 16) {
        _lastMotionMs = now;
        std::array<std::byte, 9> buf{};
        std::span<std::byte> s(buf);
        uint16_t pos10 = clampU16(_state.actual_position_mm.load(std::memory_order_relaxed) * 100.0f);
        uint16_t tgt10 = clampU16(_state.commanded_target_mm * 100.0f);
        // The PRE-PLANNING demand: what the controlling input asked for, mapped
        // into the stroke window, one stage upstream of commanded_target_mm.
        // Stamped by drainMotionStream() for the SlopSync motion-stream path.
        uint16_t raw10 = clampU16(_state.commanded_raw_mm * 100.0f);
        int16_t spd10 = clampI16(_state.live_speed_mm_s.load(std::memory_order_relaxed) * 10.0f);
        uint8_t flags = 0;
        if (_state.homed) flags |= 1u << 0;
        if (_state.homing_in_progress) flags |= 1u << 1;
        if (_state.gen_active) flags |= 1u << 2;
        if (_state.paused) flags |= 1u << 3;
        if (_state.manual_override) flags |= 1u << 4;
        if (_state.estop_latched) flags |= 1u << 5;
        if (_state.last_intiface_ms != 0 && (now - _state.last_intiface_ms) < 250) flags |= 1u << 6;
        slopsync::putU16(s.subspan(0, 2), pos10);
        slopsync::putU16(s.subspan(2, 2), tgt10);
        slopsync::putU16(s.subspan(4, 2), uint16_t(spd10));
        slopsync::putU8(s.subspan(6, 1), flags);
        slopsync::putU16(s.subspan(7, 2), raw10);
        _hub.publishState(ch::motion, s);
    }

    // ---- 0x0081 machine-config — on cfg_gen change -----------------------
    uint16_t gen = _state.cfg_gen.load(std::memory_order_relaxed);
    if (!_cfgEverSent || gen != _lastCfgGen) {
        _cfgEverSent = true;
        _lastCfgGen = gen;
        // 37 B — MUST stay byte-for-byte in step with the 0x0081 layout in
        // SlopSyncCatalog.h (field 7 "input_jerk" appended in fw 2.1.47,
        // field 8 "enabled_mask" appended at M5a, field 9 "measured_stroke"
        // appended in fw 2.1.76 / item 3). max_rail (field 6) became a real
        // setting in the SAME release (item 1) — that is a metadata change
        // only, it keeps its byte 24 offset.
        std::array<std::byte, 37> buf{};
        std::span<std::byte> s(buf);
        slopsync::putF32(s.subspan(0, 4), _state.config.min_position_mm);
        slopsync::putF32(s.subspan(4, 4), _state.config.max_position_mm);
        slopsync::putF32(s.subspan(8, 4), _state.config.user_max_speed_mm_s);
        slopsync::putF32(s.subspan(12, 4), _state.config.user_max_accel_mm_s2);
        slopsync::putF32(s.subspan(16, 4), _state.config.input_max_speed_mm_s);
        slopsync::putF32(s.subspan(20, 4), _state.config.input_max_accel_mm_s2);
        slopsync::putF32(s.subspan(24, 4), _state.config.max_rail_mm);
        slopsync::putF32(s.subspan(28, 4), _state.config.input_max_jerk_mm_s3);
        // RFC-009 enabled_mask, bit i = the i-th setting-annotated field:
        //   0 window_min 1 window_max 2 user_speed 3 user_accel
        //   4 input_speed 5 input_accel 6 max_rail 7 input_jerk
        //
        // ALL EIGHT, ALWAYS — and that is the HONEST publish, not a stub.
        // I went looking for a gate to make this dynamic and there isn't one:
        // the delegate's 0x0101 handler has no e-stop, homed, or pause guard,
        // and applySettings clamps values rather than refusing them (its only
        // refusal is min >= max, which is value VALIDATION and belongs to
        // min/max, not to enablement). Publishing a bit low here to make the
        // field look busy would be a UI that lies about machine state, which
        // on this product is a safety defect — so the mask says what is true:
        // every limit on this machine is editable at all times, including
        // while latched, which is exactly when an operator wants to lower one.
        // 0x0082's mask is where this field earns its keep dynamically.
        slopsync::putU8(s.subspan(32, 1), 0xFF);
        // measured_stroke (item 3): the REAL homing measurement, distinct
        // from max_rail above. Item 4: a value carried over from a PRIOR
        // boot's NVS restore (ConfigStore::load() seeds this into the motor
        // regardless of _state.homed) must never overstate the CONFIGURED
        // ceiling while this session hasn't yet earned "measurement wins" by
        // completing a fresh home — only a home that finished THIS session
        // (_state.homed true) is trusted past max_rail.
        float measured_stroke = _motor.getMeasuredStrokeMm();
        if (!_state.homed && measured_stroke > _state.config.max_rail_mm) {
            measured_stroke = _state.config.max_rail_mm;
        }
        slopsync::putF32(s.subspan(33, 4), measured_stroke);
        _hub.publishState(ch::machine_config, s);
    }

    // ---- 0x008A machine-modes — on change --------------------------------
    // M5b. Cheap enough (5 B) to diff locally rather than lean on cfg_gen:
    // these modes route through WebUI::handleCommand, which does NOT bump
    // cfg_gen for all of them, so gating on cfg_gen would silently drop mode
    // changes and leave every client rendering a stale dropdown. Comparing the
    // bytes we last SENT is the honest trigger — it cannot disagree with what
    // the subscribers actually hold.
    {
        // _motor, NOT _arbiter: applySettings writes the MOTOR's blend mode and
        // echoes it, so publishing the arbiter's copy could report a value no
        // write ever produced. One source of truth per field.
        //
        // `blend` (byte 0, "blend_mode_reserved") is RETIRED (item 2, fw
        // 2.1.76) — still read from _motor.getBlendMode() only because that is
        // the smaller diff, not because the value means anything anymore. It
        // is excluded from `mask` below and no client should render it.
        const uint8_t blend  = _motor.getBlendMode();
        const uint8_t smode  = _state.stream_speed_mode;
        const uint8_t oclamp = _state.interp_clamp_overshoot ? 1u : 0u;
        // ALL, ALWAYS — and, as on 0x0081, that is the honest publish rather
        // than a stub. Each takes effect on the NEXT move; none reshapes one
        // already in flight, and none is refused while latched, paused or
        // driven. `transport` was retired rather than gated (SlopSync is the
        // only way in now; the hub listens on WS and BLE by default), and
        // `blend_mode` was retired outright (item 2 — see the field comment on
        // SlopSyncCatalog.h's `blend_mode_reserved`). If a future mode CAN be
        // refused, drop its bit — a UI graying a control the machine would
        // accept is the same lie as one offering a control it would refuse.
        const uint8_t mask = 0x03u;           // bits 0..1 = stream_speed_mode, overshoot_clamp
        std::array<std::byte, 4> buf{};
        std::span<std::byte> s(buf);
        slopsync::putU8(s.subspan(0, 1), blend);
        slopsync::putU8(s.subspan(1, 1), smode);
        slopsync::putU8(s.subspan(2, 1), oclamp);
        slopsync::putU8(s.subspan(3, 1), mask);
        if (!_modesEverSent || buf != _lastModes) {
            _modesEverSent = true;
            _lastModes = buf;
            _hub.publishState(ch::machine_modes, s);
        }
    }


    // ---- 0x008B/0x008C/0x008D slopmotion-* tuning — on change -------------
    // M5c. Same diff-what-we-SENT trigger as 0x008A: these are written from
    // httpTask today and from the hub task via 0x0105, and neither bumps
    // cfg_gen, so gating on cfg_gen would silently strand a client on a stale
    // card. Comparing the bytes subscribers actually hold cannot disagree with
    // them.
    {
        std::array<std::byte, 18> lim{};
        std::span<std::byte> l(lim);
        slopsync::putF32(l.subspan(0, 4),  _state.sm_tune_jmax_ovr);
        slopsync::putF32(l.subspan(4, 4),  _state.sm_tune_vmax_ovr);
        slopsync::putF32(l.subspan(8, 4),  _state.sm_tune_amax_ovr);
        slopsync::putU8 (l.subspan(12, 1), _state.sm_tune_centering ? 1u : 0u);
        slopsync::putF32(l.subspan(13, 4), _state.sm_tune_centering_gain);
        slopsync::putU8 (l.subspan(17, 1), 0x1Fu);   // all 5 always settable
        if (!_smLimEverSent || lim != _lastSmLim) {
            _smLimEverSent = true; _lastSmLim = lim;
            _hub.publishState(ch::sm_limits, l);
        }

        std::array<std::byte, 20> ch_{};
        std::span<std::byte> h(ch_);
        slopsync::putU8 (h.subspan(0, 1),  _state.sm_tune_chase_ff ? 1u : 0u);
        slopsync::putU8 (h.subspan(1, 1),  _state.sm_tune_chase_aff ? 1u : 0u);
        slopsync::putF32(h.subspan(2, 4),  _state.sm_tune_chase_gain);
        slopsync::putF32(h.subspan(6, 4),  _state.sm_tune_chase_look);
        slopsync::putU32(h.subspan(10, 4), _state.sm_tune_dense_us);
        slopsync::putU8 (h.subspan(14, 1), _state.sm_tune_aim_extrap ? 1u : 0u);
        slopsync::putF32(h.subspan(15, 4), _state.sm_tune_handoff_k);
        slopsync::putU8 (h.subspan(19, 1), 0x7Fu);   // all 7 always settable
        if (!_smChaseEverSent || ch_ != _lastSmChase) {
            _smChaseEverSent = true; _lastSmChase = ch_;
            _hub.publishState(ch::sm_chase, h);
        }

        std::array<std::byte, 21> wav{};
        std::span<std::byte> w(wav);
        slopsync::putU8 (w.subspan(0, 1),  _state.sm_tune_curve_policy);
        slopsync::putU8 (w.subspan(1, 1),  _state.sm_tune_infeas_policy);
        slopsync::putF32(w.subspan(2, 4),  _state.sm_tune_infeas_margin);
        slopsync::putF32(w.subspan(6, 4),  _state.sm_tune_smooth_budget);
        slopsync::putF32(w.subspan(10, 4), _state.sm_tune_amp_budget);
        slopsync::putU8 (w.subspan(14, 1), _state.sm_tune_blend_steps);
        slopsync::putU8 (w.subspan(15, 1), _state.sm_tune_reshape_steps);
        slopsync::putU32(w.subspan(16, 4), _state.sm_tune_settle_grace_us);
        slopsync::putU8 (w.subspan(20, 1), 0xFFu);   // all 8 always settable
        if (!_smWavEverSent || wav != _lastSmWav) {
            _smWavEverSent = true; _lastSmWav = wav;
            _hub.publishState(ch::sm_waveform, w);
        }
    }

    // ---- 0x0082 pattern-state — on change, ≥100 ms -----------------------
    if (_patternEngine && (now - _lastPatternMs >= 100)) {
        bool running = _patternEngine->isRunning();
        uint8_t idx = uint8_t(_patternEngine->getPatternIdx());
        float speed = _patternEngine->getSpeedPercent();
        float depth = _patternEngine->getDepthPercent();
        float stroke = _patternEngine->getStrokePercent();
        float sensation = _patternEngine->getSensationPercent();
        // RFC-009 enabled_mask, bit i = the i-th setting-annotated field:
        //   0 running 1 pattern 2 speed 3 depth 4 stroke 5 sensation.
        // GENUINELY dynamic and derived from the delegate's OWN refusals, not
        // from a guess: applyIntent(pattern_cmd) returns ESTOP_ACTIVE while the
        // latch is set and NOT_HOMED before homing, so in either state none of
        // the six is settable and every bit drops together. Gray, never hide.
        // Bit 6 (background_run) is UNCONDITIONALLY set: it is a standing
        // policy control, not a live motion command, so it is never gated by
        // homed/estop the way bits 0-5 are (SlopSyncCatalog.h's enabled_mask
        // comment).
        const uint8_t mask = ((_state.estop_latched || !_state.homed) ? 0x00 : 0x3F) | 0x40;
        bool backgroundRun = _state.pattern_background_run;
        bool changed = running != _patRunning || idx != _patIdx || speed != _patSpeed ||
                       depth != _patDepth || stroke != _patStroke || sensation != _patSensation ||
                       mask != _patMask || backgroundRun != _patBackgroundRun;
        if (changed) {
            _lastPatternMs = now;
            _patRunning = running;
            _patIdx = idx;
            _patSpeed = speed;
            _patDepth = depth;
            _patStroke = stroke;
            _patSensation = sensation;
            _patMask = mask;
            _patBackgroundRun = backgroundRun;
            // Phase D (RFC-045/048): background_run APPENDED at offset 19 (19
            // -> 20 B) — bytes 0..18 keep their offsets, per the catalog's own
            // append-only comment.
            std::array<std::byte, 20> buf{};
            std::span<std::byte> s(buf);
            slopsync::putU8(s.subspan(0, 1), running ? 1 : 0);
            slopsync::putU8(s.subspan(1, 1), idx);
            slopsync::putF32(s.subspan(2, 4), speed);
            slopsync::putF32(s.subspan(6, 4), depth);
            slopsync::putF32(s.subspan(10, 4), stroke);
            slopsync::putF32(s.subspan(14, 4), sensation);
            slopsync::putU8(s.subspan(18, 1), mask);
            slopsync::putU8(s.subspan(19, 1), backgroundRun ? 1 : 0);
            _hub.publishState(ch::pattern_state, s);
        }
    }

    // ---- 0x008E pattern-advanced + 0x008F..0x0094 pattern-adv-mod-* -------
    // Same diff-what-we-SENT trigger as 0x008B/C/D: written from the hub task
    // via 0x0107 and never bumps cfg_gen, so gating on cfg_gen would strand a
    // client on a stale card. Reads PatternEngine directly — the SAME source
    // 0x0082 above reads — never a shadow copy: two sources of truth for the
    // same value is a bug class this project has already been bitten by
    // (blend mode, motor vs arbiter).
    if (_patternEngine) {
        const advpat::Settings& ap = _patternEngine->apSettings();
        // GENUINELY dynamic, narrower than 0x0082's mask: none of these
        // setters is gated on `homed` (see SlopSyncCatalog.h's 0x008E
        // comment), so this tracks e-stop alone.
        const uint8_t mask = _state.estop_latched ? 0x00 : 0xFF;

        std::array<std::byte, 9> base{};
        std::span<std::byte> b(base);
        slopsync::putU8(b.subspan(0, 1), _patternEngine->isAdvancedMode() ? 1u : 0u);
        slopsync::putU8(b.subspan(1, 1), uint8_t(ap.master.value));
        slopsync::putU8(b.subspan(2, 1), uint8_t(ap.max_depth.value));
        slopsync::putU8(b.subspan(3, 1), uint8_t(ap.min_depth.value));
        slopsync::putU8(b.subspan(4, 1), uint8_t(ap.in_speed.value));
        slopsync::putU8(b.subspan(5, 1), uint8_t(ap.out_speed.value));
        slopsync::putU8(b.subspan(6, 1), uint8_t(ap.in_accel.value));
        slopsync::putU8(b.subspan(7, 1), uint8_t(ap.out_accel.value));
        slopsync::putU8(b.subspan(8, 1), mask);
        if (!_apBaseEverSent || base != _lastApBase) {
            _apBaseEverSent = true;
            _lastApBase = base;
            _hub.publishState(ch::pattern_advanced, b);
        }

        // Indexed by advpat::BaseId, NOT by ascending channel id (Phase C4 put
        // the six modifier lanes in member order speedin/out, accelin/out,
        // depth1/2 — see the ch:: namespace comment on these constants).
        static constexpr uint16_t kModChannels[kApBaseCount] = {
            ch::pattern_adv_mod_depth1, ch::pattern_adv_mod_depth2, ch::pattern_adv_mod_speedin,
            ch::pattern_adv_mod_speedout, ch::pattern_adv_mod_accelin, ch::pattern_adv_mod_accelout,
        };
        for (uint8_t id = 0; id < kApBaseCount; ++id) {
            const advpat::BaseControl* bc = ap.byId(id);
            if (!bc) continue;
            const advpat::Modifier& m = bc->modifier;
            std::array<std::byte, 7> mb{};
            std::span<std::byte> ms(mb);
            slopsync::putU8(ms.subspan(0, 1), m.amplitude);
            slopsync::putU8(ms.subspan(1, 1), m.in_step);
            slopsync::putU8(ms.subspan(2, 1), m.in_wait);
            slopsync::putU8(ms.subspan(3, 1), m.out_step);
            slopsync::putU8(ms.subspan(4, 1), m.out_wait);
            slopsync::putU8(ms.subspan(5, 1), m.offset);
            slopsync::putU8(ms.subspan(6, 1), mask);
            if (!_apModEverSent[id] || mb != _lastApMod[id]) {
                _apModEverSent[id] = true;
                _lastApMod[id] = mb;
                _hub.publishState(kModChannels[id], ms);
            }
        }
    }

    // ---- 0x0086 plan-strip — ≥22 ms (≤~45 Hz) ----------------------------
    // The planner's CURRENT SEGMENT. Fed from the interp_* SystemState slots
    // that Core 1's streamSamplerTask fills from slopmotion::Snapshot each
    // tick — the same source the legacy :81 0x04 INTERP frame reads, so this
    // is a re-home of an existing feed and not a new measurement.
    //
    // A torn read across those scalars is visually harmless at 45 Hz (they are
    // aligned 32-bit stores from a single writer) — the same judgement the
    // legacy frame documented, restated because it is the reason this needs no
    // lock on the motion path.
    //
    // Published even while idle: `active` low IS the answer, and a strip that
    // simply stopped arriving would be indistinguishable from a lost
    // subscription. Cheap (18 B, on-change gated below).
    if (now - _lastPlanMs >= 22) {
        uint8_t flags = 0;
        if (_state.interp_active) flags |= 1u << 0;
        if (_state.interp_live_mode) flags |= 1u << 1;
        if (_state.interp_grad_mode) flags |= 1u << 2;
        // Idle strips are near-static; skip republishing an all-quiet frame
        // once it has been sent, so an idle machine costs a subscriber nothing.
        if (_state.interp_active || !_planEverSent) {
            _lastPlanMs = now;
            _planEverSent = true;
            std::array<std::byte, 18> buf{};
            std::span<std::byte> s(buf);
            slopsync::putU8(s.subspan(0, 1), flags);
            slopsync::putU8(s.subspan(1, 1), _state.interp_style);
            slopsync::putU16(s.subspan(2, 2), clampU16(_state.interp_start_pos * 10000.0f));
            slopsync::putU16(s.subspan(4, 2), clampU16(_state.interp_end_pos * 10000.0f));
            slopsync::putU16(s.subspan(6, 2), clampU16(_state.interp_cur_pos * 10000.0f));
            slopsync::putU16(s.subspan(8, 2), uint16_t(clampI16(_state.interp_cur_vel * 1000.0f)));
            slopsync::putU32(s.subspan(10, 4), _state.interp_duration_us);
            slopsync::putU32(s.subspan(14, 4), _state.interp_elapsed_us);
            _hub.publishState(ch::plan_strip, s);
        } else {
            _lastPlanMs = now;
        }
    }

    // ---- 0x0087 power — 2 Hz (catalog allows up to 10) -------------------
    // Only reached on a machine whose driver reports a current sensor; the
    // channel does not exist otherwise (see the constructor's feature probe),
    // so this block is both the publisher AND the proof the gate is real.
    if (_hasPowerChannel && now - _lastPowerMs >= 500) {
        _lastPowerMs = now;
        const size_t n = _hasDieTemp ? 8u : 6u;
        std::array<std::byte, 8> buf{};
        std::span<std::byte> s(buf);
        slopsync::putU16(s.subspan(0, 2), clampU16(_motor.getBusVoltageV() * 1000.0f));
        slopsync::putU16(s.subspan(2, 2), clampU16(fabsf(_motor.getPeakBusCurrentA()) * 1000.0f));
        slopsync::putU16(s.subspan(4, 2), uint16_t(clampI16(_motor.getBusCurrentA() * 1000.0f)));
        if (_hasDieTemp)
            slopsync::putU16(s.subspan(6, 2), uint16_t(clampI16(_motor.getDieTempC() * 10.0f)));
        _hub.publishState(ch::power, s.first(n));
    }

    // ---- 1 Hz block: 0x0083 odometer, 0x0006 hub-status, pairing persist -
    if (now - _lastSlowMs >= 1000) {
        _lastSlowMs = now;

        {  // 0x0083 odometer — 20 B (energy_wh + session_ms appended at M5a)
            std::array<std::byte, 20> buf{};
            std::span<std::byte> s(buf);
            slopsync::putU32(s.subspan(0, 4), _state.stroke_count.load(std::memory_order_relaxed));
            slopsync::putF32(s.subspan(4, 4),
                             _state.session_distance_mm.load(std::memory_order_relaxed) / 1000.0f);
            slopsync::putF32(s.subspan(8, 4), _state.max_speed_mm_s.load(std::memory_order_relaxed));
            // Wh straight from the driver (0.0 with no power monitor — honest;
            // 0x0087's presence is the capability answer, not this number).
            slopsync::putF32(s.subspan(12, 4), _motor.getBusEnergyWh());
            slopsync::putU32(s.subspan(16, 4), uint32_t(now - _state.session_start_ms));
            _hub.publishState(ch::odometer, s);
        }

        {  // 0x0088 slopmotion-diag — 88 B, the /api/slopmotion stats+sync blocks
            // 80 -> 84 at M4d (a ninth per-kind counter, handoff_bounded) and
            // 84 -> 88 with slopmotion 0.8.0 (a tenth, waveform_smoothed).
            // Because that block sits in the MIDDLE of the layout, every field
            // after it shifted by 4 bytes BOTH times. That is a layout CHANGE,
            // not an append — legal here only because the catalog is
            // self-describing and its etag moves with it, so a client re-reads
            // the layout before it decodes a byte, and because 0x0088 is a
            // pre-v1.0 device channel under the RFC-queue's standing "breaking
            // is allowed before the v1.0 tag" ruling. After that tag this same
            // growth would need a new channel id.
            //
            // The offsets below are NOT independent constants: the per-kind
            // loop ends at 14 + SM_ANOM_KINDS*4 and everything after starts
            // there. Byte map (SM_ANOM_KINDS = 10):
            //   0 plans, 4 failures, 8 anomalies, 12 mode, 13 plan_kind,
            //   14..53 the ten u32 per-kind counters,
            //   54 plan_us_last, 58 plan_us_max, 62 plan_us_avg,
            //   66 sync_bundles, 70 sync_samples, 74 sync_enqueued,
            //   78 sync_dropped, 82 sync_seg_bundles, 86 reset_gen (u16) = 88.
            std::array<std::byte, 88> buf{};
            std::span<std::byte> s(buf);
            slopsync::putU32(s.subspan(0, 4), _state.sm_plans);
            slopsync::putU32(s.subspan(4, 4), _state.sm_failures);
            slopsync::putU32(s.subspan(8, 4), _state.sm_anomalies);
            slopsync::putU8(s.subspan(12, 1), _state.sm_mode);
            slopsync::putU8(s.subspan(13, 1), _state.sm_plan_kind);
            // Per-kind breakdown, indexed by slopmotion::AnomalyType — the
            // catalog declares exactly SM_ANOM_KINDS named fields in this
            // order, so the loop bound IS the layout and a new engine kind
            // moves both together or neither.
            for (uint8_t k = 0; k < SystemState::SM_ANOM_KINDS; ++k)
                slopsync::putU32(s.subspan(14 + size_t(k) * 4, 4), _state.sm_anom_kind[k]);
            slopsync::putU32(s.subspan(54, 4), _state.sm_plan_us_last);
            slopsync::putU32(s.subspan(58, 4), _state.sm_plan_us_max);
            slopsync::putF32(s.subspan(62, 4), _state.sm_plan_us_avg);
            slopsync::putU32(s.subspan(66, 4), _state.sm_sync_bundles);
            slopsync::putU32(s.subspan(70, 4), _state.sm_sync_samples);
            slopsync::putU32(s.subspan(74, 4), _state.sm_sync_enqueued);
            slopsync::putU32(s.subspan(78, 4), _state.sm_sync_dropped);
            slopsync::putU32(s.subspan(82, 4), _state.sm_sync_seg_bundles);
            slopsync::putU16(s.subspan(86, 2), _state.sm_reset_gen);
            _hub.publishState(ch::motion_diag, s);
        }

        {  // 0x0006 hub-status — 14 B (log_dropped appended at M5b)
            std::array<std::byte, 14> buf{};
            std::span<std::byte> s(buf);
            slopsync::putU32(s.subspan(0, 4), uint32_t(ESP.getFreeHeap()));
            slopsync::putU32(s.subspan(4, 4), uint32_t(now / 1000u));
            slopsync::putU8(s.subspan(8, 1), uint8_t(int8_t(WiFi.RSSI())));
            slopsync::putU8(s.subspan(9, 1), uint8_t(_hub.sessionCount()));
            // §9.4: never-silent. ALL THREE loss points in the chain summed —
            // the sloplog core ring (where a flood is shed first, severity-aware),
            // the httpTask->hub bridge ring, and the hub's own replay ring. A
            // client watching this number sees the whole pipeline, not one stage.
            slopsync::putU32(s.subspan(10, 4),
                             _hub.logDropped() +
                                 _state.sloplog_bridge_dropped.load(std::memory_order_relaxed) +
                                 sloplog::logger().totalLost());
            _hub.publishState(slopsync::channels::hub_status, s);
        }

        persistPairingIfChanged();
        pumpPresencePairingWindow(now);  // RFC-027(c): streak reset + SlopGlow mirror
        pumpEndpointAndRadios(now);      // RFC-046 (Phase E): WELCOME endpoint + BLE/UDP radios
        persistPresetsIfChanged();       // RFC-021 pattern-preset store, write-on-change
        publishPresetRoster();           // 0x0096, on-change (generation-diffed internally)
        // M5c: coalesced tuning persist. 0x0105 only FLAGS a change; the write
        // happens here, at most once a second. NVS is flash — a slider dragged
        // at the channel's 5 Hz would otherwise be five erase/write cycles per
        // second for a value the operator is still moving.
        if (_delegate._smTuneDirty) {
            _delegate._smTuneDirty = false;
            // Reuse the EXISTING persist path rather than calling
            // ConfigStore::save directly: WS_OP_SAVE already owns the OTA
            // deferral and the range-mapper binding, and one save path means
            // one place where "what actually gets written" is decided.
            JsonDocument in, out;
            _webui.handleCommand(WS_OP_SAVE, in, out);
            SLOGI("slopsync", "slopmotion tuning persisted to NVS");
        }
        // Item 1 (fw 2.1.76): max_rail is now a savable setting. Same
        // coalesced-persist contract as _smTuneDirty above — 0x0101 key 8
        // only flags the change, the write happens here at most once a
        // second.
        if (_delegate._maxRailDirty) {
            _delegate._maxRailDirty = false;
            JsonDocument in, out;
            _webui.handleCommand(WS_OP_SAVE, in, out);
            SLOGI("slopsync", "max_rail persisted to NVS");
        }
        // RFC-045/048 (Phase D): source.background_run, same coalesced-persist
        // contract as _maxRailDirty above.
        if (_delegate._patternBackgroundRunDirty) {
            _delegate._patternBackgroundRunDirty = false;
            JsonDocument in, out;
            _webui.handleCommand(WS_OP_SAVE, in, out);
            SLOGI("slopsync", "pattern background_run persisted to NVS");
        }
        // Item 3 (fw 2.1.76): persist the freshly-measured stroke on EVERY
        // successful home, not just whenever the operator happens to hit
        // Save next. motorTask (Core 1) raises this flag the instant a
        // homing cycle completes with _state.homed true; NVS writes are
        // flash I/O and must never run on the real-time core, so the actual
        // ConfigStore::save() happens here, on the hub's Core-0 task.
        if (_state.stroke_measured_pending) {
            _state.stroke_measured_pending = false;
            JsonDocument in, out;
            _webui.handleCommand(WS_OP_SAVE, in, out);
            SLOGI("slopsync", "measured stroke persisted to NVS after home");
        }
    }
}

// ============================================================================
// 0x0089 motion-anomaly — the Core-1 anomaly ring, turned into EVENTs
// ============================================================================
//
// Runs on the SlopSyncHub task (Core 0) like every other publisher here. The
// engine's own anomaly ring lives on Core 1 and MUST be drained there (that is
// where slopmotion runs), so main.cpp's sampler forwards each edge into the
// SPSC ring in SystemState and this is the consumer half.
//
// BOUNDED PER TICK. A pathological replan burst could otherwise turn one 5 ms
// tick into dozens of encodes + fan-outs on the task that also pumps the WS
// port and the deadman; the ring's own drop counter (never silent) covers the
// overflow case, and 8 per tick is 1600/s of headroom.
void SlopSyncHubService::publishAnomalies() {
    if (_catalog.find(ch::motion_anomaly) == nullptr) return;

    SystemState::SmAnomalyRec rec;
    for (int i = 0; i < 8 && _state.smAnomalyPop(rec); ++i) {
        slopsync::EventMsg ev{};
        ev.channel_id = ch::motion_anomaly;
        ev.timestamp = _clock.nowMs();
        // The protocol's own discriminator (§9.4) — what a client switches on.
        ev.event_kind = rec.kind;
        ev.has_body = true;
        ev.body_count = 0;
        // ...and the SAME value again in the body, where the catalog's option
        // labels can name it. The catalog has no vocabulary for labeling
        // event kinds; `options` on a schema field is the one registered
        // mechanism that turns a number into a word, so this is what lets a
        // generic client print "waveform_scaled" instead of "6".
        ev.body[ev.body_count++] = {anom_body::kind, IntentValue::ofU64(rec.kind)};
        ev.body[ev.body_count++] = {anom_body::seq, IntentValue::ofU64(rec.seq)};
        ev.body[ev.body_count++] = {anom_body::target, IntentValue::ofF32(rec.target)};
        ev.body[ev.body_count++] = {anom_body::detail, IntentValue::ofF32(rec.detail)};
        ev.body[ev.body_count++] = {anom_body::t_us, IntentValue::ofU64(rec.t_us)};

        std::array<std::byte, 96> buf{};
        size_t n = slopsync::encodeEvent(ev, std::span<std::byte>(buf));
        if (n > 0) _hub.publishEvent(ch::motion_anomaly, std::span<const std::byte>(buf.data(), n));
    }
}

// ============================================================================
// RFC-027(c) push-to-pair — the boot-counter gesture (namespace "slopsync")
// ============================================================================
//
// hub.hpp is explicit that the library provides the window, never the
// gesture: "the application's job and only the application's". This is that
// job. Registry `pairing_modes` bit2 (docs/slopsync/registry/registry.yaml)
// is the normative text: N=3 consecutive boots with uptime <10 s opens the
// window, NVS counter only — it cannot collide with a live session because
// any power loss already stops motion and forces re-home, and FACTORY RESET
// MUST STAY A HARDER GESTURE than this one (it is: factory reset is not
// implemented via NVS counter at all, so there is no shared mechanism to
// under-shoot).
namespace {
constexpr const char* kQuickBootKey = "qboots";  // NVS key, <=15 chars
constexpr uint8_t kQuickBootThreshold = 3;
constexpr uint32_t kQuickBootSurviveMs = 10000;  // "uptime < ~10 s" per the registry
}  // namespace

// Runs ONCE from init(), before the hub task exists — this is boot-sequence
// work, exactly the CLAUDE.md §2 exception that permits a blocking NVS open
// here (never in a runtime loop).
void SlopSyncHubService::checkQuickBootPairingGesture() {
    Preferences prefs;
    if (!prefs.begin("slopsync", false)) return;

    uint8_t count = prefs.getUChar(kQuickBootKey, 0);
    // A streak that was already AT threshold on read means a previous boot's
    // window-open write raced a reboot before pumpPresencePairingWindow() could
    // clear it back to 0 (belt-and-braces below already clears it inline, but
    // this keeps the counter self-healing even if that write was lost).
    count = (count >= kQuickBootThreshold) ? uint8_t(1) : uint8_t(count + 1);
    prefs.putUChar(kQuickBootKey, count);
    SLOGI("slopsync", "pairing: quick-boot count %u/%u", unsigned(count), unsigned(kQuickBootThreshold));

    if (count >= kQuickBootThreshold) {
        prefs.putUChar(kQuickBootKey, 0);  // consume the gesture immediately, not on next boot
        _hub.openPresenceWindow();
        _presenceGlowOn = true;
        slopglowEngine().set(slopglow::GlowState::Pairing, true);
        SLOGI("slopsync", "pairing: PRESENCE WINDOW OPEN (3 quick power-cycles) — pair within %u s",
              unsigned(slopsync::limits::pairing_window_default_s));
    }
    prefs.end();
}

// Runs every 1 Hz tick on the hub task (see publishTelemetry's slow block).
void SlopSyncHubService::pumpPresencePairingWindow(uint32_t nowMs) {
    // The streak only means anything for boots that die fast. Once THIS boot
    // has visibly survived past the gesture's own window, the next reboot —
    // fast or not — starts counting fresh rather than silently continuing a
    // stale streak from an unrelated earlier power-cycle. One-shot: nothing
    // past the first successful reset needs to touch flash again.
    if (!_qbootResetDone && nowMs >= kQuickBootSurviveMs) {
        _qbootResetDone = true;
        Preferences prefs;
        if (prefs.begin("slopsync", false)) {
            if (prefs.getUChar(kQuickBootKey, 0) != 0) {
                prefs.putUChar(kQuickBootKey, 0);
                SLOGI("slopsync", "pairing: quick-boot streak reset (uptime > 10 s)");
            }
            prefs.end();
        }
    }

    // The presence window auto-expires INSIDE the library (120 s, same
    // duration as the PIN window) — nothing calls closePresenceWindow() for
    // that edge, so SlopGlow only learns about it if something polls. 1 Hz is
    // plenty; the window is open for whole seconds at minimum.
    const bool open = _hub.presenceWindowOpen();
    if (open != _presenceGlowOn) {
        _presenceGlowOn = open;
        slopglowEngine().set(slopglow::GlowState::Pairing, open);
        if (!open) SLOGI("slopsync", "pairing: presence window closed");
    }
}

// ============================================================================
// RFC-046 (Phase E) — the hub's own endpoint (WELCOME keys 46/47) and the
// live-changing half of the BLE-advertising / UDP-discovery flags byte.
// Runs at 1 Hz on the hub task; every downstream call here is diff-gated
// against its own last-published state, so a quiet second (nothing changed)
// costs a WiFi.status() call and a couple of bool compares — no radio touch.
// ============================================================================
void SlopSyncHubService::pumpEndpointAndRadios(uint32_t /*nowMs*/) {
    const bool wifiUp = (WiFi.status() == WL_CONNECTED);

    // §6.3: "0 means absent" for BOTH keys — Hub::setEndpoint already omits
    // them from the wire at 0 (see welcome.hpp), so WiFi being down simply
    // means every session's next WELCOME-shaped message stops offering an
    // endpoint, which is the honest answer (Ground Truth doctrine).
    uint16_t wsPort = 0;
    uint32_t ipv4 = 0;
    if (wifiUp) {
        wsPort = uint16_t(SLOPSYNC_WS_PORT);
        // Packed big-endian per registry.yaml's own worked example
        // (192.168.1.229 = 0xC0A801E5) — NOT IPAddress's raw in-memory byte
        // order, which this codebase does not rely on matching.
        const IPAddress ip = WiFi.localIP();
        ipv4 = (uint32_t(ip[0]) << 24) | (uint32_t(ip[1]) << 16) | (uint32_t(ip[2]) << 8) | uint32_t(ip[3]);
    }
    _hub.setEndpoint(wsPort, ipv4);

    // §13.4/§13.6/§13.8: the same "is an association window open right now"
    // signal feeds the BLE advertising flag, the UDP DISCOVER_REPLY flag, and
    // (already) WELCOME's trust.pairing_modes — one source of truth.
    const bool pairingOpen = _hub.pairingWindowOpen();

#if defined(BLE_ENABLED)
    _blePort.updateAdvertising(pairingOpen, wifiUp);
#endif
    _udpDiscovery.setPairingWindowOpen(pairingOpen);
    _udpDiscovery.setWsAvailable(wifiUp);
}

// ============================================================================
// Pairing window + NVS persistence (namespace "slopsync")
// ============================================================================

void SlopSyncHubService::openPairing(const char* pin) {
    if (!pin) return;
    std::strncpy(_pairPin, pin, sizeof(_pairPin) - 1);
    _pairPin[sizeof(_pairPin) - 1] = '\0';
    size_t len = std::strlen(_pairPin);
    _hub.openPairingWindow(std::span<const char>(_pairPin, len));
    slopglowEngine().set(slopglow::GlowState::Pairing, true);
    SLOGI("slopsync", "pairing window OPEN (pin len %u)", unsigned(len));
}

void SlopSyncHubService::closePairing() {
    _hub.closePairingWindow();
    slopglowEngine().set(slopglow::GlowState::Pairing, false);
    // A ceremony just ended — persist any freshly issued (or re-issued) tokens.
    savePairing();
    SLOGI("slopsync", "pairing window closed");
}

// ---- RFC-029 item 3: the trust ledger, persisted ---------------------------
// ONE CBOR blob, all-or-nothing, under limits::trust_ledger_max_bytes (1900 —
// one NVS page). The library owns the format (PairingManager::encodeLedger /
// decodeLedger); this side owns only the flash.
//
// The all-or-nothing property is not a nicety: decodeLedger stages the whole
// ledger and swaps it in only if every entry parsed, so a truncated or corrupt
// page leaves the hub with an EMPTY trust list rather than an arbitrary prefix
// of one. A half-applied ledger is an authorization decision nobody made.
namespace {
constexpr const char* kLedgerKey = "ledger";
constexpr const char* kLegacyTokensKey = "tokens";  // pre-M5b 25-byte-per-entry blob
}  // namespace

void SlopSyncHubService::loadPairing() {
    Preferences prefs;
    // Open read-WRITE: a read-only begin() on a namespace that has never been
    // written fails NOT_FOUND and the Preferences lib logs a scary E-line on
    // serial right as the first client connects — operators read it as a boot
    // blocker (field-reported 2026-07-24). Read-write creates the (empty)
    // namespace on first boot; every later open is quiet. Still honors the
    // OTA gate pattern: this runs at service init only, never during a flash.
    if (!prefs.begin("slopsync", false)) return;

    size_t n = prefs.getBytesLength(kLedgerKey);
    if (n > 0 && n <= size_t(slopsync::limits::trust_ledger_max_bytes)) {
        prefs.getBytes(kLedgerKey, _ledgerBlob, n);
        if (_hub.pairing().decodeLedger(
                std::span<const std::byte>(reinterpret_cast<const std::byte*>(_ledgerBlob), n))) {
            _pairCountCached = _hub.pairing().entryCount();
            // decodeLedger touch()es the ledger (it moved), which would make the
            // very next 1 Hz tick rewrite the identical bytes back to flash.
            // Loading is not a change.
            _hub.pairing().clearDirty();
            SLOGI("slopsync", "trust ledger loaded: %u entr%s, %u B", unsigned(_pairCountCached),
                  _pairCountCached == 1 ? "y" : "ies", unsigned(n));
        } else {
            SLOGE("slopsync", "trust ledger REJECTED (%u B corrupt/truncated) — starting empty", unsigned(n));
        }
        prefs.end();
        return;
    }

    // ---- One-time migration from the pre-M5b hand-rolled blob ---------------
    // 25 bytes per entry: instance_id(8) + token(16) + role(1). Everything the
    // richer ledger adds (name, kind, version, state, seen timestamps) simply
    // starts unknown, which is honest — the old format never held it.
    n = prefs.getBytesLength(kLegacyTokensKey);
    constexpr size_t kEntryBytes = 8 + 16 + 1;
    constexpr size_t kMax = slopsync::PairingManager::kMaxPaired;
    if (n > 0 && n % kEntryBytes == 0 && n <= kMax * kEntryBytes) {
        uint8_t blob[kMax * kEntryBytes];
        prefs.getBytes(kLegacyTokensKey, blob, n);
        size_t cnt = n / kEntryBytes;
        for (size_t k = 0; k < cnt; ++k) {
            slopsync::PairingManager::PairedEntry e{};
            std::memcpy(e.instance_id.data(), blob + k * kEntryBytes, 8);
            std::memcpy(e.token.data(), blob + k * kEntryBytes + 8, 16);
            uint8_t r = blob[k * kEntryBytes + 24];
            e.role = (r > uint8_t(slopsync::AccessLevel::configure)) ? slopsync::AccessLevel::control
                                                                      : slopsync::AccessLevel(r);
            e.used = true;
            _hub.pairing().importEntry(e);
        }
        _pairCountCached = cnt;
        SLOGI("slopsync", "migrated %u legacy paired token(s) to the trust ledger", unsigned(cnt));
        prefs.end();
        // Leave the ledger DIRTY on purpose: the next savePairing() writes the
        // new format and drops the legacy key. Doing the write here would mean
        // doing it before the OTA gate is even readable.
        return;
    }
    prefs.end();
}

void SlopSyncHubService::savePairing() {
    // Respect the OTA NVS gate: a flash-cache access during an OTA write window
    // can reset the chip (same reason ConfigStore::save defers under ota_active).
    // Deferring is free — dirty() stays set and the next 1 Hz tick retries.
    if (_state.ota_active.load(std::memory_order_relaxed)) return;

    auto& pm = _hub.pairing();
    const size_t n = pm.encodeLedger(
        std::span<std::byte>(reinterpret_cast<std::byte*>(_ledgerBlob), sizeof(_ledgerBlob)));
    if (n == 0 || n > sizeof(_ledgerBlob)) {
        // encodeLedger returns 0 when the ledger does not fit. Registry sizing
        // (paired_devices_max 8 x per_item_max 237) says it cannot, so this is a
        // shout-not-silently-truncate guard, never an expected branch.
        SLOGE("slopsync", "trust ledger did NOT encode (%u entries) — NOT persisted",
              unsigned(pm.entryCount()));
        return;
    }

    Preferences prefs;
    if (prefs.begin("slopsync", false)) {
        prefs.putBytes(kLedgerKey, _ledgerBlob, n);
        // Migration cleanup: once the ledger is on flash the legacy key is dead
        // weight and, worse, a second source of truth.
        if (prefs.getBytesLength(kLegacyTokensKey) > 0) prefs.remove(kLegacyTokensKey);
        prefs.end();
        pm.clearDirty();
        _pairCountCached = pm.entryCount();
        SLOGI("slopsync", "trust ledger persisted: %u entr%s, %u B", unsigned(_pairCountCached),
              _pairCountCached == 1 ? "y" : "ies", unsigned(n));
    }
}

void SlopSyncHubService::persistPairingIfChanged() {
    // WRITE ONLY ON CHANGE, and `dirty()` is the library's own answer to what a
    // change is: a new pairing, a re-issued token, a revocation, a tripwire
    // state transition, a refreshed last_seen. The old entryCount() heuristic
    // missed every one of those that did not move the COUNT.
    if (_hub.pairing().dirty()) savePairing();
}

// ============================================================================
// RFC-021 pattern-preset store — NVS persistence (namespace "slopsync") + the
// one-time legacy migration off the retired /api/pattern/presets handler.
// ============================================================================
namespace {
constexpr const char* kPresetKey = "presets";  // NVS key, <=15 chars, namespace "slopsync"
}  // namespace

void SlopSyncHubService::loadPresets() {
    // Same read-WRITE-to-avoid-a-scary-first-boot-E-line reasoning as
    // loadPairing() — see that function's comment.
    Preferences prefs;
    if (!prefs.begin("slopsync", false)) return;

    size_t n = prefs.getBytesLength(kPresetKey);
    if (n == PatternPresetStore::kEncodedBytes) {
        prefs.getBytes(kPresetKey, _presetBlob, n);
        if (_presets.decode(std::span<const std::byte>(reinterpret_cast<const std::byte*>(_presetBlob), n))) {
            _presets.clearDirty();  // loading is not a change
            SLOGI("slopsync", "pattern presets loaded: %u/%u slots", unsigned(_presets.count()),
                  unsigned(PatternPresetStore::kCapacity));
        } else {
            SLOGE("slopsync", "pattern preset blob REJECTED (%u B) — starting empty", unsigned(n));
        }
        prefs.end();
        return;
    }
    if (n != 0) {
        // Some OTHER size lives under this key — not a format this build
        // recognizes (the key is new at M5, so this should never fire in
        // practice). Leave it alone rather than guess at a decode.
        SLOGW("slopsync", "pattern preset blob is %u B, expected %u — ignoring", unsigned(n),
              unsigned(PatternPresetStore::kEncodedBytes));
        prefs.end();
        return;
    }
    prefs.end();

    // ---- One-time migration from the pre-M5 HTTP handler's NVS format ------
    // "advpreset"/"list": a JSON array of {name, def:{in_speed, out_speed,
    // in_accel, out_accel, mods:[{ctrl, amplitude, in_step, in_wait, out_step,
    // out_wait, offset}]}} — src/ui/WebUI.cpp's own retired format. Read-only
    // (this store never writes the legacy key); best-effort, so a preset that
    // doesn't fit (more than kCapacity, or a malformed `def`) is skipped, never
    // silently eaten — the summary log line says how many made it across.
    Preferences legacy;
    if (!legacy.begin("advpreset", true)) return;  // read-only; namespace may not exist yet
    String stored = legacy.getString("list", "[]");
    legacy.end();

    JsonDocument listDoc;
    if (deserializeJson(listDoc, stored) || !listDoc.is<JsonArray>()) return;
    JsonArray arr = listDoc.as<JsonArray>();
    if (arr.size() == 0) return;

    auto clampU8 = [](int v) -> uint8_t { return uint8_t(v < 0 ? 0 : (v > 100 ? 100 : v)); };
    uint8_t migrated = 0;
    for (JsonObject e : arr) {
        if (migrated >= PatternPresetStore::kCapacity) break;
        const char* nm = e["name"] | "";
        if (!nm || !*nm || !e["def"].is<JsonObject>()) continue;
        JsonObject def = e["def"];

        uint8_t payload[PatternPresetStore::kPayloadBytes];
        payload[0] = clampU8(def["in_speed"]  | 100);
        payload[1] = clampU8(def["out_speed"] | 100);
        payload[2] = clampU8(def["in_accel"]  | 40);
        payload[3] = clampU8(def["out_accel"] | 40);
        // advpat::Settings' own ctor defaults (AdvancedPattern.h) for any
        // modifier a legacy entry never mentions (amplitude 100 = off).
        for (uint8_t id = 0; id < advpat::BASE_COUNT; ++id) {
            const uint8_t base = uint8_t(4 + id * 6);
            payload[base + 0] = 100; payload[base + 1] = 1; payload[base + 2] = 0;
            payload[base + 3] = 1;   payload[base + 4] = 0; payload[base + 5] = 0;
        }
        if (def["mods"].is<JsonArray>()) {
            for (JsonObject m : def["mods"].as<JsonArray>()) {
                int ctrl = m["ctrl"] | -1;
                if (ctrl < 0 || ctrl >= int(advpat::BASE_COUNT)) continue;
                const uint8_t base = uint8_t(4 + ctrl * 6);
                payload[base + 0] = clampU8(m["amplitude"] | 100);
                payload[base + 1] = clampU8(m["in_step"]   | 1);
                payload[base + 2] = clampU8(m["in_wait"]   | 0);
                payload[base + 3] = clampU8(m["out_step"]  | 1);
                payload[base + 4] = clampU8(m["out_wait"]  | 0);
                payload[base + 5] = clampU8(m["offset"]    | 0);
            }
        }
        if (_presets.importRaw(migrated, nm, payload)) ++migrated;
    }
    if (migrated > 0) {
        _presets.markDirty();  // the next savePresets() writes the new format
        SLOGI("slopsync", "migrated %u/%u legacy pattern preset(s) to the RFC-021 store",
              unsigned(migrated), unsigned(arr.size()));
    }
}

void SlopSyncHubService::savePresets() {
    // Same OTA NVS gate as savePairing() — a flash-cache access during an OTA
    // write window can reset the chip.
    if (_state.ota_active.load(std::memory_order_relaxed)) return;

    const size_t n = _presets.encode(
        std::span<std::byte>(reinterpret_cast<std::byte*>(_presetBlob), sizeof(_presetBlob)));
    if (n == 0) {
        SLOGE("slopsync", "pattern preset store did NOT encode — NOT persisted");
        return;
    }
    Preferences prefs;
    if (prefs.begin("slopsync", false)) {
        prefs.putBytes(kPresetKey, _presetBlob, n);
        prefs.end();
        _presets.clearDirty();
        SLOGI("slopsync", "pattern presets persisted: %u/%u slots, %u B", unsigned(_presets.count()),
              unsigned(PatternPresetStore::kCapacity), unsigned(n));
    }
}

void SlopSyncHubService::persistPresetsIfChanged() {
    if (_presets.dirty()) savePresets();
}

// 0x0096 roster — on-change only, keyed off the store's own generation. BARE
// {generation,count,capacity}, same shape as 0x000D paired-devices-roster —
// see SlopSyncCatalog.h's comment on the 0x0096 entry for why an embedded
// per-slot name preview was cut (Catalog32's layout-field pool had only 11
// free slots; 17 were needed). Enumerate names via BLOB_REQ per slot.
void SlopSyncHubService::publishPresetRoster() {
    if (_catalog.find(ch::pattern_presets_roster) == nullptr) return;
    if (_presetRosterEverSent && _presets.generation() == _lastPresetGen) return;
    _lastPresetGen = _presets.generation();
    _presetRosterEverSent = true;

    std::array<std::byte, 4> buf{};
    std::span<std::byte> s(buf);
    slopsync::putU16(s.subspan(0, 2), _presets.generation());
    slopsync::putU8(s.subspan(2, 1), _presets.count());
    slopsync::putU8(s.subspan(3, 1), PatternPresetStore::kCapacity);
    _hub.publishState(ch::pattern_presets_roster, s);
}

// ============================================================================
// OTA park/revive
// ============================================================================

void SlopSyncHubService::suspendTask() {
    if (_task) vTaskSuspend(_task);
    // The signing task goes down too, and for a HARDER reason than the hub
    // task's: mid-sign it is deep inside mbedtls reading this object's MPI
    // state, and this object lives in PSRAM. A flash-write window disables the
    // cache, which makes PSRAM unreachable — a suspended task cannot be caught
    // there. (It also checks ota_active before starting a job; that guard
    // handles the arrive-during-OTA case, this one handles already-running.)
    if (_signTask) vTaskSuspend(_signTask);
}

void SlopSyncHubService::resumeTask() {
    if (_task) vTaskResume(_task);
    if (_signTask) vTaskResume(_signTask);
}

}  // namespace slopdrive
