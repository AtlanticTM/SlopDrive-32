#pragma once

#include <atomic>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config_api.h"
#include "MotorDriver.h"           // for DriverConfig
#include "MotionInterpolator.h"    // for InterpAnomaly (cross-core ring)

// ============================================================================
// On-device motion generator configuration
// ============================================================================
// Held in RAM; parameters pushed live from the web UI via /api/gen.
//
// gen_mux (portMUX spinlock) serialises writes from Core 0 (WebUI handler)
// and reads from Core 1 (generator task) so that the task always sees a
// complete, consistent snapshot — no torn field-by-field reads mid-update.
//
// Waveform: 0=sine 1=triangle 2=square 3=saw
// Mod:      0=off  1=rate(FM) 2=depth(AM)
// ModWave:  0=sine 1=triangle 2=random
struct GeneratorConfig {
    bool    running   = false;
    uint8_t wave      = 0;     // carrier shape
    float   rate_hz   = 0.8f;  // strokes per second
    float   depth     = 0.80f; // 0..1 fraction of window used
    float   offset    = 0.50f; // 0..1 center position within window
    float   ease      = 0.0f;  // 0..1 smoothing of carrier ends
    uint8_t mod       = 0;     // modulator type
    uint8_t mod_wave  = 0;     // modulator shape
    float   mod_rate  = 0.10f; // modulator rate (Hz)
    float   mod_amp   = 0.80f; // modulation amplitude / swing (Hz)
};

// ============================================================================
// Input control mode (how raw Intiface samples become motion)
// ============================================================================
//
// Two strategies, selectable from the web UI:
//
//   EXTRAPOLATE (the original behavior): each incoming sample is sized to the
//     measured command cadence (Auto Duration) and projected slightly ahead via
//     streamExtrapolated(). Excellent when the app sends at a STABLE rate.
//
//   BUFFERED: we push incoming samples into a small ring buffer and a local
//     timer task (20/50/100 Hz) interpolates between consecutive buffered points
//     with a selectable easing curve. Playback is deliberately delayed by
//     `depth` samples so there's always a "next" point to head toward, which
//     turns SPORADIC/bursty packet streams into smooth continuous motion.
//
// Auto Duration is meaningful only in EXTRAPOLATE mode; in BUFFERED mode the
// local tick rate + buffer depth control timing instead.
enum class InputMode : uint8_t { EXTRAPOLATE = 0, BUFFERED = 1 };

// ============================================================================
// Sample ring entry (BUFFERED input mode)
// ============================================================================
struct BufSample {
    float    pos_mm;
    uint32_t t_ms;
};

// ============================================================================
// SlopMotion anomaly kind names — THE ONE TABLE
// ============================================================================
// Index == slopmotion::AnomalyType ordinal. Both consumers read this and only
// this: the Core-1 drain log line (src/main.cpp) and the
// GET /api/slopmotion `stats.anomalies_by_kind` JSON keys (src/ui/WebUI.cpp).
// Two tables would silently drift the moment the engine grows a kind, and a
// log that says one thing while the API says another is worse than neither.
//
// Deliberately snake_case: JSON keys want it, and matching log text means an
// operator can grep the device log for the exact key they saw in the API.
//
// Bounds are ALWAYS derived from this table's own size — never a literal. That
// is what makes adding a kind here (and only here) the whole change; a kind the
// engine reports past the end of the table prints as "?<n>" rather than "?", so
// an out-of-date table is self-identifying instead of anonymous.
//
// NOT engine-header-derived on purpose: SystemState.h stays slopmotion-free
// (same rule as sm_tune_infeas_policy's plain uint8_t just below).
inline constexpr const char* kSmAnomalyNames[] = {
    "none",                // AnomalyType::None              = 0
    "plan_failed",         //              ::PlanFailed      = 1
    "settle",              //              ::SettleEngaged   = 2
    "endvel_clamped",      //              ::EndVelClamped   = 3
    "deadline_stretched",  //              ::DeadlineStretched = 4
    "waveform_fallback",   //              ::WaveformFallback  = 5
    "waveform_scaled",     //              ::WaveformScaled    = 6
    "waveform_centred",    //              ::WaveformCentred   = 7
    "handoff_bounded",     //              ::HandoffBounded    = 8
    "waveform_smoothed",   //              ::WaveformSmoothed  = 9
};
inline constexpr uint8_t kSmAnomalyNameCount =
    uint8_t(sizeof(kSmAnomalyNames) / sizeof(kSmAnomalyNames[0]));

// ============================================================================
// SystemState — centralised, thread-safe runtime state container
// ============================================================================
//
// All mutable runtime globals that were previously file-scope statics in
// main.cpp live here.  Scalars crossing Core0 ↔ Core1 are marked volatile
// (32-bit aligned loads/stores are hardware-atomic on ESP32-S3).  The ring
// buffer and generator config use portMUX critical-section protection for
// multi-field updates that must appear atomic.
//
// Fields are grouped by subsystem; initial values mirror the original file-
// scope defaults exactly.

struct SystemState {

    // ---- Buffered-sample ring (BUFFERED mode) --------------------------------
    static constexpr uint8_t BUF_CAP = 8;   // ring capacity (≥ max depth + slack)

