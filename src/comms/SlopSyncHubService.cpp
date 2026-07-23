#include "SlopSyncHubService.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>

#include <array>
#include <cmath>
#include <cstring>

#include "MotionArbiter.h"
#include "PatternEngine.h"
#include "SlopGlowBoard.h"
#include "SystemState.h"
#include "UiProtocol.h"
#include "WebUI.h"
#include "sloplog/sloplog.h"
#include "slopsync/util/byte_io.hpp"

namespace slopdrive {

// ============================================================================
// Small helpers
// ============================================================================

namespace {

using slopsync::IntentValue;
using slopsync::IntentValueField;
using slopsync::IntentValueMap;
using slopsync::NackCode;

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
    (void)instance_id;
    (void)token;
    (void)hasToken;
    // TODO(pairing-enforcement): return controller unconditionally for now, to
    // match the LAN-trust posture of the existing zero-auth WS UI. The
    // PairingManager plumbing (persisted tokens, pairing window) is fully wired
    // and compiling; flipping this to consult the pairing store (viewer by
    // default, controller only for a valid token) is the whole enforcement
    // step and MUST be a deliberate change, not a silent default.
    return slopsync::AccessLevel::controller;
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
            if (f1) in["range_min"] = fieldF32(f1, 0.0f);
            if (f2) in["range_max"] = fieldF32(f2, 0.0f);
            // applySettings gates the user/input branches on is<uint32_t>(), so
            // these MUST be integer-typed in the doc, not floats.
            if (f3) in["user_max_speed"] = uint32_t(lroundf(fieldF32(f3, 0.0f)));
            if (f4) in["user_max_accel"] = uint32_t(lroundf(fieldF32(f4, 0.0f)));
            if (f5) in["input_max_speed"] = uint32_t(lroundf(fieldF32(f5, 0.0f)));
            if (f6) in["input_max_accel"] = uint32_t(lroundf(fieldF32(f6, 0.0f)));
            // Bump the protocol cfg_gen (that is what cfgChanged means) but do
            // NOT hammer NVS on a potentially-streamed intent — no_persist keeps
            // flash safe; a future explicit save channel handles durability.
            in["no_persist"] = true;
            if (!_webui.handleCommand(WS_OP_SET_WINDOW, in, out)) {
                return Ret::err(NackCode::INVALID_VALUE);  // e.g. min >= max
            }
            cfgChanged = true;
            uint32_t n = 0;
            if (f1) applied.fields[n++] = {1, IntentValue::ofF32(out["range_min"] | 0.0f)};
            if (f2) applied.fields[n++] = {2, IntentValue::ofF32(out["range_max"] | 0.0f)};
            if (f3) applied.fields[n++] = {3, IntentValue::ofF32(float(out["user_max_speed"] | 0u))};
            if (f4) applied.fields[n++] = {4, IntentValue::ofF32(float(out["user_max_accel"] | 0u))};
            if (f5) applied.fields[n++] = {5, IntentValue::ofF32(float(out["input_max_speed"] | 0u))};
            if (f6) applied.fields[n++] = {6, IntentValue::ofF32(float(out["input_max_accel"] | 0u))};
            applied.count = n;
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
            if (f1) in["running"] = fieldBool(f1, false);
            if (f2) in["pattern"] = int(fieldU64(f2, 0));
            if (f3) in["speed"] = fieldF32(f3, 0.0f);
            if (f4) in["depth"] = fieldF32(f4, 0.0f);
            if (f5) in["stroke"] = fieldF32(f5, 0.0f);
            if (f6) in["sensation"] = fieldF32(f6, 0.0f);
            if (!_webui.handleCommand(WS_OP_GEN_CFG, in, out)) {
                return Ret::err(_state.homed ? NackCode::INVALID_VALUE : NackCode::NOT_HOMED);
            }
            cfgChanged = false;  // session-volatile
            // applyPattern echoes running(bool), pattern/speed/depth/stroke/
            // sensation as ints — re-widen to the schema's f32.
            uint32_t n = 0;
            if (f1) applied.fields[n++] = {1, IntentValue::ofBool(out["running"] | false)};
            if (f2) applied.fields[n++] = {2, IntentValue::ofU64(uint64_t(int(out["pattern"] | 0)))};
            if (f3) applied.fields[n++] = {3, IntentValue::ofF32(float(int(out["speed"] | 0)))};
            if (f4) applied.fields[n++] = {4, IntentValue::ofF32(float(int(out["depth"] | 0)))};
            if (f5) applied.fields[n++] = {5, IntentValue::ofF32(float(int(out["stroke"] | 0)))};
            if (f6) applied.fields[n++] = {6, IntentValue::ofF32(float(int(out["sensation"] | 0)))};
            applied.count = n;
            return Ret::ok(applied);
        }

