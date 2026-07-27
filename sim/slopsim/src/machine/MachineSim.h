#pragma once

// ============================================================================
// MachineSim — the virtual SlopDrive. Roadmap §6 "SlopSim" v1+v2 rough-in.
//
// Composition mirrors the firmware's SlopSyncHubService one-for-one:
//   HostClock/HostRandom  (SlopSyncPlatform.h's EspClock/EspRandom analogs)
//   buildSlopDriveCatalog()   — the REAL device catalog, included verbatim
//   slopsync::Hub             — the REAL hub, byte-identical protocol behavior
//   slopmotion::Engine        — the REAL motion core (quintic/chase/settle)
//   SimStepper                — FAS modeled at the MotorDriver seam: the
//                               trapezoidal ramp follower that streamToSteps()
//                               drives. ACTUAL position comes from HERE, the
//                               engine only provides the 1 kHz setpoints —
//                               same division of truth as the hardware.
//   SlopSimWsPort             — real WS transport (net/WsServerPort.h)
//
// The delegate half of this class is a transcription of SlopDriveHubDelegate
// (src/comms/SlopSyncHubService.cpp) with WebUI::handleCommand replaced by
// direct application to the sim's machine state — gates, NACK codes, clamp
// semantics, and post-clamp echoes are kept intentionally identical so a
// client cannot tell the difference (Ground Truth doctrine).
//
// THREADING: everything in this class runs on the ONE sim thread (tick()).
// The WS port marshals its connection threads; see WsServerPort.h.
// ============================================================================

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "SlopSyncCatalog.h"
#include "slopmotion/slopmotion.hpp"
#include "slopsync/hub/hub.hpp"

#include "common/HostPlatform.h"
#include "common/SessionLog.h"
#include "net/WsServerPort.h"

namespace slopsim {

// ---- SlopMotion anomaly kind names --------------------------------------
// MIRROR of the device's kSmAnomalyNames (include/system/SystemState.h), kept
// byte-identical ON PURPOSE: the whole value of this table is that
// GET /api/slopmotion on the sim and on the device produce the SAME
// `anomalies_by_kind` keys, so an operator can diff the two responses directly.
// A separate copy (rather than including the firmware header) is forced by the
// sim's host-clean rule — SystemState.h drags in FreeRTOS. If the engine grows
// a kind, BOTH tables move together; the drain bounds-checks against this
// array's own size, and an unnamed kind logs as "?<n>" rather than "?".
inline constexpr const char* kSmAnomalyNames[] = {
    "none",                // slopmotion::AnomalyType::None              = 0
    "plan_failed",         //                        ::PlanFailed        = 1
    "settle",              //                        ::SettleEngaged     = 2
    "endvel_clamped",      //                        ::EndVelClamped     = 3
    "deadline_stretched",  //                        ::DeadlineStretched = 4
    "waveform_fallback",   //                        ::WaveformFallback  = 5
    "waveform_scaled",     //                        ::WaveformScaled    = 6
    "waveform_centred",    //                        ::WaveformCentred   = 7
    "handoff_bounded",     //                        ::HandoffBounded    = 8
    "waveform_smoothed",   //                        ::WaveformSmoothed  = 9
};
// Counter array width: as of slopmotion 0.8.0 the enum ends at WaveformSmoothed
// = 9, so this array is EXACTLY FULL again — there is no spare slot. A kind 10
// REQUIRES bumping kSmAnomalyKinds (here AND on the device) in the same change
// as the name append; until then out-of-range kinds are dropped by the drain,
// never written, and log as "?10".
//
// DEVICE SIDE IS DELIBERATELY STILL AT 9 (sim-first prototype): the firmware's
// SystemState::SM_ANOM_KINDS, its own kSmAnomalyNames, and the per-kind field
// list on the 0x0088 slopmotion-diag channel all move together when the
// budgeted policies go to hardware — and that last one is an ETAG CHANGE, so it
// lands as one deliberate commit rather than drifting in ahead of the test.
inline constexpr size_t kSmAnomalyNameCount =
    sizeof(kSmAnomalyNames) / sizeof(kSmAnomalyNames[0]);
inline constexpr size_t kSmAnomalyKinds = 10;
static_assert(kSmAnomalyNameCount <= kSmAnomalyKinds,
              "kSmAnomalyNames outgrew the counter array — bump kSmAnomalyKinds");

// ---- PacingRing — verbatim host copy of the firmware's (SlopSyncHubService.h).
// Same single-thread producer/consumer (onStreamBundle fires inside
// hub.update() on the sim thread; drain runs on the sim thread), so it stays
// lock-free for the same structural reason.
struct PacingEntry {
    uint64_t due_us = 0;
    float    target = 0.0f;
    float    vel    = 0.0f;
    uint32_t duration_us  = 0;
    bool     has_duration = false;
    bool     has_end_vel  = false;
};

class PacingRing {
public:
    static constexpr size_t kCapacity = 64;

    bool push(const PacingEntry& e) {
        bool overwrote = false;
        if (_count == kCapacity) {
            _tail = (_tail + 1) % kCapacity;
            overwrote = true;
        } else {
            ++_count;
        }
        _buf[_head] = e;
        _head = (_head + 1) % kCapacity;
        return overwrote;
    }

    bool popDue(uint64_t now_us, PacingEntry& out) {
        if (_count == 0 || _buf[_tail].due_us > now_us) return false;
        out = _buf[_tail];
        _tail = (_tail + 1) % kCapacity;
        --_count;
        return true;
    }