    BufSample              buf[BUF_CAP] {};
    volatile uint8_t       buf_head   = 0;   // next write index
    volatile uint8_t       buf_count  = 0;   // valid samples in ring
    portMUX_TYPE           buf_mux    = portMUX_INITIALIZER_UNLOCKED;

    // Interpolator playback state — DORMANT (Interpolator engine removed)
    // The ring-buffer config fields (buf_easing, buf_depth, buf_tick_hz) are
    // retained for persisted settings compatibility and the WebUI config card.
    // buf_active is always false now — no engine is behind it to thrust. :3
    volatile bool          buf_active         = false;
    // ---- Loading / flow (cross-core) -----------------------------------------
    volatile bool          homed               = false;
    volatile bool          homing_in_progress  = false;
    // Item 3 (fw 2.1.76): raised by motorTask (Core 1) the instant a homing
    // cycle completes successfully (homed transitions false -> true), so the
    // freshly-measured stroke gets persisted to NVS on EVERY home, not only
    // whenever the operator next hits Save. NVS writes are flash I/O and must
    // never run on the real-time core (motion doctrine §2), so this is only a
    // flag — SlopSyncHubService's Core-0 1 Hz tick does the actual
    // ConfigStore::save() and clears it. Same volatile-bool cross-core
    // contract as homed/homing_in_progress above.
    volatile bool          stroke_measured_pending = false;
    // WebUI "home override" (bench test, no motor): when >0 the UI is told the
    // machine is homed and this value is reported as the measured stroke (mm) so
    // the rail populates without a real homing cycle. 0 = normal (use motor). :3
    volatile float         test_stroke_override_mm = 0.0f;
    std::atomic<bool>      estop_requested{false};        // Core 0 stores, Core 1 exchanges — atomic RMW closes TOCTOU window
    // Latched "device is in an e-stopped state" flag. estop_requested is a
    // transient REQUEST (motorTask consumes it within ~1ms), so telemetry
    // sampling at ~45Hz almost never catches it — the UI could never learn an
    // e-stop happened from another client/transport. This latch is set when an
    // e-stop is requested, cleared when a new homing cycle starts, and is what
    // the 0x01 telemetry flags bit3 actually reports. :3
    volatile bool          estop_latched = false;
    bool                   wifi_ready          = false;   // Core 0 only

    // ---- OTA update in flight (cross-core, Core 0 sets/clears) ----------------
    // Raised by OtaService::prepareForOta() the instant an over-the-air update
    // is accepted, BEFORE the first flash write. While true, ConfigStore::save()
    // (the only NVS flash writer reachable from the gated state) DEFERS instead
    // of writing — a flash-cache access during an OTA write window can reset the
    // chip. Cleared on OTA failure; on success the device reboots so the flag is
    // moot. Read/written only on Core 0 but atomic for a clean cross-core read
    // barrier from any diagnostic path. :3
    std::atomic<bool>      ota_active{false};

    // ---- WiFi link telemetry (Core 0 only — written by TransportManager's
    // event handler + poll timer, read by WebUI::handleApiStatus. Both run on
    // Core 0 (WiFi event task + httpTask), so no cross-core mutex needed —
    // this is a simple diagnostic readout, not a control path. :3 ----------
    int8_t                 wifi_rssi                = 0;      // dBm, 0 = unknown
    uint8_t                wifi_channel             = 0;
    char                   wifi_bssid[18]           = {0};    // "AA:BB:CC:DD:EE:FF\0" — proves which AP we're actually on (band-steering evidence)
    uint32_t               wifi_reconnects          = 0;      // count of STA_DISCONNECTED events since boot
    uint8_t                wifi_last_disconnect_reason = 0;   // esp_wifi disconnect reason code
    uint32_t               wifi_last_disconnect_ms  = 0;      // millis() at last disconnect

    // ---- Config snapshots ----------------------------------------------------
    DeviceConfig           config;                        // read cross-core
    DriverConfig           driver;                        // Core 0 only

    // ---- Control gating (cross-core) -----------------------------------------
    volatile bool          paused               = false;
    volatile bool          manual_override      = false;
    volatile uint32_t      resume_start_ms      = 0;
    bool                   expert_mode          = false;   // Core 0 only

    // ---- Transport (cross-core) ----------------------------------------------
    volatile uint8_t       transport = static_cast<uint8_t>(DEFAULT_TRANSPORT_MODE);

    // ---- Intiface parser workaround (cross-core) -----------------------------
    // Intiface's buttplug→TCode bridge does fuckshit to the magnitude: instead
    // of the spec-correct variable-digit fraction (L0500 = 0.500) that MFP and
    // the TCode v0.3 spec use, it emits values that only decode correctly when
    // scaled against the legacy fixed /999 magnitude ceiling. With the normal
    // digit-count decode, an Intiface "full depth" command lands shallow and
    // the stroke never fully gapes. :3
    //
    // When TRUE: parser scales magnitude as mag / TCODE_MAGNITUDE_MAX (legacy
    //            Intiface/buttplug convention).
    // When FALSE (default): parser uses spec-correct mag / 10^digits — the
    //            decode MFP needs. We had a hard-coded Intiface fix before and
    //            it broke MFP; a per-source toggle lets each app get what it
    //            wants without one stealing the other's lube. :3
    volatile bool          intiface_compat      = false;

