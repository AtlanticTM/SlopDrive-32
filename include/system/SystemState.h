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
    // Written by Core 1 (motionConsumerTask) after each FAS dispatch — the step
    // target we just sent to the motor, converted back to mm. Read by Core 0's
    // telemetry timer (WebUI::telemetryTimerCb) to feed the position graph.
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