    // RFC-008 one-segment lookahead — see the firmware's own peekOldest().
    const PacingEntry* peekOldest() const {
        return _count == 0 ? nullptr : &_buf[_tail];
    }

private:
    std::array<PacingEntry, kCapacity> _buf{};
    size_t _head = 0, _tail = 0, _count = 0;
};

// ---- SimStepper — FastAccelStepper modeled at the MotorDriver seam ---------
// The firmware's 1 kHz sampler path ends in streamToSteps(target, speed,
// accel) -> FAS re-ramps from CURRENT velocity toward the micro-target under
// those ceilings. FAS's ramp generator is a trapezoidal follower; this is that
// follower in the mm domain (step quantization is 1/AIM_STEPS_PER_MM =
// 1/20.372 = 0.0491 mm — below anything the UI shows or the operator feels, so
// steps are not modeled). NOT modeled: RMT jitter, driver electrical behavior
// — roadmap §6 explicitly scopes those out.
class SimStepper {
public:
    void reset(float pos_mm) { _pos = pos_mm; _vel = 0.0f; _target = pos_mm; }

    // streamToSteps()/moveTo() seam: retarget with ceilings. Non-blocking,
    // replans from current velocity — exactly FAS's moveTo contract.
    void command(float target_mm, float speed_mm_s, float accel_mm_s2) {
        _target = target_mm;
        _vmax = speed_mm_s > 1.0f ? speed_mm_s : 1.0f;
        _amax = accel_mm_s2 > 1.0f ? accel_mm_s2 : 1.0f;
    }

    void hardStop() { _vel = 0.0f; _target = _pos; }  // forceStop seam (e-stop)

    // One integration step (dt in seconds; sim runs 1 ms substeps like the
    // firmware sampler). Classic trapezoidal servo: decelerate when the
    // stopping distance reaches the remaining distance, else run at vmax.
    void step(float dt) {
        const float dist = _target - _pos;
        const float adist = dist < 0 ? -dist : dist;
        if (adist < 0.005f && _vel * _vel < 2.0f * _amax * 0.005f) {
            _pos = _target;
            _vel = 0.0f;
            return;
        }
        const float dir = dist < 0 ? -1.0f : 1.0f;
        const float stopDist = (_vel * _vel) / (2.0f * _amax);
        float a;
        if (_vel * dir > 0 && stopDist >= adist) {
            a = -dir * _amax;                       // braking into the target
        } else {
            a = dir * _amax;                        // accelerate toward target
        }
        _vel += a * dt;
        if (_vel > _vmax) _vel = _vmax;
        if (_vel < -_vmax) _vel = -_vmax;
        _pos += _vel * dt;
    }

    float positionMm() const { return _pos; }
    float velocityMmS() const { return _vel; }
    float targetMm() const { return _target; }

private:
    float _pos = 0.0f, _vel = 0.0f, _target = 0.0f;
    float _vmax = 100.0f, _amax = 1000.0f;
};

// ---- SimPattern — stand-in generators (PatternEngine is FreeRTOS-tainted) --
// Emits ONE waveform segment per half-stroke into the engine — the same seam
// the firmware PatternEngine drives. v1 patterns: 0 stroke, 1 tease (sensation
// skews in/out durations), 2 shallow-fast.
struct SimPattern {
    bool running = false;
    uint8_t idx = 0;
    float speed = 50.0f;      // % -> stroke cadence
    float depth = 100.0f;     // % -> far end of travel (within window, norm)
    float stroke = 100.0f;    // % -> amplitude below depth
    float sensation = 0.0f;   // -100..100 -> in/out skew
    bool goingDeep = false;
    uint64_t next_due_us = 0;
};

// ---- The sim machine --------------------------------------------------------
class MachineSim final : public slopsync::HubDelegate {
public:
    explicit MachineSim(SessionLog& log);

    bool begin(uint16_t wsPort, bool startHomed);
    void shutdown();

    // One sim tick — call it as fast as the host loop can (target ~1 ms).
    // TWO RATES, mirroring the firmware's task split:
    //   ~5 ms  COMMS pump (SlopSyncHubService::taskLoop's pdMS_TO_TICKS(5)):
    //          port pump -> hub.update -> safety sync -> telemetry publish.
    //   ~1 ms  MOTION substeps (streamSamplerTask's vTaskDelayUntil(1 ms)):
    //          pacing-ring drain + pattern + engine sample + stepper integrate,
    //          all at the SAME substep instant, so a commit is only ever
    //          "current" for one 1 ms sample exactly like the device.
    void tick();

    // ---- Fault injection / TUI controls (sim thread) -----------------------
    void injectEstop();
    void injectClearEstop();
    void startHoming();
    void forceUnhome();
    void togglePause();
    void toggleOverride();
    void kickSession(uint8_t slot) { _port.kick(slot); }
    void setCongestion(uint8_t slot, uint8_t level) { _hub.setCongestionLevel(slot, level); }