    // ---- Cadence / auto-duration ---------------------------------------------
    volatile bool          auto_duration        = true;
    uint32_t               last_cmd_ms          = 0;      // Core 1 only
    volatile float         measured_interval_ms = 0.0f;   // written Core 0 (commsTask), read Core 1 — mark volatile (F-024)
    volatile uint16_t      measured_hz          = 0;      // written Core 1, read Core 0

    // ---- Default range (Core 0 only) -----------------------------------------
    float                  default_range_min = 0.0f;
    float                  default_range_max = DEFAULT_MAX_RAIL_MM;

    // ---- On-device motion generator ------------------------------------------
    GeneratorConfig        gen;
    portMUX_TYPE           gen_mux     = portMUX_INITIALIZER_UNLOCKED;

    // Generator runtime phase/clock (Core 1 only — generatorTask)
    float                  gen_phase      = 0.0f;
    uint32_t               gen_last_us    = 0;
    float                  gen_mod_clock  = 0.0f;

    // Whether the generator is actually emitting motion (cross-core)
    volatile bool          gen_active      = false;

    // Timestamp of last Intiface command (cross-core!). Written on Core 0
    // (commsTask → buttplugLinearCmd/buttplugStop in main.cpp), read on Core 1
    // in hot gating paths (streamSamplerTask's recent-packet gate and the
    // arbiter's Intiface-recency gate in _gatesPass). 32-bit aligned store is
    // hardware-atomic on the S3; volatile keeps the Core-1 reads fresh. The
    // old "Core 1 only" comment here was factually wrong. :3
    volatile uint32_t      last_intiface_ms = 0;

    // ---- Stream-vs-pattern arbitration (yield on MOTION, not packets) --------
    // Streaming hosts (Intiface/XToys) keep sending position packets as
    // keep-alives while connected, so packet recency alone held the pattern
    // gated off FOREVER after any stream session ("running" in the UI, machine
    // dead still). last_intiface_move_ms stamps only packets that actually
    // MOVED the commanded target (>0.3% of the window); the pattern yields to
    // that, and a user-started pattern (pattern_running) reclaims the sampler
    // when the stream stops driving. Last active driver wins — both ways.
    volatile uint32_t      last_intiface_move_ms = 0;  // Core 0 writes, Core 1 reads
    volatile bool          pattern_running       = false; // PatternEngine user start/stop

    // ---- Bypass-limits toggle (cross-core, Core 0 writes) --------------------
    // Set by WS_OP_BYPASS, honored by applyMove when a move doesn't carry its
    // own per-request bypass_limits field, exposed in WS_OP_GET_CFG so clients
    // can resync the real state on reconnect. Session-only (not persisted) —
    // a reboot always comes back with limits enforced. :3
    volatile bool          bypass_limits = false;

    // ---- Commanded target (cross-core) ---------------------------------------
    // The position the host/generator just TOLD us to go to (mm), before FAS
    // has actually pounded its way there. Written by whoever issues the move
    // (buttplugLinearCmd on the TCode path, generatorTask on the gen path),
    // read by Core 0's telemetry capture so the UI can draw "what we were asked
    // for" right next to "where the shaft actually is." 32-bit aligned float =
    // hardware-atomic on the S3, no mutex needed. :3
    volatile float         commanded_target_mm = 0.0f;

    // ---- Raw parsed target (cross-core) --------------------------------------
    // The position the TCode parser + RangeMapper spat out (mm), BEFORE the
    // kinematics planner gets its hands on it. This is the rawest "what the host
    // actually asked for, mapped into our stroke window" — one stage upstream of
    // commanded_target_mm (which is the planner's clamped/shaped result). Drawing
    // all three (raw → planned → actual) side by side lets us see exactly which
    // stage mangles the motion path. Same hardware-atomic float deal, no mutex. :3
    volatile float         commanded_raw_mm = 0.0f;

    // ---- Actual dispatched position (cross-core) -----------------------------
    // Written ONLY by WebUI::applyMove's seed store (Core 0, after a manual
    // point move lands) — NOT by the arbiter/planner or the telemetry sampler;
    // MotionArbiter deliberately never writes this from motion dispatch (see
    // the D4 comments in MotionArbiter.cpp/PatternEngine.cpp), and the live
    // position readout goes through _motor.getPosition() directly. Read by
    // Core 1's streamSamplerTask (main.cpp) on a stream's rising edge, to seed
    // the interpolator from the last known manual endpoint.
    // std::atomic<float> gives the no-tear guarantee with zero overhead on S3.
    // memory_order_relaxed is correct — telemetry is display-only, no ordering
    // dependency with any other variable. :3
    std::atomic<float>     actual_position_mm{0.0f};

    // ---- Session odometer stats (single-writer: WebUI::captureTelemetry 240Hz) --
    // Derived from the position stream: live/peak speed, cumulative distance, and
    // a stroke (direction-reversal) count. Read by the 0x06 STATS frame + the
    // dashboard SESSION card. Only the telemetry timer writes them; every other
    // core only loads → relaxed read-modify-write in the single writer is safe.
    // Zeroed by resetSessionStats() (also resets the INA228 Wh accumulator). :3
    std::atomic<float>     live_speed_mm_s{0.0f};    // EMA-smoothed current speed
    std::atomic<float>     max_speed_mm_s{0.0f};     // session peak speed
    std::atomic<float>     session_distance_mm{0.0f};// cumulative |Δposition|
    std::atomic<uint32_t>  stroke_count{0};          // direction reversals
    volatile uint32_t      session_start_ms{0};      // millis() at boot / last reset