        // ---- 0x0103 home → WS_OP_HOME ------------------------------------
        case ch::home: {
            if (_state.estop_latched) return Ret::err(NackCode::ESTOP_ACTIVE);
            uint64_t op = fieldU64(findField(requested, 1), 0);
            if (op != 1) return Ret::err(NackCode::UNSUPPORTED_OP);
            _webui.handleCommand(WS_OP_HOME, in, out);  // always accepts
            applied.count = 1;
            applied.fields[0] = {1, IntentValue::ofU64(1)};
            return Ret::ok(applied);
        }

        // ---- 0x0005 safety-intents (stop/hold/pause/resume) ---------------
        // estop_clear (op 1) is hub-handled and never reaches the delegate.
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
                default:
                    return Ret::err(NackCode::UNSUPPORTED_OP);
            }
            applied.count = 1;
            applied.fields[0] = {1, IntentValue::ofU64(op)};
            return Ret::ok(applied);
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
    return std::nullopt;
}

slopsync::SourceLossPolicy SlopDriveHubDelegate::sourcePolicy(uint8_t source_id) {
    // Pattern is hub-autonomous — a controller dropping off must NOT kill a
    // running pattern (user-locked "pattern continues" doctrine). Live control
    // (move) is initiator-bound: silence stops motion.
    if (source_id == uint8_t(MotionSource::PATTERN)) return slopsync::SourceLossPolicy::Continue;
    return slopsync::SourceLossPolicy::Stop;
}

void SlopDriveHubDelegate::onDeadmanStop(uint8_t source_id) {
    (void)source_id;
    // §11.3: STOP MOTION NOW, via the same hard-stop path WS_OP_HALT uses.
    JsonDocument in;
    JsonDocument out;
    _webui.handleCommand(WS_OP_HALT, in, out);
    SLOGW("slopsync", "deadman stop on source %u — motion halted", source_id);
}

void SlopDriveHubDelegate::onSessionJoined(uint32_t session_id) {
    SLOGI("slopsync", "session %08x joined", session_id);
}

void SlopDriveHubDelegate::onSessionLeft(uint32_t session_id) {
    SLOGI("slopsync", "session %08x left", session_id);
}

// ============================================================================
// SlopSyncHubService
// ============================================================================

SlopSyncHubService::SlopSyncHubService(SystemState& state, WebUI& webui, MotionArbiter& arbiter)
    : _state(state),
      _webui(webui),
      _arbiter(arbiter),
      _catalog(buildSlopDriveCatalog()),
      _delegate(state, webui, arbiter),
      _hub(_catalog, _clock, _rng, _delegate),
      _port() {}

void SlopSyncHubService::init() {
    loadPairing();
    _port.begin(&_hub);

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
        _hub.update(_clock.nowUs());  // pump every session: frames, pacing, deadman
        syncSafety();
        publishTelemetry();
    }
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
        // Firmware left the e-stopped state (e.g. a HOME cycle cleared it) while
        // the hub still holds the latch: clear it. clearEstop() consults
        // canClearEstop() and is a no-op if motion isn't actually stopped yet.
        _hub.clearEstop();
    }
}