    // ---- Palette-driven config overrides (sim thread) -----------------------
    // Same clamps as the 0x0101 config-set delegate path; return the APPLIED
    // value (ground truth for the toast). Publishes a fresh 0x0081 snapshot.
    // RFC-011 (spec gap CLOSED): a machine-side edit now bumps the protocol
    // cfg_gen too, via Hub::bumpConfigGeneration() — see the 0x0081 publish
    // block. Before that API existed, cfg_gen only moved via intents, so a
    // client's `precondition` CAS passed against config that had already
    // changed underneath it.
    enum class LimitKind { UserSpeed, UserAccel, InputSpeed, InputAccel, InputJerk };
    float uiSetLimit(LimitKind kind, float value);
    bool uiSetWindow(float min_mm, float max_mm);  // false = rejected (min>=max)

    // Stream speed feed (SystemState::StreamSpeedMode). DEVICE DEFAULT is
    // ceiling-pegged (0) — the follower gets full speed authority and the 1 ms
    // micro-target deltas shape the velocity. Velocity-matched (1) hands FAS
    // the curve's own instantaneous speed as its vmax, which structurally
    // prevents any recovery from lag. Returns the APPLIED mode.
    uint8_t uiSetStreamSpeedMode(uint8_t mode);
    uint8_t streamSpeedMode() const { return _stream_speed_mode; }

    // Measured stroke (MotorDriver::effectiveCeilingMm): once homed the sampler
    // target is clamped to the MEASURED stroke, not the configured rail.
    // 0 = "not measured" -> the rail is the ceiling. Returns APPLIED.
    float uiSetMeasuredStroke(float mm);
    float measuredStrokeMm() const { return _measured_stroke_mm; }
    float effectiveCeilingMm() const {
        return _measured_stroke_mm > 0.0f ? _measured_stroke_mm : _max_rail_mm;
    }
    void uiSetPattern(bool running, int idx /*-1 keep*/);
    void uiSetPatternParam(int key /*3..6 like 0x0102*/, float value);

    // ---- SlopMotion engine knobs (sim thread) -------------------------------
    // Mirror of the firmware's GET/POST /api/slopmotion tuning block: clamped
    // here, APPLIED value returned for the toast, pushed straight into the
    // engine via the same setConfig() path deriveEngineLimits() uses. Not
    // persisted — a sim restart restores the engine's compile-time defaults,
    // exactly like the device.
    // THREE policies since slopmotion 0.4.0 (Stretch/Scale/Reshape) — Reshape
    // is the engine default. Never map this from a bool.
    slopmotion::InfeasiblePolicy uiSetInfeasiblePolicy(slopmotion::InfeasiblePolicy p);
    float uiSetInfeasibleMargin(float margin);        // clamped [0.50 .. 1.00]
    // RESHAPE bisection depth — plan-time budget dial, clamped [0 .. 8].
    uint8_t uiSetReshapeSteps(uint8_t steps);
    // BUDGETED policies (slopmotion 0.8.0). Each policy uses exactly ONE of
    // these: the cap on the axis it spends FIRST to protect the other.
    //   smooth budget    — prio-amplitude: max alpha (handle reduction toward
    //                      the chord). 1.0 reaches a straight line, which is C0
    //                      at the knots. Clamped [0 .. 1].
    //   amplitude budget — prio-smooth: max fraction of the commanded stroke
    //                      surrendered. 0.5 stops at the segment midpoint.
    //                      Clamped [0 .. 1].
    // Curve family for waveform-segment reconstruction (slopmotion 0.8.0).
    // FollowClient == today's behaviour until curve_family wire signalling
    // exists; ForceC1 rebuilds a Pchip/Makima span as the cubic it actually is.
    slopmotion::CurvePolicy uiSetCurvePolicy(slopmotion::CurvePolicy p);
    float   uiSetSmoothBudget(float budget);
    float   uiSetAmplitudeBudget(float budget);
    // Alpha-search bisection depth. Cheaper per step than the reshape search
    // (one quintic build + one scan, no Ruckig call). Clamped [1 .. 10].
    uint8_t uiSetBlendSteps(uint8_t steps);
    // Settle grace in MILLISECONDS (the operator-facing unit, matching the
    // device's /api/slopmotion settle_grace_ms); the engine field is µs and the
    // conversion happens in the setter. Clamped [0 .. 200] ms; returns APPLIED ms.
    float uiSetSettleGraceMs(float ms);
    bool  uiSetChaseAimAccelExtrap(bool on);
    // DC centring of a degraded band (slopmotion 0.5.0, WAVEFORM path): keep the
    // achieved stroke symmetric about the COMMANDED midpoint when the machine
    // cannot deliver the full amplitude on the clock. ON is the engine default;
    // OFF restores the 0.4.0 contract. Gain is a feel dial clamped [0 .. 1] and
    // deliberately NOT monotone — see the engine header. Return APPLIED.
    bool  uiSetWaveCentering(bool on);
    float uiSetWaveCenteringGain(float gain);   // clamped [0.0 .. 1.0]
    // NORMALIZED jerk OVERRIDE (units/s^3 over the stroke window — multiply by
    // the window span for mm/s^3). Held as sim state, NOT written straight to
    // the engine, because deriveEngineLimits() rebuilds the whole Limits struct
    // on every window/limit change and would otherwise clobber it. Same role as
    // (and same 0-means-derive semantics as) the firmware's jovr /
    // SystemState::sm_tune_jmax: when > 0 it WINS over the mm-domain
    // input_jerk_mm_s3 / window_span derivation; 0 clears it back to derived.
    // Clamped [0 .. 1e6].
    float uiSetJmax(float jmax);
    // Read-back for the `motion` readout / toasts.
    float inputJerkMmS3() const { return _input_jerk; }
    float jerkOverrideNorm() const { return _jmax_norm; }
    // Read-back for the `motion` palette command (engine is ground truth).
    const slopmotion::Config& engineConfig() const { return _engine.config(); }
    float windowSpanMm() const { return windowSpan(); }