    // ---- WS UI config generation counter (Core 0 only, atomic) ---------------
    // Incremented on every applied settings change (HTTP, WS, serial/BLE).
    // Sent in HELLO and 0x02 STATUS so clients can detect stale state and
    // request a full resync via get_cfg. uint16_t wraps harmlessly — clients
    // detect a difference, not a direction. :3
    std::atomic<uint16_t>  cfg_gen{0};


    // Generator local tick rate (cross-core)

    // Default cadence we pound at — 100 Hz gives buttery-smooth position updates
    // without making the S3 break a sweat. Bump to 200 if you're feeling greedy. :3
    volatile uint16_t      gen_rate_tick_hz = 100;
    
    // ---- Input mode & buffer tuning (cross-core) -----------------------------
    volatile uint8_t       input_mode   = static_cast<uint8_t>(InputMode::BUFFERED);
    volatile uint8_t       buf_easing   = 1;    // 0=linear 1=ease-in-out …
    volatile uint8_t       buf_depth    = 2;    // 1..5 samples of look-behind
    volatile uint16_t      buf_tick_hz  = 100;   // local interpolation rate (Hz)

    // ---- Stream sampler speed-feed mode (cross-core) -------------------------
    // Selects how the Core-1 streamSamplerTask feeds FAS speed each tick while
    // following the MotionInterpolator's cubic. Written by Core 0 (WebUI toggle),
    // read by Core 1 (sampler). 32-bit read/write is hardware-atomic on the S3.
    //   0 = CEILING_PEGGED  (default): feed a constant high speed; the 1kHz
    //       micro-target position deltas themselves shape velocity. Keeps the
    //       57AIM grit-cache quiet (speed/accel steady → no FAS ramp re-plan).
    //   1 = VELOCITY_MATCHED: feed |interp velocity| each tick so FAS coasts the
    //       exact cubic speed. Truer curve, but rewrites setSpeedInHz per tick.
    // Exposed as a live A/B toggle so it can be felt on real hardware. :3
    enum StreamSpeedMode : uint8_t { SPEED_CEILING_PEGGED = 0, SPEED_VELOCITY_MATCHED = 1 };
    volatile uint8_t       stream_speed_mode = SPEED_CEILING_PEGGED;

    // ---- Interpolator overshoot clamp (cross-core) ---------------------------
    // WebUI toggle. When true, Core 1's streamSamplerTask pushes the flag into
    // the MotionInterpolator so the v4 gradient cubic's Hermite tangents get
    // monotone-limited (Fritsch–Carlson) before setCubic — the invented
    // overshoot-then-return micromotion is eliminated at the cost of slightly
    // softer MFP slope shaping. Written by Core 0 (handler), read by Core 1
    // (sampler). 32-bit aligned bool → hardware-atomic on the S3, no mutex. :3
    volatile bool          interp_clamp_overshoot = false;

    // ---- Interpolator telemetry (cross-core, display-only) -------------------
    // Written by Core 1 (streamSamplerTask) once per tick from the live
    // MotionInterpolator snapshot; read by Core 0 telemetry for the WebUI's
    // high-refresh planned-path / interp-state overlay. Each field is an
    // independently-readable aligned scalar — no lock needed for a display feed
    // (a torn set across fields is visually harmless at UI refresh rates). :3
    volatile float         interp_start_pos   = 0.5f;  // segment start (0..1)
    volatile float         interp_end_pos     = 0.5f;  // segment target (0..1)
    volatile float         interp_cur_pos     = 0.5f;  // sampled position (0..1)
    volatile float         interp_cur_vel     = 0.0f;  // units/second (signed)
    volatile uint32_t      interp_duration_us = 0;     // active segment length
    volatile uint32_t      interp_elapsed_us  = 0;     // time into segment
    volatile bool          interp_live_mode   = false; // v3 high-rate live extrapolation
    volatile bool          interp_grad_mode   = false; // v4 G<slope> gradient segment
    volatile uint8_t       interp_style       = 0;     // InterpStyle enum
    volatile bool          interp_active       = false; // sampler currently driving motion

    // ---- Interpolator anomaly ring (cross-core) ------------------------------
    // Core 1's streamSamplerTask drains the MotionInterpolator's local anomaly
    // ring each tick and publishes events here; Core 0's UiSocket sender drains
    // them into 0x05 ANOMALY frames. Seq-counter ring identical in spirit to the
    // telemetry ring: the producer bumps anom_write, the consumer tracks how far
    // it has read. portMUX serialises the multi-field InterpAnomaly copy so a
    // reader never sees a torn event. Overflow (producer laps consumer by > CAP)
    // is clamped on the read side — oldest unread events are dropped, newest win,
    // since a fresh anomaly is always more actionable than a stale one. :3
    static constexpr uint8_t ANOM_CAP = 32;
    InterpAnomaly          anom_ring[ANOM_CAP] {};
    volatile uint32_t      anom_write = 0;   // total events ever enqueued (Core 1)
    volatile uint32_t      anom_read  = 0;   // total events drained    (Core 0)
    portMUX_TYPE           anom_mux   = portMUX_INITIALIZER_UNLOCKED;