void SlopSyncHubService::publishTelemetry() {
    uint32_t now = millis();

    // ---- 0x0080 motion — ≥16 ms (≤~60 Hz) --------------------------------
    if (now - _lastMotionMs >= 16) {
        _lastMotionMs = now;
        std::array<std::byte, 7> buf{};
        std::span<std::byte> s(buf);
        uint16_t pos10 = clampU16(_state.actual_position_mm.load(std::memory_order_relaxed) * 100.0f);
        uint16_t tgt10 = clampU16(_state.commanded_target_mm * 100.0f);
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
        _hub.publishState(ch::motion, s);
    }

    // ---- 0x0081 machine-config — on cfg_gen change -----------------------
    uint16_t gen = _state.cfg_gen.load(std::memory_order_relaxed);
    if (!_cfgEverSent || gen != _lastCfgGen) {
        _cfgEverSent = true;
        _lastCfgGen = gen;
        std::array<std::byte, 28> buf{};
        std::span<std::byte> s(buf);
        slopsync::putF32(s.subspan(0, 4), _state.config.min_position_mm);
        slopsync::putF32(s.subspan(4, 4), _state.config.max_position_mm);
        slopsync::putF32(s.subspan(8, 4), _state.config.user_max_speed_mm_s);
        slopsync::putF32(s.subspan(12, 4), _state.config.user_max_accel_mm_s2);
        slopsync::putF32(s.subspan(16, 4), _state.config.input_max_speed_mm_s);
        slopsync::putF32(s.subspan(20, 4), _state.config.input_max_accel_mm_s2);
        slopsync::putF32(s.subspan(24, 4), _state.config.max_rail_mm);
        _hub.publishState(ch::machine_config, s);
    }

    // ---- 0x0082 pattern-state — on change, ≥100 ms -----------------------
    if (_patternEngine && (now - _lastPatternMs >= 100)) {
        bool running = _patternEngine->isRunning();
        uint8_t idx = uint8_t(_patternEngine->getPatternIdx());
        float speed = _patternEngine->getSpeedPercent();
        float depth = _patternEngine->getDepthPercent();
        float stroke = _patternEngine->getStrokePercent();
        float sensation = _patternEngine->getSensationPercent();
        bool changed = running != _patRunning || idx != _patIdx || speed != _patSpeed ||
                       depth != _patDepth || stroke != _patStroke || sensation != _patSensation;
        if (changed) {
            _lastPatternMs = now;
            _patRunning = running;
            _patIdx = idx;
            _patSpeed = speed;
            _patDepth = depth;
            _patStroke = stroke;
            _patSensation = sensation;
            std::array<std::byte, 18> buf{};
            std::span<std::byte> s(buf);
            slopsync::putU8(s.subspan(0, 1), running ? 1 : 0);
            slopsync::putU8(s.subspan(1, 1), idx);
            slopsync::putF32(s.subspan(2, 4), speed);
            slopsync::putF32(s.subspan(6, 4), depth);
            slopsync::putF32(s.subspan(10, 4), stroke);
            slopsync::putF32(s.subspan(14, 4), sensation);
            _hub.publishState(ch::pattern_state, s);
        }
    }

    // ---- 1 Hz block: 0x0083 odometer, 0x0006 hub-status, pairing persist -
    if (now - _lastSlowMs >= 1000) {
        _lastSlowMs = now;

        {  // 0x0083 odometer
            std::array<std::byte, 12> buf{};
            std::span<std::byte> s(buf);
            slopsync::putU32(s.subspan(0, 4), _state.stroke_count.load(std::memory_order_relaxed));
            slopsync::putF32(s.subspan(4, 4),
                             _state.session_distance_mm.load(std::memory_order_relaxed) / 1000.0f);
            slopsync::putF32(s.subspan(8, 4), _state.max_speed_mm_s.load(std::memory_order_relaxed));
            _hub.publishState(ch::odometer, s);
        }

        {  // 0x0006 hub-status
            std::array<std::byte, 10> buf{};
            std::span<std::byte> s(buf);
            slopsync::putU32(s.subspan(0, 4), uint32_t(ESP.getFreeHeap()));
            slopsync::putU32(s.subspan(4, 4), uint32_t(now / 1000u));
            slopsync::putU8(s.subspan(8, 1), uint8_t(int8_t(WiFi.RSSI())));
            slopsync::putU8(s.subspan(9, 1), uint8_t(_hub.sessionCount()));
            _hub.publishState(slopsync::channels::hub_status, s);
        }

        persistPairingIfChanged();
    }
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

void SlopSyncHubService::loadPairing() {
    Preferences prefs;
    if (!prefs.begin("slopsync", true)) return;  // read-only; absent namespace is fine
    size_t n = prefs.getBytesLength("tokens");
    // Each entry is fixed 25 bytes: instance_id(8) + token(16) + role(1).
    constexpr size_t kEntryBytes = 8 + 16 + 1;
    constexpr size_t kMax = slopsync::PairingManager::kMaxPaired;
    if (n > 0 && n % kEntryBytes == 0 && n <= kMax * kEntryBytes) {
        uint8_t blob[kMax * kEntryBytes];
        prefs.getBytes("tokens", blob, n);
        size_t cnt = n / kEntryBytes;
        for (size_t k = 0; k < cnt; ++k) {
            slopsync::PairingManager::PairedEntry e{};
            std::memcpy(e.instance_id.data(), blob + k * kEntryBytes, 8);
            std::memcpy(e.token.data(), blob + k * kEntryBytes + 8, 16);
            uint8_t r = blob[k * kEntryBytes + 24];
            e.role = (r > uint8_t(slopsync::AccessLevel::admin)) ? slopsync::AccessLevel::controller
                                                                 : slopsync::AccessLevel(r);
            e.used = true;
            _hub.pairing().importEntry(e);
        }
        _pairCountCached = cnt;
        SLOGI("slopsync", "loaded %u paired token(s) from NVS", unsigned(cnt));
    }
    prefs.end();
}

void SlopSyncHubService::savePairing() {
    // Respect the OTA NVS gate: a flash-cache access during an OTA write window
    // can reset the chip (same reason ConfigStore::save defers under ota_active).
    if (_state.ota_active.load(std::memory_order_relaxed)) return;

    auto& pm = _hub.pairing();
    size_t cnt = pm.entryCount();
    constexpr size_t kEntryBytes = 8 + 16 + 1;
    constexpr size_t kMax = slopsync::PairingManager::kMaxPaired;
    uint8_t blob[kMax * kEntryBytes];
    size_t off = 0;
    for (size_t k = 0; k < cnt && k < kMax; ++k) {
        const slopsync::PairingManager::PairedEntry* e = pm.entry(k);
        if (!e) break;
        std::memcpy(blob + off, e->instance_id.data(), 8);
        off += 8;
        std::memcpy(blob + off, e->token.data(), 16);
        off += 16;
        blob[off++] = uint8_t(e->role);
    }
    Preferences prefs;
    if (prefs.begin("slopsync", false)) {
        if (off > 0) {
            prefs.putBytes("tokens", blob, off);
        } else {
            prefs.remove("tokens");
        }
        prefs.end();
    }
    _pairCountCached = cnt;
}

void SlopSyncHubService::persistPairingIfChanged() {
    // Cheap backstop: a NEW pairing grows entryCount. (A re-pair of an existing
    // instance reissues a token at the SAME count — that case is caught by the
    // savePairing() in closePairing(), which runs when the window ends.)
    if (_hub.pairing().entryCount() != _pairCountCached) savePairing();
}

// ============================================================================
// OTA park/revive
// ============================================================================

void SlopSyncHubService::suspendTask() {
    if (_task) vTaskSuspend(_task);
}

void SlopSyncHubService::resumeTask() {
    if (_task) vTaskResume(_task);
}

}  // namespace slopdrive