    // ---- Motion trace (sim thread; recorded at the 1 ms substep rate) -------
    struct TraceSample {
        float t_s = 0;        // sim seconds
        float pos_mm = 0;     // ACTUAL (SimStepper)
        float tgt_mm = 0;     // engine setpoint fed to the stepper
        float vel_mm_s = 0;   // ACTUAL velocity
        // The COMMANDED move, NORMALIZED 0..1, exactly as it was handed to the
        // engine — the wire's own `target` for a 0x0084 sample or a 0x0085
        // segment, the post-clamp normalized point for a 0x0100 move intent,
        // the leg target for a pattern. Held between commands (a staircase),
        // -1 until the first command of the session.
        //
        // WHY IT IS RECORDED NORMALIZED AND NOT IN mm: mapping normalized ->
        // window is the step under test. The analyzer applies `win_min +
        // c * (win_max - win_min)` itself and overlays the result on tgt_mm;
        // if the mapping (or the window, or a scale factor anywhere upstream)
        // is wrong, the two lines separate. Converting here would bake the
        // suspect arithmetic into the recording and hide the very bug the
        // overlay exists to expose.
        float cmd_norm = -1.0f;
        // The SENDER'S OWN CURVE at this instant, normalized 0..1 — the "raw"
        // analyzer line. Rebuilt from the client's boundary conditions
        // (previous knot, this target, its wire end-velocity) and advanced on
        // the sim clock, NEVER from machine state. cmd_norm is the staircase of
        // knot TARGETS; this is the curve drawn THROUGH those knots.
        //
        // Read the three position lines together and each gap has one meaning:
        //   raw vs tgt : what the planner could not deliver (infeasibility)
        //   tgt vs pos : what the machine could not track (following error)
        // -1 until the first timed segment of the session.
        float raw_norm = -1.0f;
    };
    size_t traceCount() const { return _traceCount; }
    // i = 0 is the OLDEST retained sample.
    const TraceSample& traceAt(size_t i) const {
        return _trace[(_traceHead + kTraceCap - _traceCount + i) % kTraceCap];
    }
    // Writes the whole retained ring as CSV; returns rows written (0 = failed).
    size_t exportTrace(const std::string& path) const;

    // ---- Inbound wire recorder (sim thread; written in onStreamBundle) ------
    // WHY THIS EXISTS: "the machine moves wrong" has two very different causes —
    // the machine renders badly, or the CLIENT SENDS BADLY. The trace ring
    // answers the first. This ring answers the second: every 0x0084 sample and
    // 0x0085 segment exactly as it came off the wire, RAW (pre-scale) and
    // DECODED side by side, so a units bug, a timestamp skew and a skipped
    // script interval are each visible without inference.
    //
    // Same time base as TraceSample::t_s (HostClock µs / 1e6) so a row here
    // lines up with a sample from /api/trace.bin.
    struct IngressRecord {
        float    t_s = 0;             // arrival (sim seconds) — trace time base
        uint16_t channel_id = 0;      // 0x0084 motion-input | 0x0085 motion-segment
        uint16_t raw_target = 0;      // WIRE u16, pre-scale (target × 1e4)
        uint16_t raw_dur_ms = 0;      // WIRE u16 (0x0085 only; 0 on 0x0084)
        int16_t  raw_end_vel = 0;     // WIRE i16 (× 1e3; INT16_MIN = sentinel)
        float    target = 0;          // DECODED normalized target
        uint32_t duration_us = 0;     // DECODED (0x0085 only)
        float    end_vel = 0;         // DECODED normalized end velocity
        bool     has_duration = false;
        bool     has_end_vel = false; // false on 0x0085 == sentinel was sent
        bool     accepted = true;     // false = refused at decode (duration 0)
        bool     ts_clamped = false;  // far-future t_off hit the 250 ms clamp
        uint32_t wire_t_off_us = 0;   // t_off[i] within the bundle
        uint32_t wire_t_us = 0;       // t_base + t_off[i] — the absolute wire stamp
        uint64_t due_us = 0;          // resolved local due time (pacing ring)
        float    due_delta_ms = 0;    // due − now: the sender's timestamp skew
        float    gap_ms = 0;          // arrival Δ from the previous record on
                                      // the SAME channel (−1 = first)
    };