    // ---- SlopMotion live tuning (ROUGH-IN — WebUI card lands with the UI
    // refactor; until then this is driven by GET/POST /api/slopmotion). -------
    // Core 0 (HTTP handler) writes the sm_tune_* fields; Core 1's sampler
    // pushes them into the slopmotion::Engine every tick, so a POST takes
    // effect within ~1 ms. Aligned 32-bit scalars, single writer per field —
    // same lock-free pattern as interp_clamp_overshoot above. NOT persisted:
    // reboot restores compile-time defaults (deliberate for a tuning session).
    // jmax joined vmax/amax as an OVERRIDE in fw 2.1.47 — the real ceiling now
    // derives from config.input_max_jerk_mm_s3 / window span, so this field is
    // a bench knob of the same shape as the two below, not the source of truth.
    volatile float         sm_tune_jmax_ovr    = 0.0f;   // 0 = derive from config.input_max_jerk_mm_s3 / span;
                                                         // >0 = normalized override (units/s^3) for tuning sessions
    volatile float         sm_tune_vmax_ovr    = 0.0f;   // >0 overrides mm-derived vmax (units/s)
    volatile float         sm_tune_amax_ovr    = 0.0f;   // >0 overrides mm-derived amax (units/s^2)
    volatile bool          sm_tune_chase_ff    = true;   // chase velocity feedforward
    volatile bool          sm_tune_chase_aff   = true;   // chase curvature (accel) feedforward
    volatile float         sm_tune_chase_gain  = 0.9f;   // estimate damping 0..1.5
    volatile float         sm_tune_chase_look  = 3.0f;   // predictive aim, intervals
    volatile uint32_t      sm_tune_dense_us    = 60000;  // dense-stream gate (mean interval)
    // Infeasible-segment policy: what gives when a commanded stroke cannot
    // physically happen in its commanded duration. 0 = Stretch (range-first:
    // keep the full stroke, overrun the deadline), 1 = Scale (timing-first +
    // shape-first: keep the deadline, shrink the stroke to a legal quintic),
    // 2 = Reshape (timing-first + machine-first: keep the deadline AND as much
    // range as the machine can physically deliver, giving up the SHAPE — a
    // Ruckig time-optimal move, bisected toward the midpoint only if even that
    // does not fit). Mirrors slopmotion::InfeasiblePolicy — kept as a plain
    // uint8_t so SystemState.h stays engine-header-free; the mapping to the
    // enum lives in main.cpp's per-tick config push, and anything out of range
    // there falls back to the ENGINE's own default rather than silently
    // picking a policy the operator never asked for.
    volatile uint8_t       sm_tune_infeas_policy   = 2;      // default: Reshape (engine 0.4.0)
    volatile float         sm_tune_infeas_margin   = 0.92f;  // stroke-scale margin 0.50..1.00
    // RESHAPE bisection depth: each step halves the remaining stroke interval,
    // so N steps resolve the delivered stroke to stroke/2^N. Each step costs
    // ONE Ruckig calculate() — this is a direct plan-time budget dial, clamped
    // [0, 8] (0 = no bisection: full amplitude when it fits, guard when not).
    volatile uint8_t       sm_tune_reshape_steps   = 6;      // slopmotion default
    // Settle grace: how long an expired plan may HOLD its end state before the
    // engine concludes the stream is starved and brakes to rest. Microseconds
    // here (engine units); the /api/slopmotion surface talks MILLISECONDS.
    // 0 = pre-0.4 behaviour (brake the instant the plan expires).
    volatile uint32_t      sm_tune_settle_grace_us = 30000;  // 30 ms, slopmotion default
    volatile bool          sm_tune_aim_extrap      = true;   // 2nd-order chase aim (crest overshoot)
    // DC centring of a degraded band (WAVEFORM path, Scale + Reshape policies).
    // When the machine cannot deliver the commanded amplitude on the commanded
    // clock, ON (engine default) shrinks the achieved band SYMMETRICALLY about
    // the commanded midpoint instead of letting it walk off one end — every such
    // stroke is reported as a WaveformCentred anomaly, so the deviation is
    // visible, never silent. OFF restores the slopmotion 0.4.0 contract.
    volatile bool          sm_tune_centring        = true;   // slopmotion wave_centering
    // Correction strength 0..1 (clamped both here and in the engine). 1 = full
    // centring, 0 = same as the bool off. A FEEL dial, not a calibration, and
    // deliberately not monotone — the debt loop closes around the pull it
    // actually applied, so the mid settings are for experimenting only.
    volatile float         sm_tune_centring_gain   = 1.0f;   // slopmotion wave_centering_gain
    // RFC-008 handoff sanity guard: the Fritsch-Carlson chord factor k applied
    // to an inbound segment's end velocity against the FOLLOWING segment's
    // chord. 1.5 = the shape-preserving bound (engine default); 0 DISABLES the
    // guard, which is the A/B switch for comparing machine-side bounding
    // against a client that still carries its own limiter (the MFP plugin's
    // SegHandoffLimiterEnabled is the other half of that experiment). Clamped
    // [0, 8] here AND in the engine — a config push is not a trusted input.
    volatile float         sm_tune_handoff_k       = 1.5f;   // slopmotion handoff_chord_factor
    // Which CURVE FAMILY the waveform path rebuilds a segment with. A funscript
    // rendered through Pchip/Makima is a C1 CUBIC Hermite spline, and a C2
    // quintic cannot reproduce one across a knot by construction — the script's
    // acceleration genuinely STEPS there. 0 = FollowClient (honour the sender's
    // declared family; no wire signalling exists yet, so today it resolves to
    // C2 — pre-0.8.0 behaviour byte for byte), 1 = ForceC1 (cubic), 2 = ForceC2
    // (quintic, always). Plain uint8_t for the same reason as
    // sm_tune_infeas_policy: SystemState.h stays engine-header-free, and the
    // mapping to slopmotion::CurvePolicy lives in main.cpp's per-tick push,
    // where an out-of-range value falls back to the ENGINE's own default rather
    // than silently picking a family the operator never asked for.
    volatile uint8_t       sm_tune_curve_policy    = 0;      // default: FollowClient (engine 0.8.0)
    // Budgeted-policy spend limits (PrioritizeAmplitude / PrioritizeSmooth
    // only — inert under the other three). Both are FRACTIONS in [0, 1] of how
    // much of one axis the policy may spend before it starts spending the
    // other:
    //   smooth budget    — max alpha, i.e. how far the span's END HANDLE may be
    //                      lerped toward its own chord slope. 0 = never touch
    //                      the sender's curve; 1 = the flattest quintic
    //                      reachable from the machine's ACTUAL state, which is
    //                      NOT the same thing as a straight line.
    //   amplitude budget — max fraction of the COMMANDED stroke that may be
    //                      surrendered. 0.5 = may shrink to the segment
    //                      midpoint; 1.0 = may decline to move at all.
    // 0.5/0.5 are the engine's defaults, so a fresh boot changes nothing.
    volatile float         sm_tune_smooth_budget   = 0.5f;   // slopmotion infeasible_smooth_budget
    volatile float         sm_tune_amp_budget      = 0.5f;   // slopmotion infeasible_amplitude_budget
    // Bisection depth on the alpha search. Each step costs ONE quintic build +
    // one legality scan (no Ruckig call), so it is far cheaper per step than
    // sm_tune_reshape_steps. Clamped [1, 10] here AND in the engine; 6 resolves
    // alpha to 1/64 of the budget, well under anything perceptible.
    volatile uint8_t       sm_tune_blend_steps     = 6;      // slopmotion infeasible_blend_steps

