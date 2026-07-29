// MachineSim — virtual SlopDrive implementation (see MachineSim.h for the
// composition contract and threading rule).

#include "machine/MachineSim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "slopsync/util/byte_io.hpp"

namespace slopsim {

using slopsync::IntentValue;
using slopsync::IntentValueField;
using slopsync::IntentValueMap;
using slopsync::NackCode;

// Unqualified `ch::` below means the REAL device catalog's channel ids
// (include/comms/SlopSyncCatalog.h) — `benchrig::ch::` (Alien) stays fully
// qualified everywhere so the two id spaces are never visually confusable.
namespace ch = slopdrive::ch;

namespace {

// Firmware clamp ceilings — these mirror the EXPERT-mode values in
// include/system/config_api.h (~lines 332-348): EXPERT_MAX_SPEED_MM_S = 10000,
// EXPERT_MAX_ACCEL_MM_S2 = 100000. The sim runs expert-wide on purpose: the
// operator's real machine does ~1000 mm/s and ~50000 mm/s^2, and the NORMAL
// accel cap (20000) would refuse 50000 outright.
// DIVERGENCE TO KNOW: a device left in NORMAL mode enforces 1000/20000 and
// WILL refuse an accel the sim happily accepted. Sim accepts != device accepts.
// DEFAULT INPUT LIMITS ARE NO LONGER THE DEVICE FACTORY PAIR: the sim boots at
// 1000 mm/s / 60000 mm/s^2 (was 550/8000) because that is what the bench machine
// is being tuned toward. A device still on 550/8000 will NOT reproduce a sim
// trace taken at these defaults — set the device to match (or the sim back)
// before comparing two traces. See MachineSim.h `_input_speed`.
constexpr float kSpeedCeiling = 10000.0f;    // mm/s     — EXPERT_MAX_SPEED_MM_S
constexpr float kAccelCeiling = 100000.0f;   // mm/s^2   — EXPERT_MAX_ACCEL_MM_S2
constexpr float kJerkCeiling  = 50000000.0f; // mm/s^3   — EXPERT_MAX_JERK_MM_S3
                                             // (NORMAL is 10000000; same
                                             // expert-wide posture as above.)
// Soft-start on the stream feed. Two DIFFERENT, opposite quantities, both from
// include/system/config_api.h (~409-410):
//   kSafeApproachSpeed — a FLOOR under the speed the sampler feeds FAS, so a
//                        gentle ceiling never collapses to a crawl.
//   kSafeResumeRampMs  — the window over which SystemState::safeSpeedCap ramps
//                        the CEILING up from kSafeApproachSpeed after a
//                        discontinuity (un-pause, new stream, homing done).
constexpr float kSafeApproachSpeed = 100.0f; // mm/s   SAFE_APPROACH_SPEED_MM_S
constexpr uint32_t kSafeResumeRampMs = 1200; // ms     SAFE_RESUME_RAMP_MS
// SystemState::StreamSpeedMode. 0 is the DEVICE DEFAULT.
constexpr uint8_t kSpeedCeilingPegged = 0;
constexpr uint8_t kSpeedVelocityMatched = 1;
// RangeMapper::setRange minimum window span (src/motion/range_mapper.cpp:27).
constexpr float kMinWindowSpanMm = 5.0f;
constexpr int16_t kSegNoEndVel = -32768;    // 0x0085 sentinel (SlopSyncCatalog)
constexpr uint64_t kHomingDurationUs = 3'000'000;  // fake sensorless homing time

// Arbiter sources (MotionSource enum values, MotionArbiter.h).
constexpr uint8_t kSrcManual = 0;
constexpr uint8_t kSrcTcodeStream = 1;
constexpr uint8_t kSrcPattern = 2;

const IntentValueField* findField(const IntentValueMap& m, uint8_t key) {
    for (uint32_t i = 0; i < m.count; ++i)
        if (m.fields[i].key == key) return &m.fields[i];
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

// Device-profile scaled-u16/i16 wire helpers (0x1100 motion, 0x1110 plan-strip
// etc. — the real device's packed layouts, unlike Alien's plain f32 shapes).
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

uint64_t fieldU64(const IntentValueField* f, uint64_t dflt) {
    if (!f) return dflt;
    if (f->value.kind == IntentValue::Kind::U64) return f->value.u64_val;
    if (f->value.kind == IntentValue::Kind::I64 && f->value.i64_val >= 0) return uint64_t(f->value.i64_val);
    return dflt;
}

// benchrig-only: 0x0111's `device_label` (CborFieldType::tstr_t). No other
// channel on this catalog writes a string, so this stays local to MachineSim
// rather than joining the generic fieldF32/fieldBool/fieldU64 family above.
std::string_view fieldTstr(const IntentValueField* f, std::string_view dflt) {
    if (!f) return dflt;
    if (f->value.kind == IntentValue::Kind::Tstr) return f->value.tstr_val;
    return dflt;
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---- fray-d Advanced pattern setters -- mirror PatternEngine::setApBase/
// setApModifier (src/motion/PatternEngine.cpp) exactly, minus the Arduino
// `constrain()` calls (plain clamps here) and the `_ap_gen` bump (no
// generation-counted UI poll seam on the sim side). Free functions, not
// MachineSim members, so pattern_advanced_cmd AND the preset "load" op
// (pattern_presets_cmd) can share ONE clamp/coupling implementation instead
// of two copies drifting apart.
void setApBase(advpat::Settings& ap, uint8_t id, int v) {
    advpat::BaseControl* c = ap.byId(id);
    if (!c) return;
    c->set(v);
    // fray-d setDepthLimits: the depth pair may never cross.
    if (id == advpat::DEPTH_MAX || id == advpat::DEPTH_MIN) ap.coupleDepths();
}

void setApModifier(advpat::Settings& ap, uint8_t id, int amplitude, int in_step, int in_wait,
                    int out_step, int out_wait, int offset) {
    advpat::BaseControl* c = ap.byId(id);
    if (!c) return;
    auto cl = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    advpat::Modifier& m = c->modifier;
    m.amplitude = uint8_t(cl(amplitude, 0, 100));
    m.in_step   = uint8_t(cl(in_step,   1, 25));
    m.in_wait   = uint8_t(cl(in_wait,   0, 25));
    m.out_step  = uint8_t(cl(out_step,  1, 25));
    m.out_wait  = uint8_t(cl(out_wait,  0, 25));
    m.offset    = uint8_t(cl(offset,    0, 100));
}

}  // namespace

// ---- Lifecycle --------------------------------------------------------------

// See SlopSyncHubService.cpp's twin: buildSlopDriveCatalog() is an out-param
// builder (a Catalog32 is ~22 KB and never a return value), but `_hub` binds
// the catalog by reference and encodes it in ITS constructor — so the fill has
// to happen inside the member-initializer list, not the constructor body.
//
// slopsim's catalog is SELECTED BY PROFILE (SlopDeck DESIGN.md §5/§7):
// `device` (default) is the REAL SlopDrive-32 catalog, LITERALLY
// buildSlopDriveCatalog() from include/comms/SlopSyncCatalog.h — the two can
// never silently diverge because there is only one definition. `alien` is
// benchrig (SlopSimCatalog.h) — a deliberately different conformant hub, so a
// client pointed at it must prove it renders a machine it has never met.
// `minimal` is a SUBSET of the real device catalog (SlopMinimalCatalog.h).
static slopsync::Catalog32& initCatalog(slopsync::Catalog32& c, Profile profile) {
    switch (profile) {
        case Profile::Alien:
            benchrig::buildDivergentCatalog(c);
            break;
        case Profile::Minimal:
            slopdrive::buildMinimalCatalog(c);
            break;
        case Profile::Device:
        default:
            // has_current_sensor/has_power_monitor: true/true — the sim mirrors
            // the real SlopDrive-32's INA228-equipped build, so the `power`
            // channel (0x1010) is advertised exactly like the live device.
            slopdrive::buildSlopDriveCatalog(c, {/*current sensor*/ true, /*power monitor*/ true});
            break;
    }
    return c;
}

MachineSim::MachineSim(SessionLog& log, Profile profile)
    : _log(log),
      _profile(profile),
      _realDeviceIds(profile != Profile::Alien),
      _hasFullDeviceCatalog(profile == Profile::Device),
      _catalog(),
      _hub(initCatalog(_catalog, profile), _clock, _rng, *this, _crypto) {
    // A fixed, printable "keypair" so a probe run is reproducible across
    // restarts — the real device generates one at first boot and persists it in
    // NVS (firmware M5), which is the only part of this that is not simulable
    // without hardware.
    _crypto.p256Supported = true;
    for (size_t i = 0; i < _crypto.pubkey.size(); ++i) _crypto.pubkey[i] = std::byte(uint8_t(0x02 + i));

    // Runtime defaults are profile-dependent — see the field comments in
    // MachineSim.h. Alien keeps its own smaller/cheaper benchrig:: numbers;
    // Device and Minimal use the real device's config_api-mirrored factory::
    // numbers, so a `device`-profile trace is comparable to a hardware one.
    if (profile == Profile::Alien) {
        _win_min_mm = benchrig::factory::window_min;
        _win_max_mm = benchrig::factory::window_max;
        _max_rail_mm = benchrig::factory::window_max;
        _user_speed = benchrig::factory::user_speed;
        _user_accel = benchrig::factory::user_accel;
    } else {
        _win_min_mm = slopdrive::factory::window_min;
        _win_max_mm = slopdrive::factory::window_max;
        _max_rail_mm = slopdrive::factory::max_rail;
        _user_speed = slopdrive::factory::user_speed;
        _user_accel = slopdrive::factory::user_accel;
    }
}

bool MachineSim::begin(uint16_t wsPort, bool startHomed) {
    // Catalog encode health — a catalog that overflowed the hub's scratch
    // encodes to ZERO bytes, hashes to an etag over nothing, and serves an
    // empty catalog while everything else looks fine. M5a's annotations found
    // that ceiling for real; this line is so the sim can never hide it again.
    if (_hub.catalogEncodedBytes() == 0) {
        _log.logf('E', "sim: CATALOG DID NOT ENCODE (scratch %u B) — this hub advertises NOTHING",
                  unsigned(slopsync::Hub::catalogScratchCapacity()));
    } else {
        _log.logf('I', "sim: catalog encodes to %u B of %u B scratch",
                  unsigned(_hub.catalogEncodedBytes()),
                  unsigned(slopsync::Hub::catalogScratchCapacity()));
    }
    deriveEngineLimits();
    _stepper.reset(0.0f);
    _engine.resetAt(clampf(mmToNorm(0.0f), 0.0f, 1.0f), _clock.nowUs64());
    _lastSampleUs = _clock.nowUs64();
    if (startHomed) {
        _homed = true;
        // A homed machine HAS a measured stroke (a device that skipped the
        // cycle wouldn't be homed), so seed it like the cycle would.
        if (_measured_stroke_mm <= 0.0f) _measured_stroke_mm = _max_rail_mm;
        _log.logf('I', "sim: starting pre-homed (--homed), stroke %.1f mm",
                  double(_measured_stroke_mm));
    }
    return _port.begin(&_hub, wsPort, &_log);
}

void MachineSim::shutdown() { _port.stop(); }

void MachineSim::tick() {
    const uint64_t now64 = _clock.nowUs64();
    const uint32_t nowMs = uint32_t(now64 / 1000);

    // ---- Host loop-period instrumentation -----------------------------------
    // The sim's whole motion fidelity rests on being CALLED often enough: the
    // substep loop can only place commits on the 1 ms grid if ticks arrive on
    // roughly that grid. Windows' default 15.6 ms timer resolution used to make
    // every commit land at the head of a ~16 ms substep burst (a flat setpoint
    // for the whole burst, then a jump). Measured here so the lie is visible in
    // GET /api/slopmotion instead of hiding in the trace.
    if (_lastTickUs != 0) {
        const float dt_ms = float(double(now64 - _lastTickUs) / 1000.0);
        _tick_dt_avg_ms = _tick_dt_avg_ms <= 0.0f ? dt_ms : 0.99f * _tick_dt_avg_ms + 0.01f * dt_ms;
        if (dt_ms > _tick_dt_max_ms) _tick_dt_max_ms = dt_ms;
    }
    _lastTickUs = now64;

    // ---- COMMS pump @ ~5 ms (SlopSyncHubService::taskLoop) ------------------
    // The device's hub task runs on a pdMS_TO_TICKS(5) delay; frames, pacing,
    // deadman and telemetry all inherit that granularity. Pumping it at the
    // motion rate would make the sim's wire FASTER than the device's, which is
    // the same class of lie in the other direction.
    constexpr uint64_t kPumpPeriodUs = 5000;
    if (now64 - _lastPumpUs >= kPumpPeriodUs) {
        _lastPumpUs = now64;
        _port.loop(nowMs);
        _hub.update(_clock.nowUs());  // fires onStreamBundle -> pacing ring
        // THE DEFERRED SIGNER, standing in for the firmware's low-priority
        // worker task. It is deliberately a SEPARATE call from update() rather
        // than something the hub does for itself: on the S3 one software ECDSA
        // is ~30-80 ms in a single uninterruptible call, and doing it inside
        // the 5 ms comms pump would stall STATE pacing, the deadman and the
        // motion-input drain for 6-16 ticks every time a client connected. On a
        // host the sign is free, so draining it here costs nothing and still
        // exercises the exact HUB_SIG wire the device will use.
        _hub.signPendingNow(1);
        syncSafety();
        publishTelemetry(nowMs);

        {  // refresh the HTTP facade's cross-thread copy
            std::lock_guard<std::mutex> lk(_facadeM);
            _facade.bundles = _sync_bundles;
            _facade.seg_bundles = _sync_seg_bundles;
            _facade.samples = _sync_samples;
            _facade.enqueued = _sync_enqueued;
            _facade.dropped = _sync_dropped;
            _facade.plan_rejected = _plan_rejected;
            _facade.anomalies = _anom_total;
            _facade.anom_kind = _anom_kind;
            _facade.ts_clamped = _ts_clamped;
            _facade.substeps_discarded = _substeps_discarded;
            _facade.max_rail_mm = _max_rail_mm;
            _facade.tick_ms_avg = _tick_dt_avg_ms;
            _facade.tick_ms_max = _tick_dt_max_ms;
            _facade.stream_speed_mode = _stream_speed_mode;
        }
    }

    // ---- MOTION @ 1 ms (streamSamplerTask) ----------------------------------
    // Drains the pacing ring INSIDE the substep loop, so a commit is current
    // for exactly one 1 ms sample — see tickMachine().
    tickMachine(now64);
}

MachineSim::FacadeStats MachineSim::facadeStats() const {
    std::lock_guard<std::mutex> lk(_facadeM);
    return _facade;
}

void MachineSim::deriveEngineLimits() {
    // Firmware glue derives the engine's normalized ceilings from the mm-domain
    // INPUT limit set across the stroke-window span.
    slopmotion::Limits l;
    const float span = windowSpan();
    // v/a follow jerk's own "override wins when > 0" rule (main.cpp's
    // smCfg.limits.vmax/amax derivation) via sm-set (0x3120) keys 2/3 —
    // _vmax_norm/_amax_norm are sim-held for the SAME reason _jmax_norm is:
    // this rebuild runs on every window/limit change and would otherwise
    // clobber a standing override.
    l.vmax = _vmax_norm > 0.0f ? _vmax_norm : _input_speed / span;
    l.amax = _amax_norm > 0.0f ? _amax_norm : _input_accel / span;
    // Jerk derives the SAME way as v/a now (mm-domain limit ÷ span). The
    // normalized override (uiSetJmax / --jmax / motion.jmax) is sim-held so
    // this rebuild can't clobber it, and wins when > 0 — firmware `jovr`.
    l.jmax = _jmax_norm > 0.0f ? _jmax_norm : _input_jerk / span;
    _engine.setLimits(l);
}

// ---- Machine physics --------------------------------------------------------
// Homing, pattern, the 1 ms sampler/stepper substep loop.

// SystemState::safeSpeedCap, transcribed verbatim (SystemState.h ~403-412).
float MachineSim::safeSpeedCap(float configured_max, uint32_t now_ms) const {
    const uint32_t t0 = _resume_start_ms;
    if (t0 == 0) return configured_max;
    const uint32_t dt = now_ms - t0;
    if (dt >= kSafeResumeRampMs) return configured_max;
    if (configured_max <= kSafeApproachSpeed) return configured_max;
    const float f = float(dt) / float(kSafeResumeRampMs);
    return kSafeApproachSpeed + f * (configured_max - kSafeApproachSpeed);
}

// MotionArbiter::_isOutsideWindow (MotionArbiter.cpp ~315).
bool MachineSim::isOutsideWindow(float p0_mm) const {
    const float eps = 0.5f;  // sub-safety-zone slack, avoids edge chatter
    return (p0_mm < _win_min_mm - eps) || (p0_mm > _win_max_mm + eps);
}

void MachineSim::tickMachine(uint64_t now64) {
    // Homing completion (time-based fake of the sensorless cycle).
    if (_homing && now64 >= _homing_done_us) {
        _homing = false;
        _homed = true;
        _stepper.reset(0.0f);
        _engine.resetAt(clampf(mmToNorm(0.0f), 0.0f, 1.0f), now64);
        // A real cycle MEASURES the stroke; without this the effective ceiling
        // stays the configured rail forever (MotorDriver::effectiveCeilingMm).
        if (_measured_stroke_mm <= 0.0f) _measured_stroke_mm = _max_rail_mm;
        // Soft-start guard, exactly like the firmware's post-home stamp
        // (WebUI.cpp "soft-start guard like a real home").
        _resume_start_ms = uint32_t(now64 / 1000);
        _log.logf('I', "sim: homing complete — homed at 0.0 mm (stroke %.1f mm)",
                  double(_measured_stroke_mm));
    }

    // 1 ms substeps from the last cursor to now. The firmware sampler is a hard
    // vTaskDelayUntil(1 ms) loop; here the host loop period is whatever Windows
    // gives us, so we REPLAY the missed 1 ms instants rather than sampling once
    // at "now". Replay (not skip) is the right call because the SimStepper IS
    // the physics: skipping instants would silently delete integration time and
    // the carriage would move less than wall-clock says it should. The firmware
    // can skip harmlessly only because FAS keeps ramping in hardware meanwhile.
    // Cap the catch-up burst at 100 ms so a stalled host can't spiral; beyond
    // that we DO lose time — counted in _substeps_discarded and logged, never
    // silent.
    constexpr uint64_t kDt = 1000;
    constexpr uint64_t kMaxCatchup = 100;
    uint64_t steps = (now64 - _lastSampleUs) / kDt;
    if (steps > kMaxCatchup) {
        _substeps_discarded += uint32_t(steps - kMaxCatchup);
        _lastSampleUs = now64 - kMaxCatchup * kDt;
        steps = kMaxCatchup;
        _log.logf('W', "sim: host stall — discarded %u ms of motion time (total %u)",
                  unsigned(_substeps_discarded), unsigned(_substeps_discarded));
    }

    std::lock_guard<std::mutex> traceLk(_traceM);  // bulk-guard this tick's appends
    for (uint64_t i = 0; i < steps; ++i) {
        _lastSampleUs += kDt;
        const uint64_t t = _lastSampleUs;

        // Commit sources FIRST, at this substep's instant. This ordering is the
        // whole point: slopmotion::Engine::elapsedS() returns 0 while
        // now_us <= plan_start, so a plan committed at T evaluates to its START
        // point for every sample at or before T. Draining once per HOST tick
        // (as this used to) therefore held the setpoint flat for the entire
        // tick and then jumped — a staircase at the host timer's granularity,
        // not a 1 kHz curve. Commit at t, sample at t: elapsed==0 lasts exactly
        // one 1 ms sample, which is what streamSamplerTask does on the device.
        tickPattern(t);
        drainMotionStream(t);
        // Immediately after the commit point — see drainAnomalies()'s note on
        // why this is per-substep and not per-host-tick.
        drainAnomalies(t);

        // motorTask's Core-1 consumption of estop_requested: the pulse train
        // dies HERE, one motion tick after the latch — which is exactly the
        // window in which canClearEstop() must refuse (CLEAR_REFUSED).
        if (_estop_pending) {
            _stepper.hardStop();
            _estop_pending = false;
        }

        if (_homing) {
            // Homing drives the stepper directly at the gentle homing feed —
            // the arbiter path is bypassed on the firmware too.
            _stepper.command(0.0f, 40.0f, 500.0f);
        } else if (_homed && !_paused && !_override && !_estop_latched) {
            // ---- MotionArbiter::submitStreamSample, transcribed -------------
            const float norm = clampf(_engine.positionAt(t), 0.0f, 1.0f);
            const float vel_norm = _engine.velocityAt(t);
            // Window clamp, then the HARD machine envelope: measured stroke
            // once homed, else the configured rail (effectiveCeilingMm).
            float target_mm = clampf(normToMm(norm), _win_min_mm, _win_max_mm);
            target_mm = clampf(target_mm, 0.0f, effectiveCeilingMm());

            // Window-entry gentleness: a carriage OUTSIDE the window glides in
            // on the USER (gentle) set, both speed AND accel.
            const bool entering = isOutsideWindow(_stepper.positionMm());
            float speed_ceiling = entering ? std::min(_input_speed, _user_speed) : _input_speed;
            // Safe-approach soft start ramps the CEILING back up after a
            // discontinuity — applied BEFORE the floor clamp below.
            const float safe_cap = safeSpeedCap(speed_ceiling, uint32_t(t / 1000));
            if (safe_cap < speed_ceiling) speed_ceiling = safe_cap;
            const float accel_ceiling =
                entering ? std::min(_input_accel, _user_accel) : _input_accel;
            const float speed_floor = std::min(kSafeApproachSpeed, speed_ceiling);

            float speed_mm_s;
            if (_stream_speed_mode == kSpeedVelocityMatched) {
                // FAS coasts the curve's own instantaneous speed.
                speed_mm_s = std::fabs(vel_norm) * windowSpan();
                if (speed_mm_s > speed_ceiling) speed_mm_s = speed_ceiling;
                if (speed_mm_s < speed_floor) speed_mm_s = speed_floor;
            } else {
                // DEVICE DEFAULT — ceiling-pegged: constant speed, the 1 ms
                // micro-target deltas shape the velocity. The follower keeps
                // full authority, so it can actually recover lag.
                speed_mm_s = speed_ceiling;
                if (speed_mm_s < speed_floor) speed_mm_s = speed_floor;
            }
            _stepper.command(target_mm, speed_mm_s, accel_ceiling);
        }
        _stepper.step(0.001f);

        // Odometer integration off the ACTUAL (stepper) motion.
        const float v = _stepper.velocityMmS();
        _distance_mm += std::fabs(v) * 0.001f;
        if (std::fabs(v) > _peak_mm_s) _peak_mm_s = std::fabs(v);
        if (_lastOdoVel <= -15.0f && v >= 15.0f) ++_strokes;  // bottom reversal = one stroke
        if (std::fabs(v) > 15.0f) _lastOdoVel = v;

        // Sender-curve shadow, advanced on the SIM clock (not the machine's
        // progress) and held at its endpoint once the span expires, which is
        // what makes an overrun visible as raw-flat-while-planned-still-moving.
        if (_raw_T > 0.0 && t >= _raw_start_us) {
            double rt = double(t - _raw_start_us) * 1e-6;
            if (rt > _raw_T) rt = _raw_T;
            double rp, rv, ra;
            slopmotion::Engine::evalCurve(_raw_c, _raw_T, rt / _raw_T, rp, rv, ra);
            _trace_raw_norm = float(rp < 0.0 ? 0.0 : (rp > 1.0 ? 1.0 : rp));
        }
        // Motion trace (1 kHz): the graph pane + CSV export read this ring.
        _trace[_traceHead] = {float(double(t) / 1e6), _stepper.positionMm(), _stepper.targetMm(), v,
                              _trace_cmd_norm, _trace_raw_norm};
        _traceHead = (_traceHead + 1) % kTraceCap;
        if (_traceCount < kTraceCap) ++_traceCount;
    }

    _commanded_target_mm = _stepper.targetMm();
}

void MachineSim::tickPattern(uint64_t now64) {
    auto& p = _pattern;
    if (!p.running || !_homed || _paused || _override || _estop_latched) return;
    if (now64 < p.next_due_us) return;

    // Stroke band in normalized window units: [depth-stroke, depth] (%).
    float top = clampf(p.depth / 100.0f, 0.0f, 1.0f);
    float amp = clampf(p.stroke / 100.0f, 0.0f, 1.0f);
    if (p.idx == 2) amp *= 0.35f;  // shallow-fast
    float bottom = clampf(top - amp, 0.0f, 1.0f);

    // Cadence: speed% maps to per-leg duration; sensation skews deep vs pull.
    float leg_ms = 2000.0f - 18.0f * clampf(p.speed, 0.0f, 100.0f);
    if (p.idx == 2) leg_ms *= 0.6f;
    const float skew = clampf(p.sensation, -100.0f, 100.0f) / 200.0f;  // -0.5..0.5
    leg_ms *= p.goingDeep ? (1.0f - skew) : (1.0f + skew);
    if (leg_ms < 120.0f) leg_ms = 120.0f;

    slopmotion::Command cmd;
    cmd.target = p.goingDeep ? top : bottom;
    cmd.duration_us = uint32_t(leg_ms * 1000.0f);
    cmd.has_duration = true;
    cmd.end_vel = 0.0f;      // each leg ends at rest (tease idx 1 keeps flow)
    cmd.has_end_vel = (p.idx != 1);
    _trace_cmd_norm = cmd.target;   // analyzer overlay: the commanded leg
    _engine.commit(cmd, now64);

    p.goingDeep = !p.goingDeep;
    p.next_due_us = now64 + uint64_t(leg_ms * 1000.0f);
}

// WS_OP_HALT / safety STOP+HOLD seam. The device's MotionArbiter::hardStopMotion
// is ONE line — `_motor.hardStop()`. It is a forceStop, NOT a latch: it does not
// touch the interpolator plan and does not stop the pattern generator, so on the
// next 1 ms sample the sampler re-commands FAS onto the live curve and motion
// RESUMES. The sim used to reset the engine and kill the pattern here, which
// made a sim STOP stickier than the machine's — a safety-shaped lie in the
// flattering direction (the sim looked safer than the hardware). Now it matches.
// Callers that model the device's motor RELEASE (deadman, unhome, e-stop) use
// releaseMotion() instead — see below.
void MachineSim::hardStopMotion() { _stepper.hardStop(); }

// The sim's stand-in for streamSamplerTask going !streamActive: on the device,
// motion after a stop only STAYS stopped because the sampler stops feeding FAS
// (STREAM_IDLE_TIMEOUT_MS with no packets, or the gates closing). The sim has no
// stream-idle gate — its sampler always renders the live plan — so anything that
// on hardware would leave the motor unfed drops the plan here instead. Same net
// behavior, one less moving part. §11.3's "no unmonitored path to motion" holds.
void MachineSim::releaseMotion() {
    _stepper.hardStop();
    _engine.resetAt(clampf(mmToNorm(_stepper.positionMm()), 0.0f, 1.0f), _clock.nowUs64());
}

// RangeMapper::setRange (src/motion/range_mapper.cpp:16-30), transcribed:
// swap if inverted, clamp both ends to [0, max_rail], then guarantee a 5 mm
// minimum span. NOTE the firmware's own quirk is preserved: the 5 mm expansion
// pushes the MAX end and is NOT re-clamped to the rail, so a degenerate window
// at the very top of the rail ends 5 mm past it. Mirroring it keeps the sim
// honest about what the device does; do not "fix" it here.
void MachineSim::applyWindowLegality(float min_mm, float max_mm) {
    if (min_mm > max_mm) std::swap(min_mm, max_mm);
    _win_min_mm = clampf(min_mm, 0.0f, _max_rail_mm);
    _win_max_mm = clampf(max_mm, 0.0f, _max_rail_mm);
    if (_win_max_mm - _win_min_mm < kMinWindowSpanMm) {
        _win_max_mm = _win_min_mm + kMinWindowSpanMm;
    }
}

// ---- Stream drain -----------------------------------------------------------
// Transcription of SlopSyncHubService::drainMotionStream.

// Rebuild the sender's own curve for this segment. See the state block in
// MachineSim.h for why it is advanced in the sender's frame and never chases
// the machine.
void MachineSim::noteSenderCurve(const slopmotion::Command& cmd, uint64_t now64) {
    // Chase points carry no span, so there is no curve to draw through them —
    // leave the previous one to finish rather than inventing a shape.
    if (!cmd.has_duration || cmd.duration_us == 0) return;
    const double T = double(cmd.duration_us) * 1e-6;
    const double target = cmd.target < 0.0f ? 0.0
                        : (cmd.target > 1.0f ? 1.0 : double(cmd.target));
    if (_raw_p < 0.0) {                       // anchor the sender frame once
        _raw_p = double(_engine.snapshot(now64).pos);
        _raw_v = 0.0;
    }
    const double vf = cmd.has_end_vel ? double(cmd.end_vel) : 0.0;
    // af mirrors the engine's OWN backward-difference estimator (commitWaveform)
    // so that raw and planned differ by FEASIBILITY only, never because the two
    // lines guessed the sender's curvature differently. Ignored outright in C1,
    // where a cubic takes no end acceleration.
    double af = 0.0;
    if (cmd.has_end_vel && _raw_prev_ok && now64 > _raw_prev_us) {
        const double gap = double(now64 - _raw_prev_us) * 1e-6;
        if (gap < 3.0 * T) af = (vf - _raw_prev_vf) / gap;
    }
    // Start acceleration is 0 in the sender's frame: the wire carries no such
    // field, and a C1 cubic ignores it entirely. It only shades the C2 line.
    slopmotion::Engine::senderCurve(
        _engine.config().curve_policy == slopmotion::CurvePolicy::ForceC1,
        _raw_p, _raw_v, 0.0, target, vf, af, T, _raw_c);
    _raw_T         = T;
    _raw_start_us  = now64;
    _raw_p         = target;   // the sender's frame advances to ITS endpoint
    _raw_v         = vf;
    _raw_prev_vf   = vf;
    _raw_prev_us   = now64;
    _raw_prev_ok   = cmd.has_end_vel;
}

void MachineSim::drainMotionStream(uint64_t now64) {
    PacingEntry entry;
    while (_pacingRing.popDue(now64, entry)) {
        if (!_homed) {
            ++_sync_dropped;
            continue;
        }
        if (_paused || _override) {
            _resume_start_ms = uint32_t(now64 / 1000);
            ++_sync_dropped;
            continue;
        }

        const uint32_t now = uint32_t(now64 / 1000);
        if (_last_intiface_ms == 0 || (now - _last_intiface_ms) > 2000) {
            _resume_start_ms = now;  // new-stream soft start stamp
        }
        if (_last_cmd_ms != 0) {
            const uint32_t gap = now - _last_cmd_ms;
            if (gap > 0 && gap < 1000) {
                _measured_interval_ms = _measured_interval_ms <= 0.0f
                                            ? float(gap)
                                            : 0.7f * _measured_interval_ms + 0.3f * float(gap);
            }
        }
        _last_cmd_ms = now;
        _last_intiface_ms = now;
        if (_syncPrevTarget < 0.0f || std::fabs(entry.target - _syncPrevTarget) > 0.003f) {
            _last_intiface_move_ms = now;
            _syncPrevTarget = entry.target;
        }

        // Pre-planning demand telemetry (0x0080 `raw_10um`), set at the same
        // point buttplugLinearCmd sets SystemState::commanded_raw_mm on the
        // device: after the gates, before the engine sees anything.
        _commanded_raw_mm = normToMm(entry.target);

        slopmotion::Command cmd;
        cmd.target = entry.target;
        cmd.end_vel = entry.vel;
        cmd.has_end_vel = entry.has_end_vel;
        cmd.duration_us = entry.duration_us;
        cmd.has_duration = entry.has_duration;

        // RFC-008 one-segment LOOKAHEAD — verbatim mirror of the firmware's
        // drainMotionStream. The engine owns the bound; ingress owns only "a
        // successor exists, and this is how fast it moves".
        if (entry.has_duration && entry.has_end_vel) {
            const PacingEntry* next = _pacingRing.peekOldest();
            if (next && next->has_duration && next->duration_us > 0) {
                const float next_dur_s = float(next->duration_us) * 1e-6f;
                cmd.next_chord     = std::fabs(next->target - entry.target) / next_dur_s;
                cmd.has_next_chord = true;
            }
        }

        // No cross-core queue on the host — the engine IS right here. The
        // device counts sm_sync_enqueued/_dropped on the QUEUE handoff (a
        // 16-deep queue drained every 1 ms: effectively never full), so the
        // handoff always succeeds here too and the counters stay comparable.
        // A planner refusal is a DIFFERENT event and gets its own counter —
        // folding it into `dropped` made the sim look like it was losing wire
        // data when it was rejecting plans.
        ++_sync_enqueued;
        // Analyzer overlay: the WIRE's own normalized target, before the
        // window clamp the substep loop applies. Recorded on the accepted path
        // only — a sample the gates dropped above never became a command.
        _trace_cmd_norm = cmd.target;
        noteSenderCurve(cmd, now64);
        if (!_engine.commit(cmd, now64)) ++_plan_rejected;
    }
}

// Empties the engine's anomaly ring. Mirrors the firmware's Core-1 drain
// (src/main.cpp streamSamplerTask) down to the throttle interval, so the two
// logs read the same during a side-by-side session.
//
// THE RULE: counting is unconditional, logging is throttled. Anomalies arrive
// in bursts (one replan can emit several), and a 1 s throttle would otherwise
// report a 14-event cycle as a single line — which is exactly how this whole
// feed became invisible in the first place.
void MachineSim::drainAnomalies(uint64_t now64) {
    slopmotion::Anomaly ev;
    while (_engine.popAnomaly(ev)) {
        ++_anom_total;
        if (ev.kind < kSmAnomalyKinds) ++_anom_kind[ev.kind];

        // benchrig advertises NO motion-anomaly EVENT channel (no SlopMotion
        // tuning/diagnostics surface at all — see SlopSimCatalog.h) — this
        // machine doesn't expose planner internals over the wire, only the
        // resulting telemetry. The counters still accumulate for local
        // logging/bench diagnosis either way; only the wire publish is
        // profile-gated.
        if (_hasFullDeviceCatalog) {
            // 0x4100 motion-anomaly — the same body-map grammar as the
            // firmware's (slopdrive::anom_body): kind mirrored into both the
            // frame's event_kind AND body key 1 (the catalog's only mechanism
            // for LABELING an event kind via `options`).
            slopsync::EventMsg em{};
            em.channel_id = ch::motion_anomaly;
            em.timestamp = _clock.nowMs();
            em.event_kind = ev.kind;
            em.has_body = true;
            em.body_count = 0;
            em.body[em.body_count++] = {slopdrive::anom_body::kind,
                                        slopsync::IntentValue::ofU64(ev.kind)};
            em.body[em.body_count++] = {slopdrive::anom_body::seq,
                                        slopsync::IntentValue::ofU64(ev.seq)};
            em.body[em.body_count++] = {slopdrive::anom_body::target,
                                        slopsync::IntentValue::ofF32(ev.target)};
            em.body[em.body_count++] = {slopdrive::anom_body::detail,
                                        slopsync::IntentValue::ofF32(ev.detail)};
            em.body[em.body_count++] = {slopdrive::anom_body::t_us,
                                        slopsync::IntentValue::ofU64(uint32_t(ev.t_us & 0xFFFFFFFFull))};
            std::array<std::byte, 96> buf{};
            const size_t n = slopsync::encodeEvent(em, std::span<std::byte>(buf));
            if (n > 0)
                _hub.publishEvent(ch::motion_anomaly, std::span<const std::byte>(buf.data(), n));
        }

        const uint32_t nowMs = uint32_t(now64 / 1000);
        if (nowMs - _lastAnomLogMs >= 1000 || _lastAnomLogMs == 0) {
            _lastAnomLogMs = nowMs;
            // Unknown kinds name their ordinal ("?7"), never a bare "?" — a
            // stale name table should tell you the number to add.
            char nm[24];
            if (ev.kind < kSmAnomalyNameCount) {
                std::snprintf(nm, sizeof(nm), "%s", kSmAnomalyNames[ev.kind]);
            } else {
                std::snprintf(nm, sizeof(nm), "?%u", unsigned(ev.kind));
            }
            _log.logf('I', "motion: slopmotion %s target=%.3f detail=%.3f (total %u)", nm,
                      double(ev.target), double(ev.detail), unsigned(_anom_total));
        }
    }
}

void MachineSim::resetAnomalies() {
    _anom_total = 0;
    _anom_kind.fill(0);
    _lastAnomLogMs = 0;
    // RFC-019: bump LAST-ish but before publication, so no snapshot can carry
    // the new generation with the old totals. Observable to every subscriber
    // of 0x0088, not just to whoever pressed reset.
    ++_reset_gen;
    std::lock_guard<std::mutex> lk(_facadeM);
    _facade.anomalies = 0;
    _facade.anom_kind.fill(0);
}

// ---- Safety sync + telemetry ------------------------------------------------
// Transcriptions of the firmware service.

void MachineSim::syncSafety() {
    const bool hubLatched = _hub.estopLatched();
    if (_estop_latched && !hubLatched) {
        _hub.latchEstop(0, 0, ++_estopSeq);
    } else if (!_estop_latched && hubLatched) {
        _hub.clearEstop();
    }
    // RFC-025c: same both-directions reconciliation for the appended `modes`
    // byte — the TUI can toggle override/bypass without SlopSync ever hearing
    // about it, so the machine state is the source of truth and the hub
    // publishes only on a real change.
    _hub.setSafetyModes(_override, _bypass_limits);
}

void MachineSim::publishTelemetry(uint32_t nowMs) {
    if (!_realDeviceIds) {
        // ---- ALIEN (benchrig) -----------------------------------------------
        // Plain f32 shapes, deliberately NOT the real device's scaled
        // u16/i16 packing (proves a client isn't assuming a specific
        // numeric wire encoding). See SlopSimCatalog.h.

        // ---- 0x0090 telemetry — >=33 ms (<=30 Hz) ---------------------------
        if (nowMs - _lastMotionMs >= 33) {
            _lastMotionMs = nowMs;
            std::array<std::byte, 9> buf{};
            std::span<std::byte> s(buf);
            uint8_t flags = 0;
            if (_homed) flags |= 1u << 0;
            if (std::fabs(_stepper.velocityMmS()) > 0.5f) flags |= 1u << 1;  // moving
            slopsync::putF32(s.subspan(0, 4), _stepper.positionMm());
            slopsync::putF32(s.subspan(4, 4), _stepper.velocityMmS());
            slopsync::putU8(s.subspan(8, 1), flags);
            _hub.publishState(benchrig::ch::telemetry, s);
        }

        // ---- 0x0091 limits — on cfg_gen change OR machine-side edit ---------
        const uint16_t gen = _hub.cfgGen();
        if (!_cfgEverSent || gen != _lastCfgGen || _cfgDirty) {
            if (_cfgDirty) _hub.bumpConfigGeneration();
            _cfgEverSent = true;
            _lastCfgGen = _hub.cfgGen();
            _cfgDirty = false;
            std::array<std::byte, 21> buf{};   // 4*f32 + max_rail f32 + enabled_mask
            std::span<std::byte> s(buf);
            slopsync::putF32(s.subspan(0, 4), _win_min_mm);
            slopsync::putF32(s.subspan(4, 4), _win_max_mm);
            slopsync::putF32(s.subspan(8, 4), _user_speed);
            slopsync::putF32(s.subspan(12, 4), _user_accel);
            slopsync::putF32(s.subspan(16, 4), _max_rail_mm);
            slopsync::putU8(s.subspan(20, 1), 0x0F);
            _hub.publishState(benchrig::ch::limits, s);
        }

        // ---- 0x0092 device-settings — on change -----------------------------
        if (_warmup_mode != _lastWarmupMode || _device_label != _lastDeviceLabel || !_devSettingsSent) {
            _devSettingsSent = true;
            _lastWarmupMode = _warmup_mode;
            _lastDeviceLabel = _device_label;
            std::array<std::byte, 18> buf{};   // u8 + str16(16) + enabled_mask
            std::span<std::byte> s(buf);
            slopsync::putU8(s.subspan(0, 1), _warmup_mode);
            {
                const std::string_view label = _device_label;
                const size_t n = std::min<size_t>(label.size(), 16);
                if (n > 0) std::memcpy(s.data() + 1, label.data(), n);
                if (n < 16) std::memset(s.data() + 1 + n, 0, 16 - n);
            }
            slopsync::putU8(s.subspan(17, 1), 0x03);
            _hub.publishState(benchrig::ch::device_settings, s);
        }
    } else {
        // ---- DEVICE / MINIMAL -----------------------------------------------
        // Byte-identical to the real firmware's own publishers
        // (SlopSyncHubService.cpp), same scaled u16/i16 packing.

        // ---- 0x1100 motion — >=16 ms (<=60 Hz) ------------------------------
        if (nowMs - _lastMotionMs >= 16) {
            _lastMotionMs = nowMs;
            std::array<std::byte, 9> buf{};   // M5a: + raw_10um
            std::span<std::byte> s(buf);
            const uint16_t pos10 = clampU16(_stepper.positionMm() * 100.0f);
            const uint16_t tgt10 = clampU16(_commanded_target_mm * 100.0f);
            const uint16_t raw10 = clampU16(_commanded_raw_mm * 100.0f);
            const int16_t spd10 = clampI16(_stepper.velocityMmS() * 10.0f);
            uint8_t flags = 0;
            if (_homed) flags |= 1u << 0;
            if (_homing) flags |= 1u << 1;
            if (_pattern.running) flags |= 1u << 2;
            if (_paused) flags |= 1u << 3;
            if (_override) flags |= 1u << 4;
            if (_estop_latched) flags |= 1u << 5;
            if (_last_intiface_ms != 0 && (nowMs - _last_intiface_ms) < 250) flags |= 1u << 6;
            slopsync::putU16(s.subspan(0, 2), pos10);
            slopsync::putU16(s.subspan(2, 2), tgt10);
            slopsync::putU16(s.subspan(4, 2), uint16_t(spd10));
            slopsync::putU8(s.subspan(6, 1), flags);
            slopsync::putU16(s.subspan(7, 2), raw10);
            _hub.publishState(ch::motion, s);
        }

        if (_hasFullDeviceCatalog) {
            // ---- 0x1000 machine-config — on cfg_gen change OR machine edit -
            const uint16_t gen = _hub.cfgGen();
            if (!_cfgEverSent || gen != _lastCfgGen || _cfgDirty) {
                if (_cfgDirty) _hub.bumpConfigGeneration();
                _cfgEverSent = true;
                _lastCfgGen = _hub.cfgGen();
                _cfgDirty = false;
                std::array<std::byte, 37> buf{};   // 8*f32 + enabled_mask + measured_stroke
                std::span<std::byte> s(buf);
                slopsync::putF32(s.subspan(0, 4), _win_min_mm);
                slopsync::putF32(s.subspan(4, 4), _win_max_mm);
                slopsync::putF32(s.subspan(8, 4), _user_speed);
                slopsync::putF32(s.subspan(12, 4), _user_accel);
                slopsync::putF32(s.subspan(16, 4), _input_speed);
                slopsync::putF32(s.subspan(20, 4), _input_accel);
                slopsync::putF32(s.subspan(24, 4), _max_rail_mm);
                slopsync::putF32(s.subspan(28, 4), _input_jerk);
                // fw 2.1.76: max_rail joined the setting-annotated set (bit 6),
                // pushing input_jerk to bit 7 — all 8 bits spoken for, 0xFF.
                slopsync::putU8(s.subspan(32, 1), 0xFF);
                slopsync::putF32(s.subspan(33, 4), _measured_stroke_mm);
                _hub.publishState(ch::machine_config, s);
            }

            // ---- 0x1200 pattern-state — on change, >=100 ms -----------------
            {
                const uint8_t mask = uint8_t(((_estop_latched || !_homed) ? 0x00 : 0x3F) | 0x40);
                const bool changed = _pattern.running != _patRunning || _pattern.idx != _patIdx ||
                                     _pattern.speed != _patSpeed || _pattern.depth != _patDepth ||
                                     _pattern.stroke != _patStroke || _pattern.sensation != _patSensation ||
                                     mask != _patMask;
                if (changed && (nowMs - _lastPatternMs >= 100 || !_planEverSent)) {
                    _lastPatternMs = nowMs;
                    _patRunning = _pattern.running;
                    _patIdx = _pattern.idx;
                    _patSpeed = _pattern.speed;
                    _patDepth = _pattern.depth;
                    _patStroke = _pattern.stroke;
                    _patSensation = _pattern.sensation;
                    _patMask = mask;
                    std::array<std::byte, 20> buf{};   // Phase D: + background_run
                    std::span<std::byte> s(buf);
                    slopsync::putU8(s.subspan(0, 1), _pattern.running ? 1 : 0);
                    slopsync::putU8(s.subspan(1, 1), _pattern.idx);
                    slopsync::putF32(s.subspan(2, 4), _pattern.speed);
                    slopsync::putF32(s.subspan(6, 4), _pattern.depth);
                    slopsync::putF32(s.subspan(10, 4), _pattern.stroke);
                    slopsync::putF32(s.subspan(14, 4), _pattern.sensation);
                    slopsync::putU8(s.subspan(18, 1), mask);
                    slopsync::putU8(s.subspan(19, 1), _pattern.background_run ? 1 : 0);
                    _hub.publishState(ch::pattern_state, s);
                }
            }

            // ---- 0x1110 plan-strip — >=22 ms (<=45 Hz) ----------------------
            if (nowMs - _lastPlanMs >= 22) {
                _lastPlanMs = nowMs;
                const slopmotion::Snapshot d = _engine.snapshot(_clock.nowUs64());
                const bool active = d.duration_s > 0.0f;
                if (active || !_planEverSent) {
                    _planEverSent = true;
                    uint8_t flags = 0;
                    if (active) flags |= 1u << 0;
                    if (d.mode == uint8_t(slopmotion::Mode::Chase)) flags |= 1u << 1;
                    if (d.plan_kind == uint8_t(slopmotion::PlanKind::Quintic) ||
                        d.plan_kind == uint8_t(slopmotion::PlanKind::Cubic)) flags |= 1u << 2;
                    std::array<std::byte, 18> buf{};
                    std::span<std::byte> s(buf);
                    slopsync::putU8(s.subspan(0, 1), flags);
                    slopsync::putU8(s.subspan(1, 1), d.mode);
                    slopsync::putU16(s.subspan(2, 2), clampU16(d.start * 10000.0f));
                    slopsync::putU16(s.subspan(4, 2), clampU16(d.target * 10000.0f));
                    slopsync::putU16(s.subspan(6, 2), clampU16(d.pos * 10000.0f));
                    slopsync::putU16(s.subspan(8, 2), uint16_t(clampI16(d.vel * 1000.0f)));
                    slopsync::putU32(s.subspan(10, 4), uint32_t(d.duration_s * 1e6f));
                    slopsync::putU32(s.subspan(14, 4), uint32_t(d.elapsed_s * 1e6f));
                    _hub.publishState(ch::plan_strip, s);
                }
            }

            // ---- 0x1010 power — 2 Hz. MODELED, and labeled as such ----------
            if (nowMs - _lastPowerMs >= 500) {
                const float dt = _lastPowerMs == 0 ? 0.5f : float(nowMs - _lastPowerMs) / 1000.0f;
                _lastPowerMs = nowMs;
                const float speed = std::fabs(_stepper.velocityMmS());
                _bus_a = 0.15f + speed * 0.004f;
                if (_bus_a > _bus_peak_a) _bus_peak_a = _bus_a;
                _bus_v = 24.0f - _bus_a * 0.08f;
                const float target_c = 30.0f + _bus_a * 6.0f;
                _die_c += (target_c - _die_c) * 0.05f;
                _energy_wh += _bus_v * _bus_a * dt / 3600.0f;

                std::array<std::byte, 8> buf{};
                std::span<std::byte> s(buf);
                slopsync::putU16(s.subspan(0, 2), clampU16(_bus_v * 1000.0f));
                slopsync::putU16(s.subspan(2, 2), clampU16(_bus_peak_a * 1000.0f));
                slopsync::putU16(s.subspan(4, 2), uint16_t(clampI16(_bus_a * 1000.0f)));
                slopsync::putU16(s.subspan(6, 2), uint16_t(clampI16(_die_c * 10.0f)));
                _hub.publishState(ch::power, s);
            }

            // ---- 0x1030 machine-modes — on change ---------------------------
            // Byte-identical layout to the firmware's own publisher
            // (SlopSyncHubService.cpp): blend_mode_reserved stays a literal 0
            // (retired padding — no motor/blend concept exists here either),
            // enabled_mask is ALL-ALWAYS (0x03) for the same "honest publish,
            // not a stub" reason the firmware states — neither setter is ever
            // refused.
            {
                std::array<std::byte, 4> buf{};
                std::span<std::byte> s(buf);
                slopsync::putU8(s.subspan(0, 1), 0);  // blend_mode_reserved
                slopsync::putU8(s.subspan(1, 1), _stream_speed_mode);
                slopsync::putU8(s.subspan(2, 1), _overshoot_clamp ? 1u : 0u);
                slopsync::putU8(s.subspan(3, 1), 0x03u);
                if (!_modesEverSent || buf != _lastModes) {
                    _modesEverSent = true;
                    _lastModes = buf;
                    _hub.publishState(ch::machine_modes, s);
                }
            }

            // ---- 0x1120/1121/1122 slopmotion-limits/chase/waveform — on change
            // Byte-identical layout to the firmware's own publisher. Reads the
            // REAL engine Config directly (the same source sm-set just wrote),
            // never a shadow copy — same "one source of truth" rule the
            // firmware comment states for 0x008B/C/D.
            {
                const slopmotion::Config& cfg = _engine.config();

                std::array<std::byte, 18> lim{};
                std::span<std::byte> l(lim);
                slopsync::putF32(l.subspan(0, 4),  _jmax_norm);
                slopsync::putF32(l.subspan(4, 4),  _vmax_norm);
                slopsync::putF32(l.subspan(8, 4),  _amax_norm);
                slopsync::putU8 (l.subspan(12, 1), cfg.wave_centering ? 1u : 0u);
                slopsync::putF32(l.subspan(13, 4), cfg.wave_centering_gain);
                slopsync::putU8 (l.subspan(17, 1), 0x1Fu);
                if (!_smLimEverSent || lim != _lastSmLim) {
                    _smLimEverSent = true;
                    _lastSmLim = lim;
                    _hub.publishState(ch::sm_limits, l);
                }

                std::array<std::byte, 20> chc{};
                std::span<std::byte> h(chc);
                slopsync::putU8 (h.subspan(0, 1),  cfg.chase_feedforward ? 1u : 0u);
                slopsync::putU8 (h.subspan(1, 1),  cfg.chase_accel_ff ? 1u : 0u);
                slopsync::putF32(h.subspan(2, 4),  cfg.chase_ff_gain);
                slopsync::putF32(h.subspan(6, 4),  cfg.chase_lookahead);
                slopsync::putU32(h.subspan(10, 4), cfg.chase_dense_us);
                slopsync::putU8 (h.subspan(14, 1), cfg.chase_aim_accel_extrap ? 1u : 0u);
                slopsync::putF32(h.subspan(15, 4), cfg.handoff_chord_factor);
                slopsync::putU8 (h.subspan(19, 1), 0x7Fu);
                if (!_smChaseEverSent || chc != _lastSmChase) {
                    _smChaseEverSent = true;
                    _lastSmChase = chc;
                    _hub.publishState(ch::sm_chase, h);
                }

                std::array<std::byte, 21> wav{};
                std::span<std::byte> w(wav);
                slopsync::putU8 (w.subspan(0, 1),  uint8_t(cfg.curve_policy));
                slopsync::putU8 (w.subspan(1, 1),  uint8_t(cfg.infeasible_policy));
                slopsync::putF32(w.subspan(2, 4),  cfg.infeasible_scale_margin);
                slopsync::putF32(w.subspan(6, 4),  cfg.infeasible_smooth_budget);
                slopsync::putF32(w.subspan(10, 4), cfg.infeasible_amplitude_budget);
                slopsync::putU8 (w.subspan(14, 1), cfg.infeasible_blend_steps);
                slopsync::putU8 (w.subspan(15, 1), cfg.infeasible_reshape_steps);
                slopsync::putU32(w.subspan(16, 4), cfg.settle_grace_us);
                slopsync::putU8 (w.subspan(20, 1), 0xFFu);
                if (!_smWavEverSent || wav != _lastSmWav) {
                    _smWavEverSent = true;
                    _lastSmWav = wav;
                    _hub.publishState(ch::sm_waveform, w);
                }
            }

            // ---- 0x1210 pattern-advanced + 0x1211..1216 pattern-adv-mod-* ---
            // Byte-identical layout to the firmware's own publisher. Reads
            // `_ap` directly — the SAME struct pattern_advanced_cmd/preset
            // load write — never a shadow copy. Mask tracks e-stop alone (none
            // of these setters is gated on `homed` either — see the field
            // comment in SlopSyncCatalog.h).
            {
                const uint8_t mask = _estop_latched ? 0x00u : 0xFFu;

                std::array<std::byte, 9> base{};
                std::span<std::byte> b(base);
                slopsync::putU8(b.subspan(0, 1), _ap_mode ? 1u : 0u);
                slopsync::putU8(b.subspan(1, 1), _ap.master.value);
                slopsync::putU8(b.subspan(2, 1), _ap.max_depth.value);
                slopsync::putU8(b.subspan(3, 1), _ap.min_depth.value);
                slopsync::putU8(b.subspan(4, 1), _ap.in_speed.value);
                slopsync::putU8(b.subspan(5, 1), _ap.out_speed.value);
                slopsync::putU8(b.subspan(6, 1), _ap.in_accel.value);
                slopsync::putU8(b.subspan(7, 1), _ap.out_accel.value);
                slopsync::putU8(b.subspan(8, 1), mask);
                if (!_apBaseEverSent || base != _lastApBase) {
                    _apBaseEverSent = true;
                    _lastApBase = base;
                    _hub.publishState(ch::pattern_advanced, b);
                }

                // Indexed by advpat::BaseId, NOT by ascending channel id —
                // Phase C4 put the six modifier lanes in member order
                // speedin/out, accelin/out, depth1/2 (see the ch:: namespace
                // comment in SlopSyncCatalog.h), matching the firmware's own
                // kModChannels table exactly.
                static constexpr uint16_t kModChannels[kApBaseCount] = {
                    ch::pattern_adv_mod_depth1, ch::pattern_adv_mod_depth2, ch::pattern_adv_mod_speedin,
                    ch::pattern_adv_mod_speedout, ch::pattern_adv_mod_accelin, ch::pattern_adv_mod_accelout,
                };
                for (uint8_t id = 0; id < kApBaseCount; ++id) {
                    const advpat::BaseControl* bc = _ap.byId(id);
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

            // ---- 0x1220 pattern-presets-roster — on change, generation-diffed
            // Bare {generation,count,capacity}, same shape as the firmware's
            // publishPresetRoster().
            {
                if (!_presetRosterEverSent || _presets.generation() != _lastPresetGen) {
                    _lastPresetGen = _presets.generation();
                    _presetRosterEverSent = true;
                    std::array<std::byte, 4> buf{};
                    std::span<std::byte> s(buf);
                    slopsync::putU16(s.subspan(0, 2), _presets.generation());
                    slopsync::putU8(s.subspan(2, 1), _presets.count());
                    slopsync::putU8(s.subspan(3, 1), slopdrive::PatternPresetStore::kCapacity);
                    _hub.publishState(ch::pattern_presets_roster, s);
                }
            }
        }
    }

    if (_hasFullDeviceCatalog && (nowMs - _lastSlowMs >= 1000)) {
        // ---- 0x1020 odometer ------------------------------------------------
        {
            std::array<std::byte, 20> buf{};
            std::span<std::byte> s(buf);
            slopsync::putU32(s.subspan(0, 4), _strokes);
            slopsync::putF32(s.subspan(4, 4), _distance_mm / 1000.0f);
            slopsync::putF32(s.subspan(8, 4), _peak_mm_s);
            slopsync::putF32(s.subspan(12, 4), _energy_wh);
            slopsync::putU32(s.subspan(16, 4), nowMs - _sessionStartMs);
            _hub.publishState(ch::odometer, s);
        }
        // ---- 0x1111 slopmotion-diag — the /api/slopmotion stats+sync blocks
        {
            const slopmotion::Snapshot d = _engine.snapshot(_clock.nowUs64());
            std::array<std::byte, 88> buf{};   // slopmotion 0.8.0: + waveform_smoothed
            std::span<std::byte> s(buf);
            slopsync::putU32(s.subspan(0, 4), d.plans);
            slopsync::putU32(s.subspan(4, 4), d.failures);
            slopsync::putU32(s.subspan(8, 4), _anom_total);
            slopsync::putU8(s.subspan(12, 1), d.mode);
            slopsync::putU8(s.subspan(13, 1), d.plan_kind);
            for (size_t k = 0; k < kSmAnomalyKinds; ++k)
                slopsync::putU32(s.subspan(14 + k * 4, 4), _anom_kind[k]);
            // No honest host analog of the xtensa plan-time bench; zero rather
            // than invent a number an operator might compare.
            slopsync::putU32(s.subspan(54, 4), 0);
            slopsync::putU32(s.subspan(58, 4), 0);
            slopsync::putF32(s.subspan(62, 4), 0.0f);
            slopsync::putU32(s.subspan(66, 4), _sync_bundles);
            slopsync::putU32(s.subspan(70, 4), _sync_samples);
            slopsync::putU32(s.subspan(74, 4), _sync_enqueued);
            slopsync::putU32(s.subspan(78, 4), _sync_dropped);
            slopsync::putU32(s.subspan(82, 4), _sync_seg_bundles);
            slopsync::putU16(s.subspan(86, 2), _reset_gen);
            _hub.publishState(ch::motion_diag, s);
        }
    }

    // ---- 1 Hz block: 0x0006 hub-status (SPEC-CORE; every profile) -----------
    if (nowMs - _lastSlowMs >= 1000) {
        _lastSlowMs = nowMs;
        std::array<std::byte, 14> buf{};  // 14 B since M5b: log_dropped appended
        std::span<std::byte> s(buf);
        slopsync::putU32(s.subspan(0, 4), 4u * 1024u * 1024u);  // "heap": a host fiction
        slopsync::putU32(s.subspan(4, 4), nowMs / 1000u);
        slopsync::putU8(s.subspan(8, 1), uint8_t(int8_t(-42)));  // "rssi": wired-grade
        slopsync::putU8(s.subspan(9, 1), uint8_t(_hub.sessionCount()));
        slopsync::putU32(s.subspan(10, 4), _hub.logDropped());
        _hub.publishState(slopsync::channels::hub_status, s);
    }
}

// ---- HubDelegate ------------------------------------------------------------
// Transcription of SlopDriveHubDelegate with the model applied directly (no
// WebUI::handleCommand on the host; the gates/clamps/echoes are kept
// semantically identical).

slopsync::AccessLevel MachineSim::validateToken(std::span<const std::byte> instance_id,
                                                std::span<const std::byte> token, bool hasToken) {
    // Rung 1: the hub's own trust ledger — REAL, so knock-and-approve round
    // trips are testable in-sim (the device's rung 2; there is no /uitoken
    // sideband here). validate() is the same constant-time lookup the
    // firmware consults.
    if (hasToken) {
        const auto role = _hub.pairing().validate(instance_id, token);
        if (role > slopsync::AccessLevel::watch) return role;
    }
    // Sim convenience floor: bare sessions get `control` (parity with the
    // device's /uitoken-on-LAN posture) — but NEVER `configure`. Configure
    // must be EARNED through pairing, or the session-admin surface's tier
    // gate would be untestable here.
    return slopsync::AccessLevel::control;
}

slopsync::Result<IntentValueMap, NackCode> MachineSim::applyIntent(uint16_t channel_id,
                                                                   const IntentValueMap& requested,
                                                                   slopsync::AccessLevel /*role*/,
                                                                   bool& cfgChanged) {
    using Ret = slopsync::Result<IntentValueMap, NackCode>;
    IntentValueMap applied{};

    switch (channel_id) {
        // benchrig::ch::move and the real device's ch::move (0x3100) are the
        // SAME shape (key 1 position f32, key 2 bypass bool) and the SAME
        // semantics — one body serves both profiles.
        case benchrig::ch::move:
        case ch::move: {
            if (_estop_latched) return Ret::err(NackCode::ESTOP_ACTIVE);
            if (!_homed) return Ret::err(NackCode::NOT_HOMED);
            // Per-request bypass wins; otherwise honor the STANDING bypass
            // mode (RFC-025c), exactly like WebUI::applyMove does — otherwise
            // the 0x0003 `modes` byte would advertise a bypass the sim never
            // actually applied.
            const bool bypass = fieldBool(findField(requested, 2), _bypass_limits);
            float mm = fieldF32(findField(requested, 1), 0.0f);
            mm = bypass ? clampf(mm, 0.0f, _max_rail_mm) : clampf(mm, _win_min_mm, _win_max_mm);

            slopmotion::Command cmd;  // no duration -> chase point move
            cmd.target = clampf(mmToNorm(mm), 0.0f, 1.0f);
            _commanded_raw_mm = mm;   // 0x0080 raw_10um: the asked-for point
            _trace_cmd_norm = cmd.target;   // analyzer overlay (post-clamp here:
                                            // 0x0100 speaks mm, and the clamp is
                                            // what the machine agreed to do)
            _engine.commit(cmd, _clock.nowUs64());

            applied.count = 2;
            applied.fields[0] = {1, IntentValue::ofF32(mm)};
            applied.fields[1] = {2, IntentValue::ofBool(bypass)};
            return Ret::ok(applied);
        }

        // benchrig has NO input (machine-driven) limit set and NO jerk setting
        // (see SlopSimCatalog.h) — this replaces the real device's config_set
        // (0x0101) with a smaller writer that only ever touches the window and
        // the user limit set. _input_speed/_input_accel/_input_jerk stay fixed
        // firmware constants (bench-only via the TUI's `speed.input` etc.),
        // never reachable from this switch.
        case benchrig::ch::limits_set: {
            const auto* f1 = findField(requested, 1);
            const auto* f2 = findField(requested, 2);
            const auto* f3 = findField(requested, 3);
            const auto* f4 = findField(requested, 4);

            // Window legality mirrors the device path exactly: refuse min>=max
            // on the RAW request (INVALID_VALUE), then RangeMapper::setRange
            // clamps to the rail and enforces the 5 mm minimum span. Echo is
            // post-clamp (ground truth).
            const float wmin = f1 ? fieldF32(f1, _win_min_mm) : _win_min_mm;
            const float wmax = f2 ? fieldF32(f2, _win_max_mm) : _win_max_mm;
            if (wmin >= wmax) return Ret::err(NackCode::INVALID_VALUE);

            // RFC-002: cfg_gen advances IFF an applied value actually CHANGED.
            const float p0 = _win_min_mm, p1 = _win_max_mm, p2 = _user_speed, p3 = _user_accel;

            // Clamp to the CATALOG's own declared bounds (benchrig::ceiling),
            // not the wider firmware-parity kSpeedCeiling/kAccelCeiling those
            // constants exist for (the TUI's bench-only _input_* overrides) —
            // 0x0091 advertises max=3000/20000, so the wire delegate must
            // actually enforce that or the catalog would be lying.
            applyWindowLegality(wmin, wmax);
            if (f3) _user_speed = float(lroundf(clampf(fieldF32(f3, _user_speed), 1.0f, benchrig::ceiling::speed_max)));
            if (f4) _user_accel = float(lroundf(clampf(fieldF32(f4, _user_accel), 1.0f, benchrig::ceiling::accel_max)));
            deriveEngineLimits();
            cfgChanged = (p0 != _win_min_mm) || (p1 != _win_max_mm) || (p2 != _user_speed) ||
                         (p3 != _user_accel);

            uint32_t n = 0;
            if (f1) applied.fields[n++] = {1, IntentValue::ofF32(_win_min_mm)};
            if (f2) applied.fields[n++] = {2, IntentValue::ofF32(_win_max_mm)};
            if (f3) applied.fields[n++] = {3, IntentValue::ofF32(_user_speed)};
            if (f4) applied.fields[n++] = {4, IntentValue::ofF32(_user_accel)};
            applied.count = n;
            return Ret::ok(applied);
        }

        // The setting the real device does not have (0x0092's writer). Proves
        // a brand-new select+str16 pair round-trips with zero client changes.
        // Session-volatile, like the real device's mode settings (0x0104) —
        // not part of the safety envelope, so it does not bump cfg_gen.
        case benchrig::ch::device_set: {
            const auto* f1 = findField(requested, 1);
            const auto* f2 = findField(requested, 2);

            if (f1) _warmup_mode = uint8_t(std::min<uint64_t>(fieldU64(f1, _warmup_mode), 2));
            if (f2) {
                std::string_view sv = fieldTstr(f2, _device_label);
                if (sv.size() > 16) sv = sv.substr(0, 16);  // str16 wire width (RFC-026)
                _device_label.assign(sv);
            }
            cfgChanged = false;

            uint32_t n = 0;
            if (f1) applied.fields[n++] = {1, IntentValue::ofU64(_warmup_mode)};
            if (f2) applied.fields[n++] = {2, IntentValue::ofTstr(_device_label)};
            applied.count = n;
            return Ret::ok(applied);
        }

        // Device-profile config_set (0x3000) — the real device's SEVEN-key
        // writer: window + BOTH limit sets (user AND input) + jerk. benchrig
        // has no equivalent (see ch::limits_set above, a 4-key subset).
        case ch::config_set: {
            const auto* f1 = findField(requested, 1);
            const auto* f2 = findField(requested, 2);
            const auto* f3 = findField(requested, 3);
            const auto* f4 = findField(requested, 4);
            const auto* f5 = findField(requested, 5);
            const auto* f6 = findField(requested, 6);
            const auto* f7 = findField(requested, 7);

            const float wmin = f1 ? fieldF32(f1, _win_min_mm) : _win_min_mm;
            const float wmax = f2 ? fieldF32(f2, _win_max_mm) : _win_max_mm;
            if (wmin >= wmax) return Ret::err(NackCode::INVALID_VALUE);

            // RFC-002: cfg_gen advances IFF an applied value actually CHANGED.
            const float p0 = _win_min_mm, p1 = _win_max_mm, p2 = _user_speed, p3 = _user_accel;
            const float p4 = _input_speed, p5 = _input_accel, p6 = _input_jerk;

            applyWindowLegality(wmin, wmax);
            if (f3) _user_speed = float(lroundf(clampf(fieldF32(f3, _user_speed), 1.0f, kSpeedCeiling)));
            if (f4) _user_accel = float(lroundf(clampf(fieldF32(f4, _user_accel), 1.0f, kAccelCeiling)));
            if (f5) _input_speed = float(lroundf(clampf(fieldF32(f5, _input_speed), 1.0f, kSpeedCeiling)));
            if (f6) _input_accel = float(lroundf(clampf(fieldF32(f6, _input_accel), 1.0f, kAccelCeiling)));
            if (f7) _input_jerk = float(lroundf(clampf(fieldF32(f7, _input_jerk), 1.0f, kJerkCeiling)));
            deriveEngineLimits();
            cfgChanged = (p0 != _win_min_mm) || (p1 != _win_max_mm) || (p2 != _user_speed) ||
                         (p3 != _user_accel) || (p4 != _input_speed) || (p5 != _input_accel) ||
                         (p6 != _input_jerk);

            uint32_t n = 0;
            if (f1) applied.fields[n++] = {1, IntentValue::ofF32(_win_min_mm)};
            if (f2) applied.fields[n++] = {2, IntentValue::ofF32(_win_max_mm)};
            if (f3) applied.fields[n++] = {3, IntentValue::ofF32(_user_speed)};
            if (f4) applied.fields[n++] = {4, IntentValue::ofF32(_user_accel)};
            if (f5) applied.fields[n++] = {5, IntentValue::ofF32(_input_speed)};
            if (f6) applied.fields[n++] = {6, IntentValue::ofF32(_input_accel)};
            if (f7) applied.fields[n++] = {7, IntentValue::ofF32(_input_jerk)};
            applied.count = n;
            return Ret::ok(applied);
        }

        // Device-profile pattern-cmd (0x3200) — the 6 stand-in generator
        // params PLUS Phase D's background_run (key 7). Not advertised at all
        // on Minimal (subset catalog) or Alien (no pattern channel).
        case ch::pattern_cmd: {
            if (_estop_latched) return Ret::err(NackCode::ESTOP_ACTIVE);
            const auto* f1 = findField(requested, 1);
            const auto* f2 = findField(requested, 2);
            const auto* f3 = findField(requested, 3);
            const auto* f4 = findField(requested, 4);
            const auto* f5 = findField(requested, 5);
            const auto* f6 = findField(requested, 6);
            const auto* f7 = findField(requested, 7);

            const bool wantRunning = fieldBool(f1, _pattern.running);
            if (wantRunning && !_homed) return Ret::err(NackCode::NOT_HOMED);

            if (f2) _pattern.idx = uint8_t(std::min<uint64_t>(fieldU64(f2, 0), 6));
            if (f3) _pattern.speed = clampf(fieldF32(f3, _pattern.speed), 0.0f, 100.0f);
            if (f4) _pattern.depth = clampf(fieldF32(f4, _pattern.depth), 0.0f, 100.0f);
            if (f5) _pattern.stroke = clampf(fieldF32(f5, _pattern.stroke), 0.0f, 100.0f);
            if (f6) _pattern.sensation = clampf(fieldF32(f6, _pattern.sensation), -100.0f, 100.0f);
            if (f7) _pattern.background_run = fieldBool(f7, _pattern.background_run);
            if (f1) {
                _pattern.running = wantRunning;
                _pattern.next_due_us = 0;  // next tickPattern emits immediately
                // Stopping a pattern does NOT stop the motor on the device:
                // PatternEngine::stop() only clears its own flags, so the leg
                // already submitted to the arbiter runs to completion.
            }
            // background_run (key 7) is a standing NVS-persisted policy on the
            // device — session-volatile here (sim restart resets it, like every
            // other bench default), so it does NOT bump cfg_gen either.
            cfgChanged = false;

            uint32_t n = 0;
            if (f1) applied.fields[n++] = {1, IntentValue::ofBool(_pattern.running)};
            if (f2) applied.fields[n++] = {2, IntentValue::ofU64(_pattern.idx)};
            if (f3) applied.fields[n++] = {3, IntentValue::ofF32(_pattern.speed)};
            if (f4) applied.fields[n++] = {4, IntentValue::ofF32(_pattern.depth)};
            if (f5) applied.fields[n++] = {5, IntentValue::ofF32(_pattern.stroke)};
            if (f6) applied.fields[n++] = {6, IntentValue::ofF32(_pattern.sensation)};
            if (f7) applied.fields[n++] = {7, IntentValue::ofBool(_pattern.background_run)};
            applied.count = n;
            return Ret::ok(applied);
        }

        // benchrig::ch::home and the real device's ch::home (0x3101) share the
        // SAME three bench ops (home / force_home / clear_override) — one body.
        case benchrig::ch::home:
        case ch::home: {
            const uint64_t op = fieldU64(findField(requested, 1), 0);
            switch (op) {
                case 1:
                    if (_estop_latched) return Ret::err(NackCode::ESTOP_ACTIVE);
                    startHoming();
                    applied.count = 1;
                    applied.fields[0] = {1, IntentValue::ofU64(1)};
                    return Ret::ok(applied);

                case 2: {
                    // force_home {stroke} — BENCH op (RFC-025). *** IT CLEARS
                    // THE E-STOP LATCH ***, which is why it is deliberately NOT
                    // gated on !_estop_latched: gating it would make it useless
                    // for the one job it exists to do. Transcribes the device's
                    // WS_OP_HOME_OVERRIDE `on:true` branch (WebUI.cpp): homed
                    // asserted with an UNMEASURED stroke, soft-start re-armed.
                    float stroke = fieldF32(findField(requested, 2), 250.0f);
                    if (!(stroke >= 1.0f)) stroke = 250.0f;  // also catches NaN
                    _estop_latched = false;
                    _homing = false;
                    _homed = true;
                    _max_rail_mm = stroke;
                    applyWindowLegality(_win_min_mm, _win_max_mm);
                    _resume_start_ms = uint32_t(_clock.nowUs64() / 1000);
                    deriveEngineLimits();
                    _log.logf('W', "sim: BENCH force_home — homed asserted, stroke %.1f mm, e-stop cleared",
                              double(stroke));
                    applied.count = 2;
                    applied.fields[0] = {1, IntentValue::ofU64(2)};
                    applied.fields[1] = {2, IntentValue::ofF32(stroke)};
                    return Ret::ok(applied);
                }

                case 3:
                    forceUnhome();
                    _log.logf('I', "sim: BENCH clear_override — back to real homing");
                    applied.count = 1;
                    applied.fields[0] = {1, IntentValue::ofU64(3)};
                    return Ret::ok(applied);

                default:
                    return Ret::err(NackCode::UNSUPPORTED_OP);
            }
        }

        case slopsync::channels::safety_intents: {
            const uint64_t op = fieldU64(findField(requested, 1), 0);
            switch (op) {
                case slopsync::safety_ops::stop:
                case slopsync::safety_ops::hold:
                    hardStopMotion();
                    break;
                case slopsync::safety_ops::pause:
                    _paused = true;
                    break;
                case slopsync::safety_ops::resume:
                    _paused = false;
                    _resume_start_ms = uint32_t(_clock.nowUs64() / 1000);
                    break;
                // RFC-025c: override/bypass are safety-domain state. The HUB
                // latches the resulting bits into 0x0003 on our acceptance;
                // syncSafety() below then keeps them true to the machine even
                // when the TUI toggled them locally.
                case slopsync::safety_ops::override_on:
                case slopsync::safety_ops::override_off: {
                    const bool was = _override;
                    _override = (op == slopsync::safety_ops::override_on);
                    if (was && !_override) _resume_start_ms = uint32_t(_clock.nowUs64() / 1000);
                    break;
                }
                case slopsync::safety_ops::bypass_on:
                case slopsync::safety_ops::bypass_off:
                    _bypass_limits = (op == slopsync::safety_ops::bypass_on);
                    break;
                default:
                    return Ret::err(NackCode::UNSUPPORTED_OP);
            }
            applied.count = 1;
            applied.fields[0] = {1, IntentValue::ofU64(op)};
            return Ret::ok(applied);
        }

        // ---- modes-set (0x3030) -> machine-modes (0x1030) -------------------
        // Mirrors SlopDriveHubDelegate's ch::modes_set case: keys 1/2 (retired
        // blend_mode/transport) are PERMANENT GAPS on the firmware too, so a
        // request touching ONLY those falls through unhandled here as well —
        // same NACK. Echo is read back from the applied sim state, not the
        // request, same Ground Truth rule the firmware comment states.
        case ch::modes_set: {
            applied.count = 0;
            bool anyApplied = false;
            if (const auto* f = findField(requested, 3)) {   // stream_speed_mode
                const uint8_t applied_mode = uiSetStreamSpeedMode(uint8_t(fieldU64(f, _stream_speed_mode)));
                applied.fields[applied.count++] = {3, IntentValue::ofU64(applied_mode)};
                anyApplied = true;
            }
            if (const auto* f = findField(requested, 4)) {   // overshoot_clamp
                _overshoot_clamp = fieldU64(f, _overshoot_clamp ? 1 : 0) != 0;
                applied.fields[applied.count++] = {4, IntentValue::ofU64(_overshoot_clamp ? 1u : 0u)};
                anyApplied = true;
            }
            if (!anyApplied) return Ret::err(NackCode::INVALID_VALUE);
            return Ret::ok(applied);
        }

        // ---- sm-set (0x3120) -> slopmotion-limits/chase/waveform ------------
        // Mirrors SlopDriveHubDelegate's ch::sm_set case: 20 keys, every one
        // optional, clamped to the SAME bounds the catalog advertises, echoed
        // post-clamp. jmax/vmax/amax overrides are sim-held (see the field
        // comments in MachineSim.h) and rebuild the engine's Limits via
        // deriveEngineLimits(); every other key writes straight into the REAL
        // engine's Config — one read-modify-write, one setConfig() call, so
        // the tuning takes effect on the very next plan, same as the
        // firmware's per-tick config push (main.cpp) does.
        case ch::sm_set: {
            applied.count = 0;
            bool any = false;
            bool touchedLimits = false;
            bool touchedCfg = false;
            slopmotion::Config cfg = _engine.config();

            auto setNorm = [&](uint8_t key, float lo, float hi, float& dst) {
                if (const auto* f = findField(requested, key)) {
                    dst = clampf(fieldF32(f, dst), lo, hi);
                    applied.fields[applied.count++] = {key, IntentValue::ofF32(dst)};
                    any = true;
                    touchedLimits = true;
                }
            };
            setNorm(1, 0.0f, 2000000.0f, _jmax_norm);
            setNorm(2, 0.0f, 20.0f,      _vmax_norm);
            setNorm(3, 0.0f, 500.0f,     _amax_norm);

            if (const auto* f = findField(requested, 4)) {   // centering
                cfg.wave_centering = fieldU64(f, cfg.wave_centering ? 1 : 0) != 0;
                applied.fields[applied.count++] = {4, IntentValue::ofU64(cfg.wave_centering ? 1u : 0u)};
                any = true; touchedCfg = true;
            }
            if (const auto* f = findField(requested, 5)) {   // centering_gain
                cfg.wave_centering_gain = clampf(fieldF32(f, cfg.wave_centering_gain), 0.0f, 1.0f);
                applied.fields[applied.count++] = {5, IntentValue::ofF32(cfg.wave_centering_gain)};
                any = true; touchedCfg = true;
            }
            if (const auto* f = findField(requested, 6)) {   // chase_ff
                cfg.chase_feedforward = fieldU64(f, cfg.chase_feedforward ? 1 : 0) != 0;
                applied.fields[applied.count++] = {6, IntentValue::ofU64(cfg.chase_feedforward ? 1u : 0u)};
                any = true; touchedCfg = true;
            }
            if (const auto* f = findField(requested, 7)) {   // chase_accel_ff
                cfg.chase_accel_ff = fieldU64(f, cfg.chase_accel_ff ? 1 : 0) != 0;
                applied.fields[applied.count++] = {7, IntentValue::ofU64(cfg.chase_accel_ff ? 1u : 0u)};
                any = true; touchedCfg = true;
            }
            if (const auto* f = findField(requested, 8)) {   // chase_gain
                cfg.chase_ff_gain = clampf(fieldF32(f, cfg.chase_ff_gain), 0.0f, 1.5f);
                applied.fields[applied.count++] = {8, IntentValue::ofF32(cfg.chase_ff_gain)};
                any = true; touchedCfg = true;
            }
            if (const auto* f = findField(requested, 9)) {   // chase_lookahead
                cfg.chase_lookahead = clampf(fieldF32(f, cfg.chase_lookahead), 0.0f, 8.0f);
                applied.fields[applied.count++] = {9, IntentValue::ofF32(cfg.chase_lookahead)};
                any = true; touchedCfg = true;
            }
            if (const auto* f = findField(requested, 10)) {  // chase_dense_ms (wire ms, engine us)
                const float ms = clampf(fieldF32(f, float(cfg.chase_dense_us) / 1000.0f), 10.0f, 500.0f);
                cfg.chase_dense_us = uint32_t(ms * 1000.0f);
                applied.fields[applied.count++] = {10, IntentValue::ofF32(ms)};
                any = true; touchedCfg = true;
            }
            if (const auto* f = findField(requested, 11)) {  // chase_aim_extrap
                cfg.chase_aim_accel_extrap = fieldU64(f, cfg.chase_aim_accel_extrap ? 1 : 0) != 0;
                applied.fields[applied.count++] = {11, IntentValue::ofU64(cfg.chase_aim_accel_extrap ? 1u : 0u)};
                any = true; touchedCfg = true;
            }
            if (const auto* f = findField(requested, 12)) {  // handoff_k
                cfg.handoff_chord_factor = clampf(fieldF32(f, cfg.handoff_chord_factor), 0.0f, 8.0f);
                applied.fields[applied.count++] = {12, IntentValue::ofF32(cfg.handoff_chord_factor)};
                any = true; touchedCfg = true;
            }
            if (const auto* f = findField(requested, 13)) {  // curve_policy
                uint64_t v = fieldU64(f, uint64_t(cfg.curve_policy));
                if (v > 2) v = 2;
                cfg.curve_policy = slopmotion::CurvePolicy(v);
                applied.fields[applied.count++] = {13, IntentValue::ofU64(v)};
                any = true; touchedCfg = true;
            }
            if (const auto* f = findField(requested, 14)) {  // infeasible_policy
                uint64_t v = fieldU64(f, uint64_t(cfg.infeasible_policy));
                if (v > 4) v = 4;
                cfg.infeasible_policy = slopmotion::InfeasiblePolicy(v);
                applied.fields[applied.count++] = {14, IntentValue::ofU64(v)};
                any = true; touchedCfg = true;
            }
            if (const auto* f = findField(requested, 15)) {  // infeasible_margin
                cfg.infeasible_scale_margin = clampf(fieldF32(f, cfg.infeasible_scale_margin), 0.5f, 1.0f);
                applied.fields[applied.count++] = {15, IntentValue::ofF32(cfg.infeasible_scale_margin)};
                any = true; touchedCfg = true;
            }
            if (const auto* f = findField(requested, 16)) {  // smooth_budget
                cfg.infeasible_smooth_budget = clampf(fieldF32(f, cfg.infeasible_smooth_budget), 0.0f, 1.0f);
                applied.fields[applied.count++] = {16, IntentValue::ofF32(cfg.infeasible_smooth_budget)};
                any = true; touchedCfg = true;
            }
            if (const auto* f = findField(requested, 17)) {  // amplitude_budget
                cfg.infeasible_amplitude_budget = clampf(fieldF32(f, cfg.infeasible_amplitude_budget), 0.0f, 1.0f);
                applied.fields[applied.count++] = {17, IntentValue::ofF32(cfg.infeasible_amplitude_budget)};
                any = true; touchedCfg = true;
            }
            if (const auto* f = findField(requested, 18)) {  // blend_steps
                uint64_t v = fieldU64(f, cfg.infeasible_blend_steps);
                if (v < 1) v = 1;
                if (v > 10) v = 10;
                cfg.infeasible_blend_steps = uint8_t(v);
                applied.fields[applied.count++] = {18, IntentValue::ofU64(v)};
                any = true; touchedCfg = true;
            }
            if (const auto* f = findField(requested, 19)) {  // reshape_steps
                uint64_t v = fieldU64(f, cfg.infeasible_reshape_steps);
                if (v > 8) v = 8;
                cfg.infeasible_reshape_steps = uint8_t(v);
                applied.fields[applied.count++] = {19, IntentValue::ofU64(v)};
                any = true; touchedCfg = true;
            }
            if (const auto* f = findField(requested, 20)) {  // settle_grace_ms (wire ms, engine us)
                const float ms = clampf(fieldF32(f, float(cfg.settle_grace_us) / 1000.0f), 0.0f, 200.0f);
                cfg.settle_grace_us = uint32_t(ms * 1000.0f);
                applied.fields[applied.count++] = {20, IntentValue::ofF32(ms)};
                any = true; touchedCfg = true;
            }

            if (touchedCfg) _engine.setConfig(cfg);
            if (touchedLimits) deriveEngineLimits();
            if (!any) return Ret::err(NackCode::INVALID_VALUE);
            return Ret::ok(applied);
        }

        // ---- machine-admin (0x30F0) -----------------------------------------
        // ONE-WAY PARITY judgment call, flagged for the operator: the sim has
        // NO fault/servo-Modbus concept at all (grepped clean across
        // sim/slopsim/src), so these three ops get the closest HONEST
        // equivalent rather than invented behavior. clear_fault/save_config
        // are genuine no-ops here (nothing to clear, nothing persists — the
        // sim's session-volatile convention throughout this class); servo_scan
        // never NACKs INTERLOCK the way the firmware's async Modbus path can,
        // because the sim models no interlock condition to refuse it with.
        case ch::machine_admin: {
            const uint64_t op = fieldU64(findField(requested, 1), 0);
            switch (op) {
                case 1:  // clear_fault — no fault state modeled; accepted no-op
                    _log.logf('I', "sim: machine-admin clear_fault (no-op, no fault modeled)");
                    break;
                case 2:  // save_config — nothing persists in the sim; accepted no-op
                    _log.logf('I', "sim: machine-admin save_config (no-op, sim does not persist)");
                    break;
                case 3:  // servo_scan — no Modbus/servo seam to scan; accepted no-op
                    _log.logf('I', "sim: machine-admin servo_scan (no-op, no servo modeled)");
                    break;
                default:
                    return Ret::err(NackCode::UNSUPPORTED_OP);
            }
            applied.count = 1;
            applied.fields[0] = {1, IntentValue::ofU64(op)};
            return Ret::ok(applied);
        }

        // ---- pattern-advanced-cmd (0x3210) -> pattern-advanced + 6 mod lanes -
        // Mirrors SlopDriveHubDelegate's ch::pattern_advanced_cmd case: key 1
        // ap_mode, key 2 master (BaseControl::set() direct), keys 3-8 the
        // depth/speed/accel base controls in advpat::BaseId order (depth pair
        // re-coupled on write via setApBase()), keys 9..44 the six modifier
        // lanes (base = 9 + 6*id, sub-offsets amplitude/in_step/in_wait/
        // out_step/out_wait/offset). Echo is Ground Truth for every control
        // this call actually touched, never invented for the rest — same rule
        // the firmware's touched[] array enforces.
        //
        // PHYSICS LIMITATION (flagged): see the `_ap`/SimPattern comment in
        // MachineSim.h — this wire contract is real (clamp/echo/publish), but
        // nothing here yet drives the stepper from `_ap`.
        case ch::pattern_advanced_cmd: {
            if (_estop_latched) return Ret::err(NackCode::ESTOP_ACTIVE);

            if (const auto* f = findField(requested, 1)) {
                _ap_mode = fieldBool(f, _ap_mode);
                applied.fields[applied.count++] = {1, IntentValue::ofBool(_ap_mode)};
            }
            if (const auto* f = findField(requested, 2)) {
                _ap.master.set(int(fieldU64(f, _ap.master.value)));
                applied.fields[applied.count++] = {2, IntentValue::ofU64(_ap.master.value)};
            }
            // Key = 3 + advpat::BaseId: 3 max_depth, 4 min_depth, 5 in_speed,
            // 6 out_speed, 7 in_accel, 8 out_accel.
            for (uint8_t id = 0; id < kApBaseCount; ++id) {
                const uint8_t key = uint8_t(3 + id);
                if (const auto* f = findField(requested, key)) {
                    setApBase(_ap, id, int(fieldU64(f, _ap.byId(id)->value)));
                    applied.fields[applied.count++] = {key, IntentValue::ofU64(_ap.byId(id)->value)};
                }
            }
            // Modifier cycles: keys 9..44, base = 9 + 6*id. Only rewrite (and
            // only echo) a control whose modifier this request actually
            // touched — the rest keep whatever they already held, read back
            // from the engine state, never invented.
            for (uint8_t id = 0; id < kApBaseCount; ++id) {
                const uint8_t base = uint8_t(9 + 6 * id);
                const auto* fa  = findField(requested, uint8_t(base + 0));
                const auto* fis = findField(requested, uint8_t(base + 1));
                const auto* fiw = findField(requested, uint8_t(base + 2));
                const auto* fos = findField(requested, uint8_t(base + 3));
                const auto* fow = findField(requested, uint8_t(base + 4));
                const auto* fof = findField(requested, uint8_t(base + 5));
                if (!fa && !fis && !fiw && !fos && !fow && !fof) continue;
                const advpat::Modifier& m = _ap.byId(id)->modifier;
                setApModifier(_ap, id,
                              int(fieldU64(fa,  m.amplitude)),
                              int(fieldU64(fis, m.in_step)),
                              int(fieldU64(fiw, m.in_wait)),
                              int(fieldU64(fos, m.out_step)),
                              int(fieldU64(fow, m.out_wait)),
                              int(fieldU64(fof, m.offset)));
                const advpat::Modifier& out = _ap.byId(id)->modifier;
                if (fa)  applied.fields[applied.count++] = {uint8_t(base + 0), IntentValue::ofU64(out.amplitude)};
                if (fis) applied.fields[applied.count++] = {uint8_t(base + 1), IntentValue::ofU64(out.in_step)};
                if (fiw) applied.fields[applied.count++] = {uint8_t(base + 2), IntentValue::ofU64(out.in_wait)};
                if (fos) applied.fields[applied.count++] = {uint8_t(base + 3), IntentValue::ofU64(out.out_step)};
                if (fow) applied.fields[applied.count++] = {uint8_t(base + 4), IntentValue::ofU64(out.out_wait)};
                if (fof) applied.fields[applied.count++] = {uint8_t(base + 5), IntentValue::ofU64(out.offset)};
            }

            if (applied.count == 0) return Ret::err(NackCode::INVALID_VALUE);
            cfgChanged = false;  // session-volatile, same as classic pattern-cmd
            return Ret::ok(applied);
        }

        // ---- pattern-presets-cmd (0x3220) -> pattern-presets STORE CRUD -----
        // Mirrors SlopDriveHubDelegate's ch::pattern_presets_cmd case exactly:
        // save captures LIVE `_ap` state (never the request — Ground Truth),
        // load decodes the stored payload and re-applies it through the SAME
        // validated setApBase()/setApModifier() path a client's own 0x3210
        // write uses (clamping included), delete/rename are pure store
        // bookkeeping. The roster (0x1220) republishes lazily in
        // publishTelemetry() once it notices the store's generation moved —
        // no special-case publish needed here.
        case ch::pattern_presets_cmd: {
            using PPS = slopdrive::PatternPresetStore;
            const uint64_t op = fieldU64(findField(requested, 1), 0);
            const uint64_t slotRaw = fieldU64(findField(requested, 2), uint64_t(PPS::kCapacity));
            if (slotRaw >= PPS::kCapacity) return Ret::err(NackCode::INVALID_VALUE);
            const uint8_t slotIdx = uint8_t(slotRaw);

            const auto* nameField = findField(requested, 3);
            std::string_view name;
            if (nameField && nameField->value.kind == IntentValue::Kind::Tstr) name = nameField->value.tstr_val;

            switch (op) {
                case 1: {  // save — captures LIVE `_ap` (never the request).
                    if (name.empty()) return Ret::err(NackCode::INVALID_VALUE);
                    uint8_t payload[PPS::kPayloadBytes];
                    payload[0] = _ap.in_speed.value;
                    payload[1] = _ap.out_speed.value;
                    payload[2] = _ap.in_accel.value;
                    payload[3] = _ap.out_accel.value;
                    for (uint8_t id = 0; id < kApBaseCount; ++id) {
                        const advpat::BaseControl* bc = _ap.byId(id);
                        const uint8_t base = uint8_t(4 + id * 6);
                        payload[base + 0] = bc->modifier.amplitude;
                        payload[base + 1] = bc->modifier.in_step;
                        payload[base + 2] = bc->modifier.in_wait;
                        payload[base + 3] = bc->modifier.out_step;
                        payload[base + 4] = bc->modifier.out_wait;
                        payload[base + 5] = bc->modifier.offset;
                    }
                    if (!_presets.save(slotIdx, name, payload)) return Ret::err(NackCode::INVALID_VALUE);
                    applied.count = 3;
                    applied.fields[0] = {1, IntentValue::ofU64(1)};
                    applied.fields[1] = {2, IntentValue::ofU64(slotIdx)};
                    applied.fields[2] = {3, IntentValue::ofTstr(name)};
                    cfgChanged = false;
                    return Ret::ok(applied);
                }
                case 2: {  // load — engages Advanced mode, same as the firmware.
                    if (_estop_latched) return Ret::err(NackCode::ESTOP_ACTIVE);
                    const uint8_t* p = _presets.payload(slotIdx);
                    if (p == nullptr) return Ret::err(NackCode::INVALID_VALUE);
                    _ap_mode = true;
                    setApBase(_ap, advpat::SPEED_IN,  int(p[0]));
                    setApBase(_ap, advpat::SPEED_OUT, int(p[1]));
                    setApBase(_ap, advpat::ACCEL_IN,  int(p[2]));
                    setApBase(_ap, advpat::ACCEL_OUT, int(p[3]));
                    for (uint8_t id = 0; id < kApBaseCount; ++id) {
                        const uint8_t base = uint8_t(4 + id * 6);
                        setApModifier(_ap, id, int(p[base + 0]), int(p[base + 1]), int(p[base + 2]),
                                      int(p[base + 3]), int(p[base + 4]), int(p[base + 5]));
                    }
                    applied.count = 2;
                    applied.fields[0] = {1, IntentValue::ofU64(2)};
                    applied.fields[1] = {2, IntentValue::ofU64(slotIdx)};
                    cfgChanged = false;
                    return Ret::ok(applied);
                }
                case 3: {  // delete
                    if (!_presets.remove(slotIdx)) return Ret::err(NackCode::INVALID_VALUE);
                    applied.count = 2;
                    applied.fields[0] = {1, IntentValue::ofU64(3)};
                    applied.fields[1] = {2, IntentValue::ofU64(slotIdx)};
                    cfgChanged = false;
                    return Ret::ok(applied);
                }
                case 4: {  // rename
                    if (name.empty()) return Ret::err(NackCode::INVALID_VALUE);
                    if (!_presets.rename(slotIdx, name)) return Ret::err(NackCode::INVALID_VALUE);
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
            return Ret::err(NackCode::UNKNOWN_CHANNEL);
    }
}

void MachineSim::onEstop(uint8_t cause, uint8_t origin) {
    // §11.2: motion stops before bookkeeping — same field set as the firmware.
    // estop_requested is the RMW the motor task consumes; here _estop_pending
    // is that flag, and the next 1 ms motion substep is the Core-1 consumer.
    // Until it runs, canClearEstop() refuses (CLEAR_REFUSED) — the device's
    // "motion is not PROVEN stopped yet" window, now modeled instead of wished
    // away.
    _estop_pending = true;
    releaseMotion();
    _estop_latched = true;
    _homed = false;
    _homing = false;
    _paused = false;
    _override = false;
    _resume_start_ms = 0;
    _pattern.running = false;
    _log.logf('W', "ESTOP latched via hub (cause=%u origin=%u)", cause, origin);
}

bool MachineSim::canClearEstop() {
    // §11.2 machine-domain precondition, modeled: refuse while the stop is
    // still PENDING (the motion substep that kills the pulse train has not run
    // yet) — the hub NACKs CLEAR_REFUSED, exactly like the device does while
    // estop_requested is un-consumed. Clearing NEVER rehomes (firmware
    // doctrine): homed stays false until an explicit HOME intent.
    if (_estop_pending) return false;
    _estop_latched = false;
    return true;
}

std::optional<uint8_t> MachineSim::sourceForChannel(uint16_t channel_id) {
    // Alien (benchrig) never advertises pattern_cmd, so kSrcPattern is only
    // ever reachable over the wire on Device/Minimal — sourcePolicy() below
    // still handles it either way for the local, TUI-only pattern generator.
    if (channel_id == benchrig::ch::move || channel_id == ch::move) return kSrcManual;
    if (channel_id == benchrig::ch::motion_input || channel_id == ch::motion_input) return kSrcTcodeStream;
    if (channel_id == benchrig::ch::motion_segment || channel_id == ch::motion_segment) return kSrcTcodeStream;
    if (channel_id == ch::pattern_cmd) return kSrcPattern;
    return std::nullopt;
}

slopsync::SourceLossPolicy MachineSim::sourcePolicy(uint8_t source_id) {
    if (source_id == kSrcPattern) return slopsync::SourceLossPolicy::Continue;
    return slopsync::SourceLossPolicy::Stop;
}

void MachineSim::onDeadmanStop(uint8_t source_id) {
    // The device runs WS_OP_HALT here, and the motor STAYS stopped because a
    // deadman by definition means the source went silent past the sampler's
    // STREAM_IDLE_TIMEOUT — streamSamplerTask has already released FAS. Model
    // that release directly (releaseMotion), not the bare forceStop, or the sim
    // would jump straight back onto the abandoned plan on the next substep.
    releaseMotion();
    _log.logf('W', "deadman stop on source %u — motion halted", source_id);
}

void MachineSim::onSessionJoined(uint32_t session_id) {
    _log.logf('I', "session %08x joined", unsigned(session_id));
}

void MachineSim::onSessionLeft(uint32_t session_id) {
    _log.logf('I', "session %08x left", unsigned(session_id));
}

// Transcription of SlopDriveHubDelegate::onStreamBundle (fixed-offset decode,
// nearest-window timestamp resolve, far-future clamp, 0x0085 sentinel).
void MachineSim::onStreamBundle(uint16_t channel_id, uint32_t /*session_id*/,
                                const slopsync::BundleView& bundle) {
    const bool isSegment = (channel_id == benchrig::ch::motion_segment || channel_id == ch::motion_segment);
    if (channel_id != benchrig::ch::motion_input && channel_id != ch::motion_input && !isSegment) return;

    const int64_t now64 = int64_t(_clock.nowUs64());
    const uint32_t now32 = uint32_t(now64 & 0xFFFFFFFFll);

    uint32_t ringDrops = 0, badDuration = 0, farClamped = 0;
    const uint8_t n = bundle.sampleCount();
    // Recorder rows are built alongside the decode and committed to the ring in
    // ONE locked burst at the end — the HTTP reader must never see a half-built
    // bundle, and taking the mutex per sample would put a lock inside the
    // hottest wire path for no benefit. ~2 KB of stack (32 × 64 B) — a
    // non-issue on a host thread, and this file is HOST-ONLY; if any of this
    // ever migrates to the firmware it becomes a heap/static decision, because
    // the device has already been killed once by a multi-KB stack temporary.
    std::array<IngressRecord, slopsync::limits::bundle_max_samples> recs{};
    uint8_t recN = 0;
    for (uint8_t i = 0; i < n; ++i) {
        const uint32_t wireT = bundle.sampleTimeUs(i);
        int32_t delta = int32_t(wireT - now32);
        bool clamped = false;
        if (delta > 250000) { delta = 250000; ++farClamped; clamped = true; }
        if (delta < 0) delta = 0;

        const auto sample = bundle.sample(i);
        const uint16_t rawTarget = slopsync::getU16(sample.subspan(0, 2));

        PacingEntry e{};
        e.due_us = uint64_t(now64 + int64_t(delta));
        e.target = float(rawTarget) / 10000.0f;

        // ---- recorder row: RAW wire values first, decoded values after ------
        IngressRecord& r = recs[recN];
        r = IngressRecord{};
        r.t_s = float(double(now64) / 1e6);
        r.channel_id = channel_id;
        r.raw_target = rawTarget;
        r.target = e.target;
        r.ts_clamped = clamped;
        r.wire_t_us = wireT;
        r.wire_t_off_us = wireT - bundle.tBase();
        r.due_us = e.due_us;
        r.due_delta_ms = float(double(int64_t(e.due_us) - now64) / 1000.0);

        if (isSegment) {
            const uint16_t rawDurMs = slopsync::getU16(sample.subspan(2, 2));
            const int16_t rawEndV = int16_t(slopsync::getU16(sample.subspan(4, 2)));
            r.raw_dur_ms = rawDurMs;
            r.raw_end_vel = rawEndV;
            if (rawDurMs == 0) {
                ++badDuration;
                r.accepted = false;   // recorded ANYWAY — a refused segment is
                ++recN;               // exactly the kind of sender bug we hunt
                continue;
            }
            e.has_duration = true;
            e.duration_us = uint32_t(rawDurMs) * 1000u;
            if (rawEndV == kSegNoEndVel) {
                e.has_end_vel = false;
            } else {
                e.vel = float(rawEndV) / 1000.0f;
                e.has_end_vel = true;
            }
            r.has_duration = true;
            r.duration_us = e.duration_us;
            r.has_end_vel = e.has_end_vel;
            r.end_vel = e.has_end_vel ? e.vel : 0.0f;
        } else {
            const int16_t rawVel = int16_t(slopsync::getU16(sample.subspan(2, 2)));
            e.vel = float(rawVel) / 1000.0f;
            e.has_end_vel = (e.vel != 0.0f);
            r.raw_end_vel = rawVel;
            r.end_vel = e.vel;
            r.has_end_vel = e.has_end_vel;
        }
        ++recN;

        if (_pacingRing.push(e)) ++ringDrops;
    }
    recordIngress(recs.data(), recN);

    ++_sync_bundles;
    _sync_samples += n;
    if (isSegment) ++_sync_seg_bundles;
    if (ringDrops || badDuration) _sync_dropped += ringDrops + badDuration;
    if (farClamped) {
        // Firmware: SLOGW_EVERY_MS(2000, ...). SessionLog has no throttle, so
        // the 2 s gate is hand-rolled here — a client that lost CLOCK sync
        // would otherwise flood the log at the stream rate.
        _ts_clamped += farClamped;
        const uint32_t nowMs = uint32_t(uint64_t(now64) / 1000);
        if (_lastFarLogMs == 0 || (nowMs - _lastFarLogMs) >= 2000) {
            _lastFarLogMs = nowMs;
            _log.logf('W',
                      "motion-stream: %u sample(s) clamped from far-future t_off "
                      "(missed CLOCK resync on the client?) — %u total",
                      unsigned(farClamped), unsigned(_ts_clamped));
        }
    }
}

// ---- Fault injection + snapshot ---------------------------------------------

void MachineSim::injectEstop() {
    if (_estop_latched) return;
    // Same entry the firmware's syncSafety uses when the machine estops outside
    // the hub: latch on the hub, which calls onEstop() first (motion stops).
    _hub.latchEstop(0, 0, ++_estopSeq);
    _estop_latched = true;
}

void MachineSim::injectClearEstop() {
    _estop_latched = false;  // syncSafety clears the hub latch next tick
}

void MachineSim::startHoming() {
    if (_homing) return;
    _homing = true;
    _homed = false;
    _homing_done_us = _clock.nowUs64() + kHomingDurationUs;
    _log.logf('I', "sim: homing started (%.1fs cycle)", double(kHomingDurationUs) / 1e6);
}

void MachineSim::forceUnhome() {
    _homed = false;
    _homing = false;
    releaseMotion();  // gates shut = the sampler stops feeding on the device too
    _log.logf('W', "sim: forced UNHOMED");
}

void MachineSim::togglePause() {
    _paused = !_paused;
    if (!_paused) _resume_start_ms = uint32_t(_clock.nowUs64() / 1000);
}

void MachineSim::toggleOverride() { _override = !_override; }

// ---- Palette-driven config + trace export -----------------------------------

float MachineSim::uiSetLimit(LimitKind kind, float value) {
    float applied;
    switch (kind) {
        case LimitKind::UserSpeed:
            applied = _user_speed = float(lroundf(clampf(value, 1.0f, kSpeedCeiling)));
            break;
        case LimitKind::UserAccel:
            applied = _user_accel = float(lroundf(clampf(value, 1.0f, kAccelCeiling)));
            break;
        case LimitKind::InputSpeed:
            applied = _input_speed = float(lroundf(clampf(value, 1.0f, kSpeedCeiling)));
            break;
        case LimitKind::InputJerk:
            applied = _input_jerk = float(lroundf(clampf(value, 1.0f, kJerkCeiling)));
            break;
        case LimitKind::InputAccel:
        default:
            applied = _input_accel = float(lroundf(clampf(value, 1.0f, kAccelCeiling)));
            break;
    }
    deriveEngineLimits();
    _cfgDirty = true;
    return applied;
}

bool MachineSim::uiSetWindow(float min_mm, float max_mm) {
    // Same two-stage legality as the 0x0101 path (refuse inverted, then
    // RangeMapper::setRange clamp + 5 mm minimum span).
    if (min_mm >= max_mm) return false;
    applyWindowLegality(min_mm, max_mm);
    deriveEngineLimits();
    _cfgDirty = true;
    return true;
}

uint8_t MachineSim::uiSetStreamSpeedMode(uint8_t mode) {
    _stream_speed_mode = mode > kSpeedVelocityMatched ? kSpeedVelocityMatched : mode;
    return _stream_speed_mode;
}

float MachineSim::uiSetMeasuredStroke(float mm) {
    // 0 clears the measurement (effective ceiling falls back to the rail);
    // anything else is bounded by the rail, like a real measured stroke.
    _measured_stroke_mm = mm <= 0.0f ? 0.0f : clampf(mm, kMinWindowSpanMm, _max_rail_mm);
    return _measured_stroke_mm;
}

void MachineSim::uiSetPattern(bool running, int idx) {
    if (idx >= 0) _pattern.idx = uint8_t(std::min(idx, 2));
    _pattern.running = running && _homed && !_estop_latched;
    _pattern.next_due_us = 0;
    // No motor stop on pattern-off — PatternEngine::stop() doesn't either.
}

void MachineSim::uiSetPatternParam(int key, float value) {
    switch (key) {
        case 3: _pattern.speed = clampf(value, 0.0f, 100.0f); break;
        case 4: _pattern.depth = clampf(value, 0.0f, 100.0f); break;
        case 5: _pattern.stroke = clampf(value, 0.0f, 100.0f); break;
        case 6: _pattern.sensation = clampf(value, -100.0f, 100.0f); break;
        default: break;
    }
}

// ---- SlopMotion engine knobs (POST /api/slopmotion parity) ------------------
// Read-modify-write on the engine's own Config — same seam deriveEngineLimits()
// drives, so the sim keeps ONE source of truth for engine state (the engine)
// rather than a shadow copy that can drift. Returns the APPLIED value.

slopmotion::InfeasiblePolicy MachineSim::uiSetInfeasiblePolicy(slopmotion::InfeasiblePolicy p) {
    slopmotion::Config c = _engine.config();
    c.infeasible_policy = p;
    _engine.setConfig(c);
    return _engine.config().infeasible_policy;
}

float MachineSim::uiSetInfeasibleMargin(float margin) {
    slopmotion::Config c = _engine.config();
    c.infeasible_scale_margin = clampf(margin, 0.50f, 1.00f);
    _engine.setConfig(c);
    return _engine.config().infeasible_scale_margin;
}

uint8_t MachineSim::uiSetReshapeSteps(uint8_t steps) {
    slopmotion::Config c = _engine.config();
    c.infeasible_reshape_steps = steps > 8 ? uint8_t(8) : steps;
    _engine.setConfig(c);
    return _engine.config().infeasible_reshape_steps;
}

slopmotion::CurvePolicy MachineSim::uiSetCurvePolicy(slopmotion::CurvePolicy p) {
    slopmotion::Config c = _engine.config();
    c.curve_policy = p;
    _engine.setConfig(c);
    return _engine.config().curve_policy;
}

float MachineSim::uiSetSmoothBudget(float budget) {
    slopmotion::Config c = _engine.config();
    c.infeasible_smooth_budget = clampf(budget, 0.0f, 1.0f);
    _engine.setConfig(c);
    return _engine.config().infeasible_smooth_budget;
}

float MachineSim::uiSetAmplitudeBudget(float budget) {
    slopmotion::Config c = _engine.config();
    c.infeasible_amplitude_budget = clampf(budget, 0.0f, 1.0f);
    _engine.setConfig(c);
    return _engine.config().infeasible_amplitude_budget;
}

uint8_t MachineSim::uiSetBlendSteps(uint8_t steps) {
    slopmotion::Config c = _engine.config();
    c.infeasible_blend_steps = steps < 1 ? uint8_t(1)
                             : (steps > 10 ? uint8_t(10) : steps);
    _engine.setConfig(c);
    return _engine.config().infeasible_blend_steps;
}

float MachineSim::uiSetSettleGraceMs(float ms) {
    // ms in, µs into the engine — the unit boundary lives HERE, exactly like
    // the device's POST /api/slopmotion settle_grace_ms handler.
    slopmotion::Config c = _engine.config();
    c.settle_grace_us = uint32_t(clampf(ms, 0.0f, 200.0f) * 1000.0f);
    _engine.setConfig(c);
    return float(_engine.config().settle_grace_us) / 1000.0f;
}

float MachineSim::uiSetJmax(float jmax) {
    // 0 (or negative) CLEARS the override — jerk falls back to the derived
    // _input_jerk / span, exactly like the firmware's `jovr == 0` branch.
    _jmax_norm = jmax > 0.0f ? clampf(jmax, 1.0f, 1000000.0f) : 0.0f;
    deriveEngineLimits();   // rebuilds v/a/j from the mm limit set; override wins if set
    return _engine.config().limits.jmax;
}

bool MachineSim::uiSetChaseAimAccelExtrap(bool on) {
    slopmotion::Config c = _engine.config();
    c.chase_aim_accel_extrap = on;
    _engine.setConfig(c);
    return _engine.config().chase_aim_accel_extrap;
}

bool MachineSim::uiSetWaveCentering(bool on) {
    slopmotion::Config c = _engine.config();
    c.wave_centering = on;
    _engine.setConfig(c);
    return _engine.config().wave_centering;
}

float MachineSim::uiSetWaveCenteringGain(float gain) {
    // The engine clamps on USE; clamping at the seam too keeps the readouts and
    // the /api/slopmotion echo showing the value that is actually in force.
    slopmotion::Config c = _engine.config();
    c.wave_centering_gain = clampf(gain, 0.0f, 1.0f);
    _engine.setConfig(c);
    return _engine.config().wave_centering_gain;
}

std::vector<float> MachineSim::copyTraceSince(float since_s, float& max_rail, float& win_min,
                                              float& win_max) const {
    std::lock_guard<std::mutex> lk(_traceM);
    max_rail = _max_rail_mm;
    win_min = _win_min_mm;
    win_max = _win_max_mm;
    // Samples are time-ordered; walk back from newest to find the first > since.
    size_t start = _traceCount;
    while (start > 0 && traceAt(start - 1).t_s > since_s) --start;
    std::vector<float> out;
    out.reserve((_traceCount - start) * kTraceStride);
    for (size_t i = start; i < _traceCount; ++i) {
        const auto& s = traceAt(i);
        out.push_back(s.t_s);
        out.push_back(s.pos_mm);
        out.push_back(s.tgt_mm);
        out.push_back(s.vel_mm_s);
        out.push_back(s.cmd_norm);
        out.push_back(s.raw_norm);
    }
    return out;
}

// ---- Inbound wire recorder --------------------------------------------------
// Sim thread. One lock per BUNDLE, not per sample.
void MachineSim::recordIngress(IngressRecord* rows, uint8_t n) {
    if (n == 0) return;
    std::lock_guard<std::mutex> lk(_ingressM);
    for (uint8_t i = 0; i < n; ++i) {
        IngressRecord& r = rows[i];
        const bool isSeg = (r.channel_id == benchrig::ch::motion_segment || r.channel_id == ch::motion_segment);
        const size_t chIdx = isSeg ? 1 : 0;

        // Inter-arrival gap is PER CHANNEL — a client running both a chase feed
        // and a segment feed would otherwise interleave into meaningless deltas.
        float& last = _last_arrival_t_s[chIdx];
        r.gap_ms = last < 0.0f ? -1.0f : (r.t_s - last) * 1000.0f;
        last = r.t_s;
        // BUNDLE granularity: only row 0 of a bundle folds into the aggregate.
        // Rows 1..n-1 of a multi-sample bundle arrived at the same instant, so
        // their gap is structurally 0 and would only dilute the mean.
        if (r.gap_ms >= 0.0f && i == 0) {
            auto& g = isSeg ? _ingressStats.seg_gap : _ingressStats.smp_gap;
            if (g.n == 0) {
                g.min_ms = g.max_ms = r.gap_ms;
            } else {
                if (r.gap_ms < g.min_ms) g.min_ms = r.gap_ms;
                if (r.gap_ms > g.max_ms) g.max_ms = r.gap_ms;
            }
            _gap_sum_ms[chIdx] += double(r.gap_ms);
            ++g.n;
            g.mean_ms = float(_gap_sum_ms[chIdx] / double(g.n));
        }

        ++_ingressStats.records;
        if (isSeg) {
            ++_ingressStats.segments;
            if (!r.accepted) {
                ++_ingressStats.rejected;
            } else {
                if (!r.has_end_vel) ++_ingressStats.sentinel;
                if (r.raw_dur_ms <= 10) ++_ingressStats.dur_floor_10ms;
                if (r.raw_dur_ms < 50) ++_ingressStats.dur_under_50ms;
                // Duty ratio: commanded script time vs wall-clock time covered.
                // The LAST segment's duration is excluded because no wall-clock
                // span has elapsed for it yet — including it would make every
                // reading optimistic by one segment.
                _seg_dur_sum_ms += double(_seg_last_dur_ms);
                _seg_last_dur_ms = float(r.raw_dur_ms);
                if (_seg_first_t_s < 0.0f) _seg_first_t_s = r.t_s;
                _seg_last_t_s = r.t_s;
                const float span_ms = (_seg_last_t_s - _seg_first_t_s) * 1000.0f;
                _ingressStats.span_s = span_ms / 1000.0f;
                _ingressStats.duty_ratio =
                    span_ms > 1.0f ? float(_seg_dur_sum_ms / double(span_ms)) : 0.0f;
            }
        } else {
            ++_ingressStats.samples;
        }

        _ingress[_ingressHead] = r;
        _ingressHead = (_ingressHead + 1) % kIngressCap;
        if (_ingressCount < kIngressCap) ++_ingressCount;
    }
}

std::vector<MachineSim::IngressRecord> MachineSim::copyIngressSince(float since_s) const {
    std::lock_guard<std::mutex> lk(_ingressM);
    size_t start = _ingressCount;
    while (start > 0 && ingressAt(start - 1).t_s > since_s) --start;
    std::vector<IngressRecord> out;
    out.reserve(_ingressCount - start);
    for (size_t i = start; i < _ingressCount; ++i) out.push_back(ingressAt(i));
    return out;
}

MachineSim::IngressStats MachineSim::ingressStats() const {
    std::lock_guard<std::mutex> lk(_ingressM);
    return _ingressStats;
}

const char* MachineSim::ingressCsvHeader() {
    return "t_s,ch,kind,raw_target,raw_dur_ms,raw_end_vel,target_norm,duration_ms,"
           "end_vel_norm,has_end_vel,accepted,ts_clamped,wire_t_off_us,wire_t_us,"
           "due_us,due_delta_ms,gap_ms\n";
}

void MachineSim::formatIngressCsvRow(char* buf, size_t cap, const IngressRecord& r) {
    const bool seg = (r.channel_id == benchrig::ch::motion_segment || r.channel_id == ch::motion_segment);
    std::snprintf(buf, cap,
                  "%.6f,0x%04X,%s,%u,%u,%d,%.4f,%.3f,%.4f,%d,%d,%d,%u,%u,%llu,%.3f,%.3f\n",
                  double(r.t_s), unsigned(r.channel_id), seg ? "segment" : "sample",
                  unsigned(r.raw_target), unsigned(r.raw_dur_ms), int(r.raw_end_vel),
                  double(r.target), double(r.duration_us) / 1000.0, double(r.end_vel),
                  r.has_end_vel ? 1 : 0, r.accepted ? 1 : 0, r.ts_clamped ? 1 : 0,
                  unsigned(r.wire_t_off_us), unsigned(r.wire_t_us),
                  (unsigned long long)(r.due_us), double(r.due_delta_ms), double(r.gap_ms));
}

size_t MachineSim::exportIngress(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return 0;
    std::fputs(ingressCsvHeader(), f);
    std::lock_guard<std::mutex> lk(_ingressM);
    char line[256];
    for (size_t i = 0; i < _ingressCount; ++i) {
        formatIngressCsvRow(line, sizeof(line), ingressAt(i));
        std::fputs(line, f);
    }
    std::fclose(f);
    return _ingressCount;
}

void MachineSim::resetIngress() {
    std::lock_guard<std::mutex> lk(_ingressM);
    _ingressHead = 0;
    _ingressCount = 0;
    _ingressStats = IngressStats{};
    _gap_sum_ms[0] = _gap_sum_ms[1] = 0.0;
    _seg_dur_sum_ms = 0.0;
    _seg_last_dur_ms = 0.0f;
    _seg_first_t_s = _seg_last_t_s = -1.0f;
    _last_arrival_t_s[0] = _last_arrival_t_s[1] = -1.0f;
}

size_t MachineSim::exportTrace(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return 0;
    // cmd_norm is the commanded target in the units it was commanded in; cmd_mm
    // is that value put through the CURRENT stroke window, so a diff of the two
    // columns against tgt_mm is the same window-mapping check the analyzer draws.
    std::fputs("t_s,pos_mm,tgt_mm,vel_mm_s,cmd_norm,cmd_mm\n", f);
    const float span = _win_max_mm - _win_min_mm;
    for (size_t i = 0; i < _traceCount; ++i) {
        const auto& s = traceAt(i);
        if (s.cmd_norm < 0.0f) {
            std::fprintf(f, "%.3f,%.3f,%.3f,%.2f,,\n", double(s.t_s), double(s.pos_mm),
                         double(s.tgt_mm), double(s.vel_mm_s));
        } else {
            std::fprintf(f, "%.3f,%.3f,%.3f,%.2f,%.4f,%.3f\n", double(s.t_s), double(s.pos_mm),
                         double(s.tgt_mm), double(s.vel_mm_s), double(s.cmd_norm),
                         double(_win_min_mm + s.cmd_norm * span));
        }
    }
    std::fclose(f);
    return _traceCount;
}

MachineSim::MotionDigest MachineSim::motionDigest() const {
    MotionDigest d;
    d.pos10 = int32_t(_stepper.positionMm() * 10.0f);
    d.tgt10 = int32_t(_commanded_target_mm * 10.0f);
    d.vel10 = int32_t(_stepper.velocityMmS() * 10.0f);
    d.flags = uint8_t((_homed << 0) | (_homing << 1) | (_pattern.running << 2) | (_paused << 3) |
                      (_override << 4) | (_estop_latched << 5));
    d.sessions = uint8_t(_hub.sessionCount());
    // Unlocked read: motionDigest() is called from the TUI, which runs on the
    // sim thread — the same thread that writes it. No race exists to guard.
    d.ingress = _ingressStats.records;
    return d;
}

MachineSim::Snapshot MachineSim::snapshot() {
    Snapshot s;
    s.pos_mm = _stepper.positionMm();
    s.tgt_mm = _commanded_target_mm;
    s.vel_mm_s = _stepper.velocityMmS();
    s.win_min = _win_min_mm;
    s.win_max = _win_max_mm;
    s.max_rail = _max_rail_mm;
    s.homed = _homed;
    s.homing = _homing;
    s.paused = _paused;
    s.override_on = _override;
    s.estop = _estop_latched;
    s.pattern_running = _pattern.running;
    s.pattern_idx = _pattern.idx;
    const auto es = _engine.snapshot(_clock.nowUs64());
    s.engine_mode = es.mode;
    s.engine_plan = es.plan_kind;
    s.strokes = _strokes;
    s.distance_m = _distance_mm / 1000.0f;
    s.peak_mm_s = _peak_mm_s;
    s.sync_bundles = _sync_bundles;
    s.sync_samples = _sync_samples;
    s.sync_enqueued = _sync_enqueued;
    s.sync_dropped = _sync_dropped;
    s.plan_rejected = _plan_rejected;
    s.anomalies = _anom_total;
    s.anom_kind = _anom_kind;
    s.tick_ms_avg = _tick_dt_avg_ms;
    s.tick_ms_max = _tick_dt_max_ms;
    s.cfg_gen = _hub.cfgGen();
    s.sessions = _hub.sessionCount();
    for (uint8_t i = 0; i < SlopSimWsPort::kSlots; ++i) {
        auto info = _port.slotInfo(i);
        s.slots[i].inUse = info.inUse;
        s.slots[i].peer = info.peer;
        s.slots[i].muted = info.muted;
        const auto ic = _hub.streamIngressCounters(i);
        s.slots[i].stream_accepted = ic.accepted;
        s.slots[i].stream_dropped = ic.dropped;
    }
    return s;
}

}  // namespace slopsim