    // Derived red-flag aggregates. These are the numbers that expose a SENDER
    // bug rather than a machine bug — see README "Inbound wire recorder".
    struct IngressStats {
        uint32_t records = 0;          // rows recorded (all channels)
        uint32_t segments = 0;         // 0x0085 rows
        uint32_t samples = 0;          // 0x0084 rows
        uint32_t rejected = 0;         // duration_ms == 0 (refused at decode)
        uint32_t sentinel = 0;         // 0x0085 rows with no end-velocity
        uint32_t dur_floor_10ms = 0;   // duration_ms <= 10 — the MFP plugin's
                                       // floor; a violent infeasible command
        uint32_t dur_under_50ms = 0;   // duration_ms < 50 — BELOW the WAVEFORM
                                       // threshold: silently lands in CHASE
        // Inter-arrival gaps, measured PER CHANNEL and at BUNDLE granularity
        // (only a bundle's first row contributes). Both qualifiers matter: a
        // client running segments AND a chase feed would otherwise interleave
        // into a meaningless mean, and a 20-sample 0x0084 bundle would
        // contribute 19 zero-gaps and drag the mean to nothing.
        struct GapStats {
            uint32_t n = 0;
            float min_ms = 0, mean_ms = 0, max_ms = 0;
        };
        GapStats seg_gap;   // 0x0085 — one segment per bundle in practice
        GapStats smp_gap;   // 0x0084 — bundle arrival cadence
        float    span_s = 0;           // first→last segment arrival span
        // sum(commanded durations) / wall-clock span covered, over 0x0085.
        // ~1.0 = the sender is covering script time contiguously.
        // << 1.0 = SKIPPED script time (segments requested that end long before
        //          the next one arrives) — the primary MFP bug hypothesis.
        // >> 1.0 = the sender is overlapping/preempting its own segments.
        float    duty_ratio = 0;
    };

    size_t ingressCount() const { return _ingressCount; }
    // i = 0 is the OLDEST retained record. Sim-thread readers (the TUI) only —
    // same unlocked-reader rule as traceAt().
    const IngressRecord& ingressAt(size_t i) const {
        return _ingress[(_ingressHead + kIngressCap - _ingressCount + i) % kIngressCap];
    }
    // Cross-thread (HTTP) pulls: newest-last, records with t_s > since_s.
    std::vector<IngressRecord> copyIngressSince(float since_s) const;
    IngressStats ingressStats() const;
    // Drops the recorded rows AND zeroes the aggregates. The stats are
    // cumulative by design, which means a sim left running across idle gaps
    // reports a duty ratio diluted by the silence — reset before the take you
    // actually care about.
    void resetIngress();
    // Zeroes the engine-anomaly counters (total + every kind). Separate from
    // resetIngress on purpose: the wire recorder answers "what did the client
    // send", the anomaly counters answer "what did the planner think of it" —
    // a bench run routinely wants one cleared without losing the other.
    void resetAnomalies();
    // Writes the whole retained ring as CSV; returns rows written (0 = failed).
    size_t exportIngress(const std::string& path) const;

    // THE CSV schema lives here, once. Both the file export and
    // GET /api/segments.csv format through these, so the columns can never
    // drift between the two.
    static const char* ingressCsvHeader();
    static void formatIngressCsvRow(char* buf, size_t cap, const IngressRecord& r);

    // Cross-thread trace snapshot for the browser analysis page (HTTP thread).
    // Returns interleaved f32 records of kTraceStride floats
    // {t_s,pos,tgt,vel,cmd_norm,raw_norm} for samples with t_s > since_s, plus the
    // geometry the page needs for its axes. The sim thread appends under the
    // same mutex (bulk, once per tick).
    // 6 since the sender-curve ("raw") line: {t,pos,tgt,vel,cmd_norm,raw_norm}.
    // The analyzer reads the stride out of the 20-byte header rather than
    // assuming it, so an older page against a newer sim degrades to the
    // columns it knows instead of misreading every row.
    static constexpr uint32_t kTraceStride = 6;
    std::vector<float> copyTraceSince(float since_s, float& max_rail, float& win_min,
                                      float& win_max) const;

    // ---- TUI snapshot -------------------------------------------------------
    struct Snapshot {
        float pos_mm = 0, tgt_mm = 0, vel_mm_s = 0;
        float win_min = 0, win_max = 0, max_rail = 0;
        bool homed = false, homing = false, paused = false, override_on = false, estop = false;
        bool pattern_running = false;
        uint8_t pattern_idx = 0;
        uint8_t engine_mode = 0;      // slopmotion::Mode
        uint8_t engine_plan = 0;      // slopmotion::PlanKind
        uint32_t strokes = 0;
        float distance_m = 0, peak_mm_s = 0;
        uint32_t sync_bundles = 0, sync_samples = 0, sync_enqueued = 0, sync_dropped = 0;
        uint32_t plan_rejected = 0;   // engine.commit() refusals (sim-only metric)
        // Engine anomaly ring: total + per-kind (indexed by AnomalyType).
        uint32_t anomalies = 0;
        std::array<uint32_t, kSmAnomalyKinds> anom_kind{};
        float tick_ms_avg = 0, tick_ms_max = 0;   // measured host loop period
        uint16_t cfg_gen = 1;
        size_t sessions = 0;
        struct Slot {
            bool inUse = false;
            std::string peer;
            bool muted = false;
            uint32_t stream_accepted = 0, stream_dropped = 0;
        };
        std::array<Slot, SlopSimWsPort::kSlots> slots{};
    };
    Snapshot snapshot();

    // Allocation-free change digest for the TUI's redraw gate (full snapshot()
    // builds strings; this is safe to poll every loop iteration).
    struct MotionDigest {
        int32_t pos10 = 0, tgt10 = 0, vel10 = 0;
        uint8_t flags = 0;
        uint8_t sessions = 0;
        // Wire-recorder row count: without it the segment panel would sit stale
        // for up to a second whenever inbound frames arrive but the carriage is
        // NOT moving (unhomed, paused, e-stopped) — precisely the case an
        // operator is staring at the panel to diagnose.
        uint32_t ingress = 0;
        bool operator==(const MotionDigest&) const = default;
    };
    MotionDigest motionDigest() const;