    // ---- SlopMotion telemetry back-channel (Core 1 writes, Core 0 reads) ----
    volatile float         sm_eff_vmax    = 0.0f;  // applied normalized ceilings
    volatile float         sm_eff_amax    = 0.0f;  //   (post-derivation/override)
    volatile float         sm_eff_jmax    = 0.0f;  //   — jerk joined the family in 2.1.47
    volatile uint32_t      sm_plans       = 0;     // successful plans since stream seed
    volatile uint32_t      sm_failures    = 0;     // rejected plans since stream seed
    volatile uint32_t      sm_anomalies   = 0;     // total anomaly events (all kinds)
    // Per-kind breakdown, indexed by slopmotion::AnomalyType. The scalar total
    // above stays for back-compat; this is the diagnostic surface — "42
    // anomalies" told an investigation nothing, "40 waveform_scaled + 2
    // endvel_clamped" tells it everything.
    // WHY 10: the enum ends at WaveformSmoothed = 9 (10 values, 0..9) as of
    // slopmotion 0.8.0, so this array is EXACTLY FULL again — there is no spare
    // slot. A kind 10 REQUIRES bumping this constant, appending to
    // kSmAnomalyNames, appending a per-kind field to the 0x0088 slopmotion-diag
    // layout (SlopSyncCatalog.h) and mirroring both in sim/slopsim, all in the
    // same change; until they move, the Core-1 drain loop bounds-checks against
    // kSmAnomalyNames and DROPS the kind rather than making a stray write, and
    // the log prints it as "?10".
    //
    // The 0x0088 half of that list is an ETAG CHANGE, not an append: the
    // per-kind block sits in the MIDDLE of that layout, so a tenth counter
    // shifts every field after it (the ninth already moved plan_us_*/sync_*/
    // reset_gen by 4 B at M4d, and the tenth moved them another 4 — 84 -> 88).
    static constexpr uint8_t SM_ANOM_KINDS = 10;
    static_assert(kSmAnomalyNameCount <= SM_ANOM_KINDS,
                  "kSmAnomalyNames outgrew sm_anom_kind — bump SM_ANOM_KINDS");
    volatile uint32_t      sm_anom_kind[SM_ANOM_KINDS] = {};
    volatile uint8_t       sm_mode        = 0;     // slopmotion::Mode
    volatile uint8_t       sm_plan_kind   = 0;     // slopmotion::PlanKind
    // Plan-time bench (the software-double cost, measured where it runs):
    volatile uint32_t      sm_plan_us_last = 0;
    volatile uint32_t      sm_plan_us_max  = 0;    // POST {"reset_stats":true} clears
    volatile float         sm_plan_us_avg  = 0.0f; // EMA(0.1)

    // ---- SlopSync motion-input (0x0084) stream counters — the SlopSyncHub
    // Core-0 task writes all four (onStreamBundle + taskLoop's drain), the
    // HTTP handler (also Core 0) reads them for GET /api/slopmotion. Same
    // aligned-scalar single-writer-per-field pattern as sm_tune_* above; not
    // persisted, reboot zeroes them. ------------------------------------------
    volatile uint32_t      sm_sync_bundles  = 0;  // accepted onStreamBundle() calls (both stream channels)
    volatile uint32_t      sm_sync_samples  = 0;  // total decoded samples across those bundles
    volatile uint32_t      sm_sync_enqueued = 0;  // samples handed to the Core-1 sampler queue
    volatile uint32_t      sm_sync_dropped  = 0;  // ring-overwrite + queue-full + gate + bad-duration drops (combined)
    volatile uint32_t      sm_sync_seg_bundles = 0;  // subset of _bundles that arrived on 0x0085 motion-segment (waveform)

    // ---- RFC-019 observable reset generation --------------------------------
    // Bumped by the ONE writer that clears the counters above
    // (POST /api/slopmotion {reset_stats}, Core 0). Published as 0x0088's
    // `reset_gen` field so EVERY subscriber sees that a reset happened, not
    // just the session that asked for one — without it a watcher observes the
    // counters jump backwards and cannot tell a reset from a reboot or a wrap.
    volatile uint16_t      sm_reset_gen = 0;

    // ---- SlopMotion anomaly EVENT hand-off: Core 1 -> Core 0 ----------------
    // The 0x0089 motion-anomaly EVENT channel's feed. Core 1's streamSamplerTask
    // drains slopmotion's own ring (it must — the engine lives there), but it
    // may NEVER publish: the slopsync Hub is touched by exactly one task, the
    // Core-0 "SlopSyncHub" task (the one-task invariant SlopSyncWsTransport.h
    // documents), so an anomaly has to cross cores as DATA before it can become
    // a frame.
    //
    // Single-producer/single-consumer by construction — one writer task, one
    // reader task, monotonically increasing indices, modulo only at access.
    // NEWEST LOSES on overflow rather than overwriting the oldest: overwriting
    // a slot the consumer may be mid-copy is the only way this could tear, and
    // an anomaly FLOOD is exactly when the earliest events are the diagnostic
    // ones. Drops are counted, never silent.
    struct SmAnomalyRec {
        uint32_t t_us   = 0;    // engine time, low 32 bits
        uint16_t seq    = 0;    // engine's rolling event id
        uint8_t  kind   = 0;    // slopmotion::AnomalyType
        float    target = 0.0f;
        float    detail = 0.0f;
    };
    static constexpr uint32_t SM_ANOM_RING = 16;
    SmAnomalyRec           sm_anom_ring[SM_ANOM_RING] = {};
    std::atomic<uint32_t>  sm_anom_head{0};      // producer cursor (Core 1)
    std::atomic<uint32_t>  sm_anom_tail{0};      // consumer cursor (Core 0)
    std::atomic<uint32_t>  sm_anom_evt_dropped{0};