    // Cross-thread read surface for the HTTP facade (its handlers run on the
    // cpp-httplib listener thread and must never touch the hub/engine). The
    // sim thread refreshes the copy each tick under _facadeM.
    struct FacadeStats {
        uint32_t bundles = 0, seg_bundles = 0, samples = 0, enqueued = 0, dropped = 0;
        // Sim-only diagnostics — deliberately NOT folded into `dropped`, which
        // keeps the device's sm_sync_dropped meaning (handoff refusals + gate
        // drops + ingress ring overwrites).
        uint32_t plan_rejected = 0;      // slopmotion::Engine::commit() said no
        uint32_t ts_clamped = 0;         // far-future wire timestamps clamped
        uint32_t substeps_discarded = 0; // host-stall catch-up cap hits (µs lost)
        // Engine anomaly ring drain — DEVICE semantics (SystemState::sm_anomalies
        // + sm_anom_kind), so /api/slopmotion `stats.anomalies_by_kind` matches
        // the firmware's response key for key.
        uint32_t anomalies = 0;
        std::array<uint32_t, kSmAnomalyKinds> anom_kind{};
        float max_rail_mm = 500.0f;
        float tick_ms_avg = 0.0f, tick_ms_max = 0.0f;
        uint8_t stream_speed_mode = 0;
    };
    FacadeStats facadeStats() const;

    slopsync::Hub& hub() { return _hub; }

    // ---- HubDelegate (called from inside _hub.update(), sim thread) --------
    slopsync::AccessLevel validateToken(std::span<const std::byte> instance_id,
                                        std::span<const std::byte> token, bool hasToken) override;
    slopsync::Result<slopsync::IntentValueMap, slopsync::NackCode> applyIntent(
        uint16_t channel_id, const slopsync::IntentValueMap& requested, slopsync::AccessLevel role,
        bool& cfgChanged) override;
    void onEstop(uint8_t cause, uint8_t origin) override;
    bool canClearEstop() override;
    std::optional<uint8_t> sourceForChannel(uint16_t channel_id) override;
    slopsync::SourceLossPolicy sourcePolicy(uint8_t source_id) override;
    void onDeadmanStop(uint8_t source_id) override;
    void onSessionJoined(uint32_t session_id) override;
    void onSessionLeft(uint32_t session_id) override;
    void onStreamBundle(uint16_t channel_id, uint32_t session_id,
                        const slopsync::BundleView& bundle) override;

private:
    // Machine behavior (all sim thread).
    void hardStopMotion();               // WS_OP_HALT seam: forceStop, nothing latched
    void releaseMotion();                // stop + drop the plan (sampler-release model)
    void drainMotionStream(uint64_t now64);
    // Commits one bundle's worth of recorder rows + folds them into the
    // red-flag aggregates. Sim thread; takes _ingressM once for the burst.
    void recordIngress(IngressRecord* rows, uint8_t n);
    // Empties the engine's 16-slot anomaly ring, counting every event by kind.
    // Called once per 1 ms substep (right after the commit point) rather than
    // once per host tick: a 100 ms catch-up burst can commit many plans, and
    // draining only at the end would let the ring overwrite the very events
    // this exists to capture.
    void drainAnomalies(uint64_t now64);
    void tickMachine(uint64_t now64);    // homing timer, pattern, 1 ms sampler+stepper substeps
    void tickPattern(uint64_t now64);
    void syncSafety();
    void publishTelemetry(uint32_t nowMs);
    void deriveEngineLimits();
    float windowSpan() const { return _win_max_mm > _win_min_mm ? _win_max_mm - _win_min_mm : 1.0f; }
    float normToMm(float n) const { return _win_min_mm + n * windowSpan(); }
    float mmToNorm(float mm) const { return (mm - _win_min_mm) / windowSpan(); }

    // Verbatim SystemState::safeSpeedCap (include/system/SystemState.h ~403):
    // the ceiling ramps SAFE_APPROACH_SPEED_MM_S -> configured_max over
    // SAFE_RESUME_RAMP_MS from the last _resume_start_ms stamp.
    float safeSpeedCap(float configured_max, uint32_t now_ms) const;
    // MotionArbiter::_isOutsideWindow — 0.5 mm slack, edge-chatter proof.
    bool isOutsideWindow(float p0_mm) const;
    // RangeMapper::setRange legality: swap, clamp to [0, rail], span >= 5 mm.
    void applyWindowLegality(float min_mm, float max_mm);

    SessionLog& _log;

    // ---- Machine state (mirrors the SystemState slice a UI sees) ------------
    bool _homed = false, _homing = false, _paused = false, _override = false, _estop_latched = false;
    // RFC-025c: the STANDING limit-bypass mode (SystemState::bypass_limits on
    // the device). Distinct from the per-move `bypass` key on 0x0100, which is
    // a one-shot property of one commanded move; this is the machine mode that
    // 0x0003's appended `modes` byte publishes and 0x0005's bypass_on/off write.
    bool _bypass_limits = false;
    uint64_t _homing_done_us = 0;
    float _win_min_mm = 0.0f, _win_max_mm = 500.0f;      // config_api defaults
    float _max_rail_mm = 500.0f;
    float _user_speed = 50.0f, _user_accel = 200.0f;      // gentle set
    // Stream/pattern (INPUT) set. NOT the device factory defaults (550/8000) —
    // these are the operator's OWN HARDWARE defaults (1000 mm/s, 50000 mm/s²),
    // so a sim trace is comparable with a hardware trace out of the box.
    //
    // 50000 IS LOAD-BEARING, not a round number: at 8000 (a value used briefly
    // while chasing an unrelated chase-path bug) essentially every real
    // funscript segment is INFEASIBLE, so the waveform path falls to the Ruckig
    // guard and both curve_policy and the budgeted policies have nothing left to
    // act on. A sim tuned below the operator's real ceiling does not simulate a
    // slower machine, it simulates a machine that never runs the code under test.
    float _input_speed = 1000.0f, _input_accel = 50000.0f;
    // Jerk is a first-class mm-domain limit of the INPUT set, alongside
    // speed/accel: DEFAULT_INPUT_MAX_JERK_MM_S3 (config_api.h). It is a
    // MECHANICAL ceiling (belt/motor), not a smoothing knob — see README.
    float _input_jerk = 2000000.0f;   // mm/s^3
    // Normalized OVERRIDE (units/s^3). 0 = derive from _input_jerk / span,
    // mirroring the firmware's `jovr`. See uiSetJmax.
    float _jmax_norm = 0.0f;
    float _commanded_target_mm = 0.0f;
    // SystemState::commanded_raw_mm — the PRE-PLANNING demand, one stage
    // upstream of _commanded_target_mm. Published as 0x0080's appended
    // `raw_10um` (M5a); the diagnostic graphing CLI plots this against the
    // plan strip and the achieved position.
    float _commanded_raw_mm = 0.0f;
    // TRACE-ONLY mirror of the last normalized target committed to the engine,
    // from ANY source (stream sample, timed segment, move intent, pattern leg).
    // Deliberately NOT _commanded_raw_mm: that one is a byte-mirror of the
    // device's SystemState::commanded_raw_mm and is written only on the paths
    // the firmware writes it, so widening it would break the 0x0080 mirror.
    // This slot answers a different question — "what was the machine last told
    // to do, in the units it was told in" — and feeds TraceSample::cmd_norm.
    float _trace_cmd_norm = -1.0f;
    // ---- Sender-curve shadow (the analyzer's "raw" line) -------------------
    // A second, parallel curve carrying what the CLIENT described. It is
    // advanced entirely in the SENDER'S frame: each segment starts where the
    // PREVIOUS SEGMENT'S CURVE ENDED, not where the machine got to. That is the
    // whole point — if it chased the machine it would silently absorb the
    // planner's shortfalls and the raw-vs-planned gap would always read zero.
    // Anchored to the machine's position once, at the first segment, because
    // the two frames have to agree somewhere.
    void noteSenderCurve(const slopmotion::Command& cmd, uint64_t now64);
    double   _raw_c[6] = {0, 0, 0, 0, 0, 0};
    double   _raw_T = 0.0;
    uint64_t _raw_start_us = 0;
    double   _raw_p = -1.0;        // sender-frame position; <0 = not anchored yet
    double   _raw_v = 0.0;         // sender-frame velocity at the last knot
    double   _raw_prev_vf = 0.0;   // for the af backward difference
    uint64_t _raw_prev_us = 0;
    bool     _raw_prev_ok = false;
    float    _trace_raw_norm = -1.0f;
    // MEASURED stroke (MotorDriver::getMeasuredStrokeMm). 0 = never measured ->
    // effectiveCeilingMm() falls back to the configured rail. The fake homing
    // cycle sets it, exactly like a real sensorless cycle would.
    float _measured_stroke_mm = 0.0f;
    // SystemState::stream_speed_mode — DEVICE DEFAULT SPEED_CEILING_PEGGED (0).
    uint8_t _stream_speed_mode = 0;
    // Mirrors estop_requested: set by onEstop, consumed by the next motion
    // substep (motorTask's RMW on Core 1). canClearEstop refuses while pending.
    bool _estop_pending = false;

    // Stream gating stamps (mirrors buttplugLinearCmd / drainMotionStream).
    uint32_t _last_intiface_ms = 0, _last_cmd_ms = 0, _last_intiface_move_ms = 0, _resume_start_ms = 0;
    float _measured_interval_ms = 0.0f;
    float _syncPrevTarget = -1.0f;

    // Counters (GET /api/slopmotion "sync" block parity). _sync_dropped keeps
    // the DEVICE meaning: gate drops + ingress ring overwrites + bad durations
    // (the firmware's queue-full case can't exist here — the engine is local).
    uint32_t _sync_bundles = 0, _sync_samples = 0, _sync_seg_bundles = 0;
    uint32_t _sync_enqueued = 0, _sync_dropped = 0;
    // Sim-only: planner refusals + far-future timestamp clamps + lost substeps.
    uint32_t _plan_rejected = 0, _ts_clamped = 0, _substeps_discarded = 0;
    uint32_t _lastFarLogMs = 0;   // SLOGW_EVERY_MS(2000) analog
    // Anomaly ring drain. The engine's ring is 16 slots and NOTHING here used
    // to pop it — it filled once and then silently overwrote forever, so the
    // sim was blind to every anomaly the engine reported. Counted per kind
    // (indexed by slopmotion::AnomalyType) exactly like the device's
    // SystemState::sm_anom_kind, so sim and device JSON are diffable.
    uint32_t _anom_total = 0;
    std::array<uint32_t, kSmAnomalyKinds> _anom_kind{};
    uint32_t _lastAnomLogMs = 0;  // SLOGI_EVERY_MS(1000) analog — the log line
                                  // is throttled, the counters above never are.
    // RFC-019: bumped by resetAnomalies(), published as 0x0088's `reset_gen`
    // so EVERY subscriber observes a counter reset, not just whoever asked.
    uint16_t _reset_gen = 0;