    // Producer side (Core 1 ONLY). Returns false when the ring is full.
    bool smAnomalyPush(const SmAnomalyRec& r) {
        const uint32_t head = sm_anom_head.load(std::memory_order_relaxed);
        const uint32_t tail = sm_anom_tail.load(std::memory_order_acquire);
        if (head - tail >= SM_ANOM_RING) {
            sm_anom_evt_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        sm_anom_ring[head % SM_ANOM_RING] = r;
        // Release: the slot's stores must be visible before the consumer can
        // see the cursor that publishes them.
        sm_anom_head.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side (Core 0 ONLY).
    bool smAnomalyPop(SmAnomalyRec& out) {
        const uint32_t tail = sm_anom_tail.load(std::memory_order_relaxed);
        if (tail == sm_anom_head.load(std::memory_order_acquire)) return false;
        out = sm_anom_ring[tail % SM_ANOM_RING];
        sm_anom_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // ---- SlopLog -> SlopSync log channel (0x0008) hand-off: httpTask -> hub --
    // RFC-017, M5b item 1. The SlopLog drain (applogDrain()) runs on httpTask;
    // slopsync::Hub is single-task by contract and is touched ONLY by the Core-0
    // "SlopSyncHub" task. So a log line must cross tasks as DATA before it can
    // become an EVENT frame — exactly the shape the 0x0089 anomaly ring above
    // uses, and deliberately the SAME shape rather than a second invention.
    //
    // Producer: the SlopSyncSink in AppLog.cpp (httpTask, and during boot the
    // setup() task while immediate-drain is on — still a single producer at any
    // instant, because immediate-drain is switched off before task creation).
    // Consumer: SlopSyncHubService::drainLogBridge() on the hub task.
    //
    // NEWEST LOSES on overflow — and here it is not a preference but a
    // constraint: in an SPSC ring the producer does not own the tail, so it
    // physically cannot evict. Which is precisely why this ring needs the
    // SEVERITY RESERVE rather than an eviction policy: Trace/Debug/Info may
    // occupy at most SLOPLOG_RING - SLOPLOG_HIGH_RESERVE slots, so a Debug
    // flood can never fill the ring out from under a Warn/Error/Fatal line
    // that arrives a millisecond later. Same guarantee the sloplog core ring
    // makes, expressed with the tools SPSC allows. Drops are counted and
    // surfaced (§9.4: visible, never silent).
    //
    // The record is a local POD rather than sloplog::Record so SystemState.h
    // keeps its dependency surface; AppLog.cpp static_asserts the two agree.
    struct SlopLogRec {
        uint32_t ms    = 0;
        uint8_t  level = 0;    // sloplog::Level, which mirrors registry log_levels 0..5
        uint16_t lost  = 0;    // records SlopLog itself dropped just before this one
        char     tag[12]  = {};
        char     msg[104] = {};
    };
    static constexpr uint32_t SLOPLOG_RING = 16;
    // Slots only Warn+ may occupy. Mirrors sloplog::kReserveFloor == Warn ==
    // level 3; AppLog.cpp static_asserts that the numbering has not drifted.
    static constexpr uint32_t SLOPLOG_HIGH_RESERVE = 6;
    static constexpr uint8_t  SLOPLOG_RESERVE_LEVEL = 3;   // sloplog::Level::Warn
    SlopLogRec             sloplog_ring[SLOPLOG_RING] = {};
    std::atomic<uint32_t>  sloplog_head{0};      // producer cursor (httpTask)
    std::atomic<uint32_t>  sloplog_tail{0};      // consumer cursor (hub task)
    std::atomic<uint32_t>  sloplog_bridge_dropped{0};       // all levels
    std::atomic<uint32_t>  sloplog_bridge_dropped_high{0};  // Warn+ subset
    // Low-severity occupancy, tracked as two monotonic counters so each is
    // written by exactly ONE side (pushed: producer, popped: consumer) and the
    // SPSC discipline survives. lowPending = pushed - popped; a stale read of
    // `popped` only ever makes the producer more conservative, never less.
    std::atomic<uint32_t>  sloplog_low_pushed{0};
    std::atomic<uint32_t>  sloplog_low_popped{0};

    // Producer side (log-drain task ONLY). Never blocks, never allocates.
    bool slopLogPush(const SlopLogRec& r) {
        const uint32_t head = sloplog_head.load(std::memory_order_relaxed);
        const uint32_t tail = sloplog_tail.load(std::memory_order_acquire);
        const bool full = (head - tail) >= SLOPLOG_RING;
        const bool high = r.level >= SLOPLOG_RESERVE_LEVEL;
        if (full) {
            sloplog_bridge_dropped.fetch_add(1, std::memory_order_relaxed);
            if (high) sloplog_bridge_dropped_high.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (!high) {
            const uint32_t lowPending =
                sloplog_low_pushed.load(std::memory_order_relaxed) -
                sloplog_low_popped.load(std::memory_order_acquire);
            if (lowPending >= (SLOPLOG_RING - SLOPLOG_HIGH_RESERVE)) {
                sloplog_bridge_dropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            sloplog_low_pushed.store(
                sloplog_low_pushed.load(std::memory_order_relaxed) + 1,
                std::memory_order_relaxed);
        }
        sloplog_ring[head % SLOPLOG_RING] = r;
        sloplog_head.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side (SlopSyncHub task ONLY).
    bool slopLogPop(SlopLogRec& out) {
        const uint32_t tail = sloplog_tail.load(std::memory_order_relaxed);
        if (tail == sloplog_head.load(std::memory_order_acquire)) return false;
        out = sloplog_ring[tail % SLOPLOG_RING];
        if (out.level < SLOPLOG_RESERVE_LEVEL) {
            // Release: the producer's cap check acquire-loads this, and must
            // not observe the credit before the slot is genuinely free.
            sloplog_low_popped.store(
                sloplog_low_popped.load(std::memory_order_relaxed) + 1,
                std::memory_order_release);
        }
        sloplog_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // --------------------------------------------------------------------------
    // Convenience helpers — zero-cost inline
    // --------------------------------------------------------------------------

    TransportMode getTransport() const {
        return static_cast<TransportMode>(transport);
    }
    void setTransport(TransportMode m) {
        transport = static_cast<uint8_t>(m);
    }

    InputMode getInputMode() const {
        return static_cast<InputMode>(input_mode);
    }
    void setInputMode(InputMode m) {
        input_mode = static_cast<uint8_t>(m);
    }

    // ---- Safe-approach soft start (config_api.h SAFE_*) ----------------------
    // Returns the speed ceiling (mm/s) to use RIGHT NOW. For the first
    // SAFE_RESUME_RAMP_MS after resume_start_ms was stamped (un-pause, override
    // off, new stream, generator start, homing complete, window jump) the cap
    // ramps linearly from SAFE_APPROACH_SPEED_MM_S up to configured_max, so the
    // first move after a discontinuity glides instead of lunging. After the
    // ramp (or if never stamped) this returns configured_max unchanged. :3
    float safeSpeedCap(float configured_max, uint32_t now_ms) const {
        uint32_t t0 = resume_start_ms;
        if (t0 == 0) return configured_max;
        uint32_t dt = now_ms - t0;
        if (dt >= SAFE_RESUME_RAMP_MS) return configured_max;
        if (configured_max <= SAFE_APPROACH_SPEED_MM_S) return configured_max;
        float f = (float)dt / (float)SAFE_RESUME_RAMP_MS;   // 0..1
        return SAFE_APPROACH_SPEED_MM_S
             + f * (configured_max - SAFE_APPROACH_SPEED_MM_S);
    }
};