    // ---- M5a telemetry cadence + the modeled power plane --------------------
    uint32_t _lastPlanMs = 0, _lastPowerMs = 0;
    bool _planEverSent = false;
    uint8_t _patMask = 0xFF;
    uint32_t _sessionStartMs = 0;
    // A MODELED bus, in exactly the spirit of the "heap": 4 MB / "rssi": -42
    // fictions the 0x0006 publisher already ships. The sim advertises 0x0087
    // because a virtual machine that hid its power plane could not exercise the
    // channel at all — and the probe's job is to prove the WIRE, which is real
    // even when the amps are not. Nothing here claims to be a measurement.
    float _bus_v = 24.0f, _bus_a = 0.0f, _bus_peak_a = 0.0f, _die_c = 32.0f, _energy_wh = 0.0f;

    // Odometer.
    uint32_t _strokes = 0;
    float _distance_mm = 0.0f, _peak_mm_s = 0.0f;
    float _lastOdoVel = 0.0f;

    // ---- Owned, construction order matters (hub binds by reference) ---------
    HostClock _clock;
    HostRandom _rng;
    slopsync::Catalog32 _catalog;
    PacingRing _pacingRing;
    // RFC-029 item 1's ICrypto. ScriptedCrypto is the library's DETERMINISTIC
    // STAND-IN and emphatically NOT ECDSA: the library is std-headers-only by
    // invariant, so real P-256 belongs in the firmware's mbedtls adapter, not
    // here. What the sim exists to prove is the PROTOCOL — that the signature is
    // computed over client_nonce || session_id || boot_id, that it is delivered
    // out-of-band in HUB_SIG, and that a captured one does not verify for a
    // later session. Every one of those is curve-independent, and the probe
    // reimplements this stand-in in Python to check the bytes end to end.
    slopsync::ScriptedCrypto _crypto;
    slopsync::Hub _hub;
    SlopSimWsPort _port;

    slopmotion::Engine _engine;
    SimStepper _stepper;
    SimPattern _pattern;

    uint16_t _cfgGenBump = 0;       // local mirror of applied config generation
    uint64_t _lastSampleUs = 0;     // 1 ms substep cursor
    uint64_t _lastPumpUs = 0;       // 5 ms comms-pump cursor (taskLoop analog)
    uint64_t _lastTickUs = 0;       // host loop period instrumentation
    float _tick_dt_avg_ms = 0.0f, _tick_dt_max_ms = 0.0f;

    mutable std::mutex _facadeM;
    FacadeStats _facade;

    // Trace ring (~4 min at 1 kHz). Heap vector, host RAM is free.
    // _traceM guards ring writes (sim thread, bulk per tick) vs the HTTP
    // thread's copyTraceSince. The TUI reads WITHOUT the lock — it runs on the
    // sim thread itself, so it can never race the writer.
    mutable std::mutex _traceM;
    static constexpr size_t kTraceCap = 240'000;
    std::vector<TraceSample> _trace = std::vector<TraceSample>(kTraceCap);
    size_t _traceHead = 0;   // next write
    size_t _traceCount = 0;

    // Inbound wire recorder ring. Same producer/consumer shape as the trace
    // ring: the SIM thread writes (inside onStreamBundle, which runs inside
    // hub.update()), the HTTP thread reads under _ingressM, the TUI reads
    // unlocked because it IS the sim thread. ~4000 records ≈ 220 KB — host RAM
    // is free, and a generous ring means an operator can stream a whole scene
    // and still have the beginning of it.
    mutable std::mutex _ingressM;
    static constexpr size_t kIngressCap = 4096;
    std::vector<IngressRecord> _ingress = std::vector<IngressRecord>(kIngressCap);
    size_t _ingressHead = 0;    // next write
    size_t _ingressCount = 0;
    IngressStats _ingressStats;
    // Aggregate accumulators (kept out of IngressStats so the published struct
    // stays a pure readout). Index 0 = 0x0084, 1 = 0x0085.
    double _gap_sum_ms[2] = {0.0, 0.0};
    double _seg_dur_sum_ms = 0.0;   // sum of ALL commanded segment durations
    float _seg_last_dur_ms = 0.0f;  // subtracted out for the duty ratio
    float _seg_first_t_s = -1.0f, _seg_last_t_s = -1.0f;
    float _last_arrival_t_s[2] = {-1.0f, -1.0f};  // [0]=0x0084, [1]=0x0085

    bool _cfgDirty = false;  // UI-side config change -> republish 0x0081

    // Telemetry cadence bookkeeping (firmware parity).
    uint32_t _lastMotionMs = 0, _lastSlowMs = 0, _lastPatternMs = 0;
    uint16_t _lastCfgGen = 0;
    bool _cfgEverSent = false;
    bool _patRunning = false;
    uint8_t _patIdx = 0xFF;
    float _patSpeed = -1, _patDepth = -1, _patStroke = -1, _patSensation = -1;
    uint16_t _estopSeq = 0;
};

}  // namespace slopsim
